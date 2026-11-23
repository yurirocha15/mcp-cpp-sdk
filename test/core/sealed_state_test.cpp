#include "mcp/detail/server/sealed_state.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/text/encoding.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sealed_state_test {

namespace ss = mcp::detail::sealed_state;
namespace sealed_token = mcp::detail::constants::sealed_token;

namespace {

ss::SealKey make_key(std::uint8_t kid_seed, std::uint8_t secret_seed) {
    std::array<std::uint8_t, sealed_token::g_KID_SIZE> kid{};
    kid.fill(kid_seed);
    std::array<std::uint8_t, sealed_token::g_KEY_SIZE> secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) {
        secret[i] = static_cast<std::uint8_t>(secret_seed + i);
    }
    return ss::SealKey(kid, secret);
}

ss::Codec ring(std::vector<ss::SealKey> keys) { return ss::Codec(std::move(keys)); }

constexpr std::string_view g_payload =
    R"({"jti":"abc","r":1,"ans":{"elicitation-0":{"action":"accept"}}})";

}  // namespace

TEST(SealedStateTest, RoundTrip) {
    const auto codec = ring({make_key(1, 10)});
    const auto sealed = codec.seal(g_payload);
    ASSERT_TRUE(sealed);
    const auto opened = codec.open(sealed.token);
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.payload, g_payload);
}

TEST(SealedStateTest, EveryPayloadLengthUpToAFewBlocksRoundTrips) {
    const auto codec = ring({make_key(1, 10)});
    for (std::size_t n = 0; n <= 40; ++n) {
        const std::string payload(n, 'x');
        const auto sealed = codec.seal(payload);
        ASSERT_TRUE(sealed) << n;
        const auto opened = codec.open(sealed.token);
        ASSERT_TRUE(opened) << n;
        EXPECT_EQ(opened.payload, payload) << n;
    }
}

