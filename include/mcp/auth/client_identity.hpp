#pragma once

#include <mcp/core/export.hpp>

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth {

/**
 * @brief How the client identity presented to an authorization server was obtained.
 */
enum class ClientIdentitySource {
    pre_registered,               ///< Supplied by the application; never re-registered.
    client_id_metadata_document,  ///< A published HTTPS document URL used as the client identifier.
    dynamic_registration,         ///< Obtained from the server's RFC 7591 registration endpoint.
};

/**
 * @brief Short, stable description of a client identity source.
 *
 * @param source Source to describe.
 * @return A description suitable for diagnostics.
 */
[[nodiscard]] MCP_API std::string_view describe(ClientIdentitySource source);

/**
 * @brief Client metadata sent at dynamic registration and published as a client ID metadata
 *        document.
 *
 * @details The defaults describe the client this SDK implements: an authorization-code client that
 * also intends to renew its grant, which is why `refresh_token` is advertised in `grant_types`
 * without any assumption that the server will actually issue one.
 */
struct OAuthClientMetadata {
    std::vector<std::string> redirect_uris;  ///< Redirect URIs the client will use.
    /// SEP-837 `application_type`. Always present in the registration request body.
    std::string application_type{"native"};
    /// Advertised grant types. `refresh_token` is advertised so the server may issue one.
    std::vector<std::string> grant_types{"authorization_code", "refresh_token"};
    std::vector<std::string> response_types{"code"};  ///< Advertised response types.
    std::optional<std::string> client_name;           ///< Optional human-readable client name.
    std::optional<std::string> client_uri;            ///< Optional client home page.
    std::optional<std::string> software_id;           ///< Optional stable software identifier.
    std::optional<std::string> software_version;      ///< Optional software version.
    std::optional<std::string> scope;                 ///< Optional space-delimited requested scope.
    /// Optional token endpoint authentication method the client wishes to register for.
    std::optional<std::string> token_endpoint_auth_method;
};

/**
 * @brief Serialize client metadata into an RFC 7591 registration request body.
 *
 * @param json JSON object to populate.
 * @param metadata Metadata to serialize.
 */
MCP_API void to_json(nlohmann::json& json, const OAuthClientMetadata& metadata);

/**
 * @brief Client credentials together with the issuer they belong to.
 *
 * @details SEP-2352 binds credentials to one authorization server. `issuer` records that binding so
 * a credential can never be presented to a different server by accident, and so a stored entry
 * whose recorded issuer disagrees with its storage key can be discarded rather than trusted.
 */
struct OAuthClientInformation {
    std::string client_id;                            ///< Client identifier presented to the server.
    std::optional<std::string> client_secret;         ///< Optional confidential-client secret.
    std::optional<std::int64_t> client_id_issued_at;  ///< Issue time reported by the server.
    std::optional<std::int64_t> client_secret_expires_at;  ///< Secret expiry, 0 meaning "never".
    std::string issuer;                                    ///< Issuer these credentials are bound to.
    ClientIdentitySource source{ClientIdentitySource::dynamic_registration};  ///< How they were got.

    /**
     * @brief Determine whether the server-reported secret expiry has passed.
     *
     * @param now_seconds Current time as seconds since the Unix epoch.
     * @return True when a non-zero expiry was reported and it is in the past.
     */
    [[nodiscard]] MCP_API bool secret_expired(std::int64_t now_seconds) const;
};

/**
 * @brief Deserialize client information from an RFC 7591 registration response.
 *
 * @param json JSON registration response.
 * @param information Structure to populate.
 *
 * @note `issuer` and `source` are not carried by the wire format; the caller sets them from the
 * metadata document the registration endpoint was taken from.
 */
MCP_API void from_json(const nlohmann::json& json, OAuthClientInformation& information);

/**
 * @brief Serialize client information, including its issuer binding.
 *
 * @param json JSON object to populate.
 * @param information Information to serialize.
 */
MCP_API void to_json(nlohmann::json& json, const OAuthClientInformation& information);

/**
 * @brief Storage for acquired client credentials, keyed by authorization-server issuer.
 *
 * @details Deliberately separate from TokenStore, which is keyed by MCP server URL. One MCP server
 * URL can be protected by different authorization servers over time, and one authorization server
 * can protect several MCP server URLs, so a server-URL key would eventually present one server's
 * credentials to another. Keying by issuer is what makes the SEP-2352 binding hold.
 */
class ClientCredentialStore {
   public:
    virtual ~ClientCredentialStore() = default;

    /**
     * @brief Store or replace the credentials associated with an issuer.
     *
     * @param issuer Authorization server issuer used as the storage key.
     * @param information Credentials to persist.
     */
    virtual void store(const std::string& issuer, OAuthClientInformation information) = 0;

    /**
     * @brief Load the credentials associated with an issuer.
     *
     * @param issuer Authorization server issuer used as the storage key.
     * @return The stored credentials, if present.
     */
    [[nodiscard]] virtual std::optional<OAuthClientInformation> load(
        const std::string& issuer) const = 0;

    /**
     * @brief Remove the credentials associated with an issuer.
     *
     * @param issuer Authorization server issuer used as the storage key.
     */
    virtual void remove(const std::string& issuer) = 0;
};

/**
 * @brief Thread-safe in-memory client credential store.
 */
class MCP_API InMemoryClientCredentialStore : public ClientCredentialStore {
   public:
    InMemoryClientCredentialStore();
    ~InMemoryClientCredentialStore() override;

    InMemoryClientCredentialStore(const InMemoryClientCredentialStore&) = delete;
    InMemoryClientCredentialStore& operator=(const InMemoryClientCredentialStore&) = delete;
    InMemoryClientCredentialStore(InMemoryClientCredentialStore&&) = delete;
    InMemoryClientCredentialStore& operator=(InMemoryClientCredentialStore&&) = delete;

    /**
     * @brief Store or replace credentials in the in-memory cache.
     *
     * @param issuer Authorization server issuer used as the storage key.
     * @param information Credentials to persist.
     */
    void store(const std::string& issuer, OAuthClientInformation information) override;

    /**
     * @brief Load credentials from the in-memory cache.
     *
     * @param issuer Authorization server issuer used as the storage key.
     * @return The stored credentials, if present.
     */
    std::optional<OAuthClientInformation> load(const std::string& issuer) const override;

    /**
     * @brief Remove credentials from the in-memory cache.
     *
     * @param issuer Authorization server issuer used as the storage key.
     */
    void remove(const std::string& issuer) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Application-supplied inputs to client identity selection.
 */
struct ClientIdentityConfig {
    /// Credentials the application obtained out of band. When present they are used exactly as
    /// given for every authorization server, and registration is never attempted.
    std::optional<OAuthClientInformation> pre_registered;
    /// URL of a client ID metadata document the application publishes. Used as the client
    /// identifier itself when the authorization server advertises support for it.
    std::optional<std::string> client_metadata_url;
    /// Metadata sent when dynamic registration is the remaining path.
    OAuthClientMetadata metadata;
};

/**
 * @brief The authorization-server facts client identity selection depends on.
 *
 * @details Deliberately a small value rather than the whole metadata document, so selection is a
 * pure decision over four inputs and can be tested without any discovery machinery.
 */
struct ClientIdentityServerFacts {
    std::string issuer;                                 ///< Issuer recorded from the metadata.
    bool client_id_metadata_document_supported{false};  ///< `client_id_metadata_document_supported`.
    std::optional<std::string> registration_endpoint;   ///< RFC 7591 registration endpoint, if any.
    std::vector<std::string> scopes_supported;  ///< `scopes_supported`, used for offline access.
};

/**
 * @brief The client identity path chosen for one authorization server.
 */
enum class ClientIdentityDecision {
    use_pre_registered,               ///< Present the application's injected credentials.
    use_client_id_metadata_document,  ///< Present the configured document URL as the client ID.
    reuse_stored_registration,        ///< Present credentials already registered with this issuer.
    register_dynamically,             ///< Register with this issuer and present the result.
    unavailable,                      ///< No path is available; authorization cannot proceed.
};

/**
 * @brief Short, stable description of a client identity decision.
 *
 * @param decision Decision to describe.
 * @return A description suitable for diagnostics.
 */
[[nodiscard]] MCP_API std::string_view describe(ClientIdentityDecision decision);

/**
 * @brief Choose the client identity path for one authorization server, without any network access.
 *
 * @param config Application-supplied identity inputs.
 * @param server Facts recorded from the selected authorization server's metadata.
 * @param stored Credentials already held for this issuer, if any.
 * @return The chosen path.
 *
 * @details Precedence, highest first:
 *
 * 1. Injected credentials. They win outright and there is no fallback: if the application named a
 *    client, registering a different one behind its back would silently change who the user is
 *    consenting to.
 * 2. A configured client ID metadata document URL, when the server advertises support for one.
 *    Dynamic registration is skipped entirely on this path.
 * 3. Credentials already registered with this exact issuer. A stored entry whose recorded issuer
 *    disagrees with the issuer being contacted is ignored rather than presented, which is also what
 *    makes an authorization-server change fall through to a fresh registration.
 * 4. Dynamic registration, when the server publishes a registration endpoint. RFC 7591 registration
 *    is deprecated in favour of client ID metadata documents, so it is only ever reached last.
 */
[[nodiscard]] MCP_API ClientIdentityDecision
select_client_identity(const ClientIdentityConfig& config, const ClientIdentityServerFacts& server,
                       const std::optional<OAuthClientInformation>& stored);

/**
 * @brief Build the RFC 7591 dynamic client registration request body.
 *
 * @param metadata Client metadata to register.
 * @param server Facts recorded from the authorization server's metadata.
 * @return The JSON request body.
 *
 * @details Carries `application_type` (SEP-837) unconditionally. `offline_access` is appended to
 * the requested scope only when the server advertises it in `scopes_supported`, because asking for
 * a scope the server never published is a request it is entitled to reject outright.
 */
[[nodiscard]] MCP_API nlohmann::json build_registration_request(
    const OAuthClientMetadata& metadata, const ClientIdentityServerFacts& server);

}  // namespace mcp::auth
