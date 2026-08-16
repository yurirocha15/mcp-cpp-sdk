#pragma once

#include <mcp/core/core.hpp>
#include <mcp/core/export.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace mcp {

class ITransport;

namespace detail {

struct SerializedTransportWriterState;

/**
 * @brief Serializes asynchronous writes to a shared transport.
 *
 * Calls are queued on an Asio strand and written in FIFO order. A failed
 * transport write fails the active call, every queued call, and future calls.
 * The shared implementation keeps both the transport and outstanding writes
 * alive if this lightweight wrapper is destroyed.
 */
class MCP_API SerializedTransportWriter {
   public:
    SerializedTransportWriter(std::shared_ptr<ITransport> transport,
                              const boost::asio::any_io_executor& executor);

    /**
     * @brief Queue one complete serialized message for writing.
     *
     * The input view is copied before this function returns its awaitable, so
     * the caller does not need to keep the referenced storage alive.
     */
    [[nodiscard]] Task<void> write_message(std::string_view message) const;

    /**
     * @brief Queue an already-owned message without another payload copy.
     * @param message Immutable message storage retained until the write completes.
     */
    [[nodiscard]] Task<void> write_message(std::shared_ptr<const std::string> message) const;

    /**
     * @brief Queue an owned message that may be canceled before its write starts.
     *
     * Once the transport write has started, setting @p cancel_before_start has
     * no effect because ITransport has no per-write cancellation contract.
     */
    [[nodiscard]] Task<void> write_message(
        std::shared_ptr<const std::string> message,
        std::shared_ptr<const std::atomic<bool>> cancel_before_start) const;

   private:
    std::shared_ptr<SerializedTransportWriterState> state_;
};

}  // namespace detail
}  // namespace mcp
