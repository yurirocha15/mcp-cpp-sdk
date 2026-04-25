#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mcp {

// MCP Protocol Constants

/**
 * @brief JSON-RPC error code for unauthorized request.
 */
inline constexpr int g_UNAUTHORIZED = -32000;

/**
 * @brief JSON-RPC error code for request timeout.
 */
inline constexpr int g_REQUEST_TIMEOUT = -32001;

/**
 * @brief JSON-RPC error code for invalid requests.
 */
inline constexpr int g_INVALID_REQUEST = -32600;

/**
 * @brief JSON-RPC error code for method not found.
 */
inline constexpr int g_METHOD_NOT_FOUND = -32601;

/**
 * @brief JSON-RPC error code for invalid parameters.
 */
inline constexpr int g_INVALID_PARAMS = -32602;

/**
 * @brief JSON-RPC error code for internal errors.
 */
inline constexpr int g_INTERNAL_ERROR = -32603;

/**
 * @brief JSON-RPC error code for parse errors.
 */
inline constexpr int g_PARSE_ERROR = -32700;

/**
 * @brief JSON-RPC error code for request cancelled.
 */
inline constexpr int g_REQUEST_CANCELLED = -32800;

/**
 * @brief The sender or recipient of messages and data in a conversation.
 */
enum class Role : std::uint8_t {
    eUser,      ///< The user.
    eAssistant  ///< The assistant (LLM).
};

NLOHMANN_JSON_SERIALIZE_ENUM(Role, {{Role::eUser, "user"}, {Role::eAssistant, "assistant"}})

// JSON-RPC Base Types

/**
 * @brief JSON-RPC request identifier: either a string or an integer.
 *
 * Wraps `std::variant<std::string, int64_t>` as a named type so that ADL-based
 * `to_json`/`from_json` can live directly in `namespace mcp` without closing
 * and reopening the namespace to specialize `nlohmann::adl_serializer`.
 *
 * Implicit constructors allow transparent assignment from string and integer literals:
 * @code
 *   RequestId id = "req-1";
 *   RequestId id = int64_t{42};
 * @endcode
 */
struct RequestId {
    std::variant<std::string, int64_t> value;

