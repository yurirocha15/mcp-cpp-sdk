/**
 * @file schema_cache.hpp
 * @brief Reach-in accessors for this SDK's own tests. NOT part of the shipped interface.
 *
 * This header lives under `src/` and is not installed. The symbols are exported because the test
 * binary links the shared library, which is built with hidden visibility. Nothing in the library
 * itself uses them.
 */
#pragma once

#include <mcp/core/export.hpp>

#include <cstddef>

namespace mcp::detail {

/// The number of compiled schemas that CompiledSchema::compile() could return from its cache right
/// now: those that something still holds.
MCP_API std::size_t compiled_schema_cache_live_entries();

/// Which evaluation CompiledSchema::compile() gives a schema.
enum class SchemaEngine {
    eAutomatic,  ///< The native evaluation when it implements every keyword of the schema,
                 ///< jsoncons otherwise. Compiled schemas are cached.
    eNative,     ///< The native evaluation. A schema it does not implement is refused with
                 ///< std::domain_error. Nothing is read from or added to the cache.
    eJsoncons,   ///< The jsoncons evaluation. Nothing is read from or added to the cache.
};

/**
 * @brief Makes CompiledSchema::compile() on the constructing thread give schemas one evaluation
 * while this object exists, so that tests can compare the evaluations on the same document.
 *
 * Every check a schema gets when it is compiled still runs; only the evaluation that validate()
 * uses and the cache change. Scopes nest: the destructor restores the evaluation that was in force
 * before.
 */
class MCP_API ScopedSchemaEngine {
   public:
    /// Give schemas compiled on this thread @p engine until this object is destroyed.
    explicit ScopedSchemaEngine(SchemaEngine engine) noexcept;
    ~ScopedSchemaEngine();

    ScopedSchemaEngine(const ScopedSchemaEngine&) = delete;
    ScopedSchemaEngine& operator=(const ScopedSchemaEngine&) = delete;
    ScopedSchemaEngine(ScopedSchemaEngine&&) = delete;
    ScopedSchemaEngine& operator=(ScopedSchemaEngine&&) = delete;

   private:
    SchemaEngine previous_;
};

}  // namespace mcp::detail
