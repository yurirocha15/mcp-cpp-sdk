#include <algorithm>
#include <mcp/auth/oauth_http_client.hpp>
#include <mcp/core/async.hpp>

#include "mcp/detail/async/spawn_awaitable.hpp"
#include "mcp/detail/auth/oauth_internal.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/json/peer_json.hpp"
#include "mcp/detail/net/http_connect.hpp"
#include "mcp/detail/net/socket_deadline.hpp"
#include "mcp/detail/net/tls.hpp"
#include "mcp/detail/text/encoding.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

namespace {

/// Flattens peer-controlled text -- control characters, bidi overrides and ill-formed UTF-8 -- and
/// caps it, so a value a peer chose cannot forge or reorder a line of a diagnostic. Every throw
/// site in this file that interpolates a value originating from a peer must route it through here;
/// `MetadataPolicyError` does the same inside its own constructor, so its throw sites do not repeat
/// it. This is a convention, not something the compiler checks: a new throw site that forgets it
/// re-opens the hole silently.
using detail::sanitize_for_diagnostics;

/// Does `reference` begin with an RFC 3986 §3.1 scheme, `ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`
/// followed by ':', before any '/', '?' or '#'? Only then is it an absolute URI; "://" further on,
/// say in a query, does not make it one.
bool has_uri_scheme(std::string_view reference) {
    const auto colon = reference.find_first_of(":/?#");
    if (colon == std::string_view::npos || colon == 0 || reference[colon] != ':' ||
        std::isalpha(static_cast<unsigned char>(reference.front())) == 0) {
        return false;
    }
    return std::all_of(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(colon),
                       [](char ch) {
                           return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '+' ||
                                  ch == '-' || ch == '.';
                       });
}

/// Resolve a `Location` header against the URL that produced it.
std::string resolve_redirect_target(const std::string& base, const std::string& location) {
    if (has_uri_scheme(location)) {
        return location;
    }
    const auto origin = metadata_url_origin(base);
    if (origin.empty()) {
        return location;
    }
    if (!location.empty() && location.front() == '/') {
        return origin + location;
    }

    auto directory = base;
    if (const auto query = directory.find_first_of("?#"); query != std::string::npos) {
        directory.erase(query);
    }
    const auto last_slash = directory.rfind('/');
    if (last_slash == std::string::npos || last_slash < origin.size()) {
        return origin + "/" + location;
    }
    return directory.substr(0, last_slash + 1) + location;
}

bool is_redirect_status(unsigned int status) {
    return status == static_cast<unsigned int>(http::status::moved_permanently) ||
           status == static_cast<unsigned int>(http::status::found) ||
           status == static_cast<unsigned int>(http::status::see_other) ||
           status == static_cast<unsigned int>(http::status::temporary_redirect) ||
           status == static_cast<unsigned int>(http::status::permanent_redirect);
}

}  // namespace

namespace detail {

/// One scope's abort latch, owned jointly by the scope object and by every exchange issued
/// through it. `aborted` is guarded by the owning client's `active_mutex`, the same mutex the
/// client-wide latch is read and written under, so the scoped and unscoped checks keep the
/// single synchronisation discipline they have always had.
struct OAuthScopeState {
    OAuthScopeState() { live().fetch_add(1, std::memory_order_relaxed); }
    OAuthScopeState(const OAuthScopeState&) = delete;
    OAuthScopeState& operator=(const OAuthScopeState&) = delete;
    ~OAuthScopeState() { live().fetch_sub(1, std::memory_order_relaxed); }

    bool aborted{false};

    /// How many latches exist right now, across every client in the process. Instrumentation for
    /// the retention test: the count returning to its starting value after a churn of scopes is
    /// what says the latches were released rather than parked somewhere.
    static std::atomic<std::size_t>& live() {
        static std::atomic<std::size_t> s_count{0};
        return s_count;
    }
    static std::size_t live_count() { return live().load(std::memory_order_relaxed); }
};

}  // namespace detail

struct OAuthHttpClient::Impl : std::enable_shared_from_this<OAuthHttpClient::Impl> {
    struct ParsedUrl {
        std::string scheme;
        std::string host;         ///< Host to resolve; an IPv6 literal without its brackets.
        std::string host_header;  ///< The authority as the Host header carries it.
        std::string port;
        std::string path;  ///< Request target: the path and query, never empty.
    };

