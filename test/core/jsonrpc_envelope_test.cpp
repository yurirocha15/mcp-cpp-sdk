#include "../../src/detail/jsonrpc_envelope.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using mcp::detail::scan_jsonrpc_envelope;

TEST(JsonRpcEnvelopeTest, FindsTheIdOfAResultResponse) {
    const auto scan = scan_jsonrpc_envelope(R"({"id":7,"jsonrpc":"2.0","result":{"tools":[]}})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, "7");
    EXPECT_TRUE(scan->has_result_or_error);
    EXPECT_TRUE(scan->has_result);
}

TEST(JsonRpcEnvelopeTest, FindsTheIdOfAnErrorResponse) {
    const auto scan = scan_jsonrpc_envelope(
        R"({"error":{"code":-32601,"message":"Method not found"},"id":"req-1","jsonrpc":"2.0"})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, R"("req-1")");
    EXPECT_TRUE(scan->has_result_or_error);
    EXPECT_FALSE(scan->has_result);
}

TEST(JsonRpcEnvelopeTest, ReportsNoIdForANotification) {
    const auto scan =
        scan_jsonrpc_envelope(R"({"jsonrpc":"2.0","method":"notifications/progress","params":{}})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_TRUE(scan->id.empty());
    EXPECT_FALSE(scan->has_result_or_error);
    EXPECT_FALSE(scan->has_result);
}

TEST(JsonRpcEnvelopeTest, ReportsARequestAsNotAResponse) {
    const auto scan =
        scan_jsonrpc_envelope(R"({"id":3,"jsonrpc":"2.0","method":"sampling/createMessage"})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, "3");
    EXPECT_FALSE(scan->has_result_or_error);
}

TEST(JsonRpcEnvelopeTest, SkipsNestedValuesAndStringsThatContainDelimiters) {
    const auto scan = scan_jsonrpc_envelope(
        R"({"result":{"content":[{"text":"}{][\",:\\","type":"text"}]},"jsonrpc":"2.0","id":12})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, "12");
    EXPECT_TRUE(scan->has_result_or_error);
}

TEST(JsonRpcEnvelopeTest, KeepsAnEscapedStringIdVerbatim) {
    const auto scan = scan_jsonrpc_envelope(R"({"id":"a\"b","result":{}})");
    ASSERT_TRUE(scan.has_value());
    // Named outside the assertion: MSVC 19.44's traditional preprocessor lexes a raw string in a
    // macro argument as an ordinary one, so a \" inside it starts a string that never ends.
    constexpr std::string_view expected_id = R"("a\"b")";
    EXPECT_EQ(scan->id, expected_id);
}

TEST(JsonRpcEnvelopeTest, AcceptsWhitespaceBetweenTokens) {
    const auto scan = scan_jsonrpc_envelope(" {\n \"id\" : null ,\t\"error\" : { \"code\" : 1 } }\r\n");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, "null");
    EXPECT_TRUE(scan->has_result_or_error);
}

TEST(JsonRpcEnvelopeTest, TakesTheLastOfDuplicateIds) {
    const auto scan = scan_jsonrpc_envelope(R"({"id":1,"result":{},"id":2})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, "2");
}

TEST(JsonRpcEnvelopeTest, AcceptsAnEmptyObject) {
    const auto scan = scan_jsonrpc_envelope("{}");
    ASSERT_TRUE(scan.has_value());
    EXPECT_TRUE(scan->id.empty());
    EXPECT_FALSE(scan->has_result_or_error);
}

TEST(JsonRpcEnvelopeTest, DeclinesWhatItCannotDelimit) {
    const std::vector<std::string_view> rejected = {
        "",
        "[]",
        R"("id")",
        R"({"id":1)",
        R"({"id" 1})",
        R"({"id":})",
        R"({"id":1,})",
        R"({"id":1 "result":{}})",
        R"({"id":"unterminated})",
        R"({"result":{"open":[1,2})",
        R"({id:1})",
    };
    for (const auto text : rejected) {
        EXPECT_FALSE(scan_jsonrpc_envelope(text).has_value()) << text;
    }
}

