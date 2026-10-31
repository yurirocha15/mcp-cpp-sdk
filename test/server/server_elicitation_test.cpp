#include "../test_utils.hpp"

#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

struct DummyInput {
    std::string text;
};

inline void to_json(nlohmann::json& j, const DummyInput& input) {
    j = nlohmann::json{{"text", input.text}};
}

inline void from_json(const nlohmann::json& j, DummyInput& input) { j.at("text").get_to(input.text); }

}  // namespace

class ElicitationTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ElicitationTest, FormElicitationRoundtrip) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(server_info), std::move(caps));

    server.add_tool<DummyInput, mcp::CallToolResult>(
        "ask_user", "Asks the user via elicitation",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams form_params;
            form_params.message = "Please enter your name";
            form_params.requestedSchema =
                nlohmann::json{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};

            auto result = co_await ctx.elicit(std::move(form_params));

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            if (result.action == mcp::ElicitAction::eAccept && result.content) {
                tc.text = result.content->dump();
            } else {
                tc.text = "declined";
            }
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.value("id", "") == "initialize") {
            return;
        }

        written_messages.emplace_back(msg);

        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            auto request_id = json_msg["id"].get<std::string>();

            EXPECT_EQ(json_msg["params"]["mode"], "form");
            EXPECT_EQ(json_msg["params"]["message"], "Please enter your name");

            mcp::ElicitResult elicit_result;
            elicit_result.action = mcp::ElicitAction::eAccept;
            elicit_result.content = nlohmann::json{{"name", "Alice"}};

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(elicit_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tool_call_request;
    tool_call_request["jsonrpc"] = "2.0";
    tool_call_request["id"] = "req-1";
    tool_call_request["method"] = "tools/call";
    tool_call_request["params"] = {{"name", "ask_user"}, {"arguments", {{"text", "hello"}}}};

    raw_transport->enqueue_message(make_initialize_request("initialize").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto elicit_req = nlohmann::json::parse(written_messages[0]);
    EXPECT_EQ(elicit_req["method"], "elicitation/create");
    EXPECT_EQ(elicit_req["params"]["mode"], "form");

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-1");
    ASSERT_TRUE(tool_response.contains("result"));
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "{\"name\":\"Alice\"}");
}

TEST_F(ElicitationTest, URLElicitationRoundtrip) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    server.add_tool<DummyInput, mcp::CallToolResult>(
        "oauth_tool", "Requires user auth via URL",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestURLParams url_params;
            url_params.message = "Please authenticate";
            url_params.url = "https://example.com/auth";
            url_params.elicitationId = "elicit-123";

            auto result = co_await ctx.elicit(std::move(url_params));

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            tc.text =
                (result.action == mcp::ElicitAction::eAccept) ? "authenticated" : "not_authenticated";
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.value("id", "") == "initialize") {
            return;
        }

        written_messages.emplace_back(msg);

        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            auto request_id = json_msg["id"].get<std::string>();

            EXPECT_EQ(json_msg["params"]["mode"], "url");
            EXPECT_EQ(json_msg["params"]["url"], "https://example.com/auth");
            EXPECT_EQ(json_msg["params"]["elicitationId"], "elicit-123");

            mcp::ElicitResult elicit_result;
            elicit_result.action = mcp::ElicitAction::eAccept;

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(elicit_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tool_call_request;
    tool_call_request["jsonrpc"] = "2.0";
    tool_call_request["id"] = "req-2";
    tool_call_request["method"] = "tools/call";
    tool_call_request["params"] = {{"name", "oauth_tool"}, {"arguments", {{"text", "go"}}}};

    raw_transport->enqueue_message(make_initialize_request("initialize").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto elicit_req = nlohmann::json::parse(written_messages[0]);
    EXPECT_EQ(elicit_req["method"], "elicitation/create");
    EXPECT_EQ(elicit_req["params"]["mode"], "url");

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-2");
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "authenticated");
}

