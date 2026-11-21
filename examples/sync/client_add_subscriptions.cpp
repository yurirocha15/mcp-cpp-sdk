// client_base.cpp subscribing to a resource and waiting for the server to say it changed, as
// server_add_subscriptions.cpp does when its tool runs.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
    mcp::SubscriptionFilter filter;
    filter.resourceSubscriptions = {"greeter://last"};
    auto subscription = client.sync().listen(filter);
    if (!subscription.unhonored().resourceSubscriptions.empty()) {
        std::cout << "the server does not offer subscriptions to greeter://last\n";
        return 1;
    }
    client.sync().call_tool("remember", {{"name", "Ada"}});
    if (auto event = subscription.sync().next()) {
        std::cout << "updated: " << event->method << "\n";
    }
}
