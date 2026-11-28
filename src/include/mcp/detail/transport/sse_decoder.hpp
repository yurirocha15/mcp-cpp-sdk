#pragma once

#include "mcp/detail/constants.hpp"

#include <mcp/transport/transport.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

/// Reads a text/event-stream body in the pieces the network delivers it and returns each event, or
/// its data alone, as soon as its terminating blank line arrives. It keeps the last event id and the
/// reconnection time the server asked for, which a resume needs; comment lines and other fields are
/// skipped. As the SSE standard has it, CRLF, LF and CR each end a line, an `id` counts once its
/// event has arrived whole, and an event the stream ends inside of is dropped.
class SseDecoder {
   public:
    /// A dispatched event: its `event` field, or "message" when it named none, and its data.
    struct SseEvent {
        std::string name;
        std::string data;
    };

    SseDecoder() = default;
    /// A decoder for a stream resumed after the event @p last_id.
    explicit SseDecoder(std::string last_id) : last_id_(last_id), id_buffer_(std::move(last_id)) {}

    /// The id of the last whole event that named one, which a resume sends as Last-Event-ID.
    [[nodiscard]] const std::string& last_id() const { return last_id_; }

    /// The reconnection time a `retry` field asked for, if one did.
    [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const { return retry_; }

    /// Whether the stream carried an event or an event id.
    [[nodiscard]] bool carried_anything() const { return carried_anything_; }

    /// The events @p chunk completes, each with its name and data.
    std::vector<SseEvent> feed_events(std::string_view chunk) {
        std::vector<SseEvent> events;
        for (const char symbol : chunk) {
            // The LF of a CRLF, which may come in the next chunk, ends no line of its own.
            const bool second_of_crlf = symbol == '\n' && after_carriage_return_;
            after_carriage_return_ = symbol == '\r';
            if (second_of_crlf) {
                continue;
            }
            if (symbol != '\n' && symbol != '\r') {
                line_.push_back(symbol);
                if (line_.size() > constants::http_client::g_MAX_MODERN_MESSAGE_BYTES) {
                    throw TransportError(TransportError::Kind::eProtocol,
                                         "An SSE line from the server is too long");
                }
                continue;
            }
            consume_line(events);
        }
        return events;
    }

    /// The data of the events @p chunk completes.
    std::vector<std::string> feed(std::string_view chunk) {
        std::vector<std::string> data;
        for (auto& event : feed_events(chunk)) {
            data.push_back(std::move(event.data));
        }
        return data;
    }

   private:
    void consume_line(std::vector<SseEvent>& events) {
        if (line_.empty()) {
            // The blank line ends the event: its id, or the one before, becomes the stream's.
            if (id_named_) {
                last_id_ = id_buffer_;
                id_named_ = false;
                carried_anything_ = true;
            }
            if (!payload_.empty()) {
                events.push_back(SseEvent{
                    .name = event_name_.empty() ? "message" : std::move(event_name_),
                    .data = std::move(payload_),
                });
                payload_.clear();
                carried_anything_ = true;
            }
            event_name_.clear();
        } else if (const auto id = field_value("id:")) {
            const std::string_view value = *id;
            // An id holding NUL is ignored, as the SSE standard has it.
            if (value.find('\0') == std::string_view::npos) {
                id_buffer_.assign(value.data(), value.size());
                id_named_ = true;
            }
        } else if (const auto name = field_value("event:")) {
            event_name_.assign(name->data(), name->size());
        } else if (const auto retry = field_value("retry:")) {
            const std::string_view value = *retry;
            if (!value.empty() && value.size() <= g_max_retry_digits &&
                value.find_first_not_of("0123456789") == std::string_view::npos) {
                retry_ = std::chrono::milliseconds(std::stoll(std::string(value)));
            }
        } else if (const auto data = field_value("data:")) {
            const std::string_view value = *data;
            if (payload_.size() + value.size() + 1 >
                constants::http_client::g_MAX_MODERN_MESSAGE_BYTES) {
                throw TransportError(TransportError::Kind::eProtocol,
                                     "An SSE event from the server is too long");
            }
            if (!payload_.empty()) {
                payload_.push_back('\n');
            }
            payload_.append(value.data(), value.size());
        }
        line_.clear();
    }

    /// The value of the line when it is the field @p name, which ends with its colon: the rest of
    /// the line, less one leading space.
    [[nodiscard]] std::optional<std::string_view> field_value(std::string_view name) const {
        std::string_view line = line_;
        if (!line.starts_with(name)) {
            return std::nullopt;
        }
        line.remove_prefix(name.size());
        if (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        return line;
    }

    /// Longest `retry` value read, in digits: past this the field is ignored.
    static constexpr std::size_t g_max_retry_digits = 9;

    std::string line_;
    std::string payload_;
    std::string event_name_;
    std::string last_id_;
    /// The id named since the last blank line, which counts once that event ends.
    std::string id_buffer_;
    bool id_named_ = false;
    bool after_carriage_return_ = false;
    bool carried_anything_ = false;
    std::optional<std::chrono::milliseconds> retry_;
};

}  // namespace mcp::detail
