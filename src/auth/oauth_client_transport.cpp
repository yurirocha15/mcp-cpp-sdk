#include <mcp/auth/oauth.hpp>
#include <mcp/core/async.hpp>

#include "mcp/detail/json/peer_json.hpp"
#include "mcp/detail/net/network_failure.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>
#include <mcp/transport/http_client.hpp>

#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace jsonrpc = mcp::constants::jsonrpc;

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

namespace beast = boost::beast;
namespace http = beast::http;

struct OAuthClientTransport::Impl {
    using Clock = std::chrono::steady_clock;
    using PendingOrder = std::list<std::string>;

    struct PendingRequest {
        std::shared_ptr<const std::string> wire;
        Clock::time_point expires_at;
        PendingOrder::iterator order_position;
        std::size_t retained_bytes{0};
        std::uint64_t generation{0};
    };

    using PendingMap = std::unordered_map<std::string, PendingRequest>;

    struct WriteOperation {
        std::shared_ptr<Impl> owner;
        std::shared_ptr<const std::string> original;
        std::shared_ptr<const std::string> outgoing;
        std::optional<std::string> request_key;
        std::optional<std::uint64_t> request_generation;
        std::exception_ptr write_error;
        std::string authenticate_challenge;
        bool authentication_challenge{false};
        bool refresh_allowed{false};
        // Set once the refresh token has been spent on this request. A 401 that comes back after a
        // successful refresh means the renewed token is not accepted either, so it goes to the
        // challenge instead of refreshing again.
        bool refreshed{false};
        // Set once a 401 has driven a full authorization for this request. A 401 that comes back
        // after it means the resource server does not accept the new token either, and asking the
        // user to authorize again would not change that.
        bool authorized_after_unauthorized{false};
        int authorization_attempts{0};
        std::size_t step_ups{0};
    };

    /// Reports that `operation`'s write is waiting for the authenticator to handle a challenge, and
    /// that it has stopped, to the observer the caller of the transport installed. It holds only
    /// shared pointers, so it is safe to keep in a coroutine frame.
    class InteractionScope {
       public:
        InteractionScope(std::shared_ptr<Impl> owner, std::shared_ptr<WriteOperation> operation)
            : owner_(std::move(owner)), operation_(std::move(operation)) {
            notify(true);
        }
        InteractionScope(const InteractionScope&) = delete;
        InteractionScope& operator=(const InteractionScope&) = delete;
        ~InteractionScope() { notify(false); }

       private:
        void notify(bool waiting) noexcept {
            if (!operation_->request_key) {
                return;
            }
            try {
                ITransport::InteractionObserver observer;
                {
                    std::lock_guard lock(owner_->observer_mutex);
                    observer = owner_->interaction_observer;
                }
                if (observer) {
                    observer(*operation_->request_key, waiting);
                }
            } catch (...) {
                // An observer that throws must not turn a finished write into a crash.
            }
        }

        std::shared_ptr<Impl> owner_;
        std::shared_ptr<WriteOperation> operation_;
    };

    /// At most three authorization challenges are honoured per logical request, so a server that
    /// keeps refusing a scope it will never grant cannot drive an unbounded authorization loop.
    static constexpr int g_max_authorization_challenges = 3;

    explicit Impl(std::shared_ptr<ITransport> wrapped,
                  std::shared_ptr<Authenticator> token_authenticator,
                  OAuthClientTransportOptions replay_options)
        : inner(std::move(wrapped)),
          authenticator(std::move(token_authenticator)),
          options(std::move(replay_options)) {}

    static void erase_pending_locked(Impl& owner, PendingMap::iterator iter) {
        owner.pending_bytes -= iter->second.retained_bytes;
        owner.pending_order.erase(iter->second.order_position);
        owner.pending_requests.erase(iter);
    }

