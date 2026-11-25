#include "mcp/detail/json/json_parse.hpp"

#include "mcp/detail/constants.hpp"

#include "mcp/detail/json/json_text_scan.hpp"
#include "mcp/detail/text/unicode.hpp"

#include <array>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace mcp::detail {

namespace {

/// A recursive descent over the JSON grammar of RFC 8259 that builds the nlohmann::json value as
/// it reads, where nlohmann's own parser records every character of a token for its error
/// messages and goes through a SAX interface. It accepts exactly what nlohmann::json accepts
/// without comments, except for the inputs parse_json_fast() lists, which it leaves to nlohmann.
class FastParser {
   public:
    explicit FastParser(std::string_view text)
        : at_(reinterpret_cast<const unsigned char*>(text.data())), end_(at_ + text.size()) {}

    bool document(nlohmann::json& out) {
        skip_whitespace();
        if (!value(out)) {
            return false;
        }
        skip_whitespace();
        return at_ == end_;
    }

   private:
    const unsigned char* at_;
    const unsigned char* end_;
    /// The arrays and objects open around the value being read.
    std::size_t depth_ = 0;

    /// Counts one more open array or object for as long as it lives, and refuses one past
    /// mcp::detail::constants::json_parse::g_MAX_FAST_DEPTH, so the recursion stays within a small
    /// thread stack.
    class Nesting {
       public:
        explicit Nesting(std::size_t& depth) : depth_(depth) { ++depth_; }
        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;
        Nesting(Nesting&&) = delete;
        Nesting& operator=(Nesting&&) = delete;
        ~Nesting() { --depth_; }
        [[nodiscard]] bool too_deep() const {
            return depth_ > mcp::detail::constants::json_parse::g_MAX_FAST_DEPTH;
        }

       private:
        std::size_t& depth_;
    };

    void skip_whitespace() {
        while (at_ < end_ && (*at_ == ' ' || *at_ == '\n' || *at_ == '\r' || *at_ == '\t')) {
            ++at_;
        }
    }

    bool literal(std::string_view word) {
        if (static_cast<std::size_t>(end_ - at_) < word.size() ||
            std::memcmp(at_, word.data(), word.size()) != 0) {
            return false;
        }
        at_ += word.size();
        return true;
    }

    bool value(nlohmann::json& out) {
        if (at_ == end_) {
            return false;
        }
        switch (*at_) {
            case '{':
                return object(out);
            case '[':
                return array(out);
            case '"': {
                std::string text;
                if (!string(text)) {
                    return false;
                }
                out = std::move(text);
                return true;
            }
            case 't':
                if (!literal("true")) {
                    return false;
                }
                out = true;
                return true;
            case 'f':
                if (!literal("false")) {
                    return false;
                }
                out = false;
                return true;
            case 'n':
                if (!literal("null")) {
                    return false;
                }
                out = nullptr;
                return true;
            default:
                return number(out);
        }
    }

    bool object(nlohmann::json& out) {
        const Nesting nesting(depth_);
        if (nesting.too_deep()) {
            return false;
        }
        ++at_;
        out = nlohmann::json::object();
        skip_whitespace();
        if (at_ < end_ && *at_ == '}') {
            ++at_;
            return true;
        }
        for (;;) {
            if (at_ == end_ || *at_ != '"') {
                return false;
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skip_whitespace();
            if (at_ == end_ || *at_ != ':') {
                return false;
            }
            ++at_;
            skip_whitespace();
            // A repeated key keeps its last value, as nlohmann::json's parser does.
            if (!value(out[std::move(key)])) {
                return false;
            }
            skip_whitespace();
            if (at_ == end_) {
                return false;
            }
            if (*at_ == '}') {
                ++at_;
                return true;
            }
            if (*at_ != ',') {
                return false;
            }
            ++at_;
            skip_whitespace();
        }
    }

    bool array(nlohmann::json& out) {
        const Nesting nesting(depth_);
        if (nesting.too_deep()) {
            return false;
        }
        ++at_;
        out = nlohmann::json::array();
        skip_whitespace();
        if (at_ < end_ && *at_ == ']') {
            ++at_;
            return true;
        }
        for (;;) {
            out.push_back(nlohmann::json());
            if (!value(out.back())) {
                return false;
            }
            skip_whitespace();
            if (at_ == end_) {
                return false;
            }
            if (*at_ == ']') {
                ++at_;
                return true;
            }
            if (*at_ != ',') {
                return false;
            }
            ++at_;
            skip_whitespace();
        }
    }

    /// Reads the string that starts at the opening quote under at_ into @p out.
    bool string(std::string& out) {
        ++at_;
        const unsigned char* run = at_;
        for (;;) {
            at_ = json_text::skip_plain_blocks(at_, end_);
            if (at_ == end_) {
                return false;
            }
            const unsigned char byte = *at_;
            if (byte == '"') {
                out.append(reinterpret_cast<const char*>(run), static_cast<std::size_t>(at_ - run));
                ++at_;
                return true;
            }
            if (byte == '\\') {
                out.append(reinterpret_cast<const char*>(run), static_cast<std::size_t>(at_ - run));
                if (!escape(out)) {
                    return false;
                }
                run = at_;
                continue;
            }
            if (byte < constants::unicode::g_C0_CONTROLS_END) {
                return false;
            }
            if (byte < constants::unicode::g_ASCII_END) {
                ++at_;
                continue;
            }
            const std::size_t length = unicode::utf8_sequence_length(at_, end_);
            if (length == 0) {
                return false;
            }
            at_ += length;
        }
    }

    bool hex4(std::uint32_t& out) {
        if (end_ - at_ < 4) {
            return false;
        }
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = constants::encoding::g_HEX_VALUES[*at_++];
            if (digit < 0) {
                return false;
            }
            code = (code << constants::encoding::g_HEX_DIGIT_BITS) | static_cast<std::uint32_t>(digit);
        }
        out = code;
        return true;
    }

