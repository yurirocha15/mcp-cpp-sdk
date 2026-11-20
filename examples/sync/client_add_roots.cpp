// client_base.cpp telling servers which directories they may work in, as
// server_add_roots.cpp asks.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.on_roots_list([](const nlohmann::json&) -> mcp::Task<mcp::ListRootsResult> {
        mcp::ListRootsResult roots;
        roots.roots.push_back({"file:///home/user/projects", "projects", std::nullopt});
        co_return roots;
    });
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
    std::cout << mcp::result_text(client.sync().call_tool("count_roots", nlohmann::json::object()))
              << "\n";
}
