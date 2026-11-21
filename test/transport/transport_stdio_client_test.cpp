#include "../support/hang_guard.hpp"

#include "../support/spawn.hpp"
#include "mcp/client/client.hpp"
#include "mcp/client/stdio_session.hpp"
#include "mcp/core/async.hpp"
#include "mcp/transport/stdio_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <mcp/core/constants.hpp>
#endif

namespace transport_stdio_client_test {

namespace jsonrpc = mcp::constants::jsonrpc;

#ifndef MCP_TEST_STDIO_SERVER
#error "MCP_TEST_STDIO_SERVER must name the stdio test server executable"
#endif

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

mcp::StdioProcessOptions server_options(const std::string& mode = "serve",
                                        std::vector<std::string> extra = {}) {
    mcp::StdioProcessOptions options;
    options.command = MCP_TEST_STDIO_SERVER;
    options.arguments.push_back(mode);
    for (auto& argument : extra) {
        options.arguments.push_back(std::move(argument));
    }
    options.stderr_mode = mcp::StderrMode::eDiscard;
    return options;
}

/// Runs a coroutine on an io_context with the hang guard standing by.
class Harness {
   public:
    Harness() : guard_(io) {}

    void run(std::function<mcp::Task<void>()> body) {
        guard_.expires_after(g_hang_guard);
        guard_.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                fired_ = true;
                io.stop();
            }
        });
        mcp_test::spawn_detached(io, [this, body = std::move(body)]() -> mcp::Task<void> {
            try {
                co_await body();
            } catch (const std::exception& error) {
                ADD_FAILURE() << "unexpected exception: " << error.what();
            }
            guard_.cancel();
        });
        io.run();
        EXPECT_FALSE(fired_) << "the test did not finish";
    }

    boost::asio::io_context io;

   private:
    boost::asio::steady_timer guard_;
    bool fired_{false};
};

mcp::Task<bool> wait_until(std::function<bool()> done) {
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    const auto deadline = std::chrono::steady_clock::now() + g_hang_guard;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            co_return false;
        }
        timer.expires_after(10ms);
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return true;
}

/// A one-member object, built by a call: GCC 11 mishandles a braced temporary passed to a coroutine.
json arg(const char* key, json value) {
    json object = json::object();
    object[key] = std::move(value);
    return object;
}

mcp::Task<json> call(mcp::Client& client, const std::string& tool, json arguments = json::object()) {
    const auto result = co_await client.call_tool(tool, arguments);
    co_return result.structuredContent.value_or(json(nullptr));
}

void set_parent_variable(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

}  // namespace

TEST(StdioClientTransport, ClientWorksOverTheLaunchedServerAndShutsItDown) {
    Harness harness;
    std::optional<mcp::ProcessExit> exit;
    json args;
    harness.run([&]() -> mcp::Task<void> {
        auto transport = std::make_shared<mcp::StdioClientTransport>(
            harness.io.get_executor(), server_options("serve", {"one", "two"}));
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        args = co_await call(client, "args");
        client.close();
        exit = co_await transport->shutdown();
    });
    EXPECT_EQ(args, (json{{"args", json::array({"serve", "one", "two"})}}));
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->code, 0);
    EXPECT_FALSE(exit->signaled);
    EXPECT_FALSE(exit->forced);
    EXPECT_TRUE(exit->requested);
}

TEST(StdioClientTransport, LegacySessionWorks) {
    Harness harness;
    json args;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), server_options());
        mcp::ClientOptions options;
        options.protocol = mcp::ClientProtocol::legacy();
        mcp::Client client(transport, harness.io.get_executor(), options);
        co_await client.connect("stdio-client-test", "1.0");
        args = co_await call(client, "args");
        client.close();
        co_await transport->shutdown();
    });
    EXPECT_EQ(args, (json{{"args", json::array({"serve"})}}));
}