    /// State for one HTTP exchange. `reset` rebuilds the per-hop pieces so a redirect can be
    /// followed on a fresh connection without discarding the operation.
    struct Exchange {
        Exchange(std::shared_ptr<Impl> state, net::strand<net::any_io_executor> executor,
                 std::string target, std::shared_ptr<detail::OAuthScopeState> abort_scope,
                 MetadataFetchPolicy fetch_policy, HostResolver resolver_hook,
                 std::shared_ptr<const TlsClientContext> tls_context, ProxyOptions proxy_options)
            : owner(std::move(state)),
              strand(std::move(executor)),
              url(std::move(target)),
              resolver(strand),
              scope(std::move(abort_scope)),
              policy(std::move(fetch_policy)),
              host_resolver(std::move(resolver_hook)),
              tls(std::move(tls_context)),
              proxy(std::move(proxy_options)) {}

        /// Prepares the next hop, on a fresh connection, to the URL just parsed.
        void reset() {
            endpoints.clear();
            stream.reset();
            auto next = std::make_shared<mcp::detail::ConnectPlan>();
            next->host = parsed.host;
            next->port = parsed.port;
            next->https = parsed.scheme == "https";
            next->server_name = mcp::detail::tls_server_name(parsed.host);
            next->tls = tls;
            next->proxy = mcp::detail::select_proxy(proxy, next->https, parsed.host, parsed.port);
            plan = std::move(next);
            buffer.clear();
            parser.emplace();
            parser->body_limit(policy.max_response_bytes);
        }

        [[nodiscard]] const http::response<http::string_body>& response() const {
            return parser->get();
        }

        std::shared_ptr<Impl> owner;
        net::strand<net::any_io_executor> strand;
        ParsedUrl parsed;
        std::string url;
        net::ip::tcp::resolver resolver;
        std::vector<net::ip::tcp::endpoint> endpoints;
        /// How this hop connects: directly or through the proxy, plain or TLS.
        std::shared_ptr<const mcp::detail::ConnectPlan> plan;
        std::shared_ptr<mcp::detail::Connection> stream;
        beast::flat_buffer buffer;
        std::optional<http::response_parser<http::string_body>> parser;
        std::string body;
        /// The abort latch this exchange answers to, held for the exchange's whole life so the
        /// latch cannot be freed while this exchange can still read it. Null is the client's own
        /// unscoped work, which only abort_pending() ends.
        std::shared_ptr<detail::OAuthScopeState> scope;
        /// Pinned for this exchange's whole life; see Impl::make_exchange().
        MetadataFetchPolicy policy;
        HostResolver host_resolver;
        std::shared_ptr<const TlsClientContext> tls;
        ProxyOptions proxy;
    };

    explicit Impl(const net::any_io_executor& executor) : strand(net::make_strand(executor)) {}

    static ParsedUrl parse_url(const std::string& url) {
        const auto url_scheme = mcp::detail::http_url_scheme(url);
        if (!url_scheme) {
            throw std::invalid_argument(
                "OAuth HTTP client URL must use the http:// or https:// scheme");
        }
        std::string scheme = url_scheme->https ? "https" : "http";
        std::string default_port = url_scheme->https ? "443" : "80";

        // The authority ends where the path, query or fragment begins, as in the fetch policy's
        // own split of the URL. The fragment is never sent, and a query needs the root path.
        const auto rest = url.substr(url_scheme->prefix_length);
        const auto authority_end = rest.find_first_of("/?#");
        auto authority = rest.substr(0, authority_end);
        std::string path = authority_end == std::string::npos ? "" : rest.substr(authority_end);
        if (const auto fragment = path.find('#'); fragment != std::string::npos) {
            path.erase(fragment);
        }
        if (path.empty() || path.front() != '/') {
            path.insert(0, "/");
        }

        std::string host;
        std::string host_header;
        auto port = std::move(default_port);
        if (!authority.empty() && authority.front() == '[') {
            // An IPv6 literal: the host is inside the brackets and the Host header keeps them.
            const auto closing = authority.find(']');
            if (closing == std::string::npos ||
                (closing + 1 < authority.size() && authority[closing + 1] != ':')) {
                throw std::invalid_argument("OAuth HTTP client URL has a malformed IPv6 host");
            }
            host = authority.substr(1, closing - 1);
            if (closing + 1 < authority.size()) {
                port = authority.substr(closing + 2);
            }
        } else {
            const auto colon = authority.find(':');
            if (colon == std::string::npos) {
                host = std::move(authority);
            } else {
                host = authority.substr(0, colon);
                port = authority.substr(colon + 1);
            }
        }
        // The port is named unless it is the scheme's default (RFC 9110 section 7.2).
        host_header = mcp::detail::authority_of(host, port, scheme == "https");

        return {
            .scheme = std::move(scheme),
            .host = std::move(host),
            .host_header = std::move(host_header),
            .port = std::move(port),
            .path = std::move(path),
        };
    }

