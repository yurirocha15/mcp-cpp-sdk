#include <mcp/detail/regex.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

using mcp::detail::Regex;
using mcp::detail::RegexError;

bool matches(const char* pattern, const std::string& input) { return Regex(pattern).search(input); }

TEST(Regex, LiteralSearchIsUnanchored) {
    EXPECT_TRUE(matches("abc", "xxabcxx"));
    EXPECT_FALSE(matches("abc", "abxc"));
    EXPECT_TRUE(matches("", "anything"));
    EXPECT_TRUE(matches("a|", "zzz"));
}

TEST(Regex, AnchorsMatchInputStartAndEnd) {
    EXPECT_TRUE(matches("^abc$", "abc"));
    EXPECT_FALSE(matches("^abc$", "abcd"));
    EXPECT_FALSE(matches("^abc$", "xabc"));
    EXPECT_TRUE(matches("^$", ""));
    // No multiline flag: ^ and $ do not match around a line break.
    EXPECT_FALSE(matches("^b", "a\nb"));
}

TEST(Regex, Quantifiers) {
    EXPECT_TRUE(matches("^a*$", ""));
    EXPECT_FALSE(matches("^a+$", ""));
    EXPECT_TRUE(matches("^ab?c$", "ac"));
    EXPECT_TRUE(matches("^(ab|cd)+$", "abcdab"));
    EXPECT_FALSE(matches("^(ab|cd)+$", "abce"));
    EXPECT_TRUE(matches("^a{2,3}$", "aa"));
    EXPECT_TRUE(matches("^a{2,3}$", "aaa"));
    EXPECT_FALSE(matches("^a{2,3}$", "aaaa"));
    EXPECT_TRUE(matches("^a{2,}$", "aaaaaa"));
    EXPECT_FALSE(matches("^a{2}$", "a"));
    EXPECT_TRUE(matches("^a+?b$", "aaab"));
    EXPECT_TRUE(matches("^a{1,2}?$", "aa"));
}

TEST(Regex, BraceThatIsNotAQuantifierIsLiteral) {
    EXPECT_TRUE(matches("a{", "a{"));
    EXPECT_TRUE(matches("^x{1,2$", "x{1,2"));
    EXPECT_TRUE(matches("^p{Letter}+$", "p{Letter}}"));
    EXPECT_TRUE(matches("^}$", "}"));
    EXPECT_TRUE(matches("^]$", "]"));
}

TEST(Regex, CharacterClasses) {
    EXPECT_TRUE(matches("^[a-c]+$", "abcabc"));
    EXPECT_TRUE(matches("^[^a-c]+$", "xyz"));
    EXPECT_FALSE(matches("^[^a-c]+$", "xaz"));
    EXPECT_TRUE(matches("^[\\w-]+$", "hello-1"));
    EXPECT_TRUE(matches("^[\\w.-]+@[\\w.-]+$", "a.b@c-d.e"));
    EXPECT_TRUE(matches("^[\\b]$", "\b"));
    EXPECT_FALSE(matches("^[]$", ""));
    EXPECT_FALSE(matches("[]", "anything"));
    EXPECT_TRUE(matches("^[^]$", "x"));
    EXPECT_TRUE(matches("^[\\d-z]+$", "1-z"));  // a set beside '-' makes the '-' literal
}

TEST(Regex, PredefinedClassesFollowEcmaScript) {
    EXPECT_TRUE(matches("^\\d{3}-\\d{4}$", "555-1234"));
    EXPECT_FALSE(matches("^\\d$", "\xdf\x80"));  // NKO DIGIT ZERO is not an ASCII digit
    EXPECT_TRUE(matches("^\\D$", "\xdf\x80"));
    EXPECT_TRUE(matches("^\\w+$", "hello_1"));
    EXPECT_FALSE(matches("^\\w+$", "hello-1"));
    EXPECT_TRUE(matches("^\\W$", "\xc3\xa9"));      // e-acute is not an ASCII word character
    EXPECT_TRUE(matches("^\\s$", "\xc2\xa0"));      // NO-BREAK SPACE
    EXPECT_TRUE(matches("^\\s$", "\xe2\x80\xa9"));  // PARAGRAPH SEPARATOR
    EXPECT_TRUE(matches("^\\s$", "\xef\xbb\xbf"));  // ZERO WIDTH NO-BREAK SPACE
    EXPECT_FALSE(matches("^\\S$", " "));
}