TEST(StdioClientTransport, ArgumentsReachTheServerUnchanged) {
    Harness harness;
    json args;
    harness.run([&]() -> mcp::Task<void> {
        auto transport = std::make_shared<mcp::StdioClientTransport>(
            harness.io.get_executor(),
            server_options("serve", {"with space", "quo\"te", "back\\slash\\", "", "--flag=1"}));
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        args = co_await call(client, "args");
        client.close();
        co_await transport->shutdown();
    });
    EXPECT_EQ(args["args"],
              (json::array({"serve", "with space", "quo\"te", "back\\slash\\", "", "--flag=1"})));
}

TEST(StdioClientTransport, ServerThatIgnoresStdinCloseIsTerminated) {
    Harness harness;
    std::optional<mcp::ProcessExit> exit;
    std::chrono::steady_clock::duration elapsed{};
    auto options = server_options("ignore-stdin-close");
    options.stdin_close_wait = 300ms;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        const auto start = std::chrono::steady_clock::now();
        client.close();
        exit = co_await transport->shutdown();
        elapsed = std::chrono::steady_clock::now() - start;
    });
    ASSERT_TRUE(exit.has_value());
    EXPECT_TRUE(exit->forced);
    EXPECT_TRUE(exit->requested);
    EXPECT_GE(elapsed, 300ms);
#if !defined(_WIN32)
    EXPECT_TRUE(exit->signaled);
    EXPECT_EQ(exit->code, SIGTERM);
#endif
}

#if !defined(_WIN32)
TEST(StdioClientTransport, ServerThatIgnoresSigtermIsKilled) {
    Harness harness;
    std::optional<mcp::ProcessExit> exit;
    std::chrono::steady_clock::duration elapsed{};
    auto options = server_options("ignore-sigterm");
    options.stdin_close_wait = 200ms;
    options.terminate_wait = 200ms;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        const auto start = std::chrono::steady_clock::now();
        client.close();
        exit = co_await transport->shutdown();
        elapsed = std::chrono::steady_clock::now() - start;
    });
    ASSERT_TRUE(exit.has_value());
    EXPECT_TRUE(exit->forced);
    EXPECT_TRUE(exit->signaled);
    EXPECT_EQ(exit->code, SIGKILL);
    EXPECT_GE(elapsed, 400ms);
}

TEST(StdioClientTransport, DestructorLeavesNoProcessBehind) {
    Harness harness;
    std::uint64_t pid = 0;
    auto options = server_options("ignore-sigterm");
    options.stdin_close_wait = 100ms;
    options.terminate_wait = 100ms;
    harness.run([&]() -> mcp::Task<void> {
        {
            auto transport =
                std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
            pid = transport->process_id();
            mcp::Client client(transport, harness.io.get_executor());
            co_await client.connect("stdio-client-test", "1.0");
        }
        co_return;
    });
    ASSERT_NE(pid, 0u);
    // The process was reaped by the destructor, so its id no longer names a process of ours.
    const int signalled = ::kill(static_cast<pid_t>(pid), 0);
    const int error = errno;
    EXPECT_EQ(signalled, -1);
    EXPECT_EQ(error, ESRCH);
}
#endif

TEST(StdioClientTransport, UnexpectedExitIsReportedAndFailsPendingCalls) {
    Harness harness;
    std::optional<mcp::ProcessExit> reported;
    std::mutex mutex;
    std::optional<int> failure_code;
    std::optional<mcp::ProcessExit> status;
    auto options = server_options();
    options.on_exit = [&](const mcp::ProcessExit& exit) {
        std::lock_guard lock(mutex);
        reported = exit;
    };
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        try {
            co_await call(client, "crash", arg("status", 7));
        } catch (const mcp::McpError& error) {
            failure_code = error.code();
        }
        const bool seen = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return reported.has_value();
        });
        EXPECT_TRUE(seen);
        status = transport->exit_status();
    });
    ASSERT_TRUE(failure_code.has_value());
    EXPECT_EQ(*failure_code, jsonrpc::g_CONNECTION_CLOSED);
    ASSERT_TRUE(reported.has_value());
    EXPECT_EQ(reported->code, 7);
    EXPECT_FALSE(reported->signaled);
    EXPECT_FALSE(reported->requested);
    EXPECT_FALSE(reported->forced);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, 7);
}

