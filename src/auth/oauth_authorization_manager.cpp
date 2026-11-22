#include <mcp/auth/oauth.hpp>
#include <mcp/core/async.hpp>

#include "../detail/encoding.hpp"
#include "../detail/secure_random.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

namespace net = boost::asio;

namespace {

/// Flattens peer-controlled text -- control characters, bidi overrides and ill-formed UTF-8 -- and
/// caps it, so a value a peer chose cannot forge or reorder a line of a diagnostic. Every throw
/// site in this file that interpolates a value originating from a peer must route it through here;
/// `MetadataPolicyError` does the same inside its own constructor, so its throw sites do not repeat
/// it. This is a convention, not something the compiler checks: a new throw site that forgets it
/// re-opens the hole silently.
using detail::sanitize_for_diagnostics;

/// RFC 9728 section 3.3: does a protected-resource metadata `resource` value identify the server this
/// client is configured for? Accepted when the two are byte-exact, or when `resource` is a proper URI
/// prefix of `server_url` under RFC 8707 audience semantics: same scheme and authority, compared
/// byte-exact with no normalization (an explicit default port differs from an implicit one), and a
/// path that is empty/`"/"` (matches any path) or a prefix of the server URL's path on a `/` segment
/// boundary. A single trailing `/` on `resource`'s path is ignored. `resource` may not carry a query
/// or fragment.
///
/// An origin-root resource is accepted for any path by design: the root-PRM layout (conformance
/// `auth/metadata-var2`). This only decides whether the value may be sent as the `resource`
/// parameter; the authorization server remains the arbiter of the audience it issues.
bool resource_identifies_server(const std::string& resource, const std::string& server_url) {
    if (resource == server_url) {
        return true;
    }
    if (resource.find('?') != std::string::npos || resource.find('#') != std::string::npos) {
        return false;
    }
    const auto resource_origin = metadata_url_origin(resource);
    const auto server_origin = metadata_url_origin(server_url);
    if (resource_origin.empty() || resource_origin != server_origin) {
        return false;
    }
    auto resource_path = resource.substr(resource_origin.size());
    if (resource_path.empty() || resource_path == "/") {
        return true;
    }
    if (resource_path.back() == '/') {
        resource_path.pop_back();
    }
    const auto server_path = server_url.substr(server_origin.size());
    if (server_path == resource_path) {
        return true;
    }
    return server_path.size() > resource_path.size() &&
           server_path.compare(0, resource_path.size(), resource_path) == 0 &&
           server_path[resource_path.size()] == '/';
}

/// Throws unless `endpoint` is fit to hand the application as the authorization URL, which it opens
/// in a browser. The server names it, so it must be an absolute `https` URL (`http` only on loopback,
/// when the policy allows plain HTTP there), outside the policy's denied origins, without a fragment
/// (RFC 6749 §3.1), and made only of characters a URI may contain (RFC 3986 §2). A different origin
/// from the other endpoints is allowed: the SDK never connects to this one, the browser does.
void require_browser_safe_authorization_endpoint(const MetadataFetchPolicy& policy,
                                                 const std::string& endpoint) {
    const auto uri_character = [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               std::string_view("-._~:/?[]@!$&'()*+,;=%").find(static_cast<char>(character)) !=
                   std::string_view::npos;
    };
    bool acceptable = !endpoint.empty() && std::all_of(endpoint.begin(), endpoint.end(), uri_character);
    if (acceptable) {
        auto any_origin = policy;
        any_origin.origin_allowance = [](const std::string& /*origin*/) { return true; };
        switch (validate_metadata_url(any_origin, endpoint)) {
            case MetadataUrlDecision::eAllowed:
            case MetadataUrlDecision::eAddressLinkLocal:
            case MetadataUrlDecision::eAddressPrivate:
            case MetadataUrlDecision::eAddressLoopback:
                break;
            default:
                acceptable = false;
        }
    }
    if (!acceptable) {
        throw std::runtime_error(
            "Authorization server metadata named an authorization endpoint that is not a plain https "
            "URL");
    }
}

/// Throws unless `resource` is present and identifies `server_url`.
///
/// Called before the SDK contacts anything the protected-resource document names, so a document
/// that does not identify our configured server cannot make us fetch attacker-named authorization
/// server metadata, register a client with it, or persist those credentials.
void require_resource_identifies_server(const std::string& resource, const std::string& server_url) {
    if (resource.empty()) {
        // RFC 9728 §2 makes `resource` a required member; a PRM that omits it (or ships it
        // empty) does not meet the spec and must not be trusted silently.
        throw std::runtime_error("Protected resource metadata for server '" +
                                 sanitize_for_diagnostics(server_url) +
                                 "' is missing the required 'resource' member");
    }
    if (!resource_identifies_server(resource, server_url)) {
        throw std::runtime_error("Protected resource metadata resource '" +
                                 sanitize_for_diagnostics(resource) + "' does not identify server '" +
                                 sanitize_for_diagnostics(server_url) + "'");
    }
}

}  // namespace

namespace {

/// Join scopes into the space-delimited form an authorization request carries.
std::string join_scopes(const std::vector<std::string>& scopes) {
    std::string joined;
    for (const auto& scope : scopes) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += scope;
    }
    return joined;
}

std::vector<std::string> split_scopes(std::string_view scopes) {
    std::vector<std::string> split;
    std::size_t cursor = 0;
    while (cursor < scopes.size()) {
        const auto next = scopes.find(' ', cursor);
        const auto token = scopes.substr(cursor, next == std::string_view::npos ? next : next - cursor);
        if (!token.empty()) {
            split.emplace_back(token);
        }
        if (next == std::string_view::npos) {
            break;
        }
        cursor = next + 1;
    }
    return split;
}

