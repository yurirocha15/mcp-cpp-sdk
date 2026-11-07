/**
 * @file e2e_matrix_test.cpp
 *
 * Every client protocol choice against every server protocol policy over every transport, with
 * the SDK on both ends. Each cell connects, calls a tool that reports the era it was served in,
 * and checks the outcome the compatibility matrix of the 2026-07-28 versioning page prescribes,
 * including the combinations that must fail with an error naming the versions the server serves.
 */
#include "../pipe_stream.hpp"
#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/memory.hpp"
#include "mcp/transport/stdio.hpp"
#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

enum class ClientKind {
    eAuto,
    eModern,
    eLegacy,
    eLegacy2025_06_18,
    eLegacy2025_03_26,
    eLegacy2024_11_05
};
enum class ServerKind {
    eDualEra,
    eModernOnly,
    eLegacyOnly
};
enum class TransportKind {
    eMemory,
    eStdio,
    eSessionManager,
    eStatelessJson,
    eRunHttp,
    eWebSocket
};

const std::vector<ClientKind> g_clients = {ClientKind::eAuto,
                                           ClientKind::eModern,
                                           ClientKind::eLegacy,
                                           ClientKind::eLegacy2025_06_18,
                                           ClientKind::eLegacy2025_03_26,
                                           ClientKind::eLegacy2024_11_05};
const std::vector<ServerKind> g_servers = {ServerKind::eDualEra, ServerKind::eModernOnly,
                                           ServerKind::eLegacyOnly};
const std::vector<TransportKind> g_transports = {
    TransportKind::eMemory,        TransportKind::eStdio,   TransportKind::eSessionManager,
    TransportKind::eStatelessJson, TransportKind::eRunHttp, TransportKind::eWebSocket};

constexpr auto g_hang_guard = std::chrono::seconds(30);
constexpr unsigned short g_first_port = 20610;

std::string client_name(ClientKind kind) {
    switch (kind) {
        case ClientKind::eAuto:
            return "Auto";
        case ClientKind::eModern:
            return "Modern";
        case ClientKind::eLegacy:
            return "Legacy";
        case ClientKind::eLegacy2025_06_18:
            return "Pinned2025_06_18";
        case ClientKind::eLegacy2025_03_26:
            return "Pinned2025_03_26";
        case ClientKind::eLegacy2024_11_05:
            return "Pinned2024_11_05";
    }
    return "Unknown";
}

std::string server_name(ServerKind kind) {
    switch (kind) {
        case ServerKind::eDualEra:
            return "DualEra";
        case ServerKind::eModernOnly:
            return "ModernOnly";
        case ServerKind::eLegacyOnly:
            return "LegacyOnly";
    }
    return "Unknown";
}

std::string transport_name(TransportKind kind) {
    switch (kind) {
        case TransportKind::eMemory:
            return "Memory";
        case TransportKind::eStdio:
            return "Stdio";
        case TransportKind::eSessionManager:
            return "SessionManager";
        case TransportKind::eStatelessJson:
            return "StatelessJson";
        case TransportKind::eRunHttp:
            return "RunHttp";
        case TransportKind::eWebSocket:
            return "WebSocket";
    }
    return "Unknown";
}

mcp::ClientProtocol protocol_of(ClientKind kind) {
    switch (kind) {
        case ClientKind::eAuto:
            return mcp::ClientProtocol::Auto();
        case ClientKind::eModern:
            return mcp::ClientProtocol::Version("2026-07-28");
        case ClientKind::eLegacy:
            return mcp::ClientProtocol::Legacy();
        case ClientKind::eLegacy2025_06_18:
            return mcp::ClientProtocol::Version("2025-06-18");
        case ClientKind::eLegacy2025_03_26:
            return mcp::ClientProtocol::Version("2025-03-26");
        case ClientKind::eLegacy2024_11_05:
            return mcp::ClientProtocol::Version("2024-11-05");
    }
    return mcp::ClientProtocol::Auto();
}

mcp::ServerProtocolPolicy policy_of(ServerKind kind) {
    mcp::ServerProtocolPolicy policy;
    policy.serve_legacy = kind != ServerKind::eModernOnly;
    policy.serve_modern = kind != ServerKind::eLegacyOnly;
    return policy;
}

