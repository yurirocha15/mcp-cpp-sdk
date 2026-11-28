#include "mcp/core/concepts.hpp"
#include "mcp/protocol/protocol.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace concepts_test {

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

static_assert(mcp::JsonSerializable<mcp::InitializeRequest>);
static_assert(mcp::JsonSerializable<mcp::GetPromptRequest>);
static_assert(mcp::JsonSerializable<mcp::ReadResourceResult>);
static_assert(mcp::JsonSerializable<mcp::DetailedTask>);
static_assert(mcp::JsonSerializable<mcp::SetLevelRequest>);

static_assert(mcp::JsonSerializable<mcp::RequestId>);
static_assert(mcp::JsonSerializable<mcp::ProgressToken>);

static_assert(!mcp::JsonSerializable<void*>);
static_assert(!mcp::JsonSerializable<std::mutex>);

static_assert(mcp::JsonSerializable<std::string>);
static_assert(mcp::JsonSerializable<int>);
static_assert(mcp::JsonSerializable<bool>);
static_assert(mcp::JsonSerializable<double>);
static_assert(mcp::JsonSerializable<std::vector<int>>);

}  // namespace concepts_test
