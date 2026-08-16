#include <mcp/auth/oauth.hpp>

#include <mcp/transport/http_client.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <mcp/detail/secure_random.hpp>

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

namespace detail {

std::string base64_encode(const unsigned char* data, std::size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (std::size_t i = 0; i < len; i += 3) {
        unsigned int value = static_cast<unsigned int>(data[i]) << constants::g_shift16;
        if (i + 1 < len) {
            value |= static_cast<unsigned int>(data[i + 1]) << constants::g_shift8;
        }
        if (i + 2 < len) {
            value |= static_cast<unsigned int>(data[i + 2]);
        }

        result.push_back(
            mcp::constants::g_alphabet[(value >> constants::g_shift18) & constants::g_mask0x3F]);
        result.push_back(
            mcp::constants::g_alphabet[(value >> constants::g_shift12) & constants::g_mask0x3F]);
        result.push_back(
            (i + 1 < len)
                ? mcp::constants::g_alphabet[(value >> constants::g_shift6) & constants::g_mask0x3F]
                : '=');
        result.push_back((i + 2 < len) ? mcp::constants::g_alphabet[value & constants::g_mask0x3F]
                                       : '=');
    }

    return result;
}

std::string base64url_encode(const unsigned char* data, std::size_t len) {
    auto encoded = base64_encode(data, len);

    for (auto& ch : encoded) {
        if (ch == '+') {
            ch = '-';
        } else if (ch == '/') {
            ch = '_';
        }
    }
    encoded.erase(std::remove(encoded.begin(), encoded.end(), '='), encoded.end());
    return encoded;
}

std::array<unsigned char, constants::g_sha256_digest_length> sha256(const std::string& input) {
    std::array<unsigned char, constants::g_sha256_digest_length> digest{};
    unsigned int digest_len = 0;

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &digest_len) != 1) {
        throw std::runtime_error("OpenSSL SHA-256 failed");
    }

    return digest;
}

std::string generate_random_string(std::size_t length) {
    static const std::size_t s_charset_size = mcp::constants::g_unreserved_chars.size();
    static const auto s_bias_limit =
        static_cast<unsigned char>((256 / s_charset_size) * s_charset_size);

    std::string result;
    result.reserve(length);
    while (result.size() < length) {
        unsigned char byte = 0;
        if (RAND_bytes(&byte, 1) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        if (byte < s_bias_limit) {
            result.push_back(mcp::constants::g_unreserved_chars[byte % s_charset_size]);
        }
    }
    return result;
}

std::string url_encode(const std::string& value) {
    std::string result;
    result.reserve(value.size() * 3);

    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(mcp::constants::g_hex_digits_upper[ch >> constants::g_shift4]);
            result.push_back(mcp::constants::g_hex_digits_upper[ch & constants::g_mask0x0F]);
        }
    }
    return result;
}

std::string build_form_body(const KeyValuePairList& params) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += url_encode(key) + "=" + url_encode(value);
    }
    return body;
}

}  // namespace detail

PkcePair generate_pkce_pair(std::size_t verifier_length) {
    if (verifier_length < constants::g_min_verifier_length ||
        verifier_length > constants::g_max_verifier_length) {
        throw std::invalid_argument("PKCE verifier length must be 43-128 characters");
    }

    PkcePair pair;
    pair.code_verifier = detail::generate_random_string(verifier_length);
    pair.challenge_method = "S256";

    const auto hash = detail::sha256(pair.code_verifier);
    pair.code_challenge = detail::base64url_encode(hash.data(), hash.size());
    return pair;
}

bool TokenResponse::is_expired(int margin) const {
    if (!expires_in.has_value()) {
        return false;
    }
    const auto expiry = received_at + std::chrono::seconds(*expires_in) - std::chrono::seconds(margin);
    return std::chrono::steady_clock::now() >= expiry;
}

void from_json(const nlohmann::json& json, TokenResponse& token) {
    json.at("access_token").get_to(token.access_token);
    token.token_type = json.value("token_type", "Bearer");
    if (json.contains("refresh_token")) {
        token.refresh_token = json.at("refresh_token").get<std::string>();
    }
    if (json.contains("expires_in")) {
        token.expires_in = json.at("expires_in").get<int>();
    }
    if (json.contains("scope")) {
        token.scope = json.at("scope").get<std::string>();
    }
    token.received_at = std::chrono::steady_clock::now();
}

void to_json(nlohmann::json& json, const TokenResponse& token) {
    json = nlohmann::json{{"access_token", token.access_token}, {"token_type", token.token_type}};
    if (token.refresh_token) {
        json["refresh_token"] = *token.refresh_token;
    }
    if (token.expires_in) {
        json["expires_in"] = *token.expires_in;
    }
    if (token.scope) {
        json["scope"] = *token.scope;
    }
}

struct InMemoryTokenStore::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::string, TokenResponse> tokens;
};

InMemoryTokenStore::InMemoryTokenStore() : impl_(std::make_unique<Impl>()) {}

InMemoryTokenStore::~InMemoryTokenStore() = default;

void InMemoryTokenStore::store(const std::string& server_url, TokenResponse token) {
    std::lock_guard lock(impl_->mutex);
    impl_->tokens[server_url] = std::move(token);
}

