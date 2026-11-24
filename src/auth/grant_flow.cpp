#include "grant_flow.hpp"

#include "authorization_common.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"

#include <algorithm>
#include <mcp/auth/challenge.hpp>
#include <mcp/core/async.hpp>
#include <stdexcept>
#include <utility>

namespace mcp::auth::detail {

namespace {

/// Whether every scope of @p wanted is in @p held.
bool covers(const std::optional<std::string>& held, const std::optional<std::string>& wanted) {
    if (!wanted || wanted->empty()) {
        return true;
    }
    if (!held) {
        return false;
    }
    const auto have = split_scopes(*held);
    const auto need = split_scopes(*wanted);
    return std::ranges::all_of(
        need, [&](const std::string& scope) { return std::ranges::find(have, scope) != have.end(); });
}

}  // namespace

struct GrantFlow::Operation {
    std::shared_ptr<GrantFlow> owner;
    std::shared_ptr<OAuthHttpClient> http_client;
    std::shared_ptr<OAuthDiscoveryClient> discovery;
    std::optional<BearerChallenge> challenge;
    bool reuse_cache{false};
    GrantTarget target;
    std::optional<ProtectedResourceMetadata> resource;
    std::optional<AuthServerMetadata> server;
    TokenResponse token;
};

GrantFlow::GrantFlow(net::any_io_executor executor, std::shared_ptr<TokenStore> token_store,
                     std::string server_url, MetadataFetchPolicy policy, HostResolver host_resolver,
                     std::shared_ptr<const TlsClientContext> tls, ProxyOptions proxy,
                     std::optional<std::string> scope)
    : executor_(std::move(executor)),
      token_store_(std::move(token_store)),
      server_url_(std::move(server_url)),
      policy_(std::move(policy)),
      host_resolver_(std::move(host_resolver)),
      tls_(std::move(tls)),
      proxy_(std::move(proxy)),
      scope_(std::move(scope)) {
    http_client_ = make_client();
    discovery_ = std::make_shared<OAuthDiscoveryClient>(http_client_);
}

std::shared_ptr<OAuthHttpClient> GrantFlow::make_client() const {
    auto client = std::make_shared<OAuthHttpClient>(executor_);
    client->configure(policy_, host_resolver_, tls_, proxy_);
    return client;
}

std::string GrantFlow::access_token() const {
    const auto token = token_store_->load(server_url_);
    return token ? token->access_token : std::string{};
}

Task<GrantIdentity> GrantFlow::identity() { co_return GrantIdentity{}; }

void GrantFlow::check_server(const AuthServerMetadata& /*server*/) const {}

void GrantFlow::record(const GrantTarget& target, const TokenResponse& token) {
    std::scoped_lock lock(mutex_);
    last_target_ = target;
    granted_scope_ = token.scope ? token.scope : target.scope;
    cache_[{target.server.issuer, target.identity.key}] = {token, target.scope};
}

Task<bool> GrantFlow::run(std::shared_ptr<Operation> operation) {
    auto& owner = *operation->owner;

    // The resource document is accepted before anything it names is fetched, so a document that
    // does not identify the configured server cannot steer discovery to another one.
    auto accept = [server_url = owner.server_url_](const ProtectedResourceMetadata& resource) {
        if (resource.authorization_servers.empty()) {
            throw std::runtime_error("Protected resource metadata listed no authorization servers");
        }
        if (resource.resource.empty() || !resource_identifies_server(resource.resource, server_url)) {
            throw std::runtime_error(
                "Protected resource metadata resource '" + sanitize_for_diagnostics(resource.resource) +
                "' does not identify server '" + sanitize_for_diagnostics(server_url) + "'");
        }
    };
    const auto challenge_metadata =
        operation->challenge ? operation->challenge->resource_metadata : std::nullopt;
    operation->resource = co_await operation->discovery->discover_protected_resource(
        owner.server_url_, challenge_metadata, std::move(accept));

    const auto& issuer = operation->resource->authorization_servers.front();
    operation->server = co_await operation->discovery->discover_auth_server(issuer);
    // RFC 8414 section 3.3: the issuer a metadata document claims is the location it was fetched
    // from, compared byte-exact.
    if (operation->server->issuer.empty() || operation->server->issuer != issuer) {
        throw std::runtime_error(
            "Authorization server metadata issuer does not identify the server it was fetched from");
    }
    if (operation->server->token_endpoint.empty()) {
        throw std::runtime_error("Authorization server metadata omitted a token endpoint");
    }
    owner.check_server(*operation->server);

    // The scope comes from the configuration, else the challenge, else what the resource
    // supports. A step-up challenge names only what the refused request needed, so what an earlier
    // grant covered is requested again with it.
    std::optional<std::string> scope = owner.scope_;
    if (!scope) {
        if (operation->challenge && operation->challenge->scope &&
            !operation->challenge->scope->empty()) {
            scope = operation->challenge->scope;
        } else if (operation->resource->scopes_supported &&
                   !operation->resource->scopes_supported->empty()) {
            scope = join_scopes(*operation->resource->scopes_supported);
        }
        if (operation->challenge && operation->challenge->error &&
            *operation->challenge->error == "insufficient_scope") {
            std::scoped_lock lock(owner.mutex_);
            scope = union_scopes(scope, owner.granted_scope_);
        }
    }

    // Awaited first: GCC 11 mishandles a co_await inside a braced initializer.
    auto identity = co_await owner.identity();
    operation->target = GrantTarget{.resource = *operation->resource,
                                    .server = *operation->server,
                                    .scope = scope,
                                    .identity = std::move(identity)};

    if (operation->reuse_cache) {
        std::scoped_lock lock(owner.mutex_);
        const auto cached =
            owner.cache_.find({operation->server->issuer, operation->target.identity.key});
        if (cached != owner.cache_.end() && !cached->second.first.is_expired() &&
            covers(cached->second.second, scope)) {
            owner.token_store_->store(owner.server_url_, cached->second.first);
            owner.last_target_ = operation->target;
            owner.granted_scope_ = cached->second.second;
            co_return true;
        }
    }

    operation->token = co_await owner.request_token(operation->http_client, operation->target);
    owner.record(operation->target, operation->token);
    owner.token_store_->store(owner.server_url_, operation->token);
    co_return true;
}

Task<bool> GrantFlow::handle_challenge(std::string www_authenticate) {
    const auto challenge = select_bearer_challenge(parse_www_authenticate(www_authenticate));
    if (!challenge) {
        co_return false;
    }
    auto operation = std::make_shared<Operation>();
    operation->owner = shared_from_this();
    operation->challenge = challenge;
    {
        std::scoped_lock lock(operation->owner->mutex_);
        if (operation->owner->closed_) {
            throw std::runtime_error("OAuth authorization closed");
        }
        operation->http_client = operation->owner->http_client_;
        operation->discovery = operation->owner->discovery_;
    }
    co_return co_await run(std::move(operation));
}

Task<bool> GrantFlow::authorize() {
    auto operation = std::make_shared<Operation>();
    operation->owner = shared_from_this();
    operation->reuse_cache = true;
    {
        std::scoped_lock lock(operation->owner->mutex_);
        if (operation->owner->closed_) {
            throw std::runtime_error("OAuth authorization closed");
        }
        operation->http_client = operation->owner->http_client_;
        operation->discovery = operation->owner->discovery_;
    }
    co_return co_await run(std::move(operation));
}

Task<bool> GrantFlow::run_renewal(std::shared_ptr<GrantFlow> self) {
    std::shared_ptr<OAuthHttpClient> client;
    GrantTarget target;
    {
        std::scoped_lock lock(self->mutex_);
        if (self->closed_ || !self->last_target_) {
            co_return false;
        }
        client = self->http_client_;
        target = *self->last_target_;
    }
    std::optional<TokenResponse> token;
    try {
        target.identity = co_await self->identity();
        token = co_await self->request_token(std::move(client), target);
    } catch (const std::exception&) {
        // A renewal that fails leaves the stored token as it is; the caller falls back to the
        // challenge path.
    }
    if (!token) {
        co_return false;
    }
    self->record(target, *token);
    self->token_store_->store(self->server_url_, std::move(*token));
    co_return true;
}

Task<bool> GrantFlow::renew() { return run_renewal(shared_from_this()); }

void GrantFlow::cancel_pending() {
    std::shared_ptr<OAuthHttpClient> stale;
    {
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return;
        }
        stale = std::exchange(http_client_, make_client());
        discovery_ = std::make_shared<OAuthDiscoveryClient>(http_client_);
    }
    stale->abort_pending();
}

void GrantFlow::close() {
    std::shared_ptr<OAuthHttpClient> client;
    {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        client = http_client_;
    }
    client->abort_pending();
}

}  // namespace mcp::auth::detail