/// Union of two scope strings, preserving the order of `primary` and appending whatever only
/// `secondary` carries. Step-up authorization re-authorizes on a challenge that names only what the
/// refused operation needed, so without the union an earlier grant's scopes would be dropped.
std::optional<std::string> union_scopes(const std::optional<std::string>& primary,
                                        const std::optional<std::string>& secondary) {
    if (!primary || primary->empty()) {
        return secondary && !secondary->empty() ? secondary : std::nullopt;
    }
    if (!secondary || secondary->empty()) {
        return primary;
    }
    auto merged = split_scopes(*primary);
    for (auto& candidate : split_scopes(*secondary)) {
        if (std::find(merged.begin(), merged.end(), candidate) == merged.end()) {
            merged.push_back(std::move(candidate));
        }
    }
    return join_scopes(merged);
}

/// Choose the token endpoint authentication method from what the server advertises.
///
/// A server that publishes the list has told the client which methods it will accept, so the
/// client picks the strongest one it can actually satisfy rather than guessing. A server that
/// publishes nothing leaves the decision unset, which sends the secret, when there is one, in the
/// request body.
std::optional<std::string> select_token_endpoint_auth_method(
    const std::optional<std::vector<std::string>>& supported, bool has_client_secret) {
    if (!supported || supported->empty()) {
        return std::nullopt;
    }
    const auto advertises = [&supported](std::string_view method) {
        return std::find(supported->begin(), supported->end(), method) != supported->end();
    };
    if (has_client_secret) {
        if (advertises("client_secret_basic")) {
            return "client_secret_basic";
        }
        if (advertises("client_secret_post")) {
            return "client_secret_post";
        }
    }
    if (advertises("none")) {
        return "none";
    }
    return std::nullopt;
}

std::string append_query(const std::string& endpoint, const std::string& query) {
    if (query.empty()) {
        return endpoint;
    }
    const auto separator = endpoint.find('?') == std::string::npos ? '?' : '&';
    return endpoint + separator + query;
}

}  // namespace

struct OAuthAuthorizationManager::Impl {
    /// One coalescing authorization attempt, and the channel every follower of it reads its result
    /// from.
    ///
    /// The outcome lives on the attempt, not the manager, so a follower that wakes after a later
    /// flight finished cannot read that flight's result as its own. It carries the exception rather
    /// than a bool so a follower sees the leader's diagnostic. `succeeded` and `failure` are written
    /// once by the leader in run_leading_challenge() and read by followers after they wake, both
    /// under `state_mutex`; `expire_flight()` wakes them and is always called after that write.
    struct Flight {
        explicit Flight(const net::strand<net::any_io_executor>& flight_strand)
            : timer(flight_strand, net::steady_timer::time_point::max()) {}

        /// Never waited on to expire naturally: it parks at time_point::max() and is pushed into
        /// the past to release the followers. See expire_flight().
        net::steady_timer timer;
        /// Set when the leader has recorded its result below. A follower woken by close() or
        /// cancel() rather than by its leader finishing sees this false.
        bool finished{false};
        /// Set by cancel(). The leader's HTTP client is aborted, so it fails at its next network
        /// call, or a challenge's sooner, before it records its request and calls the consent
        /// callback. Its followers are released instead of waiting for it: a challenge's with an
        /// error, a refresh's reporting that no token was renewed.
        bool cancelled{false};
        bool succeeded{false};
        /// The leader's own exception, rethrown by every follower so they all report the same
        /// reason the leader does. Null when the flight finished without throwing.
        std::exception_ptr failure;
    };

    struct ChallengeOperation {
        std::shared_ptr<Impl> owner;
        /// The flight this operation leads, and the client it was started on. cancel() replaces
        /// the manager's client, so the operation keeps its own rather than reading the member.
        std::shared_ptr<Flight> flight;
        std::shared_ptr<OAuthHttpClient> http_client;
        std::shared_ptr<OAuthDiscoveryClient> discovery;
        BearerChallenge challenge;
        std::optional<ProtectedResourceMetadata> resource_metadata;
        std::optional<AuthServerMetadata> auth_metadata;
        ClientIdentityServerFacts facts;
        std::optional<OAuthClientInformation> stored_identity;
        OAuthClientInformation identity;
        nlohmann::json registration_response;
        AuthorizationRequest request;
        AuthorizationResponse response;
        OAuthConfig token_config;
        TokenResponse token;
    };

    struct RefreshOperation {
        std::shared_ptr<Impl> owner;
        std::shared_ptr<Flight> flight;
        std::shared_ptr<OAuthHttpClient> http_client;
        OAuthConfig token_config;
        TokenResponse stored_token;
        std::optional<TokenResponse> new_token;
    };

    Impl(const net::any_io_executor& executor, std::shared_ptr<TokenStore> store,
         OAuthAuthorizationConfig authorization_config, AuthorizationCallback authorization_callback)
        : token_store(std::move(store)),
          flight_strand(net::make_strand(executor)),
          executor(executor),
          config(std::move(authorization_config)),
          callback(std::move(authorization_callback)) {
        // A bare `client_id` is the shorthand form of injected credentials, so the two spellings
        // reach the same terminal decision instead of one of them quietly permitting registration.
        if (!config.client_identity.pre_registered && !config.client_id.empty()) {
            OAuthClientInformation injected;
            injected.client_id = config.client_id;
            injected.client_secret = config.client_secret;
            // Without this the issuer binding in `select_client_identity` is inert on the
            // shorthand path: an empty issuer would make every authorization server look like
            // the one these credentials belong to.
            injected.issuer = config.client_issuer;
            injected.source = ClientIdentitySource::ePreRegistered;
            config.client_identity.pre_registered = std::move(injected);
        }
        if (config.client_identity.metadata.redirect_uris.empty() && !config.redirect_uri.empty()) {
            config.client_identity.metadata.redirect_uris.push_back(config.redirect_uri);
        }
        http_client = make_client();
        discovery = std::make_shared<OAuthDiscoveryClient>(http_client);
    }

