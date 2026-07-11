/// @file benchmark_server.cpp
/// @brief MCP benchmark server matching TM Dev Lab v2 methodology.
///
/// Implements three I/O-bound tools with parallel HTTP + Redis operations.
/// Uses Boost.Beast for HTTP and an async Redis client with connection pooling.
///
/// Stateless JSON: uses the SDK's direct Streamable HTTP path to avoid
/// per-session MemoryTransport and DELETE cleanup overhead during the benchmark.

#include <mcp/server/server.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace benchmark {

std::string short_error(std::string message) {
    constexpr std::size_t max_len = 512;
    if (message.size() <= max_len) {
        return message;
    }
    message.resize(max_len);
    message += "...";
    return message;
}

// ============================================================================
// Data structures and JSON serialization
// ============================================================================

struct SearchProductsArgs {
    std::string category = "Electronics";
    double min_price = 50.0;
    double max_price = 500.0;
    int limit = 10;
};

struct GetUserCartArgs {
    std::string user_id = "user-00042";
};

struct CheckoutItem {
    int product_id = 0;
    int quantity = 0;
};

struct CheckoutArgs {
    std::string user_id = "user-00042";
    std::vector<CheckoutItem> items;
};

void from_json(const nlohmann::json& j, SearchProductsArgs& a) {
    if (j.contains("category") && !j["category"].is_null()) {
        a.category = j["category"].get<std::string>();
    }
    if (j.contains("min_price") && !j["min_price"].is_null()) {
        a.min_price = j["min_price"].get<double>();
    }
    if (j.contains("max_price") && !j["max_price"].is_null()) {
        a.max_price = j["max_price"].get<double>();
    }
    if (j.contains("limit") && !j["limit"].is_null()) {
        a.limit = j["limit"].get<int>();
    }
}

void to_json(nlohmann::json& j, const SearchProductsArgs& a) {
    j = {{"category", a.category},
         {"min_price", a.min_price},
         {"max_price", a.max_price},
         {"limit", a.limit}};
}

void from_json(const nlohmann::json& j, GetUserCartArgs& a) {
    if (j.contains("user_id") && !j["user_id"].is_null()) {
        a.user_id = j["user_id"].get<std::string>();
    }
}

void to_json(nlohmann::json& j, const GetUserCartArgs& a) { j = {{"user_id", a.user_id}}; }

void from_json(const nlohmann::json& j, CheckoutItem& a) {
    a.product_id = j.value("product_id", 0);
    a.quantity = j.value("quantity", 0);
}

void to_json(nlohmann::json& j, const CheckoutItem& a) {
    j = {{"product_id", a.product_id}, {"quantity", a.quantity}};
}

void from_json(const nlohmann::json& j, CheckoutArgs& a) {
    if (j.contains("user_id") && !j["user_id"].is_null()) {
        a.user_id = j["user_id"].get<std::string>();
    }
    if (j.contains("items") && j["items"].is_array()) {
        a.items = j["items"].get<decltype(a.items)>();
    }
    if (a.items.empty()) {
        a.items = {{42, 2}, {1337, 1}};
    }
}

void to_json(nlohmann::json& j, const CheckoutArgs& a) {
    j = {{"user_id", a.user_id}, {"items", a.items}};
}

// ============================================================================
// RESP protocol types and parser
// ============================================================================

struct RespValue;
using RespArray = std::vector<RespValue>;

struct RespValue {
    std::variant<std::string,     // simple string or bulk string
                 int64_t,         // integer
                 RespArray,       // array
                 std::nullptr_t>  // null
        data;

    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_integer() const { return std::holds_alternative<int64_t>(data); }
    bool is_array() const { return std::holds_alternative<RespArray>(data); }
    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }

    const std::string& as_string() const { return std::get<std::string>(data); }
    int64_t as_integer() const { return std::get<int64_t>(data); }
    const RespArray& as_array() const { return std::get<RespArray>(data); }
};