    /// Validate a target before any lookup. Refusal happens here, so the host of a refused target
    /// is never resolved and no socket is opened for it.
    static void enforce_url_policy(const MetadataFetchPolicy& policy, const std::string& url) {
        const auto decision = validate_metadata_url(policy, url);
        if (decision != MetadataUrlDecision::eAllowed) {
            throw MetadataPolicyError(decision, url);
        }
    }

    /// Classify every address a lookup produced. One blocked answer refuses the whole fetch, so a
    /// resolver that mixes a routable answer with a hostile one cannot smuggle the hostile one in.
    static void enforce_address_policy(const MetadataFetchPolicy& policy,
                                       const std::vector<net::ip::tcp::endpoint>& endpoints) {
        for (const auto& endpoint : endpoints) {
            auto literal = endpoint.address().to_string();
            const auto decision = validate_metadata_address(policy, literal);
            if (decision != MetadataUrlDecision::eAllowed) {
                throw MetadataPolicyError(decision, std::move(literal));
            }
        }
    }

    static Task<void> connect(const std::shared_ptr<Exchange>& exchange) {
        const auto plan = exchange->plan;
        if (plan->proxy) {
            // The proxy resolves the server's name and connects to it, so only an address the URL
            // names can be checked here.
            boost::system::error_code not_an_address;
            const auto literal = net::ip::make_address(exchange->parsed.host, not_an_address);
            if (!not_an_address) {
                enforce_address_policy(
                    exchange->policy,
                    {
                        net::ip::tcp::endpoint(
                            literal, static_cast<unsigned short>(std::stoul(exchange->parsed.port))),
                    });
            }
            const auto results = co_await exchange->resolver.async_resolve(
                plan->proxy->host, plan->proxy->port, net::use_awaitable);
            for (const auto& entry : results) {
                exchange->endpoints.push_back(entry.endpoint());
            }
            if (exchange->endpoints.empty()) {
                throw std::runtime_error("No address resolved for the proxy " +
                                         sanitize_for_diagnostics(plan->proxy->host));
            }
        } else if (exchange->host_resolver) {
            const auto port = static_cast<unsigned short>(std::stoul(exchange->parsed.port));
            for (const auto& literal :
                 exchange->host_resolver(exchange->parsed.host, exchange->parsed.port)) {
                boost::system::error_code parse_error;
                const auto address = net::ip::make_address(literal, parse_error);
                if (parse_error) {
                    throw std::runtime_error("Host resolver returned an unusable address: " +
                                             sanitize_for_diagnostics(literal));
                }
                exchange->endpoints.emplace_back(address, port);
            }
        } else {
            const auto results = co_await exchange->resolver.async_resolve(
                exchange->parsed.host, exchange->parsed.port, net::use_awaitable);
            for (const auto& entry : results) {
                exchange->endpoints.push_back(entry.endpoint());
            }
        }

        if (!plan->proxy) {
            if (exchange->endpoints.empty()) {
                throw std::runtime_error("No address resolved for " +
                                         sanitize_for_diagnostics(exchange->parsed.host));
            }
            enforce_address_policy(exchange->policy, exchange->endpoints);
        }

        // Last look at the abort before a socket exists. An abort_pending() that ran while the
        // lookup was past cancelling found nothing to close. This coroutine holds the client's
        // strand from here until it suspends inside async_connect(), which opens the socket
        // first, and the close a later abort posts runs on that strand: it cannot run before the
        // socket is open, whichever thread it runs on, so it finds the socket and closes it.
        throw_if_aborted(exchange);

        // Connect only to the addresses this single lookup produced. They are pinned for the
        // exchange, so a name that resolves differently later cannot redirect it. The TLS check
        // names the URL's host, not the address.
        exchange->stream = mcp::detail::make_connection(exchange->strand, *plan);
        co_await mcp::detail::establish(exchange->stream, exchange->endpoints, plan);
    }

    /// A failed read or write on a TLS connection as the TlsError it is.
    static void throw_if_tls_failure(const Exchange& exchange, const boost::system::error_code& error) {
        if (!exchange.stream || !exchange.stream->is_tls()) {
            return;
        }
        if (auto failure = mcp::detail::established_failure(error, exchange.plan->server_name)) {
            throw TlsError(*failure);
        }
    }

    /// An operation the deadline cut off fails however the closed socket made it fail; report the
    /// timeout instead.
    static void throw_if_expired(const mcp::detail::SocketDeadline& deadline) {
        if (deadline.expired()) {
            throw boost::system::system_error(beast::error::timeout);
        }
    }

