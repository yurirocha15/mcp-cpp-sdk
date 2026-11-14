#pragma once

#include <mcp/transport/http_types.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/write.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mcp::detail {

/// Whether @p response can be written as its head followed by its body, the bytes Beast's
/// serializer writes for it: a final status that carries a body on a connection that stays open,
/// and a body that is not chunked and is exactly as long as its Content-Length says. Anything else
/// stays with Beast, as does the end of a connection.
inline bool writes_as_head_and_body(const StringResponse& response) {
    namespace http = boost::beast::http;
    if (!response.keep_alive()) {
        return false;
    }
    const auto status = response.result_int();
    constexpr unsigned g_first_final_status = 200;
    constexpr unsigned g_last_status = 999;
    if (status < g_first_final_status || status > g_last_status ||
        response.result() == http::status::no_content ||
        response.result() == http::status::not_modified) {
        return false;
    }
    constexpr unsigned g_http_10 = 10;
    constexpr unsigned g_http_11 = 11;
    if (response.version() != g_http_10 && response.version() != g_http_11) {
        return false;
    }
    if (response.find(http::field::transfer_encoding) != response.end()) {
        return false;
    }
    const auto length_field = response.find(http::field::content_length);
    if (length_field == response.end() || response.count(http::field::content_length) != 1) {
        return false;
    }
    const auto text = length_field->value();
    std::uint64_t length = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), length);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           length == response.body().size();
}

/// Fills @p head with the start line and the fields of @p response, byte for byte as Beast's
/// serializer writes them: `HTTP/x.y`, the three-digit status, the reason (Beast's own for the
/// status when none is set), each field as `name: value` in the order it was set, and a blank line.
inline void serialize_response_head(const StringResponse& response, std::vector<char>& head) {
    head.clear();
    const auto append = [&head](std::string_view text) {
        head.insert(head.end(), text.begin(), text.end());
    };
    const auto version = response.version();
    const auto status = response.result_int();
    constexpr unsigned g_ten = 10;
    constexpr unsigned g_hundred = 100;
    const std::array<char, 13> start = {'H',
                                        'T',
                                        'T',
                                        'P',
                                        '/',
                                        static_cast<char>('0' + version / g_ten),
                                        '.',
                                        static_cast<char>('0' + version % g_ten),
                                        ' ',
                                        static_cast<char>('0' + status / g_hundred),
                                        static_cast<char>('0' + (status / g_ten) % g_ten),
                                        static_cast<char>('0' + status % g_ten),
                                        ' '};
    append(std::string_view(start.data(), start.size()));
    const auto reason = response.reason();
    append(std::string_view(reason.data(), reason.size()));
    append("\r\n");
    for (const auto& field : response) {
        const auto name = field.name_string();
        const auto value = field.value();
        append(std::string_view(name.data(), name.size()));
        append(": ");
        append(std::string_view(value.data(), value.size()));
        append("\r\n");
    }
    append("\r\n");
}

/// Writes @p response to @p stream: as one write of its head, built in @p head, and its body when
/// writes_as_head_and_body() allows, and through Beast's serializer otherwise. The bytes are the
/// same either way. @p response and @p head must stay untouched until the write completes.
template <typename AsyncWriteStream>
auto async_write_response(AsyncWriteStream& stream, const StringResponse& response,
                          std::vector<char>& head) {
    if (!writes_as_head_and_body(response)) {
        return boost::beast::http::async_write(stream, response, boost::asio::use_awaitable);
    }
    serialize_response_head(response, head);
    const std::array<boost::asio::const_buffer, 2> buffers = {boost::asio::buffer(head),
                                                              boost::asio::buffer(response.body())};
    return boost::asio::async_write(stream, buffers, boost::asio::use_awaitable);
}

}  // namespace mcp::detail
