#pragma once

#include <cstddef>
#include <string_view>

namespace mcp::constants {

/**
 * @brief The default capacity of the event store.
 */
inline constexpr std::size_t g_event_store_default_capacity = 1024;

/**
 * @brief Hexadecimal digits used for base64 encoding.
 */
inline constexpr std::string_view g_hex_digits = "0123456789abcdef";
inline constexpr std::string_view g_hex_digits_upper = "0123456789ABCDEF";

/**
 * @brief Unreserved characters for base64url encoding.
 */
inline constexpr std::string_view g_unreserved_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

/**
 * @brief Alphabet used for base64 encoding.
 */
inline constexpr std::string_view g_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief HTTP prefix for OAuth client URLs.
 */
inline constexpr std::string_view g_http_prefix = "http://";

/**
 * @brief Default HTTP version (HTTP/1.1 = 11).
 */
inline constexpr int g_http_version_11 = 11;

/**
 * @brief Default HTTP timeout in seconds.
 */
inline constexpr int g_http_timeout_seconds = 30;

/**
 * @brief HTTP 400 Bad Request status code.
 */
inline constexpr int g_http_bad_request = 400;

/**
 * @brief Length of a session identifier.
 */
inline constexpr std::size_t g_session_id_length = 32;

}  // namespace mcp::constants