TEST(SealedStateTest, TokenIsUnpaddedBase64urlAndEachSealIsDifferent) {
    const auto codec = ring({make_key(1, 10)});
    const auto a = codec.seal(g_payload);
    const auto b = codec.seal(g_payload);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_NE(a.token, b.token);
    for (const char c : a.token) {
        EXPECT_TRUE((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_')
            << c;
    }
    const auto raw = mcp::detail::base64url_decode(a.token);
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw->size(), sealed_token::g_MIN_TOKEN_BYTES + g_payload.size());
    EXPECT_EQ(static_cast<std::uint8_t>((*raw)[0]), sealed_token::g_VERSION);
}

TEST(SealedStateTest, FlippingAnyByteOfTheTokenIsRejected) {
    const auto codec = ring({make_key(1, 10)});
    const auto sealed = codec.seal(g_payload);
    ASSERT_TRUE(sealed);
    const auto raw = mcp::detail::base64url_decode(sealed.token);
    ASSERT_TRUE(raw.has_value());
    std::size_t opened_ok = 0;
    std::size_t attempts = 0;
    for (std::size_t i = 0; i < raw->size(); ++i) {
        for (const unsigned char mask : {0x01U, 0x80U}) {
            std::string mutated = *raw;
            mutated[i] = static_cast<char>(static_cast<unsigned char>(mutated[i]) ^ mask);
            ++attempts;
            if (codec.open(mcp::detail::base64url_encode(mutated))) {
                ++opened_ok;
            }
        }
    }
    EXPECT_EQ(attempts, raw->size() * 2);
    EXPECT_EQ(opened_ok, 0U);
}

TEST(SealedStateTest, ChangingAnyCharacterOfTheTokenStringIsRejected) {
    const auto codec = ring({make_key(1, 10)});
    const auto sealed = codec.seal(g_payload);
    ASSERT_TRUE(sealed);
    std::size_t opened_ok = 0;
    for (std::size_t i = 0; i < sealed.token.size(); ++i) {
        std::string mutated = sealed.token;
        mutated[i] = mutated[i] == 'A' ? 'B' : 'A';
        if (codec.open(mutated)) {
            ++opened_ok;
        }
    }
    EXPECT_EQ(opened_ok, 0U);
}

TEST(SealedStateTest, TruncationAtEveryLengthIsRejected) {
    const auto codec = ring({make_key(1, 10)});
    const auto sealed = codec.seal(g_payload);
    ASSERT_TRUE(sealed);
    std::size_t opened_ok = 0;
    for (std::size_t n = 0; n < sealed.token.size(); ++n) {
        if (codec.open(std::string_view(sealed.token).substr(0, n))) {
            ++opened_ok;
        }
    }
    EXPECT_EQ(opened_ok, 0U);
    EXPECT_EQ(codec.open(std::string_view(sealed.token).substr(0, 8)).error, ss::OpenError::eTruncated);
}

TEST(SealedStateTest, AppendedDataIsRejected) {
    const auto codec = ring({make_key(1, 10)});
    const auto sealed = codec.seal(g_payload);
    ASSERT_TRUE(sealed);
    EXPECT_FALSE(codec.open(sealed.token + "AAAA"));
    EXPECT_FALSE(codec.open(sealed.token + "="));
}

TEST(SealedStateTest, WrongKeyFailsAuthentication) {
    const auto sealer = ring({make_key(1, 10)});
    const auto other = ring({make_key(1, 99)});
    const auto sealed = sealer.seal(g_payload);
    ASSERT_TRUE(sealed);
    const auto opened = other.open(sealed.token);
    EXPECT_FALSE(opened);
    EXPECT_EQ(opened.error, ss::OpenError::eAuthenticationFailed);
    EXPECT_TRUE(opened.payload.empty());
}

TEST(SealedStateTest, UnknownKidIsReportedApartFromABadTag) {
    const auto sealer = ring({make_key(1, 10)});
    const auto other = ring({make_key(2, 10)});
    const auto sealed = sealer.seal(g_payload);
    ASSERT_TRUE(sealed);
    EXPECT_EQ(other.open(sealed.token).error, ss::OpenError::eUnknownKid);
}

TEST(SealedStateTest, RotationKeepsOldTokensOpenWhileTheOldKeyIsInTheRing) {
    const auto old_codec = ring({make_key(1, 10)});
    const auto next_codec = ring({make_key(2, 20), make_key(1, 10)});
    const auto retired_codec = ring({make_key(2, 20)});

    const auto old_token = old_codec.seal(g_payload);
    const auto new_token = next_codec.seal(g_payload);
    ASSERT_TRUE(old_token);
    ASSERT_TRUE(new_token);

    const auto opened_old = next_codec.open(old_token.token);
    ASSERT_TRUE(opened_old);
    EXPECT_EQ(opened_old.payload, g_payload);
    EXPECT_TRUE(next_codec.open(new_token.token));

    // keys[0] seals: the new token is under k2 and the retired ring opens it.
    EXPECT_TRUE(retired_codec.open(new_token.token));
    EXPECT_FALSE(old_codec.open(new_token.token));

    const auto rejected = retired_codec.open(old_token.token);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error, ss::OpenError::eUnknownKid);
}

TEST(SealedStateTest, EmptyInputIsReported) {
    const auto codec = ring({make_key(1, 10)});
    EXPECT_EQ(codec.open("").error, ss::OpenError::eEmpty);
}

TEST(SealedStateTest, OversizedInputIsRejectedBeforeDecoding) {
    const ss::Codec codec({make_key(1, 10)}, 64);
    // Not valid base64url: only the size can explain the cause being too_large rather than
    // bad_encoding.
    EXPECT_EQ(codec.open(std::string(65, '!')).error, ss::OpenError::eTooLarge);
    EXPECT_EQ(codec.open(std::string(64, '!')).error, ss::OpenError::eBadEncoding);
}

