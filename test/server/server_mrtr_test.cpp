#include "../test_utils.hpp"

#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using nlohmann::json;

/// Waits, a millisecond at a time, until @p condition holds. Gives up after 30 s so a broken
/// hand-off fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// Counts the live instances, so a test can tell that a parked handler's frame was released.
struct Probe {
    explicit Probe(std::shared_ptr<std::atomic<int>> live) : live_(std::move(live)) {
        live_->fetch_add(1);
    }
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe() { live_->fetch_sub(1); }

   private:
    std::shared_ptr<std::atomic<int>> live_;
};

json form_elicitation(const std::string& message) {
    mcp::ElicitRequestFormParams params;
    params.message = message;
    params.requestedSchema = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};
    return params;
}

/// The capabilities of a client that answers form elicitations.
json form_client() { return json::object({{"elicitation", json::object()}}); }

/// A 2026-07-28 request from a client that declares @p capabilities.
json modern(const json& id, std::string_view method, json params, json capabilities = form_client()) {
    auto request = make_modern_request(id, method, std::move(params));
    request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = std::move(capabilities);
    return request;
}

json call_tool(const json& id, std::string_view tool, json arguments = json::object(),
               json capabilities = form_client()) {
    return modern(id, "tools/call", {{"name", tool}, {"arguments", std::move(arguments)}},
                  std::move(capabilities));
}

/// @p request retried with @p state and @p responses under a new id.
json retry(json request, const json& id, const json& state, json responses = json::object()) {
    request["id"] = id;
    request["params"]["requestState"] = state;
    if (!responses.empty()) {
        request["params"]["inputResponses"] = std::move(responses);
    }
    return request;
}

/// An inputResponses object holding @p response under @p key.
json answer(const std::string& key, json response) {
    json responses = json::object();
    responses[key] = std::move(response);
    return responses;
}

/// A capabilities object declaring @p name.
json declaring(const std::string& name, json settings = json::object()) {
    json capabilities = json::object();
    capabilities[name] = std::move(settings);
    return capabilities;
}

json accept(const std::string& name) { return {{"action", "accept"}, {"content", {{"name", name}}}}; }

/// The only key of an InputRequiredResult's inputRequests.
std::string only_key(const json& response) {
    const auto& requests = response["result"]["inputRequests"];
    EXPECT_EQ(requests.size(), 1u) << response;
    return requests.empty() ? std::string{} : requests.begin().key();
}

class MrtrTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::unique_ptr<mcp::Server> server_ = make_server();
    std::shared_ptr<std::atomic<int>> live_probes_ = std::make_shared<std::atomic<int>>(0);
    std::atomic<int> middleware_runs_{0};
    std::atomic<bool> saw_cancel_{false};
    std::atomic<int> abandoned_caught_{0};

    std::unique_ptr<mcp::Server> make_server() {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        caps.prompts = mcp::ServerCapabilities::PromptsCapability{};
        caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{"mrtr", "1.0"}, caps);
        server->add_tool<json, json>(
            "ask", "Asks for a name", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const Probe probe(live_probes_);
                const auto answer = co_await ctx.elicit(form_elicitation("Your name?"));
                co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
            });
        server->add_tool<json, json>(
            "ask_twice", "Asks two questions, one after the other", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const auto first = co_await ctx.elicit(form_elicitation("First?"));
                const auto second = co_await ctx.elicit(form_elicitation("Second?"));
                co_return json{{"first", first.content->value("name", "")},
                               {"second", second.content->value("name", "")}};
            });
        server->add_tool<json, json>("ask_url", "Sends the user to a URL", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         mcp::ElicitRequestURLParams params;
                                         params.message = "Sign in";
                                         params.url = "https://example.com/sign-in";
                                         const auto answer = co_await ctx.elicit(params);
                                         co_return json{{"action", answer.action}};
                                     });
        server->add_tool<json, json>(
            "ask_then_wait", "Asks, then waits for cancellation", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                co_await ctx.elicit(form_elicitation("Name?"));
                saw_cancel_ = co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                co_return json{{"cancelled", saw_cancel_.load()}};
            });
        server->add_tool<json, json>("ask_then_progress", "Asks, then reports progress",
                                     json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         co_await ctx.elicit(form_elicitation("Name?"));
                                         co_await ctx.report_progress(1, 2);
                                         co_return json{{"done", true}};
                                     });
        server->add_tool<json, json>("ask_catching", "Swallows the abandonment of its question",
                                     json{{"type", "object"}},
                                     [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         try {
                                             co_await ctx.elicit(form_elicitation("Name?"));
                                         } catch (const std::exception&) {
                                             abandoned_caught_.fetch_add(1);
                                         }
                                         co_return json{{"finished", true}};
                                     });
        server->add_tool<json, json>(
            "ask_parallel", "Asks two questions at once", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                auto executor = co_await asio::this_coro::executor;
                auto sibling = std::make_shared<std::optional<mcp::ElicitResult>>();
                asio::co_spawn(
                    executor,
                    [](mcp::Context* context,
                       std::shared_ptr<std::optional<mcp::ElicitResult>> out) -> mcp::Task<void> {
                        *out = co_await context->elicit(form_elicitation("B?"));
                    }(&ctx, sibling),
                    asio::detached);
                const auto first = co_await ctx.elicit(form_elicitation("A?"));
                const auto answered = [sibling] { return sibling->has_value(); };
                co_await wait_until(answered);
                co_return json{{"a", first.content->value("name", "")},
                               {"b", (*sibling)->content->value("name", "")}};
            });
        server->add_tool<json, json>("sample", "Asks the client's model", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         mcp::CreateMessageRequestParams params;
                                         params.maxTokens = 10;
                                         const auto result = co_await ctx.sample_llm(params);
                                         co_return json{{"model", result.model}};
                                     });
        server->add_tool<json, json>("sample_with_tools", "Asks the client's model, offering a tool",
                                     json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         mcp::CreateMessageRequestParams params;
                                         params.maxTokens = 10;
                                         mcp::Tool tool;
                                         tool.name = "lookup";
                                         tool.inputSchema = json{{"type", "object"}};
                                         params.tools = std::vector<mcp::Tool>{tool};
                                         const auto result = co_await ctx.sample_llm(params);
                                         co_return json{{"model", result.model}};
                                     });
        server->add_tool<json, json>("roots", "Lists the client's roots", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         const auto result = co_await ctx.request_roots();
                                         co_return json{{"roots", result.roots.size()}};
                                     });
        mcp::Prompt prompt;
        prompt.name = "greet";
        server->add_prompt<json, json>(prompt, [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            const auto answer = co_await ctx.elicit(form_elicitation("Whom to greet?"));
            const auto name = answer.content->value("name", "");
            co_return json{
                {"messages", json::array({{{"role", "user"},
                                           {"content", {{"type", "text"}, {"text", "Hi " + name}}}}})}};
        });
        server->use([this](mcp::Context& ctx, const json& params,
                           mcp::TypeErasedHandler next) -> mcp::Task<json> {
            middleware_runs_.fetch_add(1);
            co_return co_await next(ctx, params);
        });
        return server;
    }

    /// Runs @p body on the io_context and stops it when the body returns, leaving handlers that
    /// are still parked suspended.
    template <typename Body>
    void run(Body body) {
        asio::co_spawn(
            io_ctx_,
            [this, &body]() -> mcp::Task<void> {
                co_await body();
                io_ctx_.stop();
            },
            asio::detached);
        io_ctx_.run();
    }

    mcp::Task<json> send(json request, std::string principal = std::string()) {
        mcp::RequestContext context;
        context.principal = std::move(principal);
        co_return co_await send_with(server_.get(), std::move(request), std::move(context));
    }

    static mcp::Task<json> send_with(mcp::Server* server, json request, mcp::RequestContext context) {
        const auto wire =
            co_await server->dispatch_request_direct(std::move(request), std::move(context));
        co_return json::parse(wire);
    }

    static void expect_input_required(const json& response) {
        ASSERT_TRUE(response.contains("result")) << response;
        EXPECT_EQ(response["result"]["resultType"], "input_required") << response;
        ASSERT_TRUE(response["result"]["requestState"].is_string()) << response;
        ASSERT_TRUE(response["result"]["inputRequests"].is_object()) << response;
        EXPECT_FALSE(response["result"]["inputRequests"].empty()) << response;
    }

    static void expect_unknown_state(const json& response) {
        ASSERT_TRUE(response.contains("error")) << response;
        EXPECT_EQ(response["error"]["code"], mcp::g_INVALID_PARAMS) << response;
        EXPECT_EQ(response["error"]["message"], "Unknown or expired requestState") << response;
    }
};