TEST_F(ElicitationTest, ClientDeclinesElicitation) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    server.add_tool<DummyInput, mcp::CallToolResult>(
        "needs_input", "Elicits form input",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams form_params;
            form_params.message = "Enter something";
            form_params.requestedSchema = nlohmann::json{{"type", "object"}};

            auto result = co_await ctx.elicit(std::move(form_params));

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            if (result.action == mcp::ElicitAction::eDecline) {
                tc.text = "user_declined";
            } else if (result.action == mcp::ElicitAction::eCancel) {
                tc.text = "user_cancelled";
            } else {
                tc.text = "unexpected";
            }
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.value("id", "") == "initialize") {
            return;
        }

        written_messages.emplace_back(msg);

        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            auto request_id = json_msg["id"].get<std::string>();

            mcp::ElicitResult elicit_result;
            elicit_result.action = mcp::ElicitAction::eDecline;

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(elicit_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tool_call_request;
    tool_call_request["jsonrpc"] = "2.0";
    tool_call_request["id"] = "req-3";
    tool_call_request["method"] = "tools/call";
    tool_call_request["params"] = {{"name", "needs_input"}, {"arguments", {{"text", "test"}}}};

    raw_transport->enqueue_message(make_initialize_request("initialize").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-3");
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "user_declined");
}

TEST_F(ElicitationTest, ElicitWithoutSenderThrows) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Context ctx(*raw_transport);

    bool threw = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                mcp::ElicitRequestFormParams form_params;
                form_params.message = "test";
                form_params.requestedSchema = nlohmann::json{{"type", "object"}};
                co_await ctx.elicit(std::move(form_params));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(threw);
}

namespace {

/// Runs `server` on a scripted session fed `messages` and returns everything it writes (minus
/// the initialize reply) once `expected` messages have been written.
std::vector<nlohmann::json> run_scripted(boost::asio::io_context& io_ctx, mcp::Server& server,
                                         const std::vector<nlohmann::json>& messages,
                                         std::size_t expected) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw_transport = transport.get();
    std::vector<nlohmann::json> written;
    raw_transport->set_on_write([&written, raw_transport, expected](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id") && json_msg["id"] == "initialize") {
            return;
        }
        written.push_back(json_msg);
        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            nlohmann::json response = {
                {"jsonrpc", "2.0"}, {"id", json_msg["id"]}, {"result", {{"action", "accept"}}}};
            raw_transport->enqueue_message(response.dump());
        }
        if (written.size() >= expected) {
            raw_transport->close();
        }
    });
    for (const auto& message : messages) {
        raw_transport->enqueue_message(message.dump());
    }
    boost::asio::co_spawn(
        io_ctx,
        [&server, transport, &io_ctx]() -> mcp::Task<void> {
            co_await server.run(transport, io_ctx.get_executor());
        },
        boost::asio::detached);
    io_ctx.restart();
    io_ctx.run();
    return written;
}

