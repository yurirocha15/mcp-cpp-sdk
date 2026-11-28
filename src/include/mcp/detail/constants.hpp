#pragma once

#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mcp/core/constants.hpp>
#include <string_view>
#include <utility>

namespace mcp::detail::constants {

/// Deprecation notices (DeprecationReporter).
namespace deprecation {

/// Set to a non-empty value, it stops the notices that have no handler from going to standard error.
inline constexpr std::string_view g_SILENCE_ENV = "MCP_SILENCE_DEPRECATION_NOTICES";
/// The earliest removal of the features protocol revision 2026-07-28 deprecates.
inline constexpr std::string_view g_REVISION_ON_OR_AFTER_2027_07_28 =
    "first revision released on or after 2027-07-28";

}  // namespace deprecation

/// URL-mode elicitations and their completion notifications.
namespace elicitation {

/// Most URL-mode elicitations the sessions of one server keep a record of, to route their
/// completion notifications.
inline constexpr std::size_t g_MAX_RECORDS = 4096;
/// Most of those records one session keeps; its oldest goes first.
inline constexpr std::size_t g_MAX_RECORDS_PER_SESSION = 256;
/// How long a record lasts: an out-of-band flow, such as a sign-in, can take hours.
inline constexpr std::chrono::hours g_RECORD_TTL{24};
/// Most URL-mode elicitation ids a client remembers, to tell the completions it expects.
inline constexpr std::size_t g_MAX_CLIENT_IDS = 1024;

}  // namespace elicitation

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

/// URL-safe base64 alphabet, with `-` and `_`.
inline constexpr std::string_view g_BASE64URL_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// Size of a SHA-256 digest, in bytes.
inline constexpr std::size_t g_SHA256_DIGEST_LENGTH = 32;

/// The bits of one byte.
inline constexpr unsigned g_BITS_PER_BYTE = CHAR_BIT;

/// The number of distinct byte values.
inline constexpr std::size_t g_BYTE_VALUES = 256;

/// The bits of one byte.
inline constexpr std::uint32_t g_BYTE_MASK = 0xFF;

/// The bits of one hexadecimal digit.
inline constexpr std::uint32_t g_HEX_DIGIT_MASK = 0x0F;

/// The bits one hexadecimal digit carries.
inline constexpr unsigned g_HEX_DIGIT_BITS = 4;

/// The base of decimal numerals.
inline constexpr unsigned g_DECIMAL_BASE = 10;

/// The bits one base64 digit carries, and their mask.
inline constexpr unsigned g_BASE64_DIGIT_BITS = 6;
inline constexpr std::uint32_t g_BASE64_DIGIT_MASK = 0x3F;

/// The value of each byte as a digit of @p digits, or -1 for a byte that is not one.
/// @p same_values, when given, spells the same values with other characters, as uppercase
/// hexadecimal does. The entries are std::int16_t, not signed char, so that they read as numbers.
constexpr std::array<std::int16_t, g_BYTE_VALUES> digit_values(std::string_view digits,
                                                               std::string_view same_values = {}) {
    std::array<std::int16_t, g_BYTE_VALUES> values{};
    values.fill(-1);
    for (const auto spelling : {digits, same_values}) {
        for (std::size_t value = 0; value < spelling.size(); ++value) {
            values[static_cast<unsigned char>(spelling[value])] = static_cast<std::int16_t>(value);
        }
    }
    return values;
}

/// The value of each byte as a hexadecimal digit of either case, or -1.
inline constexpr std::array<std::int16_t, g_BYTE_VALUES> g_HEX_VALUES =
    digit_values(g_HEX_DIGITS, g_HEX_DIGITS_UPPER);

/// The value of each byte as a standard base64 digit, or -1.
inline constexpr std::array<std::int16_t, g_BYTE_VALUES> g_BASE64_VALUES =
    digit_values(g_BASE64_ALPHABET);

/// The value of each byte as a base64url digit, or -1.
inline constexpr std::array<std::int16_t, g_BYTE_VALUES> g_BASE64URL_VALUES =
    digit_values(g_BASE64URL_ALPHABET);

}  // namespace encoding

