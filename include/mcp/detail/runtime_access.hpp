#pragma once

#include <mcp/core/runtime.hpp>

#include <boost/asio/any_io_executor.hpp>

namespace mcp::detail {

MCP_API boost::asio::any_io_executor get_executor(Runtime& runtime);

}  // namespace mcp::detail
