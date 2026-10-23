#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"
#include "mcp/transport/stdio.hpp"

#include <gtest/gtest.h>

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
#include <set>
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

/// What the handlers of the scripted tests were asked, in order.
std::vector<json>& handler_calls() {
    static std::vector<json> calls;
    return calls;
}

mcp::Task<json> answer_elicitation(const json& params) {
    handler_calls().push_back(params);
    co_return json{{"action", "accept"},
                   {"content", {{"name", "octocat:" + params.value("message", "")}}}};
}

mcp::Task<json> answer_sampling(const json& params) {
    handler_calls().push_back(params);
    co_return json{{"role", "assistant"},
                   {"content", {{"type", "text"}, {"text", "Paris"}}},
                   {"model", "test-model"}};
}

mcp::Task<json> answer_roots(const json& params) {
    handler_calls().push_back(params);
    co_return json{{"roots", json::array({{{"uri", "file:///work"}}})}};
}

void register_handlers(mcp::Client& client) {
    client.on_request("elicitation/create", answer_elicitation);
    client.on_request("sampling/createMessage", answer_sampling);
    client.on_request("roots/list", answer_roots);
}

json elicit_entry(const std::string& message) {
    return {{"method", "elicitation/create"},
            {"params",
             {{"mode", "form"},
              {"message", message},
              {"requestedSchema", {{"type", "object"}, {"properties", json::object()}}}}}};
}

json input_required(json requests, std::optional<std::string> state = std::nullopt) {
    json result = {{"resultType", "input_required"}};
    if (!requests.is_null()) {
        result["inputRequests"] = std::move(requests);
    }
    if (state) {
        result["requestState"] = std::move(*state);
    }
    return result;
}

json complete_tool_result() {
    return {{"resultType", "complete"},
            {"content", json::array({{{"type", "text"}, {"text", "done"}}})}};
}

json response_to(const json& request, json result) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}};
}

/// A client on a scripted transport that plays a 2026-07-28 server.
class Harness {
   public:
    using Reply = std::function<json(const json&, std::size_t)>;

    Harness() : transport(std::make_shared<ScriptedTransport>(io.get_executor())), guard(io) {
        mcp::ClientOptions options;
        options.protocol = mcp::ClientProtocol::Version("2026-07-28");
        client = std::make_unique<mcp::Client>(transport, io.get_executor(), options);
        handler_calls().clear();
        transport->set_on_write([this](std::string_view text) {
            auto message = json::parse(text);
            seen.push_back(message);
            if (!message.contains("id") || !message.contains("method")) {
                return;
            }
            const auto method = message.at("method").get<std::string>();
            if (method == "server/discover") {
                transport->enqueue_message(response_to(message, discover_result()).dump());
                return;
            }
            const auto index = ++calls[method];
            if (reply) {
                auto answer = reply(message, index);
                if (!answer.is_null()) {
                    transport->enqueue_message(response_to(message, std::move(answer)).dump());
                }
            }
        });
    }

    static json discover_result() {
        return {
            {"resultType", "complete"},
            {"supportedVersions", json::array({"2026-07-28"})},
            {"capabilities", {{"tools", json::object()}}},
            {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "peer"}, {"version", "1"}}}}}};
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
                    co_await client->connect("mrtr-client", "1.0");
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

    [[nodiscard]] std::vector<json> requests(std::string_view method) const {
        std::vector<json> matching;
        for (const auto& message : seen) {
            if (message.contains("id") && message.value("method", "") == method) {
                matching.push_back(message);
            }
        }
        return matching;
    }

    boost::asio::io_context io;
    std::shared_ptr<ScriptedTransport> transport;
    std::unique_ptr<mcp::Client> client;
    Reply reply;
    std::vector<json> seen;
    std::map<std::string, std::size_t> calls;
    boost::asio::steady_timer guard;
    bool guard_fired{false};
};

/// What a request failed with: its code and message, or code 0 when it succeeded.
struct Failure {
    int code{0};
    std::string message;
};

