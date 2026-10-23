/// @file modern_client.cpp
/// @brief Demonstrates a client and server speaking protocol revision 2026-07-28.
///
/// This example shows:
/// - Client::connect() with the default ClientProtocol::Auto(): the client probes with
///   server/discover and speaks 2026-07-28 with a server that serves it
/// - ClientOptions::on_era_decision reporting why the client chose that era
/// - ConnectResult: era, protocolVersion, serverInfo (optional), instructions, supportedVersions
/// - RequestOptions::log_level, the minimum log level for one request: a handler's log messages
///   reach the client only for a request that sets it
/// - Server::set_cache_hints(): the ttlMs and cacheScope hints on a list result
///
/// The specification lists logging as deprecated in 2026-07-28; it remains part of the revision,
/// and the log level of a request is how a 2026-07-28 client asks for it.

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
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace asio = boost::asio;

namespace {

const char* era_name(mcp::ProtocolEra era) {
    return era == mcp::ProtocolEra::eModern ? "modern" : "legacy";
}

const char* cache_scope_name(mcp::CacheScope scope) {
    return scope == mcp::CacheScope::ePublic ? "public" : "private";
}

const char* era_reason_name(mcp::EraReason reason) {
    switch (reason) {
        case mcp::EraReason::ePinned:
            return "pinned";
        case mcp::EraReason::eDiscoverOk:
            return "server/discover succeeded";
        case mcp::EraReason::eModernErrorRetry:
            return "server/discover returned a modern protocol error";
        case mcp::EraReason::eFallbackNonModernError:
            return "server/discover failed with another error";
        case mcp::EraReason::eFallbackTimeout:
            return "server/discover timed out";
        case mcp::EraReason::eReprobeAfterInitializeRejected:
            return "initialize was rejected and the server was probed again";
    }
    return "unknown";
}

}  // namespace

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        bool succeeded = false;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "modern-client-example-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);
        server.set_instructions("Call greet with a name.");

        // A list result of a 2026-07-28 request carries ttlMs and cacheScope. Without this call the
        // defaults apply: 0 (do not reuse the result) and private.
        server.set_cache_hints(CacheTarget::eToolsList, CacheHint{60000, CacheScope::ePublic});

        nlohmann::json tool_schema = {{"type", "object"},
                                      {"properties", {{"name", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"name"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "greet", "Greets someone and logs the call", tool_schema,
            [](Context& ctx, const nlohmann::json& args) -> Task<nlohmann::json> {
                // Sent only when the request that called the tool set RequestOptions::log_level.
                co_await ctx.log_info("greet was called");
                co_return nlohmann::json{
                    {"content", nlohmann::json::array({nlohmann::json{
                                    {"type", "text"},
                                    {"text", "Hello, " + args.at("name").get<std::string>() + "!"}}})}};
            });

        auto [client_transport, server_transport] = create_memory_transport_pair(io_ctx.get_executor());

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    co_await server.run(server_transport, io_ctx.get_executor());
                } catch (const std::exception& e) {
                    std::cerr << "[Server] Error: " << e.what() << '\n';
                }
            },
            asio::detached);

        // ========== CLIENT ==========
        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    // The default is ClientProtocol::Auto(); it is spelled out for the reader.
                    ClientOptions options;
                    options.protocol = ClientProtocol::Auto();
                    options.on_era_decision = [](const EraDecision& decision) {
                        std::cout << "[Client] Era decision: " << era_name(decision.era) << " ("
                                  << era_reason_name(decision.reason) << ")\n";
                    };
                    Client client(client_transport, io_ctx.get_executor(), options);

                    Implementation client_info;
                    client_info.name = "modern-client-example-client";
                    client_info.version = "1.0.0";

                    // ========== CONNECT ==========
                    {
                        ConnectResult connected =
                            co_await client.connect(client_info, ClientCapabilities{});
                        std::cout << "[Client] era: " << era_name(connected.era) << '\n';
                        std::cout << "[Client] protocolVersion: " << connected.protocolVersion << '\n';
                        // serverInfo is optional: a server may leave it out.
                        if (connected.serverInfo) {
                            std::cout << "[Client] server: " << connected.serverInfo->name << ' '
                                      << connected.serverInfo->version << '\n';
                        } else {
                            std::cout << "[Client] server: (no serverInfo)\n";
                        }
                        if (connected.instructions) {
                            std::cout << "[Client] instructions: " << *connected.instructions << '\n';
                        }
                        for (const auto& supported : connected.supportedVersions) {
                            std::cout << "[Client] server supports: " << supported << '\n';
                        }
                        if (connected.era != ProtocolEra::eModern ||
                            connected.protocolVersion != g_LATEST_PROTOCOL_VERSION) {
                            throw std::runtime_error("expected a 2026-07-28 session");
                        }
                    }

                    // ========== PER-REQUEST LOG LEVEL ==========
                    int log_messages = 0;
                    client.on_notification(
                        "notifications/message", [&log_messages](const nlohmann::json& params) {
                            ++log_messages;
                            std::cout << "[Client] Log message: "
                                      << params.value("data", nlohmann::json{}).dump() << '\n';
                        });

                    const std::optional<nlohmann::json> call_json =
                        nlohmann::json{{"name", "greet"}, {"arguments", {{"name", "Ada Lovelace"}}}};

                    // No log_level: the server sends no log messages for this request.
                    std::cout << "[Client] Calling greet without a log level\n";
                    nlohmann::json quiet_json = co_await client.send_request("tools/call", call_json);
                    if (log_messages != 0) {
                        throw std::runtime_error("a log message arrived without a log level");
                    }

                    // With log_level set, the handler's log_info() reaches this client.
                    RequestOptions request_options;
                    request_options.log_level = LoggingLevel::eInfo;
                    std::cout << "[Client] Calling greet with log_level info\n";
                    nlohmann::json loud_json =
                        co_await client.send_request("tools/call", call_json, request_options);
                    if (log_messages != 1) {
                        throw std::runtime_error("expected exactly one log message");
                    }
                    std::cout << "[Client] Result: "
                              << loud_json.at("content").at(0).at("text").get<std::string>() << '\n';
                    if (quiet_json.at("content").at(0).at("text") !=
                        loud_json.at("content").at(0).at("text")) {
                        throw std::runtime_error("the two calls returned different results");
                    }

                    // ========== CACHE HINTS ==========
                    ListToolsResult tools = co_await client.list_tools();
                    std::cout << "[Client] tools/list returned " << tools.tools.size() << " tool(s)\n";
                    if (tools.ttlMs && tools.cacheScope) {
                        std::cout << "[Client] ttlMs: " << *tools.ttlMs
                                  << ", cacheScope: " << cache_scope_name(*tools.cacheScope) << '\n';
                    }
                    if (tools.ttlMs != 60000 || tools.cacheScope != CacheScope::ePublic) {
                        throw std::runtime_error("tools/list did not carry the configured cache hints");
                    }

                    client.close();
                    succeeded = true;
                } catch (const std::exception& e) {
                    std::cerr << "[Client] Error: " << e.what() << '\n';
                }
                io_ctx.stop();
            },
            asio::detached);

        // Fail rather than hang if something never answers.
        asio::steady_timer watchdog(io_ctx.get_executor());
        watchdog.expires_after(std::chrono::seconds(10));
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
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
