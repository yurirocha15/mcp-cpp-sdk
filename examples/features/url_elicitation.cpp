/// @file url_elicitation.cpp
/// @brief Demonstrates URL-mode elicitation: the question, its completion notification and the
///        UrlElicitationRequired error.
///
/// This example shows:
/// - A tool that asks the user to visit a URL with ctx.elicit(), and tells the client that the
///   step is done with ctx.notify_elicitation_complete()
/// - Server::notify_elicitation_complete(), which does the same from code outside the handler,
///   such as the callback of the site the user visited
/// - A tool that throws UrlElicitationRequired, and a client that reads the elicitations of the
///   resulting error with url_elicitations_required()
/// - Client::on_elicitation_complete(), which reports each completion
/// - The same URL question in a 2026-07-28 session, where it comes back to the client in an
///   input_required result and has no id to complete
///
/// The completion notification and the error belong to protocol revision 2025-11-25, so the
/// first two sessions are pinned to ClientProtocol::legacy().

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/server/server.hpp>
#include <mcp/server/url_elicitation.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace jsonrpc = mcp::constants::jsonrpc;

namespace {

/// Throws when a check of the example fails.
void expect(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
    if (!condition) {
        throw std::runtime_error(what);
    }
}

/// Waits up to two seconds until @p done returns true.
mcp::Task<bool> wait_until(const std::function<bool()>& done, const asio::any_io_executor& executor) {
    asio::steady_timer timer(executor);
    for (int attempt = 0; attempt < 100 && !done(); ++attempt) {
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return done();
}

std::shared_ptr<mcp::Server> make_server(const std::shared_ptr<bool>& signed_in) {
    using namespace mcp;

    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{};
    auto server = std::make_shared<Server>(Implementation{"url-elicitation-server", "1.0.0"}, caps);

    const nlohmann::json no_arguments = {{"type", "object"}, {"properties", nlohmann::json::object()}};

    // docs-begin: url-tool
    // The tool asks the user to open a URL. An accepted answer means the user agreed to visit
    // it, not that the step is done.
    server->add_tool<nlohmann::json, nlohmann::json>(
        "link_storage", "Asks the user to link a storage account", no_arguments,
        [](Context& ctx, const nlohmann::json&) -> Task<nlohmann::json> {
            // The question is on the heap so that the coroutine holds only a pointer to it
            // while it waits for the answer.
            const auto question = std::make_shared<const ElicitRequestParams>(ElicitRequestURLParams{
                .message = "Link your storage account",
                .url = "https://storage.example/link?state=abc",
                .elicitationId = "link-1",
            });
            const auto answer = co_await ctx.elicit(*question);
            if (answer.action != ElicitAction::eAccept) {
                co_return "The user did not agree to link the account";
            }

            // The handler learned that the user finished, so it tells the client. The call
            // returns false in a session that has no such notification.
            const bool notified = co_await ctx.notify_elicitation_complete("link-1");
            co_return notified ? "Linked, and the client was told" : "Linked, the client was not told";
        });
    // docs-end: url-tool

    // docs-begin: url-required
    // A tool that cannot answer before the user signs in throws UrlElicitationRequired. In a
    // 2025-11-25 session the client receives the error g_URL_ELICITATION_REQUIRED.
    server->add_tool<nlohmann::json, nlohmann::json>(
        "read_mailbox", "Reads the mailbox of a signed-in user", no_arguments,
        [signed_in](const nlohmann::json&) -> nlohmann::json {
            if (!*signed_in) {
                throw UrlElicitationRequired({ElicitRequestURLParams{
                    .message = "Sign in to read your mailbox",
                    .url = "https://mail.example/sign-in",
                    .elicitationId = "sign-in-1",
                }});
            }
            return "3 messages";
        });
    // docs-end: url-required

    return server;
}

/// Runs @p server on a new in-memory connection and returns the client's end of it.
std::shared_ptr<mcp::ITransport> start_server(const std::shared_ptr<mcp::Server>& server,
                                              const asio::any_io_executor& executor) {
    auto [client_transport, server_transport] = mcp::make_memory_transport_pair(executor);
    asio::co_spawn(
        executor,
        [server, server_transport = server_transport, executor]() -> mcp::Task<void> {
            try {
                co_await server->run(server_transport, executor);
            } catch (const std::exception& error) {
                std::cerr << "Server error: " << error.what() << '\n';
            }
        },
        asio::detached);
    return client_transport;
}

mcp::ClientCapabilities url_capabilities() {
    mcp::ClientCapabilities capabilities;
    capabilities.elicitation = mcp::ClientCapabilities::ElicitationCapability{
        .url = nlohmann::json::object(),
    };
    return capabilities;
}

/// A session of 2025-11-25: a question, its completion, and the error that requires one.
mcp::Task<void> legacy_session(const asio::any_io_executor& executor) {
    using namespace mcp;
    std::cout << "A 2025-11-25 session\n";

    auto signed_in = std::make_shared<bool>(false);
    auto server = make_server(signed_in);
    ClientOptions options;
    options.protocol = ClientProtocol::legacy();
    Client client(start_server(server, executor), executor, options);

    // docs-begin: url-client
    // The user's answer to the question, and the notifications that say the step is done.
    std::vector<std::string> completed;
    client.on_elicitation([](const ElicitRequestParams& params) -> Task<ElicitResult> {
        ElicitResult answer;
        if (const auto* url = std::get_if<ElicitRequestURLParams>(&params)) {
            std::cout << "Client: asked to open " << url->url << '\n';
            answer.action = ElicitAction::eAccept;
        } else {
            answer.action = ElicitAction::eDecline;
        }
        co_return answer;
    });
    client.on_elicitation_complete([&completed](const ElicitationCompleteNotificationParams& done) {
        std::cout << "Client: elicitation " << done.elicitationId << " completed\n";
        completed.push_back(done.elicitationId);
    });
    // docs-end: url-client

    const Implementation client_info{"url-elicitation-client", "1.0.0"};
    const auto capabilities = url_capabilities();
    co_await client.connect(client_info, capabilities);

    const auto linked = co_await client.call_tool("link_storage", nlohmann::json::object());
    std::cout << "Client: " << result_text(linked) << '\n';
    expect(completed.size() == 1 && completed.front() == "link-1",
           "the handler completed its elicitation");

    // docs-begin: url-required-client
    // The call fails with the error that carries the elicitations the user must complete.
    std::optional<std::vector<ElicitRequestURLParams>> required;
    try {
        co_await client.call_tool("read_mailbox", nlohmann::json::object());
    } catch (const McpError& error) {
        if (error.code() == jsonrpc::g_URL_ELICITATION_REQUIRED) {
            required = url_elicitations_required(error);
        }
    }
    // docs-end: url-required-client
    expect(required && required->size() == 1 && required->front().elicitationId == "sign-in-1",
           "read_mailbox requires the elicitation sign-in-1");
    std::cout << "Client: sign in at " << required->front().url << '\n';

    // docs-begin: url-server-complete
    // The user signs in on the other site, whose callback reaches this code. The server completes
    // the elicitation of the session that sent it, after the handler has returned.
    *signed_in = true;
    const bool sent = co_await server->notify_elicitation_complete("sign-in-1");
    // docs-end: url-server-complete
    expect(sent, "Server::notify_elicitation_complete() sent the notification");
    const bool arrived = co_await wait_until([&completed] { return completed.size() == 2; }, executor);
    expect(arrived && completed.back() == "sign-in-1", "the client was told that sign-in-1 completed");

    const auto mailbox = co_await client.call_tool("read_mailbox", nlohmann::json::object());
    expect(result_text(mailbox) == "3 messages", "the request succeeds once the user signed in");
    client.close();
}

/// A session of 2026-07-28: the question comes back in an input_required result and has no id.
mcp::Task<void> modern_session(const asio::any_io_executor& executor) {
    using namespace mcp;
    std::cout << "A 2026-07-28 session\n";

    auto server = make_server(std::make_shared<bool>(false));
    Client client(start_server(server, executor), executor);

    bool asked = false;
    client.on_elicitation([&asked](const ElicitRequestParams& params) -> Task<ElicitResult> {
        const auto* url = std::get_if<ElicitRequestURLParams>(&params);
        asked = url != nullptr && !url->elicitationId;
        ElicitResult answer;
        answer.action = ElicitAction::eAccept;
        co_return answer;
    });

    const Implementation client_info{"url-elicitation-client", "1.0.0"};
    const auto capabilities = url_capabilities();
    const auto connected = co_await client.connect(client_info, capabilities);
    expect(connected.era == ProtocolEra::eModern, "the session is of the 2026-07-28 era");

    const auto linked = co_await client.call_tool("link_storage", nlohmann::json::object());
    std::cout << "Client: " << result_text(linked) << '\n';
    expect(asked, "the question carried no elicitationId");
    expect(result_text(linked) == "Linked, the client was not told",
           "a 2026-07-28 session has no completion notification");
    client.close();
}

}  // namespace

int main() {
    try {
        asio::io_context io_ctx;
        bool succeeded = false;

        asio::co_spawn(
            io_ctx,
            [&]() -> mcp::Task<void> {
                try {
                    co_await legacy_session(io_ctx.get_executor());
                    co_await modern_session(io_ctx.get_executor());
                    succeeded = true;
                } catch (const std::exception& error) {
                    std::cerr << "Error: " << error.what() << '\n';
                }
                io_ctx.stop();
            },
            asio::detached);

        // Fail rather than hang if something never answers.
        asio::steady_timer watchdog(io_ctx.get_executor());
        watchdog.expires_after(std::chrono::seconds(20));
        watchdog.async_wait([&io_ctx](const boost::system::error_code& error) {
            if (!error) {
                std::cerr << "Timed out\n";
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
    } catch (const std::exception& error) {
        std::cerr << "Exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