/// What a cell must end with: the era and version of a working connection, or a failure whose
/// message names a version the server does serve.
struct Expected {
    bool connects{true};
    mcp::ProtocolEra era{mcp::ProtocolEra::eLegacy};
    std::string version;
    std::string named_in_failure;
};

Expected expected_for(ClientKind client, ServerKind server) {
    const bool legacy_client = client != ClientKind::eAuto && client != ClientKind::eModern;
    if (client == ClientKind::eAuto) {
        if (server == ServerKind::eLegacyOnly) {
            return {true, mcp::ProtocolEra::eLegacy, "2025-11-25", ""};
        }
        return {true, mcp::ProtocolEra::eModern, "2026-07-28", ""};
    }
    if (client == ClientKind::eModern) {
        if (server == ServerKind::eLegacyOnly) {
            return {false, mcp::ProtocolEra::eModern, "", "2025-11-25"};
        }
        return {true, mcp::ProtocolEra::eModern, "2026-07-28", ""};
    }
    if (legacy_client && server == ServerKind::eModernOnly) {
        return {false, mcp::ProtocolEra::eLegacy, "", "2026-07-28"};
    }
    switch (client) {
        case ClientKind::eLegacy2025_06_18:
            return {true, mcp::ProtocolEra::eLegacy, "2025-06-18", ""};
        case ClientKind::eLegacy2025_03_26:
            return {true, mcp::ProtocolEra::eLegacy, "2025-03-26", ""};
        case ClientKind::eLegacy2024_11_05:
            return {true, mcp::ProtocolEra::eLegacy, "2024-11-05", ""};
        default:
            return {true, mcp::ProtocolEra::eLegacy, "2025-11-25", ""};
    }
}

/// A server with a tool that reports the era and version its request was served in, and one that
/// asks the client for a name.
std::unique_ptr<mcp::Server> make_server(ServerKind kind) {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{true};
    auto server =
        std::make_unique<mcp::Server>(mcp::Implementation{"matrix-server", "1.0"}, capabilities);
    server->add_tool<json, json>(
        "ask", "Asks the client for a name", json{{"type", "object"}},
        [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            mcp::ElicitRequestFormParams params;
            params.message = "Your name?";
            params.requestedSchema = {{"type", "object"},
                                      {"properties", {{"name", {{"type", "string"}}}}}};
            const auto answer = co_await ctx.elicit(params);
            co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
        });
    server->add_tool<json, json>("whoami", "Reports the era of the request", json{{"type", "object"}},
                                 [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                     const auto* meta = ctx.request_meta();
                                     if (meta == nullptr) {
                                         co_return json{{"modern", false}};
                                     }
                                     co_return json{{"modern", true},
                                                    {"version", meta->protocolVersion}};
                                 });
    server->set_protocol_policy(policy_of(kind));
    return server;
}

struct Outcome {
    mcp::ConnectResult connected;
    json whoami;
    std::string error;
};

mcp::Task<void> connect_and_ask(mcp::Client& client, std::shared_ptr<Outcome> outcome) {
    try {
        outcome->connected = co_await client.connect("matrix-client", "1.0");
        const auto result = co_await client.call_tool("whoami", json::object());
        outcome->whoami = result.structuredContent.value_or(json::object());
    } catch (const std::exception& error) {
        outcome->error = error.what();
    }
}

/// Runs @p body on @p io and stops the context if it has not finished after g_hang_guard.
void run_guarded(boost::asio::io_context& io, std::function<mcp::Task<void>()> body) {
    boost::asio::steady_timer guard(io, g_hang_guard);
    bool fired = false;
    guard.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            fired = true;
            io.stop();
        }
    });
    boost::asio::co_spawn(
        io,
        [&guard, body = std::move(body)]() -> mcp::Task<void> {
            co_await body();
            guard.cancel();
        },
        boost::asio::detached);
    io.run();
    EXPECT_FALSE(fired) << "the cell did not finish";
}

mcp::Task<void> serve(mcp::Server* server, std::shared_ptr<mcp::ITransport> transport,
                      boost::asio::any_io_executor executor) {
    try {
        co_await server->run(std::move(transport), std::move(executor));
    } catch (const std::exception&) {
    }
}

