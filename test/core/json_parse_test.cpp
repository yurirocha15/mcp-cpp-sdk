#include <mcp/detail/json_dump.hpp>
#include <mcp/detail/json_parse.hpp>

#include "../../src/detail/peer_json.hpp"

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using mcp::detail::parse_json_fast;

namespace {

/// Equal including the type of every number and the sign of zero, which operator== ignores.
bool identical(const nlohmann::json& left, const nlohmann::json& right) {
    if (left.type() != right.type()) {
        return false;
    }
    switch (left.type()) {
        case nlohmann::json::value_t::object: {
            if (left.size() != right.size()) {
                return false;
            }
            auto other = right.begin();
            for (auto member = left.begin(); member != left.end(); ++member, ++other) {
                if (member.key() != other.key() || !identical(*member, *other)) {
                    return false;
                }
            }
            return true;
        }
        case nlohmann::json::value_t::array: {
            if (left.size() != right.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i) {
                if (!identical(left[i], right[i])) {
                    return false;
                }
            }
            return true;
        }
        case nlohmann::json::value_t::number_float: {
            const double a = left.get<double>();
            const double b = right.get<double>();
            return std::memcmp(&a, &b, sizeof a) == 0;
        }
        default:
            return left == right;
    }
}

/// The fast parser either declines @p text or returns exactly what nlohmann::json returns.
void expect_agrees_with_nlohmann(const std::string& text) {
    const auto expected = nlohmann::json::parse(text, nullptr, false);
    nlohmann::json actual;
    if (!parse_json_fast(text, actual)) {
        return;
    }
    ASSERT_FALSE(expected.is_discarded()) << "accepted what nlohmann rejects: " << text;
    EXPECT_TRUE(identical(actual, expected)) << text;
}

/// @p text is valid JSON the fast parser must read itself, to the same value as nlohmann::json.
void expect_parsed_alike(const std::string& text) {
    const auto expected = nlohmann::json::parse(text, nullptr, false);
    ASSERT_FALSE(expected.is_discarded()) << text;
    nlohmann::json actual;
    ASSERT_TRUE(parse_json_fast(text, actual)) << text;
    EXPECT_TRUE(identical(actual, expected)) << text;
}

void expect_rejected(const std::string& text) {
    ASSERT_TRUE(nlohmann::json::parse(text, nullptr, false).is_discarded()) << text;
    nlohmann::json actual;
    EXPECT_FALSE(parse_json_fast(text, actual)) << text;
}

std::string bytes(std::initializer_list<int> values) {
    std::string text;
    for (const int value : values) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

/// Runs @p work on a thread whose stack is @p stack_bytes, as small as on a pool thread. Windows
/// threads here get the default stack, which is larger.
void run_on_small_stack(std::size_t stack_bytes, const std::function<void()>& work) {
#if defined(_WIN32)
    (void)stack_bytes;
    std::thread thread(work);
    thread.join();
#else
    pthread_attr_t attributes;
    ASSERT_EQ(pthread_attr_init(&attributes), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attributes, stack_bytes), 0);
    pthread_t thread{};
    auto* task = new std::function<void()>(work);
    const auto start = [](void* argument) -> void* {
        auto* job = static_cast<std::function<void()>*>(argument);
        (*job)();
        delete job;
        return nullptr;
    };
    ASSERT_EQ(pthread_create(&thread, &attributes, start, task), 0);
    pthread_attr_destroy(&attributes);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
#endif
}

std::string nested_arrays(std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

std::string nested_objects(std::size_t depth) {
    std::string text;
    for (std::size_t i = 0; i < depth; ++i) {
        text += R"({"a":)";
    }
    text += "null";
    text += std::string(depth, '}');
    return text;
}

}  // namespace

TEST(JsonParseTest, ReadsMessagesAsNlohmannDoes) {
    expect_parsed_alike(
        R"({"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"checkout","arguments":)"
        R"({"user_id":"user_1","items":[{"product_id":42,"quantity":2}]},"_meta":{"progressToken":"p"}}})");
    expect_parsed_alike(" \t\r\n[ 1 , -1 , 0 , -0 , 1.5 , -0.0 , 1e2 , 1E-2 , 2.5e+3 ] \n");
    expect_parsed_alike(R"({"a":1,"a":2,"b":{"c":[],"d":{}}})");
    expect_parsed_alike(R"(["", "plain", "\"\\\/\b\f\n\r\t", "\u0000\u001f\u00e9\u20AC\ud83d\ude00"])");
    expect_parsed_alike("\"" + bytes({0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0x7F}) +
                        "\"");
    expect_parsed_alike("true");
    expect_parsed_alike("false");
    expect_parsed_alike("null");
    expect_parsed_alike("18446744073709551615");
    expect_parsed_alike("-9223372036854775808");
    expect_parsed_alike("9223372036854775808");
    expect_parsed_alike("1e-400");
    expect_parsed_alike("0.1000000000000000055511151231257827021181583404541015625");
}

