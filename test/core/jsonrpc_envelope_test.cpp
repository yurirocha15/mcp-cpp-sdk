#include "../../src/detail/jsonrpc_envelope.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

using mcp::detail::scan_jsonrpc_envelope;

TEST(JsonRpcEnvelopeTest, FindsTheIdOfAResultResponse) {
    const auto scan = scan_jsonrpc_envelope(R"({"id":7,"jsonrpc":"2.0","result":{"tools":[]}})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, "7");
    EXPECT_TRUE(scan->has_result_or_error);
}

TEST(JsonRpcEnvelopeTest, FindsTheIdOfAnErrorResponse) {
    const auto scan = scan_jsonrpc_envelope(
        R"({"error":{"code":-32601,"message":"Method not found"},"id":"req-1","jsonrpc":"2.0"})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->id, R"("req-1")");
    EXPECT_TRUE(scan->has_result_or_error);
}

TEST(JsonRpcEnvelopeTest, ReportsNoIdForANotification) {
    const auto scan =
        scan_jsonrpc_envelope(R"({"jsonrpc":"2.0","method":"notifications/progress","params":{}})");
    ASSERT_TRUE(scan.has_value());
    EXPECT_TRUE(scan->id.empty());
    EXPECT_FALSE(scan->has_result_or_error);
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
    }
}
