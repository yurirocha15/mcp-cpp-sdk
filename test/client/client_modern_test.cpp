#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/memory.hpp"
#include "mcp/transport/stdio.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

/// Only a guard against a hang: no test asserts that anything finishes within it.
constexpr auto g_hang_guard = std::chrono::seconds(30);

constexpr std::string_view g_version_key = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view g_capabilities_key = "io.modelcontextprotocol/clientCapabilities";
constexpr std::string_view g_info_key = "io.modelcontextprotocol/clientInfo";
constexpr std::string_view g_level_key = "io.modelcontextprotocol/logLevel";

json discover_result(std::vector<std::string> versions = {"2026-07-28"}) {
    return {{"resultType", "complete"},
            {"supportedVersions", std::move(versions)},
            {"capabilities", {{"tools", json::object()}}},
            {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "peer"}, {"version", "9.9"}}}}},
            {"instructions", "use the tools"}};
}

json legacy_initialize_result(std::string version = "2025-11-25") {
    return {{"protocolVersion", std::move(version)},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "legacy-peer"}, {"version", "1.0"}}}};
}

json response_to(const json& request, json result) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}};
}

json error_to(const json& request, int code, std::string message, std::optional<json> data = {}) {
    json error = {{"code", code}, {"message", std::move(message)}};
    if (data) {
        error["data"] = std::move(*data);
    }
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"error", std::move(error)}};
}

/// The result a modern peer gives to the requests the client can send.
json modern_result_for(const std::string& method) {
    if (method == "server/discover") {
        return discover_result();
    }
    if (method == "tools/list") {
        return {{"resultType", "complete"}, {"tools", json::array()}};
    }
    if (method == "tools/call") {
        return {{"resultType", "complete"},
                {"content", json::array({{{"type", "text"}, {"text", "ok"}}})}};
    }
    if (method == "resources/list") {
        return {{"resultType", "complete"}, {"resources", json::array()}};
    }
    if (method == "resources/templates/list") {
        return {{"resultType", "complete"}, {"resourceTemplates", json::array()}};
    }
    if (method == "resources/read") {
        return {{"resultType", "complete"}, {"contents", json::array()}};
    }
    if (method == "prompts/list") {
        return {{"resultType", "complete"}, {"prompts", json::array()}};
    }
    if (method == "prompts/get") {
        return {{"resultType", "complete"}, {"messages", json::array()}};
    }
    if (method == "completion/complete") {
        return {{"resultType", "complete"}, {"completion", {{"values", json::array()}}}};
    }
    return json::object();
}

/// The server/discover and tools/list answers of a 2026-07-28 server, errors for anything else.
std::optional<json> modern_peer(const json& request) {
    const auto method = request.at("method").get<std::string>();
    if (method == "initialize") {
        return error_to(request, mcp::g_METHOD_NOT_FOUND, "Method not found: initialize",
                        json{{"supported", json::array({"2026-07-28"})}});
    }
    return response_to(request, modern_result_for(method));
}

/// A legacy server that has never heard of server/discover, as the reference servers answer it.
std::optional<json> legacy_peer_rejecting_discover(const json& request, int code) {
    const auto method = request.at("method").get<std::string>();
    if (method == "server/discover") {
        return error_to(request, code, "Method not found");
    }
    if (method == "initialize") {
        return response_to(request, legacy_initialize_result());
    }
    return response_to(request, json::object());
}

/// Plays the server on a ScriptedTransport and records everything the client writes.
class PeerScript {
   public:
    using Reply = std::function<std::optional<json>(const json&)>;

    explicit PeerScript(ScriptedTransport& transport) : transport_(transport) {
        transport_.set_on_write([this](std::string_view message) { on_write(json::parse(message)); });
    }

    /// Answers a request; returning nothing leaves it unanswered.
    Reply reply;
    /// Sees every message before it is answered.
    std::function<void(const json&)> observe;
    std::vector<json> seen;

    void deliver(const json& message) { transport_.enqueue_message(message.dump()); }
    void deliver_raw(std::string message) { transport_.enqueue_message(std::move(message)); }

    [[nodiscard]] std::vector<json> with_method(std::string_view method) const {
        std::vector<json> matching;
        for (const auto& message : seen) {
            if (message.value("method", "") == method) {
                matching.push_back(message);
            }
        }
        return matching;
    }

    [[nodiscard]] std::size_t count(std::string_view method) const {
        return with_method(method).size();
    }

   private:
    void on_write(const json& message) {
        seen.push_back(message);
        if (observe) {
            observe(message);
        }
        if (message.contains("id") && message.contains("method") && reply) {
            if (auto answer = reply(message)) {
                deliver(*answer);
            }
        }
    }

    ScriptedTransport& transport_;
};

/// One client on a scripted transport, with a hang guard around the run.
class Harness {
   public:
    explicit Harness(mcp::ClientOptions options = {})
        : transport(std::make_shared<ScriptedTransport>(io.get_executor())),
          peer(*transport),
          guard(io) {
        options.on_era_decision = [this](const mcp::EraDecision& decision) {
            decisions.push_back(decision);
            if (on_decision) {
                on_decision(decision);
            }
        };
        client = std::make_unique<mcp::Client>(transport, io.get_executor(), std::move(options));
    }

    void run(std::function<mcp::Task<void>()> body) {
        guard.expires_after(g_hang_guard);
        guard.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                guard_fired = true;
                io.stop();
            }
        });
        boost::asio::co_spawn(
            io,
            [this, body = std::move(body)]() -> mcp::Task<void> {
                try {
                    co_await body();
                } catch (const std::exception& error) {
                    ADD_FAILURE() << "unexpected exception: " << error.what();
                }
                client->close();
                guard.cancel();
            },
            boost::asio::detached);
        io.run();
        EXPECT_FALSE(guard_fired) << "the test did not finish";
    }

    [[nodiscard]] static json meta_of(const json& request) { return request.at("params").at("_meta"); }

    boost::asio::io_context io;
    std::shared_ptr<ScriptedTransport> transport;
    PeerScript peer;
    std::unique_ptr<mcp::Client> client;
    std::vector<mcp::EraDecision> decisions;
    std::function<void(const mcp::EraDecision&)> on_decision;
    boost::asio::steady_timer guard;
    bool guard_fired{false};
};

/// The error code ping() fails with, or 0 when it succeeds.
mcp::Task<int> ping_failure(mcp::Client& client) {
    try {
        co_await client.ping();
    } catch (const mcp::McpError& error) {
        co_return error.code();
    }
    co_return 0;
}

/// The error code a request fails with, or 0 when it succeeds.
mcp::Task<int> request_failure(mcp::Client& client, std::string_view method) {
    try {
        const std::optional<json> params = json{{"uri", "test://doc"}};
        auto pending = client.send_request(method, params);
        co_await std::move(pending);
    } catch (const mcp::McpError& error) {
        co_return error.code();
    }
    co_return 0;
}

mcp::ClientOptions auto_options() {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Auto();
    return options;
}

