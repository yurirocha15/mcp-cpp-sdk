// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_TRACING_GENERIC_VISITOR_HPP
#define MCP_DETAIL_JSONCONS_TRACING_GENERIC_VISITOR_HPP

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <jsoncons/nonstd/compiler_support.hpp>
#include <jsoncons/config/jsoncons_config.hpp>
#include <jsoncons/generic_visitor.hpp>

namespace mcp_detail_jsoncons { 

    class tracing_generic_visitor : public basic_default_generic_visitor<char>
    {
        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_begin_object(semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_begin_object" << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_begin_object(std::size_t length, semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_begin_object " << length << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_end_object(const ser_context&, std::error_code&) override
        {
            std::cout << "visit_end_object" << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_begin_array(semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_begin_array" << '\n';
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_begin_array(std::size_t length, semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_begin_array " << length << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_end_array(const ser_context&, std::error_code&) override
        {
            std::cout << "visit_end_array" << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_string(const string_view_type& s, semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_string " << s << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }
        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_int64(int64_t val, semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_int64 " << val << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }
        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_uint64(uint64_t val, semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_uint64 " << val << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }
        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_bool(bool val, semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_bool " << val << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }
        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_null(semantic_tag, const ser_context&, std::error_code&) override
        {
            std::cout << "visit_null " << '\n'; 
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_typed_array(const mcp_detail_jsoncons::span<const uint16_t>& s, 
            semantic_tag tag, 
            const ser_context&, 
            std::error_code&) override  
        {
            std::cout << "visit_typed_array uint16_t " << tag << '\n'; 
            for (auto val : s)
            {
                std::cout << val << "" << '\n';
            }
            std::cout << "" << '\n';
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }

        MCP_DETAIL_JSONCONS_VISITOR_RETURN_TYPE visit_typed_array(half_arg_t, const mcp_detail_jsoncons::span<const uint16_t>& s,
            semantic_tag tag,
            const ser_context&,
            std::error_code&) override
        {
            std::cout << "visit_typed_array half_arg_t uint16_t " << tag << "" << '\n';
            for (auto val : s)
            {
                std::cout << val << "" << '\n';
            }
            std::cout << "" << '\n';
            MCP_DETAIL_JSONCONS_VISITOR_RETURN;
        }
    };

} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_TRACING_GENERIC_VISITOR_HPP
