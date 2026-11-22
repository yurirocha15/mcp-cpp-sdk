#include "sealed_state.hpp"
#include "../detail/constants.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace encoding = mcp::detail::constants::encoding;
namespace sealed_token = mcp::detail::constants::sealed_token;

namespace mcp::detail::sealed_state {

namespace {

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
struct KdfDeleter {
    void operator()(EVP_KDF* kdf) const noexcept { EVP_KDF_free(kdf); }
};
struct KdfCtxDeleter {
    void operator()(EVP_KDF_CTX* ctx) const noexcept { EVP_KDF_CTX_free(ctx); }
};

using Derived = std::array<std::uint8_t, sealed_token::g_KEY_SIZE>;

/// Wipes a derived key on every exit path.
struct DerivedGuard {
    Derived key{};
    ~DerivedGuard() { OPENSSL_cleanse(key.data(), key.size()); }
};

bool derive_key(const SealKey& master, const std::uint8_t* salt, DerivedGuard& out) {
    const std::unique_ptr<EVP_KDF, KdfDeleter> kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr));
    if (!kdf) {
        return false;
    }
    const std::unique_ptr<EVP_KDF_CTX, KdfCtxDeleter> ctx(EVP_KDF_CTX_new(kdf.get()));
    if (!ctx) {
        return false;
    }
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                          const_cast<std::uint8_t*>(master.secret().data()),
                                          master.secret().size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t*>(salt),
                                          sealed_token::g_SALT_SIZE),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                          const_cast<char*>(sealed_token::g_HKDF_INFO.data()),
                                          sealed_token::g_HKDF_INFO.size()),
        OSSL_PARAM_construct_end()};
    return EVP_KDF_derive(ctx.get(), out.key.data(), out.key.size(), params) == 1;
}

int to_int(std::size_t value) { return static_cast<int>(value); }

}  // namespace

SealKey::~SealKey() { OPENSSL_cleanse(secret_.data(), secret_.size()); }

std::string base64url_encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16U) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8U) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += encoding::g_BASE64URL_ALPHABET[(v >> 18U) & 63U];
        out += encoding::g_BASE64URL_ALPHABET[(v >> 12U) & 63U];
        out += encoding::g_BASE64URL_ALPHABET[(v >> 6U) & 63U];
        out += encoding::g_BASE64URL_ALPHABET[v & 63U];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16U;
        out += encoding::g_BASE64URL_ALPHABET[(v >> 18U) & 63U];
        out += encoding::g_BASE64URL_ALPHABET[(v >> 12U) & 63U];
    } else if (rest == 2) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16U) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8U);
        out += encoding::g_BASE64URL_ALPHABET[(v >> 18U) & 63U];
        out += encoding::g_BASE64URL_ALPHABET[(v >> 12U) & 63U];
        out += encoding::g_BASE64URL_ALPHABET[(v >> 6U) & 63U];
    }
    return out;
}

std::optional<std::string> base64url_decode(std::string_view text) {
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned acc = 0;
    unsigned bits = 0;
    for (const char c : text) {
        const std::size_t pos = encoding::g_BASE64URL_ALPHABET.find(c);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }
        acc = (acc << 6U) | static_cast<unsigned>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFFU);
            acc &= (1U << bits) - 1U;
        }
    }
    if (acc != 0) {
        return std::nullopt;
    }
    return out;
}

Codec::Codec(std::vector<SealKey> keys, std::size_t max_token_chars)
    : keys_(std::move(keys)), max_token_chars_(max_token_chars) {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        for (std::size_t j = i + 1; j < keys_.size(); ++j) {
            if (keys_[i].kid() == keys_[j].kid()) {
                throw std::invalid_argument("sealed state key ring has a duplicate key id");
            }
        }
    }
}

