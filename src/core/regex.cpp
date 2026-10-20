#include <mcp/detail/regex.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp::detail {

namespace {

constexpr char32_t g_max_code_point = 0x10FFFF;
constexpr char32_t g_replacement_character = 0xFFFD;

struct Range {
    char32_t first;
    char32_t last;
};

enum class Op : std::uint8_t {
    eChar,
    eAny,
    eClass,
    eSplit,
    eJump,
    eInputStart,
    eInputEnd,
    eWordBoundary,
    eNotWordBoundary,
    eMatch,
};

/// One program instruction. `eChar`: `arg1` is the code point. `eClass`: `arg1` indexes the class
/// table. `eSplit`: continue at both `arg1` and `arg2`. `eJump`: continue at `arg1`.
struct Instruction {
    Op op;
    std::uint32_t arg1;
    std::uint32_t arg2;
};

/// Decodes the UTF-8 code point at `text[pos]` and advances `pos`. A malformed or truncated
/// sequence yields U+FFFD and advances one byte.
char32_t decode_utf8(std::string_view text, std::size_t& pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        ++pos;
        return lead;
    }
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        ++pos;
        return g_replacement_character;
    }
    if (pos + length > text.size()) {
        ++pos;
        return g_replacement_character;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[pos + i]);
        if ((byte & 0xC0) != 0x80) {
            ++pos;
            return g_replacement_character;
        }
        value = (value << 6) | (byte & 0x3F);
    }
    if (value < minimum || value > g_max_code_point || (value >= 0xD800 && value <= 0xDFFF)) {
        ++pos;
        return g_replacement_character;
    }
    pos += length;
    return value;
}