TEST(Regex, WordBoundaries) {
    EXPECT_TRUE(matches("\\bfoo\\b", "a foo b"));
    EXPECT_FALSE(matches("\\bfoo\\b", "afoob"));
    EXPECT_TRUE(matches("\\Boo\\B", "afoob"));
    EXPECT_TRUE(matches("^\\b", "a"));
    EXPECT_FALSE(matches("^\\b", " "));
}

TEST(Regex, DotMatchesOneCodePointExceptLineTerminators) {
    EXPECT_FALSE(matches("^.$", "\n"));
    EXPECT_FALSE(matches("^.$", "\r"));
    EXPECT_FALSE(matches("^.$", "\xe2\x80\xa8"));  // LINE SEPARATOR
    EXPECT_TRUE(matches("^.$", "\xf0\x9f\x98\x80"));
    EXPECT_FALSE(matches("^..$", "\xf0\x9f\x98\x80"));
}

TEST(Regex, Escapes) {
    EXPECT_TRUE(matches("^\\x41$", "A"));
    EXPECT_TRUE(matches("^[\\u0041-\\u005A]+$", "ABC"));
    EXPECT_TRUE(matches("^\\u{1F600}$", "\xf0\x9f\x98\x80"));
    EXPECT_TRUE(matches("^\\uD83D\\uDE00$", "\xf0\x9f\x98\x80"));
    EXPECT_TRUE(matches("^\\cJ$", "\n"));
    EXPECT_TRUE(matches("^\\cj$", "\n"));
    EXPECT_TRUE(matches("^\\t\\n\\v\\f\\r$", "\t\n\v\f\r"));
    EXPECT_TRUE(matches("^\\/\\.\\*\\?\\+\\(\\)\\[\\]\\{\\}\\|\\^\\$\\\\$", "/.*?+()[]{}|^$\\"));
    EXPECT_TRUE(matches(std::string("^\\0$").c_str(), std::string(1, '\0')));
}

TEST(Regex, Groups) {
    EXPECT_TRUE(matches("^(?:a|b)c$", "bc"));
    EXPECT_TRUE(matches("^(?<name>a)b$", "ab"));
    EXPECT_TRUE(matches("^((a))$", "a"));
}

TEST(Regex, InvalidUtf8InputReadsAsReplacementCharacter) {
    EXPECT_TRUE(matches("^.$", "\xff"));
    EXPECT_TRUE(matches("^\\uFFFD$", "\xc3"));
    EXPECT_TRUE(matches("^a.b$",
                        "a\xed\xa0\x80"
                        "b") == false);  // three replacement characters
    EXPECT_TRUE(matches("^a...b$",
                        "a\xed\xa0\x80"
                        "b"));
}

TEST(Regex, RejectsUnsupportedSyntax) {
    for (const char* pattern : {"(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(a)\\1", "\\k<n>", "\\p{L}",
                                "\\P{L}", "[\\p{L}]", "\\012", "[\\1]"}) {
        EXPECT_THROW(Regex{pattern}, RegexError) << pattern;
    }
}

TEST(Regex, RejectsMalformedPatterns) {
    for (const char* pattern : {"*a",   "a**",    "+",     "?",     "^*",     "\\b+",        "{1}",
                                "(a",   "a)",     "[a",    "[z-a]", "a{3,2}", "\\",          "\\q",
                                "(?x)", "(?<>a)", "\\xZ1", "\\u12", "\\u{}",  "\\u{110000}", "\\c1"}) {
        EXPECT_THROW(Regex{pattern}, RegexError) << pattern;
    }
}

TEST(Regex, BoundsNestingRepetitionAndProgramSize) {
    const std::string at_limit =
        std::string(Regex::max_nesting, '(') + "a" + std::string(Regex::max_nesting, ')');
    EXPECT_NO_THROW(Regex{at_limit});
    const std::string too_deep =
        std::string(Regex::max_nesting + 1, '(') + "a" + std::string(Regex::max_nesting + 1, ')');
    EXPECT_THROW(Regex{too_deep}, RegexError);
    EXPECT_NO_THROW(Regex{"a{1000}"});
    EXPECT_THROW(Regex{"a{1001}"}, RegexError);
    EXPECT_THROW(Regex{"(a{1000}){1000}"}, RegexError);
}

