#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mcp::constants {

/// Limits of the HTTP replay store.
namespace event_store {

/**
 * @brief The default capacity of the event store.
 */
inline constexpr std::size_t g_DEFAULT_CAPACITY = 1024;

/// Most bytes of events one replay store keeps. Older events are dropped first, and the newest is
/// kept whatever its size.
inline constexpr std::size_t g_MAX_BYTES = 32U * 1024U * 1024U;

}  // namespace event_store

/// Graceful shutdown defaults.
namespace shutdown {

/// Time graceful_shutdown() waits for in-flight requests before it stops the io_context, in ms.
inline constexpr int g_TIMEOUT_MS = 5000;

}  // namespace shutdown

/// Client defaults.
namespace client {

/// How long the destructor of a Client that runs its own event loop waits for the loop to finish
/// closing the connection, such as a launched server process exiting, before it stops the loop,
/// in ms. Covers StdioProcessOptions' default stdin_close_wait and terminate_wait; a launched
/// server whose waits add up to more is waited for that long.
inline constexpr int g_CLOSE_WAIT_MS = 5000;

}  // namespace client

/// JSON-RPC and MCP error codes carried in `JsonRpcError::code`.
namespace jsonrpc {

/**
 * @brief JSON-RPC error code for unauthorized request.
 */
inline constexpr int g_UNAUTHORIZED = -32000;

/**
 * @brief JSON-RPC error code for request timeout.
 */
inline constexpr int g_REQUEST_TIMEOUT = -32001;

/**
 * @brief Error code the SDK uses locally when the transport closes.
 *
 * @details Outside the range JSON-RPC reserves (-32768 to -32000), as the specification asks of
 * codes it does not define, so it cannot be mistaken for an error the peer sent. -32002 is resource
 * not found in protocol revisions 2025-11-25 and earlier.
 */
inline constexpr int g_CONNECTION_CLOSED = -31000;

/**
 * @brief JSON-RPC error code for a resource that does not exist, in protocol revisions 2025-11-25
 * and earlier. 2026-07-28 uses g_INVALID_PARAMS instead.
 */
inline constexpr int g_RESOURCE_NOT_FOUND = -32002;

/**
 * @brief JSON-RPC error code for a mismatched or missing required header.
 */
inline constexpr int g_HEADER_MISMATCH = -32020;

/**
 * @brief JSON-RPC error code for a missing required client capability.
 */
inline constexpr int g_MISSING_REQUIRED_CLIENT_CAPABILITY = -32021;

/**
 * @brief JSON-RPC error code for an unsupported protocol version.
 */
inline constexpr int g_UNSUPPORTED_PROTOCOL_VERSION = -32022;

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

}  // namespace jsonrpc

