#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ScriptedTransport final : public mcp::ITransport {
   public:
    explicit ScriptedTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue_message(std::string msg) {
        boost::asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> callback) {
        on_write_ = std::move(callback);
    }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

struct AddParams {
    int augend = 0;
    int addend = 0;
};

inline void to_json(nlohmann::json& json_obj, const AddParams& params) {
    json_obj = nlohmann::json{{"augend", params.augend}, {"addend", params.addend}};
}

inline void from_json(const nlohmann::json& json_obj, AddParams& params) {
    json_obj.at("augend").get_to(params.augend);
    json_obj.at("addend").get_to(params.addend);
}

struct AddResult {
    int sum = 0;
};

inline void to_json(nlohmann::json& json_obj, const AddResult& result) {
    json_obj = nlohmann::json{{"sum", result.sum}};
}

inline void from_json(const nlohmann::json& json_obj, AddResult& result) {
    json_obj.at("sum").get_to(result.sum);
}

nlohmann::json make_initialize_request(std::string_view req_id) {
    return {{"jsonrpc", "2.0"},
            {"id", req_id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

}  // namespace

class ServerHandlersTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    struct ServerSetup {
        std::shared_ptr<mcp::ITransport> transport;
        ScriptedTransport* raw_transport;
        mcp::Server server;

        ServerSetup(boost::asio::io_context& io_ctx, mcp::ServerCapabilities caps)
            : transport(std::make_shared<ScriptedTransport>(io_ctx.get_executor())),
              raw_transport(static_cast<ScriptedTransport*>(transport.get())),
              server(
                  [] {
                      mcp::Implementation info;
                      info.name = "test-server";
                      info.version = "1.0";
                      return info;
                  }(),
                  std::move(caps)) {}
    };
};

TEST_F(ServerHandlersTest, SyncToolCallReturnsResult) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add", "Adds two numbers",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"augend", {{"type", "integer"}}}, {"addend", {{"type", "integer"}}}}}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "add";
    call_params["arguments"] = nlohmann::json{{"augend", 3}, {"addend", 4}};
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["id"], "1");

    auto& tool_response = responses[1];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["sum"], 7);
}

TEST_F(ServerHandlersTest, AsyncToolCallReturnsResult) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>("add_async", "Adds two numbers asynchronously",
                                                nlohmann::json{{"type", "object"}},
                                                [](AddParams params) -> mcp::Task<AddResult> {
                                                    co_return AddResult{params.augend + params.addend};
                                                });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "add_async";
    call_params["arguments"] = nlohmann::json{{"augend", 10}, {"addend", 20}};
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& tool_response = responses[1];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["sum"], 30);
}

TEST_F(ServerHandlersTest, ToolWithContextLogsMessage) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add_with_log", "Adds and logs", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, AddParams params) -> mcp::Task<AddResult> {
            co_await ctx.log_info("computing sum");
            co_return AddResult{params.augend + params.addend};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init response + log notification + tool result = 3
        if (responses.size() == 3) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "add_with_log";
    call_params["arguments"] = nlohmann::json{{"augend", 5}, {"addend", 6}};
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);

    // Second message is the log notification
    auto& log_notification = responses[1];
    EXPECT_EQ(log_notification["method"], "notifications/message");
    EXPECT_EQ(log_notification["params"]["data"], "computing sum");
    EXPECT_FALSE(log_notification.contains("id"));

    // Third message is the tool result
    auto& tool_response = responses[2];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["sum"], 11);
}

TEST_F(ServerHandlersTest, ToolsListReturnsRegisteredTools) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add", "Adds two numbers",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"augend", {{"type", "integer"}}}, {"addend", {{"type", "integer"}}}}}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    EXPECT_EQ(list_response["id"], "2");
    ASSERT_TRUE(list_response.contains("result"));
    ASSERT_TRUE(list_response["result"].contains("tools"));
    auto& tools = list_response["result"]["tools"];
    ASSERT_EQ(tools.size(), 1);
    EXPECT_EQ(tools[0]["name"], "add");
    EXPECT_EQ(tools[0]["description"], "Adds two numbers");
}

