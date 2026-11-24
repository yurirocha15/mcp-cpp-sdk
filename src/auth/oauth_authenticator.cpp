#include <mcp/auth/oauth_authenticator.hpp>
#include <mcp/core/async.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

struct OAuthAuthenticator::Impl {
    struct RefreshOperation {
        std::shared_ptr<Impl> owner;
        TokenResponse stored_token;
        std::optional<TokenResponse> new_token;
        /// The scope current when the refresh started; cancel_pending() aborts it.
        std::optional<OAuthHttpClientScope> scope;
    };

    Impl(std::shared_ptr<TokenStore> store, std::shared_ptr<OAuthHttpClient> client,
         OAuthConfig oauth_config, std::string url)
        : token_store(std::move(store)),
          oauth_client(std::move(client)),
          scope(oauth_client->make_scope()),
          config(std::move(oauth_config)),
          server_url(std::move(url)) {}

    static Task<bool> return_false() { co_return false; }

    static Task<bool> try_refresh(std::shared_ptr<Impl> owner) {
        auto stored = owner->token_store->load(owner->server_url);
        if (!stored || !stored->refresh_token) {
            return return_false();
        }

        auto operation =
            std::make_shared<RefreshOperation>(RefreshOperation{.owner = std::move(owner),
                                                                .stored_token = std::move(*stored),
                                                                .new_token = std::nullopt,
                                                                .scope = std::nullopt});
        {
            std::scoped_lock lock(operation->owner->scope_mutex);
            operation->scope = operation->owner->scope;
        }
        return run_refresh(std::move(operation));
    }

    /// Abort the refreshes running now and issue later ones through a fresh scope.
    void cancel_pending() {
        std::optional<OAuthHttpClientScope> cancelled;
        {
            std::scoped_lock lock(scope_mutex);
            if (closed) {
                return;
            }
            cancelled = std::exchange(scope, oauth_client->make_scope());
        }
        cancelled->abort();
    }

    /// Abort this authenticator's scope for good. The scope is not replaced, so every later
    /// refresh is refused.
    void close() {
        std::optional<OAuthHttpClientScope> current;
        {
            std::scoped_lock lock(scope_mutex);
            closed = true;
            current = scope;
        }
        current->abort();
    }

    static Task<bool> run_refresh(std::shared_ptr<RefreshOperation> operation) {
        try {
            operation->new_token = co_await operation->scope->refresh_token(
                operation->owner->config, *operation->stored_token.refresh_token);
            if (!operation->new_token->refresh_token) {
                operation->new_token->refresh_token = operation->stored_token.refresh_token;
            }
            operation->owner->token_store->store(operation->owner->server_url,
                                                 std::move(*operation->new_token));
            co_return true;
        } catch (...) {
            // A refresh that fails leaves the stored token as it is and reports false.
            co_return false;
        }
    }

    std::shared_ptr<TokenStore> token_store;
    /// Kept so the client outlives this authenticator, even though every request goes through the
    /// scope below.
    std::shared_ptr<OAuthHttpClient> oauth_client;
    /// This authenticator's own slice of the client. The client is supplied by the application and
    /// may be shared with other authenticators for other servers, so close() must end this
    /// authenticator's work without ending theirs. cancel_pending() swaps in a fresh one, so the
    /// member is read and replaced under scope_mutex.
    mutable std::mutex scope_mutex;
    OAuthHttpClientScope scope;
    bool closed{false};
    OAuthConfig config;
    std::string server_url;
};

namespace {

/// Impl's constructor takes a scope off the client, so a null one faults before the object exists.
/// Refuse it the way OAuthClientTransport's constructor refuses its own null arguments.
std::shared_ptr<OAuthHttpClient> require_http_client(std::shared_ptr<OAuthHttpClient> client) {
    if (!client) {
        throw std::invalid_argument("OAuthAuthenticator requires an OAuth HTTP client");
    }
    return client;
}

}  // namespace

OAuthAuthenticator::OAuthAuthenticator(std::shared_ptr<TokenStore> token_store,
                                       std::shared_ptr<OAuthHttpClient> oauth_client,
                                       OAuthConfig config, std::string server_url)
    : impl_(std::make_shared<Impl>(std::move(token_store), require_http_client(std::move(oauth_client)),
                                   std::move(config), std::move(server_url))) {}

std::string OAuthAuthenticator::get_access_token() const {
    const auto token = impl_->token_store->load(impl_->server_url);
    return token ? token->access_token : std::string{};
}

Task<bool> OAuthAuthenticator::try_refresh_token() { return Impl::try_refresh(impl_); }

void OAuthAuthenticator::store_token(TokenResponse token) {
    impl_->token_store->store(impl_->server_url, std::move(token));
}

/// Ends only this authenticator's requests. Do not reach for abort_pending() here: it latches the
/// whole client irreversibly, which is correct for a client its owner created and wrong for one the
/// application supplied and may share. Two authenticators for two servers on one client would then
/// silently disable each other -- run_refresh() reports a failed refresh as a plain `false`, so the
/// survivor gets no error, only tokens that quietly stop renewing.
void OAuthAuthenticator::close() { impl_->close(); }

void OAuthAuthenticator::cancel_pending() { impl_->cancel_pending(); }

}  // namespace mcp::auth
