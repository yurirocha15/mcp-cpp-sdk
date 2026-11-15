#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/extensions.hpp>
#include <mcp/protocol/resources.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file skills.hpp
 * @brief Wire types and checks of the Skills extension (`io.modelcontextprotocol/skills`).
 *
 * @details A server serves Agent Skills (agentskills.io) as resources: each file of a skill
 * directory is a resource, conventionally `skill://<skill-path>/<file-path>`, and the skill's entry
 * lists its files with SHA-256 digests. The extension adds `skills/list`, `skills/get` and, when the
 * server declares `directoryRead`, `resources/directory/read`. The types follow the beta
 * specification in github.com/modelcontextprotocol/ext-skills, specification/beta/skills.mdx.
 */

namespace mcp::skills {}  // namespace mcp::skills

namespace mcp {

/// One file of a skill with the digest and size of its raw bytes.
struct SkillResource {
    std::string uri;       ///< Resource URI of the file.
    std::string digest;    ///< `sha256:` and 64 lowercase hexadecimal characters.
    std::int64_t size{0};  ///< Length in bytes of the raw content the digest covers.
};

/// The entry for one skill, as `skills/list` and `skills/get` return it.
struct Skill {
    std::string uri;  ///< Resource URI of the skill's SKILL.md.
    /// The SKILL.md frontmatter as a JSON object; `name` and `description` are always present.
    nlohmann::json frontmatter;
    /// Every file of the skill, SKILL.md included, when @ref dynamic is false.
    std::vector<SkillResource> resources;
    /// The wire carries `"resources": "dynamic"`: the content is generated, so no digests exist.
    bool dynamic{false};
    std::optional<nlohmann::json>
        meta;  ///< `_meta`; this extension's keys use `io.modelcontextprotocol.skills/`.
};

/// Result of `skills/list`.
struct ListSkillsResult {
    std::vector<Skill> skills;              ///< Entries of this page.
    std::optional<std::string> nextCursor;  ///< Cursor of the next page.
    std::optional<std::int64_t> ttlMs;      ///< Freshness hint of the listing.
    std::optional<CacheScope> cacheScope;   ///< Cache scope of the listing.
};

/// Result of `skills/get`.
struct GetSkillResult {
    Skill skill;                           ///< The entry.
    std::optional<std::int64_t> ttlMs;     ///< Freshness hint of the entry.
    std::optional<CacheScope> cacheScope;  ///< Cache scope of the entry.
};

/// Result of `resources/directory/read`.
struct ReadResourceDirectoryResult {
    std::vector<Resource> resources;        ///< The direct children of the directory.
    std::optional<std::string> nextCursor;  ///< Cursor of the next page.
    std::optional<std::int64_t> ttlMs;      ///< Freshness hint of the listing.
    std::optional<CacheScope> cacheScope;   ///< Cache scope of the listing.
};

/// Writes `uri`, `digest` and `size`.
MCP_API void to_json(nlohmann::json& json_obj, const SkillResource& resource);

/// Reads `uri`, `digest` and `size`; `at()` throws when one is missing.
MCP_API void from_json(const nlohmann::json& json_obj, SkillResource& resource);

/// Writes `resources` as `"dynamic"` when the skill is dynamic and as the file list otherwise.
MCP_API void to_json(nlohmann::json& json_obj, const Skill& skill);

/// @throws std::invalid_argument For an entry the extension calls invalid: no `frontmatter` object
///         with string `name` and `description`, or `resources` that is neither an array nor the
///         string `"dynamic"`.
MCP_API void from_json(const nlohmann::json& json_obj, Skill& skill);

/// Writes the page of entries, the cursor and the cache hints that are set.
MCP_API void to_json(nlohmann::json& json_obj, const ListSkillsResult& result);

/// Reads the page of entries, the cursor and the cache hints.
MCP_API void from_json(const nlohmann::json& json_obj, ListSkillsResult& result);

/// Writes the entry and the cache hints that are set.
MCP_API void to_json(nlohmann::json& json_obj, const GetSkillResult& result);

/// Reads the entry and the cache hints.
MCP_API void from_json(const nlohmann::json& json_obj, GetSkillResult& result);

/// Writes the children, the cursor and the cache hints that are set.
MCP_API void to_json(nlohmann::json& json_obj, const ReadResourceDirectoryResult& result);

/// Reads the children, the cursor and the cache hints.
MCP_API void from_json(const nlohmann::json& json_obj, ReadResourceDirectoryResult& result);

}  // namespace mcp

namespace mcp::skills {

/// The settings object a server declares: `{"directoryRead": true}` or an empty object.
[[nodiscard]] inline nlohmann::json server_settings(bool directory_read) {
    nlohmann::json settings = nlohmann::json::object();
    if (directory_read) {
        settings["directoryRead"] = true;
    }
    return settings;
}

/// @return Whether a server declared the extension.
[[nodiscard]] inline bool supports_skills(const ServerCapabilities& capabilities) {
    return declares_extension(capabilities, mcp::constants::skills::g_EXTENSION_ID);
}

/// @return Whether a server declared the extension with `directoryRead: true`. A client does not
///         call `resources/directory/read` on any other server.
[[nodiscard]] inline bool supports_directory_read(const ServerCapabilities& capabilities) {
    const auto settings = extension_settings(capabilities, mcp::constants::skills::g_EXTENSION_ID);
    if (!settings) {
        return false;
    }
    const auto flag = settings->find("directoryRead");
    return flag != settings->end() && flag->is_boolean() && flag->get<bool>();
}

/// @return `sha256:` followed by the 64 lowercase hexadecimal characters of the SHA-256 of @p bytes.
[[nodiscard]] MCP_API std::string sha256_digest(std::string_view bytes);

/// @return Whether @p name meets the Agent Skills naming rules: 1 to 64 characters, lowercase
///         letters, digits and hyphens, no leading, trailing or doubled hyphen.
[[nodiscard]] MCP_API bool is_valid_skill_name(std::string_view name);

/// @return The skill directory URI of a `SKILL.md` URI (the URI without `/SKILL.md`), or nothing
///         when @p skill_uri does not end in `/SKILL.md`.
[[nodiscard]] MCP_API std::optional<std::string> skill_root_uri(std::string_view skill_uri);

/**
 * @brief Check an entry against the rules the extension puts on it.
 *
 * @details Checks that the URI ends in `/SKILL.md` and that the last segment of the skill path equals
 * `frontmatter.name` and is a valid name; that `description` is 1 to 1024 characters; and, unless the
 * skill is dynamic, that `resources` lists `SKILL.md` and every other file once, each inside the
 * skill directory, with a well-formed digest and a non-negative size, within 512 files and 16 MiB.
 *
 * @return The problems found, empty when the entry is valid. A host does not load an entry with
 *         problems.
 */
[[nodiscard]] MCP_API std::vector<std::string> validate_skill(const Skill& skill);

/// What verify_content() found.
struct Verification {
    bool ok{false};       ///< The content is what the entry described.
    std::string message;  ///< Why it is not, empty when it is.
};

/**
 * @brief Compare the bytes of a file with the entry that listed it.
 *
 * @details A length that differs from `size` or a digest that differs is a mismatch; such content is
 * not used. Digests are unsigned and come from the server that serves the content, so a match shows
 * only that the two agree.
 */
[[nodiscard]] MCP_API Verification verify_content(const SkillResource& entry, std::string_view bytes);

}  // namespace mcp::skills