TEST_F(ServerHandlersTest, ResourcesReadReturnsContent) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Resource resource;
    resource.uri = "file:///hello.txt";
    resource.name = "hello.txt";
    resource.description = "A greeting file";

    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(resource), [](mcp::ReadResourceRequestParams params) -> mcp::ReadResourceResult {
            mcp::TextResourceContents text_contents;
            text_contents.uri = params.uri;
            text_contents.text = "Hello, World!";
            text_contents.mimeType = "text/plain";

            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(text_contents));
            return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json read_req;
    read_req["jsonrpc"] = "2.0";
    read_req["id"] = "2";
    read_req["method"] = "resources/read";
    read_req["params"] = nlohmann::json{{"uri", "file:///hello.txt"}};
    setup.raw_transport->enqueue_message(read_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& read_response = responses[1];
    EXPECT_EQ(read_response["id"], "2");
    ASSERT_TRUE(read_response.contains("result"));
    ASSERT_TRUE(read_response["result"].contains("contents"));
    auto& contents = read_response["result"]["contents"];
    ASSERT_EQ(contents.size(), 1);
    EXPECT_EQ(contents[0]["uri"], "file:///hello.txt");
    EXPECT_EQ(contents[0]["text"], "Hello, World!");
    EXPECT_EQ(contents[0]["mimeType"], "text/plain");
}

TEST_F(ServerHandlersTest, ResourcesListReturnsRegisteredResources) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Resource resource;
    resource.uri = "file:///test.txt";
    resource.name = "test.txt";
    resource.description = "A test file";

    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(resource), [](mcp::ReadResourceRequestParams /*params*/) -> mcp::ReadResourceResult {
            return mcp::ReadResourceResult{};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "resources/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    ASSERT_TRUE(list_response.contains("result"));
    auto& resources = list_response["result"]["resources"];
    ASSERT_EQ(resources.size(), 1);
    EXPECT_EQ(resources[0]["uri"], "file:///test.txt");
    EXPECT_EQ(resources[0]["name"], "test.txt");
}

TEST_F(ServerHandlersTest, ResourceTemplatesListReturnsRegisteredTemplates) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{path}";
    tmpl.name = "file-access";
    tmpl.description = "Access files by path";

    setup.server.add_resource_template(std::move(tmpl));

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "resources/templates/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    ASSERT_TRUE(list_response.contains("result"));
    auto& templates = list_response["result"]["resourceTemplates"];
    ASSERT_EQ(templates.size(), 1);
    EXPECT_EQ(templates[0]["uriTemplate"], "file:///{path}");
    EXPECT_EQ(templates[0]["name"], "file-access");
}

TEST_F(ServerHandlersTest, PromptsGetReturnsPromptMessages) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::PromptsCapability prompts_cap;
    caps.prompts = std::move(prompts_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Prompt prompt;
    prompt.name = "greeting";
    prompt.description = "A greeting prompt";
    mcp::PromptArgument arg;
    arg.name = "name";
    arg.description = "The name to greet";
    prompt.arguments = std::vector<mcp::PromptArgument>{std::move(arg)};

    setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        std::move(prompt), [](mcp::GetPromptRequestParams params) -> mcp::GetPromptResult {
            std::string name_val = "World";
            if (params.arguments) {
                auto iter = params.arguments->find("name");
                if (iter != params.arguments->end()) {
                    name_val = iter->second;
                }
            }

            mcp::TextContent text;
            text.text = "Hello, " + name_val + "!";

            mcp::PromptMessage msg;
            msg.role = mcp::Role::User;
            msg.content = std::move(text);

            mcp::GetPromptResult result;
            result.description = "A personalized greeting";
            result.messages.push_back(std::move(msg));
            return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json get_req;
    get_req["jsonrpc"] = "2.0";
    get_req["id"] = "2";
    get_req["method"] = "prompts/get";
    get_req["params"] = nlohmann::json{{"name", "greeting"}, {"arguments", {{"name", "Alice"}}}};
    setup.raw_transport->enqueue_message(get_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& get_response = responses[1];
    EXPECT_EQ(get_response["id"], "2");
    ASSERT_TRUE(get_response.contains("result"));
    EXPECT_EQ(get_response["result"]["description"], "A personalized greeting");
    auto& messages = get_response["result"]["messages"];
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0]["role"], "user");
    EXPECT_EQ(messages[0]["content"]["text"], "Hello, Alice!");
}

