#pragma once

namespace mcp {

class StreamableHttpSessionManager;

namespace detail {

/// Leaves the end of the run of a manager that serves a Server to its caller. Without it
/// Server::stop() closes the manager while it listens; Server::run_http() drains it instead.
void leave_stop_to_run(StreamableHttpSessionManager& manager);

}  // namespace detail

}  // namespace mcp
