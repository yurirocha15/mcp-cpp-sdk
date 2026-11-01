#include "sealed_state.hpp"

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

namespace mcp::detail::sealed_state {

namespace {

constexpr std::string_view g_hkdf_info = "mcp-cpp-sdk requestState v1";
constexpr std::string_view g_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::size_t g_nonce_size = 12;

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
struct KdfDeleter {
    void operator()(EVP_KDF* kdf) const noexcept { EVP_KDF_free(kdf); }
};
struct KdfCtxDeleter {
    void operator()(EVP_KDF_CTX* ctx) const noexcept { EVP_KDF_CTX_free(ctx); }
};

using Derived = std::array<std::uint8_t, g_key_size>;

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
                                          g_salt_size),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, const_cast<char*>(g_hkdf_info.data()),
                                          g_hkdf_info.size()),
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
        out += g_alphabet[(v >> 18U) & 63U];
        out += g_alphabet[(v >> 12U) & 63U];
        out += g_alphabet[(v >> 6U) & 63U];
        out += g_alphabet[v & 63U];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16U;
        out += g_alphabet[(v >> 18U) & 63U];
        out += g_alphabet[(v >> 12U) & 63U];
    } else if (rest == 2) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16U) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8U);
        out += g_alphabet[(v >> 18U) & 63U];
        out += g_alphabet[(v >> 12U) & 63U];
        out += g_alphabet[(v >> 6U) & 63U];
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
        const std::size_t pos = g_alphabet.find(c);
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
        return {{}, SealError::no_key};
    }
    const std::size_t raw_size = g_header_size + payload.size() + g_tag_size;
    const std::size_t encoded_size = (raw_size * 4 + 2) / 3;
    if (payload.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) - g_min_token_bytes ||
        encoded_size > max_token_chars_) {
        return {{}, SealError::too_large};
    }

    const SealKey& key = keys_.front();
    std::string raw(raw_size, '\0');
    auto* out = reinterpret_cast<std::uint8_t*>(raw.data());
    out[0] = g_version;
    std::memcpy(out + 1, key.kid().data(), g_kid_size);
    std::uint8_t* salt = out + 1 + g_kid_size;
    if (RAND_bytes(salt, to_int(g_salt_size)) != 1) {
        return {{}, SealError::crypto_failure};
    }

    DerivedGuard derived;
    if (!derive_key(key, salt, derived)) {
        return {{}, SealError::crypto_failure};
    }
    const std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return {{}, SealError::crypto_failure};
    }
    const std::array<std::uint8_t, g_nonce_size> nonce{};
    std::uint8_t* ciphertext = out + g_header_size;
    int len = 0;
    bool ok = EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, derived.key.data(),
                                 nonce.data()) == 1 &&
              EVP_EncryptUpdate(ctx.get(), nullptr, &len, out, to_int(g_header_size)) == 1;
    if (ok && !payload.empty()) {
        ok = EVP_EncryptUpdate(ctx.get(), ciphertext, &len,
                               reinterpret_cast<const unsigned char*>(payload.data()),
                               to_int(payload.size())) == 1 &&
             static_cast<std::size_t>(len) == payload.size();
    }
    int final_len = 0;
    ok = ok && EVP_EncryptFinal_ex(ctx.get(), ciphertext + payload.size(), &final_len) == 1 &&
         final_len == 0 &&
         EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, to_int(g_tag_size),
                             ciphertext + payload.size()) == 1;
    if (!ok) {
        return {{}, SealError::crypto_failure};
    }
    return {base64url_encode(raw), SealError::none};
}

OpenResult Codec::open(std::string_view token) const {
    if (token.empty()) {
        return {{}, OpenError::empty};
    }
    if (token.size() > max_token_chars_) {
        return {{}, OpenError::too_large};
    }
    const auto decoded = base64url_decode(token);
    if (!decoded) {
        return {{}, OpenError::bad_encoding};
    }
    if (decoded->size() < g_min_token_bytes) {
        return {{}, OpenError::truncated};
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(decoded->data());
    if (raw[0] != g_version) {
        return {{}, OpenError::bad_version};
    }
    const auto found = std::find_if(keys_.begin(), keys_.end(), [&](const SealKey& key) {
        return std::memcmp(key.kid().data(), raw + 1, g_kid_size) == 0;
    });
    if (found == keys_.end()) {
        return {{}, OpenError::unknown_kid};
    }

    const std::uint8_t* salt = raw + 1 + g_kid_size;
    const std::size_t cipher_size = decoded->size() - g_min_token_bytes;
    const std::uint8_t* ciphertext = raw + g_header_size;
    const std::uint8_t* tag = ciphertext + cipher_size;

    DerivedGuard derived;
    if (!derive_key(*found, salt, derived)) {
        return {{}, OpenError::crypto_failure};
    }
    const std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return {{}, OpenError::crypto_failure};
    }
    const std::array<std::uint8_t, g_nonce_size> nonce{};
    std::string payload(cipher_size, '\0');
    int len = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, derived.key.data(), nonce.data()) !=
            1 ||
        EVP_DecryptUpdate(ctx.get(), nullptr, &len, raw, to_int(g_header_size)) != 1) {
        return {{}, OpenError::crypto_failure};
    }
    if (cipher_size != 0 &&
        (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(payload.data()), &len,
                           ciphertext, to_int(cipher_size)) != 1 ||
         static_cast<std::size_t>(len) != cipher_size)) {
        return {{}, OpenError::crypto_failure};
    }
    std::array<std::uint8_t, g_tag_size> tag_copy;
    std::memcpy(tag_copy.data(), tag, g_tag_size);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, to_int(g_tag_size), tag_copy.data()) !=
        1) {
        return {{}, OpenError::crypto_failure};
    }
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(payload.data()) + cipher_size,
                            &final_len) != 1) {
        OPENSSL_cleanse(payload.data(), payload.size());
        return {{}, OpenError::authentication_failed};
    }
    return {std::move(payload), OpenError::none};
}

}  // namespace mcp::detail::sealed_state
