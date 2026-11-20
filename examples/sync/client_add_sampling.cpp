// client_base.cpp answering a server's requests for model output, such as those of
// server_add_sampling.cpp. A real client would pass the request to its language model.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.on_sampling(
        [](const mcp::CreateMessageRequestParams&) -> mcp::Task<mcp::CreateMessageResult> {
            mcp::CreateMessageResult reply;
            reply.role = mcp::Role::eAssistant;
            reply.content = mcp::ContentBlock{mcp::TextContent{.text = "Greetings, friend!"}};
            reply.model = "example-model";
            co_return reply;
        });
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
    std::cout << mcp::result_text(client.sync().call_tool("write_greeting", {{"name", "Ada"}})) << "\n";
}