/// A server whose only tool elicits through a URL-mode request without an elicitationId.
std::unique_ptr<mcp::Server> make_url_elicit_server() {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"elicit-server", "1.0"}, caps);
    server->add_tool<nlohmann::json, mcp::CallToolResult>(
        "oauth", "Elicits through a URL", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, nlohmann::json /*args*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestURLParams url_params;
            url_params.message = "Please authenticate";
            url_params.url = "https://example.com/auth";
            auto result = co_await ctx.elicit(std::move(url_params));
            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            tc.text = result.action == mcp::ElicitAction::eAccept ? "authenticated" : "declined";
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });
    return server;
}

class ElicitationLegacy : public ElicitationTest {};
class ElicitationModern : public ElicitationTest {};

}  // namespace

TEST_F(ElicitationLegacy, UrlModeGeneratesElicitationIdWhenUnset) {
    auto server = make_url_elicit_server();
    auto call = [](std::string_view id) {
        return nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", "tools/call"},
                              {"params", {{"name", "oauth"}, {"arguments", nlohmann::json::object()}}}};
    };
    auto written = run_scripted(io_ctx_, *server,
                                {make_initialize_request("initialize"), make_initialized_notification(),
                                 call("req-1"), call("req-2")},
                                4);
    std::vector<std::string> ids;
    for (const auto& msg : written) {
        if (msg.value("method", "") == "elicitation/create") {
            ASSERT_TRUE(msg["params"].contains("elicitationId")) << msg;
            ASSERT_TRUE(msg["params"]["elicitationId"].is_string()) << msg;
            ids.push_back(msg["params"]["elicitationId"].get<std::string>());
        }
    }
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_FALSE(ids[0].empty());
    EXPECT_NE(ids[0], ids[1]);
}

TEST_F(ElicitationLegacy, UrlModeKeepsExplicitElicitationId) {
    mcp::ElicitRequestURLParams url_params;
    url_params.message = "m";
    url_params.url = "https://example.com";
    url_params.elicitationId = "elicit-fixed";
    EXPECT_EQ(nlohmann::json(url_params).at("elicitationId"), "elicit-fixed");
}

TEST_F(ElicitationLegacy, CompletionNotificationStillSerializes) {
    mcp::ElicitationCompleteNotification note;
    note.params.requestId = 7;
    nlohmann::json j = note;
    EXPECT_EQ(j, nlohmann::json::parse(
                     R"({"method":"notifications/elicitation/complete","params":{"requestId":7}})"));
    EXPECT_EQ(j.get<mcp::ElicitationCompleteNotification>().params.requestId, mcp::RequestId(7));
}

TEST_F(ElicitationModern, UrlModeHasNoElicitationId) {
    mcp::ElicitRequestURLParams url_params;
    url_params.message = "Please authenticate";
    url_params.url = "https://example.com/auth";

    nlohmann::json j = url_params;
    EXPECT_FALSE(j.contains("elicitationId")) << j;
    EXPECT_EQ(j["mode"], "url");

    auto parsed = j.get<mcp::ElicitRequestURLParams>();
    EXPECT_FALSE(parsed.elicitationId.has_value());
    EXPECT_EQ(parsed.url, "https://example.com/auth");

    auto variant = j.get<mcp::ElicitRequestParams>();
    ASSERT_TRUE(std::holds_alternative<mcp::ElicitRequestURLParams>(variant));
    EXPECT_FALSE(std::get<mcp::ElicitRequestURLParams>(variant).elicitationId.has_value());
    EXPECT_EQ(nlohmann::json(variant), j);

    // An explicit null is treated as absent.
    j["elicitationId"] = nullptr;
    EXPECT_FALSE(j.get<mcp::ElicitRequestURLParams>().elicitationId.has_value());
}

TEST_F(ElicitationModern, RequestNeverEmitsElicitationIdOrCompletion) {
    auto server = make_url_elicit_server();
    // The handler waits for a retry that never comes; a short wait lets the run end.
    mcp::MrtrOptions options;
    options.ttl = std::chrono::milliseconds(1);
    server->set_mrtr_options(options);
    auto request = make_modern_request(1, "tools/call",
                                       {{"name", "oauth"}, {"arguments", nlohmann::json::object()}});
    request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = {
        {"elicitation", {{"url", nlohmann::json::object()}}}};
    auto written = run_scripted(io_ctx_, *server, {request}, 1);
    ASSERT_EQ(written.size(), 1u);
    for (const auto& msg : written) {
        EXPECT_NE(msg.value("method", ""), "elicitation/create") << msg;
        EXPECT_NE(msg.value("method", ""), "notifications/elicitation/complete") << msg;
        EXPECT_EQ(msg.dump().find("elicitationId"), std::string::npos) << msg;
    }
    // The question goes back in the answer itself, without an elicitation id.
    EXPECT_EQ(written[0]["id"], 1);
    ASSERT_TRUE(written[0].contains("result")) << written[0];
    EXPECT_EQ(written[0]["result"]["resultType"], "input_required") << written[0];
    EXPECT_EQ(written[0]["result"]["inputRequests"].begin().value()["params"]["mode"], "url");
}
