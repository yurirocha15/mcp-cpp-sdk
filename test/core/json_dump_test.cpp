#include <mcp/detail/json_dump.hpp>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using mcp::detail::append_json;
using mcp::detail::dump_json;

namespace {

/// What dump() makes of @p value: its text, or the message of the exception it throws.
struct DumpOutcome {
    bool threw = false;
    int error_id = 0;
    std::string text;
};

template <typename Dump>
DumpOutcome outcome_of(Dump dump) {
    try {
        return DumpOutcome{false, 0, dump()};
    } catch (const nlohmann::json::type_error& error) {
        return DumpOutcome{true, error.id, error.what()};
    }
}

void expect_same_as_nlohmann(const nlohmann::json& value) {
    const auto expected = outcome_of([&] { return value.dump(); });
    const auto actual = outcome_of([&] { return dump_json(value); });
    EXPECT_EQ(actual.threw, expected.threw) << expected.text;
    EXPECT_EQ(actual.error_id, expected.error_id);
    EXPECT_EQ(actual.text, expected.text);
}

std::string bytes(std::initializer_list<int> values) {
    std::string text;
    for (const int value : values) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

/// Strings built from the bytes and byte runs that need escaping or UTF-8 checks, so each mix of
/// them, around the eight-byte blocks the writer scans, is compared.
std::string random_string(std::mt19937& random) {
    static const std::vector<std::string> pieces = {
        "a",
        "plain text ",
        "0123456789abcdef",
        "\"",
        "\\",
        "/",
        bytes({0x00}),
        bytes({0x01}),
        "\b",
        "\t",
        "\n",
        "\f",
        "\r",
        bytes({0x1F}),
        bytes({0x7F}),
        bytes({0xC3, 0xA9}),
        bytes({0xE2, 0x82, 0xAC}),
        bytes({0xF0, 0x9F, 0x98, 0x80}),
        bytes({0xEF, 0xBF, 0xBF}),
        bytes({0xF4, 0x8F, 0xBF, 0xBF}),
        bytes({0xE0, 0xA0, 0x80}),
        bytes({0xED, 0x9F, 0xBF}),
        // Invalid: a lone continuation, overlong forms, a surrogate, past U+10FFFF, truncations.
        bytes({0x80}),
        bytes({0xC0, 0x80}),
        bytes({0xC1, 0xBF}),
        bytes({0xE0, 0x80, 0x80}),
        bytes({0xED, 0xA0, 0x80}),
        bytes({0xF0, 0x80, 0x80, 0x80}),
        bytes({0xF4, 0x90, 0x80, 0x80}),
        bytes({0xF5, 0x80, 0x80, 0x80}),
        bytes({0xFF}),
        bytes({0xE2, 0x82}),
        bytes({0xF0, 0x9F, 0x98}),
        bytes({0xC3}),
    };
    std::uniform_int_distribution<std::size_t> count(0, 12);
    std::uniform_int_distribution<std::size_t> pick(0, pieces.size() - 1);
    std::string text;
    for (std::size_t i = count(random); i > 0; --i) {
        text += pieces[pick(random)];
    }
    return text;
}

nlohmann::json random_value(std::mt19937& random, int depth) {
    std::uniform_int_distribution<int> kind(0, depth > 3 ? 7 : 9);
    switch (kind(random)) {
        case 0:
            return nullptr;
        case 1:
            return std::uniform_int_distribution<int>(0, 1)(random) == 1;
        case 2:
            return std::uniform_int_distribution<std::int64_t>(
                std::numeric_limits<std::int64_t>::min(),
                std::numeric_limits<std::int64_t>::max())(random);
        case 3:
            return std::uniform_int_distribution<std::uint64_t>()(random);
        case 4:
            return std::uniform_int_distribution<int>(-1000, 1000)(random);
        case 5:
            return std::uniform_real_distribution<double>(-1e6, 1e6)(random);
        case 6:
        case 7:
            return random_string(random);
        case 8: {
            nlohmann::json array = nlohmann::json::array();
            for (int i = std::uniform_int_distribution<int>(0, 4)(random); i > 0; --i) {
                array.push_back(random_value(random, depth + 1));
            }
            return array;
        }
        default: {
            nlohmann::json object = nlohmann::json::object();
            for (int i = std::uniform_int_distribution<int>(0, 4)(random); i > 0; --i) {
                object[random_string(random)] = random_value(random, depth + 1);
            }
            return object;
        }
    }
}

}  // namespace

TEST(JsonDumpTest, WritesEveryKindOfValueAsDumpDoes) {
    expect_same_as_nlohmann(nullptr);
    expect_same_as_nlohmann(true);
    expect_same_as_nlohmann(false);
    expect_same_as_nlohmann(0);
    expect_same_as_nlohmann(-0);
    expect_same_as_nlohmann(std::numeric_limits<std::int64_t>::min());
    expect_same_as_nlohmann(std::numeric_limits<std::int64_t>::max());
    expect_same_as_nlohmann(std::numeric_limits<std::uint64_t>::max());
    expect_same_as_nlohmann(0.1);
    expect_same_as_nlohmann(-0.0);
    expect_same_as_nlohmann(1e300);
    expect_same_as_nlohmann(5e-324);
    expect_same_as_nlohmann(std::numeric_limits<double>::quiet_NaN());
    expect_same_as_nlohmann(std::numeric_limits<double>::infinity());
    expect_same_as_nlohmann("");
    expect_same_as_nlohmann(nlohmann::json::object());
    expect_same_as_nlohmann(nlohmann::json::array());
    expect_same_as_nlohmann(nlohmann::json::binary({1, 2, 3}, 7));
    expect_same_as_nlohmann(nlohmann::json(nlohmann::json::value_t::discarded));
    expect_same_as_nlohmann(nlohmann::json::parse(
        R"({"jsonrpc":"2.0","id":7,"result":{"content":[{"type":"text","text":"{\"a\":[1,2.5,null]}"}],)"
        R"("isError":false,"z":{},"y":[]}})"));
}