mcp::ClientOptions pinned_options(std::string version) {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Version(std::move(version));
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// Era detection (Auto)
// ---------------------------------------------------------------------------

// "A client that supports both ... SHOULD probe with server/discover before sending any other
// request ... The server returns a DiscoverResult: the server is modern." and basic_index.md:
// "Every request supplies this metadata in its _meta field."
TEST(ClientEra, ModernNoInitializeEveryRequestHasMeta) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
        co_await harness.client->list_tools();
        co_await harness.client->call_tool("echo", json::object());
        co_await harness.client->list_resources();
        co_await harness.client->list_resource_templates();
        co_await harness.client->read_resource("test://doc");
        co_await harness.client->list_prompts();
        co_await harness.client->get_prompt("greet");
        mcp::CompleteParams complete;
        complete.ref = mcp::PromptReference{};
        std::get<mcp::PromptReference>(complete.ref).name = "greet";
        complete.argument = mcp::CompleteParamsArgument{"name", "a"};
        co_await harness.client->complete(complete);
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(connected.protocolVersion, "2026-07-28");
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
    EXPECT_EQ(harness.peer.count("notifications/initialized"), 0u);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);

    std::size_t requests = 0;
    for (const auto& message : harness.peer.seen) {
        if (!message.contains("id")) {
            continue;
        }
        ++requests;
        const auto meta = Harness::meta_of(message);
        EXPECT_EQ(meta.at(std::string(g_version_key)), "2026-07-28") << message;
        EXPECT_TRUE(meta.at(std::string(g_capabilities_key)).is_object()) << message;
        EXPECT_EQ(meta.at(std::string(g_info_key)).at("name"), "test-client") << message;
        EXPECT_EQ(meta.at(std::string(g_info_key)).at("version"), "1.0") << message;
        EXPECT_FALSE(meta.contains(std::string(g_level_key))) << message;
    }
    EXPECT_EQ(requests, 9u);
}

// stdio Backward Compatibility: "The server returns any other error ... the server is legacy. Fall
// back to the initialize handshake." and "The fallback MUST NOT be keyed to one specific error
// code: legacy servers respond ... (commonly -32601 or -32602)".
TEST(ClientEra, AutoFallsBackOnMinus32601) {
    Harness harness(auto_options());
    harness.peer.reply = [](const json& request) {
        return legacy_peer_rejecting_discover(request, mcp::g_METHOD_NOT_FOUND);
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);
    EXPECT_EQ(harness.peer.count("initialize"), 1u);
    EXPECT_EQ(harness.peer.count("notifications/initialized"), 1u);
    EXPECT_EQ(harness.peer.seen[0].at("method"), "server/discover");
    EXPECT_EQ(harness.peer.seen[1].at("method"), "initialize");
}

TEST(ClientEra, AutoFallsBackOnMinus32602) {
    Harness harness(auto_options());
    harness.peer.reply = [](const json& request) {
        return legacy_peer_rejecting_discover(request, mcp::g_INVALID_PARAMS);
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(harness.peer.count("initialize"), 1u);
    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eFallbackNonModernError);
}

// The specification's list of recognized modern errors includes an unknown method, but a modern
// server MUST implement server/discover, so -32601 on the probe can only come from a legacy
// server, and the reference TypeScript servers answer exactly that. The client treats it as
// legacy.
TEST(ClientEra, ProbeTreatsMinus32601AsLegacy) {
    Harness harness(auto_options());
    harness.peer.reply = [](const json& request) {
        return legacy_peer_rejecting_discover(request, mcp::g_METHOD_NOT_FOUND);
    };

    harness.run([&]() -> mcp::Task<void> {
        const auto connected = co_await harness.client->connect("test-client", "1.0");
        EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
        EXPECT_EQ(connected.protocolVersion, "2025-11-25");
    });

    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eFallbackNonModernError);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);
}

// A reply that is not JSON-RPC at all is an answer too: the client does not wait out the probe
// timeout for it.
TEST(ClientEra, AutoFallsBackOnPlainTextError) {
    auto options = auto_options();
    options.probe_timeout = g_hang_guard;
    Harness harness(options);
    harness.peer.reply = [&harness](const json& request) -> std::optional<json> {
        if (request.at("method") == "server/discover") {
            harness.peer.deliver_raw("Error: unknown method server/discover");
            return std::nullopt;
        }
        return legacy_peer_rejecting_discover(request, mcp::g_METHOD_NOT_FOUND);
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eFallbackNonModernError);
    EXPECT_EQ(harness.peer.count("initialize"), 1u);
}

// Modern Client / Legacy Server on stdio: "the client then surfaces an actionable error".
TEST(ClientEra, AutoProbeClosedByPeerSurfacesActionableError) {
    Harness harness(auto_options());
    harness.peer.reply = [&harness](const json& request) -> std::optional<json> {
        if (request.at("method") == "server/discover") {
            harness.transport->close();
        }
        return std::nullopt;
    };

    int code = 0;
    std::string message;
    harness.run([&]() -> mcp::Task<void> {
        try {
            co_await harness.client->connect("test-client", "1.0");
        } catch (const mcp::McpError& error) {
            code = error.code();
            message = error.message();
        }
    });

    EXPECT_EQ(code, mcp::g_CONNECTION_CLOSED);
    EXPECT_NE(message.find("ClientProtocol::Legacy()"), std::string::npos) << message;
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
}

// "or does not respond within a reasonable timeout: the server is legacy."
TEST(ClientEra, SilentLegacyStdioServerFallsBackWithinProbeTimeout) {
    auto options = auto_options();
    options.probe_timeout = std::chrono::milliseconds(150);
    Harness harness(options);
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        if (request.at("method") == "server/discover") {
            return std::nullopt;
        }
        return response_to(request, legacy_initialize_result());
    };

    mcp::ConnectResult connected;
    std::chrono::milliseconds latency{0};
    harness.run([&]() -> mcp::Task<void> {
        const auto started = std::chrono::steady_clock::now();
        connected = co_await harness.client->connect("test-client", "1.0");
        latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_GE(latency, options.probe_timeout);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);
    EXPECT_EQ(harness.peer.count("initialize"), 1u);
    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eFallbackTimeout);
    EXPECT_GE(harness.decisions[0].elapsed, options.probe_timeout);
}

