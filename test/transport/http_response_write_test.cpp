#include "../../src/detail/http_response_write.hpp"

#include <mcp/core/core.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace http = boost::beast::http;
using mcp::StringResponse;

namespace {

constexpr unsigned g_http_10 = 10;
constexpr unsigned g_http_11 = 11;

/// What Beast's serializer writes for @p response.
std::string beast_text(const StringResponse& response) {
    std::ostringstream text;
    text << response;
    return text.str();
}

/// The head serialize_response_head() builds for @p response, followed by its body.
std::string head_and_body_text(const StringResponse& response) {
    std::vector<char> head;
    mcp::detail::serialize_response_head(response, head);
    return std::string(head.begin(), head.end()) + response.body();
}

StringResponse json_response(http::status status, unsigned version, std::string body) {
    StringResponse response{status, version};
    response.set(http::field::server, "mcp-cpp-sdk");
    response.set(http::field::content_type, "application/json");
    response.keep_alive(true);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

enum class Writer : std::uint8_t {
    eHelper,
    eBeast
};

/// What a peer reads when @p responses are written one after another on one loopback connection,
/// whose stream runs on a strand when @p on_strand, by async_write_response() or by Beast's own
/// http::async_write(). The peer reads while the answers are written.
std::string bytes_read(const std::vector<StringResponse>& responses, Writer writer, bool on_strand) {
    namespace asio = boost::asio;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto accepted = acceptor.accept();
    const asio::any_io_executor executor =
        on_strand ? asio::any_io_executor(asio::make_strand(io)) : io.get_executor();
    boost::beast::tcp_stream server(
        asio::ip::tcp::socket(executor, asio::ip::tcp::v4(), accepted.release()));
    std::vector<char> received;
    std::exception_ptr failure;
    asio::co_spawn(
        executor,
        [&]() -> mcp::Task<void> {
            // One head buffer for every answer, as a connection keeps it.
            std::vector<char> head;
            for (const auto& response : responses) {
                if (writer == Writer::eHelper) {
                    co_await mcp::detail::async_write_response(server, response, head);
                } else {
                    co_await http::async_write(server, response, asio::use_awaitable);
                }
            }
            server.socket().shutdown(asio::ip::tcp::socket::shutdown_send);
        },
        [&failure](std::exception_ptr error) { failure = std::move(error); });
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            boost::system::error_code error;
            co_await asio::async_read(client, asio::dynamic_buffer(received),
                                      asio::redirect_error(asio::use_awaitable, error));
        },
        asio::detached);
    io.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return {received.begin(), received.end()};
}

/// The error a write of @p response fails with when the peer has reset the connection.
boost::system::error_code error_writing_to_reset_peer(const StringResponse& response, Writer writer) {
    namespace asio = boost::asio;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    boost::beast::tcp_stream server(acceptor.accept());
    // A zero linger makes the close send a reset rather than a FIN.
    client.set_option(asio::socket_base::linger(true, 0));
    client.close();
    boost::system::error_code result;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            std::vector<char> head;
            try {
                if (writer == Writer::eHelper) {
                    co_await mcp::detail::async_write_response(server, response, head);
                } else {
                    co_await http::async_write(server, response, asio::use_awaitable);
                }
            } catch (const boost::system::system_error& error) {
                result = error.code();
            }
        },
        asio::detached);
    io.run();
    return result;
}

}  // namespace

TEST(HttpResponseWriteTest, HeadAndBodyMatchBeastForTheAnswersTheServersSend) {
    std::vector<StringResponse> responses;
    responses.push_back(json_response(http::status::ok, g_http_11, R"({"jsonrpc":"2.0","id":1})"));
    responses.push_back(json_response(http::status::accepted, g_http_11, ""));
    responses.push_back(json_response(http::status::bad_request, g_http_11, R"({"error":"x"})"));
    responses.push_back(json_response(http::status::not_found, g_http_10, "{}"));
    responses.push_back(json_response(http::status::service_unavailable, g_http_11, "busy"));
    auto sse = json_response(http::status::ok, g_http_11, "id: 1\ndata: {}\n\n");
    sse.set(http::field::content_type, "text/event-stream");
    sse.set(http::field::cache_control, "no-cache");
    sse.set("Mcp-Session-Id", "0123456789abcdef0123456789abcdef");
    responses.push_back(sse);
    // A status Beast has no name for, which it writes with a reason of its own.
    constexpr unsigned g_unnamed_status = 299;
    auto unknown_status = json_response(http::status::ok, g_http_11, "odd");
    unknown_status.result(g_unnamed_status);
    responses.push_back(unknown_status);
    auto custom_reason = json_response(http::status::ok, g_http_11, "fine");
    custom_reason.reason("All Good");
    responses.push_back(custom_reason);
    auto repeated = json_response(http::status::method_not_allowed, g_http_11, "no");
    repeated.insert(http::field::allow, "POST");
    repeated.insert(http::field::allow, "DELETE");
    repeated.insert("X-Custom", "");
    responses.push_back(repeated);
    for (const auto& response : responses) {
        ASSERT_TRUE(mcp::detail::writes_as_head_and_body(response)) << beast_text(response);
        EXPECT_EQ(head_and_body_text(response), beast_text(response));
    }
}