TEST(StdioClientTransport, ReadFailsAndWriteFailsAfterTheServerEnds) {
    Harness harness;
    bool read_failed = false;
    bool write_failed = false;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientTransport transport(harness.io.get_executor(),
                                            server_options("exit-at-once", {"3"}));
        try {
            co_await transport.read_message();
        } catch (const std::runtime_error&) {
            read_failed = true;
        }
        const bool exited = co_await wait_until([&] { return transport.exit_status().has_value(); });
        EXPECT_TRUE(exited);
        // The pipe may still take one write before the failure shows; it always fails by the second.
        try {
            co_await transport.write_message("{}");
            co_await transport.write_message("{}");
        } catch (const std::runtime_error&) {
            write_failed = true;
        }
    });
    EXPECT_TRUE(read_failed);
    EXPECT_TRUE(write_failed);
}

TEST(StdioClientTransport, MessageWithANewlineIsRefused) {
    Harness harness;
    bool refused = false;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientTransport transport(harness.io.get_executor(), server_options());
        try {
            co_await transport.write_message("{\"a\":\n1}");
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        co_await transport.shutdown();
    });
    EXPECT_TRUE(refused);
}

TEST(StdioClientTransport, StderrIsCapturedLineByLine) {
    Harness harness;
    std::mutex mutex;
    std::vector<std::string> lines;
    auto options = server_options();
    options.stderr_mode = mcp::StderrMode::eCapture;
    options.on_stderr_line = [&](std::string_view line) {
        std::lock_guard lock(mutex);
        lines.emplace_back(line);
    };
    bool seen = false;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        co_await call(client, "log", arg("text", "first line"));
        co_await call(client, "log", arg("text", "second line"));
        client.close();
        co_await transport->shutdown();
        seen = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return lines.size() >= 3;
        });
    });
    EXPECT_TRUE(seen);
    std::lock_guard lock(mutex);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "test server started");
    EXPECT_EQ(lines[1], "first line");
    EXPECT_EQ(lines[2], "second line");
}

TEST(StdioClientTransport, StderrCanBeInherited) {
    Harness harness;
    std::optional<mcp::ProcessExit> exit;
    auto options = server_options();
    options.stderr_mode = mcp::StderrMode::eInherit;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        client.close();
        exit = co_await transport->shutdown();
    });
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->code, 0);
}

TEST(StdioClientTransport, EnvironmentIsInheritedAndOverridden) {
    set_parent_variable("MCP_TEST_FROM_PARENT", "parent");
    set_parent_variable("MCP_TEST_OVERRIDDEN", "parent");
    auto options = server_options();
    options.environment = {{"MCP_TEST_OVERRIDDEN", "child"}, {"MCP_TEST_ADDED", "added"}};
    Harness harness;
    json inherited;
    json overridden;
    json added;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        inherited = co_await call(client, "env", arg("name", "MCP_TEST_FROM_PARENT"));
        overridden = co_await call(client, "env", arg("name", "MCP_TEST_OVERRIDDEN"));
        added = co_await call(client, "env", arg("name", "MCP_TEST_ADDED"));
        client.close();
        co_await transport->shutdown();
    });
    EXPECT_EQ(inherited["value"], "parent");
    EXPECT_EQ(overridden["value"], "child");
    EXPECT_EQ(added["value"], "added");
}

#if !defined(_WIN32)
TEST(StdioClientTransport, EnvironmentCanStartEmpty) {
    set_parent_variable("MCP_TEST_FROM_PARENT", "parent");
    auto options = server_options();
    options.inherit_environment = false;
    options.environment = {{"MCP_TEST_ADDED", "added"}};
    Harness harness;
    json inherited;
    json added;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        inherited = co_await call(client, "env", arg("name", "MCP_TEST_FROM_PARENT"));
        added = co_await call(client, "env", arg("name", "MCP_TEST_ADDED"));
        client.close();
        co_await transport->shutdown();
    });
    EXPECT_EQ(inherited["set"], false);
    EXPECT_EQ(added["value"], "added");
}
#endif