/// Unicode code points, and how UTF-8 and UTF-16 write them.
namespace unicode {

/// The end of the C0 control characters, U+0000 to U+001F, which JSON strings escape.
inline constexpr std::uint32_t g_C0_CONTROLS_END = 0x20;

/// U+007F DELETE, a control character that follows the last visible ASCII character.
inline constexpr std::uint32_t g_DELETE = 0x7F;

/// The C1 control characters, U+0080 to U+009F.
inline constexpr std::uint32_t g_C1_CONTROLS_FIRST = 0x80;
inline constexpr std::uint32_t g_C1_CONTROLS_LAST = 0x9F;

/// The bidirectional embeddings and overrides, U+202A to U+202E.
inline constexpr std::uint32_t g_BIDI_EMBEDDINGS_FIRST = 0x202A;
inline constexpr std::uint32_t g_BIDI_EMBEDDINGS_LAST = 0x202E;

/// The bidirectional isolates, U+2066 to U+2069.
inline constexpr std::uint32_t g_BIDI_ISOLATES_FIRST = 0x2066;
inline constexpr std::uint32_t g_BIDI_ISOLATES_LAST = 0x2069;

/// The implicit bidirectional marks: LEFT-TO-RIGHT MARK, RIGHT-TO-LEFT MARK and ARABIC LETTER
/// MARK.
inline constexpr std::uint32_t g_LEFT_TO_RIGHT_MARK = 0x200E;
inline constexpr std::uint32_t g_RIGHT_TO_LEFT_MARK = 0x200F;
inline constexpr std::uint32_t g_ARABIC_LETTER_MARK = 0x061C;

/// The first code point past ASCII. UTF-8 writes code points from here on in two or more bytes.
inline constexpr std::uint32_t g_ASCII_END = 0x80;

/// The bits of an ASCII byte.
inline constexpr std::uint32_t g_ASCII_MASK = 0x7F;

/// The first code point UTF-8 writes in three bytes.
inline constexpr std::uint32_t g_THREE_BYTE_FIRST = 0x800;

/// The first code point past the Basic Multilingual Plane. UTF-8 writes code points from here on
/// in four bytes, and UTF-16 as a surrogate pair.
inline constexpr std::uint32_t g_SUPPLEMENTARY_FIRST = 0x10000;

/// The last code point.
inline constexpr std::uint32_t g_MAX_CODE_POINT = 0x10FFFF;

/// U+FFFD, which stands in for text that cannot be decoded.
inline constexpr std::uint32_t g_REPLACEMENT_CHARACTER = 0xFFFD;

/// U+2028 LINE SEPARATOR and U+2029 PARAGRAPH SEPARATOR, which JavaScript treats as line
/// terminators.
inline constexpr std::uint32_t g_LINE_SEPARATOR = 0x2028;
inline constexpr std::uint32_t g_PARAGRAPH_SEPARATOR = 0x2029;

/// UTF-16 high surrogates, U+D800 to U+DBFF.
inline constexpr std::uint32_t g_HIGH_SURROGATE_FIRST = 0xD800;
inline constexpr std::uint32_t g_HIGH_SURROGATE_LAST = 0xDBFF;

/// UTF-16 low surrogates, U+DC00 to U+DFFF.
inline constexpr std::uint32_t g_LOW_SURROGATE_FIRST = 0xDC00;
inline constexpr std::uint32_t g_LOW_SURROGATE_LAST = 0xDFFF;

/// The bits of a supplementary code point, less g_SUPPLEMENTARY_FIRST, that each surrogate of a
/// pair carries.
inline constexpr unsigned g_SURROGATE_BITS = 10;

/// A UTF-8 continuation byte is 10xxxxxx: this tag under g_UTF8_TAG_MASK, then six payload bits.
inline constexpr std::uint32_t g_UTF8_CONTINUATION_TAG = 0x80;
inline constexpr std::uint32_t g_UTF8_TAG_MASK = 0xC0;
inline constexpr unsigned g_UTF8_PAYLOAD_BITS = 6;
inline constexpr std::uint32_t g_UTF8_PAYLOAD_MASK = 0x3F;

/// The tag of the lead byte of a UTF-8 sequence, by sequence length: 110xxxxx, 1110xxxx and
/// 11110xxx. The lead of a sequence of n bytes carries g_ASCII_MASK >> n payload bits.
inline constexpr std::array g_UTF8_LEAD_TAGS{0x00U, 0x00U, 0xC0U, 0xE0U, 0xF0U};

/// The longest UTF-8 sequence, in bytes.
inline constexpr std::size_t g_UTF8_MAX_LENGTH = 4;

/// What a lead byte starts in well-formed UTF-8: the length of the sequence, 0 for a byte that
/// starts none, and the range of the byte that follows it. Every later byte of the sequence is a
/// continuation byte.
struct Utf8Lead {
    std::uint8_t length;
    std::uint8_t second_first;
    std::uint8_t second_last;
};

/// Unicode's table 3-7, well-formed UTF-8 byte sequences, by lead byte. The narrower second-byte
/// ranges after E0, ED, F0 and F4 leave out overlong forms, surrogates and code points past
/// U+10FFFF.
inline constexpr std::array<Utf8Lead, encoding::g_BYTE_VALUES> g_UTF8_LEADS = [] {
    struct Row {
        std::uint8_t first_lead;
        std::uint8_t last_lead;
        Utf8Lead lead;
    };
    constexpr std::array rows{
        Row{
            .first_lead = 0xC2,
            .last_lead = 0xDF,
            .lead = {.length = 2, .second_first = 0x80, .second_last = 0xBF},
        },
        Row{
            .first_lead = 0xE0,
            .last_lead = 0xE0,
            .lead = {.length = 3, .second_first = 0xA0, .second_last = 0xBF},
        },
        Row{
            .first_lead = 0xE1,
            .last_lead = 0xEC,
            .lead = {.length = 3, .second_first = 0x80, .second_last = 0xBF},
        },
        Row{
            .first_lead = 0xED,
            .last_lead = 0xED,
            .lead = {.length = 3, .second_first = 0x80, .second_last = 0x9F},
        },
        Row{
            .first_lead = 0xEE,
            .last_lead = 0xEF,
            .lead = {.length = 3, .second_first = 0x80, .second_last = 0xBF},
        },
        Row{
            .first_lead = 0xF0,
            .last_lead = 0xF0,
            .lead = {.length = 4, .second_first = 0x90, .second_last = 0xBF},
        },
        Row{
            .first_lead = 0xF1,
            .last_lead = 0xF3,
            .lead = {.length = 4, .second_first = 0x80, .second_last = 0xBF},
        },
        Row{
            .first_lead = 0xF4,
            .last_lead = 0xF4,
            .lead = {.length = 4, .second_first = 0x80, .second_last = 0x8F},
        },
    };
    std::array<Utf8Lead, encoding::g_BYTE_VALUES> leads{};
    for (const auto& row : rows) {
        for (unsigned byte = row.first_lead; byte <= row.last_lead; ++byte) {
            leads[byte] = row.lead;
        }
    }
    return leads;
}();

}  // namespace unicode

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

