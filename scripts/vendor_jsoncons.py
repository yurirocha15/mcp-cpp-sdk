#!/usr/bin/env python3
"""Produce ``third_party/jsoncons`` from a pinned jsoncons release.

The SDK compiles jsoncons's JSON Schema extension into one private translation
unit. The vendored tree is never edited by hand: this script downloads the
pinned release tarball, verifies its SHA-256, copies the headers the extension
needs and applies five recorded groups of transformations:

1. ``std::regex`` is replaced with ``mcp::detail::Regex``. libstdc++'s
   ``std::regex`` recurses once per input character, so a schema ``pattern``
   matched against a long string overflows the stack; ``mcp::detail::Regex``
   runs in linear time without recursion. Schema patterns are compiled through
   ``mcp::detail::PatternBudget``, which bounds the program size of all of a
   schema's patterns together and lets compilation go on past a pattern it
   cannot compile, so another problem in the schema is still found.
2. ``eval_context``, which jsoncons creates for every keyword it evaluates,
   enforces a limit on the evaluation depth and on the evaluation work in one
   validation, where a step costs one more than the length of its evaluation
   path. Without them a reference cycle that never descends into the value
   (``{"$ref": "#"}``) recurses until the stack overflows, and branching
   applicators over recursive references take time exponential in the value's
   depth. Pattern matching (``pattern``, ``patternProperties``) is charged to a
   separate budget of ``mcp::detail::Regex`` steps: matching is linear in the
   input, but its cost per input character grows with the pattern. Work that
   creates no ``eval_context`` is charged too: copies of JSON pointers, by
   their length, since evaluation copies the instance location into every step
   (which, like the evaluation path, keeps at most 1024 bytes of each property
   name);
   keywords that read a whole string; and messages kept by branching keywords,
   which keep at most 16 each and hold their memory against a further budget.
   Messages quote at most an excerpt of the value.
3. Large doubles are handled without undefined behavior. The ``integer``
   type check and the size keywords such as ``maxLength`` converted a double
   to an integer type whatever its range; now ``1e308`` is an integer, as JSON
   Schema defines it, and a size keyword's value saturates. With ``1e308``
   counted as an integer, ``multipleOf`` must also reject a quotient that
   overflows, as the JSON-Schema-Test-Suite expects: its tolerance of one
   unit in the last place of the value accepts any multiple at that size.
   An integer and a double are compared exactly, by ``basic_json::compare`` and
   by the bound keywords, instead of after converting the integer to a double,
   which rounds integers beyond 2^53.
4. Keyword fixes. ``uniqueItems`` compares only elements whose hashes match,
   instead of every pair of elements in time quadratic in the array's length.
   Subschemas under ``definitions`` are registered in every dialect, so a
   ``$ref`` such as ``#/definitions/name`` resolves in 2019-09 and 2020-12
   too, not only in jsoncons's compatibility mode, and a name under both
   ``definitions`` and ``$defs`` keeps both subschemas. The subschemas of
   ``patternProperties``, ``dependentSchemas`` and ``dependencies`` are
   registered at their own locations, so a ``$ref`` such as
   ``#/patternProperties/name`` resolves. A member that fails the
   ``additionalProperties`` subschema is reported at the member, with what the
   subschema reported, like the other applicators, instead of as one summary
   at the object. Property names and keywords are looked up with their full
   length instead of as C strings, which end at a NUL, and sets of property
   names are ordered instead of hashed, so names chosen to share a hash cannot
   make lookups scan them all.
5. The ``jsoncons`` namespace becomes ``mcp_detail_jsoncons`` and the
   ``JSONCONS_`` macro prefix becomes ``MCP_DETAIL_JSONCONS_``, so the copy
   compiled into the SDK (a static archive included) cannot collide with a
   jsoncons the application uses itself.

Usage:
  python3 scripts/vendor_jsoncons.py            regenerate third_party/jsoncons
  python3 scripts/vendor_jsoncons.py --check    fail if the committed tree differs
  --tarball PATH                                use a local copy of the tarball
"""

from __future__ import annotations

import argparse
import hashlib
import io
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT_DIR = REPO_ROOT / "third_party" / "jsoncons"

VERSION = "1.10.0"
TARBALL_URL = f"https://github.com/danielaparker/jsoncons/archive/refs/tags/v{VERSION}.tar.gz"
TARBALL_SHA256 = "c829350a2eece662beb378143e2ab1c1cbb07d703687bf168626665a8a7d8bc1"
# Bump when the transformation below changes, so PROVENANCE records which one produced the tree.
TRANSFORM_VERSION = 12

# Header directories, relative to the release's include/, that the extension needs.
SUBSET = ("jsoncons", "jsoncons_ext/jsonschema", "jsoncons_ext/jsonpointer")

# (old, new, expected occurrences across SUBSET). A count that no longer matches means the
# upstream code moved and the replacement must be reviewed, so the script fails instead of
# producing a tree that still calls std::regex somewhere.
REGEX_REPLACEMENTS = (
    ("#include <regex>", "#include <mcp/detail/regex.hpp>", 7),
    (
        "std::vector<std::pair<std::regex, schema_validator_ptr_type>>",
        "std::vector<std::pair<::mcp::detail::Regex, schema_validator_ptr_type>>",
        3,
    ),
    (
        "std::regex(prop.key(), std::regex::ECMAScript)",
        "::mcp::detail::PatternBudget::compile(prop.key())",
        1,
    ),
    (
        "auto regex = std::regex(pattern_string, std::regex::ECMAScript);",
        "auto regex = ::mcp::detail::PatternBudget::compile(pattern_string);",
        1,
    ),
    (
        "std::regex re(sv.data(), sv.size(), std::regex::ECMAScript);",
        "::mcp::detail::Regex re(std::string_view(sv.data(), sv.size()));",
        1,
    ),
    ("std::regex regex_;", "::mcp::detail::Regex regex_;", 1),
    (
        "const std::string& pattern_string, const std::regex& regex)",
        "const std::string& pattern_string, const ::mcp::detail::Regex& regex)",
        1,
    ),
    (
        "if (!std::regex_search(instance.as_cstring(), regex_))",
        "if (!evaluation_limits::search(regex_, instance.as_string_view()))",
        1,
    ),
    (
        "if (std::regex_search(prop.key(), schema_pp.first))",
        "if (evaluation_limits::search(schema_pp.first, prop.key()))",
        2,
    ),
)