bool is_word_character(char32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_line_terminator(char32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

std::vector<Range> normalize(std::vector<Range> ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& lhs, const Range& rhs) { return lhs.first < rhs.first; });
    std::vector<Range> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().last + 1) {
            merged.back().last = std::max(merged.back().last, range.last);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

std::vector<Range> complement(const std::vector<Range>& ranges) {
    std::vector<Range> result;
    char32_t next = 0;
    for (const auto& range : normalize(ranges)) {
        if (range.first > next) {
            result.push_back({next, range.first - 1});
        }
        next = range.last + 1;
    }
    if (next <= g_max_code_point) {
        result.push_back({next, g_max_code_point});
    }
    return result;
}

std::vector<Range> digit_ranges() { return {{'0', '9'}}; }

std::vector<Range> word_ranges() { return {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}; }

/// ECMAScript WhiteSpace and LineTerminator.
std::vector<Range> space_ranges() {
    return {{0x09, 0x0D},     {0x20, 0x20},     {0xA0, 0xA0},     {0x1680, 0x1680}, {0x2000, 0x200A},
            {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF}};
}

bool contains(const std::vector<Range>& ranges, char32_t c) {
    auto iter =
        std::upper_bound(ranges.begin(), ranges.end(), c,
                         [](char32_t value, const Range& range) { return value < range.first; });
    return iter != ranges.begin() && c <= std::prev(iter)->last;
}

/// Parsed pattern. Nodes live in one vector and refer to each other by index.
struct Node {
    enum class Kind : std::uint8_t {
        eEmpty,
        eChar,
        eAny,
        eClass,
        eConcat,
        eAlternation,
        eRepeat,
        eAssertion,
    };
    Kind kind = Kind::eEmpty;
    char32_t code_point = 0;            // eChar
    std::uint32_t class_index = 0;      // eClass
    Op assertion = Op::eInputStart;     // eAssertion
    std::vector<std::size_t> children;  // eConcat, eAlternation; eRepeat has one
    std::uint32_t min = 0;              // eRepeat
    std::optional<std::uint32_t> max;   // eRepeat; empty means unbounded
};

/// One class atom: either a single code point or a predefined set (\d, \w, \s and negations).
struct ClassAtom {
    std::optional<char32_t> code_point;
    std::vector<Range> set;
};

class Parser {
   public:
    explicit Parser(std::string_view pattern) {
        std::size_t pos = 0;
        while (pos < pattern.size()) {
            text_.push_back(decode_utf8(pattern, pos));
        }
    }

    std::size_t parse() {
        auto root = parse_alternation(0);
        if (pos_ != text_.size()) {
            fail(peek() == ')' ? "unmatched ')'" : "unexpected character");
        }
        return root;
    }

    std::vector<Node> nodes;
    std::vector<std::vector<Range>> classes;

   private:
    [[noreturn]] void fail(const std::string& reason) const {
        throw RegexError("invalid regular expression at offset " + std::to_string(pos_) + ": " +
                         reason);
    }

    bool at_end() const { return pos_ >= text_.size(); }
    char32_t peek(std::size_t ahead = 0) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : char32_t{0};
    }
    bool accept(char32_t c) {
        if (!at_end() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::size_t add(Node node) {
        nodes.push_back(std::move(node));
        return nodes.size() - 1;
    }

    std::size_t add_class(std::vector<Range> ranges) {
        classes.push_back(normalize(std::move(ranges)));
        Node node;
        node.kind = Node::Kind::eClass;
        node.class_index = static_cast<std::uint32_t>(classes.size() - 1);
        return add(std::move(node));
    }

    std::size_t add_char(char32_t c) {
        Node node;
        node.kind = Node::Kind::eChar;
        node.code_point = c;
        return add(std::move(node));
    }

    std::size_t parse_alternation(std::uint32_t depth) {
        std::vector<std::size_t> alternatives{parse_concat(depth)};
        while (accept('|')) {
            alternatives.push_back(parse_concat(depth));
        }
        if (alternatives.size() == 1) {
            return alternatives.front();
        }
        Node node;
        node.kind = Node::Kind::eAlternation;
        node.children = std::move(alternatives);
        return add(std::move(node));
    }

    std::size_t parse_concat(std::uint32_t depth) {
        Node node;
        node.kind = Node::Kind::eConcat;
        while (!at_end() && peek() != '|' && peek() != ')') {
            node.children.push_back(parse_repeat(depth));
        }
        if (node.children.empty()) {
            return add(Node{});
        }
        if (node.children.size() == 1) {
            return node.children.front();
        }
        return add(std::move(node));
    }

    /// Reads `{n}`, `{n,}` or `{n,m}` at the current position. Leaves the position unchanged and
    /// returns false when the text there is not a quantifier, in which case `{` is a literal.
    bool parse_braces(std::uint32_t& min, std::optional<std::uint32_t>& max) {
        const std::size_t start = pos_;
        auto read_number = [this](std::uint32_t& out) {
            std::size_t digits = 0;
            std::uint64_t value = 0;
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                value = value * 10 + (peek() - '0');
                if (value > Regex::max_repeat_count) {
                    fail("repetition count exceeds " + std::to_string(Regex::max_repeat_count));
                }
                ++pos_;
                ++digits;
            }
            out = static_cast<std::uint32_t>(value);
            return digits > 0;
        };
        ++pos_;  // '{'
        std::uint32_t low = 0;
        if (!read_number(low)) {
            pos_ = start;
            return false;
        }
        std::optional<std::uint32_t> high = low;
        if (accept(',')) {
            std::uint32_t value = 0;
            high = read_number(value) ? std::optional<std::uint32_t>(value) : std::nullopt;
        }
        if (!accept('}')) {
            pos_ = start;
            return false;
        }
        if (high && *high < low) {
            fail("numbers out of order in {} quantifier");
        }
        min = low;
        max = high;
        return true;
    }

    std::size_t parse_repeat(std::uint32_t depth) {
        const bool assertion =
            peek() == '^' || peek() == '$' || (peek() == '\\' && (peek(1) == 'b' || peek(1) == 'B'));
        auto atom = parse_atom(depth);
        std::uint32_t min = 0;
        std::optional<std::uint32_t> max;
        if (accept('*')) {
            min = 0;
            max.reset();
        } else if (accept('+')) {
            min = 1;
            max.reset();
        } else if (accept('?')) {
            min = 0;
            max = 1;
        } else if (!(peek() == '{' && parse_braces(min, max))) {
            return atom;
        }
        if (assertion) {
            fail("nothing to repeat");
        }
        accept('?');  // A lazy quantifier matches the same inputs.
        Node node;
        node.kind = Node::Kind::eRepeat;
        node.children.push_back(atom);
        node.min = min;
        node.max = max;
        return add(std::move(node));
    }

    std::size_t parse_atom(std::uint32_t depth) {
        const char32_t c = peek();
        switch (c) {
            case '(':
                return parse_group(depth);
            case '[':
                ++pos_;
                return parse_class();
            case '.': {
                ++pos_;
                Node node;
                node.kind = Node::Kind::eAny;
                return add(std::move(node));
            }
            case '^':
            case '$': {
                ++pos_;
                Node node;
                node.kind = Node::Kind::eAssertion;
                node.assertion = c == '^' ? Op::eInputStart : Op::eInputEnd;
                return add(std::move(node));
            }
            case '\\':
                ++pos_;
                return parse_escape();
            case '*':
            case '+':
            case '?':
                fail("nothing to repeat");
            case '{': {
                std::uint32_t min = 0;
                std::optional<std::uint32_t> max;
                if (parse_braces(min, max)) {
                    fail("nothing to repeat");
                }
                ++pos_;
                return add_char(c);
            }
            default:
                ++pos_;
                return add_char(c);
        }
    }

    std::size_t parse_group(std::uint32_t depth) {
        if (depth + 1 > Regex::max_nesting) {
            fail("groups nested deeper than " + std::to_string(Regex::max_nesting));
        }
        ++pos_;  // '('
        if (accept('?')) {
            if (accept(':')) {
                // non-capturing
            } else if (peek() == '=' || peek() == '!') {
                fail("lookahead is not supported");
            } else if (peek() == '<' && (peek(1) == '=' || peek(1) == '!')) {
                fail("lookbehind is not supported");
            } else if (accept('<')) {
                const std::size_t name_start = pos_;
                while (!at_end() && (is_word_character(peek()) || peek() == '$')) {
                    ++pos_;
                }
                if (pos_ == name_start || !accept('>')) {
                    fail("invalid group name");
                }
            } else {
                fail("invalid group");
            }
        }
        auto inner = parse_alternation(depth + 1);
        if (!accept(')')) {
            fail("missing ')'");
        }
        return inner;
    }

    std::uint32_t parse_hex(std::size_t digits) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            const char32_t h = peek();
            std::uint32_t nibble = 0;
            if (h >= '0' && h <= '9') {
                nibble = h - '0';
            } else if (h >= 'a' && h <= 'f') {
                nibble = h - 'a' + 10;
            } else if (h >= 'A' && h <= 'F') {
                nibble = h - 'A' + 10;
            } else {
                fail("invalid hexadecimal escape");
            }
            value = value * 16 + nibble;
            ++pos_;
        }
        return value;
    }

    /// `\u` escapes: `\uHHHH`, a surrogate pair written as two of them, or `\u{H...}`.
    char32_t parse_unicode_escape() {
        if (accept('{')) {
            std::uint32_t value = 0;
            std::size_t digits = 0;
            while (!at_end() && peek() != '}') {
                value = value * 16 + parse_hex(1);
                if (++digits > 6 || value > g_max_code_point) {
                    fail("code point out of range");
                }
            }
            if (digits == 0 || !accept('}')) {
                fail("invalid \\u{} escape");
            }
            return value;
        }
        std::uint32_t value = parse_hex(4);
        if (value >= 0xD800 && value <= 0xDBFF && peek() == '\\' && peek(1) == 'u') {
            const std::size_t save = pos_;
            pos_ += 2;
            const std::uint32_t low = parse_hex(4);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                return 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
            }
            pos_ = save;
        }
        return value;
    }

    /// Escapes shared by atoms and class members. Returns the set for \d \w \s and negations, or
    /// the single code point otherwise. `in_class` makes `\b` a backspace.
    ClassAtom parse_escape_atom(bool in_class) {
        if (at_end()) {
            fail("\\ at end of pattern");
        }
        const char32_t c = peek();
        ++pos_;
        switch (c) {
            case 'd':
                return {std::nullopt, digit_ranges()};
            case 'D':
                return {std::nullopt, complement(digit_ranges())};
            case 'w':
                return {std::nullopt, word_ranges()};
            case 'W':
                return {std::nullopt, complement(word_ranges())};
            case 's':
                return {std::nullopt, space_ranges()};
            case 'S':
                return {std::nullopt, complement(space_ranges())};
            case 't':
                return {char32_t{'\t'}, {}};
            case 'n':
                return {char32_t{'\n'}, {}};
            case 'v':
                return {char32_t{'\v'}, {}};
            case 'f':
                return {char32_t{'\f'}, {}};
            case 'r':
                return {char32_t{'\r'}, {}};
            case '0':
                if (peek() >= '0' && peek() <= '9') {
                    fail("octal escapes are not supported");
                }
                return {char32_t{0}, {}};
            case 'c': {
                const char32_t letter = peek();
                if (!((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z'))) {
                    fail("invalid \\c escape");
                }
                ++pos_;
                return {static_cast<char32_t>(letter % 32), {}};
            }
            case 'x':
                return {static_cast<char32_t>(parse_hex(2)), {}};
            case 'u':
                return {parse_unicode_escape(), {}};
            case 'p':
            case 'P':
                fail("Unicode property escapes are not supported");
            case 'k':
                fail("backreferences are not supported");
            case 'b':
                if (in_class) {
                    return {char32_t{'\b'}, {}};
                }
                break;
            case '-':
                return {char32_t{'-'}, {}};
            default:
                break;
        }
        if (c >= '1' && c <= '9') {
            fail(in_class ? "octal escapes are not supported" : "backreferences are not supported");
        }
        if (is_word_character(c)) {
            fail("unknown escape \\" + std::string(1, static_cast<char>(c)));
        }
        return {c, {}};
    }

    std::size_t parse_escape() {
        if (peek() == 'b' || peek() == 'B') {
            const bool negated = peek() == 'B';
            ++pos_;
            Node node;
            node.kind = Node::Kind::eAssertion;
            node.assertion = negated ? Op::eNotWordBoundary : Op::eWordBoundary;
            return add(std::move(node));
        }
        auto atom = parse_escape_atom(false);
        if (atom.code_point) {
            return add_char(*atom.code_point);
        }
        return add_class(std::move(atom.set));
    }

    ClassAtom parse_class_atom() {
        if (at_end()) {
            fail("missing ']'");
        }
        if (accept('\\')) {
            return parse_escape_atom(true);
        }
        const char32_t c = peek();
        ++pos_;
        return {c, {}};
    }

    std::size_t parse_class() {
        const bool negated = accept('^');
        std::vector<Range> ranges;
        while (!accept(']')) {
            auto low = parse_class_atom();
            if (peek() == '-' && peek(1) != ']' && peek(1) != 0) {
                ++pos_;  // '-'
                auto high = parse_class_atom();
                if (low.code_point && high.code_point) {
                    if (*low.code_point > *high.code_point) {
                        fail("range out of order in character class");
                    }
                    ranges.push_back({*low.code_point, *high.code_point});
                    continue;
                }
                // A set on either side of '-' makes the '-' literal (ECMA-262 Annex B).
                for (const auto* atom : {&low, &high}) {
                    if (atom->code_point) {
                        ranges.push_back({*atom->code_point, *atom->code_point});
                    } else {
                        ranges.insert(ranges.end(), atom->set.begin(), atom->set.end());
                    }
                }
                ranges.push_back({'-', '-'});
                continue;
            }
            if (low.code_point) {
                ranges.push_back({*low.code_point, *low.code_point});
            } else {
                ranges.insert(ranges.end(), low.set.begin(), low.set.end());
            }
        }
        return add_class(negated ? complement(ranges) : std::move(ranges));
    }

    std::vector<char32_t> text_;
    std::size_t pos_ = 0;
};

class Compiler {
   public:
    Compiler(const std::vector<Node>& nodes, std::vector<Instruction>& program)
        : nodes_(nodes), program_(program) {}

    void compile(std::size_t index) {
        const Node& node = nodes_[index];
        switch (node.kind) {
            case Node::Kind::eEmpty:
                return;
            case Node::Kind::eChar:
                emit({Op::eChar, node.code_point, 0});
                return;
            case Node::Kind::eAny:
                emit({Op::eAny, 0, 0});
                return;
            case Node::Kind::eClass:
                emit({Op::eClass, node.class_index, 0});
                return;
            case Node::Kind::eAssertion:
                emit({node.assertion, 0, 0});
                return;
            case Node::Kind::eConcat:
                for (auto child : node.children) {
                    compile(child);
                }
                return;
            case Node::Kind::eAlternation:
                compile_alternation(node);
                return;
            case Node::Kind::eRepeat:
                compile_repeat(node);
                return;
        }
    }

    std::size_t emit(Instruction instruction) {
        if (program_.size() >= Regex::max_program_size) {
            throw RegexError("regular expression compiles to more than " +
                             std::to_string(Regex::max_program_size) + " instructions");
        }
        program_.push_back(instruction);
        return program_.size() - 1;
    }

   private:
    std::uint32_t here() const { return static_cast<std::uint32_t>(program_.size()); }

    void compile_alternation(const Node& node) {
        std::vector<std::size_t> jumps;
        for (std::size_t i = 0; i + 1 < node.children.size(); ++i) {
            const auto split = emit({Op::eSplit, 0, 0});
            program_[split].arg1 = here();
            compile(node.children[i]);
            jumps.push_back(emit({Op::eJump, 0, 0}));
            program_[split].arg2 = here();
        }
        compile(node.children.back());
        for (auto jump : jumps) {
            program_[jump].arg1 = here();
        }
    }

    void compile_repeat(const Node& node) {
        const auto child = node.children.front();
        for (std::uint32_t i = 0; i < node.min; ++i) {
            const auto before = program_.size();
            compile(child);
            if (program_.size() == before) {
                // The operand compiles to nothing, so more copies add nothing either. Expanding
                // them would multiply the compile time by `min` per nesting level without ever
                // reaching the program-size limit.
                break;
            }
        }
        if (!node.max) {
            // L1: split L2, L3; L2: child; jump L1; L3:
            const auto split = emit({Op::eSplit, 0, 0});
            program_[split].arg1 = here();
            compile(child);
            emit({Op::eJump, static_cast<std::uint32_t>(split), 0});
            program_[split].arg2 = here();
            return;
        }
        // Each optional copy: split L1, END; L1: child. All exits jump to the same end.
        std::vector<std::size_t> splits;
        for (std::uint32_t i = node.min; i < *node.max; ++i) {
            const auto split = emit({Op::eSplit, 0, 0});
            program_[split].arg1 = here();
            splits.push_back(split);
            compile(child);
        }
        for (auto split : splits) {
            program_[split].arg2 = here();
        }
    }

    const std::vector<Node>& nodes_;
    std::vector<Instruction>& program_;
};

/// A set of program counters with O(1) insert, membership and clear.
class ThreadList {
   public:
    explicit ThreadList(std::size_t size) : dense_(size), sparse_(size) {}

    bool contains(std::uint32_t pc) const {
        const auto slot = sparse_[pc];
        return slot < count_ && dense_[slot] == pc;
    }
    void insert(std::uint32_t pc) {
        sparse_[pc] = static_cast<std::uint32_t>(count_);
        dense_[count_++] = pc;
    }
    void clear() { count_ = 0; }
    std::size_t size() const { return count_; }
    std::uint32_t operator[](std::size_t i) const { return dense_[i]; }

   private:
    std::vector<std::uint32_t> dense_;
    std::vector<std::uint32_t> sparse_;
    std::size_t count_ = 0;
};

/// What the zero-width instructions can see at one input position.
struct Position {
    bool at_start;
    bool at_end;
    bool previous_is_word;
    bool next_is_word;
};

}  // namespace

