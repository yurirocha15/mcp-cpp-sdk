#include "../support/hang_guard.hpp"

#include "../support/spawn.hpp"
#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace client_elicitation_test {

namespace jsonrpc = mcp::constants::jsonrpc;

namespace {

using nlohmann::json;

constexpr const char* g_form_schema =
    R"({"type":"object","properties":{"name":{"type":"string"},"age":{"type":"integer","minimum":18}},
        "required":["name"]})";

/// A server whose "ask" tool asks the form above and reports what came back; "open" asks a URL.
std::unique_ptr<mcp::Server> make_server() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"asking", "1.0"}, capabilities);
    server->add_tool<json, json>("ask", "Asks the form", json{{"type", "object"}},
                                 [](mcp::Context& ctx, json args) -> mcp::Task<json> {
                                     mcp::ElicitRequestFormParams params;
                                     params.message = "Who are you?";
                                     params.requestedSchema =
                                         args.value("unknown_dialect", false)
                                             ? json{{"$schema", "https://example.com/unknown"},
                                                    {"type", "object"}}
                                             : json::parse(g_form_schema);
                                     json outcome;
                                     try {
                                         mcp::InputOptions options;
                                         options.validate_content = args.value("validate", true);
                                         const auto answer = co_await ctx.elicit(params, options);
                                         outcome = {{"action", json(answer.action)},
                                                    {"content", answer.content.value_or(nullptr)}};
                                     } catch (const std::exception& error) {
                                         outcome = {{"error", error.what()}};
                                     }
                                     co_return outcome;
                                 });
    server->add_tool<json, json>("open", "Asks the user to open a page", json{{"type", "object"}},
                                 [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                     mcp::ElicitRequestURLParams params;
                                     params.message = "Please sign in";
                                     params.url = "https://example.com/sign-in";
                                     const auto answer = co_await ctx.elicit(params);
                                     co_return json{{"action", json(answer.action)}};
                                 });
    return server;
}

/// A client and a server on a memory transport, in one era.
class Session {
   public:
    Session(bool modern, bool client_validates) : guard_(io_) {
        mcp::ClientOptions options;
        options.protocol =
            modern ? mcp::ClientProtocol::pinned("2026-07-28") : mcp::ClientProtocol::legacy();
        options.validate_elicitation_content = client_validates;
        auto [client_transport, server_transport] = mcp::make_memory_transport_pair(io_.get_executor());
        server_ = make_server();
        server_transport_ = server_transport;
        client_ = std::make_unique<mcp::Client>(client_transport, io_.get_executor(), options);
    }

    mcp::Client& client() { return *client_; }

    /// Runs @p body after connecting, with the server serving the other end.
    void run(std::function<mcp::Task<void>()> body) {
        guard_.expires_after(g_hang_guard);
        guard_.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                fired_ = true;
                io_.stop();
            }
        });
        mcp_test::spawn_detached(io_, [this]() -> mcp::Task<void> {
            try {
                co_await server_->run(server_transport_, io_.get_executor());
            } catch (...) {
            }
        });
        mcp_test::spawn_detached(io_, [this, body = std::move(body)]() -> mcp::Task<void> {
            try {
                mcp::ClientCapabilities capabilities;
                capabilities.elicitation = mcp::ClientCapabilities::ElicitationCapability{
                    nlohmann::json::object(), nlohmann::json::object()};
                mcp::Implementation info;
                info.name = "elicit-client-with-a-long-name";
                info.version = "1.0.0-long-version-string";
                co_await client_->connect(info, capabilities);
                co_await body();
            } catch (const std::exception& error) {
                ADD_FAILURE() << "unexpected exception: " << error.what();
            }
            client_->close();
            server_transport_->close();
            guard_.cancel();
        });
        io_.run();
        EXPECT_FALSE(fired_) << "the test did not finish";
    }

   private:
    boost::asio::io_context io_;
    boost::asio::steady_timer guard_;
    bool fired_{false};
    std::unique_ptr<mcp::Server> server_;
    std::shared_ptr<mcp::ITransport> server_transport_;
    std::unique_ptr<mcp::Client> client_;
};

void answer_with(Session& session, json content, std::size_t* calls = nullptr) {
    session.client().on_elicitation(
        [content = std::move(content),
         calls](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
            if (calls != nullptr) {
                ++*calls;
            }
            mcp::ElicitResult answer;
            answer.action = mcp::ElicitAction::eAccept;
            answer.content = content;
            co_return answer;
        });
}

/// A one-member object, built by a call: GCC 11 mishandles a braced temporary passed to a coroutine.
json arg(const char* key, json value) {
    json object = json::object();
    object[key] = std::move(value);
    return object;
}

