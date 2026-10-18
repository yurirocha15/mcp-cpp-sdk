#pragma once

#include <mcp/core/export.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mcp::detail {

/**
 * @brief Thrown when a pattern is malformed, uses syntax `Regex` does not support, or compiles to
 * a program larger than `Regex` accepts.
 */
class MCP_API RegexError : public std::invalid_argument {
   public:
    using std::invalid_argument::invalid_argument;
};

struct RegexProgram;

/**
 * @brief A regular expression for patterns taken from JSON Schema documents (`pattern`,
 * `patternProperties`, format "regex").
 *
 * Matching runs in time linear in the input and uses no recursion, so a long input cannot exhaust
 * the stack and a pathological pattern cannot make matching take exponential time. The pattern is
 * compiled into a program for a Pike VM (a breadth-first simulation of its NFA); the parser bounds
 * group nesting and the compiled program size, so a pattern supplied by a peer is as safe to
 * compile as one written by the application.
 *
 * The syntax is the ECMA-262 subset that JSON Schema recommends, evaluated on Unicode code points:
 * literals, `.`, character classes with ranges and negation, `\d \D \w \W \s \S`, `\b \B`, `^`
 * and `$` (start and end of input), groups (capturing, `(?:...)` and named), alternation, and the
 * quantifiers `* + ? {n} {n,} {n,m}` with their lazy forms. Backreferences, lookaround and Unicode
 * property escapes (`\p{...}`) are rejected with a `RegexError`. Captures are not reported;
 * `search()` only answers whether the pattern matches.
 *
 * A compiled `Regex` is immutable: copies share the program, and `search()` may be called from
 * several threads at once.
 */
class MCP_API Regex {
   public:
    /// Largest `n` or `m` accepted in a `{n,m}` quantifier.
    static constexpr std::uint32_t max_repeat_count = 1000;
    /// Deepest group nesting accepted.
    static constexpr std::uint32_t max_nesting = 64;
    /// Largest compiled program, in instructions, accepted.
    static constexpr std::size_t max_program_size = 20000;

    /**
     * @brief Compile @p pattern (UTF-8).
     * @throws RegexError when the pattern cannot be compiled.
     */
    explicit Regex(std::string_view pattern);

    /**
     * @brief Whether the pattern matches anywhere in @p input (UTF-8), as ECMAScript's
     * `RegExp.prototype.test` does for a pattern without flags. Invalid UTF-8 is read as U+FFFD.
     */
    [[nodiscard]] bool search(std::string_view input) const;

    /// The pattern this was compiled from.
    [[nodiscard]] const std::string& pattern() const noexcept;

   private:
    std::shared_ptr<const RegexProgram> program_;
};

}  // namespace mcp::detail