mcp::Task<std::string> resp_read_bulk(
    asio::ip::tcp::socket& socket, std::string& buf_str,
    boost::asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>>& buf,
    int length) {
    std::size_t needed = static_cast<std::size_t>(length) + 2;
    if (buf.size() < needed) {
        co_await asio::async_read(socket, buf, asio::transfer_at_least(needed - buf.size()),
                                  asio::use_awaitable);
    }
    std::string result = buf_str.substr(0, static_cast<std::size_t>(length));
    buf.consume(needed);
    co_return result;
}

mcp::Task<std::string> resp_read_line(
    asio::ip::tcp::socket& socket, std::string& buf_str,
    boost::asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>>& buf) {
    auto n = co_await asio::async_read_until(socket, buf, "\r\n", asio::use_awaitable);
    std::string raw = buf_str.substr(0, n);
    buf.consume(n);
    if (raw.size() >= 2 && raw[raw.size() - 2] == '\r' && raw.back() == '\n') {
        raw.resize(raw.size() - 2);
    }
    co_return raw;
}

mcp::Task<RespValue> resp_parse(
    asio::ip::tcp::socket& socket, std::string& buf_str,
    boost::asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>>& buf) {
    auto line = co_await resp_read_line(socket, buf_str, buf);
    if (line.empty()) {
        co_return RespValue{std::string("")};
    }

    char type = line[0];
    std::string payload = line.substr(1);

    switch (type) {
        case '+':
            co_return RespValue{payload};
        case '-':
            co_return RespValue{std::string("ERR: " + payload)};
        case ':':
            co_return RespValue{static_cast<int64_t>(std::stoll(payload))};
        case '$': {
            int len = std::stoi(payload);
            if (len < 0) {
                co_return RespValue{nullptr};
            }
            auto data = co_await resp_read_bulk(socket, buf_str, buf, len);
            co_return RespValue{std::move(data)};
        }
        case '*': {
            int count = std::stoi(payload);
            if (count < 0) {
                co_return RespValue{nullptr};
            }
            RespArray arr;
            arr.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                arr.push_back(co_await resp_parse(socket, buf_str, buf));
            }
            co_return RespValue{std::move(arr)};
        }
        default:
            co_return RespValue{std::string(line)};
    }
}

// ============================================================================
// Async Redis client with connection pooling
// ============================================================================

class RedisPool {
   public:
    RedisPool(asio::any_io_executor executor, std::string host, std::string port,
              std::size_t pool_size = 20)
        : executor_(std::move(executor)),
          host_(std::move(host)),
          port_(std::move(port)),
          pool_size_(pool_size) {}

    mcp::Task<RespValue> execute(std::vector<std::string> cmd) {
        auto socket = co_await acquire();
        try {
            std::string request = encode_command(cmd);

            co_await asio::async_write(*socket, asio::buffer(request), asio::use_awaitable);

            std::string buf_storage;
            auto buf = asio::dynamic_buffer(buf_storage);
            auto result = co_await resp_parse(*socket, buf_storage, buf);

            release(std::move(socket));
            co_return result;
        } catch (...) {
            co_return RespValue{nullptr};
        }
    }

    mcp::Task<std::vector<RespValue>> execute_pipeline(std::vector<std::vector<std::string>> cmds) {
        auto socket = co_await acquire();
        try {
            std::string request;
            for (const auto& cmd : cmds) {
                request += encode_command(cmd);
            }

            co_await asio::async_write(*socket, asio::buffer(request), asio::use_awaitable);

            std::string buf_storage;
            auto buf = asio::dynamic_buffer(buf_storage);
            std::vector<RespValue> replies;
            replies.reserve(cmds.size());
            for (std::size_t i = 0; i < cmds.size(); ++i) {
                replies.push_back(co_await resp_parse(*socket, buf_storage, buf));
            }

            release(std::move(socket));
            co_return replies;
        } catch (...) {
            co_return std::vector<RespValue>{};
        }
    }

    template <typename F>
    auto execute_mapped(std::vector<std::string> cmd, F transform)
        -> mcp::Task<decltype(transform(std::declval<const RespValue&>()))> {
        auto reply = co_await execute(std::move(cmd));
        co_return transform(reply);
    }