    static void prune_expired_locked(Impl& owner, Clock::time_point now) {
        while (!owner.pending_order.empty()) {
            const auto iter = owner.pending_requests.find(owner.pending_order.front());
            if (iter == owner.pending_requests.end()) {
                owner.pending_order.pop_front();
                continue;
            }
            if (iter->second.expires_at > now) {
                break;
            }
            erase_pending_locked(owner, iter);
        }
    }

    static void evict_oldest_locked(Impl& owner) {
        if (owner.pending_order.empty()) {
            return;
        }
        const auto iter = owner.pending_requests.find(owner.pending_order.front());
        if (iter == owner.pending_requests.end()) {
            owner.pending_order.pop_front();
            return;
        }
        erase_pending_locked(owner, iter);
    }

    static std::optional<std::size_t> retained_size(const Impl& owner, std::string_view key,
                                                    std::string_view wire) {
        auto remaining = owner.options.max_pending_request_bytes;
        if (wire.size() > remaining) {
            return std::nullopt;
        }
        remaining -= wire.size();
        for (int key_copy = 0; key_copy < 2; ++key_copy) {
            if (key.size() > remaining) {
                return std::nullopt;
            }
            remaining -= key.size();
        }
        return owner.options.max_pending_request_bytes - remaining;
    }

    static std::optional<std::uint64_t> remember_request(
        const std::shared_ptr<Impl>& owner, const std::string& key,
        const std::shared_ptr<const std::string>& wire) {
        std::lock_guard lock(owner->pending_mutex);
        const auto now = Clock::now();
        prune_expired_locked(*owner, now);

        if (owner->closed || owner->options.max_pending_requests == 0 ||
            owner->options.pending_request_ttl <= std::chrono::milliseconds::zero()) {
            return std::nullopt;
        }
        const auto bytes = retained_size(*owner, key, *wire);
        if (!bytes) {
            return std::nullopt;
        }

        if (const auto existing = owner->pending_requests.find(key);
            existing != owner->pending_requests.end()) {
            erase_pending_locked(*owner, existing);
        }
        while (owner->pending_requests.size() >= owner->options.max_pending_requests ||
               *bytes > owner->options.max_pending_request_bytes - owner->pending_bytes) {
            evict_oldest_locked(*owner);
        }

        ++owner->next_generation;
        if (owner->next_generation == 0) {
            ++owner->next_generation;
        }
        const auto generation = owner->next_generation;
        const auto max_ttl =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
        const auto expires_at = owner->options.pending_request_ttl >= max_ttl
                                    ? Clock::time_point::max()
                                    : now + owner->options.pending_request_ttl;

        owner->pending_order.push_back(key);
        try {
            owner->pending_requests.emplace(
                key, PendingRequest{wire, expires_at, std::prev(owner->pending_order.end()), *bytes,
                                    generation});
        } catch (...) {
            owner->pending_order.pop_back();
            throw;
        }
        owner->pending_bytes += *bytes;
        return generation;
    }

    static std::string inject_token(const std::shared_ptr<Impl>& owner, JSONRPCRequest request) {
        const auto token = owner->authenticator->get_access_token();
        if (token.empty()) {
            return mcp::detail::dump_json(nlohmann::json(request));
        }
        if (!request.params) {
            request.params = nlohmann::json::object();
        }
        auto& params = *request.params;
        if (!params.is_object()) {
            params = nlohmann::json::object();
        }
        if (!params.contains("_meta")) {
            params["_meta"] = nlohmann::json::object();
        }
        params["_meta"]["auth_token"] = token;
        return mcp::detail::dump_json(nlohmann::json(request));
    }