    /// Reads the escape sequence that starts at the backslash under at_ and appends what it means.
    bool escape(std::string& out) {
        ++at_;
        if (at_ == end_) {
            return false;
        }
        switch (*at_++) {
            case '"':
                out.push_back('"');
                return true;
            case '\\':
                out.push_back('\\');
                return true;
            case '/':
                out.push_back('/');
                return true;
            case 'b':
                out.push_back('\b');
                return true;
            case 'f':
                out.push_back('\f');
                return true;
            case 'n':
                out.push_back('\n');
                return true;
            case 'r':
                out.push_back('\r');
                return true;
            case 't':
                out.push_back('\t');
                return true;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (unicode::is_high_surrogate(code)) {
                    // A high surrogate must be followed by the escape of a low one.
                    if (end_ - at_ < 2 || at_[0] != '\\' || at_[1] != 'u') {
                        return false;
                    }
                    at_ += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low) || !unicode::is_low_surrogate(low)) {
                        return false;
                    }
                    code = unicode::combine_surrogates(code, low);
                } else if (unicode::is_low_surrogate(code)) {
                    return false;
                }
                unicode::append_utf8(out, code);
                return true;
            }
            default:
                return false;
        }
    }

    static bool is_digit(unsigned char byte) { return byte >= '0' && byte <= '9'; }

    bool digits() {
        if (at_ == end_ || !is_digit(*at_)) {
            return false;
        }
        while (at_ < end_ && is_digit(*at_)) {
            ++at_;
        }
        return true;
    }

    bool number(nlohmann::json& out) {
        const unsigned char* const start = at_;
        const bool negative = *at_ == '-';
        if (negative) {
            ++at_;
        }
        if (at_ == end_) {
            return false;
        }
        const unsigned char* const integer_start = at_;
        if (*at_ == '0') {
            ++at_;
        } else if (!digits()) {
            return false;
        }
        const unsigned char* const integer_end = at_;
        bool is_float = false;
        if (at_ < end_ && *at_ == '.') {
            ++at_;
            if (!digits()) {
                return false;
            }
            is_float = true;
        }
        if (at_ < end_ && (*at_ == 'e' || *at_ == 'E')) {
            ++at_;
            if (at_ < end_ && (*at_ == '+' || *at_ == '-')) {
                ++at_;
            }
            if (!digits()) {
                return false;
            }
            is_float = true;
        }
        if (!is_float) {
            return integer(out, negative, integer_start, integer_end);
        }
        return floating(out, start);
    }

    /// An integer is unsigned without a sign and signed with one, as nlohmann::json reads it. One
    /// out of range for that type is left to nlohmann, which reads it as a double.
    static bool integer(nlohmann::json& out, bool negative, const unsigned char* begin,
                        const unsigned char* end) {
        std::uint64_t magnitude = 0;
        const auto* const first = reinterpret_cast<const char*>(begin);
        const auto* const last = reinterpret_cast<const char*>(end);
        if (std::from_chars(first, last, magnitude).ec != std::errc{}) {
            return false;
        }
        if (!negative) {
            out = static_cast<nlohmann::json::number_unsigned_t>(magnitude);
            return true;
        }
        constexpr auto limit =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
        if (magnitude > limit) {
            return false;
        }
        out = magnitude == limit ? std::numeric_limits<std::int64_t>::min()
                                 : -static_cast<std::int64_t>(magnitude);
        return true;
    }

    /// Reads the number from @p start to at_ with strtod, as nlohmann::json does. A locale whose
    /// decimal point is not `.` and a value too large for a double are left to nlohmann.
    bool floating(nlohmann::json& out, const unsigned char* start) const {
        const char* point = std::localeconv()->decimal_point;
        if (point == nullptr || point[0] != '.' || point[1] != '\0') {
            return false;
        }
        const auto length = static_cast<std::size_t>(at_ - start);
        std::array<char, constants::json_parse::g_STACK_NUMBER_BYTES> small{};
        std::string large;
        char* text = small.data();
        if (length >= small.size()) {
            large.assign(reinterpret_cast<const char*>(start), length);
            text = large.data();
        } else {
            std::memcpy(text, start, length);
            text[length] = '\0';
        }
        char* parsed_end = nullptr;
        const double value = std::strtod(text, &parsed_end);
        if (parsed_end != text + length || !std::isfinite(value)) {
            return false;
        }
        out = value;
        return true;
    }
};

}  // namespace

bool parse_json_fast(std::string_view text, nlohmann::json& out) {
    return FastParser(text).document(out);
}

}  // namespace mcp::detail