TEST_F(MrtrTest, RoundTripNewIdOnRetry) {
    json first;
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        first = co_await send(request);
        const auto key = first["result"]["inputRequests"].begin().key();
        second = co_await send(
            retry(request, 2, first["result"]["requestState"], answer(key, accept("octocat"))));
    });

    expect_input_required(first);
    EXPECT_EQ(first["id"], 1);
    const auto& input = first["result"]["inputRequests"].begin().value();
    EXPECT_EQ(input["method"], "elicitation/create") << first;
    EXPECT_EQ(input["params"]["message"], "Your name?") << first;
    ASSERT_TRUE(second.contains("result")) << second;
    EXPECT_EQ(second["id"], 2);
    EXPECT_EQ(second["result"]["resultType"], "complete") << second;
    EXPECT_EQ(second["result"]["structuredContent"]["name"], "octocat") << second;
    const auto stats = server_->mrtr_stats();
    EXPECT_EQ(stats.parked, 0u);
    EXPECT_EQ(stats.resume_hit, 1u);
}

TEST_F(MrtrTest, RetryReusingSameIdIsHandled) {
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        second = co_await send(retry(request, 1, first["result"]["requestState"],
                                     answer(only_key(first), accept("same-id"))));
    });

    EXPECT_EQ(second["id"], 1);
    EXPECT_EQ(second["result"]["structuredContent"]["name"], "same-id") << second;
}

TEST_F(MrtrTest, TwoRounds) {
    json second;
    json third;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask_twice");
        const auto first = co_await send(request);
        second = co_await send(
            retry(request, 2, first["result"]["requestState"], answer(only_key(first), accept("one"))));
        third = co_await send(retry(request, 3, second["result"]["requestState"],
                                    answer(only_key(second), accept("two"))));
    });

    expect_input_required(second);
    EXPECT_EQ(second["result"]["inputRequests"].begin().value()["params"]["message"], "Second?");
    EXPECT_EQ(third["result"]["structuredContent"], (json{{"first", "one"}, {"second", "two"}}))
        << third;
    EXPECT_EQ(middleware_runs_.load(), 1);
}

TEST_F(MrtrTest, PromptsGetRoundTrip) {
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = modern(1, "prompts/get", {{"name", "greet"}});
        const auto first = co_await send(request);
        second = co_await send(
            retry(request, 2, first["result"]["requestState"], answer(only_key(first), accept("Ada"))));
    });

    ASSERT_TRUE(second.contains("result")) << second;
    EXPECT_EQ(second["result"]["messages"][0]["content"]["text"], "Hi Ada") << second;
}

TEST_F(MrtrTest, AlteredStateIsUnknown) {
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        auto state = first["result"]["requestState"].get<std::string>();
        state.back() = state.back() == 'a' ? 'b' : 'a';
        second = co_await send(retry(request, 2, state, answer(only_key(first), accept("x"))));
    });

    expect_unknown_state(second);
    EXPECT_EQ(server_->mrtr_stats().resume_unknown, 1u);
}

TEST_F(MrtrTest, RetryWithAlteredParamsRejected) {
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        auto altered = request;
        altered["params"]["arguments"]["extra"] = true;
        second = co_await send(
            retry(altered, 2, first["result"]["requestState"], answer(only_key(first), accept("x"))));
    });

    expect_unknown_state(second);
    EXPECT_EQ(server_->mrtr_stats().resume_params_mismatch, 1u);
}