    mcp::Task<void> execute_void(std::vector<std::string> cmd) { co_await execute(std::move(cmd)); }

    mcp::Task<std::vector<std::string>> zrevrange(const std::string& key, int start, int stop) {
        return execute_mapped({"ZREVRANGE", key, std::to_string(start), std::to_string(stop)},
                              to_string_array);
    }

    mcp::Task<std::map<std::string, std::string>> hgetall(const std::string& key) {
        return execute_mapped({"HGETALL", key}, [](const RespValue& reply) {
            std::map<std::string, std::string> result;
            if (reply.is_array()) {
                const auto& arr = reply.as_array();
                for (std::size_t i = 0; i + 1 < arr.size(); i += 2) {
                    if (arr[i].is_string() && arr[i + 1].is_string()) {
                        result[arr[i].as_string()] = arr[i + 1].as_string();
                    }
                }
            }
            return result;
        });
    }

    mcp::Task<std::vector<std::string>> lrange(const std::string& key, int start, int stop) {
        return execute_mapped({"LRANGE", key, std::to_string(start), std::to_string(stop)},
                              to_string_array);
    }

    mcp::Task<int64_t> incr(const std::string& key) {
        return execute_mapped({"INCR", key}, [](const RespValue& r) -> int64_t {
            return r.is_integer() ? r.as_integer() : 0;
        });
    }

    mcp::Task<void> rpush(const std::string& key, const std::string& value) {
        return execute_void({"RPUSH", key, value});
    }

    mcp::Task<void> zincrby(const std::string& key, double increment, const std::string& member) {
        return execute_void({"ZINCRBY", key, std::to_string(increment), member});
    }

    mcp::Task<int64_t> checkout_pipeline(std::string rate_key, std::string history_key,
                                         std::string order_entry, std::string popular_member) {
        auto socket = co_await acquire();
        try {
            std::string request;
            request += encode_command({"INCR", rate_key});
            request += encode_command({"RPUSH", history_key, order_entry});
            request += encode_command({"ZINCRBY", "bench:popular", "1", popular_member});

            co_await asio::async_write(*socket, asio::buffer(request), asio::use_awaitable);

            std::string buf_storage;
            auto buf = asio::dynamic_buffer(buf_storage);
            auto rate_reply = co_await resp_parse(*socket, buf_storage, buf);
            (void)co_await resp_parse(*socket, buf_storage, buf);
            (void)co_await resp_parse(*socket, buf_storage, buf);

            release(std::move(socket));
            if (rate_reply.is_integer()) {
                co_return rate_reply.as_integer();
            }
        } catch (...) {
        }
        co_return 0;
    }

   private:
    static std::string encode_command(const std::vector<std::string>& cmd) {
        std::string request = "*" + std::to_string(cmd.size()) + "\r\n";
        for (const auto& arg : cmd) {
            request += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
        }
        return request;
    }

    mcp::Task<std::unique_ptr<asio::ip::tcp::socket>> acquire() {
        auto pooled = try_acquire_from_pool();
        if (pooled) {
            co_return std::move(pooled);
        }
        auto socket = std::make_unique<asio::ip::tcp::socket>(executor_);
        asio::ip::tcp::resolver resolver(executor_);
        auto endpoints = co_await resolver.async_resolve(host_, port_, asio::use_awaitable);
        co_await asio::async_connect(*socket, endpoints, asio::use_awaitable);
        co_return socket;
    }

    void release(std::unique_ptr<asio::ip::tcp::socket> socket) {
        if (!socket || !socket->is_open()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.size() < pool_size_) {
            pool_.push_back(std::move(socket));
        }
    }