// A modern server that is still starting up answers nothing in time. It is not misclassified for
// good: its refusal of initialize names the modern versions, and the client probes once more.
TEST(ClientEra, SlowStartingModernStdioServerNotMisclassified) {
    auto options = auto_options();
    options.probe_timeout = std::chrono::milliseconds(100);
    Harness harness(options);

    std::vector<json> withheld;
    bool started = false;
    const auto process = [&harness](const json& request) {
        if (auto answer = modern_peer(request)) {
            harness.peer.deliver(*answer);
        }
    };
    harness.peer.reply = [&](const json& request) -> std::optional<json> {
        if (!started) {
            withheld.push_back(request);
            return std::nullopt;
        }
        return modern_peer(request);
    };
    harness.on_decision = [&](const mcp::EraDecision& decision) {
        if (decision.reason == mcp::EraReason::eFallbackTimeout) {
            started = true;
            for (const auto& request : withheld) {
                process(request);
            }
            withheld.clear();
        }
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(harness.peer.count("server/discover"), 2u);
    EXPECT_EQ(harness.peer.count("initialize"), 1u);
    ASSERT_EQ(harness.decisions.size(), 2u);
    EXPECT_EQ(harness.decisions[0].era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eFallbackTimeout);
    EXPECT_EQ(harness.decisions[1].era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(harness.decisions[1].reason, mcp::EraReason::eReprobeAfterInitializeRejected);
}

// The re-probe happens once: a server that refuses initialize naming modern versions and still
// does not answer the second probe fails the connect with the refusal.
TEST(ClientEra, ReprobeHappensOnce) {
    auto options = auto_options();
    options.probe_timeout = std::chrono::milliseconds(100);
    Harness harness(options);
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        if (request.at("method") == "initialize") {
            return modern_peer(request);
        }
        return std::nullopt;
    };

    int code = 0;
    harness.run([&]() -> mcp::Task<void> {
        try {
            co_await harness.client->connect("test-client", "1.0");
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });

    EXPECT_EQ(code, mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(harness.peer.count("server/discover"), 2u);
    EXPECT_EQ(harness.peer.count("initialize"), 1u);
}

// A reply to the probe that arrives after the client gave up on it is dropped: it neither changes
// the era nor disturbs later requests.
TEST(ClientEra, LateProbeReplyIgnoredAfterFallback) {
    auto options = auto_options();
    options.probe_timeout = std::chrono::milliseconds(100);
    Harness harness(options);
    std::optional<json> probe;
    harness.peer.reply = [&](const json& request) -> std::optional<json> {
        const auto method = request.at("method").get<std::string>();
        if (method == "server/discover") {
            probe = request;
            return std::nullopt;
        }
        if (method == "initialize") {
            harness.peer.deliver(response_to(request, legacy_initialize_result()));
            // The probe's answer, long after the client moved on.
            harness.peer.deliver(response_to(*probe, discover_result()));
            return std::nullopt;
        }
        return response_to(request, json{{"tools", json::array()}});
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
        co_await harness.client->list_tools();
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    const auto listings = harness.peer.with_method("tools/list");
    ASSERT_EQ(listings.size(), 1u);
    EXPECT_FALSE(listings[0].contains("params"));
    EXPECT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);
}

// "If the server does not implement the requested version ... UnsupportedProtocolVersionError
// listing the versions it does support ... The client SHOULD select a mutually supported version
// from the supported list and retry the request."
TEST(ClientEra, Minus32022RetriesWithMutualVersion) {
    Harness harness(auto_options());
    int probes = 0;
    harness.peer.reply = [&](const json& request) -> std::optional<json> {
        if (request.at("method") == "server/discover" && probes++ == 0) {
            return error_to(request, mcp::g_UNSUPPORTED_PROTOCOL_VERSION,
                            "Unsupported protocol version",
                            json{{"supported", json::array({"2025-11-25", "2026-07-28"})},
                                 {"requested", "2026-07-28"}});
        }
        return modern_peer(request);
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(harness.peer.count("server/discover"), 2u);
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eModernErrorRetry);
}

// "Do not fall back to initialize" for a modern server, unless only a legacy version is mutual.
TEST(ClientEra, Minus32022LegacyOnlyFallsBackToInitialize) {
    Harness harness(auto_options());
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        const auto method = request.at("method").get<std::string>();
        if (method == "server/discover") {
            return error_to(request, mcp::g_UNSUPPORTED_PROTOCOL_VERSION,
                            "Unsupported protocol version",
                            json{{"supported", json::array({"2025-03-26", "2025-06-18"})},
                                 {"requested", "2026-07-28"}});
        }
        if (method == "initialize") {
            return response_to(request, legacy_initialize_result("2025-06-18"));
        }
        return response_to(request, json::object());
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(connected.protocolVersion, "2025-06-18");
    EXPECT_EQ(connected.supportedVersions, (std::vector<std::string>{"2025-03-26", "2025-06-18"}));
    const auto initializes = harness.peer.with_method("initialize");
    ASSERT_EQ(initializes.size(), 1u);
    EXPECT_EQ(initializes[0].at("params").at("protocolVersion"), "2025-06-18");
    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eModernErrorRetry);
}

// A modern error that offers no version to retry with identifies a modern server all the same: the
// connect fails instead of sending initialize.
TEST(ClientEra, OtherModernErrorsDoNotFallBack) {
    for (const int code : {mcp::g_HEADER_MISMATCH, mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY}) {
        Harness harness(auto_options());
        harness.peer.reply = [code](const json& request) -> std::optional<json> {
            return error_to(request, code, "modern refusal");
        };

        int seen_code = 0;
        harness.run([&]() -> mcp::Task<void> {
            try {
                co_await harness.client->connect("test-client", "1.0");
            } catch (const mcp::McpError& error) {
                seen_code = error.code();
            }
        });

        EXPECT_EQ(seen_code, code);
        EXPECT_EQ(harness.peer.count("initialize"), 0u);
        EXPECT_TRUE(harness.decisions.empty());
    }
}

TEST(ClientEra, NoMutualVersionFailsTheConnect) {
    Harness harness(auto_options());
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        return error_to(request, mcp::g_UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version",
                        json{{"supported", json::array({"2027-01-01"})}, {"requested", "2026-07-28"}});
    };

    int code = 0;
    harness.run([&]() -> mcp::Task<void> {
        try {
            co_await harness.client->connect("test-client", "1.0");
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });

    EXPECT_EQ(code, mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
}

// A pinned client cannot retry with another version, so its error names the versions the server
// lists and what reaches them.
TEST(ClientEra, PinnedModernRefusalNamesTheServedVersions) {
    Harness harness(pinned_options("2026-07-28"));
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        return error_to(request, mcp::g_UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version",
                        json{{"supported", json::array({"2025-11-25", "2025-06-18"})},
                             {"requested", "2026-07-28"}});
    };

    std::optional<mcp::McpError> failure;
    harness.run([&]() -> mcp::Task<void> {
        try {
            co_await harness.client->connect("test-client", "1.0");
        } catch (const mcp::McpError& error) {
            failure = error;
        }
    });

    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    const std::string message = failure->what();
    EXPECT_NE(message.find("2026-07-28"), std::string::npos) << message;
    EXPECT_NE(message.find("2025-11-25, 2025-06-18"), std::string::npos) << message;
    EXPECT_NE(message.find("ClientProtocol::Auto()"), std::string::npos) << message;
    ASSERT_TRUE(failure->data().has_value());
    EXPECT_EQ(failure->data()->value("requested", ""), "2026-07-28");
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
}

// basic_index.md: "A resultType of any value unrecognized by the client MUST be considered invalid."
TEST(ClientEra, UnknownResultTypeRejected) {
    for (const json& bad : {json("surprise"), json("input_required"), json(7), json(nullptr)}) {
        Harness harness(auto_options());
        harness.peer.reply = [&bad](const json& request) -> std::optional<json> {
            auto answer = modern_peer(request);
            if (request.at("method") == "tools/list") {
                (*answer)["result"]["resultType"] = bad;
            }
            return answer;
        };

        int code = 0;
        harness.run([&]() -> mcp::Task<void> {
            co_await harness.client->connect("test-client", "1.0");
            try {
                co_await harness.client->list_tools();
            } catch (const mcp::McpError& error) {
                code = error.code();
            }
        });

        EXPECT_EQ(code, mcp::g_INVALID_REQUEST) << bad;
    }
}

// basic_index.md: "clients MUST treat an absent resultType as complete."
TEST(ClientEra, AbsentResultTypeAccepted) {
    Harness harness(auto_options());
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        auto answer = modern_peer(request);
        if (request.at("method") == "tools/list") {
            (*answer)["result"].erase("resultType");
        }
        return answer;
    };

    std::size_t tools = 99;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("test-client", "1.0");
        const auto listed = co_await harness.client->list_tools();
        tools = listed.tools.size();
    });

    EXPECT_EQ(tools, 0u);
}

