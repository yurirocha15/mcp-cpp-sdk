#include <mcp/client/stdio_session.hpp>

#include "../detail/spawn_awaitable.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mcp {

struct StdioClientSession::Impl : std::enable_shared_from_this<Impl> {
    Impl(const boost::asio::any_io_executor& executor, StdioSessionOptions session_options)
        : strand(boost::asio::make_strand(executor)), options(std::move(session_options)) {}

    struct Started {
        std::shared_ptr<StdioClientTransport> transport;
        std::shared_ptr<Client> client;
    };

    /// Starts a process and a Client for it. Not a coroutine, so that none of the strings it handles
    /// stay in a coroutine frame.
    static Started start_process(const std::shared_ptr<Impl>& self, std::uint64_t generation) {
        auto process_options = self->options.process;
        const std::weak_ptr<Impl> weak = self->weak_from_this();
        auto user_exit = std::move(process_options.on_exit);
        process_options.on_exit = [weak, generation,
                                   user_exit = std::move(user_exit)](const ProcessExit& exit) {
            if (user_exit) {
                user_exit(exit);
            }
            if (exit.requested) {
                return;
            }
            auto session = weak.lock();
            // A server that ends before its connection is set up is not restarted: launch() fails
            // the start, or notices the exit when it finishes connecting.
            if (!session || session->connected_generation != generation ||
                !session->restart_wanted(exit)) {
                return;
            }
            boost::asio::co_spawn(session->strand, restart(session, generation), boost::asio::detached);
        };
        Started started;
        started.transport =
            std::make_shared<StdioClientTransport>(self->strand, std::move(process_options));
        started.client =
            std::make_shared<Client>(started.transport, self->strand, self->options.client);
        if (self->options.configure) {
            self->options.configure(*started.client);
        }
        return started;
    }

    /// Whether the restart policy covers a process that ended on its own as @p exit says: an
    /// abnormal end (a non-zero status or a signal), or any end when the policy includes clean ones.
    [[nodiscard]] bool restart_wanted(const ProcessExit& exit) const {
        return options.restart.restart_after_clean_exit || exit.signaled || exit.code != 0;
    }

