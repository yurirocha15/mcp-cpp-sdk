#include "support/hang_guard.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    // Asks a URL with the id in "id"; with "complete" set, it then completes that id twice and an
    // id it never sent once, and reports which completions were sent.
    server->add_tool<json, json>(
        "open_with_id", "Asks a URL with a given id", json{{"type", "object"}},
        [](mcp::Context& ctx, json args) -> mcp::Task<json> {
            const auto id = std::make_shared<const std::string>(args.at("id").get<std::string>());
            mcp::ElicitRequestURLParams params;
            params.message = "Please sign in";
            params.url = "https://example.com/sign-in";
            params.elicitationId = *id;
            const auto answer = co_await ctx.elicit(params);
            json outcome = {{"action", json(answer.action)}};
            if (args.value("complete", false)) {
                const bool first = co_await ctx.notify_elicitation_complete(*id);
                const bool again = co_await ctx.notify_elicitation_complete(*id);
                const auto never_sent = std::make_shared<const std::string>("never-sent");
                const bool foreign = co_await ctx.notify_elicitation_complete(*never_sent);
                outcome["first"] = first;
                outcome["again"] = again;
                outcome["foreign"] = foreign;
            }
            co_return outcome;
        });
    // Needs a sign-in first, which a 2025-11-25 session learns of from a -32042 error.
    server->add_tool<json, json>("needs_sign_in", "Needs a sign-in first", json{{"type", "object"}},
                                 [](mcp::Context& /*ctx*/, json /*args*/) -> mcp::Task<json> {
                                     mcp::ElicitRequestURLParams params;
                                     params.message = "Please sign in";
                                     params.url = "https://example.com/sign-in";
                                     params.elicitationId = "sign-in-1";
                                     throw mcp::UrlElicitationRequired({params});
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
    mcp::Server& server() { return *server_; }
    /// The server's end of the transport, for messages the server itself would not send.
    std::shared_ptr<mcp::ITransport> server_transport() { return server_transport_; }

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

namespace {

/// Hands everything to the transport it wraps and keeps a copy of each message in either direction.
class RecordingTransport final : public mcp::ITransport {
   public:
    explicit RecordingTransport(std::shared_ptr<mcp::ITransport> inner) : inner_(std::move(inner)) {}

    mcp::Task<std::string> read_message() override {
        auto message = co_await inner_->read_message();
        read.push_back(json::parse(message));
        co_return message;
    }

    mcp::Task<void> write_message(std::string_view message) override {
        written.push_back(json::parse(message));
        return inner_->write_message(message);
    }

    void close() override { inner_->close(); }

    std::vector<json> read;
    std::vector<json> written;

   private:
    std::shared_ptr<mcp::ITransport> inner_;
};

// Waits with no time limit of its own: sanitizer builds run many times slower, so a wait that never
// ends is left to the hang guard.
mcp::Task<void> wait_until(std::function<bool()> condition) {
    auto executor = co_await boost::asio::this_coro::executor;
    while (!condition()) {
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

mcp::ElicitRequestParams name_question() {
    mcp::ElicitRequestFormParams params;
    params.message = "Your name?";
    params.requestedSchema =
        json::parse(R"({"type":"object","properties":{"name":{"type":"string"}}})");
    return params;
}

}  // namespace

// A server that stops waiting for its own request tells the client with notifications/cancelled.
// Both peers number their requests from 1, so the id it names can be that of a request the client
// is waiting on, which goes on and gets its answer.
TEST(ElicitationTimeoutTest, TheServersCancelLeavesTheClientsRequestWithTheSameId) {
    boost::asio::io_context io;
    auto [client_end, server_end] = mcp::make_memory_transport_pair(io.get_executor());
    auto recorder = std::make_shared<RecordingTransport>(client_end);

    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    mcp::Server server(mcp::Implementation{"asking", "1.0"}, capabilities);
    server.set_reverse_request_timeout(std::chrono::milliseconds(50));
    server.add_tool<json, json>(
        "ask_twice", "Asks twice and counts the questions left unanswered", json{{"type", "object"}},
        [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            const auto question = std::make_shared<const mcp::ElicitRequestParams>(name_question());
            int unanswered = 0;
            for (int ask = 0; ask < 2; ++ask) {
                try {
                    static_cast<void>(co_await ctx.elicit(*question));
                } catch (const std::exception&) {
                    ++unanswered;
                }
            }
            co_return json{{"unanswered", unanswered}};
        });

    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    options.request_timeout = g_hang_guard;
    mcp::Client client(recorder, io.get_executor(), options);
    // The questions are answered only once the call is over, after the server stopped waiting.
    auto released = std::make_shared<bool>(false);
    auto answered = std::make_shared<int>(0);
    client.on_elicitation(
        [released, answered](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
            std::function<bool()> call_over = [released] { return *released; };
            co_await wait_until(std::move(call_over));
            ++*answered;
            mcp::ElicitResult answer;
            answer.action = mcp::ElicitAction::eDecline;
            co_return answer;
        });

    boost::asio::steady_timer guard(io, g_hang_guard);
    bool fired = false;
    guard.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            fired = true;
            io.stop();
        }
    });
    json outcome;
    std::string failure;
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        try {
            co_await server.run(server_end, io.get_executor());
        } catch (...) {
        }
    });
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        try {
            // The elicitation handler makes the client declare elicitation.
            co_await client.connect("test-client", "1.0");
            try {
                outcome = co_await call(client, "ask_twice", json::object());
            } catch (const std::exception& error) {
                failure = error.what();
            }
            *released = true;
            std::function<bool()> both_answered = [answered] { return *answered == 2; };
            co_await wait_until(std::move(both_answered));
        } catch (const std::exception& error) {
            ADD_FAILURE() << "unexpected exception: " << error.what();
        }
        client.close();
        server_end->close();
        guard.cancel();
    });
    io.run();
    ASSERT_FALSE(fired) << "the test did not finish";

    json call_id;
    for (const auto& message : recorder->written) {
        if (message.value("method", "") == "tools/call") {
            call_id = message.at("id");
        }
    }
    ASSERT_FALSE(call_id.is_null());
    bool named = false;
    for (const auto& message : recorder->read) {
        named = named || (message.value("method", "") == "notifications/cancelled" &&
                          message.at("params").at("requestId") == call_id);
    }
    ASSERT_TRUE(named) << "no cancel named the id of the client's call";
    EXPECT_EQ(failure, "");
    EXPECT_EQ(outcome, json::parse(R"({"unanswered":2})"));
}