    template <typename Request>
    static Task<void> exchange_once(const std::shared_ptr<Exchange>& exchange, Request& request) {
        co_await connect(exchange);

        const mcp::detail::SocketDeadline deadline(
            exchange->stream->get_executor(), exchange->stream->socket(),
            std::chrono::seconds(mcp::detail::constants::http_client::g_TIMEOUT_SECONDS));
        try {
            co_await http::async_write(*exchange->stream, request, net::use_awaitable);
        } catch (const boost::system::system_error& error) {
            throw_if_expired(deadline);
            throw_if_tls_failure(*exchange, error.code());
            throw;
        }
        try {
            // The header is read on its own first. Boost 1.75 and older drop a response's
            // Content-Length body_limit error when the body arrives in the same read as the
            // header, and then read the whole body.
            co_await http::async_read_header(*exchange->stream, exchange->buffer, *exchange->parser,
                                             net::use_awaitable);
            co_await http::async_read(*exchange->stream, exchange->buffer, *exchange->parser,
                                      net::use_awaitable);
        } catch (const boost::system::system_error& error) {
            beast::error_code discard_error;
            (void)exchange->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both,
                                                      discard_error);
            throw_if_expired(deadline);
            if (error.code() == http::error::body_limit) {
                // The body is abandoned mid-read rather than buffered to completion.
                throw MetadataPolicyError(MetadataUrlDecision::eResponseTooLarge, exchange->url);
            }
            throw_if_tls_failure(*exchange, error.code());
            throw;
        }

