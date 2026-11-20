// client_base.cpp answering the questions a server asks the user, such as those of
// server_add_elicitation.cpp. A real client would show the form to its user.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        co_return mcp::ElicitResult{mcp::ElicitAction::eAccept, nlohmann::json{{"title", "Dr."}},
                                    std::nullopt};
    });
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
    std::cout << mcp::result_text(client.sync().call_tool("greet_formally", {{"name", "Ada"}})) << "\n";
}