EVAL_CONTEXT_PATH = "jsoncons_ext/jsonschema/common/eval_context.hpp"

EVALUATION_LIMITS = """// Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): limits on one validation. A reference
// cycle that never descends into the value makes evaluation recurse without end, and branching
// applicators over recursive references can make the number of evaluation steps exponential in
// the value's depth. eval_context is created for every keyword evaluated, so it enforces both.
// Creating one copies the evaluation path and the dynamic scope, so a step costs work in
// proportion to the path's length, and the budget is charged accordingly. Copies of JSON
// pointers are charged to the same budget (see jsonpointer::evaluation_work).
using evaluation_limit_error = jsonpointer::evaluation_limit_error;

struct evaluation_limits
{
    // Longest evaluation path, counted in keywords and array indexes.
    static constexpr std::size_t max_depth = 192;

    // Messages one branching keyword keeps from its branches. A keyword counts every message, so
    // its verdict does not depend on how many are kept.
    static constexpr std::size_t max_kept_messages = 16;

    // Longest excerpt of a value or a schema quoted in a message, in bytes.
    static constexpr std::size_t max_excerpt = 256;

    // Evaluation work left for the validation running on this thread. A step with an evaluation
    // path of n tokens costs n + 1. The caller sets it before each validation; the initial value
    // does not limit anything.
    static std::size_t& work_left()
    {
        return jsonpointer::evaluation_work::left();
    }

    // Bytes that the messages kept by branching keywords may still take on this thread. The
    // caller sets it before each validation; the initial value does not limit anything.
    static std::size_t& kept_message_bytes_left()
    {
        static thread_local std::size_t bytes = static_cast<std::size_t>(-1);
        return bytes;
    }

    static void keep_message_bytes(std::size_t bytes)
    {
        std::size_t& left = kept_message_bytes_left();
        if (left < bytes)
        {
            throw evaluation_limit_error("the messages kept for failed branches take too much memory");
        }
        left -= bytes;
    }

    static void release_message_bytes(std::size_t bytes)
    {
        kept_message_bytes_left() += bytes;
    }

    static std::string_view excerpt(std::string_view text)
    {
        return text.substr(0, max_excerpt);
    }

    // Longest property name kept in an instance location or an evaluation path, in bytes. The SDK
    // reports only instance locations, flattened and cut to 256 bytes, and flattening turns at
    // most 3 bytes into 1, so the first 772 bytes of a name are all a report can show. Every step
    // under the name copies both, so a longer name would cost its whole length for each item
    // below it.
    static constexpr std::size_t max_location_token = 1024;

    static std::string_view location_token(std::string_view name)
    {
        return name.substr(0, max_location_token);
    }

    // What comparing @p value with another may cost, charged before the comparison: one per value
    // it holds, and a string or a member name by its length, as other whole-string work is
    // charged. Counting stops at @p limit, so a small value compared with a large one costs what
    // the small one holds.
    template <typename Json>
    static std::size_t comparison_cost(const Json& value, std::size_t limit = static_cast<std::size_t>(-1))
    {
        std::size_t cost = 1;
        if (value.is_string())
        {
            cost += value.as_string_view().size() / 16;
        }
        else if (value.is_array())
        {
            for (const auto& element : value.array_range())
            {
                if (cost >= limit)
                {
                    break;
                }
                cost += comparison_cost(element, limit - cost);
            }
        }
        else if (value.is_object())
        {
            for (const auto& member : value.object_range())
            {
                if (cost >= limit)
                {
                    break;
                }
                cost += 1 + member.key().size() / 16 + comparison_cost(member.value(), limit - cost);
            }
        }
        return (std::min)(cost, limit);
    }

    // A value as a message names it: a scalar quoted in full, a string cut to an excerpt, and an
    // array or object by its size, so a message takes the same time whatever the value holds.
    template <typename Json>
    static std::string describe(const Json& value)
    {
        if (value.is_string())
        {
            return "'" + std::string(excerpt(value.as_string_view())) + "'";
        }
        if (value.is_array())
        {
            return "An array of length " + std::to_string(value.size());
        }
        if (value.is_object())
        {
            return "An object of size " + std::to_string(value.size());
        }
        return "'" + value.template as<std::string>() + "'";
    }

    // Pattern matching steps left for the validation running on this thread, as
    // ::mcp::detail::Regex::search counts them. The caller sets it before each validation; the
    // initial value does not limit anything.
    static std::size_t& pattern_steps_left()
    {
        static thread_local std::size_t steps = static_cast<std::size_t>(-1);
        return steps;
    }

    static bool search(const ::mcp::detail::Regex& regex, std::string_view input)
    {
        const auto matched = regex.search(input, pattern_steps_left());
        if (!matched)
        {
            throw evaluation_limit_error("pattern matching takes too many steps");
        }
        return *matched;
    }

    // Charges work that does not create an eval_context, such as a comparison in a loop over a
    // keyword's values.
    static void charge(std::size_t units)
    {
        jsonpointer::evaluation_work::charge(units);
    }

    static void enter(std::size_t depth)
    {
        if (depth > max_depth)
        {
            throw evaluation_limit_error("schema evaluation nests too deeply");
        }
        std::size_t& work = work_left();
        if (work <= depth)
        {
            throw evaluation_limit_error("schema evaluation takes too many steps");
        }
        work -= depth + 1;
    }
};

template <typename Json>
class eval_context
{"""

