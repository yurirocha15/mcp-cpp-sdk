// The library's byte encodings: SHA-256, hexadecimal, base64, base64url, percent and form encoding.

#include "../../src/detail/text/encoding.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace encoding_test {

namespace {

using mcp::detail::base64_decode;
using mcp::detail::base64_encode;
using mcp::detail::base64url_decode;
using mcp::detail::base64url_encode;
using mcp::detail::digest_bytes;
using mcp::detail::form_body;
using mcp::detail::form_value;
using mcp::detail::hex_encode;
using mcp::detail::percent_decode;
using mcp::detail::percent_encode;
using mcp::detail::sha256;

std::string sha256_hex(const std::string& text) { return hex_encode(digest_bytes(sha256(text))); }

}  // namespace

TEST(Encoding, Sha256MatchesTheStandardVectors) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Encoding, HexIsLowercaseWithTwoDigitsPerByte) {
    EXPECT_EQ(hex_encode(""), "");
    EXPECT_EQ(hex_encode(std::string("\x00\x0f\xa0\xff", 4)), "000fa0ff");
}

TEST(Encoding, Base64PadsAndRoundTrips) {
    EXPECT_EQ(base64_encode(""), "");
    EXPECT_EQ(base64_encode("f"), "Zg==");
    EXPECT_EQ(base64_encode("fo"), "Zm8=");
    EXPECT_EQ(base64_encode("foo"), "Zm9v");
    EXPECT_EQ(base64_encode("Hello"), "SGVsbG8=");
    EXPECT_EQ(base64_encode(std::string("\xfb\xff", 2)), "+/8=");
    for (std::size_t n = 0; n < 50; ++n) {
        std::string bytes(n, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            bytes[i] = static_cast<char>(i * 37 + n);
        }
        EXPECT_EQ(base64_decode(base64_encode(bytes)), std::optional<std::string>(bytes)) << n;
    }
}

TEST(Encoding, Base64DecodingRefusesMalformedText) {
    EXPECT_EQ(base64_decode("Zg=="), std::optional<std::string>("f"));
    EXPECT_FALSE(base64_decode("Zg").has_value());
    EXPECT_FALSE(base64_decode("Z===").has_value());
    EXPECT_FALSE(base64_decode("Zg==Zg==").has_value());
    EXPECT_FALSE(base64_decode("Zg=A").has_value());
    EXPECT_FALSE(base64_decode("Zm9-").has_value());
    EXPECT_FALSE(base64_decode("Zm9v\n").has_value());
}

TEST(Encoding, Base64UrlHasOneSpellingPerByteString) {
    EXPECT_EQ(base64url_encode(""), "");
    EXPECT_EQ(base64url_encode("f"), "Zg");
    EXPECT_EQ(base64url_encode("fo"), "Zm8");
    EXPECT_EQ(base64url_encode("foo"), "Zm9v");
    EXPECT_EQ(base64url_encode(std::string("\xfb\xff", 2)), "-_8");
    EXPECT_EQ(base64url_decode("Zm9v"), std::optional<std::string>("foo"));
    EXPECT_EQ(base64url_decode("Zg"), std::optional<std::string>("f"));
    // "Zh" decodes to the same byte as "Zg" but with non-zero trailing bits.
    EXPECT_FALSE(base64url_decode("Zh").has_value());
    EXPECT_FALSE(base64url_decode("Zg==").has_value());
    EXPECT_FALSE(base64url_decode("Z").has_value());
    EXPECT_FALSE(base64url_decode("+/8").has_value());
    for (std::size_t n = 0; n < 50; ++n) {
        std::string bytes(n, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            bytes[i] = static_cast<char>(i * 37 + n);
        }
        EXPECT_EQ(base64url_decode(base64url_encode(bytes)), std::optional<std::string>(bytes)) << n;
    }
}

TEST(Encoding, PercentEncodingKeepsOnlyUnreservedCharacters) {
    EXPECT_EQ(percent_encode("hello world&foo=bar"), "hello%20world%26foo%3Dbar");
    EXPECT_EQ(percent_encode("abc-._~123"), "abc-._~123");
    EXPECT_EQ(percent_encode(std::string("\xc3\xa9\x00", 3)), "%C3%A9%00");
}

TEST(Encoding, PercentDecodingRefusesMalformedEscapes) {
    EXPECT_EQ(percent_decode("/%24defs/a%20b%25"), "/$defs/a b%");
    EXPECT_EQ(percent_decode("%4a%4A%c3%A9"), "JJ\xc3\xa9");
    EXPECT_FALSE(percent_decode("%2").has_value());
    EXPECT_FALSE(percent_decode("%zz").has_value());
    EXPECT_FALSE(percent_decode("abc%").has_value());
}

TEST(Encoding, FormBodyJoinsPercentEncodedPairs) {
    const std::vector<std::pair<std::string, std::string>> fields{
        {"grant_type", "authorization_code"}, {"code", "abc123"}, {"redirect_uri", "http://localhost"}};
    EXPECT_EQ(form_body(fields),
              "grant_type=authorization_code&code=abc123&redirect_uri=http%3A%2F%2Flocalhost");
    EXPECT_EQ(form_body({}), "");
}

TEST(Encoding, FormValueWritesSpacesAsPlus) {
    EXPECT_EQ(form_value("a b+c"), "a+b%2Bc");
    EXPECT_EQ(form_value(" "), "+");
    EXPECT_EQ(form_value("id:secret"), "id%3Asecret");
}

}  // namespace encoding_test