    /// A client for this manager's outbound requests, under its policy and resolver.
    [[nodiscard]] std::shared_ptr<OAuthHttpClient> make_client() const {
        auto client = std::make_shared<OAuthHttpClient>(executor);
        // One call rather than two, even here where the client has not yet issued a request: the
        // pair method is what a reader should find at a site that installs both. An empty
        // `host_resolver` leaves this client on the executor's system resolver.
        client->configure(config.policy, config.host_resolver, config.tls, config.proxy);
        return client;
    }

    static Task<bool> return_false() { co_return false; }

    /// Fails the same way return_false() succeeds: lazily, when the returned awaitable is awaited.
    /// handle_challenge() is a plain function returning an awaitable, so a bare `throw` in its body
    /// would fire when try_handle_challenge() is *called*, unlike the virtual it overrides, whose
    /// contract is the lazy one. It stays a plain function because making it a coroutine would add a
    /// frame and a suspension to a path that runs on every 401.
    static Task<bool> throw_closed() {
        throw std::runtime_error("OAuth authorization manager closed");
        co_return false;
    }

    /// Challenge scope is authoritative; `scopes_supported` is the fallback; otherwise no scope is
    /// requested at all. An explicit configured scope overrides both. The two sources are never
    /// merged with each other: the spec forbids assuming any set relationship between them. Scope
    /// already granted by an earlier authorization *is* merged in, because a 403 step-up challenge
    /// names only what the refused operation needed; only what `issuer` itself granted, since a
    /// scope means nothing to another authorization server.
    static std::optional<std::string> select_scope(const Impl& owner, const BearerChallenge& challenge,
                                                   const ProtectedResourceMetadata& resource,
                                                   const std::string& issuer) {
        if (owner.config.scope) {
            return owner.config.scope;
        }
        std::optional<std::string> selected;
        if (challenge.scope && !challenge.scope->empty()) {
            selected = challenge.scope;
        } else if (resource.scopes_supported && !resource.scopes_supported->empty()) {
            selected = join_scopes(*resource.scopes_supported);
        }
        std::optional<std::string> granted;
        {
            std::lock_guard lock(owner.state_mutex);
            granted = granted_scope_for(owner, issuer);
        }
        return union_scopes(selected, granted);
    }

    /// The scope `issuer` granted, held under `state_mutex`, which the caller holds.
    static std::optional<std::string> granted_scope_for(const Impl& owner, const std::string& issuer) {
        if (owner.granted && owner.granted->issuer == issuer) {
            return owner.granted->scope;
        }
        return std::nullopt;
    }

    /// Whether `challenge` is an insufficient-scope refusal whose every scope `issuer` already
    /// granted. Authorizing again would ask for what the token carries, so the user would be
    /// prompted for a grant the server has just shown it does not honor. Only a literal match
    /// counts: a scope the server grants under another spelling, or through a hierarchy, is not
    /// recognised here, and then the challenge is acted on as usual.
    static bool adds_no_new_scope(const Impl& owner, const BearerChallenge& challenge,
                                  const std::string& issuer) {
        if (challenge.error != "insufficient_scope" || !challenge.scope || challenge.scope->empty()) {
            return false;
        }
        std::optional<std::string> granted;
        {
            std::lock_guard lock(owner.state_mutex);
            granted = granted_scope_for(owner, issuer);
        }
        if (!granted) {
            return false;
        }
        const auto held = split_scopes(*granted);
        const auto wanted = split_scopes(*challenge.scope);
        return !wanted.empty() && std::all_of(wanted.begin(), wanted.end(), [&held](const auto& scope) {
            return std::find(held.begin(), held.end(), scope) != held.end();
        });
    }

    /// Record the four authorization-server facts client identity selection turns on.
    static ClientIdentityServerFacts server_facts(const AuthServerMetadata& auth_server) {
        ClientIdentityServerFacts facts;
        facts.issuer = auth_server.issuer;
        facts.client_id_metadata_document_supported =
            auth_server.client_id_metadata_document_supported.value_or(false);
        facts.registration_endpoint = auth_server.registration_endpoint;
        if (auth_server.scopes_supported) {
            facts.scopes_supported = *auth_server.scopes_supported;
        }
        return facts;
    }

    /// Load the credentials held for this issuer, discarding any entry that is not usable for it.
    static std::optional<OAuthClientInformation> load_stored_identity(
        const Impl& owner, const ClientIdentityServerFacts& facts) {
        if (!owner.config.credential_store || facts.issuer.empty()) {
            return std::nullopt;
        }
        auto stored = owner.config.credential_store->load(facts.issuer);
        if (!stored) {
            return std::nullopt;
        }
        // A stored entry that disagrees with its own key, or whose secret the server has already
        // retired, is worse than no entry at all: presenting it would either misbind the
        // credential or fail the exchange with a stale one.
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        if (stored->issuer != facts.issuer || stored->secret_expired(now)) {
            return std::nullopt;
        }
        return stored;
    }