TEST(StdioClientTransport, WorkingDirectoryIsApplied) {
    const auto directory = std::filesystem::canonical(std::filesystem::temp_directory_path());
    auto options = server_options();
    options.working_directory = directory;
    Harness harness;
    json reported;
    harness.run([&]() -> mcp::Task<void> {
        auto transport =
            std::make_shared<mcp::StdioClientTransport>(harness.io.get_executor(), options);
        mcp::Client client(transport, harness.io.get_executor());
        co_await client.connect("stdio-client-test", "1.0");
        reported = co_await call(client, "cwd");
        client.close();
        co_await transport->shutdown();
    });
    EXPECT_TRUE(std::filesystem::equivalent(reported["cwd"].get<std::string>(), directory));
}

TEST(StdioClientTransport, LaunchFailuresAreReportedByTheConstructor) {
    Harness harness;
    auto missing = server_options();
    missing.command = "mcp-sdk-no-such-program";
    EXPECT_THROW(mcp::StdioClientTransport(harness.io.get_executor(), missing), std::runtime_error);

    auto no_directory = server_options();
    no_directory.working_directory = std::filesystem::path("mcp-sdk-no-such-directory") / "nested";
    EXPECT_THROW(mcp::StdioClientTransport(harness.io.get_executor(), no_directory),
                 std::runtime_error);

    auto empty = server_options();
    empty.command.clear();
    EXPECT_THROW(mcp::StdioClientTransport(harness.io.get_executor(), empty), std::invalid_argument);
}

TEST(StdioClientTransport, LineLongerThanTheLimitFailsTheRead) {
    Harness harness;
    std::string within;
    bool refused = false;
    harness.run([&]() -> mcp::Task<void> {
        auto options = server_options("write-raw", {"1000", "1"});
        options.max_message_bytes = 100;
        mcp::StdioClientTransport too_long(harness.io.get_executor(), options);
        try {
            co_await too_long.read_message();
        } catch (const std::runtime_error&) {
            refused = true;
        }
        auto fitting = server_options("write-raw", {"100", "1"});
        fitting.max_message_bytes = 100;
        mcp::StdioClientTransport transport(harness.io.get_executor(), fitting);
        within = co_await transport.read_message();
    });
    EXPECT_TRUE(refused);
    EXPECT_EQ(within, std::string(100, 'x'));
}

// The limit counts the message, not its line ending: a line ending in CRLF fits it as well.
TEST(StdioClientTransport, LineEndingInCrLfFitsTheSameLimit) {
    Harness harness;
    std::string line;
    harness.run([&]() -> mcp::Task<void> {
        auto options = server_options("write-raw", {"100", "2"});
        options.max_message_bytes = 100;
        mcp::StdioClientTransport transport(harness.io.get_executor(), options);
        line = co_await transport.read_message();
    });
    EXPECT_EQ(line, std::string(100, 'x'));
}

TEST(StdioClientTransport, FinalLineWithoutANewlineIsDeliveredBeforeTheEnd) {
    Harness harness;
    std::string last;
    bool ended = false;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientTransport transport(harness.io.get_executor(),
                                            server_options("write-raw", {"7", "0"}));
        last = co_await transport.read_message();
        try {
            co_await transport.read_message();
        } catch (const std::runtime_error&) {
            ended = true;
        }
    });
    EXPECT_EQ(last, std::string(7, 'x'));
    EXPECT_TRUE(ended);
}

#if !defined(_WIN32)

namespace {

/// Whether the process no longer exists (a zombie that nothing reaped yet counts as gone).
bool process_gone(pid_t pid) {
    if (::kill(pid, 0) != 0) {
        return errno == ESRCH;
    }
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(stat, line)) {
        const auto close = line.rfind(')');
        return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
    }
    return false;
}

pid_t read_pid_file(const std::filesystem::path& file) {
    std::ifstream in(file);
    long pid = 0;
    return in >> pid ? static_cast<pid_t>(pid) : 0;
}