        mcp::detail::close_connection(*exchange->stream);
    }

    /// RAII membership in `active_exchanges` for the lifetime of one HTTP exchange, so
    /// `abort_pending()` can reach a stalled connect/write/read and never targets one that finished.
    struct ActiveExchangeGuard {
        ActiveExchangeGuard(std::shared_ptr<Impl> owner_in, Exchange* exchange_in)
            : owner(std::move(owner_in)), exchange(exchange_in) {}
        ActiveExchangeGuard(const ActiveExchangeGuard&) = delete;
        ActiveExchangeGuard& operator=(const ActiveExchangeGuard&) = delete;
        ActiveExchangeGuard& operator=(ActiveExchangeGuard&&) = delete;

        ActiveExchangeGuard(ActiveExchangeGuard&& other) noexcept
            : owner(std::move(other.owner)), exchange(other.exchange) {
            other.exchange = nullptr;
        }

        ~ActiveExchangeGuard() {
            if (!owner || exchange == nullptr) {
                return;
            }
            std::scoped_lock lock(owner->active_mutex);
            auto& list = owner->active_exchanges;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [this](const std::weak_ptr<Exchange>& weak) {
                                          const auto locked = weak.lock();
                                          return !locked || locked.get() == exchange;
                                      }),
                       list.end());
        }

        std::shared_ptr<Impl> owner;
        Exchange* exchange;
    };

    /// Registers `exchange` as in flight, unless `abort_pending()` has already run -- in which case
    /// this exchange is refused before it opens a connection, the same as one abort_pending() closes
    /// out from under. Without this check, an exchange that starts registering after abort_pending()
    /// already swept `active_exchanges` would never be reached by it and would run to completion
    /// (see the sticky `aborted` flag on abort_pending() below).
    static ActiveExchangeGuard track_exchange(const std::shared_ptr<Exchange>& exchange) {
        std::scoped_lock lock(exchange->owner->active_mutex);
        if (exchange->owner->is_aborted(exchange->scope)) {
            // The same error an exchange already in flight sees when the abort closes its socket
            // underneath it, so a caller cannot tell whether this exchange started before or after
            // the client -- or its own scope -- was closed.
            throw boost::system::system_error(net::error::operation_aborted);
        }
        exchange->owner->active_exchanges.push_back(exchange);
        return {exchange->owner, exchange.get()};
    }

    /// Whether `scope` may still issue requests. The client-wide latch ends everything; a scoped
    /// latch ends only that scope. Callers hold active_mutex.
    [[nodiscard]] static bool is_aborted_locked(bool client_aborted,
                                                const std::shared_ptr<detail::OAuthScopeState>& scope) {
        return client_aborted || (scope && scope->aborted);
    }

    [[nodiscard]] bool is_aborted(const std::shared_ptr<detail::OAuthScopeState>& scope) const {
        return is_aborted_locked(aborted, scope);
    }

    /// Re-check the sticky abort part-way through an exchange, throwing exactly what
    /// track_exchange() throws.
    ///
    /// track_exchange() runs once per exchange, but run_get() follows redirects in a loop with a
    /// fresh socket per iteration. An abort that lands as an iteration's read completes closes a
    /// finished socket, so without this check the coroutine would follow the redirect and run a new
    /// request for a torn-down client.
    static void throw_if_aborted(const std::shared_ptr<Exchange>& exchange) {
        std::scoped_lock lock(exchange->owner->active_mutex);
        if (exchange->owner->is_aborted(exchange->scope)) {
            throw boost::system::system_error(net::error::operation_aborted);
        }
    }

    /// Close the underlying socket of every exchange currently in flight, posted onto the client's
    /// strand so the closure is never raced with the coroutine using it. A pending resolve, connect,
    /// write, or read then completes with an error instead of hanging. Also latches `aborted`, so
    /// every exchange that registers with track_exchange() from this point on -- including one that
    /// has not made its first network call yet -- is refused rather than left to run past a client
    /// that has moved on. Idempotent; a no-op when nothing is in flight either way.
    void abort_pending() { abort_matching(nullptr); }

    /// The scoped counterpart, with the same guarantees confined to one scope: `scope` is latched
    /// and every exchange belonging to it is closed, while other scopes and the client's own
    /// unscoped work carry on. Pass `nullptr` to mean the whole client, which is what
    /// abort_pending() does.
    void abort_matching(const std::shared_ptr<detail::OAuthScopeState>& scope) {
        std::vector<std::shared_ptr<Exchange>> exchanges;
        {
            std::scoped_lock lock(active_mutex);
            if (!scope) {
                aborted = true;
            } else {
                scope->aborted = true;
            }
            for (auto& weak : active_exchanges) {
                auto locked = weak.lock();
                if (locked && (!scope || locked->scope == scope)) {
                    exchanges.push_back(std::move(locked));
                }
            }
        }
        for (auto& exchange : exchanges) {
            net::post(strand, [exchange]() {
                exchange->resolver.cancel();
                if (exchange->stream) {
                    beast::error_code ec;
                    exchange->stream->socket().close(ec);
                }
            });
        }
    }

    /// Hand out a fresh latch. A scope is identified by the control block itself, which every party
    /// that can consult it keeps alive, so a new scope can never alias a latched one the way a
    /// recycled id could. Do not reintroduce scope ids.
    static std::shared_ptr<detail::OAuthScopeState> new_scope() {
        return std::make_shared<detail::OAuthScopeState>();
    }

    /// Build an exchange with the fetch policy and host resolver pinned for its whole lifetime, read
    /// once under the mutex that guards the exchange list: the setters are plain writes to state the
    /// exchange coroutines read, and an exchange re-validates every redirect hop, so a policy swapped
    /// mid-chain would otherwise check later hops against different rules. Every request carries the
    /// scope it was issued under, so aborting that scope reaches exactly these exchanges; a null
    /// scope is the client's own unscoped work.
    std::shared_ptr<Exchange> make_exchange(std::string url,
                                            std::shared_ptr<detail::OAuthScopeState> scope) {
        std::scoped_lock lock(active_mutex);
        return std::make_shared<Exchange>(shared_from_this(), strand, std::move(url), std::move(scope),
                                          policy, host_resolver, tls, proxy);
    }

    /// Run one exchange on the client's strand and hand its outcome back to the caller off it.
    ///
    /// The strand keeps an exchange apart from the close abort_matching() posts when several threads
    /// run the io_context. Posting the coroutine to the strand is not enough: an awaitable's executor
    /// is fixed when it is spawned, so it is spawned on the strand and every resumption lands there.
    ///
    /// co_spawn() completes by dispatching to the caller's executor, which on an io_context thread
    /// runs the caller in place inside the strand handler; the caller is posted off the strand first
    /// so it does not hold it. The result waits out that post on the heap, not in this frame; see the
    /// GCC 11 note at the top of this file.
    template <typename Result>
    static Task<Result> on_strand(net::strand<net::any_io_executor> strand, Task<Result> exchange) {
        std::shared_ptr<Result> result;
        std::exception_ptr failure;
        try {
            result = std::make_shared<Result>(
                co_await mcp::detail::spawn_awaitable(strand, std::move(exchange)));
        } catch (...) {
            failure = std::current_exception();
        }
        if (strand.running_in_this_thread()) {
#if BOOST_VERSION >= 107700
            // A caller that has been cancelled still has to leave the strand, and co_await throws
            // for a cancelled coroutine before it initiates anything. The outcome is already
            // decided either way: the cancellation reached the exchange through co_spawn(), or
            // arrived too late to matter to it.
            //
            // The setting belongs to the caller's whole coroutine, so it goes back as it was
            // found even if the post throws. Restoring it is itself a co_await, which rules out
            // a destructor or a catch block.
            const bool throws_if_cancelled = co_await net::this_coro::throw_if_cancelled();
            co_await net::this_coro::throw_if_cancelled(false);
            std::exception_ptr hop_failure;
            try {
                co_await net::post(net::use_awaitable);
            } catch (...) {
                hop_failure = std::current_exception();
            }
            co_await net::this_coro::throw_if_cancelled(throws_if_cancelled);
            if (hop_failure) {
                std::rethrow_exception(hop_failure);
            }
#else
            co_await net::post(net::use_awaitable);
#endif
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        co_return std::move(*result);
    }

    Task<nlohmann::json> get_json(std::string url,
                                  std::shared_ptr<detail::OAuthScopeState> scope = nullptr) {
        return on_strand(strand, run_get(make_exchange(std::move(url), std::move(scope))));
    }

    Task<TokenResponse> post_token_request(std::string token_endpoint, const KeyValuePairList& params,
                                           std::string authorization,
                                           std::shared_ptr<detail::OAuthScopeState> scope = nullptr) {
        return on_strand(strand, run_post(make_exchange(std::move(token_endpoint), std::move(scope)),
                                          std::make_shared<std::string>(mcp::detail::form_body(params)),
                                          std::move(authorization)));
    }

    Task<nlohmann::json> post_form(std::string url, const KeyValuePairList& params,
                                   std::string authorization,
                                   std::shared_ptr<detail::OAuthScopeState> scope = nullptr) {
        return on_strand(strand,
                         run_post_json(make_exchange(std::move(url), std::move(scope)),
                                       std::make_shared<std::string>(mcp::detail::form_body(params)),
                                       "application/x-www-form-urlencoded", "Token request",
                                       std::move(authorization)));
    }

    Task<nlohmann::json> post_json(std::string url, std::string body,
                                   std::shared_ptr<detail::OAuthScopeState> scope = nullptr) {
        // Sanitized where the label is built, not where it is thrown: `url` is moved into the
        // exchange on the next line, and the throw site only ever sees the label.
        auto label = "HTTP POST " + sanitize_for_diagnostics(url);
        return on_strand(strand, run_post_json(make_exchange(std::move(url), std::move(scope)),
                                               std::make_shared<std::string>(std::move(body)),
                                               "application/json", std::move(label)));
    }

    /// Host, and the proxy's credentials when the request goes to the proxy itself.
    template <typename Request>
    static void set_routing_headers(Request& request, const Exchange& exchange) {
        request.set(http::field::host, exchange.parsed.host_header);
        if (exchange.plan->forwards_requests() && !exchange.plan->proxy->authorization.empty()) {
            request.set(http::field::proxy_authorization, exchange.plan->proxy->authorization);
        }
    }

    static Task<nlohmann::json> run_get(std::shared_ptr<Exchange> exchange) {
        auto guard = track_exchange(exchange);

        const auto redirect_budget = exchange->policy.max_redirects;
        for (std::size_t redirect = 0;; ++redirect) {
            // Re-read the abort on every hop, not just at track_exchange() above: abort_pending()
            // can land between two iterations, where it has nothing left to close.
            throw_if_aborted(exchange);
            // Every hop, including each redirect target, is validated afresh before it is reached.
            enforce_url_policy(exchange->policy, exchange->url);
            exchange->parsed = parse_url(exchange->url);
            exchange->reset();

            http::request<http::empty_body> request(
                http::verb::get, exchange->plan->request_target(exchange->parsed.path),
                mcp::detail::constants::http_client::g_VERSION_11);
            set_routing_headers(request, *exchange);
            request.set(http::field::accept, "application/json");
            co_await exchange_once(exchange, request);

            const auto status = exchange->response().result_int();
            if (!is_redirect_status(status)) {
                break;
            }
            if (redirect >= redirect_budget) {
                throw MetadataPolicyError(MetadataUrlDecision::eRedirectLimitExceeded, exchange->url);
            }
            const auto location = exchange->response().find(http::field::location);
            if (location == exchange->response().end()) {
                throw std::runtime_error("HTTP GET " + sanitize_for_diagnostics(exchange->url) +
                                         " returned a redirect without a Location header");
            }
            exchange->url = resolve_redirect_target(exchange->url, std::string(location->value()));
        }

        if (exchange->response().result_int() >=
            mcp::detail::constants::http_client::g_STATUS_BAD_REQUEST) {
            throw std::runtime_error("HTTP GET " + sanitize_for_diagnostics(exchange->url) +
                                     " failed with status " +
                                     std::to_string(exchange->response().result_int()));
        }

        exchange->body = exchange->response().body();
        auto response_json = mcp::detail::parse_peer_json(exchange->body);
        if (response_json.is_discarded()) {
            throw std::runtime_error("Failed to parse JSON from " +
                                     sanitize_for_diagnostics(exchange->url));
        }
        co_return response_json;
    }

    /// One POST exchange returning the parsed JSON reply. Redirects are deliberately not followed:
    /// replaying a token or registration body at a redirect target would hand its contents to
    /// whatever the first hop nominated.
    static Task<nlohmann::json> run_post_json(std::shared_ptr<Exchange> exchange,
                                              std::shared_ptr<std::string> body,
                                              std::string content_type, std::string failure_label,
                                              std::string authorization = {}) {
        auto guard = track_exchange(exchange);

        enforce_url_policy(exchange->policy, exchange->url);
        exchange->parsed = parse_url(exchange->url);
        exchange->reset();

        http::request<http::string_body> request(http::verb::post,
                                                 exchange->plan->request_target(exchange->parsed.path),
                                                 mcp::detail::constants::http_client::g_VERSION_11);
        set_routing_headers(request, *exchange);
        request.set(http::field::content_type, content_type);
        request.set(http::field::accept, "application/json");
        if (!authorization.empty()) {
            request.set(http::field::authorization, authorization);
        }
        request.body() = *body;
        request.prepare_payload();
        co_await exchange_once(exchange, request);

        if (exchange->response().result_int() >=
            mcp::detail::constants::http_client::g_STATUS_BAD_REQUEST) {
            throw std::runtime_error(failure_label + " failed with status " +
                                     std::to_string(exchange->response().result_int()) + ": " +
                                     sanitize_for_diagnostics(exchange->response().body()));
        }

        exchange->body = exchange->response().body();
        auto response_json = mcp::detail::parse_peer_json(exchange->body);
        if (response_json.is_discarded()) {
            throw std::runtime_error("Failed to parse JSON from " +
                                     sanitize_for_diagnostics(exchange->url));
        }
        co_return response_json;
    }

    static Task<TokenResponse> run_post(std::shared_ptr<Exchange> exchange,
                                        std::shared_ptr<std::string> form_body,
                                        std::string authorization) {
        auto response_json = co_await run_post_json(std::move(exchange), std::move(form_body),
                                                    "application/x-www-form-urlencoded",
                                                    "Token request", std::move(authorization));
        co_return response_json.get<TokenResponse>();
    }

    net::strand<net::any_io_executor> strand;
    std::mutex active_mutex;
    /// Both guarded by active_mutex: written by the setters, read once per exchange in
    /// make_exchange().
    MetadataFetchPolicy policy;
    HostResolver host_resolver;
    std::shared_ptr<const TlsClientContext> tls;
    ProxyOptions proxy;
    std::vector<std::weak_ptr<Exchange>> active_exchanges;
    /// Set once by abort_pending(); makes the abort sticky so an exchange started afterward is
    /// refused instead of running to completion. Guarded by active_mutex alongside the list above.
    bool aborted{false};
    // A scope's abort latch lives on the scope itself (detail::OAuthScopeState). Do not add a
    // container of scope state here: it would have to outlive every exchange that could still
    // consult it, which in practice means never being cleaned up.
};