/// The JSON-RPC methods the SDK sends and handles. Each name spells its method in upper case, with
/// `/` and each change from lower to upper case written as `_`.
namespace methods {

inline constexpr std::string_view g_INITIALIZE = "initialize";

inline constexpr std::string_view g_PING = "ping";

inline constexpr std::string_view g_SERVER_DISCOVER = "server/discover";

inline constexpr std::string_view g_TOOLS_LIST = "tools/list";

inline constexpr std::string_view g_TOOLS_CALL = "tools/call";

inline constexpr std::string_view g_RESOURCES_LIST = "resources/list";

inline constexpr std::string_view g_RESOURCES_READ = "resources/read";

inline constexpr std::string_view g_RESOURCES_SUBSCRIBE = "resources/subscribe";

inline constexpr std::string_view g_RESOURCES_UNSUBSCRIBE = "resources/unsubscribe";

inline constexpr std::string_view g_RESOURCES_TEMPLATES_LIST = "resources/templates/list";

inline constexpr std::string_view g_PROMPTS_LIST = "prompts/list";

inline constexpr std::string_view g_PROMPTS_GET = "prompts/get";

inline constexpr std::string_view g_COMPLETION_COMPLETE = "completion/complete";

inline constexpr std::string_view g_LOGGING_SET_LEVEL = "logging/setLevel";

inline constexpr std::string_view g_SAMPLING_CREATE_MESSAGE = "sampling/createMessage";

inline constexpr std::string_view g_ELICITATION_CREATE = "elicitation/create";

inline constexpr std::string_view g_ROOTS_LIST = "roots/list";

inline constexpr std::string_view g_TASKS_GET = "tasks/get";

inline constexpr std::string_view g_TASKS_CANCEL = "tasks/cancel";

inline constexpr std::string_view g_TASKS_UPDATE = "tasks/update";

inline constexpr std::string_view g_SUBSCRIPTIONS_LISTEN = "subscriptions/listen";

inline constexpr std::string_view g_NOTIFICATIONS_INITIALIZED = "notifications/initialized";

inline constexpr std::string_view g_NOTIFICATIONS_CANCELLED = "notifications/cancelled";

inline constexpr std::string_view g_NOTIFICATIONS_PROGRESS = "notifications/progress";

inline constexpr std::string_view g_NOTIFICATIONS_MESSAGE = "notifications/message";

inline constexpr std::string_view g_NOTIFICATIONS_TOOLS_LIST_CHANGED =
    "notifications/tools/list_changed";

inline constexpr std::string_view g_NOTIFICATIONS_RESOURCES_LIST_CHANGED =
    "notifications/resources/list_changed";

inline constexpr std::string_view g_NOTIFICATIONS_RESOURCES_UPDATED = "notifications/resources/updated";

inline constexpr std::string_view g_NOTIFICATIONS_PROMPTS_LIST_CHANGED =
    "notifications/prompts/list_changed";

inline constexpr std::string_view g_NOTIFICATIONS_ROOTS_LIST_CHANGED =
    "notifications/roots/list_changed";

inline constexpr std::string_view g_NOTIFICATIONS_TASKS = "notifications/tasks";

inline constexpr std::string_view g_NOTIFICATIONS_SUBSCRIPTIONS_ACKNOWLEDGED =
    "notifications/subscriptions/acknowledged";

inline constexpr std::string_view g_NOTIFICATIONS_ELICITATION_COMPLETE =
    "notifications/elicitation/complete";

// Methods of the skills extension.

inline constexpr std::string_view g_SKILLS_LIST = "skills/list";

inline constexpr std::string_view g_SKILLS_GET = "skills/get";

inline constexpr std::string_view g_RESOURCES_DIRECTORY_READ = "resources/directory/read";

}  // namespace methods

/// Protocol revisions the SDK speaks and the lists that group them.
namespace protocol {

/**
 * @brief Protocol version 2024-11-05.
 */
inline constexpr std::string_view g_PROTOCOL_VERSION_2024_11_05 = "2024-11-05";

/**
 * @brief Protocol version 2025-03-26.
 */
inline constexpr std::string_view g_PROTOCOL_VERSION_2025_03_26 = "2025-03-26";

/**
 * @brief Protocol version 2025-06-18.
 */
inline constexpr std::string_view g_PROTOCOL_VERSION_2025_06_18 = "2025-06-18";

/**
 * @brief Protocol version 2025-11-25.
 */
inline constexpr std::string_view g_PROTOCOL_VERSION_2025_11_25 = "2025-11-25";

/**
 * @brief Protocol version 2026-07-28, the first revision served per request, without initialize.
 *
 * @details Listed in g_MODERN_PROTOCOL_VERSIONS and g_SUPPORTED_PROTOCOL_VERSIONS, not in
 * g_LEGACY_PROTOCOL_VERSIONS. initialize never negotiates it.
 */
inline constexpr std::string_view g_PROTOCOL_VERSION_2026_07_28 = "2026-07-28";

/**
 * @brief The newest version in g_SUPPORTED_PROTOCOL_VERSIONS.
 *
 * @details initialize negotiation does not read it; the newest version initialize negotiates is
 * g_LATEST_LEGACY_PROTOCOL_VERSION.
 */
inline constexpr std::string_view g_LATEST_PROTOCOL_VERSION = g_PROTOCOL_VERSION_2026_07_28;

/**
 * @brief The newest protocol version that starts a session with initialize.
 */
inline constexpr std::string_view g_LATEST_LEGACY_PROTOCOL_VERSION = g_PROTOCOL_VERSION_2025_11_25;

/**
 * @brief Protocol versions that start a session with initialize (2025-11-25 and earlier).
 *
 * @details initialize negotiates only these versions, whatever the newest supported version is.
 */
inline constexpr std::array<std::string_view, 4> g_LEGACY_PROTOCOL_VERSIONS = {
    g_PROTOCOL_VERSION_2024_11_05,
    g_PROTOCOL_VERSION_2025_03_26,
    g_PROTOCOL_VERSION_2025_06_18,
    g_PROTOCOL_VERSION_2025_11_25,
};

/**
 * @brief Protocol versions whose requests carry their version in `_meta` and need no
 * initialize (2026-07-28 and later).
 */
inline constexpr std::array<std::string_view, 1> g_MODERN_PROTOCOL_VERSIONS = {
    g_PROTOCOL_VERSION_2026_07_28,
};

/**
 * @brief Every protocol version this SDK supports: the legacy versions, then the modern ones.
 *
 * @details A server serving every era advertises these in server/discover; a
 * ServerProtocolPolicy narrows the list. initialize negotiation does not read this list; it uses
 * g_LEGACY_PROTOCOL_VERSIONS, so a legacy peer requesting 2026-07-28 in initialize negotiates
 * g_LATEST_LEGACY_PROTOCOL_VERSION.
 */
inline constexpr std::array<std::string_view, 5> g_SUPPORTED_PROTOCOL_VERSIONS = {
    g_PROTOCOL_VERSION_2024_11_05, g_PROTOCOL_VERSION_2025_03_26, g_PROTOCOL_VERSION_2025_06_18,
    g_PROTOCOL_VERSION_2025_11_25, g_PROTOCOL_VERSION_2026_07_28,
};

}  // namespace protocol