    /// Starts a process and connects a Client to it, replacing the current ones. Runs on the strand.
    static Task<ConnectResult> launch(std::shared_ptr<Impl> self) {
        const auto generation = ++self->generation;
        const auto started = start_process(self, generation);
        const auto transport = started.transport;
        const auto client = started.client;
        {
            // The previous client, whose server has ended, is released outside the lock.
            std::shared_ptr<Client> previous;
            std::shared_ptr<StdioClientTransport> previous_transport;
            {
                std::lock_guard lock(self->mutex);
                previous = std::move(self->current_client);
                previous_transport = std::move(self->transport);
                self->transport = transport;
                self->current_client = client;
            }
        }
        const auto connected = std::make_shared<ConnectResult>();
        std::exception_ptr failure;
        try {
            *connected =
                co_await client->connect(self->options.client_info, self->options.capabilities);
            if (self->options.on_connected) {
                co_await self->options.on_connected(*client, *connected);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            // A server that cannot be connected to is not left running, and nothing refers to it.
            client->close();
            co_await transport->shutdown();
            {
                std::lock_guard lock(self->mutex);
                if (self->transport == transport) {
                    self->transport.reset();
                    self->current_client.reset();
                }
            }
            std::rethrow_exception(failure);
        }
        self->connected_generation = generation;
        co_return *connected;
    }

    /// The first start. A server that ended while it was being connected is started again as the
    /// policy says.
    static Task<ConnectResult> first_start(std::shared_ptr<Impl> self) {
        auto connected = co_await launch(self);
        const auto exit = self->current_exit();
        if (exit && self->restart_wanted(*exit)) {
            boost::asio::co_spawn(self->strand, restart(self, self->generation), boost::asio::detached);
        }
        co_return connected;
    }

    /// Starts the server again after generation @p ended ended without being asked to.
    static Task<void> restart(std::shared_ptr<Impl> self, std::uint64_t ended) {
        if (self->stopping || ended != self->generation || self->restarting) {
            co_return;
        }
        // One restart loop runs at a time.
        self->restarting = true;
        struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{self->restarting};
        for (;;) {
            if (self->restarts >= self->options.restart.max_restarts) {
                if (self->options.on_restart_failed) {
                    self->options.on_restart_failed("the restart policy allows no more restarts");
                }
                co_return;
            }
            ++self->restarts;
            auto timer = std::make_shared<boost::asio::steady_timer>(self->strand);
            timer->expires_after(self->options.restart.delay);
            boost::system::error_code ignored;
            co_await timer->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
            if (self->stopping) {
                co_return;
            }
            try {
                co_await launch(self);
                // A server that ended while it was being connected needs another start.
                const auto exit = self->current_exit();
                if (!exit || !self->restart_wanted(*exit)) {
                    co_return;
                }
            } catch (const std::exception& error) {
                if (self->options.on_restart_failed &&
                    self->restarts >= self->options.restart.max_restarts) {
                    self->options.on_restart_failed(error.what());
                    co_return;
                }
            }
        }
    }

    /// How the current server ended, if it has ended on its own.
    [[nodiscard]] std::optional<ProcessExit> current_exit() const {
        std::shared_ptr<StdioClientTransport> current;
        {
            std::lock_guard lock(mutex);
            current = transport;
        }
        if (!current) {
            return std::nullopt;
        }
        auto exit = current->exit_status();
        if (exit && exit->requested) {
            return std::nullopt;
        }
        return exit;
    }

    static Task<std::optional<ProcessExit>> stop(std::shared_ptr<Impl> self) {
        self->stopping = true;
        std::shared_ptr<Client> client;
        std::shared_ptr<StdioClientTransport> transport;
        {
            std::lock_guard lock(self->mutex);
            client = std::move(self->current_client);
            transport = std::move(self->transport);
        }
        std::optional<ProcessExit> exit;
        if (transport) {
            exit = co_await transport->shutdown();
        }
        if (client) {
            client->close();
        }
        co_return exit;
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    StdioSessionOptions options;
    mutable std::mutex mutex;
    std::shared_ptr<Client> current_client;
    std::shared_ptr<StdioClientTransport> transport;
    std::uint64_t generation = 0;
    /// The generation whose connection was set up last; only strand code writes it.
    std::atomic<std::uint64_t> connected_generation{0};
    bool restarting = false;
    std::atomic<std::size_t> restarts{0};
    std::atomic<bool> stopping{false};
};

StdioClientSession::StdioClientSession(const boost::asio::any_io_executor& executor,
                                       StdioSessionOptions options) {
    if (options.process.command.empty()) {
        throw std::invalid_argument("StdioProcessOptions::command must not be empty");
    }
    impl_ = std::make_shared<Impl>(executor, std::move(options));
}

StdioClientSession::~StdioClientSession() {
    impl_->stopping = true;
    std::shared_ptr<Client> client;
    std::shared_ptr<StdioClientTransport> transport;
    {
        std::lock_guard lock(impl_->mutex);
        client = std::move(impl_->current_client);
        transport = std::move(impl_->transport);
    }
    // Closed rather than released: a Client the application still holds keeps both alive.
    if (transport) {
        transport->close();
    }
    if (client) {
        client->close();
    }
}

Task<ConnectResult> StdioClientSession::start() {
    auto impl = impl_;
    co_return co_await detail::spawn_awaitable(impl->strand, Impl::first_start(impl));
}

std::shared_ptr<Client> StdioClientSession::client() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current_client;
}

std::size_t StdioClientSession::restart_count() const { return impl_->restarts; }

Task<std::optional<ProcessExit>> StdioClientSession::stop() {
    auto impl = impl_;
    co_return co_await detail::spawn_awaitable(impl->strand, Impl::stop(impl));
}

}  // namespace mcp
