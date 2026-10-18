// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_JSON_LITERALS_HPP
#define MCP_DETAIL_JSONCONS_JSON_LITERALS_HPP

#include <string> // std::string

#include <jsoncons/nonstd/compiler_support.hpp>
#include <jsoncons/config/jsoncons_config.hpp>

namespace mcp_detail_jsoncons { 

template <typename CharT>
struct json_literals
{
    static constexpr mcp_detail_jsoncons::basic_string_view<CharT> true_literal = mcp_detail_jsoncons::basic_string_view<CharT>{MCP_DETAIL_JSONCONS_CSTRING_CONSTANT(CharT, "true"),4};
    static constexpr mcp_detail_jsoncons::basic_string_view<CharT> false_literal = mcp_detail_jsoncons::basic_string_view<CharT>{MCP_DETAIL_JSONCONS_CSTRING_CONSTANT(CharT, "false"),5};
    static constexpr mcp_detail_jsoncons::basic_string_view<CharT> null_literal = mcp_detail_jsoncons::basic_string_view<CharT>{MCP_DETAIL_JSONCONS_CSTRING_CONSTANT(CharT, "null"),4};
};
#if __cplusplus >= 201703L
// not needed for C++17
#else
    template <typename CharT> constexpr mcp_detail_jsoncons::basic_string_view<CharT> json_literals<CharT>::true_literal;
    template <typename CharT> constexpr mcp_detail_jsoncons::basic_string_view<CharT> json_literals<CharT>::false_literal;
    template <typename CharT> constexpr mcp_detail_jsoncons::basic_string_view<CharT> json_literals<CharT>::null_literal;
#endif

} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_JSON_LITERALS_HPP