    static Task<OAuthClientInformation> resolve_identity(
        std::shared_ptr<ChallengeOperation> operation) {
        auto& owner = *operation->owner;
        const auto decision = select_client_identity(owner.config.client_identity, operation->facts,
                                                     operation->stored_identity);
        switch (decision) {
            case ClientIdentityDecision::eUsePreRegistered: {
                auto identity = *owner.config.client_identity.pre_registered;
                identity.source = ClientIdentitySource::ePreRegistered;
                identity.issuer = operation->facts.issuer;
                co_return identity;
            }
            case ClientIdentityDecision::eUseClientIdMetadataDocument: {
                // The document URL is the client identifier itself, so there is nothing to
                // register and nothing to persist.
                OAuthClientInformation identity;
                identity.client_id = *owner.config.client_identity.client_metadata_url;
                identity.issuer = operation->facts.issuer;
                identity.source = ClientIdentitySource::eClientIdMetadataDocument;
                co_return identity;
            }
            case ClientIdentityDecision::eReuseStoredRegistration:
                co_return *operation->stored_identity;
            case ClientIdentityDecision::eRegisterDynamically:
                break;
            case ClientIdentityDecision::eUnavailable: {
                // `unavailable` covers several causes; say which one, because the unbound-secret
                // refusal is a configuration mistake the caller can fix and the generic message
                // would send them looking in the wrong place.
                const auto& injected = owner.config.client_identity.pre_registered;
                if (injected && !injected->client_id.empty() && injected->issuer.empty() &&
                    injected->client_secret && !injected->client_secret->empty()) {
                    // Both spellings are named, with the condition on each, because they are not
                    // interchangeable: `client_issuer` only fills in credentials given as client_id
                    // and client_secret, and the constructor refuses it next to a populated
                    // `client_identity.pre_registered`.
                    throw std::runtime_error(
                        "Injected client credentials carry a client_secret but name no issuer, so "
                        "they cannot be presented to authorization server " +
                        sanitize_for_diagnostics(operation->facts.issuer) +
                        "; set the issuer these credentials are bound to. Set "
                        "client_identity.pre_registered.issuer if you populated "
                        "client_identity.pre_registered yourself; client_issuer applies only to "
                        "credentials given as client_id and client_secret");
                }
                throw std::runtime_error("No client identity is available for authorization server " +
                                         sanitize_for_diagnostics(operation->facts.issuer));
            }
        }

        operation->registration_response = co_await operation->http_client->post_json(
            *operation->facts.registration_endpoint,
            build_registration_request(owner.config.client_identity.metadata, operation->facts));

        auto registered = operation->registration_response.get<OAuthClientInformation>();
        if (registered.client_id.empty()) {
            throw std::runtime_error("Client registration response omitted client_id");
        }
        registered.issuer = operation->facts.issuer;
        registered.source = ClientIdentitySource::eDynamicRegistration;
        if (owner.config.credential_store) {
            owner.config.credential_store->store(operation->facts.issuer, registered);
        }
        co_return registered;
    }

    static AuthorizationRequest build_request(const Impl& owner, const BearerChallenge& challenge,
                                              const ProtectedResourceMetadata& resource,
                                              const AuthServerMetadata& auth_server,
                                              const OAuthClientInformation& identity) {
        const auto pkce = generate_pkce_pair();

        AuthorizationRequest request;
        request.state = mcp::detail::generate_secure_session_id();
        request.code_verifier = pkce.code_verifier;
        request.code_challenge = pkce.code_challenge;
        // Recorded from the metadata document this client fetched itself. Response validation is
        // only as trustworthy as this value's provenance.
        request.issuer = auth_server.issuer;
        request.issuer_parameter_supported =
            auth_server.authorization_response_iss_parameter_supported.value_or(false);
        request.client_id = identity.client_id;
        request.redirect_uri = owner.config.redirect_uri;
        request.scope = select_scope(owner, challenge, resource, auth_server.issuer);
        // Added after selection so it never enters a step-up union as if a challenge had named it,
        // and only when the server lists it: a client must not assume a refresh token is issued.
        // A scope the application configured is used as it is.
        if (owner.config.request_offline_access && !owner.config.scope &&
            auth_server.scopes_supported &&
            std::find(auth_server.scopes_supported->begin(), auth_server.scopes_supported->end(),
                      "offline_access") != auth_server.scopes_supported->end()) {
            request.scope = union_scopes(request.scope, std::string("offline_access"));
        }
        // Defence in depth: `run_challenge` already rejected an unidentified resource before any
        // outbound request. Re-checking here is a pure string comparison that cannot fail on that
        // path, and keeps `build_request` correct if a second caller ever appears.
        require_resource_identifies_server(resource.resource, owner.config.server_url);
        request.resource = resource.resource;

        KeyValuePairList params = {
            {"response_type", "code"},
            {"client_id", request.client_id},
            {"redirect_uri", request.redirect_uri},
            {"state", request.state},
            {"code_challenge", request.code_challenge},
            {"code_challenge_method", pkce.challenge_method},
        };
        if (request.scope) {
            params.emplace_back("scope", *request.scope);
        }
        // RFC 8707: the resource indicator travels on the authorization request as well as the
        // token request.
        if (request.resource) {
            params.emplace_back("resource", *request.resource);
        }
        request.authorization_url =
            append_query(auth_server.authorization_endpoint, mcp::detail::form_body(params));
        return request;
    }

    static OAuthConfig build_token_config(const Impl& owner, const AuthServerMetadata& auth_server,
                                          const AuthorizationRequest& request,
                                          const OAuthClientInformation& identity) {
        OAuthConfig token_config;
        token_config.client_id = identity.client_id;
        token_config.client_secret = identity.client_secret;
        token_config.token_endpoint = auth_server.token_endpoint;
        token_config.authorization_endpoint = auth_server.authorization_endpoint;
        token_config.revocation_endpoint = auth_server.revocation_endpoint;
        token_config.redirect_uri = owner.config.redirect_uri;
        token_config.scope = request.scope;
        token_config.resource = request.resource;
        token_config.token_endpoint_auth_method = select_token_endpoint_auth_method(
            auth_server.token_endpoint_auth_methods_supported, identity.client_secret.has_value());
        return token_config;
    }