# The four eval_context constructors that extend the evaluation path.
EVAL_PATH_CONSTRUCTOR = re.compile(
    r"(eval_path_\(parent\.eval_path\(\) / (?:name|index)\),\s*flags_\((?:parent\.flags_|flags)\)\s*)\{\s*\}"
)

JSONPOINTER_PATH = "jsoncons_ext/jsonpointer/jsonpointer.hpp"
FORMAT_VALIDATORS_PATH = "jsoncons_ext/jsonschema/common/format_validators.hpp"
KEYWORD_VALIDATOR_PATH = "jsoncons_ext/jsonschema/common/keyword_validator.hpp"
VALIDATOR_PATH = "jsoncons_ext/jsonschema/common/validator.hpp"
VALIDATION_MESSAGE_PATH = "jsoncons_ext/jsonschema/validation_message.hpp"
KEYWORD_FACTORY_PATH = "jsoncons_ext/jsonschema/common/keyword_validator_factory.hpp"
BASIC_JSON_PATH = "jsoncons/basic_json.hpp"
DRAFT_2020_12_FACTORY_PATH = "jsoncons_ext/jsonschema/draft202012/schema_validator_factory_202012.hpp"
DRAFT_2019_09_FACTORY_PATH = "jsoncons_ext/jsonschema/draft201909/schema_validator_factory_201909.hpp"

# (path, old, new, expected count). The integer check converted the double to int64_t to compare.
NUMERIC_REPLACEMENTS = (
    (KEYWORD_VALIDATOR_PATH, "#include <cassert>\n", "#include <cassert>\n#include <cmath>\n", 1),
    (
        KEYWORD_VALIDATOR_PATH,
        "instance.is_double() && static_cast<double>(instance.template as<int64_t>()) == "
        "instance.template as<double>()",
        "instance.is_double() && std::isfinite(instance.template as<double>()) && "
        "std::trunc(instance.template as<double>()) == instance.template as<double>()",
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        "            double rem = std::remainder(x, multiple_of);\n",
        """            // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): a quotient that overflows
            // is not a multiple.
            if (!std::isfinite(x / multiple_of))
            {
                return false;
            }
            double rem = std::remainder(x, multiple_of);\n""",
        1,
    ),
    (KEYWORD_FACTORY_PATH, "#include <functional>\n", "#include <functional>\n#include <limits>\n", 1),
    (
        KEYWORD_FACTORY_PATH,
        "    template <typename Json>\n    using resolve_uri_type",
        """    // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): a size keyword's value. A double
    // beyond std::size_t saturates instead of converting with undefined behavior; the metaschema
    // has already checked that the value is a non-negative integer.
    template <typename Json>
    std::size_t keyword_size_value(const Json& value)
    {
        if (value.is_double())
        {
            const double number = value.template as<double>();
            const double largest = static_cast<double>((std::numeric_limits<std::size_t>::max)());
            if (!(number > 0))
            {
                return 0;
            }
            return number >= largest ? (std::numeric_limits<std::size_t>::max)()
                                     : static_cast<std::size_t>(number);
        }
        return value.template as<std::size_t>();
    }

    template <typename Json>
    using resolve_uri_type""",
        1,
    ),
    (KEYWORD_FACTORY_PATH, "sch.template as<std::size_t>()", "keyword_size_value(sch)", 6),
    (
        KEYWORD_FACTORY_PATH,
        "(*it).value().template as<std::size_t>()",
        "keyword_size_value((*it).value())",
        2,
    ),
    # basic_json::compare subtracted an integer converted to a double from a double, which rounds
    # integers beyond 2^53, so `enum`, `const` and `uniqueItems` found 9007199254740993 equal to
    # 9007199254740992.0. It compares them exactly instead, as the SDK's native evaluation does.
    (BASIC_JSON_PATH, "#include <algorithm> // std::swap\n", "#include <algorithm> // std::swap\n#include <cmath>\n", 1),
    (
        BASIC_JSON_PATH,
        """    template <typename CharT,typename Policy,typename Allocator>
    class basic_json
    {
""",
        """    namespace detail {

    // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): compares an integer with a double
    // exactly, -1, 0 or 1 as the integer is less than, equal to or greater than the double. A NaN
    // compares less than every integer, which keeps it unequal to each.
    template <typename Integer>
    int compare_integer_with_double(Integer integer, double number) noexcept
    {
        constexpr bool is_signed = std::is_signed<Integer>::value;
        constexpr double upper = is_signed ? 9223372036854775808.0 : 18446744073709551616.0;
        constexpr double lower = is_signed ? -9223372036854775808.0 : 0.0;
        if (std::isnan(number) || number < lower)
        {
            return 1;
        }
        if (number >= upper)
        {
            return -1;
        }
        const double whole = std::floor(number);
        const auto truncated = static_cast<Integer>(whole);
        if (integer != truncated)
        {
            return integer < truncated ? -1 : 1;
        }
        return number > whole ? -1 : 0;
    }

    } // namespace detail

    template <typename CharT,typename Policy,typename Allocator>
    class basic_json
    {
""",
        1,
    ),
    (
        BASIC_JSON_PATH,
        """                            double r = static_cast<double>(cast<int64_storage>().value()) - rhs.cast<double_storage>().value();
                            return r == 0.0 ? 0 : (r < 0.0 ? -1 : 1);
""",
        """                            return detail::compare_integer_with_double(cast<int64_storage>().value(), rhs.cast<double_storage>().value());
""",
        1,
    ),
    (
        BASIC_JSON_PATH,
        """                            auto r = static_cast<double>(cast<uint64_storage>().value()) - rhs.cast<double_storage>().value();
                            return r == 0 ? 0 : (r < 0.0 ? -1 : 1);
""",
        """                            return detail::compare_integer_with_double(cast<uint64_storage>().value(), rhs.cast<double_storage>().value());
""",
        1,
    ),
    *[
        (
            BASIC_JSON_PATH,
            f"""                            auto r = cast<double_storage>().value() - static_cast<double>(rhs.cast<{storage}>().value());
                            return r == 0 ? 0 : (r < 0.0 ? -1 : 1);
""",
            f"""                            const double number = cast<double_storage>().value();
                            return std::isnan(number) ? 1 : -detail::compare_integer_with_double(rhs.cast<{storage}>().value(), number);
""",
            1,
        )
        for storage in ("int64_storage", "uint64_storage")
    ],
    # The bound keywords compared an integer with a double after converting both to doubles. They
    # compare the two with basic_json::compare, which is exact.
    *[
        (
            KEYWORD_VALIDATOR_PATH,
            f"                if (instance.template as<double>() {operator} value_.template as<double>())\n",
            f"                if (instance.compare(value_) {operator} 0)\n",
            1,
        )
        for operator in (">", ">=", "<", "<=")
    ],
)