std::optional<TokenResponse> InMemoryTokenStore::load(const std::string& server_url) const {
    std::lock_guard lock(impl_->mutex);
    const auto iter = impl_->tokens.find(server_url);
    if (iter == impl_->tokens.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void InMemoryTokenStore::remove(const std::string& server_url) {
    std::lock_guard lock(impl_->mutex);
    impl_->tokens.erase(server_url);
}

namespace {

constexpr std::string_view g_https_prefix = "https://";

/// Resolve a `Location` header against the URL that produced it.
std::string resolve_redirect_target(const std::string& base, const std::string& location) {
    if (location.find("://") != std::string::npos) {
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

struct OAuthHttpClient::Impl : std::enable_shared_from_this<OAuthHttpClient::Impl> {
    struct ParsedUrl {
        std::string scheme;
        std::string host;
        std::string port;
        std::string path;
    };

    /// State for one HTTP exchange. `reset` rebuilds the per-hop pieces so a redirect can be
    /// followed on a fresh connection without discarding the operation.
    struct Exchange {
        Exchange(std::shared_ptr<Impl> state, net::strand<net::any_io_executor> executor,
                 std::string target)
            : owner(std::move(state)),
              strand(std::move(executor)),
              url(std::move(target)),
              resolver(strand) {}

        void reset() {
            endpoints.clear();
            // A redirect is followed on a fresh connection; tcp_stream is not reassignable.
            stream.emplace(strand);
            buffer.clear();
            parser.emplace();
            const auto limit = owner->policy ? owner->policy->max_response_bytes
                                             : std::numeric_limits<std::uint64_t>::max();
            parser->body_limit(limit);
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
        std::optional<beast::tcp_stream> stream;
        beast::flat_buffer buffer;
        std::optional<http::response_parser<http::string_body>> parser;
        std::string body;
    };

    explicit Impl(const net::any_io_executor& executor) : strand(net::make_strand(executor)) {}

    static ParsedUrl parse_url(const std::string& url) {
        std::string scheme;
        std::string default_port;
        std::size_t prefix_length = 0;
        if (url.starts_with(mcp::constants::g_http_prefix)) {
            scheme = "http";
            default_port = "80";
            prefix_length = mcp::constants::g_http_prefix.size();
        } else if (url.starts_with(g_https_prefix)) {
            scheme = "https";
            default_port = "443";
            prefix_length = g_https_prefix.size();
        } else {
            throw std::invalid_argument(
                "OAuth HTTP client URL must use the http:// or https:// scheme");
        }

        auto authority_and_path = url.substr(prefix_length);
        const auto path_separator = authority_and_path.find('/');
        auto authority = authority_and_path.substr(0, path_separator);
        auto path =
            path_separator == std::string::npos ? "/" : authority_and_path.substr(path_separator);

        std::string host;
        auto port = std::move(default_port);
        const auto colon = authority.find(':');
        if (colon == std::string::npos) {
            host = std::move(authority);
        } else {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }

        return {std::move(scheme), std::move(host), std::move(port), std::move(path)};
    }

    /// Validate a target before any lookup. Refusal happens here, so the host of a refused target
    /// is never resolved and no socket is opened for it.
    static void enforce_url_policy(const std::optional<MetadataFetchPolicy>& policy,
                                   const std::string& url) {
        if (!policy) {
            return;
        }
        const auto decision = validate_metadata_url(*policy, url);
        if (decision != MetadataUrlDecision::allowed) {
            throw MetadataPolicyError(decision, url);
        }
    }

    /// Classify every address a lookup produced. One blocked answer refuses the whole fetch, so a
    /// resolver that mixes a routable answer with a hostile one cannot smuggle the hostile one in.
    static void enforce_address_policy(const std::optional<MetadataFetchPolicy>& policy,
                                       const std::vector<net::ip::tcp::endpoint>& endpoints) {
        if (!policy) {
            return;
        }
        for (const auto& endpoint : endpoints) {
            auto literal = endpoint.address().to_string();
            const auto decision = validate_metadata_address(*policy, literal);
            if (decision != MetadataUrlDecision::allowed) {
                throw MetadataPolicyError(decision, std::move(literal));
            }
        }
    }

    static Task<void> connect(const std::shared_ptr<Exchange>& exchange) {
        if (exchange->parsed.scheme != "http") {
            throw std::runtime_error(
                "OAuth over https requires TLS support, which this build does not provide: " +
                exchange->url);
        }

        if (exchange->owner->host_resolver) {
            const auto port = static_cast<unsigned short>(std::stoul(exchange->parsed.port));
            for (const auto& literal :
                 exchange->owner->host_resolver(exchange->parsed.host, exchange->parsed.port)) {
                boost::system::error_code parse_error;
                const auto address = net::ip::make_address(literal, parse_error);
                if (parse_error) {
                    throw std::runtime_error("Host resolver returned an unusable address: " + literal);
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

        if (exchange->endpoints.empty()) {
            throw std::runtime_error("No address resolved for " + exchange->parsed.host);
        }
        enforce_address_policy(exchange->owner->policy, exchange->endpoints);

        // Connect only to the addresses this single lookup produced. They are pinned for the
        // exchange, so a name that resolves differently later cannot redirect it.
        exchange->stream->expires_after(std::chrono::seconds(mcp::constants::g_http_timeout_seconds));
        co_await exchange->stream->async_connect(exchange->endpoints, net::use_awaitable);
    }

    template <typename Request>
    static Task<void> exchange_once(const std::shared_ptr<Exchange>& exchange, Request& request) {
        co_await connect(exchange);

        exchange->stream->expires_after(std::chrono::seconds(mcp::constants::g_http_timeout_seconds));
        co_await http::async_write(*exchange->stream, request, net::use_awaitable);
        try {
            co_await http::async_read(*exchange->stream, exchange->buffer, *exchange->parser,
                                      net::use_awaitable);
        } catch (const boost::system::system_error& error) {
            beast::error_code discard_error;
            (void)exchange->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both,
                                                      discard_error);
            if (error.code() == http::error::body_limit) {
                // The body is abandoned mid-read rather than buffered to completion.
                throw MetadataPolicyError(MetadataUrlDecision::response_too_large, exchange->url);
            }
            throw;
        }

        beast::error_code shutdown_error;
        (void)exchange->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both, shutdown_error);
    }

    Task<nlohmann::json> get_json(std::string url) {
        return run_get(std::make_shared<Exchange>(shared_from_this(), strand, std::move(url)));
    }

    Task<TokenResponse> post_token_request(std::string token_endpoint, const KeyValuePairList& params,
                                           std::string authorization) {
        return run_post(
            std::make_shared<Exchange>(shared_from_this(), strand, std::move(token_endpoint)),
            std::make_shared<std::string>(detail::build_form_body(params)), std::move(authorization));
    }

    Task<nlohmann::json> post_json(std::string url, std::string body) {
        auto label = "HTTP POST " + url;
        return run_post_json(std::make_shared<Exchange>(shared_from_this(), strand, std::move(url)),
                             std::make_shared<std::string>(std::move(body)), "application/json",
                             std::move(label));
    }

    static Task<nlohmann::json> run_get(std::shared_ptr<Exchange> exchange) {
        co_await net::post(exchange->strand, net::use_awaitable);

        const auto redirect_budget =
            exchange->owner->policy ? exchange->owner->policy->max_redirects : 0;
        for (std::size_t redirect = 0;; ++redirect) {
            // Every hop, including each redirect target, is validated afresh before it is reached.
            enforce_url_policy(exchange->owner->policy, exchange->url);
            exchange->parsed = parse_url(exchange->url);
            exchange->reset();

            http::request<http::empty_body> request(http::verb::get, exchange->parsed.path,
                                                    mcp::constants::g_http_version_11);
            request.set(http::field::host, exchange->parsed.host);
            request.set(http::field::accept, "application/json");
            co_await exchange_once(exchange, request);

            const auto status = exchange->response().result_int();
            if (!is_redirect_status(status)) {
                break;
            }
            if (redirect >= redirect_budget) {
                throw MetadataPolicyError(MetadataUrlDecision::redirect_limit_exceeded, exchange->url);
            }
            const auto location = exchange->response().find(http::field::location);
            if (location == exchange->response().end()) {
                throw std::runtime_error("HTTP GET " + exchange->url +
                                         " returned a redirect without a Location header");
            }
            exchange->url = resolve_redirect_target(exchange->url, std::string(location->value()));
        }

        if (exchange->response().result_int() >= mcp::constants::g_http_bad_request) {
            throw std::runtime_error("HTTP GET " + exchange->url + " failed with status " +
                                     std::to_string(exchange->response().result_int()));
        }

        exchange->body = exchange->response().body();
        auto response_json = nlohmann::json::parse(exchange->body, nullptr, false);
        if (response_json.is_discarded()) {
            throw std::runtime_error("Failed to parse JSON from " + exchange->url);
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
        co_await net::post(exchange->strand, net::use_awaitable);

        enforce_url_policy(exchange->owner->policy, exchange->url);
        exchange->parsed = parse_url(exchange->url);
        exchange->reset();

        http::request<http::string_body> request(http::verb::post, exchange->parsed.path,
                                                 mcp::constants::g_http_version_11);
        request.set(http::field::host, exchange->parsed.host);
        request.set(http::field::content_type, content_type);
        request.set(http::field::accept, "application/json");
        if (!authorization.empty()) {
            request.set(http::field::authorization, authorization);
        }
        request.body() = *body;
        request.prepare_payload();
        co_await exchange_once(exchange, request);

        if (exchange->response().result_int() >= mcp::constants::g_http_bad_request) {
            throw std::runtime_error(failure_label + " failed with status " +
                                     std::to_string(exchange->response().result_int()) + ": " +
                                     exchange->response().body());
        }

        exchange->body = exchange->response().body();
        auto response_json = nlohmann::json::parse(exchange->body, nullptr, false);
        if (response_json.is_discarded()) {
            throw std::runtime_error("Failed to parse JSON from " + exchange->url);
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
    std::optional<MetadataFetchPolicy> policy;
    HostResolver host_resolver;
};

OAuthHttpClient::OAuthHttpClient(const net::any_io_executor& executor)
    : impl_(std::make_shared<Impl>(executor)) {}

void OAuthHttpClient::set_metadata_policy(MetadataFetchPolicy policy) {
    impl_->policy = std::move(policy);
}

void OAuthHttpClient::set_host_resolver(HostResolver resolver) {
    impl_->host_resolver = std::move(resolver);
}

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
        const auto credentials =
            detail::url_encode(config.client_id) + ":" + detail::url_encode(*config.client_secret);
        return "Basic " +
               detail::base64_encode(reinterpret_cast<const unsigned char*>(credentials.data()),
                                     credentials.size());
    }
    params.emplace_back("client_secret", *config.client_secret);
    return {};
}

}  // namespace

Task<TokenResponse> OAuthHttpClient::exchange_code(const OAuthConfig& config, const std::string& code,
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
    return impl_->post_token_request(config.token_endpoint, params, std::move(authorization));
}

Task<TokenResponse> OAuthHttpClient::refresh_token(const OAuthConfig& config,
                                                   const std::string& refresh_token) {
    KeyValuePairList params = {
        {"grant_type", "refresh_token"},
        {"refresh_token", refresh_token},
        {"client_id", config.client_id},
    };
    auto authorization = apply_client_authentication(config, params);
    if (config.resource) {
        params.emplace_back("resource", *config.resource);
    }
    return impl_->post_token_request(config.token_endpoint, params, std::move(authorization));
}

Task<nlohmann::json> OAuthHttpClient::get_json(const std::string& url) { return impl_->get_json(url); }

Task<nlohmann::json> OAuthHttpClient::post_json(const std::string& url, const nlohmann::json& body) {
    return impl_->post_json(url, body.dump());
}

void from_json(const nlohmann::json& json, ProtectedResourceMetadata& metadata) {
    metadata.raw = json;
    if (json.contains("resource")) {
        json.at("resource").get_to(metadata.resource);
    }
    if (json.contains("authorization_servers")) {
        json.at("authorization_servers").get_to(metadata.authorization_servers);
    }
    if (json.contains("scopes_supported")) {
        metadata.scopes_supported = json.at("scopes_supported").get<std::vector<std::string>>();
    }
}

void from_json(const nlohmann::json& json, AuthServerMetadata& metadata) {
    metadata.raw = json;
    if (json.contains("issuer")) {
        json.at("issuer").get_to(metadata.issuer);
    }
    if (json.contains("authorization_endpoint")) {
        json.at("authorization_endpoint").get_to(metadata.authorization_endpoint);
    }
    if (json.contains("token_endpoint")) {
        json.at("token_endpoint").get_to(metadata.token_endpoint);
    }
    if (json.contains("revocation_endpoint")) {
        metadata.revocation_endpoint = json.at("revocation_endpoint").get<std::string>();
    }
    if (json.contains("registration_endpoint")) {
        metadata.registration_endpoint = json.at("registration_endpoint").get<std::string>();
    }
    if (json.contains("scopes_supported")) {
        metadata.scopes_supported = json.at("scopes_supported").get<std::vector<std::string>>();
    }
    if (json.contains("response_types_supported")) {
        metadata.response_types_supported =
            json.at("response_types_supported").get<std::vector<std::string>>();
    }
    if (json.contains("grant_types_supported")) {
        metadata.grant_types_supported =
            json.at("grant_types_supported").get<std::vector<std::string>>();
    }
    if (json.contains("code_challenge_methods_supported")) {
        metadata.code_challenge_methods_supported =
            json.at("code_challenge_methods_supported").get<std::vector<std::string>>();
    }
    if (json.contains("authorization_response_iss_parameter_supported")) {
        metadata.authorization_response_iss_parameter_supported =
            json.at("authorization_response_iss_parameter_supported").get<bool>();
    }
    if (json.contains("client_id_metadata_document_supported")) {
        metadata.client_id_metadata_document_supported =
            json.at("client_id_metadata_document_supported").get<bool>();
    }
    if (json.contains("token_endpoint_auth_methods_supported")) {
        metadata.token_endpoint_auth_methods_supported =
            json.at("token_endpoint_auth_methods_supported").get<std::vector<std::string>>();
    }
}

struct OAuthDiscoveryClient::Impl {
    struct UrlComponents {
        std::string scheme;
        std::string authority;
        std::string path;
    };

    template <typename Metadata>
    struct DiscoveryOperation {
        std::shared_ptr<Impl> owner;
        std::string cache_key;
        std::vector<std::string> urls;
        std::size_t next_url{0};
        nlohmann::json response;
        std::optional<Metadata> metadata;
    };

    Impl(std::shared_ptr<OAuthHttpClient> client, std::chrono::seconds ttl)
        : http_client(std::move(client)), cache_ttl(ttl) {}

    static UrlComponents parse_url_components(const std::string& url) {
        UrlComponents result;
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) {
            throw std::invalid_argument("URL missing scheme: " + url);
        }
        result.scheme = url.substr(0, scheme_end);
        auto rest = url.substr(scheme_end + 3);

        const auto path_start = rest.find('/');
        if (path_start == std::string::npos) {
            result.authority = std::move(rest);
            result.path = "/";
        } else {
            result.authority = rest.substr(0, path_start);
            result.path = rest.substr(path_start);
        }
        return result;
    }

    template <typename Metadata>
    static Task<Metadata> return_cached(std::shared_ptr<Metadata> metadata) {
        co_return std::move(*metadata);
    }

    static Task<ProtectedResourceMetadata> discover_protected_resource(
        std::shared_ptr<Impl> owner, std::string resource_url,
        const std::optional<std::string>& challenge_metadata_url) {
        // A challenge-supplied URL is its own cache key, so a server that moves its metadata is
        // never served an entry discovered through the well-known fallback.
        auto cache_key = challenge_metadata_url.value_or(resource_url);
        {
            std::lock_guard lock(owner->cache_mutex);
            const auto iter = owner->resource_cache.find(cache_key);
            if (iter != owner->resource_cache.end() && !iter->second.is_expired()) {
                return return_cached(std::make_shared<ProtectedResourceMetadata>(iter->second.data));
            }
        }

        auto operation = std::make_shared<DiscoveryOperation<ProtectedResourceMetadata>>();
        operation->owner = std::move(owner);
        operation->cache_key = std::move(cache_key);

        if (challenge_metadata_url) {
            // The challenge named the location; take the server at its word and try nothing else.
            operation->urls.push_back(*challenge_metadata_url);
            return run_protected_discovery(std::move(operation));
        }

        const auto parsed = parse_url_components(resource_url);
        const auto base = parsed.scheme + "://" + parsed.authority;
        if (!parsed.path.empty() && parsed.path != "/") {
            auto path_part = parsed.path;
            if (path_part.front() == '/') {
                path_part.erase(0, 1);
            }
            operation->urls.push_back(base + "/.well-known/oauth-protected-resource/" + path_part);
        }
        operation->urls.push_back(base + "/.well-known/oauth-protected-resource");
        return run_protected_discovery(std::move(operation));
    }

    static Task<AuthServerMetadata> discover_auth_server(std::shared_ptr<Impl> owner,
                                                         std::string issuer_url) {
        {
            std::lock_guard lock(owner->cache_mutex);
            const auto iter = owner->auth_cache.find(issuer_url);
            if (iter != owner->auth_cache.end() && !iter->second.is_expired()) {
                return return_cached(std::make_shared<AuthServerMetadata>(iter->second.data));
            }
        }

        const auto parsed = parse_url_components(issuer_url);
        const auto base = parsed.scheme + "://" + parsed.authority;
        auto operation = std::make_shared<DiscoveryOperation<AuthServerMetadata>>();
        operation->owner = std::move(owner);
        operation->cache_key = std::move(issuer_url);

        if (!parsed.path.empty() && parsed.path != "/") {
            auto path_part = parsed.path;
            if (path_part.front() == '/') {
                path_part.erase(0, 1);
            }
            if (!path_part.empty() && path_part.back() == '/') {
                path_part.pop_back();
            }
            operation->urls.push_back(base + "/.well-known/oauth-authorization-server/" + path_part);
            operation->urls.push_back(base + "/.well-known/openid-configuration/" + path_part);
            operation->urls.push_back(operation->cache_key + "/.well-known/openid-configuration");
        } else {
            operation->urls.push_back(base + "/.well-known/oauth-authorization-server");
            operation->urls.push_back(base + "/.well-known/openid-configuration");
        }
        return run_auth_discovery(std::move(operation));
    }

    static Task<ProtectedResourceMetadata> run_protected_discovery(
        std::shared_ptr<DiscoveryOperation<ProtectedResourceMetadata>> operation) {
        while (operation->next_url < operation->urls.size()) {
            try {
                const auto index = operation->next_url++;
                operation->response =
                    co_await operation->owner->http_client->get_json(operation->urls[index]);
                operation->metadata = operation->response.get<ProtectedResourceMetadata>();

                std::lock_guard lock(operation->owner->cache_mutex);
                operation->owner->resource_cache[operation->cache_key] = {
                    *operation->metadata,
                    std::chrono::steady_clock::now() + operation->owner->cache_ttl,
                };
                co_return *operation->metadata;
            } catch (const MetadataPolicyError&) {
                // A refused target is a security decision, not a candidate that missed.
                throw;
            } catch (...) {
                continue;
            }
        }
        throw std::runtime_error("Failed to discover protected resource metadata for " +
                                 operation->cache_key);
    }

    static Task<AuthServerMetadata> run_auth_discovery(
        std::shared_ptr<DiscoveryOperation<AuthServerMetadata>> operation) {
        while (operation->next_url < operation->urls.size()) {
            try {
                const auto index = operation->next_url++;
                operation->response =
                    co_await operation->owner->http_client->get_json(operation->urls[index]);
                operation->metadata = operation->response.get<AuthServerMetadata>();

                std::lock_guard lock(operation->owner->cache_mutex);
                operation->owner->auth_cache[operation->cache_key] = {
                    *operation->metadata,
                    std::chrono::steady_clock::now() + operation->owner->cache_ttl,
                };
                co_return *operation->metadata;
            } catch (const MetadataPolicyError&) {
                // A refused target is a security decision, not a candidate that missed.
                throw;
            } catch (...) {
                continue;
            }
        }
        throw std::runtime_error("Failed to discover authorization server metadata for " +
                                 operation->cache_key);
    }

    std::shared_ptr<OAuthHttpClient> http_client;
    std::chrono::seconds cache_ttl;
    std::mutex cache_mutex;
    std::unordered_map<std::string, CachedEntry<ProtectedResourceMetadata>> resource_cache;
    std::unordered_map<std::string, CachedEntry<AuthServerMetadata>> auth_cache;
};

OAuthDiscoveryClient::OAuthDiscoveryClient(std::shared_ptr<OAuthHttpClient> http_client,
                                           std::chrono::seconds cache_ttl)
    : impl_(std::make_shared<Impl>(std::move(http_client), cache_ttl)) {}

Task<ProtectedResourceMetadata> OAuthDiscoveryClient::discover_protected_resource(
    const std::string& resource_url) {
    return Impl::discover_protected_resource(impl_, resource_url, std::nullopt);
}

Task<ProtectedResourceMetadata> OAuthDiscoveryClient::discover_protected_resource(
    const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url) {
    return Impl::discover_protected_resource(impl_, resource_url, challenge_metadata_url);
}

Task<AuthServerMetadata> OAuthDiscoveryClient::discover_auth_server(const std::string& issuer_url) {
    return Impl::discover_auth_server(impl_, issuer_url);
}

void OAuthDiscoveryClient::clear_cache() {
    std::lock_guard lock(impl_->cache_mutex);
    impl_->resource_cache.clear();
    impl_->auth_cache.clear();
}

namespace {

struct MiddlewareInvocation {
    TokenValidator validator;
    mcp::Context* context;
    nlohmann::json params;
    TypeErasedHandler next;
    std::string token;
};

Task<nlohmann::json> invoke_auth_middleware(std::shared_ptr<MiddlewareInvocation> invocation) {
    if (invocation->token.empty()) {
        throw std::runtime_error("Unauthorized: missing Bearer token");
    }
    if (!co_await invocation->validator(invocation->token)) {
        throw std::runtime_error("Unauthorized: invalid Bearer token");
    }
    co_return co_await invocation->next(*invocation->context, invocation->params);
}

}  // namespace

Middleware make_auth_middleware(TokenValidator validator) {
    return [validator = std::move(validator)](mcp::Context& context, const nlohmann::json& params,
                                              TypeErasedHandler next) -> Task<nlohmann::json> {
        auto invocation = std::make_shared<MiddlewareInvocation>();
        invocation->validator = validator;
        invocation->context = &context;
        invocation->params = params;
        invocation->next = std::move(next);
        if (params.contains("_meta") && params["_meta"].contains("auth_token")) {
            invocation->token = params["_meta"]["auth_token"].get<std::string>();
        }
        return invoke_auth_middleware(std::move(invocation));
    };
}

std::string extract_bearer_token(std::string_view auth_header_value) {
    constexpr std::string_view prefix = "Bearer ";
    if (auth_header_value.size() > prefix.size() &&
        auth_header_value.substr(0, prefix.size()) == prefix) {
        return std::string(auth_header_value.substr(prefix.size()));
    }
    return {};
}

struct OAuthAuthenticator::Impl {
    struct RefreshOperation {
        std::shared_ptr<Impl> owner;
        TokenResponse stored_token;
        std::optional<TokenResponse> new_token;
    };

    Impl(std::shared_ptr<TokenStore> store, std::shared_ptr<OAuthHttpClient> client,
         OAuthConfig oauth_config, std::string url)
        : token_store(std::move(store)),
          oauth_client(std::move(client)),
          config(std::move(oauth_config)),
          server_url(std::move(url)) {}

    static Task<bool> return_false() { co_return false; }

    static Task<bool> try_refresh(std::shared_ptr<Impl> owner) {
        auto stored = owner->token_store->load(owner->server_url);
        if (!stored || !stored->refresh_token) {
            return return_false();
        }

        auto operation = std::make_shared<RefreshOperation>();
        operation->owner = std::move(owner);
        operation->stored_token = std::move(*stored);
        return run_refresh(std::move(operation));
    }

    static Task<bool> run_refresh(std::shared_ptr<RefreshOperation> operation) {
        try {
            operation->new_token = co_await operation->owner->oauth_client->refresh_token(
                operation->owner->config, *operation->stored_token.refresh_token);
            if (!operation->new_token->refresh_token) {
                operation->new_token->refresh_token = operation->stored_token.refresh_token;
            }
            operation->owner->token_store->store(operation->owner->server_url,
                                                 std::move(*operation->new_token));
            co_return true;
        } catch (...) {
            co_return false;
        }
    }

    std::shared_ptr<TokenStore> token_store;
    std::shared_ptr<OAuthHttpClient> oauth_client;
    OAuthConfig config;
    std::string server_url;
};

OAuthAuthenticator::OAuthAuthenticator(std::shared_ptr<TokenStore> token_store,
                                       std::shared_ptr<OAuthHttpClient> oauth_client,
                                       OAuthConfig config, std::string server_url)
    : impl_(std::make_shared<Impl>(std::move(token_store), std::move(oauth_client), std::move(config),
                                   std::move(server_url))) {}

std::string OAuthAuthenticator::get_access_token() const {
    const auto token = impl_->token_store->load(impl_->server_url);
    return token ? token->access_token : std::string{};
}

Task<bool> OAuthAuthenticator::try_refresh_token() { return Impl::try_refresh(impl_); }

void OAuthAuthenticator::store_token(TokenResponse token) {
    impl_->token_store->store(impl_->server_url, std::move(token));
}

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

/// Choose the token endpoint authentication method from what the server advertises.
///
/// A server that publishes the list has told the client which methods it will accept, so the
/// client picks the strongest one it can actually satisfy rather than guessing. A server that
/// publishes nothing leaves the decision unset, which keeps the pre-existing behaviour of sending
/// the secret, when there is one, in the request body.
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
    struct ChallengeOperation {
        std::shared_ptr<Impl> owner;
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
        OAuthConfig token_config;
        TokenResponse stored_token;
        std::optional<TokenResponse> new_token;
    };

    Impl(const net::any_io_executor& executor, std::shared_ptr<TokenStore> store,
         OAuthAuthorizationConfig authorization_config, AuthorizationCallback authorization_callback)
        : token_store(std::move(store)),
          http_client(std::make_shared<OAuthHttpClient>(executor)),
          config(std::move(authorization_config)),
          callback(std::move(authorization_callback)) {
        // A bare `client_id` is the shorthand form of injected credentials, so the two spellings
        // reach the same terminal decision instead of one of them quietly permitting registration.
        if (!config.client_identity.pre_registered && !config.client_id.empty()) {
            OAuthClientInformation injected;
            injected.client_id = config.client_id;
            injected.client_secret = config.client_secret;
            injected.source = ClientIdentitySource::pre_registered;
            config.client_identity.pre_registered = std::move(injected);
        }
        if (config.client_identity.metadata.redirect_uris.empty() && !config.redirect_uri.empty()) {
            config.client_identity.metadata.redirect_uris.push_back(config.redirect_uri);
        }
        http_client->set_metadata_policy(config.policy);
        if (config.host_resolver) {
            http_client->set_host_resolver(config.host_resolver);
        }
        discovery = std::make_shared<OAuthDiscoveryClient>(http_client);
    }

    static Task<bool> return_false() { co_return false; }

    /// Challenge scope is authoritative; `scopes_supported` is the fallback; otherwise no scope is
    /// requested at all. An explicit configured scope overrides both.
    static std::optional<std::string> select_scope(const Impl& owner, const BearerChallenge& challenge,
                                                   const ProtectedResourceMetadata& resource) {
        if (owner.config.scope) {
            return owner.config.scope;
        }
        if (challenge.scope && !challenge.scope->empty()) {
            return challenge.scope;
        }
        if (resource.scopes_supported && !resource.scopes_supported->empty()) {
            return join_scopes(*resource.scopes_supported);
        }
        return std::nullopt;
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
            case ClientIdentityDecision::use_pre_registered: {
                auto identity = *owner.config.client_identity.pre_registered;
                identity.source = ClientIdentitySource::pre_registered;
                identity.issuer = operation->facts.issuer;
                co_return identity;
            }
            case ClientIdentityDecision::use_client_id_metadata_document: {
                // The document URL is the client identifier itself, so there is nothing to
                // register and nothing to persist.
                OAuthClientInformation identity;
                identity.client_id = *owner.config.client_identity.client_metadata_url;
                identity.issuer = operation->facts.issuer;
                identity.source = ClientIdentitySource::client_id_metadata_document;
                co_return identity;
            }
            case ClientIdentityDecision::reuse_stored_registration:
                co_return *operation->stored_identity;
            case ClientIdentityDecision::register_dynamically:
                break;
            case ClientIdentityDecision::unavailable:
                throw std::runtime_error("No client identity is available for authorization server " +
                                         operation->facts.issuer);
        }

        operation->registration_response = co_await owner.http_client->post_json(
            *operation->facts.registration_endpoint,
            build_registration_request(owner.config.client_identity.metadata, operation->facts));

        auto registered = operation->registration_response.get<OAuthClientInformation>();
        if (registered.client_id.empty()) {
            throw std::runtime_error("Client registration response omitted client_id");
        }
        registered.issuer = operation->facts.issuer;
        registered.source = ClientIdentitySource::dynamic_registration;
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
        request.scope = select_scope(owner, challenge, resource);
        request.resource = resource.resource.empty() ? owner.config.server_url : resource.resource;

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
            append_query(auth_server.authorization_endpoint, detail::build_form_body(params));
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

        // The challenge's own metadata URL wins over the well-known fallback order.
        operation->resource_metadata = co_await owner.discovery->discover_protected_resource(
            owner.config.server_url, operation->challenge.resource_metadata);
        if (operation->resource_metadata->authorization_servers.empty()) {
            throw std::runtime_error("Protected resource metadata listed no authorization servers");
        }

        operation->auth_metadata = co_await owner.discovery->discover_auth_server(
            operation->resource_metadata->authorization_servers.front());
        if (operation->auth_metadata->authorization_endpoint.empty() ||
            operation->auth_metadata->token_endpoint.empty()) {
            throw std::runtime_error(
                "Authorization server metadata omitted an authorization or token endpoint");
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
            owner.last_request = operation->request;
            owner.last_identity = operation->identity;
            owner.token_config = operation->token_config;
        }

        operation->response = co_await owner.callback(operation->request);
        const auto validation =
            validate_authorization_response(operation->request, operation->response);
        if (!validation.accepted()) {
            throw std::runtime_error("Authorization response rejected: " + validation.message);
        }

        operation->token = co_await owner.http_client->exchange_code(
            operation->token_config, *operation->response.code, operation->request.code_verifier);
        owner.token_store->store(owner.config.server_url, operation->token);
        co_return true;
    }

    static Task<bool> handle_challenge(std::shared_ptr<Impl> owner, const std::string& header) {
        auto challenge = select_bearer_challenge(parse_www_authenticate(header));
        if (!challenge) {
            return return_false();
        }
        auto operation = std::make_shared<ChallengeOperation>();
        operation->owner = std::move(owner);
        operation->challenge = std::move(*challenge);
        return run_challenge(std::move(operation));
    }

    static Task<bool> try_refresh(std::shared_ptr<Impl> owner) {
        auto stored = owner->token_store->load(owner->config.server_url);
        if (!stored || !stored->refresh_token) {
            return return_false();
        }

        auto operation = std::make_shared<RefreshOperation>();
        {
            std::lock_guard lock(owner->state_mutex);
            if (!owner->token_config) {
                return return_false();
            }
            operation->token_config = *owner->token_config;
        }
        operation->owner = std::move(owner);
        operation->stored_token = std::move(*stored);
        return run_refresh(std::move(operation));
    }

    static Task<bool> run_refresh(std::shared_ptr<RefreshOperation> operation) {
        try {
            operation->new_token = co_await operation->owner->http_client->refresh_token(
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
            co_return false;
        }
    }

    std::shared_ptr<TokenStore> token_store;
    std::shared_ptr<OAuthHttpClient> http_client;
    std::shared_ptr<OAuthDiscoveryClient> discovery;
    OAuthAuthorizationConfig config;
    AuthorizationCallback callback;
    mutable std::mutex state_mutex;
    std::optional<AuthorizationRequest> last_request;
    std::optional<OAuthClientInformation> last_identity;
    std::optional<OAuthConfig> token_config;
};

OAuthAuthorizationManager::OAuthAuthorizationManager(const net::any_io_executor& executor,
                                                     std::shared_ptr<TokenStore> token_store,
                                                     OAuthAuthorizationConfig config,
                                                     AuthorizationCallback callback) {
    if (!token_store || !callback) {
        throw std::invalid_argument(
            "OAuthAuthorizationManager requires a token store and an authorization callback");
    }
    impl_ = std::make_shared<Impl>(executor, std::move(token_store), std::move(config),
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

std::optional<AuthorizationRequest> OAuthAuthorizationManager::last_authorization_request() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->last_request;
}

std::optional<OAuthClientInformation> OAuthAuthorizationManager::last_client_identity() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->last_identity;
}

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
    };

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
            return nlohmann::json(request).dump();
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
        return nlohmann::json(request).dump();
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

    static Task<void> run_write(std::shared_ptr<WriteOperation> operation) {
        try {
            co_await operation->owner->inner->write_message(*operation->outgoing);
        } catch (const mcp::HttpStatusError& error) {
            operation->authentication_challenge =
                operation->owner->uses_http_authorization_header &&
                error.status() == static_cast<unsigned int>(http::status::unauthorized);
            if (operation->authentication_challenge) {
                operation->authenticate_challenge = error.authenticate_challenge();
            }
            operation->write_error = std::current_exception();
        } catch (...) {
            operation->write_error = std::current_exception();
        }

        if (!operation->write_error) {
            co_return;
        }
        if (operation->authentication_challenge) {
            bool refreshed = false;
            try {
                // A challenge that names its metadata can drive a full authorization exchange;
                // renewing an existing grant is the fallback when it cannot.
                if (!operation->authenticate_challenge.empty()) {
                    refreshed = co_await operation->owner->authenticator->try_handle_challenge(
                        operation->authenticate_challenge);
                }
                if (!refreshed) {
                    refreshed = co_await operation->owner->authenticator->try_refresh_token();
                }
            } catch (...) {
                erase_pending(operation);
                throw;
            }
            if (refreshed) {
                try {
                    co_await operation->owner->inner->write_message(*operation->outgoing);
                } catch (...) {
                    erase_pending(operation);
                    throw;
                }
                co_return;
            }
        }
        erase_pending(operation);
        std::rethrow_exception(operation->write_error);
    }

    static std::shared_ptr<const std::string> take_retry_wire(const std::shared_ptr<Impl>& owner,
                                                              std::string_view raw) {
        try {
            const auto json_message = nlohmann::json::parse(raw);
            const auto message = json_message.get<JSONRPCMessage>();

            std::optional<std::string> key;
            bool unauthorized = false;
            if (const auto* result = std::get_if<JSONRPCResultResponse>(&message)) {
                key = result->id.correlation_key();
            } else if (const auto* error = std::get_if<JSONRPCErrorResponse>(&message);
                       error && error->id) {
                key = error->id->correlation_key();
                unauthorized = error->error.code == g_UNAUTHORIZED;
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
        if (operation->retry_wire && co_await operation->owner->authenticator->try_refresh_token()) {
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
    impl_->inner->close();
}

}  // namespace mcp::auth