    static std::shared_ptr<WriteOperation> prepare_write(std::shared_ptr<Impl> owner,
                                                         std::shared_ptr<const std::string> original) {
        auto operation = std::make_shared<WriteOperation>();
        operation->owner = std::move(owner);
        operation->original = std::move(original);
        auto outgoing = std::make_shared<std::string>(*operation->original);

        try {
            const auto json_message = nlohmann::json::parse(*operation->original);
            const auto message = json_message.get<JSONRPCMessage>();
            if (const auto* request = std::get_if<JSONRPCRequest>(&message)) {
                operation->request_key = request->id.correlation_key();
                if (!operation->owner->uses_http_authorization_header) {
                    *outgoing = inject_token(operation->owner, *request);
                }
            }
        } catch (const std::exception&) {
            // Non-JSON messages pass through unchanged.
        }

        operation->outgoing = std::move(outgoing);
        if (operation->request_key) {
            operation->request_generation =
                remember_request(operation->owner, *operation->request_key, operation->original);
        }
        return operation;
    }

    static Task<void> write(std::shared_ptr<Impl> owner, std::shared_ptr<const std::string> original) {
        return run_write(prepare_write(std::move(owner), std::move(original)));
    }

    static void erase_pending(const std::shared_ptr<WriteOperation>& operation) {
        if (!operation->request_key || !operation->request_generation) {
            return;
        }
        std::lock_guard lock(operation->owner->pending_mutex);
        const auto iter = operation->owner->pending_requests.find(*operation->request_key);
        if (iter != operation->owner->pending_requests.end() &&
            iter->second.generation == *operation->request_generation) {
            erase_pending_locked(*operation->owner, iter);
        }
    }

    /// Whether a 403's challenge asks for more scope. Any other 403 is an authorization decision a
    /// new grant cannot change, so it is reported to the caller rather than asking the user again.
    static bool is_insufficient_scope(const std::string& www_authenticate) {
        if (www_authenticate.empty()) {
            return false;
        }
        const auto challenge = select_bearer_challenge(parse_www_authenticate(www_authenticate));
        return challenge && challenge->error == "insufficient_scope";
    }

    /// close() cancels only the authenticator work already in flight, so a write that is still in
    /// its retry loop checks for it before asking the authenticator for anything new. Otherwise a
    /// refresh that close() cut short would hand over to an interactive authorization for a
    /// transport nobody can use any more.
    static bool is_closed(Impl& owner) {
        std::lock_guard lock(owner.pending_mutex);
        return owner.closed;
    }

    static void throw_if_closed(Impl& owner) {
        if (is_closed(owner)) {
            throw TransportError(TransportError::Kind::eClosed, "OAuthClientTransport is closed");
        }
    }

    /// Await `attempt`, which the authenticator registered when it was called. This is the second
    /// check: a close() from another thread that landed between throw_if_closed() and that call
    /// found nothing to cancel, so the write cancels the attempt itself. It still awaits it rather
    /// than dropping it. The authenticator may hold state that only the attempt's own completion
    /// clears -- OAuthAuthorizationManager's flight, which every later caller would otherwise join
    /// and wait on -- and an authenticator whose cancel_pending() is the default no-op cancels
    /// nothing.
    static Task<bool> await_unless_closed(std::shared_ptr<Impl> owner, Task<bool> attempt) {
        const bool closed = is_closed(*owner);
        if (closed) {
            owner->authenticator->cancel_pending();
        }
        bool authorized = false;
        try {
            authorized = co_await std::move(attempt);
        } catch (...) {
            if (!closed) {
                throw;
            }
        }
        if (closed) {
            throw TransportError(TransportError::Kind::eClosed, "OAuthClientTransport is closed");
        }
        co_return authorized;
    }