OAuthHttpClient::OAuthHttpClient(const Executor& executor)
    : impl_(std::make_shared<Impl>(executor.get())) {}

/// Safe to call at any time, including mid-flight: exchanges already running keep what they started
/// with, and the next one picks up the new value. See configure() below for changing both together.
void OAuthHttpClient::set_metadata_policy(MetadataFetchPolicy policy) {
    std::scoped_lock lock(impl_->active_mutex);
    impl_->policy = std::move(policy);
}

void OAuthHttpClient::set_host_resolver(HostResolver resolver) {
    std::scoped_lock lock(impl_->active_mutex);
    impl_->host_resolver = std::move(resolver);
}

/// Installs both values under one lock. Use this rather than the two setters above whenever both
/// change: calling them in sequence leaves an interval holding one new value and one old one, and
/// make_exchange() pins whatever it finds. Resolver-first is the natural order to write and the
/// dangerous one. See the @warning on set_metadata_policy() in include/mcp/auth/oauth.hpp for why.
void OAuthHttpClient::configure(MetadataFetchPolicy policy, HostResolver resolver) {
    std::scoped_lock lock(impl_->active_mutex);
    impl_->policy = std::move(policy);
    impl_->host_resolver = std::move(resolver);
}

void OAuthHttpClient::configure(MetadataFetchPolicy policy, HostResolver resolver,
                                std::shared_ptr<const TlsClientContext> tls, ProxyOptions proxy) {
    mcp::detail::validate_proxy_options(proxy);
    std::scoped_lock lock(impl_->active_mutex);
    impl_->policy = std::move(policy);
    impl_->host_resolver = std::move(resolver);
    impl_->tls = std::move(tls);
    impl_->proxy = std::move(proxy);
}