TEST_F(MrtrTest, RetryForAnotherMethodRejected) {
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = modern(1, "prompts/get", {{"name", "greet"}});
        const auto first = co_await send(request);
        auto other = modern(2, "resources/read", {{"uri", "test://none"}});
        second = co_await send(retry(other, 2, first["result"]["requestState"]));
    });

    expect_unknown_state(second);
}

TEST_F(MrtrTest, CrossPrincipalResumeByteIdenticalToUnknownToken) {
    json cross;
    json unknown;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request, "alice");
        cross = co_await send(
            retry(request, 2, first["result"]["requestState"], answer(only_key(first), accept("x"))),
            "bob");
        unknown = co_await send(retry(request, 2, "not-a-state", answer(only_key(first), accept("x"))),
                                "bob");
    });

    expect_unknown_state(cross);
    EXPECT_EQ(cross.dump(), unknown.dump());
    EXPECT_EQ(server_->mrtr_stats().resume_principal_mismatch, 1u);
}

TEST_F(MrtrTest, SamePrincipalResumes) {
    json second;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request, "alice");
        second = co_await send(
            retry(request, 2, first["result"]["requestState"], answer(only_key(first), accept("a"))),
            "alice");
    });

    EXPECT_EQ(second["result"]["structuredContent"]["name"], "a") << second;
}

TEST_F(MrtrTest, ResumeSingleUse) {
    json replayed;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        const auto state = first["result"]["requestState"];
        co_await send(retry(request, 2, state, answer(only_key(first), accept("once"))));
        replayed = co_await send(retry(request, 3, state, answer(only_key(first), accept("twice"))));
    });

    expect_unknown_state(replayed);
}

TEST_F(MrtrTest, PartialInputResponsesRerequestMissing) {
    json second;
    json third;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        second = co_await send(retry(request, 2, first["result"]["requestState"]));
        third = co_await send(retry(request, 3, second["result"]["requestState"],
                                    answer(only_key(second), accept("late"))));
    });

    expect_input_required(second);
    EXPECT_NE(second["result"]["requestState"], json(nullptr));
    EXPECT_EQ(third["result"]["structuredContent"]["name"], "late") << third;
}

TEST_F(MrtrTest, ParallelElicitationsShareOneRoundUnderUniqueKeys) {
    json last;
    std::size_t first_round = 0;
    std::set<std::string> keys;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask_parallel");
        auto response = co_await send(request);
        first_round = response["result"]["inputRequests"].size();
        int id = 2;
        while (response.contains("result") && response["result"]["resultType"] == "input_required") {
            json answers = json::object();
            for (const auto& [key, input] : response["result"]["inputRequests"].items()) {
                EXPECT_TRUE(keys.insert(key).second) << key;
                answers[key] = accept(input["params"]["message"].get<std::string>());
            }
            response = co_await send(retry(request, id++, response["result"]["requestState"], answers));
        }
        last = response;
    });

    // Asked in the same turn of the handler's strand, both questions share the first round.
    EXPECT_EQ(first_round, 2u);
    EXPECT_EQ(keys.size(), 2u);
    EXPECT_EQ(last["result"]["structuredContent"], (json{{"a", "A?"}, {"b", "B?"}})) << last;
}

TEST_F(MrtrTest, UndeclaredCapabilityIs32021WithData) {
    json response;
    run([&]() -> mcp::Task<void> {
        response = co_await send(call_tool(1, "ask", json::object(), json::object()));
    });

    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY);
    EXPECT_EQ(response["error"]["data"]["requiredCapabilities"],
              (json{{"elicitation", {{"form", json::object()}}}}))
        << response;
    EXPECT_EQ(server_->mrtr_stats().parked, 0u);
}

