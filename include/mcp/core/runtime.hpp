#pragma once

#include <mcp/core/export.hpp>

#include <memory>

namespace mcp {

/// Manages the asynchronous I/O event loop used by transports and servers.
class MCP_API Runtime {
   public:
    Runtime();
    ~Runtime();

    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Run the event loop (blocks until stop() or no more work).
    void run();

    /// Request the event loop to stop.
    void stop();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct RuntimeAccess;
};

}  // namespace mcp