    RequestId() = default;
    // NOLINTNEXTLINE(google-explicit-constructor)
    RequestId(std::string s) : value(std::move(s)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    RequestId(const char* s) : value(std::string(s)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    RequestId(int64_t i) : value(i) {}

    bool operator==(const RequestId&) const = default;

    /**
     * @brief Converts the request ID to a string representation (used as map key for pending request
     * correlation).
     */
    [[nodiscard]] std::string to_string() const {
        return std::visit(
            [](auto&& val) -> std::string {
                if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                    return val;
                } else {
                    return std::to_string(val);
                }
            },
            value);
    }
};

inline void to_json(nlohmann::json& json_obj, const RequestId& id) {
    std::visit([&json_obj](auto&& val) { json_obj = val; }, id.value);
}

inline void from_json(const nlohmann::json& json_obj, RequestId& id) {
    if (json_obj.is_string()) {
        id.value = json_obj.get<std::string>();
    } else if (json_obj.is_number_integer()) {
        id.value = json_obj.get<int64_t>();
    } else {
        throw std::invalid_argument("RequestId must be a string or integer");
    }
}

/// @brief Token used to correlate progress updates with a request or task.
using ProgressToken = RequestId;

/**
 * @brief Standard JSON-RPC error payload.
 */
struct Error {
    int code;                            ///< The error type that occurred.
    std::string message;                 ///< A short description of the error.
    std::optional<nlohmann::json> data;  ///< Additional information about the error.
};

inline void to_json(nlohmann::json& json_obj, const Error& error) {
    json_obj = nlohmann::json{{"code", error.code}, {"message", error.message}};
    if (error.data) {
        json_obj["data"] = *error.data;
    }
}

inline void from_json(const nlohmann::json& json_obj, Error& error) {
    json_obj.at("code").get_to(error.code);
    json_obj.at("message").get_to(error.message);
    if (json_obj.contains("data")) {
        error.data = json_obj.at("data");
    }
}

/**
 * @brief The status of a task.
 */
enum class TaskStatus : std::uint8_t {
    eCancelled,      ///< Task has been cancelled.
    eCompleted,      ///< Task has completed successfully.
    eFailed,         ///< Task has failed.
    eInputRequired,  ///< Task requires additional input.
    eWorking         ///< Task is currently in progress.
};

NLOHMANN_JSON_SERIALIZE_ENUM(TaskStatus, {{TaskStatus::eCancelled, "cancelled"},
                                          {TaskStatus::eCompleted, "completed"},
                                          {TaskStatus::eFailed, "failed"},
                                          {TaskStatus::eInputRequired, "input_required"},
                                          {TaskStatus::eWorking, "working"}})

/**
 * @brief Metadata about a related task.
 */
struct RelatedTaskMetadata {
    std::string id;                    ///< The ID of the related task.
    std::optional<std::string> title;  ///< An optional title.
};

inline void to_json(nlohmann::json& j, const RelatedTaskMetadata& t) {
    j = nlohmann::json{{"id", t.id}};
    if (t.title) {
        j["title"] = *t.title;
    }
}
inline void from_json(const nlohmann::json& j, RelatedTaskMetadata& t) {
    j.at("id").get_to(t.id);
    if (j.contains("title")) {
        t.title = j.at("title").get<std::string>();
    }
}

/**
 * @brief Metadata for augmenting a request with task execution.
 */
struct TaskMetadata {
    std::optional<int64_t> ttl;  ///< Requested retention duration from creation in milliseconds.
    std::optional<std::vector<RelatedTaskMetadata>> relatedTasks;  ///< Related tasks.
};

inline void to_json(nlohmann::json& json_obj, const TaskMetadata& meta) {
    json_obj = nlohmann::json::object();
    if (meta.ttl) {
        json_obj["ttl"] = *meta.ttl;
    }
    if (meta.relatedTasks) {
        json_obj["relatedTasks"] = *meta.relatedTasks;
    }
}

inline void from_json(const nlohmann::json& json_obj, TaskMetadata& meta) {
    if (json_obj.contains("ttl")) {
        meta.ttl = json_obj.at("ttl").get<int64_t>();
    }
    if (json_obj.contains("relatedTasks")) {
        meta.relatedTasks = json_obj.at("relatedTasks").get<std::vector<RelatedTaskMetadata>>();
    }
}

/**
 * @brief A JSON-RPC request message.
 */
struct JSONRPCRequest {
    RequestId id;                          ///< Unique request identifier.
    std::string jsonrpc = "2.0";           ///< JSON-RPC version (always "2.0").
    std::string method;                    ///< The method to invoke.
    std::optional<nlohmann::json> params;  ///< Optional parameters object.
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCRequest& req) {
    json_obj = nlohmann::json::object();
    json_obj["id"] = req.id;
    json_obj["jsonrpc"] = req.jsonrpc;
    json_obj["method"] = req.method;
    if (req.params) {
        json_obj["params"] = *req.params;
    }
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCRequest& req) {
    req.id = json_obj.at("id").get<RequestId>();
    json_obj.at("jsonrpc").get_to(req.jsonrpc);
    json_obj.at("method").get_to(req.method);
    if (json_obj.contains("params")) {
        req.params = json_obj.at("params");
    }
}

/**
 * @brief A JSON-RPC notification message.
 */
struct JSONRPCNotification {
    std::string jsonrpc = "2.0";           ///< JSON-RPC version (always "2.0").
    std::string method;                    ///< The notification method.
    std::optional<nlohmann::json> params;  ///< Optional parameters object.
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCNotification& notif) {
    json_obj = nlohmann::json{{"jsonrpc", notif.jsonrpc}, {"method", notif.method}};
    if (notif.params) {
        json_obj["params"] = *notif.params;
    }
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCNotification& notif) {
    json_obj.at("jsonrpc").get_to(notif.jsonrpc);
    json_obj.at("method").get_to(notif.method);
    if (json_obj.contains("params")) {
        notif.params = json_obj.at("params");
    }
}

/**
 * @brief A successful JSON-RPC response message.
 */
struct JSONRPCResultResponse {
    RequestId id;                 ///< The request ID this responds to.
    std::string jsonrpc = "2.0";  ///< JSON-RPC version (always "2.0").
    nlohmann::json result;        ///< The result payload.
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCResultResponse& resp) {
    json_obj = nlohmann::json::object();
    json_obj["id"] = resp.id;
    json_obj["jsonrpc"] = resp.jsonrpc;
    json_obj["result"] = resp.result;
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCResultResponse& resp) {
    resp.id = json_obj.at("id").get<RequestId>();
    json_obj.at("jsonrpc").get_to(resp.jsonrpc);
    json_obj.at("result").get_to(resp.result);
}

/**
 * @brief An error JSON-RPC response message.
 */
struct JSONRPCErrorResponse {
    Error error;                  ///< The error object.
    std::string jsonrpc = "2.0";  ///< JSON-RPC version (always "2.0").
    std::optional<RequestId> id;  ///< The request ID (may be absent for parse errors).
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCErrorResponse& resp) {
    json_obj = nlohmann::json::object();
    json_obj["error"] = resp.error;
    json_obj["jsonrpc"] = resp.jsonrpc;
    if (resp.id) {
        json_obj["id"] = *resp.id;
    }
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCErrorResponse& resp) {
    json_obj.at("error").get_to(resp.error);
    json_obj.at("jsonrpc").get_to(resp.jsonrpc);
    if (json_obj.contains("id")) {
        resp.id = json_obj.at("id").get<RequestId>();
    }
}

/// @brief Variant of all JSON-RPC response message forms.
using JSONRPCResponse = std::variant<JSONRPCResultResponse, JSONRPCErrorResponse>;

inline void to_json(nlohmann::json& json_obj, const JSONRPCResponse& resp) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, resp);
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCResponse& resp) {
    if (json_obj.contains("error")) {
        resp = json_obj.get<JSONRPCErrorResponse>();
    } else {
        resp = json_obj.get<JSONRPCResultResponse>();
    }
}

/// @brief Variant of all JSON-RPC message forms handled by the SDK.
using JSONRPCMessage =
    std::variant<JSONRPCRequest, JSONRPCNotification, JSONRPCResultResponse, JSONRPCErrorResponse>;

inline void to_json(nlohmann::json& json_obj, const JSONRPCMessage& msg) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, msg);
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCMessage& msg) {
    if (json_obj.contains("error")) {
        msg = json_obj.get<JSONRPCErrorResponse>();
    } else if (json_obj.contains("result")) {
        msg = json_obj.get<JSONRPCResultResponse>();
    } else if (json_obj.contains("id")) {
        msg = json_obj.get<JSONRPCRequest>();
    } else {
        msg = json_obj.get<JSONRPCNotification>();
    }
}

}  // namespace mcp
