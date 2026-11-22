#include <mcp/auth/client_authentication.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <array>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include "mcp/detail/constants.hpp"
#include "mcp/detail/text/encoding.hpp"
#include "mcp/detail/text/secure_random.hpp"

namespace mcp::auth {

namespace {

struct KeyDeleter {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
struct BioDeleter {
    void operator()(BIO* bio) const { BIO_free(bio); }
};
struct MdContextDeleter {
    void operator()(EVP_MD_CTX* context) const { EVP_MD_CTX_free(context); }
};
struct SignatureDeleter {
    void operator()(ECDSA_SIG* signature) const { ECDSA_SIG_free(signature); }
};
using KeyPtr = std::unique_ptr<EVP_PKEY, KeyDeleter>;

KeyPtr load_private_key(const std::string& pem) {
    const std::unique_ptr<BIO, BioDeleter> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    KeyPtr key(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
    if (!key) {
        throw std::invalid_argument("The private key is not a readable unencrypted PEM key");
    }
    return key;
}

bool is_p256(EVP_PKEY* key) {
    std::array<char, 32> group{};
    std::size_t length = 0;
    return EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group.data(), group.size(),
                                          &length) == 1 &&
           std::string(group.data(), length) == "prime256v1";
}

/// The algorithms @p key can sign with, most widely accepted first.
std::vector<std::string> algorithms_for(EVP_PKEY* key) {
    if (EVP_PKEY_is_a(key, "RSA")) {
        if (EVP_PKEY_bits(key) < mcp::detail::constants::client_authentication::g_MIN_RSA_BITS) {
            throw std::invalid_argument(
                "The RSA private key must be at least " +
                std::to_string(mcp::detail::constants::client_authentication::g_MIN_RSA_BITS) +
                " bits");
        }
        return {"RS256", "PS256"};
    }
    if (EVP_PKEY_is_a(key, "EC") && is_p256(key)) {
        return {"ES256"};
    }
    throw std::invalid_argument("The private key must be an RSA key or an EC key on P-256");
}

/// Converts the DER ECDSA signature OpenSSL produces into the fixed-width R || S of RFC 7518.
std::string es256_raw_signature(const std::string& der) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(der.data());
    const std::unique_ptr<ECDSA_SIG, SignatureDeleter> signature(
        d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size())));
    if (!signature) {
        throw std::runtime_error("Could not read the ECDSA signature");
    }
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(signature.get(), &r, &s);
    std::string raw(2 * mcp::detail::constants::client_authentication::g_ES256_COORDINATE_BYTES, '\0');
    auto* out = reinterpret_cast<unsigned char*>(raw.data());
    if (BN_bn2binpad(
            r, out,
            static_cast<int>(mcp::detail::constants::client_authentication::g_ES256_COORDINATE_BYTES)) <
            0 ||
        BN_bn2binpad(
            s, out + mcp::detail::constants::client_authentication::g_ES256_COORDINATE_BYTES,
            static_cast<int>(mcp::detail::constants::client_authentication::g_ES256_COORDINATE_BYTES)) <
            0) {
        throw std::runtime_error("Could not encode the ECDSA signature");
    }
    return raw;
}

std::string sign(EVP_PKEY* key, const std::string& algorithm, const std::string& input) {
    const std::unique_ptr<EVP_MD_CTX, MdContextDeleter> context(EVP_MD_CTX_new());
    EVP_PKEY_CTX* key_context = nullptr;
    if (!context || EVP_DigestSignInit(context.get(), &key_context, EVP_sha256(), nullptr, key) != 1) {
        throw std::runtime_error("Could not start signing the client assertion");
    }
    if (algorithm == "PS256") {
        // RFC 7518 section 3.5: SHA-256 for the digest and the mask, salt as long as the digest.
        if (EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) <= 0 ||
            EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
            EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, EVP_sha256()) <= 0) {
            throw std::runtime_error("Could not select PSS padding");
        }
    }
    std::size_t length = 0;
    if (EVP_DigestSign(context.get(), nullptr, &length,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
        throw std::runtime_error("Could not size the client assertion signature");
    }
    std::string signature(length, '\0');
    if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()), &length,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
        throw std::runtime_error("Could not sign the client assertion");
    }
    signature.resize(length);
    return algorithm == "ES256" ? es256_raw_signature(signature) : signature;
}

bool lists(const std::optional<std::vector<std::string>>& values, std::string_view value) {
    return values && std::find(values->begin(), values->end(), value) != values->end();
}

}  // namespace