mcp::Task<void> accept_websocket(boost::asio::ip::tcp::acceptor* acceptor, mcp::Server* server,
                                 boost::asio::any_io_executor executor) {
    try {
        auto socket = co_await acceptor->async_accept(boost::asio::use_awaitable);
        auto transport = std::make_shared<mcp::WebSocketServerTransport>(std::move(socket));
        co_await server->run(transport, std::move(executor));
    } catch (const std::exception&) {
    }
}

/// A fixed port per session-manager cell, so cells never wait for each other's sockets.
unsigned short port_for(ClientKind client, ServerKind server, TransportKind transport) {
    const auto client_index = static_cast<unsigned short>(client);
    const auto server_index = static_cast<unsigned short>(server);
    const unsigned short stateless = transport == TransportKind::eStatelessJson ? 1 : 0;
    return static_cast<unsigned short>(g_first_port + ((client_index * 3 + server_index) * 2) +
                                       stateless);
}

Outcome run_cell(ClientKind client_kind, ServerKind server_kind, TransportKind transport_kind) {
    boost::asio::io_context io;
    auto outcome = std::make_shared<Outcome>();
    mcp::ClientOptions options;
    options.protocol = protocol_of(client_kind);

    switch (transport_kind) {
        case TransportKind::eMemory: {
            auto server = make_server(server_kind);
            auto [client_transport, server_transport] =
                mcp::create_memory_transport_pair(io.get_executor());
            mcp::Client client(client_transport, io.get_executor(), options);
            boost::asio::co_spawn(io, serve(server.get(), server_transport, io.get_executor()),
                                  boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await connect_and_ask(client, outcome);
                client.close();
                server_transport->close();
            });
            break;
        }
        case TransportKind::eStdio: {
            auto server = make_server(server_kind);
            PipeBuffer to_server;
            PipeBuffer to_client;
            PipeWriter server_out(to_client);
            PipeWriter client_out(to_server);
            std::istream server_in(&to_server);
            std::ostream server_output(&server_out);
            std::istream client_in(&to_client);
            std::ostream client_output(&client_out);
            std::thread server_thread([&] { server->run_stdio(server_in, server_output); });
            auto transport =
                std::make_shared<mcp::StdioTransport>(io.get_executor(), client_in, client_output);
            mcp::Client client(transport, io.get_executor(), options);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await connect_and_ask(client, outcome);
                client.close();
            });
            // The server sees end of input and returns; then the client's reader does. The client
            // outlives both closes, because its transport waits for that reader when destroyed.
            to_server.close();
            server_thread.join();
            to_client.close();
            break;
        }
        case TransportKind::eSessionManager:
        case TransportKind::eStatelessJson: {
            const auto port = port_for(client_kind, server_kind, transport_kind);
            mcp::StreamableHttpSessionManager manager(
                io.get_executor(), "127.0.0.1", port,
                [server_kind](const boost::asio::any_io_executor&) {
                    return make_server(server_kind);
                });
            manager.set_protocol_policy(policy_of(server_kind));
            manager.set_stateless_json_mode(transport_kind == TransportKind::eStatelessJson);
            auto transport = std::make_shared<mcp::HttpClientTransport>(
                io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
            mcp::Client client(transport, io.get_executor(), options);
            boost::asio::co_spawn(io, manager.listen(), boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await connect_and_ask(client, outcome);
                client.close();
                manager.close();
            });
            break;
        }
        case TransportKind::eRunHttp: {
            auto server = make_server(server_kind);
            auto server_transport =
                std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
            const auto port = server_transport->port();
            auto transport = std::make_shared<mcp::HttpClientTransport>(
                io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
            mcp::Client client(transport, io.get_executor(), options);
            boost::asio::co_spawn(io, server_transport->listen(), boost::asio::detached);
            boost::asio::co_spawn(io, serve(server.get(), server_transport, io.get_executor()),
                                  boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await connect_and_ask(client, outcome);
                client.close();
                server_transport->close();
            });
            break;
        }
        case TransportKind::eWebSocket: {
            auto server = make_server(server_kind);
            boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
            const auto port = acceptor.local_endpoint().port();
            auto transport = std::make_shared<mcp::WebSocketClientTransport>(
                io.get_executor(), "127.0.0.1", std::to_string(port));
            mcp::Client client(transport, io.get_executor(), options);
            boost::asio::co_spawn(io, accept_websocket(&acceptor, server.get(), io.get_executor()),
                                  boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await connect_and_ask(client, outcome);
                client.close();
            });
            break;
        }
    }
    return *outcome;
}