TEST(JsonDumpTest, EscapesEveryControlCharacterAsDumpDoes) {
    for (int byte = 0; byte < 0x80; ++byte) {
        expect_same_as_nlohmann(std::string(1, static_cast<char>(byte)));
        expect_same_as_nlohmann("0123456789" + std::string(1, static_cast<char>(byte)) + "abcdefghij");
    }
}

TEST(JsonDumpTest, ThrowsWhatDumpThrowsForInvalidUtf8) {
    for (int byte = 0x80; byte < 0x100; ++byte) {
        expect_same_as_nlohmann(std::string(1, static_cast<char>(byte)));
        expect_same_as_nlohmann("valid text " + std::string(1, static_cast<char>(byte)) + " more");
    }
    const nlohmann::json in_key = {{bytes({'k', 0xC3}), 1}};
    expect_same_as_nlohmann(in_key);
    const auto thrown = outcome_of([&] { return dump_json(in_key); });
    EXPECT_TRUE(thrown.threw);
    EXPECT_EQ(thrown.error_id, 316);
}

TEST(JsonDumpTest, MatchesDumpOnRandomDocuments) {
    std::mt19937 random(20261104);
    for (int i = 0; i < 20000; ++i) {
        expect_same_as_nlohmann(random_value(random, 0));
        if (::testing::Test::HasFailure()) {
            break;
        }
    }
}

// Request ids and the numbers around them, as the response that echoes them writes them.
TEST(JsonDumpTest, WritesRequestIdsAsDumpDoes) {
    for (const nlohmann::json& id :
         {nlohmann::json(0), nlohmann::json(-1),
          nlohmann::json(std::numeric_limits<std::int64_t>::max()),
          nlohmann::json(std::numeric_limits<std::int64_t>::min()),
          nlohmann::json(std::uint64_t{9223372036854775808ULL}),
          nlohmann::json(std::numeric_limits<std::uint64_t>::max()), nlohmann::json(1.0),
          nlohmann::json(-0.0), nlohmann::json(1e21), nlohmann::json(1.5e-7), nlohmann::json(""),
          nlohmann::json(std::string(1, '\0')), nlohmann::json("\xC3\xA9"),
          nlohmann::json("\xF0\x9F\x98\x80"), nlohmann::json(true), nlohmann::json(nullptr),
          nlohmann::json::array(), nlohmann::json::object()}) {
        expect_same_as_nlohmann(nlohmann::json{{"id", id}, {"jsonrpc", "2.0"}, {"result", {}}});
    }
    for (const char* text : {R"({"id":-0})", R"({"id":1e0})", R"({"id":1E+2})", R"({"id":"é"})",
                             R"({"id":"😀"})", R"({"id":"\u0000"})", R"({"id":1,"id":2})"}) {
        expect_same_as_nlohmann(nlohmann::json::parse(text));
    }
}