    std::unique_ptr<asio::ip::tcp::socket> try_acquire_from_pool() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            auto socket = std::move(pool_.front());
            pool_.pop_front();
            return socket;
        }
        return nullptr;
    }

    static std::vector<std::string> to_string_array(const RespValue& reply) {
        std::vector<std::string> result;
        if (reply.is_array()) {
            for (const auto& elem : reply.as_array()) {
                if (elem.is_string()) {
                    result.push_back(elem.as_string());
                }
            }
        }
        return result;
    }

    asio::any_io_executor executor_;
    std::string host_;
    std::string port_;
    std::size_t pool_size_;
    std::mutex mutex_;
    std::deque<std::unique_ptr<asio::ip::tcp::socket>> pool_;
};

// ============================================================================
// HTTP client (Boost.Beast)
// ============================================================================

class HttpClient {
   public:
    HttpClient(asio::any_io_executor executor, std::string host, std::string port,
               std::size_t pool_size = 50)
        : executor_(std::move(executor)),
          host_(std::move(host)),
          port_(std::move(port)),
          pool_size_(pool_size) {}

    mcp::Task<nlohmann::json> get(std::string target) {
        try {
            auto conn = co_await acquire();
            conn->stream.expires_after(std::chrono::seconds(10));

            http::request<http::string_body> req{http::verb::get, target, 11};
            req.set(http::field::host, host_);
            req.keep_alive(true);
            co_await http::async_write(conn->stream, req, asio::use_awaitable);

            http::response<http::string_body> res;
            co_await http::async_read(conn->stream, conn->buffer, res, asio::use_awaitable);

            auto result = nlohmann::json::parse(res.body());
            release(std::move(conn), res.keep_alive());

            co_return result;
        } catch (const std::exception& e) {
            auto message = short_error(e.what());
            std::cerr << "HTTP GET error (" << target << "): " << message << std::endl;
            co_return nlohmann::json{{"error", std::move(message)}};
        }
    }

    mcp::Task<nlohmann::json> post(std::string target, nlohmann::json body) {
        try {
            auto conn = co_await acquire();
            conn->stream.expires_after(std::chrono::seconds(10));

            http::request<http::string_body> req{http::verb::post, target, 11};
            req.set(http::field::host, host_);
            req.set(http::field::content_type, "application/json");
            req.keep_alive(true);
            req.body() = body.dump();
            req.prepare_payload();
            co_await http::async_write(conn->stream, req, asio::use_awaitable);

            http::response<http::string_body> res;
            co_await http::async_read(conn->stream, conn->buffer, res, asio::use_awaitable);

            auto result = nlohmann::json::parse(res.body());
            release(std::move(conn), res.keep_alive());

            co_return result;
        } catch (const std::exception& e) {
            auto message = short_error(e.what());
            std::cerr << "HTTP POST error (" << target << "): " << message << std::endl;
            co_return nlohmann::json{{"error", std::move(message)}};
        }
    }

   private:
    struct Connection {
        explicit Connection(asio::any_io_executor executor) : stream(std::move(executor)) {}

        beast::tcp_stream stream;
        beast::flat_buffer buffer;
    };

    mcp::Task<std::unique_ptr<Connection>> acquire() {
        auto pooled = try_acquire_from_pool();
        if (pooled) {
            co_return std::move(pooled);
        }

        auto conn = std::make_unique<Connection>(executor_);
        asio::ip::tcp::resolver resolver(executor_);
        auto results = co_await resolver.async_resolve(host_, port_, asio::use_awaitable);
        co_await conn->stream.async_connect(results, asio::use_awaitable);
        beast::error_code ec;
        conn->stream.socket().set_option(asio::ip::tcp::no_delay(true), ec);
        co_return conn;
    }

    void release(std::unique_ptr<Connection> conn, bool keep_alive) {
        if (!conn || !keep_alive || !conn->stream.socket().is_open()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.size() < pool_size_) {
            pool_.push_back(std::move(conn));
        }
    }

    std::unique_ptr<Connection> try_acquire_from_pool() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop_front();
            return conn;
        }
        return nullptr;
    }

    asio::any_io_executor executor_;
    std::string host_;
    std::string port_;
    std::size_t pool_size_;
    std::mutex mutex_;
    std::deque<std::unique_ptr<Connection>> pool_;
};