TEST(HttpResponseWriteTest, LeavesToBeastWhatItDoesNotWriteAsHeadAndBody) {
    auto closing = json_response(http::status::ok, g_http_11, "{}");
    closing.keep_alive(false);
    auto no_content = json_response(http::status::no_content, g_http_11, "");
    auto not_modified = json_response(http::status::not_modified, g_http_11, "");
    auto chunked = json_response(http::status::ok, g_http_11, "{}");
    chunked.chunked(true);
    auto wrong_length = json_response(http::status::ok, g_http_11, "{}");
    wrong_length.set(http::field::content_length, "5");
    auto no_length = json_response(http::status::ok, g_http_11, "{}");
    no_length.erase(http::field::content_length);
    auto two_lengths = json_response(http::status::ok, g_http_11, "{}");
    two_lengths.insert(http::field::content_length, "2");
    auto http2 = json_response(http::status::ok, g_http_11, "{}");
    constexpr unsigned g_http_20 = 20;
    http2.version(g_http_20);
    for (const auto* response : {&closing, &no_content, &not_modified, &chunked, &wrong_length,
                                 &no_length, &two_lengths, &http2}) {
        EXPECT_FALSE(mcp::detail::writes_as_head_and_body(*response)) << beast_text(*response);
    }
}

TEST(HttpResponseWriteTest, WritesTheBytesBeastWritesOverAConnection) {
    // Bodies around the sizes where buffers and socket writes split: empty, page and 64 KiB
    // boundaries, and one larger than a loopback socket's send buffer.
    const std::vector<std::size_t> body_sizes = {0,     1,     4095,  4096,    4097,
                                                 65535, 65536, 65537, 1048577, 8388609};
    for (const auto size : body_sizes) {
        auto response = json_response(http::status::ok, g_http_11, std::string(size, 'x'));
        response.set("Mcp-Session-Id", "abc");
        for (const bool on_strand : {false, true}) {
            const std::vector<StringResponse> one = {response};
            const auto written = bytes_read(one, Writer::eHelper, on_strand);
            EXPECT_GT(written.size(), size);
            EXPECT_EQ(written, bytes_read(one, Writer::eBeast, on_strand)) << size;
        }
    }
}

TEST(HttpResponseWriteTest, WritesEachAnswerOfAKeptAliveConnectionAsBeastDoes) {
    // A connection reuses one head buffer; answers of either path follow one another.
    constexpr std::size_t g_past_64_kib = 70000;
    const std::vector<StringResponse> answers = {
        json_response(http::status::ok, g_http_11, R"({"jsonrpc":"2.0","id":1,"result":{}})"),
        json_response(http::status::accepted, g_http_11, ""),
        json_response(http::status::no_content, g_http_11, ""),
        json_response(http::status::ok, g_http_11, std::string(g_past_64_kib, 'y')),
        json_response(http::status::not_found, g_http_11, "{}")};
    for (const bool on_strand : {false, true}) {
        const auto written = bytes_read(answers, Writer::eHelper, on_strand);
        EXPECT_EQ(written, bytes_read(answers, Writer::eBeast, on_strand));

        // The peer reads them back as the same answers, in order.
        boost::beast::flat_buffer buffer;
        buffer.commit(
            boost::asio::buffer_copy(buffer.prepare(written.size()), boost::asio::buffer(written)));
        for (const auto& answer : answers) {
            http::response_parser<http::string_body> parser;
            // Without eager, put() stops after the header.
            parser.eager(true);
            boost::system::error_code error;
            buffer.consume(parser.put(buffer.data(), error));
            ASSERT_FALSE(error) << error.message();
            ASSERT_TRUE(parser.is_done());
            EXPECT_EQ(parser.get().result(), answer.result());
            EXPECT_EQ(parser.get().body(), answer.body());
        }
        EXPECT_EQ(buffer.size(), 0U);
    }
}

TEST(HttpResponseWriteTest, AnswerToAHeadRequestIsWrittenAsBeastWritesIt) {
    // Neither path knows the request was HEAD: the answer is written as it was built.
    auto with_body = json_response(http::status::method_not_allowed, g_http_11, R"({"error":"x"})");
    with_body.set(http::field::allow, "POST");
    StringResponse without_body{http::status::ok, g_http_11};
    without_body.keep_alive(true);
    without_body.content_length(42);
    for (const auto& response : {with_body, without_body}) {
        const std::vector<StringResponse> one = {response};
        EXPECT_EQ(bytes_read(one, Writer::eHelper, false), bytes_read(one, Writer::eBeast, false));
    }
}

TEST(HttpResponseWriteTest, WriteToAResetPeerFailsAsBeastsDoes) {
    const auto response = json_response(http::status::ok, g_http_11, std::string(4 << 20, 'z'));
    const auto helper_error = error_writing_to_reset_peer(response, Writer::eHelper);
    const auto beast_error = error_writing_to_reset_peer(response, Writer::eBeast);
    EXPECT_TRUE(helper_error);
    EXPECT_EQ(helper_error, beast_error) << helper_error.message() << " / " << beast_error.message();
}