    static Task<bool> run_challenge(std::shared_ptr<ChallengeOperation> operation) {
        auto& owner = *operation->owner;

        // Both checks that decide whether this document may be acted on, handed to discovery as its
        // acceptor so they gate the cache write and also run on a cache hit: applied afterwards, a
        // refused document would already be cached for the full TTL. Order matters: a document that
        // both lists no authorization servers and carries a resource that is not ours must report the
        // missing authorization servers.
        auto accept = [owner = operation->owner](const ProtectedResourceMetadata& resource) {
            if (resource.authorization_servers.empty()) {
                throw std::runtime_error("Protected resource metadata listed no authorization servers");
            }
            // Refused before the first outbound request this document would drive, so a PRM that
            // does not identify our configured server cannot make us fetch the authorization server
            // metadata it names, register a client with that server, or persist those credentials.
            require_resource_identifies_server(resource.resource, owner->config.server_url);
        };

        // A challenge means the token in hand was refused, so the resource may have moved to another
        // authorization server since the metadata was cached. Reading it again is what lets the
        // client notice and authorize, with credentials of its own, at the server named now.
        operation->discovery->forget_protected_resource(owner.config.server_url,
                                                        operation->challenge.resource_metadata);

        // The challenge's own metadata URL wins over the well-known fallback order. A fetch that
        // fails because cancel() aborted it is reported as the cancellation it is.
        try {
            operation->resource_metadata = co_await operation->discovery->discover_protected_resource(
                owner.config.server_url, operation->challenge.resource_metadata, std::move(accept));

            operation->auth_metadata = co_await operation->discovery->discover_auth_server(
                operation->resource_metadata->authorization_servers.front());
        } catch (...) {
            std::lock_guard lock(owner.state_mutex);
            if (operation->flight->cancelled) {
                throw std::runtime_error("OAuth authorization attempt was cancelled");
            }
            throw;
        }
        // RFC 8414 §3.3: the issuer a metadata document claims MUST be the location it was fetched
        // from, compared byte-exact. Normalizing here would re-open the AS mix-up this closes.
        if (operation->auth_metadata->issuer.empty() ||
            operation->auth_metadata->issuer !=
                operation->resource_metadata->authorization_servers.front()) {
            throw std::runtime_error(
                "Authorization server metadata issuer does not identify the server it was fetched "
                "from");
        }
        if (operation->auth_metadata->authorization_endpoint.empty() ||
            operation->auth_metadata->token_endpoint.empty()) {
            throw std::runtime_error(
                "Authorization server metadata omitted an authorization or token endpoint");
        }
        require_browser_safe_authorization_endpoint(owner.config.policy,
                                                    operation->auth_metadata->authorization_endpoint);
        // MCP authorization: PKCE support is known only from the metadata, and a server that does
        // not list S256 in code_challenge_methods_supported must not receive an authorization
        // request.
        const auto& pkce_methods = operation->auth_metadata->code_challenge_methods_supported;
        if (!pkce_methods ||
            std::find(pkce_methods->begin(), pkce_methods->end(), "S256") == pkce_methods->end()) {
            throw std::runtime_error(
                "Authorization server metadata does not list S256 in "
                "code_challenge_methods_supported");
        }

        // After the issuer is known and before anything is registered or asked of the user.
        if (adds_no_new_scope(owner, operation->challenge, operation->auth_metadata->issuer)) {
            co_return false;
        }

        operation->facts = server_facts(*operation->auth_metadata);
        operation->stored_identity = load_stored_identity(owner, operation->facts);
        operation->identity = co_await resolve_identity(operation);

        operation->request = build_request(owner, operation->challenge, *operation->resource_metadata,
                                           *operation->auth_metadata, operation->identity);
        operation->token_config = build_token_config(owner, *operation->auth_metadata,
                                                     operation->request, operation->identity);
        {
            std::lock_guard lock(owner.state_mutex);
            // cancel() stops a leader through its aborted client, but one served discovery from the
            // cache with a pre-registered identity makes no network call before the consent
            // callback, so the abort alone would not keep it from prompting the user.
            if (operation->flight->cancelled) {
                throw std::runtime_error("OAuth authorization attempt was cancelled");
            }
            owner.last_request = operation->request;
            owner.last_identity = operation->identity;
        }

        operation->response = co_await owner.callback(operation->request);
        const auto validation =
            validate_authorization_response(operation->request, operation->response);
        if (!validation.accepted()) {
            throw std::runtime_error("Authorization response rejected: " + validation.message);
        }

        operation->token = co_await operation->http_client->exchange_code(
            operation->token_config, *operation->response.code, operation->request.code_verifier);
        {
            std::lock_guard lock(owner.state_mutex);
            // Recorded only now that this server issued a token. Until then the stored refresh token
            // still belongs to the server before it, and a refresh must keep going there: a step-up
            // that fails at consent or at the exchange must not point it at the new server.
            owner.token_config = operation->token_config;
            // What the server actually granted, falling back to what was asked for when the token
            // response stays silent (RFC 6749 §5.1 makes `scope` optional in that case).
            const auto& granted_scope =
                operation->token.scope ? operation->token.scope : operation->request.scope;
            owner.granted = GrantedScope{operation->request.issuer, granted_scope};
        }
        owner.token_store->store(owner.config.server_url, operation->token);
        co_return true;
    }