TEST(SealedStateTest, SealRefusesAPayloadThatWouldExceedTheCap) {
    const ss::Codec codec({make_key(1, 10)}, 100);
    const auto fits = codec.seal(std::string(30, 'x'));
    ASSERT_TRUE(fits);
    EXPECT_LE(fits.token.size(), 100U);

    // The largest payload that fits makes a token of exactly the cap or just under it.
    std::size_t largest = 0;
    for (std::size_t n = 0; n < 200; ++n) {
        const auto result = codec.seal(std::string(n, 'x'));
        if (!result) {
            EXPECT_EQ(result.error, ss::SealError::eTooLarge);
            break;
        }
        EXPECT_LE(result.token.size(), 100U);
        largest = n;
    }
    EXPECT_GT(largest, 30U);
    EXPECT_LT(largest, 199U);
    EXPECT_EQ(codec.seal(std::string(largest + 1, 'x')).error, ss::SealError::eTooLarge);
}

TEST(SealedStateTest, Base64GarbageIsRejected) {
    const auto codec = ring({make_key(1, 10)});
    const std::vector<std::string> garbage = {"!!!!",
                                              "not base64 at all",
                                              "AAAA====",
                                              "AA==",
                                              "A",
                                              "AAAAA",
                                              "+/+/+/+/",
                                              "\x01\x02\x03\x04",
                                              std::string("AAAA\0AAAA", 9),
                                              "AAAA AAAA",
                                              "AAAA\nAAAA"};
    for (const auto& text : garbage) {
        const auto opened = codec.open(text);
        EXPECT_FALSE(opened);
        EXPECT_NE(opened.error, ss::OpenError::eNone);
    }
    EXPECT_EQ(codec.open("!!!!").error, ss::OpenError::eBadEncoding);
}

TEST(SealedStateTest, AnEmptyRingCannotSealButTheCodecStillRejectsTokens) {
    const ss::Codec empty_ring({});
    EXPECT_EQ(empty_ring.seal(g_payload).error, ss::SealError::eNoKey);
    const auto sealed = ring({make_key(1, 10)}).seal(g_payload);
    ASSERT_TRUE(sealed);
    EXPECT_EQ(empty_ring.open(sealed.token).error, ss::OpenError::eUnknownKid);
}

TEST(SealedStateTest, DuplicateKeyIdsAreRefused) {
    EXPECT_THROW(ss::Codec({make_key(1, 10), make_key(1, 20)}), std::invalid_argument);
}

TEST(SealedStateTest, UnknownVersionIsReportedBeforeTheKeyLookup) {
    const auto codec = ring({make_key(1, 10)});
    const auto sealed = codec.seal(g_payload);
    ASSERT_TRUE(sealed);
    auto raw = *mcp::detail::base64url_decode(sealed.token);
    raw[0] = 2;
    EXPECT_EQ(codec.open(mcp::detail::base64url_encode(raw)).error, ss::OpenError::eBadVersion);
}

TEST(SealedStateTest, KeysAreCopyableForARuntimeSwap) {
    const std::vector<ss::SealKey> keys = {make_key(1, 10), make_key(2, 20)};
    const ss::Codec a(keys);
    const ss::Codec b(keys);
    const auto sealed = a.seal(g_payload);
    ASSERT_TRUE(sealed);
    EXPECT_TRUE(b.open(sealed.token));
}

TEST(SealedStateTest, SaltsDoNotRepeatAcrossManySeals) {
    const auto codec = ring({make_key(1, 10)});
    std::set<std::string> salts;
    for (int i = 0; i < 200; ++i) {
        const auto sealed = codec.seal("x");
        ASSERT_TRUE(sealed);
        const auto raw = *mcp::detail::base64url_decode(sealed.token);
        salts.insert(raw.substr(1 + sealed_token::g_KID_SIZE, sealed_token::g_SALT_SIZE));
    }
    EXPECT_EQ(salts.size(), 200U);
}

}  // namespace sealed_state_test