# (path, old, new, expected count), applied after NUMERIC_REPLACEMENTS.
EVALUATION_REPLACEMENTS = (
    # Keywords that loop over what the schema or the value holds, where one pass costs the product
    # of the two sizes and no eval_context is created to charge it.
    (
        KEYWORD_VALIDATOR_PATH,
        """            for (const auto& item : value_.array_range())
            {
                if (item == instance)\x20
                {
                    in_range = true;""",
        """            const std::size_t instance_cost = evaluation_limits::comparison_cost(instance);
            for (const auto& item : value_.array_range())
            {
                evaluation_limits::charge(8 + evaluation_limits::comparison_cost(item, instance_cost));
                if (item == instance)\x20
                {
                    in_range = true;""",
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """            for (const auto& dep : dependent_required_)\x20
            {
                auto prop = instance.find(dep.first);""",
        """            for (const auto& dep : dependent_required_)\x20
            {
                evaluation_limits::charge(1);
                auto prop = instance.find(dep.first);""",
        3,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """            for (const auto& prop : instance.object_range())\x20
            {
                auto prop_it = properties_.find(prop.key().c_str());""",
        """            for (const auto& prop : instance.object_range())\x20
            {
                evaluation_limits::charge(1);
                auto prop_it = properties_.find(prop.key().c_str());""",
        2,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """            for (auto const& prop : properties_)\x20
            {
                //std::cout << "   prop:" << prop.first << "\\n";
                const auto finding = instance.find(prop.first);""",
        """            for (auto const& prop : properties_)\x20
            {
                evaluation_limits::charge(1);
                //std::cout << "   prop:" << prop.first << "\\n";
                const auto finding = instance.find(prop.first);""",
        1,
    ),
    # A branching keyword keeps what each failed branch reported, and a message may quote the whole
    # value, so memory was not bounded by the work budget. A kept message costs work and is cut.
    (
        VALIDATOR_PATH,
        """            errors.push_back(msg);""",
        """            evaluation_limits::charge(64 + 8 * msg.details().size());
            errors.push_back(msg);""",
        1,
    ),
    (
        VALIDATION_MESSAGE_PATH,
        """              message_(std::move(message))
""",
        """              message_(std::move(message).substr(0, 512))
""",
        1,
    ),
    (
        VALIDATION_MESSAGE_PATH,
        """              message_(message),
""",
        """              message_(message.substr(0, 512)),
""",
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        "#include <cmath>\n",
        "#include <algorithm>\n#include <cmath>\n#include <functional>\n#include <memory>\n",
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """        static bool array_has_unique_items(const Json& a)\x20
        {
            for (auto it = a.array_range().begin(); it != a.array_range().end(); ++it)\x20
            {
                for (auto jt = it+1; jt != a.array_range().end(); ++jt)\x20
                {
                    if (*it == *jt)\x20
                    {
                        return false; // contains duplicates\x20
                    }
                }
            }
            return true; // elements are unique
        }
""",
        """        // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): upstream compares every pair, in time
        // quadratic in the array's length. Equal values have equal hashes, so only elements whose
        // hashes match are compared.
        static std::size_t unique_items_hash(const Json& value)
        {
            auto mix = [](std::size_t seed, std::size_t part) {
                return seed ^ (part + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
            };
            if (value.is_number())
            {
                // Integers and doubles that are equal must hash alike; -0.0 and 0.0 are equal.
                const double number = value.template as<double>();
                return mix(1, number == 0 ? 0 : std::hash<double>()(number));
            }
            if (value.is_string())
            {
                return mix(2, std::hash<std::string>()(value.template as<std::string>()));
            }
            if (value.is_array())
            {
                std::size_t seed = 3;
                for (const auto& element : value.array_range())
                {
                    seed = mix(seed, unique_items_hash(element));
                }
                return seed;
            }
            if (value.is_object())
            {
                // Members have no order that matters, so combine them with a commutative sum.
                std::size_t sum = 0;
                for (const auto& member : value.object_range())
                {
                    sum += mix(std::hash<std::string>()(std::string(member.key())),
                               unique_items_hash(member.value()));
                }
                return mix(4, sum);
            }
            return mix(5, value.is_null() ? 0 : (value.template as<bool>() ? 1 : 2));
        }

        static bool array_has_unique_items(const Json& a)
        {
            struct hashed_element
            {
                std::size_t hash;
                std::size_t cost;
                const Json* value;
            };
            std::vector<hashed_element> hashed;
            hashed.reserve(a.size());
            for (const auto& element : a.array_range())
            {
                hashed.push_back({unique_items_hash(element), evaluation_limits::comparison_cost(element),
                                  std::addressof(element)});
            }
            std::sort(hashed.begin(), hashed.end(), [](const hashed_element& lhs, const hashed_element& rhs) {
                return lhs.hash < rhs.hash;
            });
            for (std::size_t i = 0; i < hashed.size(); ++i)
            {
                for (std::size_t j = i + 1; j < hashed.size() && hashed[j].hash == hashed[i].hash; ++j)
                {
                    // Hashes an attacker chose can all collide, and each comparison may read the
                    // smaller of the two values whole.
                    evaluation_limits::charge(8 + (std::min)(hashed[i].cost, hashed[j].cost));
                    if (*hashed[i].value == *hashed[j].value)
                    {
                        return false; // contains duplicates
                    }
                }
            }
            return true; // elements are unique
        }
""",
        1,
    ),
    # `definitions` holds subschemas in every dialect the SDK accepts: the 2019-09 and 2020-12
    # metaschemas keep it next to `$defs`, and a `$ref` such as "#/definitions/name" names a
    # location in the document whatever keywords sit around it. jsoncons only registers those
    # subschemas in its "compatibility mode", which also turns on other pre-2019 keywords, so the
    # registration is made unconditional instead.
    (
        DRAFT_2020_12_FACTORY_PATH,
        """            if (this->options().compatibility_mode())
            {
                it = sch.find("definitions");""",
        """            {
                it = sch.find("definitions");""",
        1,
    ),
    (
        DRAFT_2019_09_FACTORY_PATH,
        """            if (this->options().compatibility_mode())
            {
                auto it = sch.find("definitions");""",
        """            {
                auto it = sch.find("definitions");""",
        1,
    ),
    # One map owns the subschemas of `definitions` and `$defs`, and both keywords can hold the same
    # name. Keyed by the name alone, the second subschema would be destroyed when its insertion
    # fails, after it registered itself and its subschemas as `$ref` targets, so a reference would
    # reach freed memory. Each key starts with its keyword, so every subschema keeps its entry.
    *[
        (
            path,
            f"""{{ "{keyword}", def.key() }};
{indent}defs.emplace(def.key(), """,
            f"""{{ "{keyword}", def.key() }};
{indent}defs.emplace("{keyword}/" + def.key(), """,
            1,
        )
        for path in (DRAFT_2020_12_FACTORY_PATH, DRAFT_2019_09_FACTORY_PATH)
        for keyword, indent in (("definitions", " " * 24), ("$defs", " " * 20))
    ],
    # The subschemas of `patternProperties`, `dependentSchemas` and `dependencies` were registered
    # at the keyword's own location, without their member name, so a `$ref` such as
    # "#/patternProperties/name" found no target. Each is registered at its own location.
    (
        KEYWORD_FACTORY_PATH,
        """jsoncons::string_view sub_keys[] = {keyword};
                pattern_properties.emplace_back(""",
        """jsoncons::string_view sub_keys[] = {keyword, prop.key()};
                pattern_properties.emplace_back(""",
        1,
    ),
    *[
        (
            KEYWORD_FACTORY_PATH,
            f'jsoncons::string_view sub_keys[] = {{"{keyword}"}};',
            f'jsoncons::string_view sub_keys[] = {{"{keyword}", dep.key()}};',
            1,
        )
        for keyword in ("dependencies", "dependentSchemas")
    ],
    # jsoncons reports a member that fails the `additionalProperties` subschema as one summary at
    # the object ("Additional property 'a' found but was invalid."). `properties`, `items` and the
    # other applicators report what the subschema reported, at the failing member, which is what a
    # violation's instance path promises; this does the same.
    (
        KEYWORD_VALIDATOR_PATH,
        """                            if (!local_reporter.errors.empty())
                            {
                                std::string message{"Additional property '"};
                                message.append(prop.key().c_str());
                                message.append("' found but was invalid.");
                                result = reporter.error(this->make_validation_message(
                                    this_context.eval_path(),
                                    instance_location,\x20
                                    message), patch);
                                if (result == walk_state::abort)
                                {
                                    return result;
                                }
                            }
""",
        """                            if (!local_reporter.errors.empty())
                            {
                                // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): report what the
                                // member's subschema reported, at the member, not one summary.
                                for (const auto& error : local_reporter.errors)
                                {
                                    result = reporter.error(error, patch);
                                    if (result == walk_state::abort)
                                    {
                                        return result;
                                    }
                                }
                            }
""",
        1,
    ),
    # A JSON pointer holds its tokens by value, and evaluation copies the instance location into
    # every step and every message, so a long property name above many items is copied once per
    # item. Copies are charged to the evaluation work budget, by tokens and bytes.
    (JSONPOINTER_PATH, "#include <map>\n", "#include <map>\n#include <stdexcept>\n", 1),
    (
        JSONPOINTER_PATH,
        """    // basic_json_pointer

    template <typename CharT>
    class basic_json_pointer
""",
        """    // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): the evaluation work budget of the
    // JSON Schema validation running on this thread (see jsonschema::evaluation_limits), kept here
    // so that copies of a pointer are charged to it.
    class evaluation_limit_error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct evaluation_work
    {
        // The initial value does not limit anything.
        static std::size_t& left()
        {
            static thread_local std::size_t work = static_cast<std::size_t>(-1);
            return work;
        }

        static void charge(std::size_t units)
        {
            std::size_t& work = left();
            if (work < units)
            {
                throw evaluation_limit_error("schema evaluation takes too much work");
            }
            work -= units;
        }

        // A copy of n tokens holding b bytes costs n + b / 32.
        template <typename Tokens>
        static void charge_copy(const Tokens& tokens)
        {
            std::size_t units = tokens.size();
            for (const auto& token : tokens)
            {
                units += token.size() / 32;
            }
            charge(units);
        }
    };

    // basic_json_pointer

    template <typename CharT>
    class basic_json_pointer
""",
        1,
    ),
    (
        JSONPOINTER_PATH,
        "        basic_json_pointer(const basic_json_pointer&) = default;\n",
        """        basic_json_pointer(const basic_json_pointer& other)
        {
            evaluation_work::charge_copy(other.tokens_);
            tokens_ = other.tokens_;
        }
""",
        1,
    ),
    (
        JSONPOINTER_PATH,
        "        basic_json_pointer& operator=(const basic_json_pointer&) = default;\n",
        """        basic_json_pointer& operator=(const basic_json_pointer& other)
        {
            evaluation_work::charge_copy(other.tokens_);
            tokens_ = other.tokens_;
            return *this;
        }
""",
        1,
    ),
    (
        JSONPOINTER_PATH,
        "            tokens_.emplace_back(s.data(), s.size());\n",
        "            evaluation_work::charge(1 + s.size() / 32);\n            tokens_.emplace_back(s.data(), s.size());\n",
        1,
    ),
    # Every branch of a branching keyword reports to a listener that keeps what it is told, and
    # each kept message holds a copy of its locations and of the messages under it. A listener
    # keeps the first few, and what they take is held against a memory budget while it lives.
    (VALIDATOR_PATH, "#include <unordered_set>\n", "#include <set>\n#include <unordered_set>\n", 1),
    (
        VALIDATOR_PATH,
        """    template <typename Json>
    struct collecting_error_listener : public error_reporter<Json>
    {
        std::vector<validation_message> errors;

    private:
        walk_state do_error(const validation_message& msg,\x20
            jsoncons::optional<Json>& /*patch*/) final
        {
            evaluation_limits::charge(64 + 8 * msg.details().size());
            errors.push_back(msg);
            return walk_state::advance;
        }
    };
""",
        """    // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): the memory a copy of @p msg takes,
    // with the messages under it.
    inline std::size_t message_bytes(const validation_message& msg)
    {
        std::size_t bytes = sizeof(validation_message) + msg.keyword().size() + msg.message().size() +
                            msg.schema_location().string().size();
        for (const auto& token : msg.instance_location())
        {
            bytes += sizeof(std::string) + token.size();
        }
        for (const auto& token : msg.eval_path())
        {
            bytes += sizeof(std::string) + token.size();
        }
        for (const auto& detail : msg.details())
        {
            bytes += message_bytes(detail);
        }
        return bytes;
    }

    template <typename Json>
    struct collecting_error_listener : public error_reporter<Json>
    {
        std::vector<validation_message> errors;

        collecting_error_listener() = default;
        collecting_error_listener(const collecting_error_listener&) = delete;
        collecting_error_listener& operator=(const collecting_error_listener&) = delete;

        ~collecting_error_listener()
        {
            evaluation_limits::release_message_bytes(kept_bytes_);
        }

    private:
        std::size_t kept_bytes_ = 0;

        walk_state do_error(const validation_message& msg,
            jsoncons::optional<Json>& /*patch*/) final
        {
            evaluation_limits::charge(64 + 8 * msg.details().size());
            if (errors.size() < evaluation_limits::max_kept_messages)
            {
                const std::size_t bytes = message_bytes(msg);
                evaluation_limits::keep_message_bytes(bytes);
                kept_bytes_ += bytes;
                errors.push_back(msg);
            }
            return walk_state::advance;
        }
    };
""",
        1,
    ),
    # Whether a branch failed is read from the count of messages, which still grows once the
    # listener stops keeping them.
    (KEYWORD_VALIDATOR_PATH, "local_reporter.errors.size()", "local_reporter.error_count()", 8),
    # A message is cut to 512 bytes only once its text is built, so quoting the whole value makes
    # each failed branch cost time in proportion to the value's size. Messages quote an excerpt.
    (
        KEYWORD_VALIDATOR_PATH,
        '"\'" + instance.template as<std::string>() + "\' is not a valid enum value."',
        'evaluation_limits::describe(instance) + " is not a valid enum value."',
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """                message.append(instance.as_cstring());
                message.append("' does not match pattern '");
                message.append(pattern_string_);""",
        """                message.append(evaluation_limits::excerpt(instance.as_string_view()));
                message.append("' does not match pattern '");
                message.append(evaluation_limits::excerpt(pattern_string_));""",
        1,
    ),
    (
        FORMAT_VALIDATORS_PATH,
        "message.append(sv.data(), sv.size());",
        "message.append(sv.data(), (std::min)(sv.size(), evaluation_limits::max_excerpt));",
        12,
    ),
    (FORMAT_VALIDATORS_PATH, "#include <cassert>\n", "#include <algorithm>\n#include <cassert>\n", 1),
    # Keywords that read a whole string take time in proportion to its length on every
    # evaluation, which no eval_context charges.
    (
        KEYWORD_VALIDATOR_PATH,
        """            auto sv = instance.as_string_view();
            std::size_t length = unicode_traits::count_codepoints(sv.data(), sv.size());""",
        """            auto sv = instance.as_string_view();
            evaluation_limits::charge(1 + sv.size() / 16);
            std::size_t length = unicode_traits::count_codepoints(sv.data(), sv.size());""",
        2,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """                auto sv = instance.as_string_view();

                walk_state result = validate_(""",
        """                auto sv = instance.as_string_view();
                evaluation_limits::charge(1 + sv.size() / 16);

                walk_state result = validate_(""",
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        "            std::string str = instance.as_string();\n",
        "            evaluation_limits::charge(1 + instance.as_string_view().size() / 4);\n"
        "            std::string str = instance.as_string();\n",
        1,
    ),
    (
        KEYWORD_VALIDATOR_PATH,
        """                auto s = instance.template as<jsoncons::string_view>();
                std::string content;""",
        """                auto s = instance.template as<jsoncons::string_view>();
                evaluation_limits::charge(1 + s.size() / 4);
                std::string content;""",
        1,
    ),
    (
        FORMAT_VALIDATORS_PATH,
        """        JSONCONS_TRY\x20
        {
            ::mcp::detail::Regex re(std::string_view(sv.data(), sv.size()));""",
        """        // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): compiling takes time in proportion
        // to the pattern's length and to its program, which Regex bounds.
        evaluation_limits::charge(::mcp::detail::Regex::max_program_size / 2 + sv.size());
        JSONCONS_TRY\x20
        {
            ::mcp::detail::Regex re(std::string_view(sv.data(), sv.size()));""",
        1,
    ),
    # Hashed sets of property names become ordered ones: property names chosen to share a hash
    # make each lookup in a hashed set scan all of them.
    (VALIDATOR_PATH, "std::unordered_set<std::string>", "std::set<std::string>", 2),
    (KEYWORD_VALIDATOR_PATH, "std::unordered_set<std::string>", "std::set<std::string>", 10),
    # `const` compares the value whole, so it is charged by what the comparison may read.
    (
        KEYWORD_VALIDATOR_PATH,
        """            if (value_ != instance)""",
        """            evaluation_limits::charge(
                8 + evaluation_limits::comparison_cost(value_, evaluation_limits::comparison_cost(instance)));
            if (value_ != instance)""",
        1,
    ),
    # A property name can be as long as the value. The messages naming one quote an excerpt.
    (
        KEYWORD_VALIDATOR_PATH,
        """                            message.append(prop.key().c_str());""",
        """                            message.append(evaluation_limits::excerpt(prop.key()));""",
        2,
    ),
)

