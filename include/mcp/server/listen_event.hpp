#pragma once

#include <mcp/protocol/base.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mcp {

/// Whether a subscriptions/listen stream opened or closed.
enum class ListenEventKind : std::uint8_t {
    eOpened,  ///< The server accepted the request; its acknowledgment follows.
    eClosed,  ///< The stream ended, by the client, the server or the connection.
};

/**
 * @brief A 2026-07-28 subscriptions/listen stream that opened or closed (see Server::on_listen()).
 *
 * @details The members other than #kind, #stream and #request_id describe what the stream
 * receives: what the request asked for and the server honors, as the
 * notifications/subscriptions/acknowledged the server sends lists it.
 */
struct ListenEvent {
    ListenEventKind kind{ListenEventKind::eOpened};  ///< Whether the stream opened or closed.
    /// Identifies the stream: both events of one stream carry the same number, and the server's
    /// other streams carry other numbers.
    std::uint64_t stream{0};
    /// The id of the listen request; the streams of different clients may share it.
    RequestId request_id;
    bool tools_list_changed{false};      ///< Receives notifications/tools/list_changed.
    bool prompts_list_changed{false};    ///< Receives notifications/prompts/list_changed.
    bool resources_list_changed{false};  ///< Receives notifications/resources/list_changed.
    /// The URIs whose notifications/resources/updated the stream receives.
    std::vector<std::string> resource_uris;
    /// The tasks whose notifications/tasks the stream receives.
    std::vector<std::string> task_ids;
};

}  // namespace mcp
