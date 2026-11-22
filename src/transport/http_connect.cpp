#include "../detail/http_connect.hpp"

#include "../detail/constants.hpp"
#include "../detail/diagnostic_text.hpp"
#include "../detail/socket_deadline.hpp"
#include "../detail/tls.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace mcp::detail {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

/// The CONNECT exchange with a proxy. Held on the heap rather than in the coroutine frame: GCC 11
/// mishandles a std::string that lives across a suspension point there.
struct Tunnel {
    http::request<http::empty_body> request;
    beast::flat_buffer buffer;
    http::response_parser<http::empty_body> parser;
};

}  // namespace

std::string ConnectPlan::request_target(const std::string& path) const {
    if (!forwards_requests()) {
        return path;
    }
    return "http://" + authority_of(host, port, false) + path;
}

std::shared_ptr<Connection> make_connection(const net::any_io_executor& executor,
                                            const ConnectPlan& plan) {
    if (plan.https) {
        const auto& context = plan.tls ? plan.tls : default_client_context();
        return std::make_shared<Connection>(executor, TlsContextAccess::native(*context));
    }
    return std::make_shared<Connection>(executor);
}

Task<void> establish(std::shared_ptr<Connection> connection,
                     std::vector<net::ip::tcp::endpoint> endpoints,
                     std::shared_ptr<const ConnectPlan> plan) {
    const SocketDeadline deadline(connection->get_executor(), connection->socket(),
                                  std::chrono::seconds(constants::http_client::g_TIMEOUT_SECONDS));
    try {
        co_await net::async_connect(connection->socket(), endpoints, net::use_awaitable);
    } catch (const boost::system::system_error&) {
        if (deadline.expired()) {
            throw boost::system::system_error(beast::error::timeout);
        }
        throw;
    }
    // The wait for a response has a long limit, so this is what finds a peer that vanished.
    boost::system::error_code keep_alive_error;
    connection->socket().set_option(net::socket_base::keep_alive(true), keep_alive_error);

    if (plan->proxy && plan->https) {
        auto tunnel = std::make_shared<Tunnel>();
        const auto authority = authority_of(plan->host, plan->port, true);
        tunnel->request = http::request<http::empty_body>(http::verb::connect,
                                                          plan->host.find(':') != std::string::npos
                                                              ? "[" + plan->host + "]:" + plan->port
                                                              : plan->host + ":" + plan->port,
                                                          constants::http_client::g_VERSION_11);
        tunnel->request.set(http::field::host, tunnel->request.target());
        if (!plan->proxy->authorization.empty()) {
            tunnel->request.set(http::field::proxy_authorization, plan->proxy->authorization);
        }
        // A 2xx answer to CONNECT has no body: the tunnel starts right after its header.
        tunnel->parser.skip(true);
        try {
            co_await http::async_write(connection->tcp(), tunnel->request, net::use_awaitable);
            co_await http::async_read_header(connection->tcp(), tunnel->buffer, tunnel->parser,
                                             net::use_awaitable);
        } catch (const boost::system::system_error&) {
            if (deadline.expired()) {
                throw boost::system::system_error(beast::error::timeout);
            }
            throw;
        }
        const auto status = tunnel->parser.get().result_int();
        if (status < 200 || status > 299) {
            throw std::runtime_error("The proxy refused the tunnel to " +
                                     sanitize_for_diagnostics(authority) + " with status " +
                                     std::to_string(status));
        }
        if (tunnel->buffer.size() != 0) {
            throw std::runtime_error("The proxy sent data before the tunnel to " +
                                     sanitize_for_diagnostics(authority) + " opened");
        }
    }

    if (auto* tls = connection->tls()) {
        prepare_client_ssl(tls->native_handle(), plan->server_name);
        std::exception_ptr failure;
        boost::system::error_code error;
        try {
            co_await tls->async_handshake(net::ssl::stream_base::client, net::use_awaitable);
        } catch (const boost::system::system_error& handshake_error) {
            error = handshake_error.code();
            failure = std::current_exception();
        }
        if (failure) {
            throw handshake_failure(error, tls->native_handle(), plan->server_name, deadline.expired());
        }
        connection->mark_established();
    }
}

}  // namespace mcp::detail