# (pattern, replacement, expected matches across SUBSET), applied after the replacements above.
# jsoncons looks property names and keywords up by their C string, which ends at the first NUL, so
# it takes "a\u0000b" for "a" and "type\u0000x" for "type".
PATTERN_REPLACEMENTS = (
    (re.compile(r"\.find\((\w+)\.key\(\)\.c_str\(\)\)"), r".find(\1.key())", 14),
    # Instance locations keep at most evaluation_limits::max_location_token bytes of a name.
    (
        re.compile(r"instance_location / (prop\.key\(\)|prop\.name\(\)|prop\.first|dep\.first)"),
        r"instance_location / evaluation_limits::location_token(\1)",
        17,
    ),
)

# `jsoncons` as an identifier, not as part of an include path (`jsoncons/`) or of a longer
# name (`jsoncons_ext`).
NAMESPACE_PATTERN = re.compile(r"(?<![\w/])jsoncons(?![\w/])")
MACRO_PATTERN = re.compile(r"\bJSONCONS_")


def fetch_tarball(path: Path | None) -> bytes:
    if path is not None:
        data = path.read_bytes()
    else:
        with urllib.request.urlopen(TARBALL_URL, timeout=120) as response:  # noqa: S310
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != TARBALL_SHA256:
        raise SystemExit(f"jsoncons tarball SHA-256 mismatch: got {digest}, expected {TARBALL_SHA256}")
    return data