void OAuthHttpClient::abort_pending() { impl_->abort_pending(); }

namespace {

/// Place the client secret where the negotiated method says it belongs, and nowhere else.
///
/// @return The `Authorization` header value, empty when the secret does not travel in a header.
std::string apply_client_authentication(const OAuthConfig& config, KeyValuePairList& params) {
    const auto& method = config.token_endpoint_auth_method;
    if (method && *method == "none") {
        return {};
    }
    if (!config.client_secret) {
        return {};
    }
    if (method && *method == "client_secret_basic") {
        return "Basic " + mcp::detail::base64_encode(mcp::detail::form_value(config.client_id) + ":" +
                                                     mcp::detail::form_value(*config.client_secret));
    }
    params.emplace_back("client_secret", *config.client_secret);
    return {};
}

}  // namespace

namespace {

/// The form body and Authorization header for one token-endpoint request. Built here rather than
/// inline so the client and a scope on it issue byte-identical requests, differing only in which
/// abort scope the exchange is tracked under.
struct TokenRequest {
    KeyValuePairList params;
    std::string authorization;
};

TokenRequest build_authorization_code_request(const OAuthConfig& config, const std::string& code,
                                              const std::string& code_verifier) {
    KeyValuePairList params = {
        {"grant_type", "authorization_code"},  {"code", code},
        {"redirect_uri", config.redirect_uri}, {"client_id", config.client_id},
        {"code_verifier", code_verifier},
    };
    auto authorization = apply_client_authentication(config, params);
    if (config.resource) {
        params.emplace_back("resource", *config.resource);
    }
    return {.params = std::move(params), .authorization = std::move(authorization)};
}

TokenRequest build_refresh_request(const OAuthConfig& config, const std::string& refresh_token) {
    KeyValuePairList params = {
        {"grant_type", "refresh_token"},
        {"refresh_token", refresh_token},
        {"client_id", config.client_id},
    };
    auto authorization = apply_client_authentication(config, params);
    if (config.resource) {
        params.emplace_back("resource", *config.resource);
    }
    return {.params = std::move(params), .authorization = std::move(authorization)};
}

}  // namespace

