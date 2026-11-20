#pragma once

#include <mcp/protocol/capabilities.hpp>

#include <cstdint>
#include <optional>

namespace mcp::detail {

/// Reads the `ttlMs` and `cacheScope` caching hints of a result. A `cacheScope` this SDK does not
/// recognise is left unset: the enum's JSON conversion would read it as "public", the scope that
/// lets a cache share the result across callers.
inline void read_cache_hints(const nlohmann::json& json_obj, std::optional<std::int64_t>& ttl_ms,
                             std::optional<CacheScope>& cache_scope) {
    if (has_json_value(json_obj, "ttlMs")) {
        ttl_ms = json_obj.at("ttlMs").get<std::int64_t>();
    }
    if (const auto scope = json_obj.find("cacheScope"); scope != json_obj.end() && scope->is_string()) {
        if (*scope == "public") {
            cache_scope = CacheScope::ePublic;
        } else if (*scope == "private") {
            cache_scope = CacheScope::ePrivate;
        }
    }
}

/// Writes the `ttlMs` and `cacheScope` caching hints of a result, each only when it is set.
inline void write_cache_hints(nlohmann::json& json_obj, const std::optional<std::int64_t>& ttl_ms,
                              const std::optional<CacheScope>& cache_scope) {
    if (ttl_ms) {
        json_obj["ttlMs"] = *ttl_ms;
    }
    if (cache_scope) {
        json_obj["cacheScope"] = *cache_scope;
    }
}

}  // namespace mcp::detail
