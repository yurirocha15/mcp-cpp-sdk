/**
 * @file schema_cache.hpp
 * @brief Reach-in accessor for this SDK's own tests. NOT part of the shipped interface.
 *
 * This header lives under `src/` and is not installed. The symbol is exported because the test
 * binary links the shared library, which is built with hidden visibility. Nothing in the library
 * itself calls it.
 */
#pragma once

#include <mcp/core/export.hpp>

#include <cstddef>

namespace mcp::detail {

/// The number of compiled schemas that CompiledSchema::compile() could return from its cache right
/// now: those that something still holds.
MCP_API std::size_t compiled_schema_cache_live_entries();

}  // namespace mcp::detail