/// Runs a grandchild-starting server, shuts it down, and reports whether the grandchild is gone.
bool grandchild_gone_after_close(bool own_process_tree) {
    Harness harness;
    const auto file =
        std::filesystem::temp_directory_path() /
        ("mcp-sdk-grandchild-" + std::to_string(::getpid()) + (own_process_tree ? "-tree" : "-alone"));
    std::filesystem::remove(file);
    auto options = server_options("grandchild", {file.string()});
    options.own_process_tree = own_process_tree;
    pid_t helper = 0;
    bool gone = false;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientTransport transport(harness.io.get_executor(), options);
        const bool written = co_await wait_until([&] {
            helper = read_pid_file(file);
            return helper > 0;
        });
        EXPECT_TRUE(written);
        co_await transport.shutdown();
        // A helper that is meant to survive is checked once; waiting for it would wait for ever.
        gone = own_process_tree ? co_await wait_until([&] { return process_gone(helper); })
                                : process_gone(helper);
        if (!gone) {
            // Do not leave the helper running.
            ::kill(helper, SIGKILL);
        }
    });
    std::filesystem::remove(file);
    return gone;
}

/// Sets SIGCHLD to ignored for its lifetime.
class IgnoreChildSignal {
   public:
    IgnoreChildSignal() {
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ::sigaction(SIGCHLD, &ignore, &previous_);
    }
    ~IgnoreChildSignal() { ::sigaction(SIGCHLD, &previous_, nullptr); }
    IgnoreChildSignal(const IgnoreChildSignal&) = delete;
    IgnoreChildSignal& operator=(const IgnoreChildSignal&) = delete;

   private:
    struct sigaction previous_{};
};

}  // namespace

TEST(StdioClientTransport, HelpersOfTheServerEndWithIt) {
    EXPECT_TRUE(grandchild_gone_after_close(true));
}

TEST(StdioClientTransport, HelpersSurviveWhenTheServerHasNoProcessTree) {
    EXPECT_FALSE(grandchild_gone_after_close(false));
}

TEST(StdioClientTransport, ExitIsReportedWhenTheApplicationIgnoresSigchld) {
    const IgnoreChildSignal ignore;
    Harness harness;
    std::mutex mutex;
    std::optional<mcp::ProcessExit> reported;
    std::optional<mcp::ProcessExit> shut_down;
    auto options = server_options("exit-at-once", {"3"});
    options.on_exit = [&](const mcp::ProcessExit& exit) {
        std::lock_guard lock(mutex);
        reported = exit;
    };
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientTransport transport(harness.io.get_executor(), options);
        const bool seen = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return reported.has_value();
        });
        EXPECT_TRUE(seen);
        shut_down = co_await transport.shutdown();
    });
    ASSERT_TRUE(reported.has_value());
    EXPECT_FALSE(reported->requested);
    EXPECT_FALSE(reported->signaled);
    ASSERT_TRUE(shut_down.has_value());
}

TEST(StdioClientTransport, DestroyingTheTransportWhileTheExecutorIsBusyLeavesNoProcessBehind) {
    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);
    std::thread runner([&io] { io.run(); });
    std::atomic<bool> blocked{false};
    auto transport = std::make_unique<mcp::StdioClientTransport>(io.get_executor(), server_options());
    const auto pid = static_cast<pid_t>(transport->process_id());
    // The executor is occupied for a while, so the strand cannot close the pipe at once.
    boost::asio::post(io, [&blocked] {
        blocked = true;
        std::this_thread::sleep_for(200ms);
    });
    while (!blocked) {
        std::this_thread::yield();
    }
    transport.reset();
    EXPECT_TRUE(process_gone(pid));
    work.reset();
    runner.join();
}

#endif

// ---------------------------------------------------------------------------
// StdioClientSession
// ---------------------------------------------------------------------------

namespace {

mcp::StdioSessionOptions session_options() {
    mcp::StdioSessionOptions options;
    options.process = server_options();
    options.client_info = mcp::Implementation{"session-test", "1.0"};
    return options;
}

}  // namespace

