// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_JSON_FWD_HPP
#define MCP_DETAIL_JSONCONS_JSON_FWD_HPP

#include <memory> // std::allocator

namespace mcp_detail_jsoncons {

struct sorted_policy;
                        
template <typename CharT, 
          typename Policy = sorted_policy, 
          typename Allocator = std::allocator<CharT>>
class basic_json;

} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_JSON_FWD_HPP