TEST_F(ServerHandlersTest, PromptsListReturnsRegisteredPrompts) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::PromptsCapability prompts_cap;
    caps.prompts = std::move(prompts_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Prompt prompt;
    prompt.name = "test-prompt";
    prompt.description = "A test prompt";

    setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        std::move(prompt), [](mcp::GetPromptRequestParams /*params*/) -> mcp::GetPromptResult {
            return mcp::GetPromptResult{};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "prompts/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    ASSERT_TRUE(list_response.contains("result"));
    auto& prompts = list_response["result"]["prompts"];
    ASSERT_EQ(prompts.size(), 1);
    EXPECT_EQ(prompts[0]["name"], "test-prompt");
    EXPECT_EQ(prompts[0]["description"], "A test prompt");
}

TEST_F(ServerHandlersTest, UnknownToolReturnsError) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "nonexistent";
    call_params["arguments"] = nlohmann::json::object();
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    EXPECT_EQ(error_response["id"], "2");
    ASSERT_TRUE(error_response.contains("error"));
    EXPECT_EQ(error_response["error"]["code"], mcp::METHOD_NOT_FOUND);
    EXPECT_TRUE(error_response["error"]["message"].get<std::string>().find("nonexistent") !=
                std::string::npos);
}

// --- Pagination Tests ---

TEST_F(ServerHandlersTest, ToolsListPaginationFirstPage) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_page_size(2);

    for (int i = 0; i < 5; ++i) {
        auto name = "tool_" + std::to_string(i);
        setup.server.add_tool<AddParams, AddResult>(
            name, "Tool " + std::to_string(i), nlohmann::json{{"type", "object"}},
            [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_EQ(result["tools"].size(), 2);
    EXPECT_EQ(result["tools"][0]["name"], "tool_0");
    EXPECT_EQ(result["tools"][1]["name"], "tool_1");
    ASSERT_TRUE(result.contains("nextCursor"));
    EXPECT_EQ(result["nextCursor"], "2");
}

TEST_F(ServerHandlersTest, ToolsListPaginationWithCursor) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_page_size(2);

    for (int i = 0; i < 5; ++i) {
        auto name = "tool_" + std::to_string(i);
        setup.server.add_tool<AddParams, AddResult>(
            name, "Tool " + std::to_string(i), nlohmann::json{{"type", "object"}},
            [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    list_req["params"] = nlohmann::json{{"cursor", "2"}};
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_EQ(result["tools"].size(), 2);
    EXPECT_EQ(result["tools"][0]["name"], "tool_2");
    EXPECT_EQ(result["tools"][1]["name"], "tool_3");
    EXPECT_EQ(result["nextCursor"], "4");
}

TEST_F(ServerHandlersTest, ToolsListPaginationLastPage) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_page_size(2);

    for (int i = 0; i < 5; ++i) {
        auto name = "tool_" + std::to_string(i);
        setup.server.add_tool<AddParams, AddResult>(
            name, "Tool " + std::to_string(i), nlohmann::json{{"type", "object"}},
            [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    list_req["params"] = nlohmann::json{{"cursor", "4"}};
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_EQ(result["tools"].size(), 1);
    EXPECT_EQ(result["tools"][0]["name"], "tool_4");
    EXPECT_FALSE(result.contains("nextCursor"));
}

TEST_F(ServerHandlersTest, PaginationDisabledByDefault) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    for (int i = 0; i < 5; ++i) {
        auto name = "tool_" + std::to_string(i);
        setup.server.add_tool<AddParams, AddResult>(
            name, "Tool " + std::to_string(i), nlohmann::json{{"type", "object"}},
            [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    EXPECT_EQ(result["tools"].size(), 5);
    EXPECT_FALSE(result.contains("nextCursor"));
}

TEST_F(ServerHandlersTest, ResourcesListPagination) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_page_size(1);

    for (int i = 0; i < 3; ++i) {
        mcp::Resource resource;
        resource.uri = "file:///r" + std::to_string(i) + ".txt";
        resource.name = "r" + std::to_string(i);

        setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
            std::move(resource),
            [](mcp::ReadResourceRequestParams /*params*/) -> mcp::ReadResourceResult {
                return mcp::ReadResourceResult{};
            });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "resources/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_EQ(result["resources"].size(), 1);
    EXPECT_EQ(result["resources"][0]["name"], "r0");
    EXPECT_EQ(result["nextCursor"], "1");
}

// --- OutputSchema Tests ---

TEST_F(ServerHandlersTest, ToolWithOutputSchemaIncludesStructuredContent) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    nlohmann::json output_schema = {{"type", "object"},
                                    {"properties", {{"sum", {{"type", "integer"}}}}}};

    setup.server.add_tool<AddParams, AddResult>(
        "add_structured", "Adds with output schema",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"augend", {{"type", "integer"}}}, {"addend", {{"type", "integer"}}}}}},
        output_schema,
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "add_structured"}, {"arguments", {{"augend", 10}, {"addend", 20}}}};
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    EXPECT_EQ(result["sum"], 30);
    ASSERT_TRUE(result.contains("structuredContent"));
    EXPECT_EQ(result["structuredContent"]["sum"], 30);
}

TEST_F(ServerHandlersTest, ToolWithoutOutputSchemaNoStructuredContent) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add_plain", "Adds without output schema", nlohmann::json{{"type", "object"}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "add_plain"}, {"arguments", {{"augend", 5}, {"addend", 3}}}};
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    EXPECT_EQ(result["sum"], 8);
    EXPECT_FALSE(result.contains("structuredContent"));
}

