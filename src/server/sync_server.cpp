#include <mcp/server/sync_server.hpp>

#include "server_impl.hpp"

#include <string>

namespace mcp {

SyncServer::SyncServer(Server& server) : server_(&server) {}

void SyncServer::notify_tools_list_changed() {
    server_->run_control()->run_blocking(
        [server = server_] { return server->notify_tools_list_changed(); });
}

void SyncServer::notify_resources_list_changed() {
    server_->run_control()->run_blocking(
        [server = server_] { return server->notify_resources_list_changed(); });
}

void SyncServer::notify_prompts_list_changed() {
    server_->run_control()->run_blocking(
        [server = server_] { return server->notify_prompts_list_changed(); });
}

void SyncServer::notify_resource_updated(const std::string& uri) {
    server_->run_control()->run_blocking(
        [server = server_, uri] { return server->notify_resource_updated(uri); });
}

}  // namespace mcp
