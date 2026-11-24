#include "mcp/detail/net/network_failure.hpp"

#include <mcp/core/async.hpp>
#include <mcp/transport/transport.hpp>

#include <boost/asio/error.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/websocket/error.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

Task<IncomingMessage> ITransport::read_incoming() {
    auto message = co_await read_message();
    co_return IncomingMessage{.message = std::move(message), .principal = {}};
}

Task<void> ITransport::write_related_message(std::string_view message,
                                             std::string_view related_request) {
    static_cast<void>(related_request);
    co_await write_message(message);
}

namespace detail {

namespace {

/// Whether @p code is @p expected. Boost compares the categories of Asio's and Beast's own errors,
/// which have no id, by address, and a code made in another module of the process can hold another
/// instance of its category than this library's: on Windows each DLL and program has its own. The
/// category's name and the value are the same in every module.
bool is_error(const boost::system::error_code& code, const boost::system::error_code& expected) {
    return code.value() == expected.value() &&
           std::strcmp(code.category().name(), expected.category().name()) == 0;
}

/// Whether @p code says the peer closed or reset the connection, between messages or in the
/// middle of one.
bool is_connection_end(const boost::system::error_code& code) {
    namespace error = boost::asio::error;
    using http_error = boost::beast::http::error;
    for (const boost::system::error_code end : {
             boost::system::error_code(error::eof),
             boost::system::error_code(error::connection_reset),
             boost::system::error_code(error::connection_aborted),
             boost::system::error_code(error::broken_pipe),
             boost::system::error_code(error::not_connected),
             boost::system::error_code(http_error::end_of_stream),
             boost::system::error_code(http_error::partial_message),
             boost::system::error_code(boost::beast::websocket::error::closed),
         }) {
        if (is_error(code, end)) {
            return true;
        }
    }
    return false;
}

/// Whether @p code says a message was larger than the transport's limit.
bool is_message_too_large(const boost::system::error_code& code) {
    return is_error(code, boost::beast::websocket::error::message_too_big) ||
           is_error(code, boost::beast::http::error::body_limit);
}

}  // namespace

TransportError network_failure(const boost::system::error_code& code, TransportError::Kind kind,
                               std::string_view transport_name) {
    const std::string name(transport_name);
    if (is_error(code, boost::asio::error::operation_aborted)) {
        return {TransportError::Kind::eClosed, name + " is closed", code};
    }
    if (kind == TransportError::Kind::eConnect) {
        return {kind, name + " could not connect: " + code.message(), code};
    }
    if (is_connection_end(code)) {
        return {TransportError::Kind::eClosed, name + " lost its connection: " + code.message(), code};
    }
    if (is_message_too_large(code)) {
        return {TransportError::Kind::eProtocol,
                name + " received a message larger than its limit: " + code.message(), code};
    }
    return {kind, name + " failed to read or write: " + code.message(), code};
}

std::exception_ptr as_transport_failure(const std::exception_ptr& failure,
                                        std::string_view transport_name) {
    if (!failure) {
        return failure;
    }
    try {
        std::rethrow_exception(failure);
    } catch (const ConnectFailure& error) {
        return std::make_exception_ptr(
            network_failure(error.code(), TransportError::Kind::eConnect, transport_name));
    } catch (const boost::system::system_error& error) {
        return std::make_exception_ptr(
            network_failure(error.code(), TransportError::Kind::eIo, transport_name));
    } catch (...) {
        // Any other exception is already the transport's own, and goes on as it is.
        return failure;
    }
}

}  // namespace detail

}  // namespace mcp