// ============================================================================
// Zero-pad helper
// ============================================================================

static std::string zero_pad(int num, int width) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(width) << num;
    return oss.str();
}

// ============================================================================
// Global I/O clients
// ============================================================================

static std::unique_ptr<HttpClient> g_http_client;
static std::unique_ptr<RedisPool> g_redis_pool;

// ============================================================================
// Tool handlers
// ============================================================================

mcp::CallToolResult make_tool_result(nlohmann::json payload, bool is_error = false) {
    mcp::CallToolResult result;
    result.isError = is_error;
    result.structuredContent = payload;
    result.content.push_back(mcp::TextContent{.text = payload.dump()});
    return result;
}

mcp::Task<mcp::CallToolResult> handle_search_products(const SearchProductsArgs& args) {
    try {
        auto executor = co_await asio::this_coro::executor;
        std::string query = "/products/search?category=" + args.category +
                            "&min_price=" + std::to_string(args.min_price) +
                            "&max_price=" + std::to_string(args.max_price) +
                            "&limit=" + std::to_string(args.limit);

        auto popular_task = asio::co_spawn(executor, g_redis_pool->zrevrange("bench:popular", 0, 9),
                                           asio::use_awaitable);
        auto search_data = co_await g_http_client->get(query);
        auto popular_raw = co_await std::move(popular_task);

        std::vector<int> top10_ids;
        std::map<int, int> top10_rank;
        for (std::size_t i = 0; i < popular_raw.size(); ++i) {
            auto colon = popular_raw[i].find(':');
            if (colon != std::string::npos) {
                int id = std::stoi(popular_raw[i].substr(colon + 1));
                top10_ids.push_back(id);
                top10_rank[id] = static_cast<int>(i) + 1;
            }
        }

        nlohmann::json products = nlohmann::json::array();
        if (search_data.contains("products")) {
            for (auto& p : search_data["products"]) {
                int id = p["id"];
                p["popularity_rank"] = top10_rank.count(id) ? top10_rank[id] : 0;
                products.push_back(p);
            }
        }

        co_return make_tool_result(nlohmann::json{{"category", args.category},
                                                  {"total_found", search_data.value("total_found", 0)},
                                                  {"products", products},
                                                  {"top10_popular_ids", top10_ids},
                                                  {"server_type", "cpp"}});
    } catch (const std::exception& e) {
        std::cerr << "search_products error: " << e.what() << std::endl;
        co_return make_tool_result(nlohmann::json{{"error", e.what()}}, true);
    }
}

mcp::Task<mcp::CallToolResult> handle_get_user_cart(const GetUserCartArgs& args) {
    try {
        auto executor = co_await asio::this_coro::executor;
        std::string cart_key = "bench:cart:" + args.user_id;
        std::string history_key = "bench:history:" + args.user_id;

        auto cart_hash = co_await g_redis_pool->hgetall(cart_key);

        nlohmann::json items = nlohmann::json::array();
        int first_product_id = 1;
        if (cart_hash.count("items")) {
            try {
                items = nlohmann::json::parse(cart_hash["items"]);
                if (!items.empty() && items[0].contains("product_id")) {
                    first_product_id = items[0]["product_id"].get<int>();
                }
            } catch (...) {
            }
        }

        auto history_task =
            asio::co_spawn(executor, g_redis_pool->lrange(history_key, 0, 4), asio::use_awaitable);
        auto product_data =
            co_await g_http_client->get("/products/" + std::to_string(first_product_id));
        (void)product_data;
        auto history_raw = co_await std::move(history_task);

        nlohmann::json recent_history = nlohmann::json::array();
        for (const auto& entry : history_raw) {
            try {
                recent_history.push_back(nlohmann::json::parse(entry));
            } catch (...) {
                nlohmann::json raw_entry;
                raw_entry["raw"] = entry;
                recent_history.push_back(std::move(raw_entry));
            }
        }

        double estimated_total = 0.0;
        if (cart_hash.count("total")) {
            try {
                estimated_total = std::stod(cart_hash["total"]);
            } catch (...) {
            }
        }

        co_return make_tool_result(nlohmann::json{
            {"user_id", args.user_id},
            {"cart",
             {{"items", items}, {"item_count", items.size()}, {"estimated_total", estimated_total}}},
            {"recent_history", recent_history},
            {"server_type", "cpp"}});
    } catch (const std::exception& e) {
        std::cerr << "get_user_cart error: " << e.what() << std::endl;
        co_return make_tool_result(nlohmann::json{{"error", e.what()}}, true);
    }
}