// ping, logging/setLevel and resources/subscribe do not exist in 2026-07-28; the client refuses
// them without a round trip, and has no roots notification to send.
TEST(ClientEra, PingFailsLocallyInModern) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;

    std::vector<int> codes;
    harness.run([&]() -> mcp::Task<void> {
        const mcp::Implementation client_info{"test-client", "1.0"};
        mcp::ClientCapabilities capabilities;
        capabilities.roots = mcp::ClientCapabilities::RootsCapability{.listChanged = true};
        co_await harness.client->connect(client_info, capabilities);
        const int ping_code = co_await ping_failure(*harness.client);
        const int subscribe_code = co_await request_failure(*harness.client, "resources/subscribe");
        const int unsubscribe_code = co_await request_failure(*harness.client, "resources/unsubscribe");
        const int level_code = co_await request_failure(*harness.client, "logging/setLevel");
        codes = {ping_code, subscribe_code, unsubscribe_code, level_code};
        harness.client->set_roots({mcp::Root{"file:///work", "work"}}, true);
        co_await harness.client->list_tools();
    });

    EXPECT_EQ(codes, (std::vector<int>(4, mcp::g_METHOD_NOT_FOUND)));
    EXPECT_EQ(harness.peer.count("ping"), 0u);
    EXPECT_EQ(harness.peer.count("resources/subscribe"), 0u);
    EXPECT_EQ(harness.peer.count("resources/unsubscribe"), 0u);
    EXPECT_EQ(harness.peer.count("logging/setLevel"), 0u);
    EXPECT_EQ(harness.peer.count("notifications/roots/list_changed"), 0u);
}

// The legacy session still sends the roots notification and the ping.
TEST(ClientEra, LegacySessionKeepsPingAndRootsNotification) {
    Harness harness(pinned_options("2025-11-25"));
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        return legacy_peer_rejecting_discover(request, mcp::g_METHOD_NOT_FOUND);
    };

    harness.run([&]() -> mcp::Task<void> {
        const mcp::Implementation client_info{"test-client", "1.0"};
        mcp::ClientCapabilities capabilities;
        capabilities.roots = mcp::ClientCapabilities::RootsCapability{.listChanged = true};
        co_await harness.client->connect(client_info, capabilities);
        co_await harness.client->ping();
        harness.client->set_roots({mcp::Root{"file:///work", "work"}}, true);
        co_await harness.client->ping();
    });

    EXPECT_EQ(harness.peer.count("ping"), 2u);
    EXPECT_EQ(harness.peer.count("notifications/roots/list_changed"), 1u);
}

TEST(ClientEra, ConnectResultFieldsModernAndLegacy) {
    {
        Harness harness(auto_options());
        harness.peer.reply = [](const json& request) -> std::optional<json> {
            if (request.at("method") == "server/discover") {
                return response_to(request, discover_result({"2025-11-25", "2026-07-28"}));
            }
            return modern_peer(request);
        };
        mcp::ConnectResult connected;
        harness.run([&]() -> mcp::Task<void> {
            connected = co_await harness.client->connect("test-client", "1.0");
        });
        EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
        EXPECT_EQ(connected.protocolVersion, "2026-07-28");
        EXPECT_TRUE(connected.capabilities.tools.has_value());
        ASSERT_TRUE(connected.serverInfo.has_value());
        EXPECT_EQ(connected.serverInfo->name, "peer");
        EXPECT_EQ(connected.serverInfo->version, "9.9");
        EXPECT_EQ(connected.instructions, "use the tools");
        EXPECT_EQ(connected.supportedVersions, (std::vector<std::string>{"2025-11-25", "2026-07-28"}));
    }
    {
        Harness harness(pinned_options("2025-06-18"));
        harness.peer.reply = [](const json& request) -> std::optional<json> {
            auto result = legacy_initialize_result("2025-06-18");
            result["instructions"] = "legacy instructions";
            return response_to(request, std::move(result));
        };
        mcp::ConnectResult connected;
        harness.run([&]() -> mcp::Task<void> {
            connected = co_await harness.client->connect("test-client", "1.0");
        });
        EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
        EXPECT_EQ(connected.protocolVersion, "2025-06-18");
        EXPECT_TRUE(connected.capabilities.tools.has_value());
        ASSERT_TRUE(connected.serverInfo.has_value());
        EXPECT_EQ(connected.serverInfo->name, "legacy-peer");
        EXPECT_EQ(connected.instructions, "legacy instructions");
        EXPECT_TRUE(connected.supportedVersions.empty());
    }
}

// "Clients SHOULD cache the result for the lifetime of the server process": the era is decided
// once; no request after connect() probes again, and a second connect() is refused.
TEST(ClientEra, EraCachedForLifetime) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;

    bool second_connect_refused = false;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("test-client", "1.0");
        co_await harness.client->list_tools();
        co_await harness.client->list_tools();
        try {
            co_await harness.client->connect("test-client", "1.0");
        } catch (const mcp::McpError&) {
            second_connect_refused = true;
        }
        co_await harness.client->list_prompts();
    });

    EXPECT_TRUE(second_connect_refused);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);
    EXPECT_EQ(harness.decisions.size(), 1u);
}

TEST(ClientEra, EraDecisionCallbackReportsReason) {
    {
        Harness harness(auto_options());
        harness.peer.reply = modern_peer;
        harness.run([&]() -> mcp::Task<void> { co_await harness.client->connect("c", "1"); });
        ASSERT_EQ(harness.decisions.size(), 1u);
        EXPECT_EQ(harness.decisions[0].era, mcp::ProtocolEra::eModern);
        EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::eDiscoverOk);
    }
    {
        Harness harness;  // the default is the newest legacy version, no probe
        harness.peer.reply = [](const json& request) {
            return legacy_peer_rejecting_discover(request, mcp::g_METHOD_NOT_FOUND);
        };
        harness.run([&]() -> mcp::Task<void> { co_await harness.client->connect("c", "1"); });
        ASSERT_EQ(harness.decisions.size(), 1u);
        EXPECT_EQ(harness.decisions[0].era, mcp::ProtocolEra::eLegacy);
        EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::ePinned);
        EXPECT_EQ(harness.peer.count("server/discover"), 0u);
    }
    {
        Harness harness(pinned_options("2026-07-28"));
        harness.peer.reply = modern_peer;
        harness.run([&]() -> mcp::Task<void> { co_await harness.client->connect("c", "1"); });
        ASSERT_EQ(harness.decisions.size(), 1u);
        EXPECT_EQ(harness.decisions[0].era, mcp::ProtocolEra::eModern);
        EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::ePinned);
    }
}

