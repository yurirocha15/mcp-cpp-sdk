#pragma once

#include "../../src/detail/encoding.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mcp_test {

struct EvpKeyDeleter {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
using EvpKey = std::unique_ptr<EVP_PKEY, EvpKeyDeleter>;

/// A freshly generated RSA key (2048 bits) or P-256 key.
inline EvpKey generate_key(bool rsa) {
    EvpKey key(rsa ? EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(2048))
                   : EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!key) {
        throw std::runtime_error("key generation failed");
    }
    return key;
}

inline std::string private_key_pem(EVP_PKEY* key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const auto length = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, static_cast<std::size_t>(length));
}

/// A key the tests share, so each run generates one of each kind.
inline EVP_PKEY* shared_rsa_key() {
    static const EvpKey key = generate_key(true);
    return key.get();
}

inline EVP_PKEY* shared_ec_key() {
    static const EvpKey key = generate_key(false);
    return key.get();
}

/// Decodes a base64url segment of a compact JWS.
inline std::string base64url_decode(std::string_view text) {
    auto decoded = mcp::detail::base64url_decode(text);
    if (!decoded) {
        throw std::runtime_error("not base64url");
    }
    return *decoded;
}

struct DecodedJwt {
    nlohmann::json header;
    nlohmann::json claims;
    bool signature_valid{false};
};

/// Splits a compact JWS and verifies its signature with the public half of @p key. @p algorithm is
/// the one the test expects the header to name.
inline DecodedJwt verify_jwt(const std::string& jwt, EVP_PKEY* key, const std::string& algorithm) {
    const auto first = jwt.find('.');
    const auto second = jwt.find('.', first + 1);
    DecodedJwt decoded;
    decoded.header = nlohmann::json::parse(base64url_decode(jwt.substr(0, first)));
    decoded.claims = nlohmann::json::parse(base64url_decode(jwt.substr(first + 1, second - first - 1)));
    const auto input = jwt.substr(0, second);
    auto signature = base64url_decode(jwt.substr(second + 1));

    if (algorithm == "ES256") {
        if (signature.size() != 64) {
            return decoded;
        }
        std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> parsed(ECDSA_SIG_new(), ECDSA_SIG_free);
        auto* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()), 32, nullptr);
        auto* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()) + 32, 32, nullptr);
        ECDSA_SIG_set0(parsed.get(), r, s);
        unsigned char* der = nullptr;
        const auto der_length = i2d_ECDSA_SIG(parsed.get(), &der);
        signature.assign(reinterpret_cast<char*>(der), static_cast<std::size_t>(der_length));
        OPENSSL_free(der);
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* key_context = nullptr;
    EVP_DigestVerifyInit(context.get(), &key_context, EVP_sha256(), nullptr, key);
    if (algorithm == "PS256") {
        EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST);
        EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, EVP_sha256());
    }
    decoded.signature_valid =
        EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.data()),
                         signature.size(), reinterpret_cast<const unsigned char*>(input.data()),
                         input.size()) == 1;
    return decoded;
}

}  // namespace mcp_test
