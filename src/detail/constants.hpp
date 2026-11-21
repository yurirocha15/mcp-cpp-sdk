#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mcp::detail::constants {

/// Alphabets and bit arithmetic for hexadecimal, base64, base64url and percent encoding.
namespace encoding {

/// Lowercase hexadecimal digits.
inline constexpr std::string_view g_HEX_DIGITS = "0123456789abcdef";

/// Uppercase hexadecimal digits.
inline constexpr std::string_view g_HEX_DIGITS_UPPER = "0123456789ABCDEF";

/// Characters RFC 3986 leaves unreserved: they are written as they are in a percent-encoded value.
inline constexpr std::string_view g_UNRESERVED_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

/// Standard base64 alphabet, with `+` and `/`.
inline constexpr std::string_view g_BASE64_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Bit positions and masks of the base64 six-bit groups.
inline constexpr int g_SHIFT_4 = 4;

inline constexpr int g_SHIFT_6 = 6;

inline constexpr int g_SHIFT_8 = 8;

inline constexpr int g_SHIFT_12 = 12;

inline constexpr int g_SHIFT_16 = 16;

inline constexpr int g_SHIFT_18 = 18;

inline constexpr unsigned int g_MASK_0X3F = 0x3F;

inline constexpr unsigned int g_MASK_0X0F = 0x0F;

/// URL-safe base64 alphabet, with `-` and `_`.
inline constexpr std::string_view g_BASE64URL_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace encoding

/// Session identifier format.
namespace session {

/// Hexadecimal characters in a session identifier.
inline constexpr std::size_t g_LENGTH = 32;

}  // namespace session

/// HTTP client defaults and the limits of the client transport.
namespace http_client {

/// HTTP/1.1 as Beast numbers it.
inline constexpr int g_VERSION_11 = 11;

/// Timeout of a client request, in seconds.
inline constexpr int g_TIMEOUT_SECONDS = 30;

/// First status code of an error response (400 Bad Request).
inline constexpr int g_STATUS_BAD_REQUEST = 400;

/// The most a single response body or SSE line may hold before the exchange is abandoned.
inline constexpr std::size_t g_MAX_MODERN_MESSAGE_BYTES = 16U * 1024U * 1024U;

/// How many times one request's event stream is resumed after it ends without the response.
inline constexpr int g_MAX_STREAM_RESUMES = 3;

/// The pause before a stream is opened again when the server named none.
inline constexpr std::chrono::milliseconds g_DEFAULT_RECONNECT_DELAY{1000};

/// The shortest pause before a stream is opened again, whatever the server asked for.
inline constexpr std::chrono::milliseconds g_MIN_RECONNECT_DELAY{100};

/// The longest pause before a stream is opened again, whatever the server asked for.
inline constexpr std::chrono::milliseconds g_MAX_RECONNECT_DELAY{30000};

/// Longest `Mcp-Param-*` value sent. Longer ones do not get through the servers that limit a header.
inline constexpr std::size_t g_MAX_PARAM_HEADER_BYTES = 4096;

/// The most pages of tools/list one schema refresh follows.
inline constexpr int g_MAX_REFRESH_PAGES = 64;

}  // namespace http_client

/// URL prefixes the transports and the OAuth client compare against.
namespace url {

/// Scheme prefix of an `http` URL.
inline constexpr std::string_view g_HTTP_PREFIX = "http://";

/// Scheme prefix of an `https` URL.
inline constexpr std::string_view g_HTTPS_PREFIX = "https://";

inline constexpr std::string_view g_WELL_KNOWN_PREFIX = "/.well-known/oauth-protected-resource";

}  // namespace url

/// Read-ahead sizes of the per-request exchange watcher.
namespace modern_exchange {

/// Bytes a watch reads ahead while a request runs; they belong to the client's next request.
inline constexpr std::size_t g_WATCH_READ_SIZE = 4096;

/// Read-ahead held at once; past this the watch stops until the request is answered.
inline constexpr std::size_t g_MAX_READ_AHEAD = 64 * 1024;

}  // namespace modern_exchange

/// Descriptors, buffer sizes and limits of the stdio transport.
namespace stdio {

inline constexpr int g_STDIN_DESCRIPTOR = 0;

inline constexpr int g_STDOUT_DESCRIPTOR = 1;

inline constexpr int g_STDERR_DESCRIPTOR = 2;

// The protocol's copy of stdout must not take one of the standard descriptors.
// In a process started with stderr closed, 2 is the lowest free descriptor, and
// pointing stdout at stderr would then point it back at the protocol stream.
inline constexpr int g_FIRST_PRIVATE_DESCRIPTOR = 3;

/// The WebSocket transports' default limit, so a message fits every transport or none.
inline constexpr std::size_t g_DEFAULT_MAX_MESSAGE_BYTES = 16U * 1024U * 1024U;

inline constexpr std::size_t g_READ_CHUNK_BYTES = 64U * 1024U;

/// Input is read ahead of read_message() up to this many bytes.
inline constexpr std::size_t g_MAX_READ_AHEAD_BYTES = 16U * 1024U * 1024U;

/// What an empty line costs in the queue, so short lines cannot fill memory unnoticed.
inline constexpr std::size_t g_INPUT_LINE_OVERHEAD = 64;

/// How long the destructor waits for accepted writes to reach a peer that is reading.
inline constexpr std::chrono::seconds g_FINAL_FLUSH_WAIT{1};

}  // namespace stdio

/// Pipe buffer size and shutdown capabilities of the stdio client process handling.
namespace stdio_client {

inline constexpr std::size_t g_CHUNK_BYTES = 64U * 1024U;

/// Whether the process can be asked to exit before it is killed: SIGTERM on POSIX, nothing on Windows.
inline constexpr bool g_HAS_TERMINATE_STEP =
#if defined(_WIN32)
    false;
#else
    true;
#endif

}  // namespace stdio_client

/// Limits and sentinels of the WebSocket transports.
namespace websocket {

inline constexpr auto g_NEVER = std::chrono::steady_clock::time_point::max();

/// What Beast defaults to, and what the stdio transport uses.
inline constexpr std::size_t g_DEFAULT_MAX_MESSAGE_BYTES = 16U * 1024U * 1024U;

}  // namespace websocket

/// Back-off of the accept loops.
namespace accept_retry {

/// How long an accept loop waits after a failure that can pass.
inline constexpr auto g_DELAY = std::chrono::milliseconds(100);

}  // namespace accept_retry

/// Multi round-trip request state bounds and polling interval.
namespace mrtr {

/// How often a retry waiting for its parked handler looks for its own cancellation, which it
/// passes on to the handler.
inline constexpr std::chrono::milliseconds g_CANCEL_POLL_INTERVAL{50};

/// The shortest `requestState` limit (MrtrOptions::max_state_bytes) that set_mrtr_options() accepts.
/// A state that carries one question, a principal digest and a parameters digest seals to fewer
/// characters than this, so a smaller limit would fail every question.
inline constexpr std::size_t g_MIN_STATE_CHARS = 512;

}  // namespace mrtr

/// Resource bounds of the server.
namespace server {

// Matching a URI against every registered template costs time in proportion to the URI's length
// times the templates' size, so the length is bounded to keep one resources/read cheap whatever a
// client sends. Real resource identifiers sit far below this.
inline constexpr std::size_t g_MAX_TEMPLATE_MATCH_URI_LENGTH = 512;

/// The default of Server::set_max_in_flight_bytes().
inline constexpr std::size_t g_DEFAULT_MAX_IN_FLIGHT_BYTES = 256U * 1024U * 1024U;

/// Resources one session may subscribe to, and the bytes of their URIs together.
inline constexpr std::size_t g_MAX_SUBSCRIPTIONS = 4096;

inline constexpr std::size_t g_MAX_SUBSCRIPTION_BYTES = 8U * 1024U * 1024U;

}  // namespace server

/// Resource bounds of the client.
namespace client {

/// Requests of the server a client answers at once before it stops reading.
inline constexpr std::size_t g_MAX_INCOMING_REQUESTS = 1024;

}  // namespace client

/// Waits of the task runner.
namespace task_run {

inline constexpr std::chrono::seconds g_WAIT_LIMIT{5};

}  // namespace task_run

/// Lookup tables of the JSON serializer.
namespace json_dump {

/// For each byte below 0x80, the letter of its two-character escape, `u` for one written as
/// `\u00XX`, or 0 for a byte written as it is.
inline constexpr std::array<char, 0x80> g_ESCAPES = [] {
    std::array<char, 0x80> escapes{};
    for (std::size_t byte = 0; byte < 0x20; ++byte) {
        escapes[byte] = 'u';
    }
    escapes['\b'] = 'b';
    escapes['\t'] = 't';
    escapes['\n'] = 'n';
    escapes['\f'] = 'f';
    escapes['\r'] = 'r';
    escapes['"'] = '"';
    escapes['\\'] = '\\';
    return escapes;
}();

}  // namespace json_dump

/// Unicode code point bounds of the regular expression engine.
namespace regex {

inline constexpr char32_t g_MAX_CODE_POINT = 0x10FFFF;

inline constexpr char32_t g_REPLACEMENT_CHARACTER = 0xFFFD;

}  // namespace regex

/// Word-at-a-time scanning masks.
namespace json_text {

inline constexpr std::uint64_t g_ONES = 0x0101010101010101ULL;

inline constexpr std::uint64_t g_HIGH_BITS = 0x8080808080808080ULL;

/// The bytes needs_attention() looks at in one step.
inline constexpr std::size_t g_BLOCK_BYTES = sizeof(std::uint64_t);

}  // namespace json_text

/// Method names handled on the 2026-07-28 revision.
namespace modern_methods {

/// The client requests a server answers for a request of protocol revision 2026-07-28. Methods of
/// earlier revisions that 2026-07-28 removed (ping, logging/setLevel, resources/subscribe,
/// resources/unsubscribe) are not among them, and neither is initialize, which always starts a
/// session of an earlier revision.
inline constexpr std::array<std::string_view, 10> g_NAMES = {
    "server/discover",          "tools/list",          "tools/call",  "resources/list",
    "resources/read",           "prompts/list",        "prompts/get", "completion/complete",
    "resources/templates/list", "subscriptions/listen"};

}  // namespace modern_methods

/// Schema keywords and bounds used when mirroring parameters into HTTP headers.
namespace param_headers {

/// Keywords whose value is one subschema.
inline constexpr std::array<std::string_view, 12> g_SCHEMA_KEYWORDS = {"additionalProperties",
                                                                       "unevaluatedProperties",
                                                                       "propertyNames",
                                                                       "items",
                                                                       "additionalItems",
                                                                       "unevaluatedItems",
                                                                       "contains",
                                                                       "not",
                                                                       "if",
                                                                       "then",
                                                                       "else",
                                                                       "contentSchema"};

/// Keywords whose value is an array of subschemas.
inline constexpr std::array<std::string_view, 5> g_SCHEMA_ARRAY_KEYWORDS = {"allOf", "anyOf", "oneOf",
                                                                            "prefixItems", "items"};

/// Keywords whose value maps names to subschemas, other than `properties`.
inline constexpr std::array<std::string_view, 5> g_SCHEMA_MAP_KEYWORDS = {
    "patternProperties", "dependentSchemas", "$defs", "definitions", "dependencies"};

/// The largest integer an `x-mcp-header` argument may hold: 2^53 - 1.
inline constexpr std::int64_t g_MAX_SAFE_HEADER_INTEGER = (std::int64_t{1} << 53) - 1;

}  // namespace param_headers

/// Depth bound for JSON received from a peer.
namespace peer_json {

/// Deepest nesting of arrays and objects accepted in a document received from a peer.
///
/// nlohmann::json parses iteratively, but copying a parsed value recurses once per level, and the
/// SDK copies request params and results. Without a bound, a message of a few hundred kilobytes
/// made of nothing but brackets exhausts the stack of whichever thread copies it. Real MCP payloads
/// stay far below this.
inline constexpr std::size_t g_MAX_DEPTH = 128;

}  // namespace peer_json

/// Layout and key derivation constants of sealed state tokens.
namespace sealed_token {

inline constexpr std::string_view g_HKDF_INFO = "mcp-cpp-sdk requestState v1";

inline constexpr std::size_t g_NONCE_SIZE = 12;

inline constexpr std::size_t g_KEY_SIZE = 32;

inline constexpr std::size_t g_KID_SIZE = 4;

inline constexpr std::size_t g_SALT_SIZE = 16;

inline constexpr std::size_t g_TAG_SIZE = 16;

inline constexpr std::uint8_t g_VERSION = 1;

inline constexpr std::size_t g_HEADER_SIZE = 1 + g_KID_SIZE + g_SALT_SIZE;

/// The shortest valid token: header, no ciphertext, tag.
inline constexpr std::size_t g_MIN_TOKEN_BYTES = g_HEADER_SIZE + g_TAG_SIZE;

inline constexpr std::size_t g_DEFAULT_MAX_TOKEN_CHARS = 64 * 1024;

}  // namespace sealed_token

/// JSON Schema dialect handled by the native validator.
namespace native_schema {

inline constexpr std::string_view g_DIALECT = "https://json-schema.org/draft/2020-12/schema";

}  // namespace native_schema

/// Work and size bounds of schema compilation and validation.
namespace schema {

/// Deepest nesting accepted in a schema document. Checking a schema against its metaschema and
/// compiling it both recurse with the schema, so this bounds the stack they use.
inline constexpr std::size_t g_MAX_DEPTH = 64;

/// Evaluation work every validation may do. The vendored jsoncons charges a step one more than the
/// length of its evaluation path, which is what the step costs, and charges copies of locations
/// and whole-string keywords by their length (see scripts/vendor_jsoncons.py).
inline constexpr std::size_t g_BASE_WORK = 1'000'000;

/// Work allowed per unit of the value (see ValueShape::units), before the allowance for each node
/// of the schema below.
inline constexpr std::size_t g_WORK_PER_UNIT = 256;

/// Further work allowed per unit of the value for each node of the schema: a step that compares
/// the value with a schema value, such as an `enum` member, costs 8.
inline constexpr std::size_t g_WORK_PER_UNIT_AND_SCHEMA_NODE = 8;

/// Deepest nesting of schema evaluations in one native validation: every keyword that evaluates a
/// subschema, in place or on a member or item, is one level. The same number as the evaluation
/// path bound of the vendored jsoncons.
inline constexpr std::size_t g_MAX_EVAL_DEPTH = 192;

/// Most documents CompiledSchema::compile() accepts besides the schema.
inline constexpr std::size_t g_MAX_SCHEMA_DOCUMENTS = 64;

/// Branch violations the native evaluation keeps while an `anyOf` or `oneOf` branch is evaluated, to
/// report the first branch's when no branch matches.
inline constexpr std::size_t g_MAX_BRANCH_VIOLATIONS = 4;

/// Memory the messages kept for failed branches may hold at once during one validation.
inline constexpr std::size_t g_MAX_KEPT_MESSAGE_BYTES = 32 * 1024 * 1024;

/// Base URI jsoncons gives a schema without `$id`; references are reported relative to it.
inline constexpr std::string_view g_DEFAULT_BASE_URI = "https://jsoncons.com";

}  // namespace schema

/// Paging and validation limits of the skills listing.
namespace skills {

inline constexpr std::string_view g_CURSOR_PREFIX = "o";

inline constexpr std::size_t g_MAX_NAME_LENGTH = 64;

inline constexpr std::size_t g_MAX_DESCRIPTION_LENGTH = 1024;

inline constexpr std::size_t g_DIGEST_HEX_LENGTH = 64;

inline constexpr std::string_view g_DIGEST_PREFIX = "sha256:";

}  // namespace skills

/// Character classes used when formatting authentication challenges.
namespace challenge {

inline constexpr std::string_view g_TOKEN_SPECIALS = "!#$%&'*+-.^_`|~";

inline constexpr int g_HEX_BASE = 16;

}  // namespace challenge

/// Client assertion and key size constants.
namespace client_authentication {

inline constexpr std::string_view g_ASSERTION_TYPE =
    "urn:ietf:params:oauth:client-assertion-type:jwt-bearer";

inline constexpr std::size_t g_JTI_LENGTH = 32;

inline constexpr std::size_t g_ES256_COORDINATE_BYTES = 32;

inline constexpr int g_MIN_RSA_BITS = 2048;

}  // namespace client_authentication

/// Scope names used by client identity registration.
namespace client_identity {

inline constexpr std::string_view g_OFFLINE_ACCESS = "offline_access";

}  // namespace client_identity

/// Octet extraction for IPv4 address checks.
namespace ipv4 {

inline constexpr std::uint32_t g_OCTET_MASK = 0xFFU;

inline constexpr int g_OCTET1_SHIFT = 24;

inline constexpr int g_OCTET2_SHIFT = 16;

inline constexpr int g_OCTET3_SHIFT = 8;

}  // namespace ipv4

}  // namespace mcp::detail::constants