TEST(StdioClientSession, RestartsTheServerAndEstablishesSubscriptionsAgain) {
    Harness harness;
    auto options = session_options();
    options.restart.max_restarts = 1;
    options.restart.delay = 10ms;
    std::mutex mutex;
    std::vector<mcp::Subscription> subscriptions;
    options.on_connected = [&](mcp::Client& client, const mcp::ConnectResult&) -> mcp::Task<void> {
        mcp::SubscriptionFilter filter;
        filter.toolsListChanged = true;
        auto subscription = co_await client.listen(filter);
        std::lock_guard lock(mutex);
        subscriptions.push_back(std::move(subscription));
    };
    json before;
    json after;
    std::optional<int> failure_code;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientSession session(harness.io.get_executor(), options);
        co_await session.start();
        before = co_await call(*session.client(), "args");
        const auto first = session.client();
        try {
            co_await call(*first, "crash");
        } catch (const mcp::McpError& error) {
            failure_code = error.code();
        }
        const bool restarted = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return subscriptions.size() == 2;
        });
        EXPECT_TRUE(restarted);
        EXPECT_NE(session.client(), first);
        after = co_await call(*session.client(), "args");
        EXPECT_EQ(session.restart_count(), 1u);
        {
            // The first stream ended with its connection; the second one belongs to the new server.
            std::lock_guard lock(mutex);
            EXPECT_EQ(subscriptions.size(), 2u);
            if (subscriptions.size() == 2) {
                EXPECT_EQ(subscriptions[0].end_reason(), mcp::SubscriptionEnd::eDropped);
                EXPECT_FALSE(subscriptions[1].end_reason().has_value());
            }
        }
        co_await session.stop();
    });
    EXPECT_EQ(failure_code, jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_EQ(before, after);
}

TEST(StdioClientSession, DoesNotRestartWhenThePolicyAllowsNone) {
    Harness harness;
    auto options = session_options();
    std::mutex mutex;
    std::vector<std::string> failures;
    options.on_restart_failed = [&](const std::string& reason) {
        std::lock_guard lock(mutex);
        failures.push_back(reason);
    };
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientSession session(harness.io.get_executor(), options);
        co_await session.start();
        const auto client = session.client();
        try {
            co_await call(*client, "crash");
        } catch (const mcp::McpError&) {
        }
        const bool reported = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return !failures.empty();
        });
        EXPECT_TRUE(reported);
        EXPECT_EQ(session.restart_count(), 0u);
        EXPECT_EQ(session.client(), client);
        co_await session.stop();
    });
}

TEST(StdioClientSession, StoppingDoesNotRestart) {
    Harness harness;
    auto options = session_options();
    options.restart.max_restarts = 3;
    options.restart.delay = 10ms;
    std::mutex mutex;
    std::vector<mcp::ProcessExit> exits;
    options.process.on_exit = [&](const mcp::ProcessExit& exit) {
        std::lock_guard lock(mutex);
        exits.push_back(exit);
    };
    std::optional<mcp::ProcessExit> stopped;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientSession session(harness.io.get_executor(), options);
        co_await session.start();
        stopped = co_await session.stop();
        const bool reported = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return !exits.empty();
        });
        EXPECT_TRUE(reported);
        // Time for a restart that should not happen.
        boost::asio::steady_timer pause(co_await boost::asio::this_coro::executor);
        pause.expires_after(100ms);
        co_await pause.async_wait(boost::asio::use_awaitable);
        EXPECT_EQ(session.restart_count(), 0u);
        EXPECT_EQ(session.client(), nullptr);
    });
    ASSERT_TRUE(stopped.has_value());
    EXPECT_TRUE(stopped->requested);
    EXPECT_EQ(stopped->code, 0);
    std::lock_guard lock(mutex);
    EXPECT_EQ(exits.size(), 1u);
}