TEST_F(MrtrTest, UrlModeNeedsTheUrlCapabilityAndCarriesNoElicitationId) {
    json refused;
    json asked;
    run([&]() -> mcp::Task<void> {
        refused = co_await send(call_tool(1, "ask_url"));
        asked = co_await send(
            call_tool(2, "ask_url", json::object(), declaring("elicitation", declaring("url"))));
    });

    EXPECT_EQ(refused["error"]["code"], mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY) << refused;
    EXPECT_EQ(refused["error"]["data"]["requiredCapabilities"],
              (json{{"elicitation", {{"url", json::object()}}}}));
    expect_input_required(asked);
    const auto& params = asked["result"]["inputRequests"].begin().value()["params"];
    EXPECT_EQ(params["mode"], "url");
    EXPECT_FALSE(params.contains("elicitationId")) << asked;
}

TEST_F(MrtrTest, SamplingAndRootsNeedTheirCapabilities) {
    json sampling_refused;
    json roots_refused;
    json sampling_asked;
    json roots_asked;
    run([&]() -> mcp::Task<void> {
        sampling_refused = co_await send(call_tool(1, "sample"));
        roots_refused = co_await send(call_tool(2, "roots"));
        sampling_asked = co_await send(call_tool(3, "sample", json::object(), declaring("sampling")));
        roots_asked = co_await send(call_tool(4, "roots", json::object(), declaring("roots")));
    });

    EXPECT_EQ(sampling_refused["error"]["data"]["requiredCapabilities"],
              (json{{"sampling", json::object()}}));
    EXPECT_EQ(roots_refused["error"]["data"]["requiredCapabilities"],
              (json{{"roots", json::object()}}));
    expect_input_required(sampling_asked);
    EXPECT_EQ(sampling_asked["result"]["inputRequests"].begin().value()["method"],
              "sampling/createMessage");
    expect_input_required(roots_asked);
    EXPECT_EQ(roots_asked["result"]["inputRequests"].begin().value()["method"], "roots/list");
}

TEST_F(MrtrTest, ToolSamplingNeedsTheSamplingToolsCapability) {
    const json sampling_only = declaring("sampling");
    const json sampling_with_tools = declaring("sampling", json{{"tools", json::object()}});
    json refused;
    json asked;
    run([&]() -> mcp::Task<void> {
        refused = co_await send(call_tool(1, "sample_with_tools", json::object(), sampling_only));
        asked = co_await send(call_tool(2, "sample_with_tools", json::object(), sampling_with_tools));
    });

    EXPECT_EQ(refused.value("error", json::object()).value("code", 0),
              mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY)
        << refused;
    EXPECT_EQ(refused["error"]["data"]["requiredCapabilities"],
              (json{{"sampling", {{"tools", json::object()}}}}));
    expect_input_required(asked);
    EXPECT_EQ(asked["result"]["inputRequests"].begin().value()["params"]["tools"][0]["name"], "lookup")
        << asked;
}

TEST_F(MrtrTest, ExpiredToken) {
    mcp::MrtrOptions options;
    options.ttl = std::chrono::milliseconds(1);
    server_->set_mrtr_options(options);
    json second;
    bool evicted = false;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        evicted = co_await wait_until([this] { return server_->mrtr_stats().evicted_ttl == 1; });
        second = co_await send(retry(request, 2, first["result"]["requestState"],
                                     answer(only_key(first), accept("late"))));
    });

    EXPECT_TRUE(evicted);
    expect_unknown_state(second);
    EXPECT_EQ(server_->mrtr_stats().parked, 0u);
}

