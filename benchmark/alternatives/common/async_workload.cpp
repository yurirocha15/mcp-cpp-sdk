#include "async_workload.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mcp_benchmark {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;
using Json = nlohmann::json;
template <class T>
using Awaitable = asio::awaitable<T>;

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

struct HostPort {
    std::string host;
    std::string port;
};

/// Splits `[scheme://]host[:port][/path]`; the path is dropped.
HostPort parse_host_port(std::string url, const char* default_port) {
    const auto scheme = url.find("://");
    if (scheme != std::string::npos) {
        url.erase(0, scheme + 3);
    }
    const auto slash = url.find('/');
    if (slash != std::string::npos) {
        url.resize(slash);
    }
    const auto colon = url.rfind(':');
    if (colon == std::string::npos) {
        return {url, default_port};
    }
    return {url.substr(0, colon), url.substr(colon + 1)};
}

/// Idle connections, shared by every coroutine of the workload.
template <class Connection>
class IdlePool {
   public:
    std::unique_ptr<Connection> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idle_.empty()) {
            return nullptr;
        }
        auto connection = std::move(idle_.back());
        idle_.pop_back();
        return connection;
    }

    void give(std::unique_ptr<Connection> connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_.push_back(std::move(connection));
    }

   private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<Connection>> idle_;
};

Awaitable<void> connect_socket(tcp::socket& socket, const HostPort& endpoint) {
    tcp::resolver resolver(socket.get_executor());
    const auto addresses =
        co_await resolver.async_resolve(endpoint.host, endpoint.port, asio::use_awaitable);
    co_await asio::async_connect(socket, addresses, asio::use_awaitable);
    socket.set_option(tcp::no_delay(true));
}

struct HttpConnection {
    explicit HttpConnection(const asio::any_io_executor& executor) : socket(executor) {}
    tcp::socket socket;
    beast::flat_buffer buffer;
};

class AsyncHttp {
   public:
    explicit AsyncHttp(const std::string& base_url) : endpoint_(parse_host_port(base_url, "80")) {}

    Awaitable<std::string> get(std::string target) {
        co_return co_await request(http::verb::get, std::move(target), std::string());
    }

    Awaitable<std::string> post(std::string target, std::string body) {
        co_return co_await request(http::verb::post, std::move(target), std::move(body));
    }

   private:
    Awaitable<std::string> request(http::verb verb, std::string target, std::string body) {
        for (int attempt = 0;; ++attempt) {
            auto connection = pool_.take();
            const bool reused = connection != nullptr;
            try {
                if (!connection) {
                    connection = std::make_unique<HttpConnection>(co_await asio::this_coro::executor);
                    co_await connect_socket(connection->socket, endpoint_);
                }
                http::request<http::string_body> message{verb, target, 11};
                message.set(http::field::host, endpoint_.host);
                message.keep_alive(true);
                if (verb == http::verb::post) {
                    message.set(http::field::content_type, "application/json");
                    message.body() = body;
                    message.prepare_payload();
                }
                co_await http::async_write(connection->socket, message, asio::use_awaitable);
                http::response<http::string_body> response;
                co_await http::async_read(connection->socket, connection->buffer, response,
                                          asio::use_awaitable);
                if (response.result_int() < 200 || response.result_int() >= 300) {
                    throw std::runtime_error("HTTP request returned status " +
                                             std::to_string(response.result_int()));
                }
                std::string output = std::move(response.body());
                if (response.keep_alive()) {
                    pool_.give(std::move(connection));
                }
                co_return output;
            } catch (const boost::system::system_error&) {
                // A pooled connection can have been closed by the peer while idle; retry once on a
                // fresh one. Anything else is reported.
                if (!reused || attempt > 0) {
                    throw;
                }
            }
        }
    }

    HostPort endpoint_;
    IdlePool<HttpConnection> pool_;
};

struct RedisConnection {
    explicit RedisConnection(const asio::any_io_executor& executor) : socket(executor) {}
    tcp::socket socket;
    std::string input;
};

/// A decoded Redis reply: an integer, or a flat list of strings (a bulk string is a list of one).
struct RedisReply {
    std::int64_t integer = 0;
    std::vector<std::string> strings;
};

std::string encode_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out += arg;
        out += "\r\n";
    }
    return out;
}

Awaitable<std::string> read_line(RedisConnection& connection) {
    const std::size_t size = co_await asio::async_read_until(
        connection.socket, asio::dynamic_buffer(connection.input), "\r\n", asio::use_awaitable);
    std::string line = connection.input.substr(0, size - 2);
    connection.input.erase(0, size);
    co_return line;
}

Awaitable<std::string> read_bulk(RedisConnection& connection, std::size_t length) {
    const std::size_t wanted = length + 2;
    if (connection.input.size() < wanted) {
        co_await asio::async_read(connection.socket, asio::dynamic_buffer(connection.input),
                                  asio::transfer_exactly(wanted - connection.input.size()),
                                  asio::use_awaitable);
    }
    std::string value = connection.input.substr(0, length);
    connection.input.erase(0, wanted);
    co_return value;
}