mcp::Task<Failure> failure_of(mcp::Client& client, std::string method, const json& params,
                              mcp::RequestOptions options = {}) {
    Failure failure;
    try {
        const std::optional<json> optional_params(std::in_place, params);
        auto pending = client.send_request(method, optional_params, options);
        co_await std::move(pending);
    } catch (const mcp::McpError& error) {
        failure.code = error.code();
        failure.message = error.error().message;
    }
    co_return failure;
}

json tool_params(const std::string& name = "ask") {
    return {{"name", name}, {"arguments", {{"x", 1}}}, {"_meta", {{"traceparent", "00-trace"}}}};
}

}  // namespace

// ---------------------------------------------------------------------------
// Scripted server
// ---------------------------------------------------------------------------

// Client Requirement 1: the requested inputs are built before the original request is retried,
// and the retry answers every entry under the key the server chose.
TEST(ClientMrtr, TwoRounds) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json&, std::size_t index) -> json {
        if (index == 1) {
            return input_required({{"first", elicit_entry("one")}}, "state-1");
        }
        if (index == 2) {
            return input_required({{"second", elicit_entry("two")}}, "state-2");
        }
        return complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const json arguments = {{"x", 1}};
        const auto result = co_await harness.client->call_tool("ask", arguments);
        EXPECT_EQ(result.content.size(), 1u);
    });

    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_FALSE(calls[0].at("params").contains("inputResponses"));
    EXPECT_FALSE(calls[0].at("params").contains("requestState"));
    EXPECT_EQ(calls[1].at("params").at("requestState"), "state-1");
    EXPECT_EQ(calls[1].at("params").at("inputResponses"),
              (json{{"first", {{"action", "accept"}, {"content", {{"name", "octocat:one"}}}}}}));
    EXPECT_EQ(calls[2].at("params").at("requestState"), "state-2");
    EXPECT_EQ(calls[2].at("params").at("inputResponses"),
              (json{{"second", {{"action", "accept"}, {"content", {{"name", "octocat:two"}}}}}}));
    EXPECT_EQ(handler_calls().size(), 2u);
}

// Client Requirement 2: the exact value of requestState comes back, whatever it contains.
TEST(ClientMrtr, StateEchoedByteForByte) {
    const std::string state = "  {\"a\":[1,2]} \xC3\xA9 \\n==\tpadding=  ";
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [&state](const json&, std::size_t index) -> json {
        return index == 1 ? input_required({{"k", elicit_entry("q")}}, state) : complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        const auto result = co_await harness.client->call_tool("ask", arguments);
        EXPECT_EQ(result.content.size(), 1u);
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_TRUE(calls[1].at("params").at("requestState").is_string());
    EXPECT_EQ(calls[1].at("params").at("requestState").get<std::string>(), state);
}

// Client Requirement 2: no requestState in the result means none in the retry, also when an
// earlier round had one.
TEST(ClientMrtr, NoStateMeansNoStateInRetry) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json&, std::size_t index) -> json {
        if (index == 1) {
            return input_required({{"a", elicit_entry("one")}}, "kept?");
        }
        if (index == 2) {
            return input_required({{"b", elicit_entry("two")}});
        }
        return complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        co_await harness.client->call_tool("ask", arguments);
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[1].at("params").at("requestState"), "kept?");
    EXPECT_FALSE(calls[2].at("params").contains("requestState")) << calls[2];
    EXPECT_TRUE(calls[2].at("params").contains("inputResponses"));
}

// An input_required result that only carries a requestState is retried at once, with no
// inputResponses.
TEST(ClientMrtr, StateOnlyResultIsRetried) {
    Harness harness;
    harness.reply = [](const json&, std::size_t index) -> json {
        return index == 1 ? input_required(nullptr, "only-state") : complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        co_await harness.client->call_tool("ask", arguments);
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].at("params").at("requestState"), "only-state");
    EXPECT_FALSE(calls[1].at("params").contains("inputResponses"));
}