// Client::cancel() writes notifications/cancelled naming the request and the reason, in either
// era; a modern request's cancellation is the same notification on a stream transport.
TEST(ClientEra, CancelWritesTheCancelledNotificationInBothEras) {
    for (const std::string pin : {"2025-11-25", "2026-07-28"}) {
        SCOPED_TRACE(pin);
        Harness harness(pinned_options(pin));
        const bool modern = pin == "2026-07-28";
        harness.peer.reply = [modern](const json& request) -> std::optional<json> {
            if (request.at("method") == "tools/call") {
                return std::nullopt;
            }
            return modern ? modern_peer(request) : legacy_peer_rejecting_discover(request, -32601);
        };

        harness.run([&]() -> mcp::Task<void> {
            co_await harness.client->connect("test-client", "1.0");
            boost::asio::co_spawn(
                harness.io,
                [&]() -> mcp::Task<void> {
                    try {
                        const std::optional<json> params = json{{"name", "slow"}};
                        auto pending = harness.client->send_request("tools/call", params);
                        co_await std::move(pending);
                    } catch (const std::exception&) {
                    }
                },
                boost::asio::detached);
            boost::asio::steady_timer tick(harness.io);
            while (harness.peer.count("tools/call") == 0) {
                tick.expires_after(std::chrono::milliseconds(1));
                co_await tick.async_wait(boost::asio::use_awaitable);
            }
            const auto id = harness.peer.with_method("tools/call").front().at("id");
            const auto reason = std::make_shared<const std::optional<std::string>>("user stopped it");
            co_await harness.client->cancel(id.get<mcp::RequestId>(), *reason);
        });

        const auto cancels = harness.peer.with_method("notifications/cancelled");
        ASSERT_EQ(cancels.size(), 1u);
        EXPECT_FALSE(cancels[0].contains("id"));
        const auto params = cancels[0].value("params", json::object());
        EXPECT_EQ(params.value("requestId", json()),
                  harness.peer.with_method("tools/call").front().at("id"));
        EXPECT_EQ(params.value("reason", ""), "user stopped it");
    }
}

// After Client::cancel() a modern request stops waiting at once, since the server sends nothing more
// for it, and a response that arrives anyway is dropped without a protocol error. A legacy request
// still gets its response.
TEST(ClientEra, CancelStopsWaitingForAModernRequestOnly) {
    for (const std::string pin : {"2026-07-28", "2025-11-25"}) {
        SCOPED_TRACE(pin);
        const bool modern = pin == "2026-07-28";
        mcp::ClientOptions options = pinned_options(pin);
        std::vector<mcp::Error> reported;
        options.on_protocol_error = [&reported](const mcp::Error& error) { reported.push_back(error); };
        Harness harness(options);
        std::optional<json> held;
        harness.peer.reply = [&held, modern](const json& request) -> std::optional<json> {
            if (request.at("method") == "tools/call") {
                held = request;
                return std::nullopt;
            }
            return modern ? modern_peer(request) : legacy_peer_rejecting_discover(request, -32601);
        };
        harness.peer.observe = [&](const json& message) {
            if (message.value("method", "") == "notifications/cancelled" && held) {
                // The server answers anyway, as a cancellation may cross the response.
                harness.peer.deliver(response_to(*held, modern_result_for("tools/call")));
            }
        };

        int code = -1;
        harness.run([&]() -> mcp::Task<void> {
            co_await harness.client->connect("test-client", "1.0");
            int finished = 0;
            boost::asio::co_spawn(
                harness.io,
                [&]() -> mcp::Task<void> {
                    try {
                        const std::optional<json> params = json{{"name", "slow"}};
                        auto pending = harness.client->send_request("tools/call", params);
                        co_await std::move(pending);
                        code = 0;
                    } catch (const mcp::McpError& error) {
                        code = error.code();
                    }
                    ++finished;
                },
                boost::asio::detached);
            boost::asio::steady_timer tick(harness.io);
            while (!held) {
                tick.expires_after(std::chrono::milliseconds(1));
                co_await tick.async_wait(boost::asio::use_awaitable);
            }
            co_await harness.client->cancel(held->at("id").get<mcp::RequestId>());
            while (finished == 0) {
                tick.expires_after(std::chrono::milliseconds(1));
                co_await tick.async_wait(boost::asio::use_awaitable);
            }
            // Let the late response reach the client.
            tick.expires_after(std::chrono::milliseconds(20));
            co_await tick.async_wait(boost::asio::use_awaitable);
        });

        EXPECT_EQ(code, modern ? mcp::g_REQUEST_CANCELLED : 0);
        EXPECT_TRUE(reported.empty());
    }
}

// A modern request that times out is cancelled with notifications/cancelled, so the server stops
// working on it. A legacy request and the era probe are left as they were.
TEST(ClientEra, TimeoutCancelsAModernRequestOnly) {
    struct Case {
        std::string name;
        mcp::ClientProtocol protocol;
        bool silent_discover;
        std::size_t cancels;
    };
    const std::vector<Case> cases = {
        {"modern", mcp::ClientProtocol::Version("2026-07-28"), false, 1},
        {"legacy", mcp::ClientProtocol::Version("2025-11-25"), false, 0},
        {"auto, silent probe", mcp::ClientProtocol::Auto(), true, 0},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        mcp::ClientOptions options;
        options.protocol = item.protocol;
        options.request_timeout = std::chrono::milliseconds(100);
        options.probe_timeout = std::chrono::milliseconds(100);
        Harness harness(options);
        const bool modern = item.name == "modern";
        const bool silent_discover = item.silent_discover;
        harness.peer.reply = [modern, silent_discover](const json& request) -> std::optional<json> {
            const auto method = request.at("method").get<std::string>();
            if (method == "tools/call" || (silent_discover && method == "server/discover")) {
                return std::nullopt;
            }
            return modern ? modern_peer(request) : legacy_peer_rejecting_discover(request, -32601);
        };

        int code = 0;
        harness.run([&]() -> mcp::Task<void> {
            co_await harness.client->connect("test-client", "1.0");
            try {
                const std::optional<json> params = json{{"name", "slow"}};
                auto pending = harness.client->send_request("tools/call", params);
                co_await std::move(pending);
            } catch (const mcp::McpError& error) {
                code = error.code();
            }
            // The notification is written after the request fails; let it reach the peer.
            boost::asio::steady_timer settle(harness.io);
            settle.expires_after(std::chrono::milliseconds(20));
            co_await settle.async_wait(boost::asio::use_awaitable);
        });

        EXPECT_EQ(code, mcp::g_REQUEST_TIMEOUT);
        const auto cancels = harness.peer.with_method("notifications/cancelled");
        ASSERT_EQ(cancels.size(), item.cancels);
        if (item.cancels == 1) {
            const auto params = cancels[0].value("params", json::object());
            EXPECT_EQ(params.value("requestId", json()),
                      harness.peer.with_method("tools/call").front().at("id"));
            EXPECT_EQ(params.value("reason", ""), "Request timed out");
        }
    }
}