using CellParam = std::tuple<ClientKind, ServerKind, TransportKind>;

class E2eMatrix : public ::testing::TestWithParam<CellParam> {};

TEST_P(E2eMatrix, Cell) {
    const auto [client_kind, server_kind, transport_kind] = GetParam();
    const auto expected = expected_for(client_kind, server_kind);
    const auto outcome = run_cell(client_kind, server_kind, transport_kind);

    if (!expected.connects) {
        // The compatibility matrix prescribes a failure that tells the user what the server serves.
        ASSERT_NE(outcome.error, "") << "connected in " << outcome.connected.protocolVersion;
        EXPECT_NE(outcome.error.find(expected.named_in_failure), std::string::npos) << outcome.error;
        return;
    }
    ASSERT_EQ(outcome.error, "");
    EXPECT_EQ(outcome.connected.era, expected.era);
    EXPECT_EQ(outcome.connected.protocolVersion, expected.version);
    ASSERT_TRUE(outcome.connected.serverInfo.has_value());
    EXPECT_EQ(outcome.connected.serverInfo->name, "matrix-server");
    const bool modern = expected.era == mcp::ProtocolEra::eModern;
    EXPECT_EQ(outcome.whoami.value("modern", !modern), modern) << outcome.whoami;
    if (modern) {
        EXPECT_EQ(outcome.whoami.value("version", ""), "2026-07-28") << outcome.whoami;
    }
}

std::string cell_name(const ::testing::TestParamInfo<CellParam>& info) {
    const auto [client_kind, server_kind, transport_kind] = info.param;
    const auto prefix = expected_for(client_kind, server_kind).connects ? "" : "Negative_";
    return prefix + client_name(client_kind) + "_" + server_name(server_kind) + "_" +
           transport_name(transport_kind);
}

INSTANTIATE_TEST_SUITE_P(Eras, E2eMatrix,
                         ::testing::Combine(::testing::ValuesIn(g_clients),
                                            ::testing::ValuesIn(g_servers),
                                            ::testing::ValuesIn(g_transports)),
                         cell_name);

// ---------------------------------------------------------------------------
// 2026-07-28 features on every transport
// ---------------------------------------------------------------------------

enum class FeatureTransport {
    eMemory,
    eStdio,
    eSessionManager,
    eSessionManagerJsonOnly,
    eStatelessJson,
    eRunHttp,
    eRunHttpJsonOnly,
    eWebSocket
};

const std::vector<FeatureTransport> g_feature_transports = {
    FeatureTransport::eMemory,          FeatureTransport::eStdio,
    FeatureTransport::eSessionManager,  FeatureTransport::eSessionManagerJsonOnly,
    FeatureTransport::eStatelessJson,   FeatureTransport::eRunHttp,
    FeatureTransport::eRunHttpJsonOnly, FeatureTransport::eWebSocket};

constexpr unsigned short g_first_feature_port = 20650;

std::string feature_transport_name(FeatureTransport kind) {
    switch (kind) {
        case FeatureTransport::eMemory:
            return "Memory";
        case FeatureTransport::eStdio:
            return "Stdio";
        case FeatureTransport::eSessionManager:
            return "SessionManager";
        case FeatureTransport::eSessionManagerJsonOnly:
            return "SessionManagerJsonOnly";
        case FeatureTransport::eStatelessJson:
            return "StatelessJson";
        case FeatureTransport::eRunHttp:
            return "RunHttp";
        case FeatureTransport::eRunHttpJsonOnly:
            return "RunHttpJsonOnly";
        case FeatureTransport::eWebSocket:
            return "WebSocket";
    }
    return "Unknown";
}