mcp::Task<mcp::CallToolResult> handle_checkout(const CheckoutArgs& args) {
    try {
        auto executor = co_await asio::this_coro::executor;
        int user_num = 42;
        auto pos = args.user_id.rfind('-');
        if (pos != std::string::npos) {
            user_num = std::stoi(args.user_id.substr(pos + 1));
        }

        std::string rate_key = "bench:ratelimit:user-" + zero_pad(user_num % 100, 5);
        std::string history_key = "bench:history:" + args.user_id;
        int product_id = args.items.empty() ? 1 : args.items[0].product_id;

        auto now = std::time(nullptr);
        nlohmann::json order_entry = {{"order_id", "ORD-" + args.user_id + "-" + std::to_string(now)},
                                      {"items", args.items},
                                      {"ts", now}};

        nlohmann::json calc_payload = {{"user_id", args.user_id}, {"items", args.items}};
        std::string order_str = order_entry.dump();
        std::string popular_member = "product:" + std::to_string(product_id);

        auto redis_task = asio::co_spawn(
            executor, g_redis_pool->checkout_pipeline(rate_key, history_key, order_str, popular_member),
            asio::use_awaitable);
        auto calc_task = asio::co_spawn(executor, g_http_client->post("/cart/calculate", calc_payload),
                                        asio::use_awaitable);

        auto calc_data = co_await std::move(calc_task);
        auto rate_count = co_await std::move(redis_task);

        std::string order_id =
            calc_data.value("order_id", "ORD-" + args.user_id + "-" + std::to_string(now));
        double total = calc_data.value("total", 0.0);

        nlohmann::json result;
        result["order_id"] = order_id;
        result["user_id"] = args.user_id;
        result["total"] = total;
        result["items_count"] = args.items.size();
        result["rate_limit_count"] = rate_count;
        result["status"] = "confirmed";
        result["server_type"] = "cpp";
        co_return make_tool_result(std::move(result));
    } catch (const std::exception& e) {
        std::cerr << "checkout error: " << e.what() << std::endl;
        co_return make_tool_result(nlohmann::json{{"error", e.what()}}, true);
    }
}

}  // namespace benchmark

using namespace mcp;
using namespace benchmark;

static std::unique_ptr<Server> create_server_with_tools(const boost::asio::any_io_executor&) {
    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{};

    Implementation info;
    info.name = "BenchmarkCppServer";
    info.version = "1.0.0";

    auto server = std::make_unique<Server>(std::move(info), std::move(caps));

    nlohmann::json search_schema = {{"type", "object"},
                                    {"properties",
                                     {{"category", {{"type", "string"}, {"default", "Electronics"}}},
                                      {"min_price", {{"type", "number"}, {"default", 50.0}}},
                                      {"max_price", {{"type", "number"}, {"default", 500.0}}},
                                      {"limit", {{"type", "integer"}, {"default", 10}}}}}};

    server->add_tool<SearchProductsArgs, mcp::CallToolResult>(
        "search_products", "Search products by category and price range, merged with popularity data",
        std::move(search_schema), handle_search_products);

    nlohmann::json cart_schema = {
        {"type", "object"},
        {"properties", {{"user_id", {{"type", "string"}, {"default", "user-00042"}}}}}};

    server->add_tool<GetUserCartArgs, mcp::CallToolResult>(
        "get_user_cart", "Get user cart details with recent order history", std::move(cart_schema),
        handle_get_user_cart);

    nlohmann::json checkout_schema = {
        {"type", "object"},
        {"properties",
         {{"user_id", {{"type", "string"}, {"default", "user-00042"}}},
          {"items",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"product_id", {{"type", "integer"}}}, {"quantity", {{"type", "integer"}}}}},
              {"required", nlohmann::json::array({"product_id", "quantity"})}}}}}}}};

    server->add_tool<CheckoutArgs, mcp::CallToolResult>(
        "checkout", "Process checkout: calculate total, update rate limit, record history",
        std::move(checkout_schema), handle_checkout);

    return server;
}