SealResult Codec::seal(std::string_view payload) const {
    if (keys_.empty()) {
        return {{}, SealError::eNoKey};
    }
    const std::size_t raw_size =
        sealed_token::g_HEADER_SIZE + payload.size() + sealed_token::g_TAG_SIZE;
    const std::size_t encoded_size = (raw_size * 4 + 2) / 3;
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) -
                             sealed_token::g_MIN_TOKEN_BYTES ||
        encoded_size > max_token_chars_) {
        return {{}, SealError::eTooLarge};
    }

    const SealKey& key = keys_.front();
    std::string raw(raw_size, '\0');
    auto* out = reinterpret_cast<std::uint8_t*>(raw.data());
    out[0] = sealed_token::g_VERSION;
    std::memcpy(out + 1, key.kid().data(), sealed_token::g_KID_SIZE);
    std::uint8_t* salt = out + 1 + sealed_token::g_KID_SIZE;
    if (RAND_bytes(salt, to_int(sealed_token::g_SALT_SIZE)) != 1) {
        return {{}, SealError::eCryptoFailure};
    }

    DerivedGuard derived;
    if (!derive_key(key, salt, derived)) {
        return {{}, SealError::eCryptoFailure};
    }
    const std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return {{}, SealError::eCryptoFailure};
    }
    const std::array<std::uint8_t, sealed_token::g_NONCE_SIZE> nonce{};
    std::uint8_t* ciphertext = out + sealed_token::g_HEADER_SIZE;
    int len = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, derived.key.data(), nonce.data()) ==
            1 &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &len, out, to_int(sealed_token::g_HEADER_SIZE)) == 1;
    if (ok && !payload.empty()) {
        ok = EVP_EncryptUpdate(ctx.get(), ciphertext, &len,
                               reinterpret_cast<const unsigned char*>(payload.data()),
                               to_int(payload.size())) == 1 &&
             static_cast<std::size_t>(len) == payload.size();
    }
    int final_len = 0;
    ok = ok && EVP_EncryptFinal_ex(ctx.get(), ciphertext + payload.size(), &final_len) == 1 &&
         final_len == 0 &&
         EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, to_int(sealed_token::g_TAG_SIZE),
                             ciphertext + payload.size()) == 1;
    if (!ok) {
        return {{}, SealError::eCryptoFailure};
    }
    return {base64url_encode(raw), SealError::eNone};
}

OpenResult Codec::open(std::string_view token) const {
    if (token.empty()) {
        return {{}, OpenError::eEmpty};
    }
    if (token.size() > max_token_chars_) {
        return {{}, OpenError::eTooLarge};
    }
    const auto decoded = base64url_decode(token);
    if (!decoded) {
        return {{}, OpenError::eBadEncoding};
    }
    if (decoded->size() < sealed_token::g_MIN_TOKEN_BYTES) {
        return {{}, OpenError::eTruncated};
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(decoded->data());
    if (raw[0] != sealed_token::g_VERSION) {
        return {{}, OpenError::eBadVersion};
    }
    const auto found = std::find_if(keys_.begin(), keys_.end(), [&](const SealKey& key) {
        return std::memcmp(key.kid().data(), raw + 1, sealed_token::g_KID_SIZE) == 0;
    });
    if (found == keys_.end()) {
        return {{}, OpenError::eUnknownKid};
    }

    const std::uint8_t* salt = raw + 1 + sealed_token::g_KID_SIZE;
    const std::size_t cipher_size = decoded->size() - sealed_token::g_MIN_TOKEN_BYTES;
    const std::uint8_t* ciphertext = raw + sealed_token::g_HEADER_SIZE;
    const std::uint8_t* tag = ciphertext + cipher_size;

    DerivedGuard derived;
    if (!derive_key(*found, salt, derived)) {
        return {{}, OpenError::eCryptoFailure};
    }
    const std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return {{}, OpenError::eCryptoFailure};
    }
    const std::array<std::uint8_t, sealed_token::g_NONCE_SIZE> nonce{};
    std::string payload(cipher_size, '\0');
    int len = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, derived.key.data(), nonce.data()) !=
            1 ||
        EVP_DecryptUpdate(ctx.get(), nullptr, &len, raw, to_int(sealed_token::g_HEADER_SIZE)) != 1) {
        return {{}, OpenError::eCryptoFailure};
    }
    if (cipher_size != 0 &&
        (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(payload.data()), &len,
                           ciphertext, to_int(cipher_size)) != 1 ||
         static_cast<std::size_t>(len) != cipher_size)) {
        return {{}, OpenError::eCryptoFailure};
    }
    std::array<std::uint8_t, sealed_token::g_TAG_SIZE> tag_copy;
    std::memcpy(tag_copy.data(), tag, sealed_token::g_TAG_SIZE);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, to_int(sealed_token::g_TAG_SIZE),
                            tag_copy.data()) != 1) {
        return {{}, OpenError::eCryptoFailure};
    }
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(payload.data()) + cipher_size,
                            &final_len) != 1) {
        OPENSSL_cleanse(payload.data(), payload.size());
        return {{}, OpenError::eAuthenticationFailed};
    }
    return {std::move(payload), OpenError::eNone};
}

}  // namespace mcp::detail::sealed_state
