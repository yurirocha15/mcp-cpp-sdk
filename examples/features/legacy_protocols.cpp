/// @file legacy_protocols.cpp
/// @brief Demonstrates the older protocol revisions (2025-11-25 and earlier) next to 2026-07-28.
///
/// Every program that does not pin a protocol speaks 2026-07-28 with a server that serves it.
/// This example is about the other case: clients and servers that start a session with
/// `initialize`. It shows:
/// - ClientProtocol::Version() pinning each of the four older revisions, and Legacy() pinning
///   the newest of them (2025-11-25)
/// - ping() and resources/subscribe, which exist only in those revisions
/// - ServerProtocolPolicy and Server::set_protocol_policy(): which revisions a server serves, and
///   which versions `initialize` may negotiate
/// - What a 2026-07-28 session does with the methods that revision removed

#include <mcp/client/client.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace asio = boost::asio;
using json = nlohmann::json;

namespace {

bool check(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
    return condition;
}

const char* era_name(mcp::ProtocolEra era) {
    return era == mcp::ProtocolEra::eModern ? "modern" : "legacy";
}

const std::string g_resource_uri = "mcp://example/counter";
const mcp::Implementation g_client_info{"legacy-protocols-example-client", "1.0.0"};

std::shared_ptr<mcp::Server> make_server(const mcp::ServerProtocolPolicy& policy) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    mcp::ServerCapabilities::ResourcesCapability resources;
    resources.subscribe = true;
    caps.resources = resources;

    auto server = std::make_shared<mcp::Server>(
        mcp::Implementation{"legacy-protocols-example-server", "1.0.0"}, caps);

    mcp::Resource resource;
    resource.uri = g_resource_uri;
    resource.name = "counter";
    resource.mimeType = "text/plain";
    server->add_resource<json, json>(resource, [](const json& /*params*/) -> json {
        return json{{"contents", json::array({json{{"uri", g_resource_uri},
                                                   {"mimeType", "text/plain"},
                                                   {"text", "counter value: 1"}}})}};
    });

    server->set_protocol_policy(policy);
    return server;
}

mcp::Task<void> serve(std::shared_ptr<mcp::Server> server, std::shared_ptr<mcp::ITransport> transport,
                      asio::any_io_executor executor) {
    try {
        co_await server->run(std::move(transport), std::move(executor));
    } catch (const std::exception& error) {
        std::cerr << "[Server] Error: " << error.what() << '\n';
    }
}

/// Builds a client that speaks the given protocol.
std::unique_ptr<mcp::Client> make_client(std::shared_ptr<mcp::ITransport> transport,
                                         const asio::any_io_executor& executor,
                                         mcp::ClientProtocol protocol,
                                         std::function<void(const mcp::EraDecision&)> on_era = {}) {
    mcp::ClientOptions options;
    options.protocol = std::move(protocol);
    options.on_era_decision = std::move(on_era);
    return std::make_unique<mcp::Client>(std::move(transport), executor, std::move(options));
}

/// Starts @p server on a new in-memory connection and returns the client's end of it.
std::shared_ptr<mcp::ITransport> start_server(std::shared_ptr<mcp::Server> server,
                                              const asio::any_io_executor& executor) {
    auto [client_transport, server_transport] = mcp::create_memory_transport_pair(executor);
    asio::co_spawn(executor, serve(std::move(server), server_transport, executor), asio::detached);
    return client_transport;
}

mcp::Task<void> sleep_for(const asio::any_io_executor& executor, std::chrono::milliseconds duration) {
    asio::steady_timer timer(executor);
    timer.expires_after(duration);
    co_await timer.async_wait(asio::use_awaitable);
}

/// Waits up to two seconds for @p flag to turn true.
mcp::Task<bool> wait_for(const bool& flag, const asio::any_io_executor& executor) {
    for (int attempt = 0; attempt < 100 && !flag; ++attempt) {
        co_await sleep_for(executor, std::chrono::milliseconds(20));
    }
    co_return flag;
}

/// Pins one older revision with ClientProtocol::Version() and calls ping(), which only those
/// revisions have.
mcp::Task<bool> exercise_pinned_version(const char* version, const asio::any_io_executor& executor) {
    bool ok = true;
    auto transport = start_server(make_server(mcp::ServerProtocolPolicy{}), executor);

    auto client_owner =
        make_client(std::move(transport), executor, mcp::ClientProtocol::Version(version));
    mcp::Client& client = *client_owner;

    {
        mcp::ConnectResult connected =
            co_await client.connect(g_client_info, mcp::ClientCapabilities{});
        ok &= check(connected.era == mcp::ProtocolEra::eLegacy && connected.protocolVersion == version,
                    std::string("Version(\"") + version + "\") gives a " + era_name(connected.era) +
                        " session of " + connected.protocolVersion);
    }

    co_await client.ping();
    ok &= check(true, "ping() succeeds");
    client.close();
    co_return ok;
}