TEST(JsonDumpTest, ThrowsForInvalidUtf8AnywhereInTheDocument) {
    const std::string bad = bytes({'o', 'k', 0xC3});
    const nlohmann::json nested_value = {
        {"a", {{"b", "fine"}, {"c", nlohmann::json::array({1, bad})}}}};
    const nlohmann::json nested_key = {{"a", {{"b", "fine"}, {bad, 1}}}};
    const nlohmann::json tool_text = {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", bad}}})}, {"isError", false}};
    for (const auto* value : {&nested_value, &nested_key, &tool_text}) {
        expect_same_as_nlohmann(*value);
        const auto thrown = outcome_of([&] { return dump_json(*value); });
        EXPECT_TRUE(thrown.threw);
        EXPECT_EQ(thrown.error_id, 316);
    }
    std::string out = "prefix";
    EXPECT_THROW(append_json(out, tool_text), nlohmann::json::type_error);
}

// The writer grows its string ahead of what it writes: strings that grow most when escaped, and
// UTF-8 sequences across its eight-byte blocks, written after every length of existing text so
// each lands on the edge of the room made for it.
TEST(JsonDumpTest, WritesAtEveryOffsetOfTheRoomItMakes) {
    std::vector<std::string> texts = {std::string(300, '\x01'), std::string(300, '"'),
                                      std::string(100000, '\n')};
    for (std::size_t shift = 0; shift < 8; ++shift) {
        std::string text(shift, 'a');
        for (int i = 0; i < 64; ++i) {
            text += bytes({0xF0, 0x9F, 0x98, 0x80});
            text += '\x1f';
        }
        texts.push_back(text);
    }
    std::string long_mixed;
    for (int i = 0; i < 20000; ++i) {
        long_mixed += "plain text \t";
        long_mixed += bytes({0xE2, 0x82, 0xAC});
    }
    texts.push_back(long_mixed);
    for (const auto& text : texts) {
        const nlohmann::json value = {{text, nlohmann::json::array({text, {{"k", text}}})}};
        const std::string expected = value.dump();
        std::vector<std::size_t> prefixes;
        for (std::size_t prefix = 0; prefix < 600; prefix += (text.size() > 1000 ? 31 : 1)) {
            prefixes.push_back(prefix);
        }
        for (const std::size_t prefix : prefixes) {
            std::string out(prefix, 'x');
            out.shrink_to_fit();
            append_json(out, value);
            ASSERT_EQ(out.size(), prefix + expected.size());
            ASSERT_EQ(out.compare(prefix, std::string::npos, expected), 0) << prefix;
        }
    }
}

TEST(JsonDumpTest, AppendsAfterExistingText) {
    std::string out = R"({"id":1,"result":)";
    append_json(out, nlohmann::json{{"text", "line\n\"quoted\""}});
    out += '}';
    EXPECT_EQ(out, R"({"id":1,"result":{"text":"line\n\"quoted\""}})");
}

TEST(JsonDumpTest, WritesAnObjectWithAddedMembersAsDumpWritesTheMergedObject) {
    using mcp::detail::append_json_object_with;
    using mcp::detail::JsonMemberText;
    std::mt19937 random(5112026);
    const std::vector<std::string> keys = {
        "", "A", "Z", "_meta", "a", "cacheScope", "id", "resultType", "ttlMs", "z", "\xC3\xA9", "\x7F"};
    for (int round = 0; round < 3000; ++round) {
        nlohmann::json object = nlohmann::json::object();
        std::vector<JsonMemberText> extra;
        std::vector<std::string> texts;
        texts.reserve(keys.size());
        for (const auto& key : keys) {
            const int pick = std::uniform_int_distribution<int>(0, 3)(random);
            if (pick == 1 || pick == 3) {
                object[key] = random_value(random, 2);
            }
            if (pick >= 2) {
                try {
                    texts.push_back(dump_json(random_value(random, 2)));
                } catch (const nlohmann::json::type_error&) {
                    // A value with invalid UTF-8 has no text to add.
                }
            }
        }
        // Keys in std::map order, then the added members that order picks.
        std::vector<std::string> sorted = keys;
        std::sort(sorted.begin(), sorted.end());
        nlohmann::json expected = object;
        std::size_t next_text = 0;
        for (const auto& key : sorted) {
            if (std::uniform_int_distribution<int>(0, 1)(random) == 0 || next_text == texts.size()) {
                continue;
            }
            const auto& text = texts[next_text++];
            extra.push_back({key, text});
            expected.emplace(key, nlohmann::json::parse(text));
        }
        std::string out = "[";
        const auto outcome = outcome_of([&] {
            append_json_object_with(out, object, extra);
            return out;
        });
        const auto wanted = outcome_of([&] { return "[" + expected.dump(); });
        EXPECT_EQ(outcome.threw, wanted.threw);
        EXPECT_EQ(outcome.text, wanted.text);
        if (::testing::Test::HasFailure()) {
            break;
        }
    }
    std::string empty;
    append_json_object_with(empty, nlohmann::json::object(), {});
    EXPECT_EQ(empty, "{}");
    const nlohmann::json bad_text = {{"text", bytes({'o', 'k', 0xC3})}};
    EXPECT_THROW(append_json_object_with(empty, bad_text, {{{"resultType", R"("complete")"}}}),
                 nlohmann::json::type_error);
}