int main() {
    const char* api_url_env = std::getenv("API_SERVICE_URL");
    std::string api_url = api_url_env ? api_url_env : "http://api-service:8100";

    std::string api_host = "api-service";
    std::string api_port = "8100";
    if (api_url.substr(0, 7) == "http://") {
        std::string host_port = api_url.substr(7);
        auto colon = host_port.find(':');
        if (colon != std::string::npos) {
            api_host = host_port.substr(0, colon);
            api_port = host_port.substr(colon + 1);
        } else {
            api_host = host_port;
            api_port = "80";
        }
    }

    const char* redis_url_env = std::getenv("REDIS_URL");
    std::string redis_url = redis_url_env ? redis_url_env : "redis://redis:6379";

    std::string redis_host = "redis";
    std::string redis_port = "6379";
    if (redis_url.substr(0, 8) == "redis://") {
        std::string host_port = redis_url.substr(8);
        auto colon = host_port.find(':');
        if (colon != std::string::npos) {
            redis_host = host_port.substr(0, colon);
            redis_port = host_port.substr(colon + 1);
        }
    }

    const char* port_env = std::getenv("PORT");
    unsigned short mcp_port = port_env ? static_cast<unsigned short>(std::stoi(port_env)) : 8080;

    std::cout << "API Service: " << api_host << ":" << api_port << std::endl;
    std::cout << "Redis: " << redis_host << ":" << redis_port << std::endl;

    asio::io_context io_ctx;

    g_http_client = std::make_unique<HttpClient>(io_ctx.get_executor(), api_host, api_port, 50);
    g_redis_pool = std::make_unique<RedisPool>(io_ctx.get_executor(), redis_host, redis_port, 100);

    StreamableHttpSessionManager manager(io_ctx.get_executor(), "0.0.0.0", mcp_port,
                                         create_server_with_tools);
    manager.set_stateless_json_mode(true);

    manager.set_custom_request_handler(
        [](const boost::beast::http::request<boost::beast::http::string_body>& req)
            -> std::optional<boost::beast::http::response<boost::beast::http::string_body>> {
            if (req.target() == "/health") {
                boost::beast::http::response<boost::beast::http::string_body> res{
                    boost::beast::http::status::ok, req.version()};
                res.set(boost::beast::http::field::content_type, "application/json");
                res.set(boost::beast::http::field::server, "mcp-cpp-sdk");
                res.body() = R"({"status":"ok","server_type":"cpp"})";
                res.prepare_payload();
                res.keep_alive(req.keep_alive());
                return res;
            }
            return std::nullopt;
        });

    std::cout << "C++ MCP Benchmark Server starting (multi-session)..." << std::endl;

    // Create work guard to keep io_context alive
    auto work_guard = asio::make_work_guard(io_ctx);

    asio::co_spawn(io_ctx, manager.listen(), asio::detached);

    std::cout << "C++ MCP server listening on http://0.0.0.0:" << mcp_port << "/mcp" << std::endl;
    std::cout << "Health endpoint on http://0.0.0.0:" << mcp_port << "/health" << std::endl;
    std::cout << "Registered tools: search_products, get_user_cart, checkout" << std::endl;

    std::cout << "C++ MCP server starting with 2 threads..." << std::endl;

    // Run io_context on 2 threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&io_ctx]() { io_ctx.run(); });
    }

    for (auto& t : threads) {
        t.join();
    }

    return 0;
}
