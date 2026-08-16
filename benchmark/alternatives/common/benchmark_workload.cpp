#include "benchmark_workload.hpp"

#include <curl/curl.h>
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mcp_benchmark {
namespace {

using Json = nlohmann::json;

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

class ThreadPool {
   public:
    explicit ThreadPool(std::size_t count) {
        workers_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                        if (stopping_ && tasks_.empty()) {
                            return;
                        }
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    template <class F>
    auto submit(F&& fn) -> std::future<decltype(fn())> {
        using Result = decltype(fn());
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(fn));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* opaque) {
    auto* output = static_cast<std::string*>(opaque);
    output->append(data, size * count);
    return size * count;
}

class HttpClient {
   public:
    explicit HttpClient(std::string base_url) : base_url_(std::move(base_url)) {
        while (!base_url_.empty() && base_url_.back() == '/') {
            base_url_.pop_back();
        }
    }

    std::string get(const std::string& path) const { return request(base_url_ + path, nullptr); }

    std::string post(const std::string& path, const std::string& body) const {
        return request(base_url_ + path, &body);
    }

   private:
    static std::string request(const std::string& url, const std::string* body) {
        thread_local std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                              &curl_easy_cleanup);
        if (!curl) {
            throw std::runtime_error("curl_easy_init failed");
        }

        std::string output;
        curl_easy_reset(curl.get());
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &output);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

        curl_slist* headers = nullptr;
        if (body) {
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body->data());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
        }

        const CURLcode status = curl_easy_perform(curl.get());
        long response_code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);
        if (headers) {
            curl_slist_free_all(headers);
        }
        if (status != CURLE_OK) {
            throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(status));
        }
        if (response_code < 200 || response_code >= 300) {
            throw std::runtime_error("HTTP request returned status " + std::to_string(response_code));
        }
        return output;
    }

    std::string base_url_;
};

struct RedisEndpoint {
    std::string host = "redis";
    int port = 6379;
};

RedisEndpoint parse_redis_url(std::string url) {
    constexpr const char* prefix = "redis://";
    if (url.rfind(prefix, 0) == 0) {
        url.erase(0, std::char_traits<char>::length(prefix));
    }
    const auto slash = url.find('/');
    if (slash != std::string::npos) {
        url.resize(slash);
    }
    const auto colon = url.rfind(':');
    if (colon == std::string::npos) {
        return {url, 6379};
    }
    return {url.substr(0, colon), std::stoi(url.substr(colon + 1))};
}

class RedisClient {
   public:
    explicit RedisClient(RedisEndpoint endpoint) : endpoint_(std::move(endpoint)) {}

    std::vector<std::string> string_array(const std::vector<std::string>& args) const {
        Reply reply = command(args);
        std::vector<std::string> values;
        if (!reply.value || reply.value->type != REDIS_REPLY_ARRAY) {
            return values;
        }
        values.reserve(reply.value->elements);
        for (std::size_t i = 0; i < reply.value->elements; ++i) {
            const redisReply* item = reply.value->element[i];
            values.emplace_back(item && item->str ? std::string(item->str, item->len) : "");
        }
        return values;
    }

    std::int64_t integer(const std::vector<std::string>& args) const {
        Reply reply = command(args);
        return reply.value && reply.value->type == REDIS_REPLY_INTEGER ? reply.value->integer : 0;
    }

    void discard(const std::vector<std::string>& args) const { (void)command(args); }

   private:
    struct Reply {
        redisReply* value = nullptr;
        ~Reply() {
            if (value) {
                freeReplyObject(value);
            }
        }
        Reply(const Reply&) = delete;
        Reply& operator=(const Reply&) = delete;
        Reply(Reply&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
        explicit Reply(redisReply* input) : value(input) {}
    };

    struct Connection {
        redisContext* value = nullptr;
        ~Connection() {
            if (value) {
                redisFree(value);
            }
        }
    };

    Reply command(const std::vector<std::string>& args) const {
        thread_local Connection connection;
        if (!connection.value || connection.value->err) {
            if (connection.value) {
                redisFree(connection.value);
            }
            connection.value = redisConnect(endpoint_.host.c_str(), endpoint_.port);
            if (!connection.value || connection.value->err) {
                throw std::runtime_error("Redis connection failed");
            }
        }

        std::vector<const char*> argv;
        std::vector<std::size_t> lengths;
        argv.reserve(args.size());
        lengths.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(arg.data());
            lengths.push_back(arg.size());
        }
        auto* raw = static_cast<redisReply*>(redisCommandArgv(
            connection.value, static_cast<int>(argv.size()), argv.data(), lengths.data()));
        if (!raw) {
            redisFree(connection.value);
            connection.value = nullptr;
            throw std::runtime_error("Redis command failed");
        }
        return Reply(raw);
    }

    RedisEndpoint endpoint_;
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

}  // namespace