/// Decodes the scalar reply that starts with @p line (status, error, integer, bulk string or nil).
Awaitable<void> decode_scalar(RedisConnection& connection, const std::string& line, RedisReply& reply) {
    if (line.empty()) {
        throw std::runtime_error("Redis returned an empty reply line");
    }
    const std::string rest = line.substr(1);
    if (line[0] == '+') {
        reply.strings.push_back(rest);
    } else if (line[0] == '-') {
        throw std::runtime_error("Redis error: " + rest);
    } else if (line[0] == ':') {
        reply.integer = std::stoll(rest);
    } else if (line[0] == '$') {
        const long long length = std::stoll(rest);
        if (length < 0) {
            reply.strings.emplace_back();
        } else {
            reply.strings.push_back(co_await read_bulk(connection, static_cast<std::size_t>(length)));
        }
    } else {
        throw std::runtime_error("unsupported Redis reply type");
    }
}

Awaitable<RedisReply> read_reply(RedisConnection& connection) {
    RedisReply reply;
    const std::string line = co_await read_line(connection);
    if (!line.empty() && line[0] == '*') {
        const long long count = std::stoll(line.substr(1));
        for (long long i = 0; i < count; ++i) {
            co_await decode_scalar(connection, co_await read_line(connection), reply);
        }
    } else {
        co_await decode_scalar(connection, line, reply);
    }
    co_return reply;
}

class AsyncRedis {
   public:
    explicit AsyncRedis(const std::string& url) : endpoint_(parse_host_port(url, "6379")) {}

    Awaitable<std::vector<std::string>> string_array(std::vector<std::string> args) {
        auto connection = co_await acquire();
        const std::string payload = encode_command(args);
        co_await asio::async_write(connection->socket, asio::buffer(payload), asio::use_awaitable);
        RedisReply reply = co_await read_reply(*connection);
        pool_.give(std::move(connection));
        co_return std::move(reply.strings);
    }

    /// Sends @p commands in one round trip and returns the integer reply of the first.
    Awaitable<std::int64_t> pipeline_first_integer(std::vector<std::vector<std::string>> commands) {
        auto connection = co_await acquire();
        std::string payload;
        for (const auto& args : commands) {
            payload += encode_command(args);
        }
        co_await asio::async_write(connection->socket, asio::buffer(payload), asio::use_awaitable);
        std::int64_t first = 0;
        for (std::size_t i = 0; i < commands.size(); ++i) {
            RedisReply reply = co_await read_reply(*connection);
            if (i == 0) {
                first = reply.integer;
            }
        }
        pool_.give(std::move(connection));
        co_return first;
    }

   private:
    Awaitable<std::unique_ptr<RedisConnection>> acquire() {
        if (auto idle = pool_.take()) {
            co_return idle;
        }
        auto connection = std::make_unique<RedisConnection>(co_await asio::this_coro::executor);
        co_await connect_socket(connection->socket, endpoint_);
        co_return connection;
    }

    HostPort endpoint_;
    IdlePool<RedisConnection> pool_;
};

std::string number(double value) {
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}

int user_number(const std::string& user_id) {
    const auto dash = user_id.rfind('-');
    if (dash == std::string::npos) {
        return 42;
    }
    try {
        return std::stoi(user_id.substr(dash + 1));
    } catch (...) {
        return 42;
    }
}

Json recent_history(const std::vector<std::string>& history) {
    Json recent = Json::array();
    for (const auto& entry : history) {
        Json parsed = Json::parse(entry, nullptr, false);
        recent.push_back(parsed.is_discarded() ? Json{{"raw", entry}} : std::move(parsed));
    }
    return recent;
}

}  // namespace

class AsyncWorkload::Impl {
   public:
    explicit Impl(std::string server_type)
        : server_type_(std::move(server_type)),
          http_(env_or("API_SERVICE_URL", "http://api-service:8100")),
          redis_(env_or("REDIS_URL", "redis://redis:6379")) {}

    Awaitable<std::string> invoke(std::string name, Json args) {
        if (args.is_null()) {
            args = Json::object();
        }
        if (name == "search_products") {
            co_return (co_await search_products(args)).dump();
        }
        if (name == "get_user_cart") {
            co_return (co_await get_user_cart(args)).dump();
        }
        if (name == "checkout") {
            co_return (co_await checkout(args)).dump();
        }
        throw std::invalid_argument("unknown benchmark tool: " + name);
    }

