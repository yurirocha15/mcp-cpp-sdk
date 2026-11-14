#pragma once

#include <mcp/core/export.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
    /// Why the pattern was refused.
    enum class Kind {
        eInvalid,      ///< Not a regular expression under ECMA-262, with or without Annex B.
        eUnsupported,  ///< Uses syntax `Regex` does not implement, such as a lookahead.
        eTooLarge,     ///< Exceeds a limit of `Regex` or of the enclosing `PatternBudget`.
    };

    /// Construct with the reason @p kind and the description @p message.
    RegexError(Kind kind, const std::string& message);

    /// Why the pattern was refused.
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

struct RegexProgram;
class Regex;

/**
 * @brief Limits the program size of all the patterns compiled on this thread while it is alive,
 * and collects the patterns that cannot be compiled instead of stopping at the first.
 *
 * `Regex` bounds each program, but a schema can hold any number of patterns. Inside a budget,
 * every `Regex` constructed on the thread is charged its instructions, and one the budget cannot
 * hold throws a `RegexError` of kind `eTooLarge`. Budgets nest; the innermost one is charged.
 */
class MCP_API PatternBudget {
   public:
    /// Allows @p max_instructions instructions, in all, to the patterns compiled while this lives.
    explicit PatternBudget(std::size_t max_instructions) noexcept;
    ~PatternBudget();

    PatternBudget(const PatternBudget&) = delete;
    PatternBudget& operator=(const PatternBudget&) = delete;
    PatternBudget(PatternBudget&&) = delete;
    PatternBudget& operator=(PatternBudget&&) = delete;

    /**
     * @brief Compile @p pattern. Inside a budget, a pattern that cannot be compiled is recorded
     * (see `problem()`) and a pattern that matches nothing is returned in its place, so the
     * caller can go on to find other problems.
     * @throws RegexError outside a budget, when the pattern cannot be compiled.
     */
    static Regex compile(std::string_view pattern);

    /// What to report for the patterns that could not be compiled: the first invalid one if there
    /// is one, otherwise the first other problem. Empty when every pattern compiled.
    [[nodiscard]] const std::optional<RegexError>& problem() const noexcept { return problem_; }

   private:
    friend class Regex;

    void charge(std::size_t instructions);
    void record(const RegexError& error);

    std::size_t instructions_left_;
    std::optional<RegexError> problem_;
    PatternBudget* outer_;
};

/**
 * @brief A regular expression for patterns taken from JSON Schema documents (`pattern`,
 * `patternProperties`, format "regex").
 *
 * Matching runs in time linear in the input and uses no recursion, so a long input cannot exhaust
 * the stack and a pathological pattern cannot make matching take exponential time. The pattern is
 * compiled into a program for a Pike VM (a breadth-first simulation of its NFA). A pattern longer
 * than `max_pattern_size` is refused before it is read, and the parser bounds group nesting and the
 * compiled program size, so compiling a pattern supplied by a peer takes bounded time and memory.
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
    /// Largest `n` or `m` accepted in a `{n,m}` quantifier. The program size limit applies too:
    /// a quantifier compiles to one copy of its operand per repetition.
    static constexpr std::uint32_t max_repeat_count = 10000;
    /// Deepest group nesting accepted.
    static constexpr std::uint32_t max_nesting = 64;
    /// Largest compiled program, in instructions, accepted.
    static constexpr std::size_t max_program_size = 20000;
    /// Longest pattern accepted, in bytes. A longer one is refused before it is read.
    static constexpr std::size_t max_pattern_size = 65536;

    /**
     * @brief Compile @p pattern (UTF-8), charging its program to the thread's `PatternBudget`
     * when there is one.
     * @throws RegexError when the pattern cannot be compiled.
     */
    explicit Regex(std::string_view pattern);

    /**
     * @brief Whether the pattern matches anywhere in @p input (UTF-8), as ECMAScript's
     * `RegExp.prototype.test` does for a pattern without flags. Invalid UTF-8 is read as U+FFFD.
     */
    [[nodiscard]] bool search(std::string_view input) const;

    /**
     * @brief `search()` that counts its work against @p steps_left: setting up costs the
     * program's size, and each input position costs one step plus one for each thread the VM
     * runs there. Returns nothing, with @p steps_left at zero, when the search needs more steps
     * than @p steps_left holds; otherwise subtracts the steps it took.
     */
    [[nodiscard]] std::optional<bool> search(std::string_view input, std::size_t& steps_left) const;

    /// The pattern this was compiled from.
    [[nodiscard]] const std::string& pattern() const noexcept;

   private:
    std::shared_ptr<const RegexProgram> program_;
};

}  // namespace mcp::detail
