#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>

#include "../detail/json_text_scan.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mcp::detail {

namespace {

/// For each byte below 0x80, the letter of its two-character escape, `u` for one written as
/// `\u00XX`, or 0 for a byte written as it is.
constexpr std::array<char, 0x80> g_ESCAPES = [] {
    std::array<char, 0x80> escapes{};
    for (std::size_t byte = 0; byte < 0x20; ++byte) {
        escapes[byte] = 'u';
    }
    escapes['\b'] = 'b';
    escapes['\t'] = 't';
    escapes['\n'] = 'n';
    escapes['\f'] = 'f';
    escapes['\r'] = 'r';
    escapes['"'] = '"';
    escapes['\\'] = '\\';
    return escapes;
}();

/// Appends @p text between quotes, escaped as nlohmann::json escapes it. Returns false, with
/// @p out holding part of it, when @p text is not valid UTF-8. The text is written through a
/// pointer into @p out, grown ahead of what is written.
bool append_quoted(std::string& out, std::string_view text) {
    const auto* at = reinterpret_cast<const unsigned char*>(text.data());
    const auto* const end = at + text.size();
    std::size_t length = out.size();
    out.resize(length + text.size() + text.size() / 8 + 16);
    char* data = out.data();
    const auto make_room = [&](std::size_t needed) {
        if (out.size() - length < needed) {
            out.resize(std::max(out.size() * 2, length + needed));
            data = out.data();
        }
    };
    data[length++] = '"';
    while (at < end) {
        constexpr std::size_t block = json_text::g_BLOCK_BYTES;
        if (static_cast<std::size_t>(end - at) >= block) {
            std::uint64_t word = 0;
            std::memcpy(&word, at, sizeof word);
            if (!json_text::needs_attention(word)) {
                make_room(block);
                std::memcpy(data + length, at, block);
                length += block;
                at += block;
                continue;
            }
        }
        // Byte by byte through this block: an escape is six bytes, the most one byte becomes.
        constexpr std::size_t widest_escape = 6;
        const auto* const block_end = at + std::min(block, static_cast<std::size_t>(end - at));
        make_room(widest_escape * block);
        while (at < block_end) {
            const unsigned char byte = *at;
            if (byte >= 0x80U) {
                const std::size_t sequence = json_text::utf8_sequence_length(at, end);
                if (sequence == 0) {
                    out.resize(length);
                    return false;
                }
                // A sequence that runs past the block is copied whole; it is at most four bytes.
                std::memcpy(data + length, at, sequence);
                length += sequence;
                at += sequence;
                continue;
            }
            const char escape = g_ESCAPES[byte];
            if (escape == 0) {
                data[length++] = static_cast<char>(byte);
            } else if (escape != 'u') {
                data[length++] = '\\';
                data[length++] = escape;
            } else {
                const auto hex = constants::g_hex_digits;
                data[length++] = '\\';
                data[length++] = 'u';
                data[length++] = '0';
                data[length++] = '0';
                data[length++] = hex[byte >> 4U];
                data[length++] = hex[byte & 0x0FU];
            }
            ++at;
        }
    }
    make_room(1);
    data[length++] = '"';
    out.resize(length);
    return true;
}

void append_string(std::string& out, const std::string& text) {
    const std::size_t start = out.size();
    if (!append_quoted(out, text)) {
        // nlohmann::json reports the invalid byte; its exception is the one dump() throws.
        out.resize(start);
        out += nlohmann::json(text).dump();
    }
}

template <typename Integer>
void append_integer(std::string& out, Integer value) {
    std::array<char, 24> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out.append(digits.data(), static_cast<std::size_t>(result.ptr - digits.data()));
}

void append_value(std::string& out, const nlohmann::json& value) {
    switch (value.type()) {
        case nlohmann::json::value_t::object: {
            const auto& object = value.get_ref<const nlohmann::json::object_t&>();
            if (object.empty()) {
                out.append("{}", 2);
                return;
            }
            char separator = '{';
            for (const auto& [key, member] : object) {
                out.push_back(separator);
                separator = ',';
                append_string(out, key);
                out.push_back(':');
                append_value(out, member);
            }
            out.push_back('}');
            return;
        }
        case nlohmann::json::value_t::array: {
            const auto& array = value.get_ref<const nlohmann::json::array_t&>();
            if (array.empty()) {
                out.append("[]", 2);
                return;
            }
            char separator = '[';
            for (const auto& element : array) {
                out.push_back(separator);
                separator = ',';
                append_value(out, element);
            }
            out.push_back(']');
            return;
        }
        case nlohmann::json::value_t::string:
            append_string(out, value.get_ref<const nlohmann::json::string_t&>());
            return;
        case nlohmann::json::value_t::boolean:
            if (value.get<bool>()) {
                out.append("true", 4);
            } else {
                out.append("false", 5);
            }
            return;
        case nlohmann::json::value_t::number_integer:
            append_integer(out, value.get<nlohmann::json::number_integer_t>());
            return;
        case nlohmann::json::value_t::number_unsigned:
            append_integer(out, value.get<nlohmann::json::number_unsigned_t>());
            return;
        case nlohmann::json::value_t::null:
            out.append("null", 4);
            return;
        case nlohmann::json::value_t::number_float:
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
        default:
            // Rare in messages; nlohmann::json's own text keeps the shortest round-trip form.
            out += value.dump();
            return;
    }
}

}  // namespace

void append_json(std::string& out, const nlohmann::json& value) { append_value(out, value); }

std::string dump_json(const nlohmann::json& value) {
    std::string out;
    append_value(out, value);
    return out;
}

}  // namespace mcp::detail
