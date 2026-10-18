/// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_READ_ERROR_HPP
#define MCP_DETAIL_JSONCONS_READ_ERROR_HPP

#include <string> 
#include <system_error>
#include <type_traits>

#include <jsoncons/config/jsoncons_config.hpp>

namespace mcp_detail_jsoncons { 

enum class read_errc
{
    success = 0
};

class read_error_category_impl
   : public std::error_category
{
public:
    const char* name() const noexcept override
    {
        return "jsoncons/read_error";
    }
    std::string message(int ev) const override
    {
        switch (static_cast<read_errc>(ev))
        {
            case read_errc::success:
                return "Success";
            default:
                return "Unknown read error";
        }
    }
};

inline
const std::error_category& read_error_category() noexcept
{
  static read_error_category_impl instance;
  return instance;
}

inline 
std::error_code make_error_code(read_errc e) noexcept
{
    return std::error_code(static_cast<int>(e),read_error_category());
}


} // namespace mcp_detail_jsoncons

namespace std {
    template<>
    struct is_error_code_enum<mcp_detail_jsoncons::read_errc> : public true_type
    {
    };
} // namespace std

#endif // MCP_DETAIL_JSONCONS_READ_ERROR_HPP