// A hook that throws changes nothing about the connect.
TEST(ClientEra, ThrowingDecisionCallbackIsDiscarded) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;
    harness.on_decision = [](const mcp::EraDecision&) { throw std::runtime_error("hook failed"); };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
    });
    EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
}

// ---------------------------------------------------------------------------
// Per-request metadata
// ---------------------------------------------------------------------------

TEST(ClientEra, PerRequestLogLevelInMeta) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;

    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("test-client", "1.0");
        mcp::RequestOptions options;
        options.log_level = mcp::LoggingLevel::eWarning;
        auto pending = harness.client->send_request("tools/list", std::nullopt, options);
        co_await std::move(pending);
        co_await harness.client->list_tools();
    });

    const auto listings = harness.peer.with_method("tools/list");
    ASSERT_EQ(listings.size(), 2u);
    EXPECT_EQ(Harness::meta_of(listings[0]).at(std::string(g_level_key)), "warning");
    EXPECT_FALSE(Harness::meta_of(listings[1]).contains(std::string(g_level_key)));
}

// basic_index.md: `traceparent`, `tracestate` and `baggage` live in `_meta` too; the client adds
// its own keys and leaves the caller's alone.
TEST(ClientEra, CallerMetaTraceparentPreserved) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;

    const json caller_meta = {
        {"progressToken", "p-1"},
        {"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"},
        {"tracestate", "vendor=value"},
        {"baggage", "key=value"},
        {"example.com/custom", {{"nested", true}}}};
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("test-client", "1.0");
        const std::optional<json> params =
            json{{"name", "echo"}, {"arguments", json::object()}, {"_meta", caller_meta}};
        auto pending = harness.client->send_request("tools/call", params);
        co_await std::move(pending);
    });

    const auto calls = harness.peer.with_method("tools/call");
    ASSERT_EQ(calls.size(), 1u);
    const auto meta = Harness::meta_of(calls[0]);
    for (const auto& [key, value] : caller_meta.items()) {
        EXPECT_EQ(meta.at(key), value) << key;
    }
    EXPECT_EQ(meta.at(std::string(g_version_key)), "2026-07-28");
    EXPECT_TRUE(meta.contains(std::string(g_capabilities_key)));
    EXPECT_EQ(calls[0].at("params").at("name"), "echo");
}

TEST(ClientEra, CapabilitiesDerivedFromHandlers) {
    Harness harness(auto_options());
    harness.peer.reply = modern_peer;

    harness.client->on_roots_list(
        [](const json&) -> mcp::Task<mcp::ListRootsResult> { co_return mcp::ListRootsResult{}; });
    harness.client->on_request("sampling/createMessage",
                               [](const json&) -> mcp::Task<json> { co_return json::object(); });

    harness.run([&]() -> mcp::Task<void> {
        const mcp::Implementation client_info{"test-client", "1.0"};
        mcp::ClientCapabilities capabilities;
        capabilities.experimental = json{{"feature", true}};
        co_await harness.client->connect(client_info, capabilities);
        co_await harness.client->list_tools();
        harness.client->on_elicitation(
            [](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
                mcp::ElicitResult declined;
                declined.action = mcp::ElicitAction::eDecline;
                co_return declined;
            });
        co_await harness.client->list_prompts();
    });

    const auto discover = Harness::meta_of(harness.peer.with_method("server/discover")[0]);
    const auto listing = Harness::meta_of(harness.peer.with_method("tools/list")[0]);
    const auto later = Harness::meta_of(harness.peer.with_method("prompts/list")[0]);
    for (const auto& meta : {discover, listing}) {
        const auto& capabilities = meta.at(std::string(g_capabilities_key));
        EXPECT_TRUE(capabilities.contains("roots")) << capabilities;
        EXPECT_TRUE(capabilities.contains("sampling")) << capabilities;
        EXPECT_FALSE(capabilities.contains("elicitation")) << capabilities;
        EXPECT_EQ(capabilities.at("experimental"), (json{{"feature", true}}));
    }
    // A handler registered after connect() counts from the next request on.
    EXPECT_TRUE(later.at(std::string(g_capabilities_key)).contains("elicitation"));
}

// ---------------------------------------------------------------------------
// Pinned protocols
// ---------------------------------------------------------------------------

// Pinning a legacy version: no probe, initialize names the pin, later requests carry no `_meta`,
// and the version in effect is whatever the server answered.
void expect_pinned_legacy(const std::string& pin, const std::string& server_reply) {
    Harness harness(pinned_options(pin));
    harness.peer.reply = [&server_reply](const json& request) -> std::optional<json> {
        const auto method = request.at("method").get<std::string>();
        if (method == "initialize") {
            return response_to(request, legacy_initialize_result(server_reply));
        }
        return response_to(request, json{{"tools", json::array()}});
    };

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
        co_await harness.client->list_tools();
    });

    EXPECT_EQ(harness.peer.count("server/discover"), 0u);
    const auto initializes = harness.peer.with_method("initialize");
    ASSERT_EQ(initializes.size(), 1u);
    EXPECT_EQ(initializes[0].at("params").at("protocolVersion"), pin);
    EXPECT_FALSE(initializes[0].at("params").contains("_meta"));
    const auto listings = harness.peer.with_method("tools/list");
    ASSERT_EQ(listings.size(), 1u);
    EXPECT_FALSE(listings[0].contains("params"));
    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(connected.protocolVersion, server_reply);
    ASSERT_EQ(harness.decisions.size(), 1u);
    EXPECT_EQ(harness.decisions[0].reason, mcp::EraReason::ePinned);
}

TEST(ClientPinned, Version_2025_11_25) { expect_pinned_legacy("2025-11-25", "2025-11-25"); }
TEST(ClientPinned, Version_2025_06_18) { expect_pinned_legacy("2025-06-18", "2025-06-18"); }
TEST(ClientPinned, Version_2025_03_26) { expect_pinned_legacy("2025-03-26", "2025-03-26"); }
TEST(ClientPinned, Version_2024_11_05) { expect_pinned_legacy("2024-11-05", "2024-11-05"); }
// The version in effect is the server's, not the pin.
TEST(ClientPinned, ServerNegotiatesAnOlderVersion) { expect_pinned_legacy("2025-11-25", "2025-06-18"); }

