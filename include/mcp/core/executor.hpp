#pragma once

// Boost-free: an Executor can be stored, copied and passed on without Boost. Converting an Asio
// executor to one, and reading the Asio executor back, needs <mcp/core/async.hpp>.

#include <concepts>
#include <memory>
#include <type_traits>

namespace mcp {

class Executor;

namespace detail {

/// Wraps an Asio executor into an Executor; defined in <mcp/core/async.hpp>.
template <typename AsioExecutor>
struct ExecutorWrap;

/// Reads the Asio executor an Executor holds; defined in <mcp/core/async.hpp>.
template <typename Unused>
struct ExecutorAccess;

}  // namespace detail

/**
 * @brief Where asynchronous work runs: an event loop, a strand or a thread pool.
 *
 * @details Holds a boost::asio::any_io_executor. An Asio executor such as io.get_executor()
 * converts to an Executor implicitly, and get() returns the Asio executor; both need
 * <mcp/core/async.hpp>. Copies share the executor they hold. A default-constructed Executor holds
 * none, and the SDK refuses it with std::invalid_argument.
 */
class Executor {
   public:
    /// An executor that holds no Asio executor.
    Executor() noexcept = default;

    /// Wrap an Asio executor (for example io.get_executor() or a strand).
    template <typename AsioExecutor>
        requires(!std::same_as<std::remove_cvref_t<AsioExecutor>, Executor> &&
                 std::is_class_v<std::remove_cvref_t<AsioExecutor>>)
    Executor(const AsioExecutor& executor)  // NOLINT(google-explicit-constructor)
        : target_(detail::ExecutorWrap<AsioExecutor>::wrap(executor)) {}

    /// The Asio executor held, as a const boost::asio::any_io_executor&.
    /// @throws std::invalid_argument If this Executor holds none.
    template <typename Unused = void>
    [[nodiscard]] const auto& get() const {
        return detail::ExecutorAccess<Unused>::get(*this);
    }

    /// Whether this Executor holds an Asio executor.
    explicit operator bool() const noexcept { return target_ != nullptr; }

   private:
    template <typename>
    friend struct detail::ExecutorWrap;
    template <typename>
    friend struct detail::ExecutorAccess;

    std::shared_ptr<const void> target_;
};

}  // namespace mcp
