#include "support/test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/core/deprecation.hpp"
#include "mcp/server/server.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace deprecation_test {

namespace {

using mcp::DeprecatedFeature;

/// Sets an environment variable for one test and restores the previous value.
class ScopedVariable {
   public:
    ScopedVariable(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) {
            previous_ = previous;
        }
        set(value);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ~ScopedVariable() { set(previous_ ? previous_->c_str() : ""); }

   private:
    void set(const char* value) const {
#if defined(_WIN32)
        _putenv_s(name_, value);
#else
        if (*value == '\0') {
            ::unsetenv(name_);
        } else {
            ::setenv(name_, value, 1);
        }
#endif
    }

    const char* name_;
    std::optional<std::string> previous_;
};

constexpr const char* g_SILENCE = "MCP_SILENCE_DEPRECATION_NOTICES";

/// The features a handler received, in order.
struct Received {
    std::vector<DeprecatedFeature> features;

    mcp::DeprecationHandler handler() {
        return [this](const mcp::DeprecationNotice& notice) { features.push_back(notice.feature); };
    }
};

nlohmann::json tools_call(std::string_view id, std::string_view tool) {
    return nlohmann::json{{"jsonrpc", "2.0"},
                          {"id", id},
                          {"method", "tools/call"},
                          {"params", {{"name", tool}, {"arguments", nlohmann::json::object()}}}};
}

}  // namespace

TEST(DeprecationNoticeTest, EachFeatureNamesItsRegistryEntry) {
    struct Expected {
        DeprecatedFeature feature;
        std::string_view proposal;
        std::string_view deprecated_in;
    };
    const std::vector<Expected> expected{
        {DeprecatedFeature::eLogging, "SEP-2577", "2026-07-28"},
        {DeprecatedFeature::eSampling, "SEP-2577", "2026-07-28"},
        {DeprecatedFeature::eRoots, "SEP-2577", "2026-07-28"},
        {DeprecatedFeature::eIncludeContext, "SEP-2596", "2025-11-25"},
        {DeprecatedFeature::eDynamicClientRegistration, "PR #2858", "2026-07-28"},
    };
    for (const auto& entry : expected) {
        const auto& notice = mcp::deprecation_notice(entry.feature);
        EXPECT_EQ(notice.feature, entry.feature);
        EXPECT_EQ(notice.proposal, entry.proposal);
        EXPECT_EQ(notice.deprecated_in, entry.deprecated_in);
        EXPECT_FALSE(notice.name.empty());
        EXPECT_FALSE(notice.earliest_removal.empty());
        EXPECT_FALSE(notice.migration.empty());
    }
    EXPECT_EQ(mcp::deprecation_notice(DeprecatedFeature::eLogging).earliest_removal,
              "first revision released on or after 2027-07-28");
}

mcp::Server make_server() {
    return mcp::Server(mcp::Implementation{"test-server", "1.0"}, mcp::ServerCapabilities{});
}

TEST(DeprecationNoticeTest, ReportsEachFeatureOncePerServer) {
    Received received;
    auto server = make_server();
    server.set_deprecation_handler(received.handler());
    (void)server.get_log_level();
    (void)server.get_log_level();
    EXPECT_EQ(received.features, std::vector<DeprecatedFeature>{DeprecatedFeature::eLogging});

    Received other;
    auto second = make_server();
    second.set_deprecation_handler(other.handler());
    (void)second.get_log_level();
    EXPECT_EQ(other.features, std::vector<DeprecatedFeature>{DeprecatedFeature::eLogging});
}

TEST(DeprecationNoticeTest, AReplacedHandlerReceivesTheNotice) {
    Received first;
    Received second;
    auto server = make_server();
    server.set_deprecation_handler(first.handler());
    server.set_deprecation_handler(second.handler());
    (void)server.get_log_level();
    EXPECT_TRUE(first.features.empty());
    EXPECT_EQ(second.features, std::vector<DeprecatedFeature>{DeprecatedFeature::eLogging});
}

TEST(DeprecationNoticeTest, WithoutAHandlerTheNoticeGoesToStandardErrorOnce) {
    const ScopedVariable unset(g_SILENCE, "");
    auto server = make_server();
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    (void)server.get_log_level();
    (void)server.get_log_level();
    const auto error = testing::internal::GetCapturedStderr();
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(error,
              "mcp-cpp-sdk: SEP-2577: Logging is deprecated since protocol revision 2026-07-28; "
              "earliest removal: first revision released on or after 2027-07-28. Log to standard "
              "error on stdio; use OpenTelemetry for observability.\n");
    EXPECT_EQ(output, "");
}

