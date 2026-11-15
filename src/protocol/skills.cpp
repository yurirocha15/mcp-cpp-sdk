#include <mcp/auth/oauth.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/protocol/skills.hpp>

#include <algorithm>
#include <set>
#include "../detail/constants.hpp"
#include "cache_hints.hpp"

namespace encoding = mcp::detail::constants::encoding;

namespace mcp {

void to_json(nlohmann::json& json_obj, const SkillResource& resource) {
    json_obj =
        nlohmann::json{{"uri", resource.uri}, {"digest", resource.digest}, {"size", resource.size}};
}

void from_json(const nlohmann::json& json_obj, SkillResource& resource) {
    json_obj.at("uri").get_to(resource.uri);
    json_obj.at("digest").get_to(resource.digest);
    json_obj.at("size").get_to(resource.size);
}

void to_json(nlohmann::json& json_obj, const Skill& skill) {
    json_obj = nlohmann::json{{"uri", skill.uri}, {"frontmatter", skill.frontmatter}};
    if (skill.dynamic) {
        json_obj["resources"] = "dynamic";
    } else {
        json_obj["resources"] = skill.resources;
    }
    if (skill.meta) {
        json_obj["_meta"] = *skill.meta;
    }
}

void from_json(const nlohmann::json& json_obj, Skill& skill) {
    skill = Skill{};
    json_obj.at("uri").get_to(skill.uri);
    const auto& frontmatter = json_obj.at("frontmatter");
    if (!frontmatter.is_object() || !frontmatter.contains("name") ||
        !frontmatter.at("name").is_string() || !frontmatter.contains("description") ||
        !frontmatter.at("description").is_string()) {
        throw std::invalid_argument("A skill entry's frontmatter needs a string name and description");
    }
    skill.frontmatter = frontmatter;
    const auto& resources = json_obj.at("resources");
    if (resources.is_array()) {
        resources.get_to(skill.resources);
    } else if (resources.is_string() && resources.get_ref<const std::string&>() == "dynamic") {
        skill.dynamic = true;
    } else {
        throw std::invalid_argument("A skill entry's resources must be an array or \"dynamic\"");
    }
    if (const auto meta = json_obj.find("_meta"); meta != json_obj.end()) {
        skill.meta = *meta;
    }
}

void to_json(nlohmann::json& json_obj, const ListSkillsResult& result) {
    json_obj = nlohmann::json{{"skills", result.skills}};
    if (result.nextCursor) {
        json_obj["nextCursor"] = *result.nextCursor;
    }
    detail::write_cache_hints(json_obj, result.ttlMs, result.cacheScope);
}

void from_json(const nlohmann::json& json_obj, ListSkillsResult& result) {
    json_obj.at("skills").get_to(result.skills);
    if (detail::has_json_value(json_obj, "nextCursor")) {
        result.nextCursor = json_obj.at("nextCursor").get<std::string>();
    }
    detail::read_cache_hints(json_obj, result.ttlMs, result.cacheScope);
}

void to_json(nlohmann::json& json_obj, const GetSkillResult& result) {
    json_obj = nlohmann::json{{"skill", result.skill}};
    detail::write_cache_hints(json_obj, result.ttlMs, result.cacheScope);
}

void from_json(const nlohmann::json& json_obj, GetSkillResult& result) {
    json_obj.at("skill").get_to(result.skill);
    detail::read_cache_hints(json_obj, result.ttlMs, result.cacheScope);
}

void to_json(nlohmann::json& json_obj, const ReadResourceDirectoryResult& result) {
    json_obj = nlohmann::json{{"resources", result.resources}};
    if (result.nextCursor) {
        json_obj["nextCursor"] = *result.nextCursor;
    }
    detail::write_cache_hints(json_obj, result.ttlMs, result.cacheScope);
}

void from_json(const nlohmann::json& json_obj, ReadResourceDirectoryResult& result) {
    json_obj.at("resources").get_to(result.resources);
    if (detail::has_json_value(json_obj, "nextCursor")) {
        result.nextCursor = json_obj.at("nextCursor").get<std::string>();
    }
    detail::read_cache_hints(json_obj, result.ttlMs, result.cacheScope);
}

}  // namespace mcp

namespace mcp::skills {

namespace {

bool is_lowercase_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

/// Whether @p segment is a dot segment, written plainly or with percent-encoded dots.
bool is_dot_segment(std::string_view segment) {
    std::string decoded;
    for (std::size_t index = 0; index < segment.size(); ++index) {
        if (segment[index] == '%' && index + 2 < segment.size() && segment[index + 1] == '2' &&
            (segment[index + 2] == 'e' || segment[index + 2] == 'E')) {
            decoded.push_back('.');
            index += 2;
        } else {
            decoded.push_back(segment[index]);
        }
    }
    return decoded == "." || decoded == "..";
}

/// Describes what is wrong with @p relative, a file path under a skill's directory, or returns an
/// empty string. It applies the rule the server enforces on the paths it serves.
std::string path_problem(std::string_view relative) {
    if (relative.empty()) {
        return "is empty";
    }
    if (relative.find('\\') != std::string_view::npos) {
        return "contains a backslash";
    }
    if (relative.find('\0') != std::string_view::npos) {
        return "contains a NUL";
    }
    std::size_t start = 0;
    while (true) {
        const auto slash = relative.find('/', start);
        const auto segment =
            relative.substr(start, slash == std::string_view::npos ? slash : slash - start);
        if (segment.empty() || is_dot_segment(segment)) {
            return "has an empty, . or .. segment";
        }
        if (slash == std::string_view::npos) {
            return {};
        }
        start = slash + 1;
    }
}

}  // namespace

std::string sha256_digest(std::string_view bytes) {
    const auto digest = auth::detail::sha256(std::string(bytes));
    std::string text(mcp::detail::constants::skills::g_DIGEST_PREFIX);
    text.reserve(mcp::detail::constants::skills::g_DIGEST_PREFIX.size() +
                 mcp::detail::constants::skills::g_DIGEST_HEX_LENGTH);
    for (const auto byte : digest) {
        text.push_back(encoding::g_HEX_DIGITS[byte >> 4]);
        text.push_back(encoding::g_HEX_DIGITS[byte & 0x0F]);
    }
    return text;
}

bool is_valid_skill_name(std::string_view name) {
    if (name.empty() || name.size() > mcp::detail::constants::skills::g_MAX_NAME_LENGTH ||
        name.front() == '-' || name.back() == '-' || name.find("--") != std::string_view::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    });
}

