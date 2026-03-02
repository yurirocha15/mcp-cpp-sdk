#include "mcp/concepts.hpp"
#include "mcp/protocol.hpp"

#include <gtest/gtest.h>

// --- Positive: ADL free functions ---
static_assert(mcp::JsonSerializable<mcp::Icon>);
static_assert(mcp::JsonSerializable<mcp::Annotations>);
static_assert(mcp::JsonSerializable<mcp::ToolAnnotations>);
static_assert(mcp::JsonSerializable<mcp::Implementation>);
static_assert(mcp::JsonSerializable<mcp::ClientCapabilities>);
static_assert(mcp::JsonSerializable<mcp::ServerCapabilities>);
static_assert(mcp::JsonSerializable<mcp::InitializeResult>);
static_assert(mcp::JsonSerializable<mcp::Tool>);
static_assert(mcp::JsonSerializable<mcp::CallToolResult>);
static_assert(mcp::JsonSerializable<mcp::Resource>);
static_assert(mcp::JsonSerializable<mcp::ResourceTemplate>);
static_assert(mcp::JsonSerializable<mcp::Error>);
static_assert(mcp::JsonSerializable<mcp::JSONRPCRequest>);
static_assert(mcp::JsonSerializable<mcp::JSONRPCNotification>);
static_assert(mcp::JsonSerializable<mcp::JSONRPCResponse>);
static_assert(mcp::JsonSerializable<mcp::Prompt>);
static_assert(mcp::JsonSerializable<mcp::PromptArgument>);

// --- Positive: NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE ---
static_assert(mcp::JsonSerializable<mcp::InitializeRequest>);
static_assert(mcp::JsonSerializable<mcp::GetPromptRequest>);
static_assert(mcp::JsonSerializable<mcp::ReadResourceResult>);
static_assert(mcp::JsonSerializable<mcp::ToolExecution>);
static_assert(mcp::JsonSerializable<mcp::SetLevelRequest>);

// --- Positive: adl_serializer specialization ---
static_assert(mcp::JsonSerializable<mcp::RequestId>);
static_assert(mcp::JsonSerializable<mcp::ProgressToken>);

// --- Negative: types without JSON serialization ---
static_assert(!mcp::JsonSerializable<void*>);
static_assert(!mcp::JsonSerializable<std::mutex>);

// --- Positive: nlohmann::json-native types (sanity check) ---
static_assert(mcp::JsonSerializable<std::string>);
static_assert(mcp::JsonSerializable<int>);
static_assert(mcp::JsonSerializable<bool>);
static_assert(mcp::JsonSerializable<double>);
static_assert(mcp::JsonSerializable<std::vector<int>>);

class ConceptsTest : public ::testing::Test {};

TEST_F(ConceptsTest, JsonSerializableRoundTrip) {
    mcp::Tool tool;
    tool.name = "test_tool";
    tool.description = "A test tool";
    tool.inputSchema = nlohmann::json::object();

    nlohmann::json j = tool;
    auto deserialized = j.get<mcp::Tool>();

    EXPECT_EQ(deserialized.name, tool.name);
    EXPECT_EQ(deserialized.description, tool.description);
}

TEST_F(ConceptsTest, JsonSerializableRequestIdRoundTrip) {
    mcp::RequestId string_id = std::string("req-42");
    nlohmann::json j = string_id;
    auto deserialized = j.get<mcp::RequestId>();
    EXPECT_EQ(deserialized, string_id);

    mcp::RequestId int_id = int64_t{99};
    j = int_id;
    deserialized = j.get<mcp::RequestId>();
    EXPECT_EQ(deserialized, int_id);
}
