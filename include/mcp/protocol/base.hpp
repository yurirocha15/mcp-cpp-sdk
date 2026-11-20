#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mcp {

namespace detail {

inline void validate_jsonrpc_version(std::string_view version) {
    if (version != "2.0") {
        throw std::invalid_argument("jsonrpc must be \"2.0\"");
    }
}

/**
 * @brief Reports whether @p key is present in @p json_obj and carries a value.
 *
 * An absent optional and one serialized as an explicit `null` are the same statement on
 * the wire -- "no value" -- so `from_json` must read them the same way. Writing
 * `contains(key)` alone accepts the null and then throws when the value is extracted.
 *
 * Returns false for a @p json_obj that is not an object, so a null payload decodes into a
 * type whose members are all optional rather than failing.
 */
inline bool has_json_value(const nlohmann::json& json_obj, const char* key) {
    const auto iter = json_obj.find(key);
    return iter != json_obj.end() && !iter->is_null();
}

}  // namespace detail

// MCP Protocol Constants

// Error-code allocation policy: -32000..-32019 is a grandfathered legacy band
// (pre-existing codes above); -32020..-32099 is the spec-reserved band for
// codes introduced by newer MCP spec revisions, such as the ones below.

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
 * Wraps `std::variant<std::string, int64_t>` as a named type so that `to_json`/`from_json` are found
 * by ADL in `namespace mcp`.
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
     * @brief Converts the request ID to its unadorned string representation.
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

    /** @brief Return a type-preserving key for request correlation. */
    [[nodiscard]] std::string correlation_key() const {
        return std::visit(
            [](const auto& val) -> std::string {
                if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                    return "s:" + val;
                } else {
                    return "i:" + std::to_string(val);
                }
            },
            value);
    }
};

MCP_API void to_json(nlohmann::json& json_obj, const RequestId& id);

namespace detail {

/// Whether @p value can be a request id: a string, or an integer that fits in int64_t. A larger one
/// could not be echoed back unchanged, so its answer would never be matched to the request.
[[nodiscard]] inline bool is_request_id(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    }
    return value.is_string() || value.is_number_integer();
}

}  // namespace detail

MCP_API void from_json(const nlohmann::json& json_obj, RequestId& id);

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

MCP_API void to_json(nlohmann::json& json_obj, const Error& error);

MCP_API void from_json(const nlohmann::json& json_obj, Error& error);

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

MCP_API void to_json(nlohmann::json& json_obj, const TaskStatus& status);

/// Not NLOHMANN_JSON_SERIALIZE_ENUM, which gives a value it does not know the first entry: a task
/// from a server that sends something else would then read as cancelled.
MCP_API void from_json(const nlohmann::json& json_obj, TaskStatus& status);

/**
 * @brief A JSON-RPC request message.
 */
struct JSONRPCRequest {
    RequestId id;                          ///< Unique request identifier.
    std::string jsonrpc = "2.0";           ///< JSON-RPC version (always "2.0").
    std::string method;                    ///< The method to invoke.
    std::optional<nlohmann::json> params;  ///< Optional parameters object.
};

MCP_API void to_json(nlohmann::json& json_obj, const JSONRPCRequest& req);

MCP_API void from_json(const nlohmann::json& json_obj, JSONRPCRequest& req);

/**
 * @brief A JSON-RPC notification message.
 */
struct JSONRPCNotification {
    std::string jsonrpc = "2.0";           ///< JSON-RPC version (always "2.0").
    std::string method;                    ///< The notification method.
    std::optional<nlohmann::json> params;  ///< Optional parameters object.
};

MCP_API void to_json(nlohmann::json& json_obj, const JSONRPCNotification& notif);

MCP_API void from_json(const nlohmann::json& json_obj, JSONRPCNotification& notif);

/**
 * @brief A successful JSON-RPC response message.
 */
struct JSONRPCResultResponse {
    RequestId id;                 ///< The request ID this responds to.
    std::string jsonrpc = "2.0";  ///< JSON-RPC version (always "2.0").
    nlohmann::json result;        ///< The result payload.
};

MCP_API void to_json(nlohmann::json& json_obj, const JSONRPCResultResponse& resp);

MCP_API void from_json(const nlohmann::json& json_obj, JSONRPCResultResponse& resp);

/**
 * @brief An error JSON-RPC response message.
 */
struct JSONRPCErrorResponse {
    Error error;                  ///< The error object.
    std::string jsonrpc = "2.0";  ///< JSON-RPC version (always "2.0").
    /// The request ID, or no value when it cannot be determined. Then `id` is left out, as MCP's
    /// schema has it; a null id is read as no value.
    std::optional<RequestId> id;
};

MCP_API void to_json(nlohmann::json& json_obj, const JSONRPCErrorResponse& resp);

MCP_API void from_json(const nlohmann::json& json_obj, JSONRPCErrorResponse& resp);

/// @brief Variant of all JSON-RPC response message forms.
using JSONRPCResponse = std::variant<JSONRPCResultResponse, JSONRPCErrorResponse>;

MCP_API void to_json(nlohmann::json& json_obj, const JSONRPCResponse& resp);

MCP_API void from_json(const nlohmann::json& json_obj, JSONRPCResponse& resp);

/// @brief Variant of all JSON-RPC message forms handled by the SDK.
using JSONRPCMessage =
    std::variant<JSONRPCRequest, JSONRPCNotification, JSONRPCResultResponse, JSONRPCErrorResponse>;

MCP_API void to_json(nlohmann::json& json_obj, const JSONRPCMessage& msg);

MCP_API void from_json(const nlohmann::json& json_obj, JSONRPCMessage& msg);

}  // namespace mcp