    /// Push `flight`'s deadline into the past, on `flight_strand` so it never races the timer's own
    /// operations: expires_at() and async_wait() both execute on the calling thread (the timer's
    /// executor governs only its completion handler), and await_in_flight() initiates its wait on the
    /// same strand.
    ///
    /// expires_at() rather than cancel(): it also moves the deadline, so a follower that has joined
    /// `flight` but not yet called async_wait() completes immediately instead of parking on
    /// time_point::max() forever. A null `flight` is a no-op.
    static void expire_flight(const net::strand<net::any_io_executor>& flight_strand,
                              std::shared_ptr<Flight> flight) {
        if (!flight) {
            return;
        }
        net::dispatch(flight_strand, [flight = std::move(flight)]() {
            flight->timer.expires_at(net::steady_timer::time_point::min());
        });
    }

#if BOOST_VERSION < 107700
    /// Before Boost 1.77, dispatch() cannot complete a use_awaitable bound to another executor, so
    /// await_in_flight() and await_refresh() start the wait on flight_strand here, with the awaiting
    /// coroutine's own handler: the wait is initiated on the strand and the coroutine still resumes
    /// on its own executor. A wait released by expire_flight() ends with operation_aborted, which
    /// use_awaitable throws; the flight's fields say how it ended.
    static Task<void> wait_on_flight_strand(net::strand<net::any_io_executor> flight_strand,
                                            std::shared_ptr<Flight> flight) {
        try {
            co_await net::async_initiate<const net::use_awaitable_t<>, void(boost::system::error_code)>(
                [&flight_strand, &flight](auto handler) {
                    net::dispatch(flight_strand, [flight, handler = std::move(handler)]() mutable {
                        flight->timer.async_wait(std::move(handler));
                    });
                },
                net::use_awaitable);
        } catch (const boost::system::system_error&) {
        }
    }
#endif

    /// Waiters share the leader's outcome instead of opening a second authorization flow, so a
    /// burst of concurrent requests that all hit the same challenge authorizes exactly once.
    static Task<bool> await_in_flight(std::shared_ptr<Impl> owner, std::shared_ptr<Flight> flight) {
        // async_wait() touches the timer synchronously on the thread that calls it, so it has to be
        // initiated on the same strand expire_flight() dispatches its expires_at() onto; the timer's
        // associated executor governs only where its completion handler runs.
        //
        // bind_executor() rather than a bare post(flight_strand, use_awaitable): a bare post leaves
        // the resumption on this coroutine's own executor and only happens to land inside the strand
        // when the two share one io_context.
#if BOOST_VERSION >= 107700
        co_await net::dispatch(net::bind_executor(owner->flight_strand, net::use_awaitable));
        boost::system::error_code ignored;
        co_await flight->timer.async_wait(net::redirect_error(net::use_awaitable, ignored));
#else
        co_await wait_on_flight_strand(owner->flight_strand, flight);
#endif

        // The dispatch above moves this coroutine onto flight_strand, but only until the next
        // suspension: an awaitable's executor is fixed when it is spawned, and the wait's completion
        // handler carries that executor, so the caller is back on its own executor here and no
        // explicit hop back is needed. That matters -- Client spawns its write onto its own strand
        // and SerializedTransportWriter builds another to serialise writes, and both would be
        // bypassed by a continuation left on flight_strand. Asserted by
        // AFollowerReleasedFromTheSingleFlightTimerResumesOnItsOwnStrand.
        std::exception_ptr failure;
        bool succeeded = false;
        {
            std::lock_guard lock(owner->state_mutex);
            // close() expires the same timer to release a follower parked here; a follower that
            // wakes because the manager closed gets a clear error rather than the misleading "not
            // authorized" that the leader's own outcome would otherwise report. Checked before the
            // flight's own result because a closed manager is the more specific answer: the flight
            // it joined may never have finished at all.
            if (owner->closed) {
                throw std::runtime_error("OAuth authorization manager closed");
            }
            if (flight->cancelled && !flight->finished) {
                throw std::runtime_error("OAuth authorization attempt was cancelled");
            }
            // Read off the flight this follower actually waited on. Reading a manager-wide field
            // here would let a late waker report a different attempt's outcome.
            //
            // `finished` is a guard rather than a case that arises today: the only things that
            // expire this timer are the leader recording its result, close() and cancel(), and the
            // checks above already took the other two. It keeps a wake-up added later from making a
            // follower report a flat "not authorized" that is indistinguishable from a real refusal.
            if (!flight->finished) {
                throw std::runtime_error(
                    "OAuth authorization attempt ended without recording an outcome");
            }
            failure = flight->failure;
            succeeded = flight->succeeded;
        }

        // Rethrown outside the lock: the leader's exception must not travel through a destructor
        // while state_mutex is held. Every follower rethrows the same exception_ptr, which is safe --
        // the object it refers to is shared and read-only. Nothing on the follower path swallows it:
        // run_write() catches (...) only to erase_pending() and rethrow.
        if (failure) {
            std::rethrow_exception(failure);
        }
        co_return succeeded;
    }

    static Task<bool> run_leading_challenge(std::shared_ptr<ChallengeOperation> operation) {
        auto owner = operation->owner;
        auto finished = operation->flight;
        bool succeeded = false;
        std::exception_ptr failure;
        try {
            succeeded = co_await run_challenge(std::move(operation));
        } catch (...) {
            failure = std::current_exception();
        }

        {
            std::lock_guard lock(owner->state_mutex);
            // After cancel() the manager may already be running a newer flight; that one is not
            // this leader's to end.
            if (owner->flight == finished) {
                owner->flight.reset();
            }
            if (finished) {
                // Recorded on the attempt itself, before any follower is woken, so each follower
                // reads the result of the flight it joined and gets the leader's own reason for it
                // rather than a bare false. The same exception_ptr is rethrown below.
                finished->succeeded = succeeded;
                finished->failure = failure;
                finished->finished = true;
            }
        }
        // A late joiner may have read the old `flight` out of the lock above just before this reset
        // and not yet be waiting on it (see expire_flight()'s comment); expires_at(), not cancel(),
        // is what still reaches it.
        expire_flight(owner->flight_strand, std::move(finished));
        if (failure) {
            std::rethrow_exception(failure);
        }
        co_return succeeded;
    }