/// What a 2026-07-28 session saw: the answer of a tool that asked the client for input, and the
/// first notification of a subscriptions/listen stream or why the stream was refused.
struct FeatureOutcome {
    std::string error;
    json asked;
    std::string notified;
    int listen_code{0};
};

mcp::Task<void> feature_scenario(mcp::Client& client, std::shared_ptr<mcp::Server> notifier,
                                 std::shared_ptr<FeatureOutcome> outcome) {
    try {
        const auto connected = co_await client.connect("matrix-client", "1.0");
        if (connected.era != mcp::ProtocolEra::eModern) {
            outcome->error = "connected in " + connected.protocolVersion;
            co_return;
        }
        const auto result = co_await client.call_tool("ask", json::object());
        outcome->asked = result.structuredContent.value_or(json());
    } catch (const std::exception& error) {
        outcome->error = error.what();
        co_return;
    }
    try {
        mcp::SubscriptionFilter filter;
        filter.toolsListChanged = true;
        auto subscription = co_await client.listen(filter);
        co_await notifier->notify_tools_list_changed();
        const auto event = co_await subscription.next();
        outcome->notified = event ? event->method : "the stream ended";
        co_await subscription.close();
    } catch (const mcp::McpError& error) {
        outcome->listen_code = error.code();
    }
}

void answer_elicitations(mcp::Client& client) {
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        mcp::ElicitResult result;
        result.action = mcp::ElicitAction::eAccept;
        result.content = json{{"name", "matrix"}};
        co_return result;
    });
}

FeatureOutcome run_feature_cell(ClientKind client_kind, ServerKind server_kind,
                                FeatureTransport transport_kind) {
    boost::asio::io_context io;
    auto outcome = std::make_shared<FeatureOutcome>();
    mcp::ClientOptions options;
    options.protocol = protocol_of(client_kind);
    std::shared_ptr<mcp::Server> server = make_server(server_kind);

    switch (transport_kind) {
        case FeatureTransport::eMemory: {
            auto [client_transport, server_transport] =
                mcp::create_memory_transport_pair(io.get_executor());
            mcp::Client client(client_transport, io.get_executor(), options);
            answer_elicitations(client);
            boost::asio::co_spawn(io, serve(server.get(), server_transport, io.get_executor()),
                                  boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await feature_scenario(client, server, outcome);
                client.close();
                server_transport->close();
            });
            break;
        }
        case FeatureTransport::eStdio: {
            PipeBuffer to_server;
            PipeBuffer to_client;
            PipeWriter server_out(to_client);
            PipeWriter client_out(to_server);
            std::istream server_in(&to_server);
            std::ostream server_output(&server_out);
            std::istream client_in(&to_client);
            std::ostream client_output(&client_out);
            std::thread server_thread([&] { server->run_stdio(server_in, server_output); });
            auto transport =
                std::make_shared<mcp::StdioTransport>(io.get_executor(), client_in, client_output);
            mcp::Client client(transport, io.get_executor(), options);
            answer_elicitations(client);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await feature_scenario(client, server, outcome);
                client.close();
            });
            // The client outlives both closes, because its transport waits for its reader.
            to_server.close();
            server_thread.join();
            to_client.close();
            break;
        }
        case FeatureTransport::eSessionManager:
        case FeatureTransport::eSessionManagerJsonOnly:
        case FeatureTransport::eStatelessJson: {
            const auto variant = static_cast<unsigned short>(transport_kind) -
                                 static_cast<unsigned short>(FeatureTransport::eSessionManager);
            const auto port =
                static_cast<unsigned short>(g_first_feature_port +
                                            (static_cast<unsigned short>(client_kind) * 3 +
                                             static_cast<unsigned short>(server_kind)) *
                                                3 +
                                            variant);
            mcp::StreamableHttpSessionManager manager(
                io.get_executor(), "127.0.0.1", port,
                [server_kind](const boost::asio::any_io_executor&) {
                    return make_server(server_kind);
                });
            manager.set_protocol_policy(policy_of(server_kind));
            manager.set_json_only(transport_kind == FeatureTransport::eSessionManagerJsonOnly);
            manager.set_stateless_json_mode(transport_kind == FeatureTransport::eStatelessJson);
            manager.set_modern_server(server);
            auto transport = std::make_shared<mcp::HttpClientTransport>(
                io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
            mcp::Client client(transport, io.get_executor(), options);
            answer_elicitations(client);
            boost::asio::co_spawn(io, manager.listen(), boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await feature_scenario(client, server, outcome);
                client.close();
                manager.close();
            });
            break;
        }
        case FeatureTransport::eRunHttp:
        case FeatureTransport::eRunHttpJsonOnly: {
            auto server_transport =
                std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
            server_transport->set_json_only(transport_kind == FeatureTransport::eRunHttpJsonOnly);
            const auto port = server_transport->port();
            auto transport = std::make_shared<mcp::HttpClientTransport>(
                io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
            mcp::Client client(transport, io.get_executor(), options);
            answer_elicitations(client);
            boost::asio::co_spawn(io, server_transport->listen(), boost::asio::detached);
            boost::asio::co_spawn(io, serve(server.get(), server_transport, io.get_executor()),
                                  boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await feature_scenario(client, server, outcome);
                client.close();
                server_transport->close();
            });
            break;
        }
        case FeatureTransport::eWebSocket: {
            boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
            const auto port = acceptor.local_endpoint().port();
            auto transport = std::make_shared<mcp::WebSocketClientTransport>(
                io.get_executor(), "127.0.0.1", std::to_string(port));
            mcp::Client client(transport, io.get_executor(), options);
            answer_elicitations(client);
            boost::asio::co_spawn(io, accept_websocket(&acceptor, server.get(), io.get_executor()),
                                  boost::asio::detached);
            run_guarded(io, [&]() -> mcp::Task<void> {
                co_await feature_scenario(client, server, outcome);
                client.close();
            });
            break;
        }
    }
    return *outcome;
}