// notifications/elicitation/complete (2025-11-25) reaches the session that sent the elicitation,
// once, and a client hands its callback only the ids it was sent.
TEST(ElicitationCompletion, TheHandlerCompletesItsOwnElicitationOnce) {
    Session session(false, true);
    answer_with(session, json::object());
    std::vector<std::string> completed;
    session.client().on_elicitation_complete(
        [&completed](const mcp::ElicitationCompleteNotificationParams& params) {
            completed.push_back(params.elicitationId);
        });
    json outcome;
    session.run([&]() -> mcp::Task<void> {
        json args = arg("id", "flow-1");
        args["complete"] = true;
        outcome = co_await call(session.client(), "open_with_id", std::move(args));
    });
    EXPECT_EQ(outcome.value("first", false), true) << outcome;
    EXPECT_EQ(outcome.value("again", true), false) << outcome;
    EXPECT_EQ(outcome.value("foreign", true), false) << outcome;
    EXPECT_EQ(completed, std::vector<std::string>{"flow-1"});
}

TEST(ElicitationCompletion, TheServerCompletesAnElicitationAfterTheHandlerReturned) {
    Session session(false, true);
    answer_with(session, json::object());
    std::vector<std::string> completed;
    session.client().on_elicitation_complete(
        [&completed](const mcp::ElicitationCompleteNotificationParams& params) {
            completed.push_back(params.elicitationId);
        });
    bool sent = false;
    bool again = true;
    session.run([&]() -> mcp::Task<void> {
        static_cast<void>(co_await call(session.client(), "open_with_id", arg("id", "flow-2")));
        const auto id = std::make_shared<const std::string>("flow-2");
        sent = co_await session.server().notify_elicitation_complete(*id);
        again = co_await session.server().notify_elicitation_complete(*id);
        std::function<bool()> arrived = [&completed] { return !completed.empty(); };
        co_await wait_until(std::move(arrived));
    });
    EXPECT_TRUE(sent);
    EXPECT_FALSE(again);
    EXPECT_EQ(completed, std::vector<std::string>{"flow-2"});
}

// 2026-07-28 has no elicitation ids, so there is nothing to complete.
TEST(ElicitationCompletion, AModernRequestSendsNoCompletion) {
    Session session(true, true);
    answer_with(session, json::object());
    std::size_t completions = 0;
    session.client().on_elicitation_complete(
        [&completions](const mcp::ElicitationCompleteNotificationParams&) { ++completions; });
    json outcome;
    session.run([&]() -> mcp::Task<void> {
        json args = arg("id", "flow-3");
        args["complete"] = true;
        outcome = co_await call(session.client(), "open_with_id", std::move(args));
    });
    EXPECT_EQ(outcome.value("first", true), false) << outcome;
    EXPECT_EQ(completions, 0U);
}