/// The default of HttpClientTransport::set_response_timeout().
inline constexpr std::chrono::milliseconds g_DEFAULT_RESPONSE_TIMEOUT = std::chrono::minutes(10);

/// The buffer a response body is read into, a part at a time.
inline constexpr std::size_t g_READ_CHUNK_BYTES = 8192;

/// First status code of an error response (400 Bad Request).
inline constexpr int g_STATUS_BAD_REQUEST = 400;

/// 404 Not Found.
inline constexpr unsigned int g_STATUS_NOT_FOUND = 404;

/// 410 Gone.
inline constexpr unsigned int g_STATUS_GONE = 410;

/// The most a single response body or SSE line may hold before the exchange is abandoned.
inline constexpr std::size_t g_MAX_MODERN_MESSAGE_BYTES = std::size_t{16} * 1024 * 1024;

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

/// The most connections a transport keeps open between requests for the next ones.
inline constexpr std::size_t g_MAX_IDLE_CONNECTIONS = 4;

/// How long a kept connection waits for its next request before it is closed.
inline constexpr std::chrono::seconds g_IDLE_CONNECTION_TIMEOUT{90};

}  // namespace http_client

/// The HTTP+SSE transport of protocol revision 2024-11-05.
namespace sse_legacy {

/// What the client transport's errors name it.
inline constexpr std::string_view g_TRANSPORT_NAME = "LegacySseClientTransport";

/// The event that names the URL messages are POSTed to.
inline constexpr std::string_view g_EVENT_ENDPOINT = "endpoint";

/// The event that carries a JSON-RPC message.
inline constexpr std::string_view g_EVENT_MESSAGE = "message";

/// The content type of an event stream.
inline constexpr std::string_view g_CONTENT_TYPE_EVENT_STREAM = "text/event-stream";

/// The query parameter of the message URL that names the session.
inline constexpr std::string_view g_SESSION_ID_PARAMETER = "sessionId";

/// The last character code of the space and control characters an endpoint may not hold.
inline constexpr unsigned char g_LAST_SPACE_OR_CONTROL = 0x20;

/// The DEL character, which an endpoint may not hold either.
inline constexpr unsigned char g_DELETE_CHARACTER = 0x7f;

}  // namespace sse_legacy