def read_release(data: bytes) -> tuple[dict[str, str], str]:
    """Return {path under include/: text} for SUBSET, and the LICENSE text."""
    headers: dict[str, str] = {}
    license_text = ""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            parts = member.name.split("/", 1)
            if len(parts) != 2:
                continue
            relative = parts[1]
            extracted = archive.extractfile(member)
            if extracted is None:
                continue
            if relative == "LICENSE":
                license_text = extracted.read().decode("utf-8")
            elif relative.startswith("include/"):
                inner = relative[len("include/") :]
                if any(inner.startswith(prefix + "/") for prefix in SUBSET):
                    if ".." in Path(inner).parts:
                        raise SystemExit(f"refusing unsafe path in tarball: {member.name}")
                    headers[inner] = extracted.read().decode("utf-8")
    if not license_text:
        raise SystemExit("jsoncons tarball has no LICENSE file")
    return headers, license_text


def transform(headers: dict[str, str]) -> dict[str, str]:
    counts = {old: 0 for old, _, _ in REGEX_REPLACEMENTS}
    result: dict[str, str] = {}
    for path, text in headers.items():
        for old, new, _ in REGEX_REPLACEMENTS:
            counts[old] += text.count(old)
            text = text.replace(old, new)
        if "std::regex" in text:
            raise SystemExit(f"std::regex left in {path} after the replacements")
        if path == EVAL_CONTEXT_PATH:
            text = add_evaluation_limits(text)
        for target, old, new, expected in NUMERIC_REPLACEMENTS + EVALUATION_REPLACEMENTS:
            if target == path:
                if text.count(old) != expected:
                    raise SystemExit(
                        f"expected {expected} occurrence(s) of {old!r} in {path}, found {text.count(old)}"
                    )
                text = text.replace(old, new)
        text = NAMESPACE_PATTERN.sub("mcp_detail_jsoncons", text)
        text = MACRO_PATTERN.sub("MCP_DETAIL_JSONCONS_", text)
        result[path] = text
    for old, _, expected in REGEX_REPLACEMENTS:
        if counts[old] != expected:
            raise SystemExit(f"expected {expected} occurrence(s) of {old!r}, found {counts[old]}")
    for pattern, replacement, expected in PATTERN_REPLACEMENTS:
        found = 0
        for path, text in result.items():
            result[path], matches = pattern.subn(replacement, text)
            found += matches
        if found != expected:
            raise SystemExit(f"expected {expected} match(es) of {pattern.pattern!r}, found {found}")
    return result


