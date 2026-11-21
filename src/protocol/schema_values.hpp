#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace mcp::detail {

/// Thrown when an evaluation needs more work than its budget holds.
struct WorkBudgetExceeded {};

/// Thrown when an evaluation nests deeper than its bound.
struct EvaluationTooDeep {};

/// The JSON Schema type name of @p value: "integer" for an integer, "number" for a float.
std::string_view type_name(const nlohmann::json& value);

/// Whether @p value is of the JSON Schema type @p type. "integer" also takes a float with no
/// fractional part, and "number" takes every number.
bool has_type(const nlohmann::json& value, std::string_view type);

/// Compares two numbers exactly: negative, zero or positive as @p left is less than, equal to or
/// greater than @p right.
int compare_numbers(const nlohmann::json& left, const nlohmann::json& right);

/// Whether @p value is a NaN or an infinity, which JSON cannot represent.
bool is_non_finite(const nlohmann::json& value);

/// Takes @p cost units from @p work, or throws WorkBudgetExceeded when it holds fewer.
void charge(std::size_t& work, std::size_t cost);

/// JSON Schema equality: numbers are equal when their values are, whatever their representation.
/// nlohmann's operator== converts an unsigned integer to a signed one when comparing the two, so
/// it finds -1 equal to 18446744073709551615.
///
/// A comparison costs @p work, which pays for values as large as the schema and the instance
/// together: an enum of many long values compared with a long value is quadratic otherwise.
bool json_equal(const nlohmann::json& left, const nlohmann::json& right, std::size_t& work);

/// Whether no two items of @p array are json_equal(), charging @p work for each comparison.
bool all_unique(const nlohmann::json& array, std::size_t& work);

}  // namespace mcp::detail
