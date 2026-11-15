#pragma once

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace mcp_benchmark {

/// The same tool workload as Workload, written for C++20 coroutines.
///
/// Redis and the API service are reached through asio sockets, so a tool handler suspends while it
/// waits instead of blocking a thread, and independent requests of one tool call run concurrently.
/// The Redis commands, HTTP requests and result shape match Workload.
class AsyncWorkload {
   public:
    explicit AsyncWorkload(std::string server_type);
    ~AsyncWorkload();

    AsyncWorkload(const AsyncWorkload&) = delete;
    AsyncWorkload& operator=(const AsyncWorkload&) = delete;

    boost::asio::awaitable<std::string> invoke(std::string tool_name, nlohmann::json arguments);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp_benchmark