    static Task<void> run_write(std::shared_ptr<WriteOperation> operation) {
        while (true) {
            operation->write_error = nullptr;
            operation->authentication_challenge = false;
            operation->refresh_allowed = false;
            operation->authenticate_challenge.clear();

            try {
                co_await operation->owner->inner->write_message(*operation->outgoing);
            } catch (const mcp::HttpStatusError& error) {
                // 401 says the request was not authenticated at all. A 403 whose challenge carries
                // error="insufficient_scope" is the step-up case: the token is valid but its scope
                // does not cover this operation, so the challenge drives a fresh authorization
                // rather than a refresh.
                const auto unauthorized =
                    error.status() == static_cast<unsigned int>(http::status::unauthorized);
                const auto stepped_up =
                    error.status() == static_cast<unsigned int>(http::status::forbidden) &&
                    is_insufficient_scope(error.authenticate_challenge());
                operation->authentication_challenge =
                    operation->owner->uses_http_authorization_header && (unauthorized || stepped_up);
                if (operation->authentication_challenge) {
                    operation->authenticate_challenge = error.authenticate_challenge();
                    operation->refresh_allowed = unauthorized;
                }
                operation->write_error = std::current_exception();
            } catch (...) {
                operation->write_error = std::current_exception();
            }

            if (!operation->write_error) {
                co_return;
            }
            if (!operation->authentication_challenge ||
                operation->authorization_attempts >= g_max_authorization_challenges ||
                (operation->refresh_allowed && operation->authorized_after_unauthorized)) {
                break;
            }
            // A step-up asks for consent to a wider scope, which a server could ask for without
            // end; the application sets how many one request may cost.
            if (!operation->refresh_allowed) {
                if (operation->step_ups >= operation->owner->options.max_step_ups_per_request) {
                    break;
                }
                ++operation->step_ups;
            }
            ++operation->authorization_attempts;

            bool authorized = false;
            try {
                // A 401 first tries to renew the existing grant, which needs no user interaction; a
                // challenge that names its metadata drives a full authorization exchange only when
                // renewal is unavailable or fails. Renewal is pointless against an
                // insufficient-scope refusal, so a 403 goes straight to the challenge.
                if (operation->refresh_allowed && !operation->refreshed) {
                    operation->refreshed = true;
                    throw_if_closed(*operation->owner);
                    authorized = co_await await_unless_closed(
                        operation->owner, operation->owner->authenticator->try_refresh_token());
                }
                if (!authorized && !operation->authenticate_challenge.empty()) {
                    operation->authorized_after_unauthorized =
                        operation->authorized_after_unauthorized || operation->refresh_allowed;
                    throw_if_closed(*operation->owner);
                    InteractionScope waiting_for_person(operation->owner, operation);
                    authorized = co_await await_unless_closed(
                        operation->owner, operation->owner->authenticator->try_handle_challenge(
                                              operation->authenticate_challenge));
                }
            } catch (...) {
                erase_pending(operation);
                // A network error on the way to the authorization server leaves as the
                // transport's; the authenticator's own failures go on as they are.
                std::rethrow_exception(mcp::detail::as_transport_failure(std::current_exception(),
                                                                         "OAuthClientTransport"));
            }
            if (!authorized) {
                break;
            }
        }

        erase_pending(operation);
        std::rethrow_exception(operation->write_error);
    }

    static std::shared_ptr<const std::string> take_retry_wire(const std::shared_ptr<Impl>& owner,
                                                              std::string_view raw) {
        try {
            const auto json_message = mcp::detail::parse_peer_json(raw);
            if (json_message.is_discarded()) {
                return {};
            }
            const auto message = json_message.get<JSONRPCMessage>();

            std::optional<std::string> key;
            bool unauthorized = false;
            if (const auto* result = std::get_if<JSONRPCResultResponse>(&message)) {
                key = result->id.correlation_key();
            } else if (const auto* error = std::get_if<JSONRPCErrorResponse>(&message);
                       error && error->id) {
                key = error->id->correlation_key();
                unauthorized = error->error.code == jsonrpc::g_UNAUTHORIZED;
            }
            if (!key) {
                return {};
            }

            std::lock_guard lock(owner->pending_mutex);
            prune_expired_locked(*owner, Clock::now());
            const auto iter = owner->pending_requests.find(*key);
            if (iter == owner->pending_requests.end()) {
                return {};
            }
            auto wire = unauthorized ? iter->second.wire : std::shared_ptr<const std::string>{};
            erase_pending_locked(*owner, iter);
            return wire;
        } catch (const std::exception&) {
            return {};
        }
    }

    struct ReadOperation {
        explicit ReadOperation(std::shared_ptr<Impl> state) : owner(std::move(state)) {}

