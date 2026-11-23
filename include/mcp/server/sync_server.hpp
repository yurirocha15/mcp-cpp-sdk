#pragma once

#include <mcp/core/export.hpp>

#include <string>

namespace mcp {

class Server;

/**
 * @brief Blocking twins of the Server's notify_ coroutines, from Server::sync().
 *
 * @details Each member runs the coroutine on the event loop of run_stdio() or run_http() and
 * blocks the calling thread until it finishes, rethrowing what it throws. When the server is not
 * running there is no client to notify, and the member returns at once; a run that ends before the
 * notification is sent ends the wait as well.
 * @throws std::logic_error From each member when called on that event loop's thread, where
 *         synchronous handlers run (except those set_handler_executor() moves) and waiting
 *         would never end; and when the server runs only through
 *         Server::run() or a StreamableHttpSessionManager, whose coroutines are awaited instead.
 */
class MCP_API SyncServer {
   public:
    explicit SyncServer(Server& server);

    /// Blocking Server::notify_tools_list_changed().
    void notify_tools_list_changed();
    /// Blocking Server::notify_resources_list_changed().
    void notify_resources_list_changed();
    /// Blocking Server::notify_prompts_list_changed().
    void notify_prompts_list_changed();
    /// Blocking Server::notify_resource_updated().
    void notify_resource_updated(const std::string& uri);

   private:
    Server* server_;
};

}  // namespace mcp