TEST(JsonParseTest, RejectsWhatNlohmannRejects) {
    for (const char* text : {"",
                             " ",
                             "{",
                             "}",
                             "[",
                             "]",
                             "[1,]",
                             "[,1]",
                             "{\"a\":1,}",
                             "{\"a\"}",
                             "{\"a\" 1}",
                             "{1:2}",
                             "01",
                             "-",
                             "1.",
                             ".5",
                             "1e",
                             "1e+",
                             "+1",
                             "0x10",
                             "tru",
                             "nul",
                             "falsey",
                             "1 2",
                             "[1] x",
                             "\"unterminated",
                             "\"\\x\"",
                             "\"\\u12\"",
                             "\"\\u12g4\"",
                             "\"\\ud800\"",
                             "\"\\ud800x\"",
                             "\"\\ud800\\u0041\"",
                             "\"\\udc00\"",
                             "1e400",
                             "-1e400",
                             "NaN",
                             "Infinity",
                             "/* c */ 1",
                             "[1]\x01"}) {
        expect_rejected(text);
    }
    expect_rejected(std::string("\"a\0b\"", 5));
    expect_rejected("\"\x01\"");
    expect_rejected("\"\x1f\"");
    for (const auto& invalid :
         {bytes({0x80}), bytes({0xC0, 0x80}), bytes({0xC1, 0xBF}), bytes({0xE0, 0x80, 0x80}),
          bytes({0xED, 0xA0, 0x80}), bytes({0xF0, 0x80, 0x80, 0x80}), bytes({0xF4, 0x90, 0x80, 0x80}),
          bytes({0xF5, 0x80, 0x80, 0x80}), bytes({0xFF}), bytes({0xE2, 0x82}), bytes({0xC3})}) {
        expect_rejected("\"" + invalid + "\"");
        expect_rejected("\"0123456789" + invalid + "abcdefghij\"");
    }
}

TEST(JsonParseTest, LeavesNumbersOutOfRangeToNlohmann) {
    nlohmann::json value;
    EXPECT_FALSE(parse_json_fast("18446744073709551616", value));
    EXPECT_FALSE(parse_json_fast("-9223372036854775809", value));
    const auto peer = mcp::detail::parse_peer_json("[18446744073709551616,-9223372036854775809]");
    EXPECT_TRUE(identical(peer, nlohmann::json::parse("[18446744073709551616,-9223372036854775809]")));
    EXPECT_TRUE(peer[0].is_number_float());
}

TEST(JsonParseTest, PeerJsonFallsBackToNlohmannForTextAfterANulByte) {
    // nlohmann::json reads a NUL byte outside a string as the end of the input.
    const std::string text("[1] \0 trailing", 14);
    nlohmann::json value;
    EXPECT_FALSE(parse_json_fast(text, value));
    const auto expected = nlohmann::json::parse(text, nullptr, false);
    ASSERT_FALSE(expected.is_discarded());
    EXPECT_TRUE(identical(mcp::detail::parse_peer_json(text), expected));
}

TEST(JsonParseTest, PeerJsonFallsBackToNlohmannForAByteOrderMark) {
    const std::string text = bytes({0xEF, 0xBB, 0xBF}) + R"({"a":1})";
    nlohmann::json value;
    EXPECT_FALSE(parse_json_fast(text, value));
    EXPECT_TRUE(identical(mcp::detail::parse_peer_json(text), nlohmann::json::parse(text)));
}