TEST(JsonRpcEnvelopeTest, DeclinesAnEscapedMemberName) {
    // The member name below is "id" spelled with a unicode escape; matching it would need the
    // full parser.
    const std::string text = std::string(R"({")") + '\\' + R"(u0069d":1,"result":{}})";
    EXPECT_FALSE(scan_jsonrpc_envelope(text).has_value());
}

TEST(JsonRpcEnvelopeTest, AgreesWithTheFullParserOnServerMessages) {
    const std::vector<nlohmann::json> messages = {
        {{"jsonrpc", "2.0"},
         {"id", 1},
         {"result", {{"content", {{{"type", "text"}, {"text", "{}"}}}}}}},
        {{"jsonrpc", "2.0"}, {"id", "x"}, {"error", {{"code", -32603}, {"message", "boom"}}}},
        {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse"}}}},
        {{"jsonrpc", "2.0"}, {"method", "notifications/message"}, {"params", {{"data", "]["}}}},
        {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "roots/list"}},
    };
    for (const auto& message : messages) {
        const auto wire = message.dump();
        const auto scan = scan_jsonrpc_envelope(wire);
        ASSERT_TRUE(scan.has_value()) << wire;
        if (message.contains("id")) {
            EXPECT_EQ(nlohmann::json::parse(scan->id), message.at("id")) << wire;
        } else {
            EXPECT_TRUE(scan->id.empty()) << wire;
        }
        EXPECT_EQ(scan->has_result_or_error, message.contains("result") || message.contains("error"))
            << wire;
        EXPECT_EQ(scan->has_result, message.contains("result")) << wire;
    }
}

namespace {

/// The scan as it was before it skipped eight bytes at a time: one byte at a time throughout. The
/// tests below hold the current scan to it.
namespace byte_scan {

std::size_t skip_json_whitespace(std::string_view text, std::size_t pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

std::size_t skip_json_string(std::string_view text, std::size_t pos) {
    for (++pos; pos < text.size(); ++pos) {
        if (text[pos] == '\\') {
            ++pos;
        } else if (text[pos] == '"') {
            return pos + 1;
        }
    }
    return std::string_view::npos;
}

std::size_t skip_json_value(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return std::string_view::npos;
    }
    if (text[pos] == '"') {
        return skip_json_string(text, pos);
    }
    if (text[pos] != '{' && text[pos] != '[') {
        const auto end = text.find_first_of(",}] \t\r\n", pos);
        return end == std::string_view::npos ? text.size() : end;
    }
    std::size_t depth = 0;
    while (pos < text.size()) {
        const char current = text[pos];
        if (current == '"') {
            pos = skip_json_string(text, pos);
            if (pos == std::string_view::npos) {
                return pos;
            }
            continue;
        }
        if (current == '{' || current == '[') {
            ++depth;
        } else if ((current == '}' || current == ']') && --depth == 0) {
            return pos + 1;
        }
        ++pos;
    }
    return std::string_view::npos;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): kept as the scan was written.
std::optional<mcp::detail::JsonRpcEnvelope> scan_jsonrpc_envelope(std::string_view text) {
    auto pos = skip_json_whitespace(text, 0);
    if (pos >= text.size() || text[pos] != '{') {
        return std::nullopt;
    }
    mcp::detail::JsonRpcEnvelope envelope;
    pos = skip_json_whitespace(text, pos + 1);
    if (pos < text.size() && text[pos] == '}') {
        return envelope;
    }
    for (;;) {
        if (pos >= text.size() || text[pos] != '"') {
            return std::nullopt;
        }
        const auto key_end = skip_json_string(text, pos);
        if (key_end == std::string_view::npos) {
            return std::nullopt;
        }
        const auto key = text.substr(pos + 1, key_end - pos - 2);
        if (key.find('\\') != std::string_view::npos) {
            return std::nullopt;
        }
        pos = skip_json_whitespace(text, key_end);
        if (pos >= text.size() || text[pos] != ':') {
            return std::nullopt;
        }
        pos = skip_json_whitespace(text, pos + 1);
        const auto value_end = skip_json_value(text, pos);
        if (value_end == std::string_view::npos || value_end == pos) {
            return std::nullopt;
        }
        if (key == "id") {
            envelope.id = text.substr(pos, value_end - pos);
        } else if (key == "result") {
            envelope.has_result_or_error = true;
            envelope.has_result = true;
        } else if (key == "error") {
            envelope.has_result_or_error = true;
        }
        pos = skip_json_whitespace(text, value_end);
        if (pos >= text.size()) {
            return std::nullopt;
        }
        if (text[pos] == '}') {
            return envelope;
        }
        if (text[pos] != ',') {
            return std::nullopt;
        }
        pos = skip_json_whitespace(text, pos + 1);
    }
}

}  // namespace byte_scan

/// Checks the current string scan against the byte scan on @p text, closed and cut short.
void expect_string_end_like_byte_scan(const std::string& text) {
    EXPECT_EQ(mcp::detail::skip_json_string(text, 0), byte_scan::skip_json_string(text, 0)) << text;
}

/// Checks the current envelope scan against the byte scan on every prefix of @p text.
std::size_t expect_envelope_like_byte_scan(const std::string& text) {
    for (std::size_t length = 0; length <= text.size(); ++length) {
        const auto view = std::string_view(text).substr(0, length);
        const auto current = scan_jsonrpc_envelope(view);
        const auto reference = byte_scan::scan_jsonrpc_envelope(view);
        EXPECT_EQ(current.has_value(), reference.has_value()) << view;
        if (current.has_value() && reference.has_value()) {
            EXPECT_EQ(current->id, reference->id) << view;
            EXPECT_EQ(current->has_result_or_error, reference->has_result_or_error) << view;
            EXPECT_EQ(current->has_result, reference->has_result) << view;
        }
    }
    return text.size() + 1;
}

}  // namespace

TEST(JsonRpcEnvelopeTest, FindsTheEndOfAStringAtEveryOffsetOfItsEscapes) {
    // Quotes and backslash runs at each position around the eight-byte steps of the scan,
    // including strings that end inside an escape and so never close.
    const std::vector<std::string> tails = {R"(")",  R"(\"")",   R"(\\")",   R"(\\\"")",
                                            R"(\)",  R"(\\)",    R"(\")",    R"(x\\\"y")",
                                            R"(A")", R"(\n\t")", R"(\\\\")", ""};
    constexpr std::size_t g_longest_prefix = 24;
    constexpr std::size_t g_longest_suffix = 10;
    for (std::size_t prefix = 0; prefix < g_longest_prefix; ++prefix) {
        for (const auto& tail : tails) {
            const std::string opening = '"' + std::string(prefix, 'a') + tail;
            expect_string_end_like_byte_scan(opening);
            for (std::size_t suffix = 0; suffix < g_longest_suffix; ++suffix) {
                std::string text = opening;
                text.append(suffix, 'b').append(1, '"').append(suffix, 'c');
                expect_string_end_like_byte_scan(text);
            }
        }
    }
}

