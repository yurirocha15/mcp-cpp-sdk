#pragma once

#include "header_value.hpp"
#include "param_headers.hpp"
#include "peer_json.hpp"
#include "request_meta.hpp"

#include <mcp/protocol/base.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/transport/http_types.hpp>

#include <boost/beast/http/status.hpp>

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

/// The value of header @p name, or nullopt when the request does not carry it. Header names are
/// matched case-insensitively.
inline std::optional<std::string_view> http_header(const StringRequest& request,
                                                   std::string_view name) {
    const auto header = request.find(boost::beast::string_view(name.data(), name.size()));
    if (header == request.end()) {
        return std::nullopt;
    }
    return std::string_view(header->value().data(), header->value().size());
}

/// Whether an HTTP POST body is a request of protocol revision 2026-07-28: its method is not
/// initialize and either its `_meta` names a protocol version, its MCP-Protocol-Version header names
/// one that is not negotiated by initialize, or it is server/discover, which no earlier revision
/// has. initialize stays a legacy request whatever it carries.
inline bool is_modern_http_request(const nlohmann::json& body,
                                   std::optional<std::string_view> protocol_header) {
    const auto method = body.find("method");
    if (method == body.end() || !method->is_string()) {
        return false;
    }
    const auto& name = method->get_ref<const std::string&>();
    if (name == "initialize") {
        return false;
    }
    if (name == "server/discover" || is_modern_request(body, name)) {
        return true;
    }
    return protocol_header.has_value() && !is_legacy_protocol_version(*protocol_header);
}

/// The body value an Mcp-Name header mirrors, or null for a method that has none.
inline const nlohmann::json* mirrored_name(const nlohmann::json& body, std::string_view method) {
    const char* member = nullptr;
    if (method == "tools/call" || method == "prompts/get") {
        member = "name";
    } else if (method == "resources/read") {
        member = "uri";
    } else {
        return nullptr;
    }
    const auto params = body.find("params");
    if (params == body.end() || !params->is_object()) {
        return nullptr;
    }
    const auto value = params->find(member);
    return value == params->end() ? nullptr : &*value;
}

/// Checks the headers a 2026-07-28 request mirrors from its body (MCP-Protocol-Version, Mcp-Method
/// and, for tools/call, prompts/get and resources/read, Mcp-Name) and returns why they do not match,
/// or nullopt when they do. A body value that is missing or malformed is left to the server, which
/// answers it as invalid params.
inline std::optional<std::string> modern_header_mismatch(const StringRequest& request,
                                                         const nlohmann::json& body) {
    const auto& method = body.at("method").get_ref<const std::string&>();

    // Two copies of a mirrored header let an intermediary act on one and the server on the other.
    for (const std::string_view name : {"MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"}) {
        if (request.count(boost::beast::string_view(name.data(), name.size())) > 1) {
            return "Duplicate " + std::string(name) + " header";
        }
    }

    const auto protocol_header = http_header(request, "MCP-Protocol-Version");
    if (!protocol_header) {
        return "Missing MCP-Protocol-Version header";
    }
    if (!is_valid_header_value(*protocol_header)) {
        return "Invalid characters in MCP-Protocol-Version header";
    }
    if (const auto* version = requested_protocol_version(body);
        version != nullptr && *version != *protocol_header) {
        return "MCP-Protocol-Version header does not match " + std::string(g_META_PROTOCOL_VERSION) +
               " in _meta";
    }

    const auto method_header = http_header(request, "Mcp-Method");
    if (!method_header) {
        return "Missing Mcp-Method header";
    }
    if (*method_header != method) {
        return "Mcp-Method header does not match the request method";
    }

    const auto* name = mirrored_name(body, method);
    if (name == nullptr || !name->is_string()) {
        return std::nullopt;
    }
    const auto name_header = http_header(request, "Mcp-Name");
    if (!name_header) {
        return "Missing Mcp-Name header";
    }
    const auto decoded = decode_header_value(*name_header);
    if (!decoded) {
        return "Invalid Mcp-Name header value";
    }
    if (*decoded != name->get_ref<const std::string&>()) {
        return "Mcp-Name header does not match the request body";
    }
    return std::nullopt;
}

/// The HTTP status a JSON-RPC error answers a 2026-07-28 request with: 400 for invalid params and
/// the errors the specification reserves (header mismatch, missing client capability, unsupported
/// version), 404 for an unknown method, and 200 with the JSON-RPC body for anything else.
inline boost::beast::http::status http_status_for_error(int code) {
    switch (code) {
        case g_INVALID_PARAMS:
        case g_HEADER_MISMATCH:
        case g_MISSING_REQUIRED_CLIENT_CAPABILITY:
        case g_UNSUPPORTED_PROTOCOL_VERSION:
            return boost::beast::http::status::bad_request;
        case g_METHOD_NOT_FOUND:
            return boost::beast::http::status::not_found;
        default:
            return boost::beast::http::status::ok;
    }
}

/// The error code of a serialized JSON-RPC response, or nullopt for a result.
inline std::optional<int> response_error_code(const std::string& response) {
    const auto parsed = parse_peer_json(response);
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    const auto error = parsed.find("error");
    if (error == parsed.end() || !error->is_object()) {
        return std::nullopt;
    }
    const auto code = error->find("code");
    if (code == error->end() || !code->is_number_integer()) {
        return std::nullopt;
    }
    return code->get<int>();
}

/// The `Mcp-Param-{Name}` headers of @p request as {Name, value} pairs, in the order received.
/// The prefix is matched ignoring case, and every instance of a repeated header is kept.
inline std::vector<std::pair<std::string, std::string>> mcp_param_headers(
    const StringRequest& request) {
    constexpr std::string_view prefix = "mcp-param-";
    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto& field : request) {
        const std::string_view name(field.name_string().data(), field.name_string().size());
        if (name.size() > prefix.size() &&
            equals_ignoring_case(name.substr(0, prefix.size()), prefix)) {
            headers.emplace_back(std::string(name.substr(prefix.size())),
                                 std::string(field.value().data(), field.value().size()));
        }
    }
    return headers;
}

}  // namespace mcp::detail