def add_evaluation_limits(text: str) -> str:
    anchor = "template <typename Json>\nclass eval_context\n{"
    if text.count(anchor) != 1 or text.count("#include <vector>\n") != 1:
        raise SystemExit(f"{EVAL_CONTEXT_PATH} changed upstream; review the evaluation limits patch")
    text = text.replace(
        "#include <vector>\n",
        "#include <stdexcept>\n#include <string_view>\n#include <vector>\n\n#include <mcp/detail/regex.hpp>\n",
    )
    text = text.replace(anchor, EVALUATION_LIMITS)
    text, count = EVAL_PATH_CONSTRUCTOR.subn(
        lambda match: match.group(1) + "{\n        evaluation_limits::enter(eval_path_.tokens().size());\n    }",
        text,
    )
    if count != 4:
        raise SystemExit(f"expected 4 eval_context constructors that extend the path, found {count}")
    # An evaluation path holds the instance's property names too.
    named = "eval_path_(parent.eval_path() / name)"
    if text.count(named) != 2:
        raise SystemExit(f"expected 2 eval_context constructors that extend the path by a name")
    return text.replace(named, "eval_path_(parent.eval_path() / evaluation_limits::location_token(name))")


def provenance() -> str:
    return (
        f"jsoncons {VERSION}\n"
        f"source: {TARBALL_URL}\n"
        f"sha256: {TARBALL_SHA256}\n"
        f"subset: {', '.join(SUBSET)}\n"
        f"transform: {TRANSFORM_VERSION} (scripts/vendor_jsoncons.py: std::regex replaced with "
        "mcp::detail::Regex, schema patterns compiled within a PatternBudget; evaluation depth, work "
        "and pattern matching limits added to eval_context, with JSON pointer copies, whole-string "
        "keywords and kept branch messages charged, and messages quoting excerpts; integer type "
        "check, size keywords and multipleOf made safe for large doubles; integers and doubles "
        "compared exactly; uniqueItems compares only "
        "elements with equal hashes; subschemas under definitions registered in every dialect and "
        "kept apart from those under $defs of the same name; subschemas of patternProperties, "
        "dependentSchemas and dependencies registered at their own locations; "
        "additionalProperties violations reported at the member; property names looked up by full "
        "length in ordered sets; "
        "namespace jsoncons renamed "
        "mcp_detail_jsoncons; macro prefix JSONCONS_ renamed MCP_DETAIL_JSONCONS_)\n"
    )


