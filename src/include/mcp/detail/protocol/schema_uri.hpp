#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

/// A URI reference split into its RFC 3986 components. A component that is absent differs from
/// one that is present and empty: `http://h` has no query, `http://h?` has an empty one.
struct UriReference {
    std::optional<std::string> scheme;
    std::optional<std::string> authority;
    std::string path;
    std::optional<std::string> query;
    std::optional<std::string> fragment;

    /// The reference written back as text (RFC 3986 section 5.3).
    [[nodiscard]] std::string text() const;
};

/// Splits @p text into its components (RFC 3986 appendix B). Every string splits.
UriReference parse_uri_reference(std::string_view text);

/// @p reference resolved against @p base (RFC 3986 section 5.2), dot segments removed.
std::string resolve_uri(std::string_view base, std::string_view reference);

/// @p uri without its fragment, and the fragment ("" when there is none).
std::pair<std::string, std::string> split_fragment(std::string_view uri);

/// The reference tokens of the JSON Pointer @p pointer (RFC 6901), unescaped; nothing when it is
/// not one. "" is the whole document.
std::optional<std::vector<std::string>> json_pointer_tokens(std::string_view pointer);

/// @p token escaped for a JSON Pointer.
std::string escape_pointer_token(std::string_view token);

}  // namespace mcp::detail