struct RegexProgram {
    std::string pattern;
    std::vector<Instruction> instructions;
    std::vector<std::vector<Range>> classes;
    /// The program starts with `^`, so a match can only start at offset 0.
    bool anchored = false;
};

Regex::Regex(std::string_view pattern) {
    auto program = std::make_shared<RegexProgram>();
    program->pattern = std::string(pattern);
    Parser parser(pattern);
    const auto root = parser.parse();
    program->classes = std::move(parser.classes);
    Compiler compiler(parser.nodes, program->instructions);
    compiler.compile(root);
    compiler.emit({Op::eMatch, 0, 0});
    program->anchored = program->instructions.front().op == Op::eInputStart;
    program_ = std::move(program);
}

const std::string& Regex::pattern() const noexcept { return program_->pattern; }

bool Regex::search(std::string_view input) const {
    const auto& instructions = program_->instructions;
    const auto& classes = program_->classes;
    const bool anchored = program_->anchored;
    const std::size_t size = instructions.size();
    ThreadList current(size);
    ThreadList next(size);
    std::vector<std::uint32_t> stack;
    stack.reserve(size);

    // Follows the zero-width instructions reachable from `start` at `where` and records every
    // instruction visited in `list`. Returns true once a match is reachable.
    auto add_thread = [&](ThreadList& list, std::uint32_t start, const Position& where) {
        stack.push_back(start);
        bool matched = false;
        while (!stack.empty()) {
            const auto pc = stack.back();
            stack.pop_back();
            if (list.contains(pc)) {
                continue;
            }
            list.insert(pc);
            const auto& instruction = instructions[pc];
            switch (instruction.op) {
                case Op::eJump:
                    stack.push_back(instruction.arg1);
                    break;
                case Op::eSplit:
                    stack.push_back(instruction.arg2);
                    stack.push_back(instruction.arg1);
                    break;
                case Op::eInputStart:
                    if (where.at_start) {
                        stack.push_back(pc + 1);
                    }
                    break;
                case Op::eInputEnd:
                    if (where.at_end) {
                        stack.push_back(pc + 1);
                    }
                    break;
                case Op::eWordBoundary:
                    if (where.previous_is_word != where.next_is_word) {
                        stack.push_back(pc + 1);
                    }
                    break;
                case Op::eNotWordBoundary:
                    if (where.previous_is_word == where.next_is_word) {
                        stack.push_back(pc + 1);
                    }
                    break;
                case Op::eMatch:
                    matched = true;
                    break;
                default:
                    break;
            }
        }
        return matched;
    };

    std::size_t offset = 0;
    std::optional<char32_t> current_char;
    if (offset < input.size()) {
        current_char = decode_utf8(input, offset);
    }
    Position where{true, !current_char, false, current_char && is_word_character(*current_char)};

    if (add_thread(current, 0, where)) {
        return true;
    }
    while (current_char) {
        const char32_t c = *current_char;
        std::optional<char32_t> following;
        if (offset < input.size()) {
            following = decode_utf8(input, offset);
        }
        const Position after{false, !following, is_word_character(c),
                             following && is_word_character(*following)};
        next.clear();
        for (std::size_t i = 0; i < current.size(); ++i) {
            const auto pc = current[i];
            const auto& instruction = instructions[pc];
            bool consumes = false;
            switch (instruction.op) {
                case Op::eChar:
                    consumes = instruction.arg1 == c;
                    break;
                case Op::eAny:
                    consumes = !is_line_terminator(c);
                    break;
                case Op::eClass:
                    consumes = contains(classes[instruction.arg1], c);
                    break;
                default:
                    break;
            }
            if (consumes && add_thread(next, pc + 1, after)) {
                return true;
            }
        }
        // Unanchored search: a match may also start at the next position.
        if (!anchored && add_thread(next, 0, after)) {
            return true;
        }
        if (anchored && next.size() == 0) {
            return false;
        }
        std::swap(current, next);
        current_char = following;
    }
    return false;
}

}  // namespace mcp::detail