TEST_F(MrtrTest, GlobalAndPerPrincipalCaps) {
    mcp::MrtrOptions options;
    options.max_parked = 3;
    options.max_parked_per_principal = 2;
    std::vector<mcp::MrtrEvictionReason> evictions;
    options.on_eviction = [&evictions](mcp::MrtrEvictionReason reason) { evictions.push_back(reason); };
    server_->set_mrtr_options(options);
    std::vector<json> alice;
    json alice_oldest_retry;
    json bob_retry;
    run([&]() -> mcp::Task<void> {
        for (int id = 1; id <= 3; ++id) {
            alice.push_back(co_await send(call_tool(id, "ask"), "alice"));
        }
        // Alice's third park evicts her first; Bob's second fills the server past three, evicting
        // Alice's second. Bob's retry then takes his first, leaving Alice's third and Bob's second.
        const auto bob_first = co_await send(call_tool(10, "ask"), "bob");
        co_await send(call_tool(11, "ask"), "bob");
        alice_oldest_retry =
            co_await send(retry(call_tool(1, "ask"), 20, alice[0]["result"]["requestState"],
                                answer(only_key(alice[0]), accept("x"))),
                          "alice");
        bob_retry = co_await send(retry(call_tool(10, "ask"), 21, bob_first["result"]["requestState"],
                                        answer(only_key(bob_first), accept("bob"))),
                                  "bob");
    });

    expect_unknown_state(alice_oldest_retry);
    EXPECT_EQ(bob_retry["result"]["structuredContent"]["name"], "bob") << bob_retry;
    const auto stats = server_->mrtr_stats();
    EXPECT_EQ(stats.evicted_cap, 2u);
    EXPECT_EQ(evictions, (std::vector<mcp::MrtrEvictionReason>{mcp::MrtrEvictionReason::eCapacity,
                                                               mcp::MrtrEvictionReason::eCapacity}));
    EXPECT_EQ(stats.parked, 2u);
}

TEST_F(MrtrTest, CancelOfRetryCancelsParkedHandler) {
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask_then_wait");
        const auto first = co_await send(request);
        mcp::RequestContext context;
        context.cancelled = std::make_shared<std::atomic<bool>>(false);
        auto flag = context.cancelled;
        auto executor = co_await asio::this_coro::executor;
        auto done = std::make_shared<std::atomic<bool>>(false);
        asio::co_spawn(executor,
                       send_with(server_.get(),
                                 retry(request, 2, first["result"]["requestState"],
                                       answer(only_key(first), accept("x"))),
                                 std::move(context)),
                       [done](std::exception_ptr, json) { done->store(true); });
        flag->store(true);
        const auto finished = [done] { return done->load(); };
        co_await wait_until(finished);
    });

    EXPECT_TRUE(saw_cancel_.load());
}

TEST_F(MrtrTest, HandlerSeesRetryStreamForProgress) {
    std::vector<json> notifications;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask_then_progress");
        const auto first = co_await send(request);
        auto again =
            retry(request, 2, first["result"]["requestState"], answer(only_key(first), accept("x")));
        again["params"]["_meta"]["progressToken"] = "retry-token";
        mcp::RequestContext context;
        context.notifications = [&notifications](std::string_view message) -> mcp::Task<void> {
            notifications.push_back(json::parse(message));
            co_return;
        };
        co_await send_with(server_.get(), std::move(again), std::move(context));
    });

    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_EQ(notifications[0]["method"], "notifications/progress");
    EXPECT_EQ(notifications[0]["params"]["progressToken"], "retry-token");
}

TEST_F(MrtrTest, HandlerCatchingAbandonmentIsDiscardedAndCounted) {
    mcp::MrtrOptions options;
    options.ttl = std::chrono::milliseconds(1);
    server_->set_mrtr_options(options);
    bool counted = false;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await send(call_tool(1, "ask_catching"));
        expect_input_required(first);
        counted =
            co_await wait_until([this] { return server_->mrtr_stats().abandoned_completed == 1; });
    });

    EXPECT_TRUE(counted);
    EXPECT_EQ(abandoned_caught_.load(), 1);
}

TEST_F(MrtrTest, MultiInstanceRetryGetsTypedUnknown) {
    auto other = make_server();
    json elsewhere;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        const auto first = co_await send(request);
        mcp::RequestContext context;
        elsewhere = co_await send_with(
            other.get(),
            retry(request, 2, first["result"]["requestState"], answer(only_key(first), accept("x"))),
            std::move(context));
    });

    expect_unknown_state(elsewhere);
}

constexpr int g_parks = 20;