/// URL prefixes the transports and the OAuth client compare against.
namespace url {

/// Scheme prefix of an `http` URL.
inline constexpr std::string_view g_HTTP_PREFIX = "http://";

/// Scheme prefix of an `https` URL.
inline constexpr std::string_view g_HTTPS_PREFIX = "https://";

inline constexpr std::string_view g_WELL_KNOWN_PREFIX = "/.well-known/oauth-protected-resource";

/// The port an `http` URL names when it names none.
inline constexpr std::uint32_t g_HTTP_DEFAULT_PORT = 80;

/// The port an `https` URL names when it names none.
inline constexpr std::uint32_t g_HTTPS_DEFAULT_PORT = 443;

}  // namespace url

/// Read-ahead sizes of the per-request exchange watcher.
namespace modern_exchange {

/// Bytes a watch reads ahead while a request runs; they belong to the client's next request.
inline constexpr std::size_t g_WATCH_READ_SIZE = 4096;

/// Read-ahead held at once; past this the watch stops until the request is answered.
inline constexpr std::size_t g_MAX_READ_AHEAD = std::size_t{64} * 1024;

}  // namespace modern_exchange

/// Memory for the requests an HTTP server reads.
namespace http_request {

/// Bytes each connection keeps for the header fields of the request it reads; a larger header
/// takes heap memory past them.
inline constexpr std::size_t g_HEADER_ARENA_BYTES = 4096;

}  // namespace http_request

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
inline constexpr std::size_t g_DEFAULT_MAX_MESSAGE_BYTES = std::size_t{16} * 1024 * 1024;

inline constexpr std::size_t g_READ_CHUNK_BYTES = std::size_t{64} * 1024;

/// Input is read ahead of read_message() up to this many bytes.
inline constexpr std::size_t g_MAX_READ_AHEAD_BYTES = std::size_t{16} * 1024 * 1024;

/// What an empty line costs in the queue, so short lines cannot fill memory unnoticed.
inline constexpr std::size_t g_INPUT_LINE_OVERHEAD = 64;

/// How long the destructor waits for accepted writes to reach a peer that is reading.
inline constexpr std::chrono::seconds g_FINAL_FLUSH_WAIT{1};

}  // namespace stdio

/// Pipe buffer size and shutdown capabilities of the stdio client process handling.
namespace stdio_client {

/// The exit status of a child whose exec failed, the status POSIX shells give a command they cannot
/// find.
inline constexpr int g_EXEC_FAILED_STATUS = 127;

inline constexpr std::size_t g_CHUNK_BYTES = std::size_t{64} * 1024;

/// How long a killed process may take to be seen as ended, which the destructor of a Client that
/// launched it waits on top of the process's stdin_close_wait and terminate_wait.
inline constexpr std::chrono::milliseconds g_KILL_WAIT{1000};

/// Whether the process can be asked to exit before it is killed: SIGTERM on POSIX, nothing on Windows.
inline constexpr bool g_HAS_TERMINATE_STEP =
#ifdef _WIN32
    false;
#else
    true;
#endif

}  // namespace stdio_client