    static Task<bool> handle_challenge(std::shared_ptr<Impl> owner, const std::string& header) {
        auto challenge = select_bearer_challenge(parse_www_authenticate(header));
        if (!challenge) {
            return return_false();
        }

        std::shared_ptr<Flight> joined;
        auto operation = std::make_shared<ChallengeOperation>();
        {
            std::lock_guard lock(owner->state_mutex);
            // Checked in the same critical section that reads or creates `flight`: close() also
            // takes this lock, so the two can never interleave as a new flight being created right
            // after close() already ran past it, unnoticed. (http_client's own sticky abort would
            // still stop that flight's first network call either way, but this keeps a closed
            // manager from starting one at all.)
            if (owner->closed) {
                return throw_closed();
            }
            if (owner->flight) {
                joined = owner->flight;
            } else {
                owner->flight = std::make_shared<Flight>(owner->flight_strand);
                operation->flight = owner->flight;
                operation->http_client = owner->http_client;
                operation->discovery = owner->discovery;
            }
        }
        if (joined) {
            return await_in_flight(std::move(owner), std::move(joined));
        }

        operation->owner = std::move(owner);
        operation->challenge = std::move(*challenge);
        return run_leading_challenge(std::move(operation));
    }

    static Task<bool> try_refresh(std::shared_ptr<Impl> owner) {
        auto operation = std::make_shared<RefreshOperation>();
        std::shared_ptr<Flight> joined;
        {
            std::lock_guard lock(owner->state_mutex);
            if (!owner->token_config || owner->closed) {
                return return_false();
            }
            // Concurrent refreshes share one exchange. A public client's refresh token may be
            // rotated on every use (RFC 9700 section 4.14.2), so a second exchange would present an
            // already invalidated token, and the authorization server answers that by revoking the
            // active refresh token as well.
            if (owner->refresh_flight) {
                joined = owner->refresh_flight;
            } else {
                owner->refresh_flight = std::make_shared<Flight>(owner->flight_strand);
                operation->flight = owner->refresh_flight;
                operation->http_client = owner->http_client;
                operation->token_config = *owner->token_config;
            }
        }
        if (joined) {
            return await_refresh(std::move(owner), std::move(joined));
        }
        operation->owner = std::move(owner);
        return run_leading_refresh(std::move(operation));
    }

    static Task<bool> run_leading_refresh(std::shared_ptr<RefreshOperation> operation) {
        auto owner = operation->owner;
        auto finished = operation->flight;
        bool succeeded = false;
        std::exception_ptr failure;
        try {
            succeeded = co_await run_refresh(std::move(operation));
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(owner->state_mutex);
            if (owner->refresh_flight == finished) {
                owner->refresh_flight.reset();
            }
            finished->succeeded = succeeded;
            finished->failure = failure;
            finished->finished = true;
        }
        expire_flight(owner->flight_strand, std::move(finished));
        if (failure) {
            std::rethrow_exception(failure);
        }
        co_return succeeded;
    }