TEST_F(MrtrTest, EvictionReleasesFrames) {
    mcp::MrtrOptions options;
    options.ttl = std::chrono::milliseconds(20);
    server_->set_mrtr_options(options);
    bool released = false;
    run([&]() -> mcp::Task<void> {
        for (int id = 0; id < g_parks; ++id) {
            co_await send(call_tool(id, "ask"), "principal-" + std::to_string(id % 5));
        }
        released = co_await wait_until([this] {
            return server_->mrtr_stats().evicted_ttl == g_parks && live_probes_->load() == 0;
        });
    });

    EXPECT_TRUE(released);
    EXPECT_EQ(server_->mrtr_stats().parked, 0u);
    EXPECT_EQ(live_probes_->load(), 0);
}

TEST_F(MrtrTest, ChurnNoLeak) {
    bool clean = true;
    run([&]() -> mcp::Task<void> {
        for (int cycle = 0; cycle < 200; ++cycle) {
            const auto request = call_tool(cycle, "ask");
            const auto first = co_await send(request);
            const auto second =
                co_await send(retry(request, 1000 + cycle, first["result"]["requestState"],
                                    answer(only_key(first), accept("c"))));
            clean = clean && second["result"]["structuredContent"]["name"] == "c";
            if (cycle % 50 == 49) {
                const bool idle = co_await wait_until([this] { return live_probes_->load() == 0; });
                clean = clean && idle && server_->mrtr_stats().parked == 0;
            }
        }
    });

    EXPECT_TRUE(clean);
    EXPECT_EQ(server_->mrtr_stats().resume_hit, 200u);
}

TEST_F(MrtrTest, ServerDestructionWithParkedEntries) {
    bool released = false;
    run([&]() -> mcp::Task<void> {
        co_await send(call_tool(1, "ask"));
        co_await send(call_tool(2, "ask"));
        server_.reset();
        released = co_await wait_until([this] { return live_probes_->load() == 0; });
    });

    EXPECT_TRUE(released);
}

TEST_F(MrtrTest, ParkedRequestDoesNotBlockSecondRequestOnASession) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    std::vector<json> written;
    raw_transport->set_on_write([&written, raw_transport](std::string_view message) {
        written.push_back(json::parse(message));
        if (written.size() == 2) {
            raw_transport->close();
        }
    });
    raw_transport->enqueue_message(call_tool(1, "ask").dump());
    raw_transport->enqueue_message(make_modern_request(2, "tools/list").dump());
    asio::steady_timer guard(io_ctx_, std::chrono::seconds(30));
    guard.async_wait([raw_transport](const boost::system::error_code& error) {
        if (!error) {
            raw_transport->close();
        }
    });
    asio::co_spawn(
        io_ctx_,
        [this, transport, &guard]() -> mcp::Task<void> {
            co_await server_->run(transport, io_ctx_.get_executor());
            guard.cancel();
            io_ctx_.stop();
        },
        asio::detached);
    io_ctx_.run();

    ASSERT_EQ(written.size(), 2u);
    std::set<int> ids;
    for (const auto& message : written) {
        ids.insert(message["id"].get<int>());
    }
    EXPECT_EQ(ids, (std::set<int>{1, 2}));
}

TEST_F(MrtrTest, ModernSessionNeverWritesARequest) {
    json everything = json::object();
    everything["elicitation"] = {{"form", json::object()}, {"url", json::object()}};
    everything["sampling"] = json::object();
    everything["roots"] = json::object();
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    std::vector<json> written;
    raw_transport->set_on_write([&written, raw_transport](std::string_view message) {
        written.push_back(json::parse(message));
        if (written.size() == 4) {
            raw_transport->close();
        }
    });
    int id = 0;
    for (const auto* tool : {"ask", "ask_url", "sample", "roots"}) {
        raw_transport->enqueue_message(call_tool(++id, tool, json::object(), everything).dump());
    }
    asio::steady_timer guard(io_ctx_, std::chrono::seconds(30));
    guard.async_wait([raw_transport](const boost::system::error_code& error) {
        if (!error) {
            raw_transport->close();
        }
    });
    asio::co_spawn(
        io_ctx_,
        [this, transport, &guard]() -> mcp::Task<void> {
            co_await server_->run(transport, io_ctx_.get_executor());
            guard.cancel();
            io_ctx_.stop();
        },
        asio::detached);
    io_ctx_.run();

    // Every question goes out inside an input_required result; nothing is sent as a request.
    ASSERT_EQ(written.size(), 4u);
    for (const auto& message : written) {
        EXPECT_FALSE(message.contains("method")) << message;
        EXPECT_EQ(message.value("result", json::object()).value("resultType", ""), "input_required")
            << message;
    }
}

