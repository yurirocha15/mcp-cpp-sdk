#pragma once

#include "constants.hpp"

#include <mcp/core/export.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

/// A SHA-256 digest.
using Sha256Digest = std::array<unsigned char, constants::encoding::g_SHA256_DIGEST_LENGTH>;

/// SHA-256 of @p bytes. Throws std::runtime_error when OpenSSL fails.
MCP_API Sha256Digest sha256(std::string_view bytes);

/// The bytes of @p digest as a string view.
inline std::string_view digest_bytes(const Sha256Digest& digest) {
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

/// @p bytes as lowercase hexadecimal, two digits per byte.
MCP_API std::string hex_encode(std::string_view bytes);

/// @p bytes as standard base64 (RFC 4648 section 4) with `=` padding.
MCP_API std::string base64_encode(std::string_view bytes);

/// Decodes standard base64 with `=` padding and no line breaks. Nothing when @p text holds a
/// character outside the alphabet, misplaced padding or a length that is not a multiple of four.
MCP_API std::optional<std::string> base64_decode(std::string_view text);

/// @p bytes as unpadded base64url (RFC 4648 section 5).
MCP_API std::string base64url_encode(std::string_view bytes);

/// Decodes canonical unpadded base64url. Nothing for padding, another character, a length that is
/// 1 mod 4 or non-zero trailing bits, so one byte string has exactly one accepted spelling.
MCP_API std::optional<std::string> base64url_decode(std::string_view text);

/// @p text with every byte outside RFC 3986's unreserved set written as `%XX`, in uppercase.
MCP_API std::string percent_encode(std::string_view text);

/// @p text with every `%XX` escape decoded; nothing when an escape is malformed.
MCP_API std::optional<std::string> percent_decode(std::string_view text);

/// @p fields as an `application/x-www-form-urlencoded` body: `name=value` pairs joined by `&`, each
/// side written with percent_encode().
MCP_API std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields);

/// @p text as one `application/x-www-form-urlencoded` value, which RFC 6749 section 2.3.1 applies to
/// a client identifier and secret sent with HTTP Basic: a space becomes `+`, and every other byte
/// percent_encode() would escape is escaped.
MCP_API std::string form_value(std::string_view text);

}  // namespace mcp::detail
