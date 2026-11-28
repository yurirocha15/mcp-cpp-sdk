#include <mcp/server/sync_server.hpp>

#include "server_impl.hpp"

#include <memory>
#include <string>

namespace mcp {
namespace {

/// Awaits Server::notify_elicitation_complete() and stores what it returns in @p sent.
Task<void> store_completion(Server& server, std::shared_ptr<const std::string> elicitation_id,
                            std::shared_ptr<bool> sent) {
    *sent = co_await server.notify_elicitation_complete(*elicitation_id);
}

}  // namespace

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

bool SyncServer::notify_elicitation_complete(const std::string& elicitation_id) {
    auto sent = std::make_shared<bool>(false);
    server_->run_control()->run_blocking([server = server_,
                                          id = std::make_shared<const std::string>(elicitation_id),
                                          sent] { return store_completion(*server, id, sent); });
    return *sent;
}

void SyncServer::notify(std::string method, std::optional<nlohmann::json> params) {
    Server::Impl::check_notification(method, params);
    server_->run_control()->run_blocking(
        [server = server_, method = std::make_shared<const std::string>(std::move(method)),
         params = std::make_shared<const std::optional<nlohmann::json>>(std::move(params))] {
            return server->notify(*method, *params);
        });
}

void SyncServer::notify_resource_updated(const std::string& uri) {
    server_->run_control()->run_blocking(
        [server = server_, uri] { return server->notify_resource_updated(uri); });
}

}  // namespace mcp