TEST_F(MrtrTest, SessionEndGivesUpParkedHandlers) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    raw_transport->set_on_write(
        [raw_transport](std::string_view /*message*/) { raw_transport->close(); });
    raw_transport->enqueue_message(call_tool(1, "ask").dump());
    bool released = false;
    asio::co_spawn(
        io_ctx_,
        [this, transport, &released]() -> mcp::Task<void> {
            co_await server_->run(transport, io_ctx_.get_executor());
            released = co_await wait_until([this] { return live_probes_->load() == 0; });
        },
        asio::detached);
    // Without the handler given up, its wait would keep this running until the default timeout.
    io_ctx_.run();

    EXPECT_TRUE(released);
    EXPECT_EQ(server_->mrtr_stats().parked, 0u);
}

TEST_F(MrtrTest, AbandonWaitingHandlersUnwindsThem) {
    bool released = false;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await send(call_tool(1, "ask"));
        EXPECT_EQ(first["result"]["resultType"], "input_required") << first;
        EXPECT_EQ(live_probes_->load(), 1);
        server_->abandon_waiting_handlers();
        released = co_await wait_until([this] { return live_probes_->load() == 0; });
        const auto retried =
            co_await send(retry(call_tool(1, "ask"), 2, first["result"]["requestState"],
                                answer(only_key(first), accept("octocat"))));
        EXPECT_EQ(retried.value("error", json::object()).value("message", ""),
                  "Unknown or expired requestState")
            << retried;
    });

    EXPECT_TRUE(released);
    EXPECT_EQ(server_->mrtr_stats().parked, 0u);
}

TEST_F(MrtrTest, StatsAccessorMatchesCounts) {
    run([&]() -> mcp::Task<void> {
        co_await send(call_tool(1, "ask"));
        co_await send(call_tool(2, "ask"));
        co_await send(retry(call_tool(3, "ask"), 3, "unknown"));
    });

    const auto stats = server_->mrtr_stats();
    EXPECT_EQ(stats.parked, 2u);
    EXPECT_EQ(stats.resume_unknown, 1u);
    EXPECT_EQ(stats.resume_hit, 0u);
    EXPECT_EQ(stats.evicted_ttl, 0u);
    EXPECT_EQ(stats.evicted_cap, 0u);
}

TEST_F(MrtrTest, LegacyRequestsStillUseServerToClientRequests) {
    // A request of an earlier revision is not a multi round-trip request: its handler's question
    // goes to the client as an elicitation/create request on the session.
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    std::vector<json> written;
    raw_transport->set_on_write([&written, raw_transport](std::string_view message) {
        written.push_back(json::parse(message));
        if (written.size() == 2) {
            raw_transport->close();
        }
    });
    auto initialize = make_initialize_request("init");
    initialize["params"]["capabilities"] = {{"elicitation", json::object()}};
    raw_transport->enqueue_message(initialize.dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(make_tool_call_request("call", "ask").dump());
    asio::steady_timer guard(io_ctx_, std::chrono::seconds(30));
    guard.async_wait([raw_transport](const boost::system::error_code& error) {
        if (!error) {
            raw_transport->close();
        }
    });
    asio::co_spawn(
        io_ctx_,
        [this, transport, &guard]() -> mcp::Task<void> {
            co_await server_->run(transport, io_ctx_.get_executor());
            guard.cancel();
            io_ctx_.stop();
        },
        asio::detached);
    io_ctx_.run();

    ASSERT_GE(written.size(), 2u);
    EXPECT_EQ(written[1]["method"], "elicitation/create") << written[1];
    EXPECT_EQ(server_->mrtr_stats().parked, 0u);
}

}  // namespace
