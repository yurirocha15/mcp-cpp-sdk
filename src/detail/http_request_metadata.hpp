#pragma once

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

/// RFC 9110 field-value characters: visible ASCII, space and horizontal tab.
inline bool is_valid_header_value(std::string_view value) {
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte != '\t' && (byte < 0x20 || byte > 0x7E)) {
            return false;
        }
    }
    return true;
}

/// Decodes standard padded Base64, or returns nullopt when @p text is not that.
inline std::optional<std::string> decode_base64(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string decoded;
    decoded.reserve(text.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t padding = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char symbol = text[index];
        int value = -1;
        if (symbol >= 'A' && symbol <= 'Z') {
            value = symbol - 'A';
        } else if (symbol >= 'a' && symbol <= 'z') {
            value = symbol - 'a' + 26;
        } else if (symbol >= '0' && symbol <= '9') {
            value = symbol - '0' + 52;
        } else if (symbol == '+') {
            value = 62;
        } else if (symbol == '/') {
            value = 63;
        } else if (symbol == '=' && index + 2 >= text.size()) {
            ++padding;
            continue;
        } else {
            return std::nullopt;
        }
        if (padding != 0) {
            return std::nullopt;  // data after padding
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return decoded;
}

/// The value a client meant by an Mcp-Name or Mcp-Param header: the header text itself, or the
/// UTF-8 text of a `=?base64?...?=` sentinel. Nullopt when the value has characters a header may not
/// carry or its sentinel does not hold Base64.
inline std::optional<std::string> decode_header_value(std::string_view value) {
    if (!is_valid_header_value(value)) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    if (value.size() >= prefix.size() + suffix.size() && value.substr(0, prefix.size()) == prefix &&
        value.substr(value.size() - suffix.size()) == suffix) {
        return decode_base64(value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()));
    }
    return std::string(value);
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

/// The HTTP status of a serialized JSON-RPC response to a 2026-07-28 request.
inline boost::beast::http::status http_status_for_response(const std::string& response) {
    const auto parsed = parse_peer_json(response);
    if (!parsed.is_object()) {
        return boost::beast::http::status::ok;
    }
    const auto error = parsed.find("error");
    if (error == parsed.end() || !error->is_object()) {
        return boost::beast::http::status::ok;
    }
    const auto code = error->find("code");
    if (code == error->end() || !code->is_number_integer()) {
        return boost::beast::http::status::ok;
    }
    return http_status_for_error(code->get<int>());
}

}  // namespace mcp::detail
