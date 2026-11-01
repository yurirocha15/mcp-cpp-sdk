/// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons2 for latest version

#ifndef MCP_DETAIL_JSONCONS_NONSTD_UTILITY_HPP    
#define MCP_DETAIL_JSONCONS_NONSTD_UTILITY_HPP    

#include <utility>
#include <jsoncons/nonstd/compiler_support.hpp>
#include <iostream>

namespace mcp_detail_jsoncons {
namespace nonstd {
    
#if (defined(MCP_DETAIL_JSONCONS_HAS_2017))

    using in_place_t = std::in_place_t;

#else
    
    struct in_place_t
    {
        explicit in_place_t() = default; 
    };

#endif

MCP_DETAIL_JSONCONS_INLINE_CONSTEXPR in_place_t in_place{};

} // namespace nonstd
} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_NONSTD_UTILITY_HPP
