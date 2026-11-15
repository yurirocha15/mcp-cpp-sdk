#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "constants.hpp"

namespace mcp::detail {

/// Sealed tokens: bytes that only a holder of the key ring can produce or read, carried as an opaque
/// string. The wire form is `base64url(ver(1) | kid(4) | salt(16) | ciphertext | tag(16))` with
/// `ver` 1. Each token gets its own AES-256-GCM key, HKDF-SHA256 of the master key with a fresh
/// 128-bit random salt, so the fixed zero nonce is never reused under one key. The AAD is
/// `ver | kid | salt`. This layer only seals and opens bytes; what the bytes mean, and when a token
/// has expired, is for the caller.
namespace sealed_state {

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

enum class SealError {
    none,
    /// The ring has no keys.
    no_key,
    /// The token would be longer than the cap.
    too_large,
    /// OpenSSL failed (allocation, random source, cipher or KDF).
    crypto_failure,
};

enum class OpenError {
    none,
    empty,
    /// Longer than the cap; checked before any decoding.
    too_large,
    /// Not canonical unpadded base64url.
    bad_encoding,
    /// Decoded to fewer bytes than a header and a tag.
    truncated,
    bad_version,
    /// The kid names no key in the ring. Counted apart from a bad tag.
    unknown_kid,
    /// The tag did not verify: tampered, truncated, or sealed under another secret.
    authentication_failed,
    /// OpenSSL failed for a reason other than the tag.
    crypto_failure,
};

struct SealResult {
    std::string token;
    SealError error = SealError::none;
    [[nodiscard]] explicit operator bool() const noexcept { return error == SealError::none; }
};

struct OpenResult {
    std::string payload;
    OpenError error = OpenError::none;
    [[nodiscard]] explicit operator bool() const noexcept { return error == OpenError::none; }
};

/// Canonical unpadded base64url (RFC 4648 section 5). The decoder rejects padding, other
/// characters, a length that is 1 mod 4, and non-zero trailing bits, so one byte string has exactly
/// one accepted spelling.
std::string base64url_encode(std::string_view bytes);
std::optional<std::string> base64url_decode(std::string_view text);

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

}  // namespace sealed_state

}  // namespace mcp::detail
