#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "mcp/detail/constants.hpp"

/// Sealed tokens: bytes that only a holder of the key ring can produce or read, carried as an opaque
/// string. The wire form is `base64url(ver(1) | kid(4) | salt(16) | ciphertext | tag(16))` with
/// `ver` 1. Each token gets its own AES-256-GCM key, HKDF-SHA256 of the master key with a fresh
/// 128-bit random salt, so the fixed zero nonce is never reused under one key. The AAD is
/// `ver | kid | salt`. This layer only seals and opens bytes; what the bytes mean, and when a token
/// has expired, is for the caller.
namespace mcp::detail::sealed_state {

/// One master key and the id that names it in a token. Each SealKey overwrites its own copy of the
/// secret with `OPENSSL_cleanse` when it is destroyed, and the secret is never printed. A copy
/// taken from secret() is the caller's to overwrite.
class SealKey {
   public:
    SealKey(std::array<std::uint8_t, mcp::detail::constants::sealed_token::g_KID_SIZE> kid,
            const std::array<std::uint8_t, mcp::detail::constants::sealed_token::g_KEY_SIZE>& secret)
        : kid_(kid), secret_(secret) {}
    SealKey(const SealKey&) = default;
    SealKey& operator=(const SealKey&) = default;
    SealKey(SealKey&&) noexcept = default;
    SealKey& operator=(SealKey&&) noexcept = default;
    ~SealKey();

    [[nodiscard]] const std::array<std::uint8_t, mcp::detail::constants::sealed_token::g_KID_SIZE>&
    kid() const noexcept {
        return kid_;
    }
    [[nodiscard]] const std::array<std::uint8_t, mcp::detail::constants::sealed_token::g_KEY_SIZE>&
    secret() const noexcept {
        return secret_;
    }

   private:
    std::array<std::uint8_t, mcp::detail::constants::sealed_token::g_KID_SIZE> kid_;
    std::array<std::uint8_t, mcp::detail::constants::sealed_token::g_KEY_SIZE> secret_;
};

enum class SealError : std::uint8_t {
    eNone,
    /// The ring has no keys.
    eNoKey,
    /// The token would be longer than the cap.
    eTooLarge,
    /// OpenSSL failed (allocation, random source, cipher or KDF).
    eCryptoFailure,
};

enum class OpenError : std::uint8_t {
    eNone,
    eEmpty,
    /// Longer than the cap; checked before any decoding.
    eTooLarge,
    /// Not canonical unpadded base64url.
    eBadEncoding,
    /// Decoded to fewer bytes than a header and a tag.
    eTruncated,
    eBadVersion,
    /// The kid names no key in the ring. Counted apart from a bad tag.
    eUnknownKid,
    /// The tag did not verify: tampered, truncated, or sealed under another secret.
    eAuthenticationFailed,
    /// OpenSSL failed for a reason other than the tag.
    eCryptoFailure,
};

struct SealResult {
    std::string token;
    SealError error = SealError::eNone;
    [[nodiscard]] explicit operator bool() const noexcept { return error == SealError::eNone; }
};

struct OpenResult {
    std::string payload;
    OpenError error = OpenError::eNone;
    [[nodiscard]] explicit operator bool() const noexcept { return error == OpenError::eNone; }
};

/// A key ring: `keys[0]` seals, every key opens. Immutable once built, so one codec can be used from
/// any number of threads; rotating keys means building a new codec.
class Codec {
   public:
    /// Throws `std::invalid_argument` if two keys share a kid.
    explicit Codec(
        std::vector<SealKey> keys,
        std::size_t max_token_chars = mcp::detail::constants::sealed_token::g_DEFAULT_MAX_TOKEN_CHARS);

    [[nodiscard]] SealResult seal(std::string_view payload) const;
    [[nodiscard]] OpenResult open(std::string_view token) const;

   private:
    std::vector<SealKey> keys_;
    std::size_t max_token_chars_;
};

}  // namespace mcp::detail::sealed_state