// Client Requirement 3: the retry is a new request. Apart from the id, inputResponses and
// requestState it carries the original params, `_meta` included.
TEST(ClientMrtr, RetryUsesNewId) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json&, std::size_t index) -> json {
        if (index <= 2) {
            return input_required({{"k", elicit_entry("q")}}, "s" + std::to_string(index));
        }
        return complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const std::optional<json> params = tool_params();
        auto pending = harness.client->send_request("tools/call", params);
        const auto result = co_await std::move(pending);
        EXPECT_EQ(result.at("resultType"), "complete");
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 3u);
    std::set<std::string> ids;
    for (const auto& call : calls) {
        ids.insert(call.at("id").dump());
    }
    EXPECT_EQ(ids.size(), 3u);
    for (const auto& call : calls) {
        auto params = call.at("params");
        params.erase("inputResponses");
        params.erase("requestState");
        EXPECT_EQ(params.at("name"), "ask");
        EXPECT_EQ(params.at("arguments"), (json{{"x", 1}}));
        EXPECT_EQ(params.at("_meta").at("traceparent"), "00-trace");
        EXPECT_TRUE(params.at("_meta").contains("io.modelcontextprotocol/protocolVersion"));
    }
}

// A request the client has no handler for fails the call with the method named, and nothing is
// retried.
TEST(ClientMrtr, MissingHandlerErrors) {
    Harness harness;
    harness.client->on_request("roots/list", answer_roots);
    harness.reply = [](const json&, std::size_t) -> json {
        return input_required({{"roots", {{"method", "roots/list"}}}, {"ask", elicit_entry("q")}},
                              "state");
    };
    Failure failure;
    harness.run([&]() -> mcp::Task<void> {
        const json params = tool_params();
        failure = co_await failure_of(*harness.client, "tools/call", params);
    });
    EXPECT_NE(failure.code, 0);
    EXPECT_NE(failure.message.find("elicitation/create"), std::string::npos) << failure.message;
    EXPECT_EQ(harness.requests("tools/call").size(), 1u);
}

// A method outside elicitation/sampling/roots in inputRequests is refused without a handler call.
TEST(ClientMrtr, UnknownRequestedMethodErrors) {
    Harness harness;
    harness.client->on_request("ping", answer_roots);
    harness.reply = [](const json&, std::size_t) -> json {
        return input_required({{"p", {{"method", "ping"}}}}, "state");
    };
    Failure failure;
    harness.run([&]() -> mcp::Task<void> {
        const json params = tool_params();
        failure = co_await failure_of(*harness.client, "tools/call", params);
    });
    EXPECT_NE(failure.code, 0);
    EXPECT_NE(failure.message.find("ping"), std::string::npos) << failure.message;
    EXPECT_TRUE(handler_calls().empty());
    EXPECT_EQ(harness.requests("tools/call").size(), 1u);
}

// A server that keeps asking is given up on after RequestOptions::max_input_rounds retries.
TEST(ClientMrtr, MaxInputRoundsExceeded) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json&, std::size_t) -> json {
        return input_required({{"k", elicit_entry("again")}}, "s");
    };
    Failure custom;
    Failure by_default;
    std::size_t custom_requests = 0;
    harness.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions options;
        options.max_input_rounds = 3;
        const json params = tool_params();
        custom = co_await failure_of(*harness.client, "tools/call", params, options);
        custom_requests = harness.requests("tools/call").size();
        by_default = co_await failure_of(*harness.client, "tools/call", params);
    });
    EXPECT_NE(custom.code, 0);
    EXPECT_NE(custom.message.find("max_input_rounds"), std::string::npos) << custom.message;
    EXPECT_EQ(custom_requests, 4u);
    EXPECT_NE(by_default.code, 0);
    EXPECT_EQ(mcp::RequestOptions{}.max_input_rounds, 8);
    EXPECT_EQ(harness.requests("tools/call").size(), 4u + 9u);
}