TEST(ClientPinned, PinnedModernSkipsProbe) {
    Harness harness(pinned_options("2026-07-28"));
    harness.peer.reply = [](const json& request) -> std::optional<json> {
        if (request.at("method") == "server/discover") {
            return error_to(request, mcp::g_METHOD_NOT_FOUND, "Method not found");
        }
        return std::nullopt;
    };

    int code = 0;
    std::string message;
    harness.run([&]() -> mcp::Task<void> {
        try {
            co_await harness.client->connect("test-client", "1.0");
        } catch (const mcp::McpError& error) {
            code = error.code();
            message = error.message();
        }
    });

    // Never falls back: a legacy server fails the pinned client, with the way out in the message.
    EXPECT_EQ(code, mcp::g_METHOD_NOT_FOUND);
    EXPECT_NE(message.find("ClientProtocol::Auto()"), std::string::npos) << message;
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
    EXPECT_EQ(harness.peer.count("server/discover"), 1u);
    EXPECT_TRUE(harness.decisions.empty());
}

TEST(ClientPinned, PinnedModernConnectsAndNamesThePin) {
    Harness harness(pinned_options("2026-07-28"));
    harness.peer.reply = modern_peer;

    mcp::ConnectResult connected;
    harness.run([&]() -> mcp::Task<void> {
        connected = co_await harness.client->connect("test-client", "1.0");
        co_await harness.client->list_tools();
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(connected.protocolVersion, "2026-07-28");
    EXPECT_EQ(harness.peer.count("initialize"), 0u);
    for (const auto& request : harness.peer.seen) {
        EXPECT_EQ(Harness::meta_of(request).at(std::string(g_version_key)), "2026-07-28");
    }
}

TEST(ClientPinned, InvalidPinThrows) {
    EXPECT_THROW(mcp::ClientProtocol::Version("1999-01-01"), std::invalid_argument);
    EXPECT_THROW(mcp::ClientProtocol::Version(""), std::invalid_argument);
    EXPECT_THROW(mcp::ClientProtocol::Version("2026-07-29"), std::invalid_argument);
    for (const auto* version : {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}) {
        EXPECT_NO_THROW(mcp::ClientProtocol::Version(version));
    }
    EXPECT_EQ(mcp::ClientProtocol::Legacy().version(), "2025-11-25");
    EXPECT_EQ(mcp::ClientProtocol::Auto().kind(), mcp::ClientProtocol::Kind::eAuto);
    EXPECT_EQ(mcp::ClientOptions{}.protocol.kind(), mcp::ClientProtocol::Kind::eLegacy);
}

TEST(ClientPinned, ProbeTimeoutMustBePositive) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    mcp::ClientOptions options;
    options.probe_timeout = std::chrono::milliseconds(0);
    EXPECT_THROW(mcp::Client(transport, io.get_executor(), options), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// The SDK client against the SDK server
// ---------------------------------------------------------------------------

namespace {

/// A server with one tool that reports what it can see of the request's era.
std::unique_ptr<mcp::Server> make_dual_era_server() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server =
        std::make_unique<mcp::Server>(mcp::Implementation{"dual-era-server", "3.1"}, capabilities);
    server->add_tool<json, json>(
        "whoami", "Reports the request's per-request fields", json{{"type", "object"}},
        [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            const auto* meta = ctx.request_meta();
            if (meta == nullptr) {
                co_return json{{"modern", false}};
            }
            co_return json{{"modern", true},
                           {"version", meta->protocolVersion},
                           {"client", meta->clientInfo ? meta->clientInfo->name : ""},
                           {"log_level", meta->logLevel ? json(*meta->logLevel) : json(nullptr)}};
        });
    return server;
}

struct EraOutcome {
    mcp::ConnectResult connected;
    json whoami;
    std::vector<mcp::EraDecision> decisions;
    std::string error;
};

/// A blocking byte pipe: reads wait for data, EOF follows close().
class PipeBuffer : public std::streambuf {
   public:
    void feed(std::string_view data) {
        {
            std::lock_guard lock(mutex_);
            buffer_.append(data);
        }
        condition_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

   protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return position_ < buffer_.size() || closed_; });
        if (position_ == buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[position_]);
    }

    int_type uflow() override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return position_ < buffer_.size() || closed_; });
        if (position_ == buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[position_++]);
    }

   private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string buffer_;
    std::size_t position_{0};
    bool closed_{false};
};

/// Output that lands in a PipeBuffer.
class PipeWriter : public std::streambuf {
   public:
    explicit PipeWriter(PipeBuffer& target) : target_(target) {}

   protected:
    int_type overflow(int_type ch) override {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            const char byte = traits_type::to_char_type(ch);
            target_.feed(std::string_view(&byte, 1));
        }
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        target_.feed(std::string_view(data, static_cast<std::size_t>(count)));
        return count;
    }

   private:
    PipeBuffer& target_;
};

void run_guarded(boost::asio::io_context& io, boost::asio::steady_timer& guard, bool& fired,
                 std::function<mcp::Task<void>()> body) {
    guard.expires_after(g_hang_guard);
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
    EXPECT_FALSE(fired) << "the test did not finish";
}

mcp::Task<void> connect_and_ask(mcp::Client& client, std::shared_ptr<EraOutcome> outcome) {
    try {
        outcome->connected = co_await client.connect("era-client", "2.0");
        const auto result = co_await client.call_tool("whoami", json::object());
        outcome->whoami = *result.structuredContent;
    } catch (const std::exception& error) {
        outcome->error = error.what();
    }
}

}  // namespace

// Over a memory pair, every client protocol against a server that serves both eras.
TEST(E2eEra, MemoryClientAgainstDualEraServer) {
    struct Case {
        mcp::ClientProtocol protocol;
        mcp::ProtocolEra era;
        std::string version;
    };
    const std::vector<Case> cases = {
        {mcp::ClientProtocol::Auto(), mcp::ProtocolEra::eModern, "2026-07-28"},
        {mcp::ClientProtocol::Version("2026-07-28"), mcp::ProtocolEra::eModern, "2026-07-28"},
        {mcp::ClientProtocol::Legacy(), mcp::ProtocolEra::eLegacy, "2025-11-25"},
        {mcp::ClientProtocol::Version("2025-06-18"), mcp::ProtocolEra::eLegacy, "2025-06-18"},
    };
    for (const auto& item : cases) {
        boost::asio::io_context io;
        boost::asio::steady_timer guard(io);
        bool fired = false;
        auto [client_transport, server_transport] =
            mcp::create_memory_transport_pair(io.get_executor());
        auto server = make_dual_era_server();
        mcp::ClientOptions options;
        options.protocol = item.protocol;
        std::vector<mcp::EraDecision> decisions;
        options.on_era_decision = [&decisions](const mcp::EraDecision& decision) {
            decisions.push_back(decision);
        };
        mcp::Client client(client_transport, io.get_executor(), options);
        auto outcome = std::make_shared<EraOutcome>();

        boost::asio::co_spawn(
            io,
            [&]() -> mcp::Task<void> {
                try {
                    co_await server->run(server_transport, io.get_executor());
                } catch (...) {
                }
            },
            boost::asio::detached);
        run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
            co_await connect_and_ask(client, outcome);
            client.close();
            server_transport->close();
        });

        EXPECT_EQ(outcome->error, "");
        EXPECT_EQ(outcome->connected.era, item.era);
        EXPECT_EQ(outcome->connected.protocolVersion, item.version);
        ASSERT_TRUE(outcome->connected.serverInfo.has_value());
        EXPECT_EQ(outcome->connected.serverInfo->name, "dual-era-server");
        EXPECT_TRUE(outcome->connected.capabilities.tools.has_value());
        const bool modern = item.era == mcp::ProtocolEra::eModern;
        EXPECT_EQ(outcome->whoami.at("modern"), modern) << outcome->whoami;
        if (modern) {
            EXPECT_EQ(outcome->whoami.at("version"), "2026-07-28");
            EXPECT_EQ(outcome->whoami.at("client"), "era-client");
        }
        ASSERT_EQ(decisions.size(), 1u);
        EXPECT_EQ(decisions[0].era, item.era);
    }
}