TEST(DeprecationNoticeTest, TheEnvironmentVariableSilencesStandardErrorOnly) {
    const ScopedVariable silence(g_SILENCE, "1");
    auto silent = make_server();
    testing::internal::CaptureStderr();
    (void)silent.get_log_level();
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "");

    Received received;
    auto handled = make_server();
    handled.set_deprecation_handler(received.handler());
    (void)handled.get_log_level();
    EXPECT_EQ(received.features, std::vector<DeprecatedFeature>{DeprecatedFeature::eLogging});
}

// The four Context features report through the server's handler, once each however often a
// handler uses them; get_log_level() reports Logging too.
TEST(DeprecationNoticeTest, ServerHandlersReportTheDeprecatedFeaturesTheyUse) {
    boost::asio::io_context io_ctx;
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw_transport = transport.get();

    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    caps.logging = nlohmann::json::object();
    mcp::Server server(mcp::Implementation{"test-server", "1.0"}, std::move(caps));
    Received received;
    server.set_deprecation_handler(received.handler());

    server.add_tool<nlohmann::json, nlohmann::json>(
        "deprecated", "Uses deprecated features", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
            co_await ctx.log_info("one");
            co_await ctx.log(mcp::LoggingLevel::eError, "two");
            mcp::CreateMessageRequestParams request;
            request.maxTokens = 1;
            request.includeContext = std::string(mcp::constants::include_context::g_THIS_SERVER);
            try {
                // The client did not declare sampling.context, so this throws.
                (void)co_await ctx.sample_llm(request);
            } catch (const std::invalid_argument&) {
            }
            const auto roots = co_await ctx.request_roots();
            co_return nlohmann::json{{"roots", roots.roots.size()}};
        });

    int results = 0;
    raw_transport->set_on_write([&results, raw_transport](std::string_view text) {
        const auto message = nlohmann::json::parse(text);
        if (message.value("method", "") == "roots/list") {
            raw_transport->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"},
                               {"id", message.at("id")},
                               {"result", {{"roots", nlohmann::json::array()}}}}
                    .dump());
        } else if (message.contains("result") && message.value("id", "") != "init") {
            if (++results == 2) {
                raw_transport->close();
            }
        }
    });

    nlohmann::json initialize = make_initialize_request("init");
    initialize["params"]["capabilities"] = {{"roots", nlohmann::json::object()},
                                            {"sampling", nlohmann::json::object()}};
    raw_transport->enqueue_message(initialize.dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(tools_call("1", "deprecated").dump());
    raw_transport->enqueue_message(tools_call("2", "deprecated").dump());

    mcp_test::spawn_detached(
        io_ctx, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx.get_executor()); });
    io_ctx.run();

    ASSERT_EQ(results, 2);
    (void)server.get_log_level();
    EXPECT_EQ(received.features, (std::vector<DeprecatedFeature>{
                                     DeprecatedFeature::eLogging, DeprecatedFeature::eSampling,
                                     DeprecatedFeature::eIncludeContext, DeprecatedFeature::eRoots}));
}

TEST(DeprecationNoticeTest, ClientRegistrationsReportSamplingAndRootsOnce) {
    boost::asio::io_context io_ctx;
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    Received received;
    mcp::ClientOptions options;
    options.deprecation_handler = received.handler();
    mcp::Client client(transport, io_ctx.get_executor(), std::move(options));

    client.set_roots({});
    client.on_roots_list([](const nlohmann::json&) -> mcp::Task<mcp::ListRootsResult> {
        co_return mcp::ListRootsResult{};
    });
    client.on_sampling(
        [](const mcp::CreateMessageRequestParams&) -> mcp::Task<mcp::CreateMessageResult> {
            co_return mcp::CreateMessageResult{};
        });
    client.on_sampling(
        [](const mcp::CreateMessageRequestParams&) -> mcp::Task<mcp::CreateMessageResult> {
            co_return mcp::CreateMessageResult{};
        });
    EXPECT_EQ(received.features, (std::vector<DeprecatedFeature>{DeprecatedFeature::eRoots,
                                                                 DeprecatedFeature::eSampling}));
}

TEST(DeprecationNoticeTest, SetLogLevelReportsLoggingOnce) {
    boost::asio::io_context io_ctx;
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    Received received;
    mcp::ClientOptions options;
    options.deprecation_handler = received.handler();
    mcp::Client client(transport, io_ctx.get_executor(), std::move(options));

    // The client is not connected, so nothing is sent; each call still uses the feature.
    for (int call = 0; call < 2; ++call) {
        EXPECT_THROW(static_cast<void>(client.set_log_level(mcp::LoggingLevel::eInfo)), mcp::McpError);
    }
    EXPECT_EQ(received.features, std::vector<DeprecatedFeature>{DeprecatedFeature::eLogging});
}

}  // namespace deprecation_test