mcp::Task<json> call(mcp::Client& client, const std::string& tool, json arguments) {
    const auto result = co_await client.call_tool(tool, arguments);
    co_return result.structuredContent.value_or(json(nullptr));
}

class ElicitationValidation : public ::testing::TestWithParam<bool> {
   protected:
    [[nodiscard]] bool modern() const { return GetParam(); }
};

}  // namespace

TEST_P(ElicitationValidation, ConformingContentIsSent) {
    Session session(modern(), true);
    answer_with(session, json{{"name", "Ada"}, {"age", 36}});
    json outcome;
    session.run(
        [&]() -> mcp::Task<void> { outcome = co_await call(session.client(), "ask", json::object()); });
    EXPECT_EQ(outcome["action"], "accept");
    EXPECT_EQ(outcome["content"], (json{{"name", "Ada"}, {"age", 36}}));
}

TEST_P(ElicitationValidation, NonConformingContentIsNotSent) {
    Session session(modern(), true);
    std::size_t calls = 0;
    answer_with(session, json{{"age", 5}}, &calls);
    json outcome;
    std::optional<int> code;
    session.run([&]() -> mcp::Task<void> {
        try {
            outcome = co_await call(session.client(), "ask", json::object());
        } catch (const mcp::ElicitationContentError& error) {
            code = error.code();
            EXPECT_TRUE(error.data()->at("violations").is_array());
        }
    });
    EXPECT_EQ(calls, 1u);
    if (modern()) {
        // The question came back in the result of the call, so its failure is the call's.
        EXPECT_EQ(code, jsonrpc::g_INVALID_PARAMS);
    } else {
        // The server asked with a request of its own, and that request was answered with an error.
        EXPECT_FALSE(code.has_value());
        ASSERT_TRUE(outcome.contains("error")) << outcome;
        EXPECT_FALSE(outcome.contains("content"));
    }
}

TEST_P(ElicitationValidation, ClientValidationCanBeTurnedOff) {
    Session session(modern(), false);
    answer_with(session, json{{"age", 5}});
    json outcome;
    session.run([&]() -> mcp::Task<void> {
        outcome = co_await call(session.client(), "ask", arg("validate", false));
    });
    EXPECT_EQ(outcome["content"], (json{{"age", 5}}));
}

TEST_P(ElicitationValidation, ServerRejectsContentThatDoesNotConform) {
    Session session(modern(), false);
    answer_with(session, json{{"age", 5}});
    json outcome;
    session.run(
        [&]() -> mcp::Task<void> { outcome = co_await call(session.client(), "ask", json::object()); });
    ASSERT_TRUE(outcome.contains("error")) << outcome;
    EXPECT_NE(outcome["error"].get<std::string>().find("does not match"), std::string::npos) << outcome;
}

TEST_P(ElicitationValidation, UncompilableSchemaLeavesContentUnchecked) {
    Session session(modern(), true);
    answer_with(session, json{{"anything", true}});
    json outcome;
    session.run([&]() -> mcp::Task<void> {
        outcome = co_await call(session.client(), "ask", arg("unknown_dialect", true));
    });
    EXPECT_EQ(outcome["content"], (json{{"anything", true}}));
}

TEST_P(ElicitationValidation, OtherActionsAndUrlModeAreNotChecked) {
    Session session(modern(), true);
    session.client().on_elicitation(
        [](const mcp::ElicitRequestParams& params) -> mcp::Task<mcp::ElicitResult> {
            mcp::ElicitResult answer;
            // A decline carries no content; a URL answer is consent and carries none either.
            answer.action = std::holds_alternative<mcp::ElicitRequestURLParams>(params)
                                ? mcp::ElicitAction::eAccept
                                : mcp::ElicitAction::eDecline;
            co_return answer;
        });
    json declined;
    json opened;
    session.run([&]() -> mcp::Task<void> {
        declined = co_await call(session.client(), "ask", json::object());
        opened = co_await call(session.client(), "open", json::object());
    });
    EXPECT_EQ(declined["action"], "decline");
    EXPECT_EQ(opened["action"], "accept");
}

TEST_P(ElicitationValidation, AcceptWithoutContentFailsARequiredField) {
    Session session(modern(), true);
    session.client().on_elicitation(
        [](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
            mcp::ElicitResult answer;
            answer.action = mcp::ElicitAction::eAccept;
            co_return answer;
        });
    std::optional<int> code;
    json outcome;
    session.run([&]() -> mcp::Task<void> {
        try {
            outcome = co_await call(session.client(), "ask", json::object());
        } catch (const mcp::ElicitationContentError& error) {
            code = error.code();
        }
    });
    EXPECT_TRUE(code.has_value() || outcome.contains("error")) << outcome;
}

INSTANTIATE_TEST_SUITE_P(Eras, ElicitationValidation, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("Modern") : std::string("Legacy");
                         });

}  // namespace client_elicitation_test