        std::shared_ptr<Impl> owner;
        std::string raw;
        std::shared_ptr<const std::string> retry_wire;
    };

    static Task<std::string> read(std::shared_ptr<Impl> owner) {
        auto operation = std::make_shared<ReadOperation>(std::move(owner));
        return run_read(std::move(operation));
    }

    static Task<std::string> run_read(std::shared_ptr<ReadOperation> operation) {
        operation->raw = co_await operation->owner->inner->read_message();
        operation->retry_wire = take_retry_wire(operation->owner, operation->raw);
        bool refreshed = false;
        if (operation->retry_wire) {
            refreshed = co_await operation->owner->authenticator->try_refresh_token();
        }
        if (refreshed) {
            co_await write(operation->owner, operation->retry_wire);
            operation->raw = co_await operation->owner->inner->read_message();
            (void)take_retry_wire(operation->owner, operation->raw);
        }
        co_return operation->raw;
    }

    std::shared_ptr<ITransport> inner;
    std::shared_ptr<Authenticator> authenticator;
    OAuthClientTransportOptions options;
    std::mutex pending_mutex;
    PendingMap pending_requests;
    PendingOrder pending_order;
    std::size_t pending_bytes{0};
    std::uint64_t next_generation{0};
    bool closed{false};
    bool uses_http_authorization_header{false};
    std::mutex observer_mutex;
    ITransport::InteractionObserver interaction_observer;
};

OAuthClientTransport::OAuthClientTransport(std::shared_ptr<ITransport> inner,
                                           std::shared_ptr<Authenticator> authenticator)
    : OAuthClientTransport(std::move(inner), std::move(authenticator), {}) {}

OAuthClientTransport::OAuthClientTransport(std::shared_ptr<ITransport> inner,
                                           std::shared_ptr<Authenticator> authenticator,
                                           OAuthClientTransportOptions options) {
    if (!inner || !authenticator) {
        throw std::invalid_argument(
            "OAuthClientTransport requires an inner transport and authenticator");
    }

    impl_ = std::make_shared<Impl>(std::move(inner), std::move(authenticator), std::move(options));
    if (const auto http_transport = std::dynamic_pointer_cast<mcp::HttpClientTransport>(impl_->inner)) {
        std::weak_ptr<Authenticator> weak_authenticator = impl_->authenticator;
        http_transport->set_bearer_token_provider([weak_authenticator]() {
            const auto active_authenticator = weak_authenticator.lock();
            return active_authenticator ? active_authenticator->get_access_token() : std::string{};
        });
        impl_->uses_http_authorization_header = true;
    }
}

Task<std::string> OAuthClientTransport::read_message() { return Impl::read(impl_); }

bool OAuthClientTransport::overlapping_writes() const noexcept {
    return impl_->inner->overlapping_writes();
}

void OAuthClientTransport::set_interaction_observer(InteractionObserver observer) {
    std::lock_guard lock(impl_->observer_mutex);
    impl_->interaction_observer = std::move(observer);
}

Task<void> OAuthClientTransport::write_message(std::string_view message) {
    return Impl::write(impl_, std::make_shared<const std::string>(message));
}

void OAuthClientTransport::close() {
    {
        std::lock_guard lock(impl_->pending_mutex);
        if (impl_->closed) {
            return;
        }
        impl_->closed = true;
        impl_->pending_requests.clear();
        impl_->pending_order.clear();
        impl_->pending_bytes = 0;
    }
    // Releases anything the authenticator has parked -- an in-flight discovery/token exchange, or a
    // follower waiting on a single-flight timer -- before the inner transport is torn down, so a
    // write() still working its way through the authorization retry loop fails promptly instead of
    // outliving this call. Only that work is cancelled: the authenticator can be shared with other
    // transports and outlive this one, so closing it is left to its owner.
    impl_->authenticator->cancel_pending();
    impl_->inner->close();
}

}  // namespace mcp::auth