TEST(Regex, CopiesShareTheCompiledPattern) {
    const Regex original("^ab+$");
    const Regex copy = original;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.pattern(), "^ab+$");
    EXPECT_TRUE(copy.search("abbb"));
    EXPECT_FALSE(copy.search("a"));
}

// A backtracking engine takes time exponential in the input for this pattern and input; the Pike
// VM visits each (position, instruction) pair at most once.
TEST(Regex, NestedQuantifierOnFailingInputIsLinear) {
    const Regex regex("^(a+)+$");
    const std::string input = std::string(200'000, 'a') + "!";
    EXPECT_FALSE(regex.search(input));
    EXPECT_TRUE(regex.search(std::string(200'000, 'a')));
}

TEST(Regex, ConcurrentSearchesShareOneRegex) {
    const Regex regex("^[a-z]+-\\d+$");
    std::vector<std::thread> threads;
    std::vector<int> wrong(4, 0);
    for (std::size_t t = 0; t < wrong.size(); ++t) {
        threads.emplace_back([&regex, &wrong, t] {
            for (int i = 0; i < 2000; ++i) {
                if (!regex.search("abc-123") || regex.search("abc-") || regex.search("ABC-1")) {
                    ++wrong[t];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int count : wrong) {
        EXPECT_EQ(count, 0);
    }
}

#if defined(__unix__) || defined(__APPLE__)
// std::regex recurses once per input character, so a one-megabyte input against "^(a|b)*$"
// overflows even an 8 MiB stack. Run the same search on a 256 KiB stack: it must not use stack
// in proportion to the input.
TEST(Regex, LongInputRunsOnASmallStack) {
    struct Job {
        const Regex* regex;
        const std::string* input;
        bool result;
    };
    const Regex regex("^(a|b)*$");
    const std::string input(1'000'000, 'a');
    Job job{&regex, &input, false};

    pthread_attr_t attributes;
    ASSERT_EQ(pthread_attr_init(&attributes), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attributes, 256 * 1024), 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(
                  &thread, &attributes,
                  [](void* arg) -> void* {
                      auto* work = static_cast<Job*>(arg);
                      work->result = work->regex->search(*work->input);
                      return nullptr;
                  },
                  &job),
              0);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
    pthread_attr_destroy(&attributes);
    EXPECT_TRUE(job.result);
}
#endif

// On every platform: a long input on the test thread's own stack (1 MiB by default on Windows).
// Shorter than the input above, which runs once, so the two searches stay quick under MSan.
TEST(Regex, LongInputOnTheDefaultStack) {
    const Regex regex("^(a|b)*$");
    EXPECT_TRUE(regex.search(std::string(200'000, 'a')));
    EXPECT_FALSE(regex.search(std::string(200'000, 'a') + "c"));
}

// The counted search takes the program's size plus, at each input position, one step and one per
// thread; it gives up, leaving no steps, when the budget cannot pay for the search.
TEST(Regex, CountedSearchStopsWhenTheBudgetRunsOut) {
    const Regex unanchored(".{1,1000}x");
    const std::string input(600, 'a');  // several hundred thousand steps for the unanchored pattern
    std::size_t steps = 100'000;
    EXPECT_EQ(unanchored.search(input, steps), std::nullopt);
    EXPECT_EQ(steps, 0U);

    const Regex anchored("^[a-z]+$");
    steps = 100'000;
    EXPECT_EQ(anchored.search(input, steps), std::optional<bool>(true));
    EXPECT_GT(steps, 100'000U - 10'000U);
    EXPECT_LT(steps, 100'000U - input.size());

    steps = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(unanchored.search(input + "x", steps), std::optional<bool>(true));
    EXPECT_TRUE(unanchored.search(input + "x"));
}

// A quantified group that compiles to no instructions adds nothing however often it repeats, so
// nesting such groups must not multiply the compile time.
TEST(Regex, NestedRepetitionOfAnEmptyGroupCompilesQuickly) {
    const Regex regex("(?:(?:(?:(?:){1000}){1000}){1000}){1000}");
    EXPECT_TRUE(regex.search(""));
    EXPECT_TRUE(regex.search("abc"));
}

}  // namespace
