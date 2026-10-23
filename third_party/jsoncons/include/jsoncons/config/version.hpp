// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_CONFIG_VERSION_HPP
#define MCP_DETAIL_JSONCONS_CONFIG_VERSION_HPP
 
#include <ostream>
    
#define MCP_DETAIL_JSONCONS_VERSION_MAJOR 1
#define MCP_DETAIL_JSONCONS_VERSION_MINOR 10
#define MCP_DETAIL_JSONCONS_VERSION_PATCH 0

#define MCP_DETAIL_JSONCONS_VERSION_CONCAT_EX(major, minor, patch) \
    # major ## "." ## # minor ## "." ## # patch

#define MCP_DETAIL_JSONCONS_VERSION_CONCAT(major, minor, patch) \
    MCP_DETAIL_JSONCONS_VERSION_CONCAT_EX(major, minor, patch)

#define MCP_DETAIL_JSONCONS_VERSION_STRING MCP_DETAIL_JSONCONS_PP_STRINGIFY(MCP_DETAIL_JSONCONS_VERSION_MAJOR) "." MCP_DETAIL_JSONCONS_PP_STRINGIFY(MCP_DETAIL_JSONCONS_VERSION_MINOR) "." MCP_DETAIL_JSONCONS_PP_STRINGIFY(MCP_DETAIL_JSONCONS_VERSION_PATCH)

namespace mcp_detail_jsoncons {

struct versioning_info
{
    unsigned int const major;
    unsigned int const minor;
    unsigned int const patch;

    friend std::ostream& operator<<(std::ostream& os, const versioning_info& ver)
    {
        os << ver.major << '.'
           << ver.minor << '.'
           << ver.patch;
        return os;
    } 
}; 

constexpr versioning_info version()
{
    return versioning_info{MCP_DETAIL_JSONCONS_VERSION_MAJOR, MCP_DETAIL_JSONCONS_VERSION_MINOR, MCP_DETAIL_JSONCONS_VERSION_PATCH};
}

} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_CONFIG_VERSION_HPP