// The timeout of the call ends the round in flight, and no retry follows it.
TEST(ClientMrtr, CancelDuringRound) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json&, std::size_t index) -> json {
        return index == 1 ? input_required({{"k", elicit_entry("q")}}, "s") : json(nullptr);
    };
    Failure failure;
    harness.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions options;
        options.timeout = std::chrono::milliseconds(200);
        const json params = tool_params();
        failure = co_await failure_of(*harness.client, "tools/call", params, options);
        EXPECT_EQ(harness.client->pending_request_count(), 0u);
    });
    EXPECT_EQ(failure.code, mcp::g_REQUEST_TIMEOUT);
    EXPECT_EQ(harness.requests("tools/call").size(), 2u);
}

// Closing the client while a round is in flight fails the call and sends nothing more.
TEST(ClientMrtr, CloseDuringRound) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [&harness](const json&, std::size_t index) -> json {
        if (index == 1) {
            return input_required({{"k", elicit_entry("q")}}, "s");
        }
        harness.client->close();
        return json(nullptr);
    };
    Failure failure;
    harness.run([&]() -> mcp::Task<void> {
        const json params = tool_params();
        failure = co_await failure_of(*harness.client, "tools/call", params);
    });
    EXPECT_NE(failure.code, 0);
    EXPECT_EQ(harness.requests("tools/call").size(), 2u);
}

// Nothing is cached: the same call made twice asks the server twice, and the second call does not
// start from the first one's state or answers.
TEST(ClientMrtr, NoCaching) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json& request, std::size_t index) -> json {
        const bool resumed = request.at("params").contains("requestState");
        if (!resumed && index % 2 == 1) {
            return input_required({{"k", elicit_entry("q")}}, "s");
        }
        return complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        co_await harness.client->call_tool("ask", arguments);
        co_await harness.client->call_tool("ask", arguments);
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 4u);
    EXPECT_FALSE(calls[0].at("params").contains("requestState"));
    EXPECT_TRUE(calls[1].at("params").contains("requestState"));
    EXPECT_FALSE(calls[2].at("params").contains("requestState"));
    EXPECT_FALSE(calls[2].at("params").contains("inputResponses"));
    EXPECT_TRUE(calls[3].at("params").contains("requestState"));
    EXPECT_EQ(handler_calls().size(), 2u);
}