TEST(JsonRpcEnvelopeTest, ScansALongToolResultLikeTheFullParser) {
    constexpr int g_items = 200;
    std::string text;
    for (int index = 0; index < g_items; ++index) {
        text.append(R"({"id":)")
            .append(std::to_string(index))
            .append(R"(,"name":"item \"quoted\" \\ )")
            .append(std::to_string(index))
            .append(R"("})");
    }
    const nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"id", "call-7"},
        {"result", {{"content", {{{"type", "text"}, {"text", text}}}}, {"isError", false}}}};
    const auto wire = message.dump();
    const auto scan = scan_jsonrpc_envelope(wire);
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan.value().id, R"("call-7")");
    EXPECT_TRUE(scan.value().has_result);
    expect_envelope_like_byte_scan(wire);
}

TEST(JsonRpcEnvelopeTest, AgreesWithTheByteScanOnGeneratedEnvelopes) {
    // Ids at and past the int64 range, doubles, negative zero, exponents, escapes, duplicate and
    // reordered members, and every truncation of each.
    const std::vector<std::string> ids = {"0",
                                          "-0",
                                          "7",
                                          "9223372036854775807",
                                          "9223372036854775808",
                                          "18446744073709551616",
                                          "-9223372036854775809",
                                          "1.5",
                                          "-0.0",
                                          "1e3",
                                          "1E+30",
                                          "2.5e-7",
                                          "null",
                                          "true",
                                          R"("")",
                                          R"("req-1")",
                                          R"("a\"b")",
                                          R"("\\")",
                                          R"("A\n")",
                                          R"("long id with \"quotes\" past eight bytes")",
                                          R"({"nested":"}"})",
                                          R"([1,"]",2])"};
    const std::vector<std::string> bodies = {
        R"("result":{})",
        R"("result":null)",
        R"("error":{"code":-32603,"message":"a \"b\" ]}"})",
        R"("method":"tools/call","params":{"a":"\\"})",
        R"("result":{"content":[{"type":"text","text":"{\"id\":1,\"name\":\"x\\y\"}"}]})",
        R"("result":{},"result":{"again":true})"};
    std::size_t compared = 0;
    for (const auto& id : ids) {
        for (const auto& body : bodies) {
            compared +=
                expect_envelope_like_byte_scan(R"({"id":)" + id + R"(,"jsonrpc":"2.0",)" + body + "}");
            compared +=
                expect_envelope_like_byte_scan(R"({"jsonrpc":"2.0",)" + body + R"(,"id":)" + id + "}");
            compared += expect_envelope_like_byte_scan(R"({ "id" : )" + id + " ,\n\t" + body + " }");
            compared += expect_envelope_like_byte_scan(R"({"id":1,"id":)" + id + "," + body + "}");
        }
    }
    constexpr std::size_t g_least_compared = 10000;
    EXPECT_GT(compared, g_least_compared);
}