/// Legacy() is the newest of the older revisions.
mcp::Task<bool> exercise_legacy_pin(const asio::any_io_executor& executor) {
    bool ok = true;
    auto transport = start_server(make_server(mcp::ServerProtocolPolicy{}), executor);

    auto client_owner = make_client(std::move(transport), executor, mcp::ClientProtocol::Legacy());
    mcp::Client& client = *client_owner;

    {
        mcp::ConnectResult connected =
            co_await client.connect(g_client_info, mcp::ClientCapabilities{});
        ok &= check(connected.era == mcp::ProtocolEra::eLegacy &&
                        connected.protocolVersion == mcp::g_LATEST_LEGACY_PROTOCOL_VERSION,
                    "Legacy() gives a session of " + connected.protocolVersion);
    }
    client.close();
    co_return ok;
}

/// resources/subscribe and the notifications that follow it, in a 2025-11-25 session.
mcp::Task<bool> exercise_subscribe(const asio::any_io_executor& executor) {
    bool ok = true;
    auto server = make_server(mcp::ServerProtocolPolicy{});
    auto subscribed = std::make_shared<std::vector<std::string>>();
    auto unsubscribed = std::make_shared<std::vector<std::string>>();
    server->on_subscribe([subscribed](const std::string& uri) { subscribed->push_back(uri); });
    server->on_unsubscribe([unsubscribed](const std::string& uri) { unsubscribed->push_back(uri); });
    auto transport = start_server(server, executor);

    auto client_owner = make_client(std::move(transport), executor, mcp::ClientProtocol::Legacy());
    mcp::Client& client = *client_owner;

    bool updated = false;
    bool list_changed = false;
    client.on_notification("notifications/resources/updated", [&updated](const json& params) {
        std::cout << "  resource updated: " << params.dump() << '\n';
        updated = true;
    });
    client.on_notification("notifications/tools/list_changed", [&list_changed](const json&) {
        std::cout << "  tools list changed\n";
        list_changed = true;
    });

    co_await client.connect(g_client_info, mcp::ClientCapabilities{});

    const std::optional<json> subscribe_params = json{{"uri", g_resource_uri}};
    co_await client.send_request("resources/subscribe", subscribe_params);
    ok &= check(subscribed->size() == 1 && subscribed->front() == g_resource_uri,
                "resources/subscribe reached Server::on_subscribe");

    co_await server->notify_resource_updated(g_resource_uri);
    const bool got_update = co_await wait_for(updated, executor);
    ok &= check(got_update, "the subscriber received notifications/resources/updated");

    // Unlike resources/updated, the list-changed notifications go to every legacy session.
    co_await server->notify_tools_list_changed();
    const bool got_list_changed = co_await wait_for(list_changed, executor);
    ok &= check(got_list_changed, "the session received notifications/tools/list_changed");

    co_await client.send_request("resources/unsubscribe", subscribe_params);
    ok &= check(unsubscribed->size() == 1, "resources/unsubscribe reached Server::on_unsubscribe");
    client.close();
    co_return ok;
}

/// A policy that serves only 2025-03-26 and 2024-11-05 and no 2026-07-28 requests.
mcp::Task<bool> exercise_legacy_only_policy(const asio::any_io_executor& executor) {
    bool ok = true;
    mcp::ServerProtocolPolicy policy;
    policy.serve_modern = false;
    policy.legacy_versions = std::vector<std::string>{"2024-11-05", "2025-03-26"};
    auto transport = start_server(make_server(policy), executor);

    // The default Auto() probes with server/discover. This server refuses it, so the client falls
    // back to initialize, which negotiates the newest version the policy lists.
    auto client_owner = make_client(std::move(transport), executor, mcp::ClientProtocol::Auto(),
                                    [](const mcp::EraDecision& decision) {
                                        std::cout << "  era decision: " << era_name(decision.era)
                                                  << '\n';
                                    });
    mcp::Client& client = *client_owner;

    {
        mcp::ConnectResult connected =
            co_await client.connect(g_client_info, mcp::ClientCapabilities{});
        ok &= check(
            connected.era == mcp::ProtocolEra::eLegacy && connected.protocolVersion == "2025-03-26",
            "an Auto() client lands in a legacy session of " + connected.protocolVersion);
    }
    client.close();
    co_return ok;
}