// Two calls in flight at once each retry with their own requestState and answers, never the
// other's.
TEST(ClientMrtr, ParallelCallsKeepTheirOwnState) {
    Harness harness;
    // The handler suspends, so the other call's round runs while this one waits for its answer.
    harness.client->on_request("elicitation/create", [&harness](const json& params) -> mcp::Task<json> {
        boost::asio::steady_timer pause(harness.io);
        pause.expires_after(std::chrono::milliseconds(5));
        co_await pause.async_wait(boost::asio::use_awaitable);
        co_return co_await answer_elicitation(params);
    });
    std::optional<json> held;
    harness.reply = [&](const json& request, std::size_t /*index*/) -> json {
        const auto name = request.at("params").at("name").get<std::string>();
        if (request.at("params").contains("requestState")) {
            return complete_tool_result();
        }
        const auto question = input_required({{"k-" + name, elicit_entry(name)}}, "state-" + name);
        if (!held) {
            // Hold the first call's answer until the second call is on the wire too.
            held = response_to(request, question);
            return nullptr;
        }
        harness.transport->enqueue_message(held->dump());
        return question;
    };
    harness.run([&]() -> mcp::Task<void> {
        int finished = 0;
        // Names longer than the small-string buffer, as they live across suspensions.
        for (const std::string name : {"parallel-call-first", "parallel-call-second"}) {
            boost::asio::co_spawn(
                harness.io,
                [&harness, &finished, name]() -> mcp::Task<void> {
                    json arguments = json::object();
                    co_await harness.client->call_tool(name, arguments);
                    ++finished;
                },
                boost::asio::detached);
        }
        boost::asio::steady_timer tick(harness.io);
        while (finished < 2) {
            tick.expires_after(std::chrono::milliseconds(1));
            co_await tick.async_wait(boost::asio::use_awaitable);
        }
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 4u);
    for (const auto& call : calls) {
        const auto& params = call.at("params");
        if (!params.contains("requestState")) {
            continue;
        }
        const auto name = params.at("name").get<std::string>();
        EXPECT_EQ(params.at("requestState"), "state-" + name) << call;
        ASSERT_TRUE(params.contains("inputResponses")) << call;
        EXPECT_EQ(params.at("inputResponses").size(), 1u) << call;
        EXPECT_TRUE(params.at("inputResponses").contains("k-" + name)) << call;
    }
}

// prompts/get and resources/read resume like tools/call, keeping their own params.
TEST(ClientMrtr, PromptsGetAndResourcesReadResume) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json& request, std::size_t index) -> json {
        const auto method = request.at("method").get<std::string>();
        if (index == 1) {
            return input_required({{"k", elicit_entry(method)}}, "state-" + method);
        }
        if (method == "prompts/get") {
            return {{"resultType", "complete"}, {"messages", json::array()}};
        }
        return {{"resultType", "complete"}, {"contents", json::array()}};
    };
    harness.run([&]() -> mcp::Task<void> {
        std::map<std::string, std::string> arguments = {{"who", "me"}};
        co_await harness.client->get_prompt("greet", arguments);
        const std::string uri = "test://doc";
        co_await harness.client->read_resource(uri);
    });
    const auto prompts = harness.requests("prompts/get");
    const auto resources = harness.requests("resources/read");
    ASSERT_EQ(prompts.size(), 2u);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(prompts[1].at("params").at("name"), "greet");
    EXPECT_EQ(prompts[1].at("params").at("arguments"), (json{{"who", "me"}}));
    EXPECT_EQ(prompts[1].at("params").at("requestState"), "state-prompts/get");
    EXPECT_EQ(prompts[1].at("params").at("inputResponses").at("k").at("content").at("name"),
              "octocat:prompts/get");
    EXPECT_EQ(resources[1].at("params").at("uri"), "test://doc");
    EXPECT_EQ(resources[1].at("params").at("requestState"), "state-resources/read");
}

// Sampling and roots are answered like elicitation, several entries in one round.
TEST(ClientMrtr, SamplingAndRootsAnswered) {
    Harness harness;
    register_handlers(*harness.client);
    const json sampling = {
        {"method", "sampling/createMessage"},
        {"params",
         {{"messages",
           json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", "?"}}}}})},
          {"maxTokens", 10}}}};
    const json roots = {{"method", "roots/list"}};
    harness.reply = [&](const json&, std::size_t index) -> json {
        if (index == 1) {
            return input_required({{"sample", sampling}, {"roots", roots}, {"form", elicit_entry("f")}},
                                  "s");
        }
        return complete_tool_result();
    };
    harness.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        co_await harness.client->call_tool("ask", arguments);
    });
    const auto calls = harness.requests("tools/call");
    ASSERT_EQ(calls.size(), 2u);
    const auto& responses = calls[1].at("params").at("inputResponses");
    EXPECT_EQ(responses.size(), 3u);
    EXPECT_EQ(responses.at("sample").at("model"), "test-model");
    EXPECT_EQ(responses.at("roots").at("roots").size(), 1u);
    EXPECT_EQ(responses.at("form").at("action"), "accept");
    ASSERT_EQ(handler_calls().size(), 3u);
    int sampling_calls = 0;
    for (const auto& call : handler_calls()) {
        if (call.contains("maxTokens")) {
            EXPECT_EQ(call.at("maxTokens"), 10);
            ++sampling_calls;
        }
    }
    EXPECT_EQ(sampling_calls, 1);
}