Task<TokenResponse> OAuthHttpClient::exchange_code(const OAuthConfig& config, const std::string& code,
                                                   const std::string& code_verifier) {
    auto request = build_authorization_code_request(config, code, code_verifier);
    return impl_->post_token_request(config.token_endpoint, request.params,
                                     std::move(request.authorization));
}

Task<TokenResponse> OAuthHttpClient::refresh_token(const OAuthConfig& config,
                                                   const std::string& refresh_token) {
    auto request = build_refresh_request(config, refresh_token);
    return impl_->post_token_request(config.token_endpoint, request.params,
                                     std::move(request.authorization));
}

Task<nlohmann::json> OAuthHttpClient::get_json(const std::string& url) { return impl_->get_json(url); }

Task<nlohmann::json> OAuthHttpClient::post_json(const std::string& url, const nlohmann::json& body) {
    return impl_->post_json(url, body.dump());
}

Task<nlohmann::json> OAuthHttpClient::post_form(const std::string& url, const KeyValuePairList& params,
                                                const std::string& authorization) {
    return impl_->post_form(url, params, authorization);
}

namespace internal {

std::size_t live_scope_latch_count() { return detail::OAuthScopeState::live_count(); }

}  // namespace internal

OAuthHttpClientScope OAuthHttpClient::make_scope() { return {impl_, impl_->new_scope()}; }

OAuthHttpClientScope::OAuthHttpClientScope(std::shared_ptr<OAuthHttpClient::Impl> impl,
                                           std::shared_ptr<detail::OAuthScopeState> state)
    : impl_(std::move(impl)), state_(std::move(state)) {}

Task<TokenResponse> OAuthHttpClientScope::exchange_code(const OAuthConfig& config,
                                                        const std::string& code,
                                                        const std::string& code_verifier) {
    auto request = build_authorization_code_request(config, code, code_verifier);
    return impl_->post_token_request(config.token_endpoint, request.params,
                                     std::move(request.authorization), state_);
}

Task<TokenResponse> OAuthHttpClientScope::refresh_token(const OAuthConfig& config,
                                                        const std::string& refresh_token) {
    auto request = build_refresh_request(config, refresh_token);
    return impl_->post_token_request(config.token_endpoint, request.params,
                                     std::move(request.authorization), state_);
}

Task<nlohmann::json> OAuthHttpClientScope::get_json(const std::string& url) {
    return impl_->get_json(url, state_);
}

Task<nlohmann::json> OAuthHttpClientScope::post_json(const std::string& url,
                                                     const nlohmann::json& body) {
    return impl_->post_json(url, body.dump(), state_);
}

Task<nlohmann::json> OAuthHttpClientScope::post_form(const std::string& url,
                                                     const KeyValuePairList& params,
                                                     const std::string& authorization) {
    return impl_->post_form(url, params, authorization, state_);
}

void OAuthHttpClientScope::abort() { impl_->abort_matching(state_); }

}  // namespace mcp::auth
