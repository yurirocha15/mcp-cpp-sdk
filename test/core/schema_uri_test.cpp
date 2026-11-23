// URI reference resolution (RFC 3986) and JSON Pointer tokens, which schema references are
// resolved with.

#include "mcp/detail/protocol/schema_uri.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace schema_uri_test {

namespace {

using mcp::detail::json_pointer_tokens;
using mcp::detail::parse_uri_reference;
using mcp::detail::resolve_uri;
using mcp::detail::split_fragment;

// RFC 3986 section 5.4.1.
TEST(SchemaUri, ResolvesTheNormalExamples) {
    const std::string base = "http://a/b/c/d;p?q";
    const std::vector<std::pair<const char*, const char*>> examples = {
        {"g:h", "g:h"},
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"},
        {"g?y#s", "http://a/b/c/g?y#s"},
        {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"},
        {"g;x?y#s", "http://a/b/c/g;x?y#s"},
        {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
    };
    for (const auto& [reference, expected] : examples) {
        EXPECT_EQ(resolve_uri(base, reference), expected) << reference;
    }
}

// RFC 3986 section 5.4.2.
TEST(SchemaUri, ResolvesTheAbnormalExamples) {
    const std::string base = "http://a/b/c/d;p?q";
    const std::vector<std::pair<const char*, const char*>> examples = {
        {"../../../g", "http://a/g"},
        {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"},
        {"g?y/../x", "http://a/b/c/g?y/../x"},
        {"g#s/./x", "http://a/b/c/g#s/./x"},
        {"g#s/../x", "http://a/b/c/g#s/../x"},
        {"http:g", "http:g"},
    };
    for (const auto& [reference, expected] : examples) {
        EXPECT_EQ(resolve_uri(base, reference), expected) << reference;
    }
}

TEST(SchemaUri, ResolvesAgainstUrnsAndAuthoritiesWithoutPaths) {
    EXPECT_EQ(resolve_uri("urn:example:root", "#/$defs/a"), "urn:example:root#/$defs/a");
    EXPECT_EQ(resolve_uri("urn:example:root", "#anchor"), "urn:example:root#anchor");
    EXPECT_EQ(resolve_uri("https://example.com", "a.json"), "https://example.com/a.json");
}

// Against a base without '/' in its path the reference's path is used as it is, so it can start
// with dot segments, which are removed.
TEST(SchemaUri, RemovesLeadingDotSegments) {
    EXPECT_EQ(resolve_uri("urn:abc", "def"), "urn:def");
    EXPECT_EQ(resolve_uri("urn:abc", "./def"), "urn:def");
    EXPECT_EQ(resolve_uri("urn:abc", "../def"), "urn:def");
    EXPECT_EQ(resolve_uri("urn:abc", "."), "urn:");
    EXPECT_EQ(resolve_uri("urn:abc", ".."), "urn:");
}

TEST(SchemaUri, ReadsSchemes) {
    EXPECT_EQ(parse_uri_reference("A1+b-c.d:x").scheme, "A1+b-c.d");
    EXPECT_FALSE(parse_uri_reference(":x").scheme.has_value());
    EXPECT_FALSE(parse_uri_reference("a_b:x").scheme.has_value());
}

TEST(SchemaUri, SplitsComponents) {
    const auto reference = parse_uri_reference("https://user@host:8/p/q?x=1#frag");
    EXPECT_EQ(reference.scheme, "https");
    EXPECT_EQ(reference.authority, "user@host:8");
    EXPECT_EQ(reference.path, "/p/q");
    EXPECT_EQ(reference.query, "x=1");
    EXPECT_EQ(reference.fragment, "frag");
    EXPECT_FALSE(parse_uri_reference("a/b:c").scheme.has_value());
    EXPECT_FALSE(parse_uri_reference("1a:b").scheme.has_value());
    EXPECT_EQ(split_fragment("a.json#/x"), std::make_pair(std::string("a.json"), std::string("/x")));
    EXPECT_EQ(split_fragment("a.json"), std::make_pair(std::string("a.json"), std::string()));
}

TEST(SchemaUri, ReadsJsonPointers) {
    EXPECT_EQ(json_pointer_tokens(""), std::vector<std::string>{});
    EXPECT_EQ(json_pointer_tokens("/a~1b/c~0d/0/"), (std::vector<std::string>{"a/b", "c~d", "0", ""}));
    EXPECT_FALSE(json_pointer_tokens("a").has_value());
    EXPECT_FALSE(json_pointer_tokens("/a~2").has_value());
    EXPECT_FALSE(json_pointer_tokens("/a~").has_value());
}

}  // namespace

}  // namespace schema_uri_test