// input_required is valid on three methods only; elsewhere it is as unusable as any other
// unknown resultType.
TEST(ClientMrtr, InputRequiredOnOtherMethodRejected) {
    Harness harness;
    register_handlers(*harness.client);
    harness.reply = [](const json&, std::size_t) -> json {
        return input_required({{"k", elicit_entry("q")}}, "s");
    };
    Failure listing;
    harness.run([&]() -> mcp::Task<void> {
        const json params = json::object();
        listing = co_await failure_of(*harness.client, "tools/list", params);
    });
    EXPECT_EQ(listing.code, mcp::g_INVALID_REQUEST);
    EXPECT_NE(listing.message.find("input_required"), std::string::npos) << listing.message;
    EXPECT_EQ(harness.requests("tools/list").size(), 1u);
    EXPECT_TRUE(handler_calls().empty());
}

// An input_required result with neither inputRequests nor requestState cannot be answered.
TEST(ClientMrtr, EmptyInputRequiredRejected) {
    Harness harness;
    harness.reply = [](const json&, std::size_t) -> json { return input_required(nullptr); };
    Failure failure;
    harness.run([&]() -> mcp::Task<void> {
        const json params = tool_params();
        failure = co_await failure_of(*harness.client, "tools/call", params);
    });
    EXPECT_EQ(failure.code, mcp::g_INVALID_REQUEST);
    EXPECT_EQ(harness.requests("tools/call").size(), 1u);
}

// ---------------------------------------------------------------------------
// The SDK client against the SDK server
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<mcp::Server> make_asking_server() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"asking", "1.0"}, capabilities);
    server->add_tool<json, json>(
        "ask", "Asks for a name", json{{"type", "object"}},
        [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            mcp::ElicitRequestFormParams params;
            params.message = "Your name?";
            params.requestedSchema = {{"type", "object"},
                                      {"properties", {{"name", {{"type", "string"}}}}}};
            const auto answer = co_await ctx.elicit(params);
            co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
        });
    return server;
}

mcp::Task<json> ask_the_tool(mcp::Client& client) {
    co_await client.connect("mrtr-client", "1.0");
    const auto result = co_await client.call_tool("ask", json::object());
    co_return *result.structuredContent;
}

mcp::Task<mcp::ElicitResult> accept_name(const mcp::ElicitRequestParams&) {
    mcp::ElicitResult accepted;
    accepted.action = mcp::ElicitAction::eAccept;
    accepted.content = json{{"name", "octocat"}};
    co_return accepted;
}

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

}  // namespace

TEST(ClientMrtr, RoundTripAgainstSdkServerMemory) {
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    auto [client_transport, server_transport] = mcp::create_memory_transport_pair(io.get_executor());
    auto server = make_asking_server();
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Version("2026-07-28");
    mcp::Client client(client_transport, io.get_executor(), options);
    client.on_elicitation(accept_name);
    json answer;
    std::string error;

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
        try {
            answer = co_await ask_the_tool(client);
        } catch (const std::exception& caught) {
            error = caught.what();
        }
        client.close();
        server_transport->close();
    });
    EXPECT_EQ(error, "");
    EXPECT_EQ(answer, (json{{"name", "octocat"}}));
}

TEST(ClientMrtr, RoundTripAgainstSdkServerStdio) {
    PipeBuffer to_server;
    PipeBuffer to_client;
    PipeWriter server_out(to_client);
    PipeWriter client_out(to_server);
    std::istream server_in(&to_server);
    std::ostream server_output(&server_out);
    std::istream client_in(&to_client);
    std::ostream client_output(&client_out);

    auto server = make_asking_server();
    std::thread server_thread([&] { server->run_stdio(server_in, server_output); });

    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    auto transport = std::make_shared<mcp::StdioTransport>(io.get_executor(), client_in, client_output);
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Version("2026-07-28");
    mcp::Client client(transport, io.get_executor(), options);
    client.on_elicitation(accept_name);
    json answer;
    std::string error;

    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        try {
            answer = co_await ask_the_tool(client);
        } catch (const std::exception& caught) {
            error = caught.what();
        }
        client.close();
    });

    to_server.close();
    server_thread.join();
    to_client.close();

    EXPECT_EQ(error, "");
    EXPECT_EQ(answer, (json{{"name", "octocat"}}));
}