   private:
    Awaitable<Json> search_products(const Json& args) {
        using namespace asio::experimental::awaitable_operators;
        const std::string category = args.value("category", "Electronics");
        const double min_price = args.value("min_price", 50.0);
        const double max_price = args.value("max_price", 500.0);
        const int limit = args.value("limit", 10);
        std::string path = "/products/search?category=" + category + "&min_price=" + number(min_price) +
                           "&max_price=" + number(max_price) + "&limit=" + std::to_string(limit);

        auto fetched = co_await (redis_.string_array({"ZREVRANGE", "bench:popular", "0", "9"}) &&
                                 http_.get(std::move(path)));
        auto& popular_raw = std::get<0>(fetched);
        auto& body = std::get<1>(fetched);
        Json search_data = Json::parse(body);

        Json ids = Json::array();
        std::vector<int> top_ids;
        for (const auto& member : popular_raw) {
            const auto colon = member.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const int id = std::stoi(member.substr(colon + 1));
            top_ids.push_back(id);
            ids.push_back(id);
        }

        Json products = Json::array();
        for (const auto& product : search_data.value("products", Json::array())) {
            const int id = product.value("id", 0);
            const auto it = std::find(top_ids.begin(), top_ids.end(), id);
            const int rank = it == top_ids.end() ? 0 : static_cast<int>(it - top_ids.begin()) + 1;
            products.push_back({{"id", id},
                                {"sku", product.value("sku", "")},
                                {"name", product.value("name", "")},
                                {"price", product.value("price", 0.0)},
                                {"rating", product.value("rating", 0.0)},
                                {"popularity_rank", rank}});
        }
        Json result = {{"category", category},
                       {"total_found", search_data.value("total_found", 0)},
                       {"products", std::move(products)},
                       {"top10_popular_ids", std::move(ids)},
                       {"server_type", server_type_}};
        co_return result;
    }

    Awaitable<Json> get_user_cart(const Json& args) {
        using namespace asio::experimental::awaitable_operators;
        const std::string user_id = args.value("user_id", "user-00042");
        std::vector<std::string> hgetall{"HGETALL", "bench:cart:" + user_id};
        const auto hash = co_await redis_.string_array(std::move(hgetall));
        Json items = Json::array();
        double total = 0.0;
        for (std::size_t i = 0; i + 1 < hash.size(); i += 2) {
            if (hash[i] == "items") {
                items = Json::parse(hash[i + 1], nullptr, false);
            }
            if (hash[i] == "total") {
                total = std::stod(hash[i + 1]);
            }
        }
        if (!items.is_array()) {
            items = Json::array();
        }
        const int product_id = items.empty() ? 1 : items.front().value("product_id", 1);

        std::vector<std::string> lrange{"LRANGE", "bench:history:" + user_id, "0", "4"};
        std::string product_path = "/products/" + std::to_string(product_id);
        auto fetched =
            co_await (redis_.string_array(std::move(lrange)) && http_.get(std::move(product_path)));
        auto& history = std::get<0>(fetched);
        auto& product_body = std::get<1>(fetched);
        (void)product_body;
        Json recent = recent_history(history);
        Json result = {
            {"user_id", user_id},
            {"cart", {{"items", items}, {"item_count", items.size()}, {"estimated_total", total}}},
            {"recent_history", std::move(recent)},
            {"server_type", server_type_}};
        co_return result;
    }

    Awaitable<Json> checkout(const Json& args) {
        using namespace asio::experimental::awaitable_operators;
        const std::string user_id = args.value("user_id", "user-00042");
        Json items = args.value("items", Json::array());
        if (!items.is_array() || items.empty()) {
            items = Json::array(
                {{{"product_id", 42}, {"quantity", 2}}, {{"product_id", 1337}, {"quantity", 1}}});
        }
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        const std::string history_key = "bench:history:" + user_id;
        const int product_id = items.front().value("product_id", 1);
        std::ostringstream rate_key;
        rate_key << "bench:ratelimit:user-" << std::setw(5) << std::setfill('0')
                 << (user_number(user_id) % 100);
        const Json order_entry = {
            {"order_id", "ORD-" + user_id + "-" + std::to_string(now)}, {"items", items}, {"ts", now}};
        const Json calculate = {{"user_id", user_id}, {"items", items}};

        std::vector<std::vector<std::string>> commands{
            {"INCR", rate_key.str()},
            {"RPUSH", history_key, order_entry.dump()},
            {"ZINCRBY", "bench:popular", "1", "product:" + std::to_string(product_id)}};
        std::string calculate_body = calculate.dump();
        auto fetched = co_await (redis_.pipeline_first_integer(std::move(commands)) &&
                                 http_.post("/cart/calculate", std::move(calculate_body)));
        auto& rate_count = std::get<0>(fetched);
        auto& calculated = std::get<1>(fetched);
        Json calc = Json::parse(calculated);
        Json result = {{"order_id", calc.value("order_id", order_entry["order_id"].get<std::string>())},
                       {"user_id", user_id},
                       {"total", calc.value("total", 0.0)},
                       {"items_count", items.size()},
                       {"rate_limit_count", rate_count},
                       {"status", "confirmed"},
                       {"server_type", server_type_}};
        co_return result;
    }

    std::string server_type_;
    AsyncHttp http_;
    AsyncRedis redis_;
};

AsyncWorkload::AsyncWorkload(std::string server_type)
    : impl_(std::make_unique<Impl>(std::move(server_type))) {}
AsyncWorkload::~AsyncWorkload() = default;

Awaitable<std::string> AsyncWorkload::invoke(std::string tool_name, Json arguments) {
    co_return co_await impl_->invoke(std::move(tool_name), std::move(arguments));
}

}  // namespace mcp_benchmark
