#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <functional>
#include <mcp/core/async.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace mcp_test {

namespace http = boost::beast::http;

/// Loopback HTTP server that answers each request with a scripted handler and records what it
/// received, for tests of OAuth flows against a mock authorization server.
class LoopbackServer final {
   public:
    using Handler =
        std::function<http::response<http::string_body>(const http::request<http::string_body>&)>;

    explicit LoopbackServer(boost::asio::io_context& io_ctx)
        : acceptor_(io_ctx, {boost::asio::ip::make_address("127.0.0.1"), 0}) {}

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
    }

    void set_handler(Handler handler) { handler_ = std::move(handler); }

    [[nodiscard]] const std::vector<std::string>& targets() const { return targets_; }
    [[nodiscard]] const std::vector<std::string>& bodies() const { return bodies_; }
    [[nodiscard]] const std::vector<std::string>& authorizations() const { return authorizations_; }

    /// Serves at most @p request_budget requests, then stops. close() ends a pending accept, so the
    /// io_context always drains.
    boost::asio::awaitable<void> serve(int request_budget) {
        for (int index = 0; index < request_budget; ++index) {
            boost::system::error_code accept_error;
            auto socket = co_await acceptor_.async_accept(
                boost::asio::redirect_error(boost::asio::use_awaitable, accept_error));
            if (accept_error) {
                co_return;
            }

            boost::beast::tcp_stream stream(std::move(socket));
            boost::beast::flat_buffer buffer;
            http::request<http::string_body> request;
            boost::system::error_code read_error;
            co_await http::async_read(
                stream, buffer, request,
                boost::asio::redirect_error(boost::asio::use_awaitable, read_error));
            if (read_error) {
                co_return;
            }

            targets_.emplace_back(request.target());
            bodies_.push_back(request.body());
            authorizations_.emplace_back(request[http::field::authorization]);

            auto response = handler_(request);
            response.version(request.version());
            response.prepare_payload();
            boost::system::error_code write_error;
            co_await http::async_write(
                stream, response, boost::asio::redirect_error(boost::asio::use_awaitable, write_error));

            boost::beast::error_code shutdown_error;
            (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, shutdown_error);
        }
    }

    void close() {
        boost::system::error_code ignored;
        (void)acceptor_.close(ignored);
    }

   private:
    boost::asio::ip::tcp::acceptor acceptor_;
    Handler handler_;
    std::vector<std::string> targets_;
    std::vector<std::string> bodies_;
    std::vector<std::string> authorizations_;
};

inline http::response<http::string_body> json_response(const nlohmann::json& body) {
    http::response<http::string_body> response{http::status::ok, 11};
    response.set(http::field::content_type, "application/json");
    response.body() = body.dump();
    return response;
}

inline http::response<http::string_body> status_response(http::status status) {
    http::response<http::string_body> response{status, 11};
    response.body() = "{}";
    return response;
}

/// The value of @p name in an `application/x-www-form-urlencoded` body, percent-decoded; empty when
/// the body has no such field.
inline std::string form_value(const std::string& body, const std::string& name) {
    const auto decode = [](const std::string& text) {
        std::string decoded;
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == '+') {
                decoded.push_back(' ');
            } else if (text[index] == '%' && index + 2 < text.size()) {
                decoded.push_back(static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16)));
                index += 2;
            } else {
                decoded.push_back(text[index]);
            }
        }
        return decoded;
    };
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find('&', start);
        const auto pair =
            body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto equals = pair.find('=');
        if (equals != std::string::npos && decode(pair.substr(0, equals)) == name) {
            return decode(pair.substr(equals + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

/// Whether the form body has a field called @p name.
inline bool form_has(const std::string& body, const std::string& name) {
    return body.rfind(name + "=", 0) == 0 || body.find("&" + name + "=") != std::string::npos;
}

}  // namespace mcp_test