// An SDK server that serves only one era, met with Auto.
TEST(E2eEra, MemoryAutoAgainstSingleEraServers) {
    struct Case {
        mcp::ServerProtocolPolicy policy;
        mcp::ProtocolEra era;
        mcp::EraReason reason;
    };
    mcp::ServerProtocolPolicy modern_only;
    modern_only.serve_legacy = false;
    mcp::ServerProtocolPolicy legacy_only;
    legacy_only.serve_modern = false;
    const std::vector<Case> cases = {
        {modern_only, mcp::ProtocolEra::eModern, mcp::EraReason::eDiscoverOk},
        {legacy_only, mcp::ProtocolEra::eLegacy, mcp::EraReason::eModernErrorRetry},
    };
    for (const auto& item : cases) {
        boost::asio::io_context io;
        boost::asio::steady_timer guard(io);
        bool fired = false;
        auto [client_transport, server_transport] =
            mcp::create_memory_transport_pair(io.get_executor());
        auto server = make_dual_era_server();
        server->set_protocol_policy(item.policy);
        mcp::ClientOptions options = auto_options();
        std::vector<mcp::EraDecision> decisions;
        options.on_era_decision = [&decisions](const mcp::EraDecision& decision) {
            decisions.push_back(decision);
        };
        mcp::Client client(client_transport, io.get_executor(), options);
        auto outcome = std::make_shared<EraOutcome>();

        boost::asio::co_spawn(
            io,
            [&]() -> mcp::Task<void> {
                try {
                    co_await server->run(server_transport, io.get_executor());
                } catch (...) {
                }
            },
            boost::asio::detached);
        run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
            co_await connect_and_ask(client, outcome);
            client.close();
            server_transport->close();
        });

        EXPECT_EQ(outcome->error, "");
        EXPECT_EQ(outcome->connected.era, item.era);
        ASSERT_EQ(decisions.size(), 1u);
        EXPECT_EQ(decisions[0].reason, item.reason);
        EXPECT_EQ(outcome->whoami.at("modern"), item.era == mcp::ProtocolEra::eModern);
    }
}

// A legacy-pinned client of a modern-only server gets the server's explanation, not a hang.
TEST(E2eEra, LegacyClientOfModernOnlyServerGetsTheServedVersions) {
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    auto [client_transport, server_transport] = mcp::create_memory_transport_pair(io.get_executor());
    auto server = make_dual_era_server();
    mcp::ServerProtocolPolicy modern_only;
    modern_only.serve_legacy = false;
    server->set_protocol_policy(modern_only);
    mcp::Client client(client_transport, io.get_executor());
    auto outcome = std::make_shared<EraOutcome>();

    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                co_await server->run(server_transport, io.get_executor());
            } catch (...) {
            }
        },
        boost::asio::detached);
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await connect_and_ask(client, outcome);
        client.close();
        server_transport->close();
    });

    EXPECT_NE(outcome->error.find("2026-07-28"), std::string::npos) << outcome->error;
}

// A real stdio pair: the client reads and writes streams, the server runs run_stdio() on a thread.
TEST(E2eEra, StdioClientAgainstDualEraServerBothEras) {
    for (const bool auto_detect : {true, false}) {
        PipeBuffer to_server;
        PipeBuffer to_client;
        PipeWriter server_out(to_client);
        PipeWriter client_out(to_server);
        std::istream server_in(&to_server);
        std::ostream server_output(&server_out);
        std::istream client_in(&to_client);
        std::ostream client_output(&client_out);

        auto server = make_dual_era_server();
        std::thread server_thread([&] { server->run_stdio(server_in, server_output); });

        boost::asio::io_context io;
        boost::asio::steady_timer guard(io);
        bool fired = false;
        auto transport =
            std::make_shared<mcp::StdioTransport>(io.get_executor(), client_in, client_output);
        mcp::ClientOptions options = auto_detect ? auto_options() : pinned_options("2025-11-25");
        mcp::Client client(transport, io.get_executor(), options);
        auto outcome = std::make_shared<EraOutcome>();

        run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
            co_await connect_and_ask(client, outcome);
            client.close();
        });

        // Let the server see end of input, then the client's reader see end of input.
        to_server.close();
        server_thread.join();
        to_client.close();

        EXPECT_EQ(outcome->error, "");
        EXPECT_EQ(outcome->connected.era,
                  auto_detect ? mcp::ProtocolEra::eModern : mcp::ProtocolEra::eLegacy);
        EXPECT_EQ(outcome->whoami.at("modern"), auto_detect);
        EXPECT_EQ(outcome->connected.serverInfo->name, "dual-era-server");
    }
}

// Auto over HTTP: HttpClientTransport does not send the probe as a modern request, so the server
// refuses it and the client carries on with initialize. The session is a legacy one.
TEST(E2eEra, AutoOverHttpEndsUpLegacy) {
    constexpr unsigned short port = 20500;
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [](const boost::asio::any_io_executor&) { return make_dual_era_server(); });
    auto transport = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    std::vector<mcp::EraDecision> decisions;
    mcp::ClientOptions options = auto_options();
    options.on_era_decision = [&decisions](const mcp::EraDecision& decision) {
        decisions.push_back(decision);
    };
    mcp::Client client(transport, io.get_executor(), options);
    auto outcome = std::make_shared<EraOutcome>();

    boost::asio::co_spawn(io, manager.listen(), boost::asio::detached);
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await connect_and_ask(client, outcome);
        client.close();
        manager.close();
    });

    EXPECT_EQ(outcome->error, "");
    EXPECT_EQ(outcome->connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(outcome->whoami.at("modern"), false);
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].reason, mcp::EraReason::eFallbackNonModernError);
}

// A client left at its default speaks the legacy protocol over HTTP exactly as before.
TEST(E2eEra, DefaultClientOverHttpIsLegacy) {
    constexpr unsigned short port = 20501;
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [](const boost::asio::any_io_executor&) { return make_dual_era_server(); });
    auto transport = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client client(transport, io.get_executor());
    auto outcome = std::make_shared<EraOutcome>();

    boost::asio::co_spawn(io, manager.listen(), boost::asio::detached);
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await connect_and_ask(client, outcome);
        client.close();
        manager.close();
    });

    EXPECT_EQ(outcome->error, "");
    EXPECT_EQ(outcome->connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(outcome->whoami.at("modern"), false);
}
