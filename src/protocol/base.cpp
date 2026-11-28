#include <mcp/core/constants.hpp>
#include <mcp/protocol/base.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

namespace mcp {

namespace detail {
namespace {

/// Whether @p value is a number that is not an integer in the range of int. JSON does not tell
/// -32601 from -32601.0, so a float with no fraction counts as an integer.
[[nodiscard]] bool is_number_outside_int(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max();
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        return number < static_cast<double>(std::numeric_limits<int>::min()) ||
               number > static_cast<double>(std::numeric_limits<int>::max()) ||
               number != std::trunc(number);
    }
    return false;
}

}  // namespace
}  // namespace detail

void to_json(nlohmann::json& json_obj, const RequestId& id) {
    std::visit([&json_obj](auto&& val) { json_obj = val; }, id.value);
}

void from_json(const nlohmann::json& json_obj, RequestId& id) {
    if (!detail::is_request_id(json_obj)) {
        throw std::invalid_argument("RequestId must be a string or an integer within int64_t");
    }
    if (json_obj.is_string()) {
        id.value = json_obj.get<std::string>();
    } else {
        id.value = json_obj.get<int64_t>();
    }
}

void to_json(nlohmann::json& json_obj, const Error& error) {
    json_obj = nlohmann::json{{"code", error.code}, {"message", error.message}};
    if (error.data) {
        json_obj["data"] = *error.data;
    }
}

void from_json(const nlohmann::json& json_obj, Error& error) {
    error = Error{};
    // A numeric code that does not fit is still an error: converting it would be undefined
    // behaviour, and refusing it would leave the caller waiting for an answer.
    if (detail::is_number_outside_int(json_obj.at("code"))) {
        error.code = mcp::constants::jsonrpc::g_INTERNAL_ERROR;
    } else {
        json_obj.at("code").get_to(error.code);
    }
    // JSON-RPC 2.0 requires `message`, but a peer that omits it or sends it as null has still
    // identified which error occurred. Decoding to an empty message keeps `code` -- the part a
    // caller acts on -- rather than discarding the whole error object.
    if (detail::has_json_value(json_obj, "message")) {
        json_obj.at("message").get_to(error.message);
    } else {
        error.message.clear();
    }
    if (detail::has_json_value(json_obj, "data")) {
        error.data = json_obj.at("data");
    }
}

void to_json(nlohmann::json& json_obj, const TaskStatus& status) {
    switch (status) {
        case TaskStatus::eCancelled:
            json_obj = "cancelled";
            return;
        case TaskStatus::eCompleted:
            json_obj = "completed";
            return;
        case TaskStatus::eFailed:
            json_obj = "failed";
            return;
        case TaskStatus::eInputRequired:
            json_obj = "input_required";
            return;
        case TaskStatus::eWorking:
            json_obj = "working";
            return;
    }
    throw std::invalid_argument("Unknown task status");
}

void from_json(const nlohmann::json& json_obj, TaskStatus& status) {
    const auto& text = json_obj.get_ref<const std::string&>();
    if (text == "cancelled") {
        status = TaskStatus::eCancelled;
    } else if (text == "completed") {
        status = TaskStatus::eCompleted;
    } else if (text == "failed") {
        status = TaskStatus::eFailed;
    } else if (text == "input_required") {
        status = TaskStatus::eInputRequired;
    } else if (text == "working") {
        status = TaskStatus::eWorking;
    } else {
        throw std::invalid_argument("Unknown task status");
    }
}

void to_json(nlohmann::json& json_obj, const JSONRPCRequest& req) {
    json_obj = nlohmann::json::object();
    json_obj["id"] = req.id;
    json_obj["jsonrpc"] = req.jsonrpc;
    json_obj["method"] = req.method;
    if (req.params) {
        json_obj["params"] = *req.params;
    }
}

void from_json(const nlohmann::json& json_obj, JSONRPCRequest& req) {
    req = JSONRPCRequest{};
    req.id = json_obj.at("id").get<RequestId>();
    json_obj.at("jsonrpc").get_to(req.jsonrpc);
    detail::validate_jsonrpc_version(req.jsonrpc);
    json_obj.at("method").get_to(req.method);
    if (detail::has_json_value(json_obj, "params")) {
        req.params = json_obj.at("params");
    }
}

void to_json(nlohmann::json& json_obj, const JSONRPCNotification& notif) {
    json_obj = nlohmann::json{{"jsonrpc", notif.jsonrpc}, {"method", notif.method}};
    if (notif.params) {
        json_obj["params"] = *notif.params;
    }
}

void from_json(const nlohmann::json& json_obj, JSONRPCNotification& notif) {
    notif = JSONRPCNotification{};
    json_obj.at("jsonrpc").get_to(notif.jsonrpc);
    detail::validate_jsonrpc_version(notif.jsonrpc);
    json_obj.at("method").get_to(notif.method);
    if (detail::has_json_value(json_obj, "params")) {
        notif.params = json_obj.at("params");
    }
}

void to_json(nlohmann::json& json_obj, const JSONRPCResultResponse& resp) {
    json_obj = nlohmann::json::object();
    json_obj["id"] = resp.id;
    json_obj["jsonrpc"] = resp.jsonrpc;
    json_obj["result"] = resp.result;
}

void from_json(const nlohmann::json& json_obj, JSONRPCResultResponse& resp) {
    resp.id = json_obj.at("id").get<RequestId>();
    json_obj.at("jsonrpc").get_to(resp.jsonrpc);
    detail::validate_jsonrpc_version(resp.jsonrpc);
    json_obj.at("result").get_to(resp.result);
}

void to_json(nlohmann::json& json_obj, const JSONRPCErrorResponse& resp) {
    json_obj = nlohmann::json::object();
    json_obj["error"] = resp.error;
    json_obj["jsonrpc"] = resp.jsonrpc;
    if (resp.id) {
        json_obj["id"] = *resp.id;
    }
}

void from_json(const nlohmann::json& json_obj, JSONRPCErrorResponse& resp) {
    resp = JSONRPCErrorResponse{};
    json_obj.at("error").get_to(resp.error);
    json_obj.at("jsonrpc").get_to(resp.jsonrpc);
    detail::validate_jsonrpc_version(resp.jsonrpc);
    if (detail::has_json_value(json_obj, "id")) {
        resp.id = json_obj.at("id").get<RequestId>();
    } else {
        resp.id.reset();
    }
}

void to_json(nlohmann::json& json_obj, const JSONRPCResponse& resp) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, resp);
}

void from_json(const nlohmann::json& json_obj, JSONRPCResponse& resp) {
    // A null `error` alongside a real `result` is how some peers spell "no error"; selecting on
    // presence alone would route such a response into the error branch and fail to decode it.
    if (detail::has_json_value(json_obj, "error")) {
        resp = json_obj.get<JSONRPCErrorResponse>();
    } else {
        resp = json_obj.get<JSONRPCResultResponse>();
    }
}

void to_json(nlohmann::json& json_obj, const JSONRPCMessage& msg) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, msg);
}

void from_json(const nlohmann::json& json_obj, JSONRPCMessage& msg) {
    if (detail::has_json_value(json_obj, "error")) {
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