std::string choose_signing_algorithm(const PrivateKeyJwtConfig& config,
                                     const std::optional<std::vector<std::string>>& supported) {
    const auto key = load_private_key(config.private_key_pem);
    const auto usable = algorithms_for(key.get());
    if (config.algorithm) {
        if (std::find(usable.begin(), usable.end(), *config.algorithm) == usable.end()) {
            throw std::invalid_argument("The private key cannot sign with " + *config.algorithm);
        }
        return *config.algorithm;
    }
    if (!supported || supported->empty()) {
        return usable.front();
    }
    for (const auto& algorithm : usable) {
        if (lists(supported, algorithm)) {
            return algorithm;
        }
    }
    throw std::runtime_error(
        "The authorization server lists no signing algorithm the private key can use");
}

std::string sign_client_assertion(const PrivateKeyJwtConfig& config, const std::string& client_id,
                                  const std::string& audience, const std::string& algorithm) {
    const auto key = load_private_key(config.private_key_pem);
    const auto usable = algorithms_for(key.get());
    if (std::find(usable.begin(), usable.end(), algorithm) == usable.end()) {
        throw std::invalid_argument("The private key cannot sign with " + algorithm);
    }

    nlohmann::json header = {{"alg", algorithm}};
    if (config.key_id) {
        header["kid"] = *config.key_id;
    }
    const auto issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const nlohmann::json claims = {
        {"iss", client_id},
        {"sub", client_id},
        {"aud", config.audience ? *config.audience : audience},
        {"iat", issued_at},
        {"exp", issued_at + config.lifetime.count()},
        {"jti", mcp::detail::generate_random_string(
                    mcp::detail::constants::client_authentication::g_JTI_LENGTH)}};
    const auto signing_input = mcp::detail::base64url_encode(header.dump()) + "." +
                               mcp::detail::base64url_encode(claims.dump());
    return signing_input + "." +
           mcp::detail::base64url_encode(sign(key.get(), algorithm, signing_input));
}

std::string authenticate_token_request(const ClientAuthentication& credentials,
                                       const AuthServerMetadata& server, KeyValuePairList& params,
                                       bool send_client_id) {
    const auto& listed = server.token_endpoint_auth_methods_supported;
    const bool lists_nothing = !listed || listed->empty();
    const auto acceptable = [&](std::string_view method) {
        return lists_nothing || lists(listed, method);
    };

    std::string method;
    if (credentials.method) {
        method = *credentials.method;
        if (!acceptable(method)) {
            throw std::runtime_error(
                "The authorization server does not list the token endpoint "
                "authentication method " +
                method);
        }
    } else if (credentials.private_key_jwt && acceptable("private_key_jwt")) {
        method = "private_key_jwt";
    } else if (credentials.client_secret && lists_nothing) {
        // RFC 8414 section 2: a server that lists nothing accepts client_secret_basic.
        method = "client_secret_basic";
    } else if (credentials.client_secret && lists(listed, "client_secret_basic")) {
        method = "client_secret_basic";
    } else if (credentials.client_secret && lists(listed, "client_secret_post")) {
        method = "client_secret_post";
    } else {
        throw std::runtime_error(
            "The authorization server lists no token endpoint authentication method the client's "
            "credentials fit");
    }

    if (method == "private_key_jwt") {
        if (!credentials.private_key_jwt) {
            throw std::invalid_argument("private_key_jwt needs a private key");
        }
        std::optional<std::vector<std::string>> signing_algorithms;
        if (server.raw.is_object() &&
            server.raw.contains("token_endpoint_auth_signing_alg_values_supported")) {
            signing_algorithms = server.raw.at("token_endpoint_auth_signing_alg_values_supported")
                                     .get<std::vector<std::string>>();
        }
        const auto algorithm =
            choose_signing_algorithm(*credentials.private_key_jwt, signing_algorithms);
        params.emplace_back(
            "client_assertion_type",
            std::string(mcp::detail::constants::client_authentication::g_ASSERTION_TYPE));
        params.emplace_back(
            "client_assertion",
            sign_client_assertion(*credentials.private_key_jwt, credentials.client_id,
                                  server.issuer.empty() ? server.token_endpoint : server.issuer,
                                  algorithm));
        if (send_client_id) {
            params.emplace_back("client_id", credentials.client_id);
        }
        return {};
    }
    if (method == "client_secret_basic" || method == "client_secret_post") {
        if (!credentials.client_secret) {
            throw std::invalid_argument(method + " needs a client secret");
        }
        if (method == "client_secret_post") {
            params.emplace_back("client_id", credentials.client_id);
            params.emplace_back("client_secret", *credentials.client_secret);
            return {};
        }
        return "Basic " +
               mcp::detail::base64_encode(mcp::detail::form_value(credentials.client_id) + ":" +
                                          mcp::detail::form_value(*credentials.client_secret));
    }
    if (method == "none") {
        params.emplace_back("client_id", credentials.client_id);
        return {};
    }
    throw std::invalid_argument("Unsupported token endpoint authentication method: " + method);
}

}  // namespace mcp::auth
