#include "../detail/encoding.hpp"

#include <openssl/evp.h>

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
    std::size_t index = 0;
    for (; index + 3 <= bytes.size(); index += 3) {
        const auto word =
            byte_at(bytes, index) << 16U | byte_at(bytes, index + 1) << 8U | byte_at(bytes, index + 2);
        out += alphabet[(word >> 18U) & 0x3FU];
        out += alphabet[(word >> 12U) & 0x3FU];
        out += alphabet[(word >> 6U) & 0x3FU];
        out += alphabet[word & 0x3FU];
    }
    const std::size_t rest = bytes.size() - index;
    if (rest == 0) {
        return out;
    }
    auto word = byte_at(bytes, index) << 16U;
    if (rest == 2) {
        word |= byte_at(bytes, index + 1) << 8U;
    }
    out += alphabet[(word >> 18U) & 0x3FU];
    out += alphabet[(word >> 12U) & 0x3FU];
    if (rest == 2) {
        out += alphabet[(word >> 6U) & 0x3FU];
    }
    if (pad) {
        out.append(3 - rest, '=');
    }
    return out;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
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
        out += encoding::g_HEX_DIGITS[byte >> 4U];
        out += encoding::g_HEX_DIGITS[byte & 0x0FU];
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
            const auto position = encoding::g_BASE64_ALPHABET.find(character);
            if (padding != 0 || position == std::string_view::npos) {
                return std::nullopt;
            }
            quad[offset] = static_cast<std::uint32_t>(position);
        }
        const auto word = quad[0] << 18U | quad[1] << 12U | quad[2] << 6U | quad[3];
        bytes += static_cast<char>((word >> 16U) & 0xFFU);
        if (padding < 2) {
            bytes += static_cast<char>((word >> 8U) & 0xFFU);
        }
        if (padding < 1) {
            bytes += static_cast<char>(word & 0xFFU);
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
        const std::size_t position = encoding::g_BASE64URL_ALPHABET.find(character);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((accumulator >> bits) & 0xFFU);
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
        out += encoding::g_HEX_DIGITS_UPPER[byte >> 4U];
        out += encoding::g_HEX_DIGITS_UPPER[byte & 0x0FU];
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
        const int high = hex_value(text[index + 1]);
        const int low = hex_value(text[index + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        decoded += static_cast<char>(high * 16 + low);
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
