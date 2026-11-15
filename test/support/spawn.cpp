#include "spawn.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_future.hpp>
#include <nlohmann/json.hpp>

namespace mcp_test {

namespace {

// The function is a parameter, so it lives in this coroutine's frame until the coroutine it
// returned has finished.
mcp::Task<void> run_function(TaskFunction function) { co_await function(); }

mcp::Task<bool> run_function(UniqueFunction<mcp::Task<bool>()> function) {
    co_return co_await function();
}

}  // namespace

void spawn_detached(const SpawnExecutor& executor, mcp::Task<void> task) {
    boost::asio::co_spawn(executor.get(), std::move(task), boost::asio::detached);
}

void spawn_detached(const SpawnExecutor& executor, TaskFunction function) {
    spawn_detached(executor, run_function(std::move(function)));
}

void spawn_then(const SpawnExecutor& executor, mcp::Task<void> task,
                UniqueFunction<void(std::exception_ptr)> completion) {
    boost::asio::co_spawn(executor.get(), std::move(task),
                          [completion = std::move(completion)](std::exception_ptr error) mutable {
                              completion(std::move(error));
                          });
}

void spawn_then(const SpawnExecutor& executor, TaskFunction function,
                UniqueFunction<void(std::exception_ptr)> completion) {
    spawn_then(executor, run_function(std::move(function)), std::move(completion));
}

void spawn_then(const SpawnExecutor& executor, mcp::Task<std::string> task,
                UniqueFunction<void(std::exception_ptr, std::string)> completion) {
    boost::asio::co_spawn(
        executor.get(), std::move(task),
        [completion = std::move(completion)](std::exception_ptr error, std::string value) mutable {
            completion(std::move(error), std::move(value));
        });
}

void spawn_then(const SpawnExecutor& executor, mcp::Task<nlohmann::json> task,
                UniqueFunction<void(std::exception_ptr, nlohmann::json)> completion) {
    boost::asio::co_spawn(
        executor.get(), std::move(task),
        [completion = std::move(completion)](std::exception_ptr error, nlohmann::json value) mutable {
            completion(std::move(error), std::move(value));
        });
}

std::future<void> spawn_future(const SpawnExecutor& executor, mcp::Task<void> task) {
    return boost::asio::co_spawn(executor.get(), std::move(task), boost::asio::use_future);
}

std::future<void> spawn_future(const SpawnExecutor& executor, TaskFunction function) {
    return spawn_future(executor, run_function(std::move(function)));
}

std::future<bool> spawn_future(const SpawnExecutor& executor, mcp::Task<bool> task) {
    return boost::asio::co_spawn(executor.get(), std::move(task), boost::asio::use_future);
}

std::future<bool> spawn_future(const SpawnExecutor& executor,
                               UniqueFunction<mcp::Task<bool>()> function) {
    return spawn_future(executor, run_function(std::move(function)));
}

std::future<std::string> spawn_future(const SpawnExecutor& executor, mcp::Task<std::string> task) {
    return boost::asio::co_spawn(executor.get(), std::move(task), boost::asio::use_future);
}

std::future<nlohmann::json> spawn_future(const SpawnExecutor& executor,
                                         mcp::Task<nlohmann::json> task) {
    return boost::asio::co_spawn(executor.get(), std::move(task), boost::asio::use_future);
}

}  // namespace mcp_test