std::optional<std::string> skill_root_uri(std::string_view skill_uri) {
    const std::string suffix = "/" + std::string(mcp::constants::skills::g_SKILL_FILE_NAME);
    if (skill_uri.size() <= suffix.size() ||
        skill_uri.substr(skill_uri.size() - suffix.size()) != suffix) {
        return std::nullopt;
    }
    return std::string(skill_uri.substr(0, skill_uri.size() - suffix.size()));
}

std::vector<std::string> validate_skill(const Skill& skill) {
    std::vector<std::string> problems;
    const auto root = skill_root_uri(skill.uri);
    if (!root) {
        problems.push_back("uri does not end in /SKILL.md: " + skill.uri);
    }

    const auto& name = skill.frontmatter.at("name").get_ref<const std::string&>();
    if (!is_valid_skill_name(name)) {
        problems.push_back("name is not a valid skill name: " + name);
    }
    if (root) {
        // The skill path is what follows the scheme separator; its last segment is the name.
        const auto separator = root->find("://");
        const auto path = separator == std::string::npos ? *root : root->substr(separator + 3);
        const auto last_slash = path.rfind('/');
        const auto segment = last_slash == std::string::npos ? path : path.substr(last_slash + 1);
        if (segment != name) {
            problems.push_back("the last path segment " + segment + " does not equal the name " + name);
        }
    }
    const auto& description = skill.frontmatter.at("description").get_ref<const std::string&>();
    if (description.empty() ||
        description.size() > mcp::detail::constants::skills::g_MAX_DESCRIPTION_LENGTH) {
        problems.push_back("description must be 1 to 1024 characters");
    }

    if (skill.dynamic) {
        return problems;
    }
    if (skill.resources.size() > mcp::constants::skills::g_MAX_RESOURCES_PER_SKILL) {
        problems.push_back("lists more than 512 files");
    }
    std::set<std::string> seen;
    std::int64_t total = 0;
    bool lists_skill_file = false;
    for (const auto& resource : skill.resources) {
        if (!seen.insert(resource.uri).second) {
            problems.push_back("lists a file twice: " + resource.uri);
        }
        if (resource.uri == skill.uri) {
            lists_skill_file = true;
        } else if (root && resource.uri.compare(0, root->size() + 1, *root + "/") != 0) {
            problems.push_back("lists a file outside the skill directory: " + resource.uri);
        } else if (root) {
            const auto problem = path_problem(std::string_view(resource.uri).substr(root->size() + 1));
            if (!problem.empty()) {
                problems.push_back("lists a file whose path " + problem + ": " + resource.uri);
            }
        }
        const bool well_formed =
            resource.digest.size() == mcp::detail::constants::skills::g_DIGEST_PREFIX.size() +
                                          mcp::detail::constants::skills::g_DIGEST_HEX_LENGTH &&
            resource.digest.compare(0, mcp::detail::constants::skills::g_DIGEST_PREFIX.size(),
                                    mcp::detail::constants::skills::g_DIGEST_PREFIX) == 0 &&
            is_lowercase_hex(std::string_view(resource.digest)
                                 .substr(mcp::detail::constants::skills::g_DIGEST_PREFIX.size()));
        if (!well_formed) {
            problems.push_back("digest of " + resource.uri + " is not sha256:<64 lowercase hex>");
        }
        if (resource.size < 0) {
            problems.push_back("size of " + resource.uri + " is negative");
        } else {
            total += std::min(resource.size, mcp::constants::skills::g_MAX_SKILL_BYTES + 1);
        }
    }
    if (!lists_skill_file) {
        problems.push_back("resources does not list SKILL.md");
    }
    if (total > mcp::constants::skills::g_MAX_SKILL_BYTES) {
        problems.push_back("listed files add up to more than 16 MiB");
    }
    return problems;
}

Verification verify_content(const SkillResource& entry, std::string_view bytes) {
    if (static_cast<std::int64_t>(bytes.size()) != entry.size) {
        return {false, "size of " + entry.uri + " is " + std::to_string(bytes.size()) +
                           " bytes, the entry lists " + std::to_string(entry.size)};
    }
    if (sha256_digest(bytes) != entry.digest) {
        return {false, "digest of " + entry.uri + " differs from the entry"};
    }
    return {true, {}};
}

}  // namespace mcp::skills