/// Reserved `_meta` keys that carry per-request values on the 2026-07-28 revision.
namespace meta {

/// @brief `_meta` key carrying a request's protocol version (2026-07-28 and later).
inline constexpr std::string_view g_META_PROTOCOL_VERSION = "io.modelcontextprotocol/protocolVersion";

/// @brief `_meta` key carrying the client capabilities relevant to a request.
inline constexpr std::string_view g_META_CLIENT_CAPABILITIES =
    "io.modelcontextprotocol/clientCapabilities";

/// @brief `_meta` key carrying the client's name and version.
inline constexpr std::string_view g_META_CLIENT_INFO = "io.modelcontextprotocol/clientInfo";

/// @brief `_meta` key carrying the minimum log level the server should emit for a request.
inline constexpr std::string_view g_META_LOG_LEVEL = "io.modelcontextprotocol/logLevel";

/// @brief `_meta` key carrying the server's name and version on a result.
inline constexpr std::string_view g_META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";

/// @brief `_meta` key carrying the id of the subscriptions/listen request a message belongs to.
inline constexpr std::string_view g_META_SUBSCRIPTION_ID = "io.modelcontextprotocol/subscriptionId";

}  // namespace meta

/// MCP Apps extension identifiers, MIME type and URI prefix.
namespace apps {

/// Identifier of the extension in `capabilities.extensions`.
inline constexpr std::string_view g_EXTENSION_ID = "io.modelcontextprotocol/ui";

/// MIME type of an HTML view.
inline constexpr std::string_view g_RESOURCE_MIME_TYPE = "text/html;profile=mcp-app";

/// URI scheme every view resource uses, with the `://` separator.
inline constexpr std::string_view g_URI_PREFIX = "ui://";

/// Deprecated flat `_meta` key that names a tool's view; read for compatibility, never written.
inline constexpr std::string_view g_LEGACY_RESOURCE_URI_KEY = "ui/resourceUri";

}  // namespace apps

/// Skills extension identifiers, URI prefix, MIME type and size limits.
namespace skills {

/// Identifier of the extension in `capabilities.extensions`.
inline constexpr std::string_view g_EXTENSION_ID = "io.modelcontextprotocol/skills";

/// URI prefix servers conventionally use for skill resources.
inline constexpr std::string_view g_URI_PREFIX = "skill://";

/// Name of the file every skill has at its root.
inline constexpr std::string_view g_SKILL_FILE_NAME = "SKILL.md";

/// MIME type of a directory resource.
inline constexpr std::string_view g_DIRECTORY_MIME_TYPE = "inode/directory";

/// Most files a skill entry may list; hosts support skills up to this size.
inline constexpr std::size_t g_MAX_RESOURCES_PER_SKILL = 512;

/// Most bytes a skill's listed files may add up to (16 MiB).
inline constexpr std::int64_t g_MAX_SKILL_BYTES = 16 * 1024 * 1024;

}  // namespace skills