using FeatureParam = std::tuple<ClientKind, ServerKind, FeatureTransport>;

class E2eModernFeatures : public ::testing::TestWithParam<FeatureParam> {};

/// The error a subscriptions/listen gets on @p kind, 0 where the stream works. It needs a stream the
/// server can keep writing to: the session manager refuses it without an event stream, and
/// HttpServerTransport does not serve it.
int listen_refusal(FeatureTransport kind) {
    switch (kind) {
        case FeatureTransport::eSessionManagerJsonOnly:
        case FeatureTransport::eStatelessJson:
            return mcp::g_INVALID_REQUEST;
        case FeatureTransport::eRunHttp:
        case FeatureTransport::eRunHttpJsonOnly:
            return mcp::g_METHOD_NOT_FOUND;
        default:
            return 0;
    }
}

// Every transport carries a multi round-trip request: the tool's elicitation comes back to the
// client as an input_required result, the client answers it and retries, and the tool sees the
// answer. Where a transport can carry subscriptions/listen, the stream delivers a notification.
TEST_P(E2eModernFeatures, Cell) {
    const auto [client_kind, server_kind, transport_kind] = GetParam();
    const auto outcome = run_feature_cell(client_kind, server_kind, transport_kind);

    ASSERT_EQ(outcome.error, "");
    EXPECT_EQ(outcome.asked, (json{{"name", "matrix"}}));
    const auto refusal = listen_refusal(transport_kind);
    EXPECT_EQ(outcome.listen_code, refusal);
    EXPECT_EQ(outcome.notified, refusal == 0 ? "notifications/tools/list_changed" : "");
}

std::string feature_cell_name(const ::testing::TestParamInfo<FeatureParam>& info) {
    const auto [client_kind, server_kind, transport_kind] = info.param;
    return client_name(client_kind) + "_" + server_name(server_kind) + "_" +
           feature_transport_name(transport_kind);
}

INSTANTIATE_TEST_SUITE_P(Eras, E2eModernFeatures,
                         ::testing::Combine(::testing::Values(ClientKind::eAuto, ClientKind::eModern),
                                            ::testing::Values(ServerKind::eDualEra,
                                                              ServerKind::eModernOnly),
                                            ::testing::ValuesIn(g_feature_transports)),
                         feature_cell_name);

}  // namespace
