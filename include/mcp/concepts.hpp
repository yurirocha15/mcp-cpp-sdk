#pragma once

#include <concepts>
#include <nlohmann/json.hpp>

namespace mcp {

/**
 * @brief Concept that constrains a type to be serializable to and from JSON.
 *
 * A type satisfies JsonSerializable if it can be assigned to a nlohmann::json
 * object and retrieved from one via nlohmann::json::get<T>(). This works with
 * all nlohmann serialization mechanisms: ADL to_json/from_json free functions,
 * NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE macros, and adl_serializer specializations.
 *
 * @tparam T The type to check for JSON serializability.
 */
template <typename T>
concept JsonSerializable = requires(T val, nlohmann::json j) {
    { j = val } -> std::same_as<nlohmann::json&>;
    { j.template get<T>() } -> std::same_as<T>;
};

}  // namespace mcp