TEST(JsonParseTest, AgreesWithNlohmannOnMutatedDocuments) {
    std::mt19937 random(4112026);
    const std::vector<std::string> seeds = {
        R"({"jsonrpc":"2.0","id":"a-1","method":"initialize","params":{"protocolVersion":"2025-06-18",)"
        R"("capabilities":{"roots":{"listChanged":true}},"clientInfo":{"name":"c","version":"1.0"}}})",
        R"([0,-0,1.25e-3,-1234567890123456789,18446744073709551615,true,false,null,"\u00e9\ud83d\ude00"])",
        R"({"text":"line\nnext \"quoted\" back\\slash \/ tab\t","nested":[[[{"k":[]}]]]})",
    };
    const std::string alphabet =
        std::string("{}[],:\"\\/-+.0123456789eEtrufalsn \t\r\nabcdx") +
        bytes({0x00, 0x01, 0x1F, 0x7F, 0x80, 0xBF, 0xC3, 0xE2, 0xED, 0xF0, 0xF4, 0xFF});
    std::uniform_int_distribution<std::size_t> pick_char(0, alphabet.size() - 1);
    for (const auto& seed : seeds) {
        expect_parsed_alike(seed);
        for (int round = 0; round < 20000; ++round) {
            std::string text = seed;
            const int edits = std::uniform_int_distribution<int>(1, 3)(random);
            for (int edit = 0; edit < edits && !text.empty(); ++edit) {
                const std::size_t at =
                    std::uniform_int_distribution<std::size_t>(0, text.size() - 1)(random);
                switch (std::uniform_int_distribution<int>(0, 3)(random)) {
                    case 0:
                        text[at] = alphabet[pick_char(random)];
                        break;
                    case 1:
                        text.insert(at, 1, alphabet[pick_char(random)]);
                        break;
                    case 2:
                        text.erase(at, 1);
                        break;
                    default:
                        text.resize(at);
                        break;
                }
            }
            expect_agrees_with_nlohmann(text);
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

TEST(JsonParseTest, ReadsBackWhatDumpWrites) {
    std::mt19937 random(11042026);
    for (int round = 0; round < 5000; ++round) {
        nlohmann::json value = nlohmann::json::object();
        for (int i = std::uniform_int_distribution<int>(0, 6)(random); i > 0; --i) {
            std::string key;
            for (int j = std::uniform_int_distribution<int>(0, 20)(random); j > 0; --j) {
                key.push_back(static_cast<char>(std::uniform_int_distribution<int>(0, 0x7F)(random)));
            }
            switch (std::uniform_int_distribution<int>(0, 4)(random)) {
                case 0:
                    value[key] = std::uniform_int_distribution<std::int64_t>(
                        std::numeric_limits<std::int64_t>::min(), -1)(random);
                    break;
                case 1:
                    value[key] = std::uniform_int_distribution<std::uint64_t>()(random);
                    break;
                case 2:
                    value[key] = std::uniform_real_distribution<double>(-1e300, 1e300)(random);
                    break;
                case 3:
                    value[key] = std::uniform_real_distribution<double>(-1e-300, 1e-300)(random);
                    break;
                default:
                    value[key] = nlohmann::json::array({key, nullptr, true});
                    break;
            }
        }
        expect_parsed_alike(mcp::detail::dump_json(value));
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(JsonParseTest, NestingUpToItsLimitParsesOnASmallStack) {
    constexpr std::size_t small_stack = 256 * 1024;
    constexpr std::size_t depth = mcp::detail::g_MAX_FAST_PARSE_DEPTH;
    for (const auto& text : {nested_arrays(depth), nested_objects(depth)}) {
        run_on_small_stack(small_stack, [&text] { expect_parsed_alike(text); });
    }
    nlohmann::json value;
    EXPECT_FALSE(parse_json_fast(nested_arrays(mcp::detail::g_MAX_FAST_PARSE_DEPTH + 1), value));
    EXPECT_FALSE(parse_json_fast(nested_objects(mcp::detail::g_MAX_FAST_PARSE_DEPTH + 1), value));
}

// A hostile peer's deep nesting is declined at the parser's limit, without recursing further,
// and parse_peer_json() refuses it before parsing, as it always has.
TEST(JsonParseTest, DeepNestingIsDeclinedWithoutOverflowingTheStack) {
    constexpr std::size_t small_stack = 256 * 1024;
    for (const std::size_t depth : {std::size_t{100000}, std::size_t{1000000}}) {
        for (const auto& text : {nested_arrays(depth), nested_objects(depth)}) {
            const auto check = [&text] {
                nlohmann::json value;
                EXPECT_FALSE(parse_json_fast(text, value));
                EXPECT_TRUE(mcp::detail::parse_peer_json(text).is_discarded());
            };
            check();
            run_on_small_stack(small_stack, check);
        }
    }
}

TEST(JsonParseTest, PeerJsonAgreesWithNlohmannAroundNulAndByteOrderMarks) {
    const auto same_as_nlohmann = [](std::string_view text) {
        const auto expected = nlohmann::json::parse(text, nullptr, false);
        const auto actual = mcp::detail::parse_peer_json(text);
        EXPECT_EQ(actual.is_discarded(), expected.is_discarded()) << text;
        EXPECT_TRUE(expected.is_discarded() || identical(actual, expected)) << text;
    };
    using namespace std::string_view_literals;
    same_as_nlohmann(R"({"jsonrpc":"2.0","id":1,"method":"ping"})"
                     "\0garbage"sv);
    same_as_nlohmann(R"({"jsonrpc":"2.0","id":1,"method":"ping"} )"
                     "\0"sv);
    same_as_nlohmann("{\"method\":\"pi\0ng\"}"sv);
    same_as_nlohmann("[1,\0]"sv);
    same_as_nlohmann("\0"sv);
    same_as_nlohmann("\xEF\xBB\xBF{}"sv);
    same_as_nlohmann("\xEF\xBB\xBF\xEF\xBB\xBF{}"sv);
    same_as_nlohmann("\xEF\xBB{}"sv);
    EXPECT_FALSE(mcp::detail::parse_peer_json(R"({"id":1} )"
                                              "\0garbage"sv)
                     .is_discarded());
    EXPECT_TRUE(mcp::detail::parse_peer_json("\xEF\xBB\xBF\xEF\xBB\xBF{}"sv).is_discarded());
}

// The request ids a peer can send, as nlohmann::json reads them: numbers keep their type, the
// last of repeated members wins, and escapes become the UTF-8 they stand for.
TEST(JsonParseTest, ReadsRequestIdsAsNlohmannDoes) {
    for (const char* text : {R"({"id":0})",
                             R"({"id":-1})",
                             R"({"id":-0})",
                             R"({"id":9223372036854775807})",
                             R"({"id":-9223372036854775808})",
                             R"({"id":9223372036854775808})",
                             R"({"id":18446744073709551615})",
                             R"({"id":1.0})",
                             R"({"id":1e0})",
                             R"({"id":-0.0})",
                             R"({"id":""})",
                             R"({"id":"\u0000"})",
                             R"({"id":"\u00e9"})",
                             "{\"id\":\"\xC3\xA9\"}",
                             R"({"id":"\ud83d\ude00"})",
                             R"({"id":1,"id":"last"})",
                             R"({"id":true})",
                             R"({"id":null})",
                             R"({"id":[]})",
                             R"({"id":{}})"}) {
        expect_parsed_alike(text);
    }
    nlohmann::json value;
    ASSERT_TRUE(parse_json_fast(R"({"id":-0})", value));
    EXPECT_TRUE(value["id"].is_number_integer());
    EXPECT_EQ(value["id"], 0);
    ASSERT_TRUE(parse_json_fast(R"({"id":9223372036854775807})", value));
    EXPECT_TRUE(value["id"].is_number_unsigned());
    ASSERT_TRUE(parse_json_fast(R"({"id":-9223372036854775808})", value));
    EXPECT_EQ(value["id"].get<std::int64_t>(), std::numeric_limits<std::int64_t>::min());
    ASSERT_TRUE(parse_json_fast(R"({"id":1,"id":"last"})", value));
    EXPECT_EQ(value["id"], "last");
    ASSERT_TRUE(parse_json_fast(R"({"id":"\u0000"})", value));
    EXPECT_EQ(value["id"].get<std::string>(), std::string(1, '\0'));
    ASSERT_TRUE(parse_json_fast(R"({"id":"\ud83d\ude00"})", value));
    EXPECT_EQ(value["id"].get<std::string>(), "\xF0\x9F\x98\x80");
    EXPECT_FALSE(parse_json_fast(R"({"id":1e400})", value));
    EXPECT_TRUE(mcp::detail::parse_peer_json(R"({"id":1e400})").is_discarded());
    EXPECT_TRUE(mcp::detail::parse_peer_json(R"({"id":01})").is_discarded());
}

namespace {

/// What parse_peer_json() makes of @p text: what nlohmann::json::parse() makes of it, down to the
/// type of every number, while the fast parser either declines it or reads the same value.
void expect_peer_parse_alike(const std::string& text) {
    expect_agrees_with_nlohmann(text);
    const auto expected = nlohmann::json::parse(text, nullptr, false);
    const auto actual = mcp::detail::parse_peer_json(text);
    const auto shown =
        nlohmann::json(text).dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
    ASSERT_EQ(actual.is_discarded(), expected.is_discarded()) << shown;
    EXPECT_TRUE(expected.is_discarded() || identical(actual, expected)) << shown;
}

}  // namespace

TEST(JsonParseTest, AgreesWithNlohmannOnACorpusOfEdgeCases) {
    const std::vector<std::string> texts = {
        "",
        " ",
        "null",
        "true",
        "false",
        "nul",
        "truex",
        "nulll",
        "TRUE",
        "0",
        "-0",
        "-0.0",
        "01",
        "-01",
        "1.",
        ".5",
        "-",
        "+1",
        "1e",
        "1e+",
        "1E5",
        "1e-5",
        "1.5e+300",
        "1e309",
        "-1e309",
        "1e-400",
        "2.2250738585072014e-308",
        "9223372036854775807",
        "9223372036854775808",
        "-9223372036854775808",
        "-9223372036854775809",
        "18446744073709551615",
        "18446744073709551616",
        "123456789012345678901234567890",
        "0.1",
        "1.7976931348623157e308",
        "  [1 , 2 ]  ",
        "[1,]",
        "[,1]",
        "[",
        "]",
        "[[]]",
        "{}",
        "{ }",
        "{\"a\":1}",
        "{\"a\" : 1 }",
        "{\"a\":1,}",
        "{,}",
        "{\"a\"}",
        "{\"a\":}",
        "{a:1}",
        "{'a':1}",
        "{\"a\":1 \"b\":2}",
        "{\"a\":1,\"a\":2}",
        "{\"b\":1,\"a\":[true,false,null]}",
        "\"\"",
        "\"abc\"",
        "\"a\\\"b\"",
        "\"\\\\\"",
        "\"\\/\"",
        "\"\\b\\f\\n\\r\\t\"",
        "\"\\x\"",
        "\"\\u0041\"",
        "\"\\u00e9\"",
        "\"\\u20AC\"",
        "\"\\uD83D\\uDE00\"",
        "\"\\uD83D\"",
        "\"\\uD83Dx\"",
        "\"\\uD83D\\u0041\"",
        "\"\\uDE00\"",
        "\"\\u004\"",
        "\"\\u00G1\"",
        "\"\\u0000\"",
        "\"tab\there\"",
        "\"nl\nhere\"",
        "\"\x7f\"",
        "\"caf\xc3\xa9\"",
        "\"\xe2\x82\xac\"",
        "\"\xf0\x9f\x98\x80\"",
        "\"\xc3\"",
        "\"\xc0\x80\"",
        "\"\xc1\xbf\"",
        "\"\xe0\x80\x80\"",
        "\"\xe0\xa0\x80\"",
        "\"\xed\xa0\x80\"",
        "\"\xed\x9f\xbf\"",
        "\"\xf0\x80\x80\x80\"",
        "\"\xf0\x90\x80\x80\"",
        "\"\xf4\x8f\xbf\xbf\"",
        "\"\xf4\x90\x80\x80\"",
        "\"\xf5\x80\x80\x80\"",
        "\"\xff\"",
        "\"\x80\"",
        "\"unterminated",
        "\xef\xbb\xbf{}",
        "\xef\xbb{}",
        "\xef{}",
        " \xef\xbb\xbf{}",
        "\xef\xbb\xbf",
        "{}\xef\xbb\xbf",
        "1 2",
        "[1] [2]",
        "{} x",
        "/* c */ 1",
        "1 // c",
        "[1,2,3]\n",
        "\t{\r\n}\t",
        std::string("[1,\0 2]", 7),
        std::string("\"a\0b\"", 5),
        std::string("1\0", 2),
        "NaN",
        "Infinity",
        "-Infinity",
        "[-]",
        "[1e5,1E-5,0e0,-0e-0]",
        "{\"\":\"\"}",
        "[\"\\u0000\\u001f\"]",
    };
    for (const auto& text : texts) {
        expect_peer_parse_alike(text);
    }
}

TEST(JsonParseTest, ReadsGeneratedNumbersAsNlohmannDoes) {
    std::mt19937 random(7);
    for (int round = 0; round < 2000; ++round) {
        std::string text;
        if (random() % 2 == 0) {
            text += '-';
        }
        text += std::to_string(random() % 1000000);
        if (random() % 2 == 0) {
            text += '.' + std::to_string(random() % 100000);
        }
        if (random() % 3 == 0) {
            text += (random() % 2 == 0 ? "e" : "E");
            text += (random() % 2 == 0 ? "-" : "+");
            text += std::to_string(random() % 330);
        }
        expect_peer_parse_alike(text);
        expect_peer_parse_alike("[" + text + "]");
    }
}