    /// A refresh that joined another's reports that one's outcome. Woken without one, by close()
    /// or cancel(), it reports that no token was renewed.
    static Task<bool> await_refresh(std::shared_ptr<Impl> owner, std::shared_ptr<Flight> flight) {
        // Initiated on flight_strand and resumed on the caller's executor, as in await_in_flight().
#if BOOST_VERSION >= 107700
        co_await net::dispatch(net::bind_executor(owner->flight_strand, net::use_awaitable));
        boost::system::error_code ignored;
        co_await flight->timer.async_wait(net::redirect_error(net::use_awaitable, ignored));
#else
        co_await wait_on_flight_strand(owner->flight_strand, flight);
#endif

        std::exception_ptr failure;
        bool succeeded = false;
        {
            std::lock_guard lock(owner->state_mutex);
            if (flight->finished) {
                failure = flight->failure;
                succeeded = flight->succeeded;
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        co_return succeeded;
    }

    static Task<bool> run_refresh(std::shared_ptr<RefreshOperation> operation) {
        // Read only once this refresh leads the flight. A token read before that may already have
        // been rotated by a refresh that finished in between, and presenting it again is the reuse
        // the flight is there to prevent.
        auto stored = operation->owner->token_store->load(operation->owner->config.server_url);
        if (!stored || !stored->refresh_token) {
            co_return false;
        }
        operation->stored_token = std::move(*stored);
        try {
            operation->new_token = co_await operation->http_client->refresh_token(
                operation->token_config, *operation->stored_token.refresh_token);
            if (!operation->new_token->refresh_token) {
                operation->new_token->refresh_token = operation->stored_token.refresh_token;
            }
            operation->owner->token_store->store(operation->owner->config.server_url,
                                                 std::move(*operation->new_token));
            co_return true;
        } catch (const MetadataPolicyError&) {
            throw;
        } catch (...) {
            // A refresh that fails leaves the stored token as it is and reports false.
            co_return false;
        }
    }

    std::shared_ptr<TokenStore> token_store;
    std::shared_ptr<OAuthHttpClient> http_client;
    std::shared_ptr<OAuthDiscoveryClient> discovery;
    /// Serialises every access to `flight`. A boost::asio::steady_timer is not safe for concurrent
    /// use, and its two touch points -- expire_flight()'s expires_at() and await_in_flight()'s
    /// async_wait() -- both execute synchronously on whatever thread calls them, so nothing but a
    /// strand shared by both of them keeps them apart on a multi-threaded io_context. The timer is
    /// constructed on this strand as well, so its completion handlers run here too.
    net::strand<net::any_io_executor> flight_strand;
    net::any_io_executor executor;
    OAuthAuthorizationConfig config;
    AuthorizationCallback callback;
    mutable std::mutex state_mutex;
    std::optional<AuthorizationRequest> last_request;
    std::optional<OAuthClientInformation> last_identity;
    std::optional<OAuthConfig> token_config;
    /// The scope the last successful authorization was granted, with the issuer that granted it.
    struct GrantedScope {
        std::string issuer;
        std::optional<std::string> scope;
    };
    std::optional<GrantedScope> granted;
    /// Non-null exactly while one authorization flow is running; cancelling it releases the
    /// requests that coalesced onto it.
    std::shared_ptr<Flight> flight;
    /// Non-null exactly while one refresh exchange is running; concurrent refreshes wait on it.
    std::shared_ptr<Flight> refresh_flight;
    bool closed{false};

    /// Abort whatever this manager has in flight and release every parked follower with an error.
    ///
    /// `http_client->abort_pending()` unblocks a leader parked in an HTTP call (discovery,
    /// registration and token exchange share the one client) and, being sticky, fails a leader or a
    /// racing fresh flow at its first network call. `expire_flight()` releases followers directly,
    /// which also covers a leader parked in the application's consent callback, where there is no
    /// network state to cancel.
    static void close(const std::shared_ptr<Impl>& owner) {
        std::shared_ptr<Flight> flight;
        std::shared_ptr<Flight> refresh;
        std::shared_ptr<OAuthHttpClient> client;
        {
            std::lock_guard lock(owner->state_mutex);
            if (owner->closed) {
                return;
            }
            owner->closed = true;
            flight = owner->flight;
            refresh = owner->refresh_flight;
            client = owner->http_client;
        }
        client->abort_pending();
        expire_flight(owner->flight_strand, std::move(flight));
        expire_flight(owner->flight_strand, std::move(refresh));
    }

    /// End the authorization work in flight without closing the manager.
    ///
    /// The running flight and refresh are marked cancelled and their waiters released with an
    /// error. Their HTTP client is aborted, which stops the leader at its next network call, and is
    /// replaced with a new one, so later requests authorize and refresh as before. The discovery
    /// cache goes with the old client.
    static void cancel(const std::shared_ptr<Impl>& owner) {
        std::shared_ptr<Flight> flight;
        std::shared_ptr<Flight> refresh;
        std::shared_ptr<OAuthHttpClient> client;
        {
            std::lock_guard lock(owner->state_mutex);
            if (owner->closed || (!owner->flight && !owner->refresh_flight)) {
                return;
            }
            flight = std::exchange(owner->flight, nullptr);
            refresh = std::exchange(owner->refresh_flight, nullptr);
            if (flight) {
                flight->cancelled = true;
            }
            if (refresh) {
                refresh->cancelled = true;
            }
            client = std::exchange(owner->http_client, owner->make_client());
            owner->discovery = std::make_shared<OAuthDiscoveryClient>(owner->http_client);
        }
        client->abort_pending();
        expire_flight(owner->flight_strand, std::move(flight));
        expire_flight(owner->flight_strand, std::move(refresh));
    }
};

namespace {

/// A client metadata document URL must use https (the scheme is case-insensitive) and carry a
/// path, without a fragment, a username or password, or single-dot and double-dot path segments:
/// any of them would make it a different client identifier than the one the document names.
bool is_valid_client_metadata_url(const std::string& url) {
    constexpr std::string_view scheme = "https://";
    if (url.size() < scheme.size() || url.find('#') != std::string::npos ||
        !std::equal(scheme.begin(), scheme.end(), url.begin(), [](char expected, char actual) {
            return std::tolower(static_cast<unsigned char>(actual)) == expected;
        })) {
        return false;
    }
    const auto authority_end = url.find_first_of("/?", scheme.size());
    if (authority_end == std::string::npos || authority_end == scheme.size() ||
        url[authority_end] != '/') {
        return false;
    }
    if (url.find('@', scheme.size()) < authority_end) {
        return false;
    }
    const auto path_end = std::min(url.find('?', authority_end), url.size());
    const std::string_view path(url.data() + authority_end + 1, path_end - authority_end - 1);
    for (std::size_t start = 0; start <= path.size();) {
        const auto slash = path.find('/', start);
        const auto segment =
            path.substr(start, slash == std::string_view::npos ? slash : slash - start);
        if (segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

}  // namespace

OAuthAuthorizationManager::OAuthAuthorizationManager(const Executor& executor,
                                                     std::shared_ptr<TokenStore> token_store,
                                                     OAuthAuthorizationConfig config,
                                                     AuthorizationCallback callback) {
    if (!token_store || !callback) {
        throw std::invalid_argument(
            "OAuthAuthorizationManager requires a token store and an authorization callback");
    }
    if (config.client_identity.pre_registered &&
        (!config.client_id.empty() || config.client_secret || !config.client_issuer.empty())) {
        // The shorthand fields describe pre-registered credentials too, so with both present one
        // set would be ignored.
        throw std::invalid_argument(
            "OAuthAuthorizationConfig sets client_id, client_secret or client_issuer alongside "
            "client_identity.pre_registered; set only one of them");
    }
    const auto& metadata_url = config.client_identity.client_metadata_url;
    if (metadata_url && !metadata_url->empty() && !is_valid_client_metadata_url(*metadata_url)) {
        throw std::invalid_argument(
            "client_identity.client_metadata_url must be an https URL with a path and no "
            "fragment: " +
            sanitize_for_diagnostics(*metadata_url));
    }
    impl_ = std::make_shared<Impl>(executor.get(), std::move(token_store), std::move(config),
                                   std::move(callback));
}

std::string OAuthAuthorizationManager::get_access_token() const {
    const auto token = impl_->token_store->load(impl_->config.server_url);
    return token ? token->access_token : std::string{};
}

Task<bool> OAuthAuthorizationManager::try_refresh_token() { return Impl::try_refresh(impl_); }

Task<bool> OAuthAuthorizationManager::try_handle_challenge(const std::string& www_authenticate) {
    return Impl::handle_challenge(impl_, www_authenticate);
}

void OAuthAuthorizationManager::close() { Impl::close(impl_); }

void OAuthAuthorizationManager::cancel_pending() { Impl::cancel(impl_); }

std::optional<AuthorizationRequest> OAuthAuthorizationManager::last_authorization_request() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->last_request;
}

std::optional<OAuthClientInformation> OAuthAuthorizationManager::last_client_identity() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->last_identity;
}

}  // namespace mcp::auth