TEST_F(ServerHandlersTest, ToolsListIncludesOutputSchema) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    nlohmann::json output_schema = {{"type", "object"},
                                    {"properties", {{"result", {{"type", "string"}}}}}};

    setup.server.add_tool<AddParams, AddResult>(
        "schema_tool", "Has output schema", nlohmann::json{{"type", "object"}}, output_schema,
        [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& tools = responses[1]["result"]["tools"];
    ASSERT_EQ(tools.size(), 1);
    EXPECT_EQ(tools[0]["name"], "schema_tool");
    ASSERT_TRUE(tools[0].contains("outputSchema"));
    EXPECT_EQ(tools[0]["outputSchema"]["type"], "object");
}

// --- Completion Tests ---

TEST_F(ServerHandlersTest, CompletionReturnsResults) {
    mcp::ServerCapabilities caps;
    caps.completions = nlohmann::json::object();

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_completion_provider(
        [](const mcp::CompleteParams& params) -> mcp::Task<mcp::CompleteResult> {
            mcp::CompleteResult result;
            result.completion.values = {"hello", "help", "hero"};
            result.completion.total = 3;
            result.completion.hasMore = false;
            co_return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json complete_req;
    complete_req["jsonrpc"] = "2.0";
    complete_req["id"] = "2";
    complete_req["method"] = "completion/complete";
    complete_req["params"] = nlohmann::json{{"ref", {{"type", "ref/prompt"}, {"name", "my-prompt"}}},
                                            {"argument", {{"name", "arg1"}, {"value", "hel"}}}};
    setup.raw_transport->enqueue_message(complete_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_TRUE(result.contains("completion"));
    auto& completion = result["completion"];
    ASSERT_EQ(completion["values"].size(), 3);
    EXPECT_EQ(completion["values"][0], "hello");
    EXPECT_EQ(completion["values"][1], "help");
    EXPECT_EQ(completion["values"][2], "hero");
    EXPECT_EQ(completion["total"], 3);
    EXPECT_FALSE(completion["hasMore"].get<bool>());
}

TEST_F(ServerHandlersTest, CompletionWithoutHandlerReturnsError) {
    mcp::ServerCapabilities caps;

    ServerSetup setup(io_ctx_, std::move(caps));

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json complete_req;
    complete_req["jsonrpc"] = "2.0";
    complete_req["id"] = "2";
    complete_req["method"] = "completion/complete";
    complete_req["params"] = nlohmann::json{{"ref", {{"type", "ref/prompt"}, {"name", "test"}}},
                                            {"argument", {{"name", "arg", "value", "val"}}}};
    setup.raw_transport->enqueue_message(complete_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    ASSERT_TRUE(error_response.contains("error"));
    EXPECT_EQ(error_response["error"]["code"], mcp::METHOD_NOT_FOUND);
}