/// A policy that serves only 2026-07-28 refuses a client that starts with initialize.
mcp::Task<bool> exercise_modern_only_policy(const asio::any_io_executor& executor) {
    bool ok = true;
    mcp::ServerProtocolPolicy policy;
    policy.serve_legacy = false;
    auto transport = start_server(make_server(policy), executor);

    auto client_owner = make_client(std::move(transport), executor, mcp::ClientProtocol::Legacy());
    mcp::Client& client = *client_owner;

    bool refused = false;
    try {
        co_await client.connect(g_client_info, mcp::ClientCapabilities{});
    } catch (const mcp::McpError& error) {
        std::cout << "  initialize refused: " << error.what() << '\n';
        refused = error.code() == mcp::g_METHOD_NOT_FOUND;
    }
    ok &= check(refused, "initialize is answered with g_METHOD_NOT_FOUND");
    client.close();
    co_return ok;
}

/// A 2026-07-28 session has no ping and no resources/subscribe; the client says so locally.
mcp::Task<bool> exercise_modern_contrast(const asio::any_io_executor& executor) {
    bool ok = true;
    auto transport = start_server(make_server(mcp::ServerProtocolPolicy{}), executor);

    auto client_owner =
        make_client(std::move(transport), executor, mcp::ClientProtocol::Version("2026-07-28"));
    mcp::Client& client = *client_owner;

    {
        mcp::ConnectResult connected =
            co_await client.connect(g_client_info, mcp::ClientCapabilities{});
        ok &= check(
            connected.era == mcp::ProtocolEra::eModern,
            std::string("Version(\"2026-07-28\") gives a ") + era_name(connected.era) + " session");
    }

    bool ping_failed = false;
    try {
        co_await client.ping();
    } catch (const mcp::McpError& error) {
        std::cout << "  ping: " << error.what() << '\n';
        ping_failed = error.code() == mcp::g_METHOD_NOT_FOUND;
    }
    ok &= check(ping_failed, "ping() fails with g_METHOD_NOT_FOUND");

    bool subscribe_failed = false;
    try {
        const std::optional<json> subscribe_params = json{{"uri", g_resource_uri}};
        co_await client.send_request("resources/subscribe", subscribe_params);
    } catch (const mcp::McpError& error) {
        std::cout << "  resources/subscribe: " << error.what() << '\n';
        subscribe_failed = error.code() == mcp::g_METHOD_NOT_FOUND;
    }
    ok &= check(subscribe_failed, "resources/subscribe fails with g_METHOD_NOT_FOUND");
    client.close();
    co_return ok;
}

mcp::Task<bool> run_all(const asio::any_io_executor& executor) {
    bool ok = true;

    std::cout << "Pinning each older revision\n";
    for (const char* version : {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}) {
        const bool pinned = co_await exercise_pinned_version(version, executor);
        ok = ok && pinned;
    }
    std::cout << "Legacy()\n";
    const bool legacy_pin = co_await exercise_legacy_pin(executor);
    ok = ok && legacy_pin;

    std::cout << "resources/subscribe in a 2025-11-25 session\n";
    const bool subscribe = co_await exercise_subscribe(executor);
    ok = ok && subscribe;

    std::cout << "ServerProtocolPolicy: legacy versions only\n";
    const bool legacy_only = co_await exercise_legacy_only_policy(executor);
    ok = ok && legacy_only;

    std::cout << "ServerProtocolPolicy: 2026-07-28 only\n";
    const bool modern_only = co_await exercise_modern_only_policy(executor);
    ok = ok && modern_only;

    std::cout << "The same calls in a 2026-07-28 session\n";
    const bool contrast = co_await exercise_modern_contrast(executor);
    ok = ok && contrast;
    co_return ok;
}

}  // namespace

int main() {
    try {
        asio::io_context io_ctx;
        bool succeeded = false;

        asio::co_spawn(
            io_ctx,
            [&]() -> mcp::Task<void> {
                try {
                    succeeded = co_await run_all(io_ctx.get_executor());
                } catch (const std::exception& error) {
                    std::cerr << "Error: " << error.what() << '\n';
                }
                io_ctx.stop();
            },
            asio::detached);

        // Fail rather than hang if something never answers.
        asio::steady_timer watchdog(io_ctx.get_executor());
        watchdog.expires_after(std::chrono::seconds(20));
        watchdog.async_wait([&io_ctx](const boost::system::error_code& error) {
            if (!error) {
                io_ctx.stop();
            }
        });

        io_ctx.run();

        if (!succeeded) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