// elicitation.mdx: clients MUST ignore notifications that name an unknown or completed id.
TEST(ElicitationCompletion, TheClientIgnoresUnknownAndRepeatedIds) {
    Session session(false, true);
    answer_with(session, json::object());
    std::vector<std::string> completed;
    session.client().on_elicitation_complete(
        [&completed](const mcp::ElicitationCompleteNotificationParams& params) {
            completed.push_back(params.elicitationId);
        });
    bool marker = false;
    session.client().on_notification("notifications/tools/list_changed",
                                     [&marker](const json&) { marker = true; });
    session.run([&]() -> mcp::Task<void> {
        static_cast<void>(co_await call(session.client(), "open_with_id", arg("id", "flow-4")));
        auto server_end = session.server_transport();
        for (const char* id : {"unknown", "flow-4", "flow-4"}) {
            const auto wire = std::make_shared<const std::string>(json{
                {"jsonrpc", "2.0"},
                {"method", "notifications/elicitation/complete"},
                {"params", {{"elicitationId", id}}}}.dump());
            co_await server_end->write_message(*wire);
        }
        const auto last = std::make_shared<const std::string>(
            json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}.dump());
        co_await server_end->write_message(*last);
        std::function<bool()> seen = [&marker] { return marker; };
        co_await wait_until(std::move(seen));
    });
    EXPECT_EQ(completed, std::vector<std::string>{"flow-4"});
}

// A 2025-11-25 server answers -32042 when the user must complete an elicitation first; the client
// keeps the id, so the completion that follows reaches the callback.
TEST(ElicitationCompletion, AnElicitationFromAUrlElicitationErrorIsCompleted) {
    Session session(false, true);
    std::vector<std::string> completed;
    session.client().on_elicitation_complete(
        [&completed](const mcp::ElicitationCompleteNotificationParams& params) {
            completed.push_back(params.elicitationId);
        });
    int code = 0;
    std::optional<std::vector<mcp::ElicitRequestURLParams>> required;
    bool sent = false;
    session.run([&]() -> mcp::Task<void> {
        try {
            static_cast<void>(co_await call(session.client(), "needs_sign_in", json::object()));
        } catch (const mcp::McpError& error) {
            code = error.code();
            required = mcp::url_elicitations_required(error);
        }
        const auto id = std::make_shared<const std::string>("sign-in-1");
        sent = co_await session.server().notify_elicitation_complete(*id);
        std::function<bool()> arrived = [&completed] { return !completed.empty(); };
        co_await wait_until(std::move(arrived));
    });
    EXPECT_EQ(code, jsonrpc::g_URL_ELICITATION_REQUIRED);
    ASSERT_TRUE(required.has_value());
    ASSERT_EQ(required->size(), 1U);
    EXPECT_EQ(required->at(0).elicitationId, "sign-in-1");
    EXPECT_EQ(required->at(0).url, "https://example.com/sign-in");
    EXPECT_EQ(required->at(0).message, "Please sign in");
    EXPECT_TRUE(sent);
    EXPECT_EQ(completed, std::vector<std::string>{"sign-in-1"});
}

TEST(UrlElicitationsRequired, ReadsOnlyAUrlElicitationErrorThatListsUrlElicitations) {
    const auto elicitation = json::parse(
        R"({"mode":"url","message":"Sign in","url":"https://example.com","elicitationId":"e-1"})");
    const auto data = [](json listed) { return json{{"elicitations", std::move(listed)}}; };

    const auto required = mcp::url_elicitations_required(mcp::McpError(
        jsonrpc::g_URL_ELICITATION_REQUIRED, "Sign in first", data(json::array({elicitation}))));
    ASSERT_TRUE(required.has_value());
    ASSERT_EQ(required->size(), 1U);
    EXPECT_EQ(json(required->at(0)), elicitation);

    EXPECT_FALSE(mcp::url_elicitations_required(mcp::McpError(jsonrpc::g_INTERNAL_ERROR, "Failed",
                                                              data(json::array({elicitation}))))
                     .has_value());
    EXPECT_FALSE(
        mcp::url_elicitations_required(mcp::McpError(jsonrpc::g_URL_ELICITATION_REQUIRED, "No data"))
            .has_value());
    auto without_id = elicitation;
    without_id.erase("elicitationId");
    auto form = elicitation;
    form["mode"] = "form";
    for (const auto& listed : {json::object(), json::array({without_id}), json::array({form}),
                               json::array({"not an object"})}) {
        EXPECT_FALSE(mcp::url_elicitations_required(
                         mcp::McpError(jsonrpc::g_URL_ELICITATION_REQUIRED, "Bad data", data(listed)))
                         .has_value())
            << listed;
    }
}

}  // namespace client_elicitation_test