class Workload::Impl {
   public:
    explicit Impl(std::string server_type)
        : server_type_(std::move(server_type)),
          http_(env_or("API_SERVICE_URL", "http://api-service:8100")),
          redis_(parse_redis_url(env_or("REDIS_URL", "redis://redis:6379"))),
          pool_(64) {
        static const int curl_initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
        if (curl_initialized != CURLE_OK) {
            throw std::runtime_error("curl_global_init failed");
        }
    }

    std::string invoke(const std::string& name, const std::string& raw_arguments) {
        const Json args = raw_arguments.empty() ? Json::object() : Json::parse(raw_arguments);
        if (name == "search_products") {
            return search_products(args).dump();
        }
        if (name == "get_user_cart") {
            return get_user_cart(args).dump();
        }
        if (name == "checkout") {
            return checkout(args).dump();
        }
        throw std::invalid_argument("unknown benchmark tool: " + name);
    }

   private:
    Json search_products(const Json& args) {
        const std::string category = args.value("category", "Electronics");
        const double min_price = args.value("min_price", 50.0);
        const double max_price = args.value("max_price", 500.0);
        const int limit = args.value("limit", 10);
        const std::string path = "/products/search?category=" + category +
                                 "&min_price=" + number(min_price) + "&max_price=" + number(max_price) +
                                 "&limit=" + std::to_string(limit);

        auto search = pool_.submit([this, path] { return http_.get(path); });
        auto popular = pool_.submit(
            [this] { return redis_.string_array({"ZREVRANGE", "bench:popular", "0", "9"}); });
        Json search_data = Json::parse(search.get());
        const auto popular_raw = popular.get();

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
        return {{"category", category},
                {"total_found", search_data.value("total_found", 0)},
                {"products", std::move(products)},
                {"top10_popular_ids", std::move(ids)},
                {"server_type", server_type_}};
    }

    Json get_user_cart(const Json& args) {
        const std::string user_id = args.value("user_id", "user-00042");
        const auto hash = redis_.string_array({"HGETALL", "bench:cart:" + user_id});
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

        auto product = pool_.submit(
            [this, product_id] { return http_.get("/products/" + std::to_string(product_id)); });
        auto history = pool_.submit([this, user_id] {
            return redis_.string_array({"LRANGE", "bench:history:" + user_id, "0", "4"});
        });
        (void)product.get();
        Json recent = Json::array();
        for (const auto& entry : history.get()) {
            Json parsed = Json::parse(entry, nullptr, false);
            recent.push_back(parsed.is_discarded() ? Json{{"raw", entry}} : std::move(parsed));
        }
        return {{"user_id", user_id},
                {"cart", {{"items", items}, {"item_count", items.size()}, {"estimated_total", total}}},
                {"recent_history", std::move(recent)},
                {"server_type", server_type_}};
    }

    Json checkout(const Json& args) {
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

        auto calculated = pool_.submit(
            [this, body = calculate.dump()] { return http_.post("/cart/calculate", body); });
        auto rate =
            pool_.submit([this, key = rate_key.str()] { return redis_.integer({"INCR", key}); });
        auto history = pool_.submit([this, history_key, value = order_entry.dump()] {
            redis_.discard({"RPUSH", history_key, value});
        });
        auto popularity = pool_.submit([this, product_id] {
            redis_.discard({"ZINCRBY", "bench:popular", "1", "product:" + std::to_string(product_id)});
        });
        Json calc = Json::parse(calculated.get());
        const auto rate_count = rate.get();
        history.get();
        popularity.get();
        return {{"order_id", calc.value("order_id", order_entry["order_id"].get<std::string>())},
                {"user_id", user_id},
                {"total", calc.value("total", 0.0)},
                {"items_count", items.size()},
                {"rate_limit_count", rate_count},
                {"status", "confirmed"},
                {"server_type", server_type_}};
    }

    std::string server_type_;
    HttpClient http_;
    RedisClient redis_;
    ThreadPool pool_;
};

Workload::Workload(std::string server_type) : impl_(std::make_unique<Impl>(std::move(server_type))) {}
Workload::~Workload() = default;

std::string Workload::invoke(const std::string& tool_name, const std::string& arguments_json) {
    return impl_->invoke(tool_name, arguments_json);
}

std::string Workload::input_schema(const std::string& tool_name) {
    if (tool_name == "search_products") {
        return R"({"type":"object","properties":{"category":{"type":"string"},"min_price":{"type":"number"},"max_price":{"type":"number"},"limit":{"type":"integer"}}})";
    }
    if (tool_name == "get_user_cart") {
        return R"({"type":"object","properties":{"user_id":{"type":"string"}}})";
    }
    if (tool_name == "checkout") {
        return R"({"type":"object","properties":{"user_id":{"type":"string"},"items":{"type":"array","items":{"type":"object","properties":{"product_id":{"type":"integer"},"quantity":{"type":"integer"}},"required":["product_id","quantity"]}}}})";
    }
    throw std::invalid_argument("unknown benchmark tool: " + tool_name);
}

}  // namespace mcp_benchmark
