#pragma once

#include <initializer_list>
#include <optional>
#include <string_view>

namespace mcp::detail {

/// The JSON Schema dialects CompiledSchema accepts, oldest first, so that a comparison says
/// whether a dialect has a keyword.
enum class SchemaDialect : unsigned char {
    eDraft4,
    eDraft6,
    eDraft7,
    eDraft201909,
    eDraft202012,
};

/// The `$schema` URI of @p dialect, without the empty fragment.
constexpr std::string_view dialect_uri(SchemaDialect dialect) {
    switch (dialect) {
        case SchemaDialect::eDraft4:
            return "http://json-schema.org/draft-04/schema";
        case SchemaDialect::eDraft6:
            return "http://json-schema.org/draft-06/schema";
        case SchemaDialect::eDraft7:
            return "http://json-schema.org/draft-07/schema";
        case SchemaDialect::eDraft201909:
            return "https://json-schema.org/draft/2019-09/schema";
        case SchemaDialect::eDraft202012:
            break;
    }
    return "https://json-schema.org/draft/2020-12/schema";
}

/// The dialect @p uri names, or nothing. "...schema#" and "...schema" name the same dialect.
inline std::optional<SchemaDialect> find_schema_dialect(std::string_view uri) {
    if (!uri.empty() && uri.back() == '#') {
        uri.remove_suffix(1);
    }
    for (const auto dialect :
         {SchemaDialect::eDraft202012, SchemaDialect::eDraft201909, SchemaDialect::eDraft7,
          SchemaDialect::eDraft6, SchemaDialect::eDraft4}) {
        if (uri == dialect_uri(dialect)) {
            return dialect;
        }
    }
    return std::nullopt;
}

}  // namespace mcp::detail
