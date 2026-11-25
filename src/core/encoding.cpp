#include "mcp/detail/text/encoding.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace encoding = mcp::detail::constants::encoding;

namespace mcp::detail {

namespace {

std::uint32_t byte_at(std::string_view bytes, std::size_t index) {
    return static_cast<unsigned char>(bytes[index]);
}

/// @p bytes in the base64 @p alphabet, with `=` padding when @p pad is set.
std::string encode_base64(std::string_view bytes, std::string_view alphabet, bool pad) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        // Three bytes, or the one or two that are left, as the high bits of a 24-bit group.
        const std::size_t count = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t group = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            group = (group << encoding::g_BITS_PER_BYTE) | (i < count ? byte_at(bytes, index + i) : 0U);
        }
        // n bytes take n + 1 digits.
        for (std::size_t digit = 0; digit <= count; ++digit) {
            const auto shift = static_cast<unsigned>(3 - digit) * encoding::g_BASE64_DIGIT_BITS;
            out += alphabet[(group >> shift) & encoding::g_BASE64_DIGIT_MASK];
        }
        if (pad) {
            out.append(3 - count, '=');
        }
    }
    return out;
}

}  // namespace

Sha256Digest sha256(std::string_view bytes) {
    Sha256Digest digest{};
    unsigned int digest_length = 0;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                          EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &digest_length) != 1) {
        throw std::runtime_error("OpenSSL SHA-256 failed");
    }
    return digest;
}

std::string hex_encode(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        out += encoding::g_HEX_DIGITS[byte >> encoding::g_HEX_DIGIT_BITS];
        out += encoding::g_HEX_DIGITS[byte & encoding::g_HEX_DIGIT_MASK];
    }
    return out;
}

std::string base64_encode(std::string_view bytes) {
    return encode_base64(bytes, encoding::g_BASE64_ALPHABET, true);
}

std::optional<std::string> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t index = 0; index < text.size(); index += 4) {
        std::array<std::uint32_t, 4> quad{};
        std::size_t padding = 0;
        for (std::size_t offset = 0; offset < 4; ++offset) {
            const char character = text[index + offset];
            if (character == '=') {
                // Padding is only the last one or two characters of the final quad.
                if (index + 4 != text.size() || offset < 2) {
                    return std::nullopt;
                }
                ++padding;
                continue;
            }
            const int value = encoding::g_BASE64_VALUES[static_cast<unsigned char>(character)];
            if (padding != 0 || value < 0) {
                return std::nullopt;
            }
            quad[offset] = static_cast<std::uint32_t>(value);
        }
        std::uint32_t group = 0;
        for (const std::uint32_t value : quad) {
            group = (group << encoding::g_BASE64_DIGIT_BITS) | value;
        }
        // Four digits hold three bytes; each `=` stands for one byte fewer.
        for (std::size_t byte = 0; byte < 3 - padding; ++byte) {
            const auto shift = static_cast<unsigned>(2 - byte) * encoding::g_BITS_PER_BYTE;
            bytes += static_cast<char>((group >> shift) & encoding::g_BYTE_MASK);
        }
    }
    return bytes;
}

std::string base64url_encode(std::string_view bytes) {
    return encode_base64(bytes, encoding::g_BASE64URL_ALPHABET, false);
}

std::optional<std::string> base64url_decode(std::string_view text) {
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const char character : text) {
        const int value = encoding::g_BASE64URL_VALUES[static_cast<unsigned char>(character)];
        if (value < 0) {
            return std::nullopt;
        }
        accumulator = (accumulator << encoding::g_BASE64_DIGIT_BITS) | static_cast<unsigned>(value);
        bits += encoding::g_BASE64_DIGIT_BITS;
        if (bits >= encoding::g_BITS_PER_BYTE) {
            bits -= encoding::g_BITS_PER_BYTE;
            out += static_cast<char>((accumulator >> bits) & encoding::g_BYTE_MASK);
            accumulator &= (1U << bits) - 1U;
        }
    }
    if (accumulator != 0) {
        return std::nullopt;
    }
    return out;
}

std::string percent_encode(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (const char character : text) {
        if (encoding::g_UNRESERVED_CHARS.find(character) != std::string_view::npos) {
            out += character;
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        out += '%';
        out += encoding::g_HEX_DIGITS_UPPER[byte >> encoding::g_HEX_DIGIT_BITS];
        out += encoding::g_HEX_DIGITS_UPPER[byte & encoding::g_HEX_DIGIT_MASK];
    }
    return out;
}

std::optional<std::string> percent_decode(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '%') {
            decoded += text[index];
            continue;
        }
        if (index + 2 >= text.size()) {
            return std::nullopt;
        }
        const int high = encoding::g_HEX_VALUES[static_cast<unsigned char>(text[index + 1])];
        const int low = encoding::g_HEX_VALUES[static_cast<unsigned char>(text[index + 2])];
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        decoded += static_cast<char>((static_cast<unsigned>(high) << encoding::g_HEX_DIGIT_BITS) |
                                     static_cast<unsigned>(low));
        index += 2;
    }
    return decoded;
}

std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [name, value] : fields) {
        if (!body.empty()) {
            body += '&';
        }
        body += percent_encode(name);
        body += '=';
        body += percent_encode(value);
    }
    return body;
}

std::string form_value(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    for (auto space = text.find(' '); space != std::string_view::npos; space = text.find(' ', start)) {
        out += percent_encode(text.substr(start, space - start));
        out += '+';
        start = space + 1;
    }
    out += percent_encode(text.substr(start));
    return out;
}

}  // namespace mcp::detail
