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

/// Whether @p byte is written as it is and needs no UTF-8 check.
constexpr bool is_plain(unsigned char byte) {
    return byte >= 0x20U && byte < 0x80U && byte != '"' && byte != '\\';
}

/// Writes JSON text at the end of a string through an index into it. The string is grown ahead of
/// what is written, so a write is a store and a bounds check rather than a call into
/// std::string, and it is cut to what was written when the writer finishes.
class Writer {
   public:
    explicit Writer(std::string& out) : out_(out), length_(out.size()) {
        out_.resize(length_ + g_INITIAL_ROOM);
    }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;
    ~Writer() { out_.resize(length_); }

    void value(const nlohmann::json& value) {
        switch (value.type()) {
            case nlohmann::json::value_t::object: {
                const auto& object = value.get_ref<const nlohmann::json::object_t&>();
                if (object.empty()) {
                    write("{}", 2);
                    return;
                }
                char separator = '{';
                for (const auto& [key, member] : object) {
                    put(separator);
                    separator = ',';
                    string(key);
                    put(':');
                    this->value(member);
                }
                put('}');
                return;
            }
            case nlohmann::json::value_t::array: {
                const auto& array = value.get_ref<const nlohmann::json::array_t&>();
                if (array.empty()) {
                    write("[]", 2);
                    return;
                }
                char separator = '[';
                for (const auto& element : array) {
                    put(separator);
                    separator = ',';
                    this->value(element);
                }
                put(']');
                return;
            }
            case nlohmann::json::value_t::string:
                string(value.get_ref<const nlohmann::json::string_t&>());
                return;
            case nlohmann::json::value_t::boolean:
                if (value.get<bool>()) {
                    write("true", 4);
                } else {
                    write("false", 5);
                }
                return;
            case nlohmann::json::value_t::number_integer:
                integer(value.get<nlohmann::json::number_integer_t>());
                return;
            case nlohmann::json::value_t::number_unsigned:
                integer(value.get<nlohmann::json::number_unsigned_t>());
                return;
            case nlohmann::json::value_t::null:
                write("null", 4);
                return;
            case nlohmann::json::value_t::number_float:
            case nlohmann::json::value_t::binary:
            case nlohmann::json::value_t::discarded:
            default: {
                // Rare in messages; nlohmann::json's own text keeps the shortest round-trip form.
                const auto text = value.dump();
                write(text.data(), text.size());
                return;
            }
        }
    }

   private:
    static constexpr std::size_t g_INITIAL_ROOM = 256;
    /// An escape is six bytes, the most one byte of a string becomes.
    static constexpr std::size_t g_WIDEST_ESCAPE = 6;

    std::string& out_;
    std::size_t length_;

    void make_room(std::size_t needed) {
        if (out_.size() - length_ < needed) {
            out_.resize(std::max(out_.size() * 2, length_ + needed));
        }
    }

    void put(char symbol) {
        make_room(1);
        out_[length_++] = symbol;
    }

    void write(const void* bytes, std::size_t count) {
        make_room(count);
        std::memcpy(out_.data() + length_, bytes, count);
        length_ += count;
    }

    template <typename Integer>
    void integer(Integer value) {
        constexpr std::size_t widest = 20;  // The digits of the largest 64-bit value, or a sign and 19.
        make_room(widest);
        char* const at = out_.data() + length_;
        const auto result = std::to_chars(at, at + widest, value);
        length_ += static_cast<std::size_t>(result.ptr - at);
    }

    /// Writes @p text between quotes, escaped as nlohmann::json escapes it. A text that is not
    /// valid UTF-8 is handed to nlohmann::json, which throws the error dump() throws.
    void string(const std::string& text) {
        const std::size_t start = length_;
        if (!quoted(text)) {
            length_ = start;
            const auto fallback = nlohmann::json(text).dump();
            write(fallback.data(), fallback.size());
        }
    }

    bool quoted(std::string_view text) {
        const auto* at = reinterpret_cast<const unsigned char*>(text.data());
        const auto* const end = at + text.size();
        const auto* plain_end = json_text::skip_plain_blocks(at, end);
        while (plain_end < end && is_plain(*plain_end)) {
            ++plain_end;
        }
        const auto prefix = static_cast<std::size_t>(plain_end - at);
        make_room(prefix + 2);
        char* data = out_.data();
        data[length_++] = '"';
        std::memcpy(data + length_, at, prefix);
        length_ += prefix;
        at = plain_end;
        constexpr std::size_t block = json_text::g_BLOCK_BYTES;
        while (at < end) {
            if (static_cast<std::size_t>(end - at) >= block) {
                std::uint64_t word = 0;
                std::memcpy(&word, at, sizeof word);
                if (!json_text::needs_attention(word)) {
                    write(at, block);
                    at += block;
                    continue;
                }
            }
            // Byte by byte through this block.
            const auto* const block_end = at + std::min(block, static_cast<std::size_t>(end - at));
            make_room(g_WIDEST_ESCAPE * block);
            data = out_.data();
            while (at < block_end) {
                const unsigned char byte = *at;
                if (byte >= 0x80U) {
                    const std::size_t sequence = json_text::utf8_sequence_length(at, end);
                    if (sequence == 0) {
                        return false;
                    }
                    // A sequence that runs past the block is copied whole; it is at most four
                    // bytes, which the room made for the block covers.
                    std::memcpy(data + length_, at, sequence);
                    length_ += sequence;
                    at += sequence;
                    continue;
                }
                const char escape = g_ESCAPES[byte];
                if (escape == 0) {
                    data[length_++] = static_cast<char>(byte);
                } else if (escape != 'u') {
                    data[length_++] = '\\';
                    data[length_++] = escape;
                } else {
                    const auto hex = constants::g_hex_digits;
                    data[length_++] = '\\';
                    data[length_++] = 'u';
                    data[length_++] = '0';
                    data[length_++] = '0';
                    data[length_++] = hex[byte >> 4U];
                    data[length_++] = hex[byte & 0x0FU];
                }
                ++at;
            }
        }
        put('"');
        return true;
    }
};

}  // namespace

void append_json(std::string& out, const nlohmann::json& value) {
    Writer writer(out);
    writer.value(value);
}

std::string dump_json(const nlohmann::json& value) {
    std::string out;
    append_json(out, value);
    return out;
}

}  // namespace mcp::detail