// The application may keep the Client after the session is gone; the server is shut down anyway,
// and the Client it holds is closed.
TEST(StdioClientSession, DestructorShutsTheServerDownWhileItsClientIsHeld) {
    Harness harness;
    auto options = session_options();
    std::mutex mutex;
    std::optional<mcp::ProcessExit> exit;
    options.process.on_exit = [&](const mcp::ProcessExit& ended) {
        std::lock_guard lock(mutex);
        exit = ended;
    };
    std::shared_ptr<mcp::Client> held;
    bool call_failed = false;
    harness.run([&]() -> mcp::Task<void> {
        {
            mcp::StdioClientSession session(harness.io.get_executor(), options);
            co_await session.start();
            held = session.client();
        }
        const bool ended = co_await wait_until([&] {
            std::lock_guard lock(mutex);
            return exit.has_value();
        });
        EXPECT_TRUE(ended);
        try {
            co_await held->list_tools();
        } catch (const std::exception&) {
            call_failed = true;
        }
    });
    EXPECT_TRUE(call_failed);
    std::lock_guard lock(mutex);
    ASSERT_TRUE(exit.has_value());
    EXPECT_TRUE(exit->requested);
}

TEST(StdioClientSession, StartFailsWhenTheServerCannotConnect) {
    Harness harness;
    auto options = session_options();
    options.process.arguments = {"exit-at-once", "5"};
    bool failed = false;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientSession session(harness.io.get_executor(), options);
        try {
            co_await session.start();
        } catch (const std::exception&) {
            failed = true;
        }
    });
    EXPECT_TRUE(failed);
}

TEST(StdioClientSession, FailedStartDoesNotRestartOrLeaveServersRunning) {
    Harness harness;
    auto options = session_options();
    options.process.arguments = {"exit-at-once", "5"};
    options.restart.max_restarts = 3;
    options.restart.delay = 10ms;
    std::mutex mutex;
    int launches = 0;
    options.process.on_exit = [&](const mcp::ProcessExit&) {
        std::lock_guard lock(mutex);
        ++launches;
    };
    bool failed = false;
    std::size_t restarts = 99;
    std::shared_ptr<mcp::Client> client;
    harness.run([&]() -> mcp::Task<void> {
        mcp::StdioClientSession session(harness.io.get_executor(), options);
        try {
            co_await session.start();
        } catch (const std::exception&) {
            failed = true;
        }
        // Time for a restart that should not happen.
        boost::asio::steady_timer pause(co_await boost::asio::this_coro::executor);
        pause.expires_after(200ms);
        co_await pause.async_wait(boost::asio::use_awaitable);
        restarts = session.restart_count();
        client = session.client();
    });
    EXPECT_TRUE(failed);
    EXPECT_EQ(restarts, 0u);
    EXPECT_EQ(client, nullptr);
    std::lock_guard lock(mutex);
    EXPECT_EQ(launches, 1);
}

TEST(StdioClientSession, CleanExitRestartsOnlyWhenThePolicyAsksForIt) {
    for (const bool after_clean_exit : {false, true}) {
        Harness harness;
        auto options = session_options();
        options.restart.max_restarts = 1;
        options.restart.delay = 10ms;
        options.restart.restart_after_clean_exit = after_clean_exit;
        std::mutex mutex;
        int exits = 0;
        options.process.on_exit = [&](const mcp::ProcessExit&) {
            std::lock_guard lock(mutex);
            ++exits;
        };
        std::size_t restarts = 0;
        harness.run([&]() -> mcp::Task<void> {
            mcp::StdioClientSession session(harness.io.get_executor(), options);
            co_await session.start();
            try {
                co_await call(*session.client(), "crash", arg("status", 0));
            } catch (const mcp::McpError&) {
            }
            const bool ended = co_await wait_until([&] {
                std::lock_guard lock(mutex);
                return exits >= 1;
            });
            EXPECT_TRUE(ended);
            if (after_clean_exit) {
                EXPECT_TRUE(co_await wait_until([&] { return session.restart_count() == 1; }));
            } else {
                // Time for a restart that should not happen.
                boost::asio::steady_timer pause(co_await boost::asio::this_coro::executor);
                pause.expires_after(200ms);
                co_await pause.async_wait(boost::asio::use_awaitable);
            }
            restarts = session.restart_count();
            co_await session.stop();
        });
        EXPECT_EQ(restarts, after_clean_exit ? 1u : 0u);
    }
}

}  // namespace transport_stdio_client_test