def write_tree(root: Path, headers: dict[str, str], license_text: str) -> None:
    for path, text in sorted(headers.items()):
        target = root / "include" / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")
    (root / "LICENSE").write_text(license_text, encoding="utf-8", newline="\n")
    (root / "PROVENANCE").write_text(provenance(), encoding="utf-8", newline="\n")


def tree_files(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare instead of writing")
    parser.add_argument("--tarball", type=Path, help="local copy of the pinned release tarball")
    args = parser.parse_args()

    headers, license_text = read_release(fetch_tarball(args.tarball))
    transformed = transform(headers)

    if not args.check:
        readme = OUTPUT_DIR / "README.md"
        readme_text = readme.read_bytes() if readme.exists() else None
        if OUTPUT_DIR.exists():
            shutil.rmtree(OUTPUT_DIR)
        write_tree(OUTPUT_DIR, transformed, license_text)
        if readme_text is not None:
            readme.write_bytes(readme_text)
        print(f"wrote {len(transformed)} headers to {OUTPUT_DIR.relative_to(REPO_ROOT)}")
        return 0

    with tempfile.TemporaryDirectory() as scratch:
        expected_root = Path(scratch)
        write_tree(expected_root, transformed, license_text)
        expected = tree_files(expected_root)
    actual = tree_files(OUTPUT_DIR) if OUTPUT_DIR.exists() else {}
    # README.md is written by hand next to the generated files.
    actual.pop("README.md", None)
    problems = sorted(
        [f"missing: {name}" for name in expected.keys() - actual.keys()]
        + [f"unexpected: {name}" for name in actual.keys() - expected.keys()]
        + [f"differs: {name}" for name in expected.keys() & actual.keys() if expected[name] != actual[name]]
    )
    spec = (REPO_ROOT / "packaging" / "rpm" / "mcp-cpp-sdk.spec.in").read_text(encoding="utf-8")
    if f"Provides:       bundled(jsoncons) = {VERSION}\n" not in spec:
        problems.append(f"packaging/rpm/mcp-cpp-sdk.spec.in does not declare bundled(jsoncons) = {VERSION}")
    if problems:
        print("third_party/jsoncons does not match the pinned release plus the recorded transformation:")
        for problem in problems:
            print(f"  {problem}")
        print("Regenerate it with: python3 scripts/vendor_jsoncons.py")
        return 1
    print(f"third_party/jsoncons matches jsoncons {VERSION} plus transform {TRANSFORM_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