/// Limits and sentinels of the WebSocket transports.
namespace websocket {

inline constexpr auto g_NEVER = std::chrono::steady_clock::time_point::max();

/// What Beast defaults to, and what the stdio transport uses.
inline constexpr std::size_t g_DEFAULT_MAX_MESSAGE_BYTES = std::size_t{16} * 1024 * 1024;

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

/// While the handler of a request still runs or has a question on its way, the request looks
/// again after this long whether every question of the round is asked; each look that finds the
/// handler busy doubles the wait, up to g_SETTLE_POLL_MAX.
inline constexpr std::chrono::milliseconds g_SETTLE_POLL_FIRST{1};
inline constexpr std::chrono::milliseconds g_SETTLE_POLL_MAX{16};

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

/// The default of Server::set_max_in_flight_requests().
inline constexpr std::size_t g_DEFAULT_MAX_IN_FLIGHT_REQUESTS = 1024;

/// The default of Server::set_reverse_request_timeout().
inline constexpr std::chrono::milliseconds g_DEFAULT_REVERSE_REQUEST_TIMEOUT = std::chrono::minutes(10);

/// The default of Server::set_max_in_flight_bytes().
inline constexpr std::size_t g_DEFAULT_MAX_IN_FLIGHT_BYTES = std::size_t{256} * 1024 * 1024;

/// Resources one session may subscribe to, and the bytes of their URIs together.
inline constexpr std::size_t g_MAX_SUBSCRIPTIONS = 4096;

inline constexpr std::size_t g_MAX_SUBSCRIPTION_BYTES = std::size_t{8} * 1024 * 1024;

/// The longest tool name the tools page of the specification allows.
inline constexpr std::size_t g_MAX_TOOL_NAME_LENGTH = 128;

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
inline constexpr std::array<char, unicode::g_ASCII_END> g_ESCAPES = [] {
    std::array<char, unicode::g_ASCII_END> escapes{};
    for (std::size_t byte = 0; byte < unicode::g_C0_CONTROLS_END; ++byte) {
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

/// Bounds of the JSON parsing helpers.
namespace json_parse {

/// The deepest nesting of arrays and objects parse_json_fast() reads itself, one recursion per
/// level; a document nested this deep parses on a thread with a 256 KiB stack, also when built
/// with AddressSanitizer. Deeper text is left to nlohmann::json, whose parser keeps its own stack.
inline constexpr std::size_t g_MAX_FAST_DEPTH = 128;

/// The buffer on the stack that a number is copied into, with a terminating null, for strtod. A
/// longer number is copied to the heap.
inline constexpr std::size_t g_STACK_NUMBER_BYTES = 64;

}  // namespace json_parse

/// The regular expression engine.
namespace regex {

/// ECMAScript WhiteSpace and LineTerminator, as code point ranges in ascending order.
inline constexpr std::array<std::pair<char32_t, char32_t>, 10> g_SPACE_RANGES{
    {
        {0x09, 0x0D},
        {0x20, 0x20},
        {0xA0, 0xA0},
        {0x1680, 0x1680},
        {0x2000, 0x200A},
        {0x2028, 0x2029},
        {0x202F, 0x202F},
        {0x205F, 0x205F},
        {0x3000, 0x3000},
        {0xFEFF, 0xFEFF},
    },
};

/// An ECMAScript `\cX` escape stands for the control character whose code is the letter's code
/// modulo this.
inline constexpr char32_t g_CONTROL_LETTER_MODULUS = 32;

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
    mcp::constants::methods::g_SERVER_DISCOVER,
    mcp::constants::methods::g_TOOLS_LIST,
    mcp::constants::methods::g_TOOLS_CALL,
    mcp::constants::methods::g_RESOURCES_LIST,
    mcp::constants::methods::g_RESOURCES_READ,
    mcp::constants::methods::g_PROMPTS_LIST,
    mcp::constants::methods::g_PROMPTS_GET,
    mcp::constants::methods::g_COMPLETION_COMPLETE,
    mcp::constants::methods::g_RESOURCES_TEMPLATES_LIST,
    mcp::constants::methods::g_SUBSCRIPTIONS_LISTEN,
};

}  // namespace modern_methods

/// Schema keywords and bounds used when mirroring parameters into HTTP headers.
namespace param_headers {

/// Keywords whose value is one subschema.
inline constexpr std::array<std::string_view, 12> g_SCHEMA_KEYWORDS = {
    "additionalProperties",
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
    "contentSchema",
};

/// Keywords whose value is an array of subschemas.
inline constexpr std::array<std::string_view, 5> g_SCHEMA_ARRAY_KEYWORDS = {
    "allOf", "anyOf", "oneOf", "prefixItems", "items",
};

/// Keywords whose value maps names to subschemas, other than `properties`.
inline constexpr std::array<std::string_view, 5> g_SCHEMA_MAP_KEYWORDS = {
    "patternProperties", "dependentSchemas", "$defs", "definitions", "dependencies",
};

/// The largest integer an `x-mcp-header` argument may hold: 2^53 - 1.
inline constexpr std::int64_t g_MAX_SAFE_HEADER_INTEGER = 9'007'199'254'740'991;

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

inline constexpr std::size_t g_DEFAULT_MAX_TOKEN_CHARS = std::size_t{64} * 1024;

}  // namespace sealed_token

/// Work and size bounds of schema compilation and validation.
namespace schema {

/// Deepest nesting accepted in a schema document. Checking a schema against its dialect's rules and
/// compiling it both recurse with the schema, so this bounds the stack they use.
inline constexpr std::size_t g_MAX_DEPTH = 64;

/// Evaluation work every validation may do. A subschema evaluation is charged one more than the
/// evaluations it is nested in, and property names and whole-string keywords are charged by their
/// length.
inline constexpr std::size_t g_BASE_WORK = 1'000'000;

/// Bytes of a property name that count towards the size of a value's paths (see
/// ValueShape::path_bytes).
inline constexpr std::size_t g_MAX_COUNTED_NAME_BYTES = 1024;

/// Work allowed per unit of the value (see ValueShape::units), before the allowance for each node
/// of the schema below.
inline constexpr std::size_t g_WORK_PER_UNIT = 256;

/// A value's size in units (see ValueShape::units) counts one per level of its paths, one per this
/// many bytes of the names on them, and one per g_TEXT_BYTES_PER_UNIT bytes of its text.
inline constexpr std::size_t g_PATH_BYTES_PER_UNIT = 32;
inline constexpr std::size_t g_TEXT_BYTES_PER_UNIT = 16;

/// Further work allowed per unit of the value for each node of the schema: a step that compares
/// the value with a schema value, such as an `enum` member, costs 8.
inline constexpr std::size_t g_WORK_PER_UNIT_AND_SCHEMA_NODE = 8;

/// Deepest nesting of schema evaluations in one native validation: every keyword that evaluates a
/// subschema, in place or on a member or item, is one level.
inline constexpr std::size_t g_MAX_EVAL_DEPTH = 192;

/// Most documents CompiledSchema::compile() accepts besides the schema.
inline constexpr std::size_t g_MAX_SCHEMA_DOCUMENTS = 64;

/// Branch violations the native evaluation keeps while an `anyOf` or `oneOf` branch is evaluated, to
/// report the first branch's when no branch matches.
inline constexpr std::size_t g_MAX_BRANCH_VIOLATIONS = 4;

/// Base URI of a schema without `$id`; references are reported relative to it. The `.invalid`
/// domain is reserved (RFC 2606), so no registered document has this URI.
inline constexpr std::string_view g_DEFAULT_BASE_URI = "https://schema.invalid";

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

/// IPv4 special-purpose address blocks, which a metadata fetch must not reach.
namespace ipv4 {

/// An address block: an address in host byte order and the number of leading bits that name the
/// block.
struct Block {
    std::uint32_t network;
    unsigned prefix_length;  ///< 1 to 32.

    /// Whether @p address, in host byte order, is in the block.
    [[nodiscard]] constexpr bool contains(std::uint32_t address) const {
        const unsigned host_bits = std::numeric_limits<std::uint32_t>::digits - prefix_length;
        return (address >> host_bits) == (network >> host_bits);
    }
};

/// The address a.b.c.d in host byte order.
constexpr std::uint32_t address(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    constexpr unsigned bits = encoding::g_BITS_PER_BYTE;
    return (std::uint32_t{a} << (3U * bits)) | (std::uint32_t{b} << (2U * bits)) |
           (std::uint32_t{c} << bits) | std::uint32_t{d};
}

/// Loopback, 127.0.0.0/8 (RFC 1122).
inline constexpr Block g_LOOPBACK{.network = address(127, 0, 0, 0), .prefix_length = 8};

/// Link-local, 169.254.0.0/16 (RFC 3927).
inline constexpr Block g_LINK_LOCAL{.network = address(169, 254, 0, 0), .prefix_length = 16};

/// Private use, 10.0.0.0/8, 172.16.0.0/12 and 192.168.0.0/16 (RFC 1918).
inline constexpr std::array g_PRIVATE_USE{
    Block{.network = address(10, 0, 0, 0), .prefix_length = 8},
    Block{.network = address(172, 16, 0, 0), .prefix_length = 12},
    Block{.network = address(192, 168, 0, 0), .prefix_length = 16},
};

/// Limited broadcast, 255.255.255.255 (RFC 919).
inline constexpr std::uint32_t g_LIMITED_BROADCAST = address(255, 255, 255, 255);

/// Multicast, 224.0.0.0/4 (RFC 5771).
inline constexpr Block g_MULTICAST{.network = address(224, 0, 0, 0), .prefix_length = 4};

/// Blocks that reach no public host: "this network" 0.0.0.0/8 (RFC 791), reserved 240.0.0.0/4
/// (RFC 1112), shared address space 100.64.0.0/10 (RFC 6598), IETF protocol assignments
/// 192.0.0.0/24 (RFC 6890), benchmarking 198.18.0.0/15 (RFC 2544), and the documentation blocks
/// 192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24 (RFC 5737).
inline constexpr std::array g_RESERVED{
    Block{.network = address(0, 0, 0, 0), .prefix_length = 8},
    Block{.network = address(240, 0, 0, 0), .prefix_length = 4},
    Block{.network = address(100, 64, 0, 0), .prefix_length = 10},
    Block{.network = address(192, 0, 0, 0), .prefix_length = 24},
    Block{.network = address(198, 18, 0, 0), .prefix_length = 15},
    Block{.network = address(192, 0, 2, 0), .prefix_length = 24},
    Block{.network = address(198, 51, 100, 0), .prefix_length = 24},
    Block{.network = address(203, 0, 113, 0), .prefix_length = 24},
};

}  // namespace ipv4

/// IPv6 address blocks that embed an IPv4 address or reach no public host.
namespace ipv6 {

/// The NAT64 well-known prefix, 64:ff9b::/96 (RFC 6052). An IPv4 address follows it.
inline constexpr std::array<unsigned char, 12> g_NAT64_PREFIX{
    0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0,
};

/// The 6to4 prefix, 2002::/16 (RFC 3056). An IPv4 address follows it.
inline constexpr std::array<unsigned char, 2> g_SIX_TO_FOUR_PREFIX{0x20, 0x02};

/// Unique local addresses, fc00::/7 (RFC 4193): a first byte that is this prefix under this mask.
inline constexpr unsigned char g_UNIQUE_LOCAL_PREFIX = 0xFC;
inline constexpr unsigned char g_UNIQUE_LOCAL_MASK = 0xFE;

}  // namespace ipv6

/// Requests a Client sends.
namespace client_requests {

/// Starts each progress token the client makes for RequestOptions::on_progress. A string, so that
/// no integer token an application writes into another request can equal it.
inline constexpr std::string_view g_PROGRESS_TOKEN_PREFIX = "mcp-cpp-sdk-progress-";

}  // namespace client_requests

/// The stdio client transport that starts its server again.
namespace stdio_restart {

/// Starts the id a request of a restarted server gets toward the client when the id is still that
/// of an unanswered request of an earlier process of that server.
inline constexpr std::string_view g_RENAMED_REQUEST_PREFIX = "mcp-cpp-sdk-restart-";

}  // namespace stdio_restart

/// Accounting constants of the serialized transport writer.
namespace transport_writer {

/// What queued_bytes() counts for each queued message besides its text: an allowance for the memory
/// the queue holds per message (the shared text, the operation with its timer, the queue slot and
/// the posted handler). A bound on queued_bytes() then bounds memory even when the messages are
/// small.
inline constexpr std::size_t g_QUEUED_MESSAGE_OVERHEAD = 512;

}  // namespace transport_writer

}  // namespace mcp::detail::constants