/// Tasks extension identifier and result type.
namespace tasks {

/// @brief The identifier of the tasks extension, the key it is declared under in the `extensions`
/// capability of a client request and of a server.
inline constexpr std::string_view g_EXTENSION_ID = "io.modelcontextprotocol/tasks";

/// @brief The `resultType` of a CreateTaskResult.
inline constexpr std::string_view g_RESULT_TYPE_TASK = "task";

}  // namespace tasks

/// OAuth extension ids, token types, grant types and defaults of the OAuth client helpers.
namespace oauth {

/// Identifier of the extension in `capabilities.extensions`.
inline constexpr std::string_view g_CLIENT_CREDENTIALS_EXTENSION_ID =
    "io.modelcontextprotocol/oauth-client-credentials";

/// Identifier of the extension in `capabilities.extensions`.
inline constexpr std::string_view g_ENTERPRISE_AUTHORIZATION_EXTENSION_ID =
    "io.modelcontextprotocol/enterprise-managed-authorization";

/// Token type of an OpenID Connect ID token.
inline constexpr std::string_view g_ID_TOKEN_TYPE = "urn:ietf:params:oauth:token-type:id_token";

/// Token type of a refresh token the IdP issued for a SAML assertion.
inline constexpr std::string_view g_REFRESH_TOKEN_TYPE =
    "urn:ietf:params:oauth:token-type:refresh_token";

/// Token type of a SAML 2.0 assertion.
inline constexpr std::string_view g_SAML2_TYPE = "urn:ietf:params:oauth:token-type:saml2";

/// Token type of an Identity Assertion JWT Authorization Grant (ID-JAG).
inline constexpr std::string_view g_ID_JAG_TOKEN_TYPE = "urn:ietf:params:oauth:token-type:id-jag";

/// Entry of an authorization server's `authorization_grant_profiles_supported` that names the ID-JAG
/// profile.
inline constexpr std::string_view g_ID_JAG_PROFILE = "urn:ietf:params:oauth:grant-profile:id-jag";

/// Grant type of an RFC 8693 token exchange request.
inline constexpr std::string_view g_TOKEN_EXCHANGE_GRANT =
    "urn:ietf:params:oauth:grant-type:token-exchange";

/// Grant type of an RFC 7523 JWT bearer request.
inline constexpr std::string_view g_JWT_BEARER_GRANT = "urn:ietf:params:oauth:grant-type:jwt-bearer";

/// Seconds before its expiry at which a cached access token counts as expired.
inline constexpr int g_DEFAULT_TOKEN_LIFETIME_SAFETY_MARGIN_SECONDS = 30;

/// Shortest PKCE code verifier, in characters (RFC 7636).
inline constexpr std::size_t g_MIN_VERIFIER_LENGTH = 43;

/// Longest PKCE code verifier, in characters (RFC 7636).
inline constexpr std::size_t g_MAX_VERIFIER_LENGTH = 128;

/// Length of the PKCE code verifiers the client generates, in characters.
inline constexpr std::size_t g_DEFAULT_VERIFIER_LENGTH = 64;

/// Seconds a cached discovery response is kept by default.
inline constexpr std::size_t g_DEFAULT_CACHE_TTL_SECONDS = 300;

}  // namespace oauth

/// Defaults of the fetch policy for authorization metadata documents.
namespace metadata_policy {

/// Most response body bytes accepted from a metadata or token endpoint.
inline constexpr std::size_t g_DEFAULT_MAX_BYTES = 256 * 1024;

/// Most redirects followed while fetching a metadata document.
inline constexpr std::size_t g_DEFAULT_MAX_REDIRECTS = 3;

}  // namespace metadata_policy

/// Defaults of the HTTP server transports.
namespace http_server {

/**
 * @brief Default cap on an HTTP request body accepted by the server transports, in bytes.
 *
 * Binary content reaches an MCP server as base64 inside the JSON body -- `ImageContent::data`,
 * `BlobResourceContents::blob` and `AudioContent` have no out-of-band or streaming path -- and base64
 * inflates by 4/3. Eight mebibytes carries roughly 6 MB of raw content, while still bounding what one
 * unauthenticated connection can make a server allocate.
 */
inline constexpr std::size_t g_DEFAULT_MAX_REQUEST_BODY_BYTES = 8 * 1024 * 1024;

}  // namespace http_server

}  // namespace mcp::constants
