#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/skills.hpp>
#include <mcp/server/server.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

/**
 * @file skills.hpp
 * @brief Serving Agent Skills from a Server with the Skills extension.
 */

namespace mcp::skills {

/// One file of a skill.
struct SkillFile {
    /// Path relative to the skill directory, `/`-separated, such as `SKILL.md` or
    /// `references/FORMS.md`. No empty, `.` or `..` segments.
    std::string path;
    /// The file's bytes. Ignored when @ref generate is set.
    std::string content;
    /// MIME type; guessed from the file extension when unset. Text types are sent as `text`, every
    /// other type as a base64 `blob`.
    std::optional<std::string> mime_type;
    /// Produces the bytes at each read. A skill with such a file is listed with
    /// `"resources": "dynamic"` and no digests, because its content can change between reads.
    std::function<std::string()> generate;
};

/// A skill to serve.
struct SkillDefinition {
    /// The skill's path in the server's namespace, such as `git-workflow` or `acme/billing/refunds`.
    /// Its last segment is the skill's name.
    std::string path;
    /// The SKILL.md frontmatter as JSON. Must hold string `name` and `description`; `name` equals the
    /// last segment of @ref path. It is published as is and must match the frontmatter of the
    /// SKILL.md file below; a host compares the two.
    nlohmann::json frontmatter;
    /// Every file of the skill. Must include `SKILL.md`.
    std::vector<SkillFile> files;
    /// `_meta` of the skill's entry.
    std::optional<nlohmann::json> meta;
};

/// Behaviour of the Skills extension on a server.
struct SkillsOptions {
    /// Declare `directoryRead` and serve `resources/directory/read`.
    bool directory_read{true};
    /// `ttlMs` the listings and entries carry.
    std::int64_t ttl_ms{mcp::constants::skills::g_DEFAULT_TTL_MS};
    /// `cacheScope` the listings and entries carry.
    CacheScope cache_scope{CacheScope::ePublic};
    /// Entries or children per page of `skills/list` and `resources/directory/read`; at least 1.
    std::size_t page_size{mcp::constants::skills::g_DEFAULT_PAGE_SIZE};
};

/**
 * @brief Declares the Skills extension on a Server and serves the skills added to it.
 *
 * @details The constructor declares `io.modelcontextprotocol/skills` and registers `skills/list`,
 * `skills/get` and, when enabled, `resources/directory/read`. add_skill() registers each file of a
 * skill as a resource, so `resources/read` serves it. Skills are added before the server serves its
 * first request. The Server must outlive this object's add_skill() calls; the registered handlers do
 * not need the object itself.
 */
class MCP_API SkillsExtension {
   public:
    /**
     * @param server The server to extend, whose capabilities gain `resources` when they lack it.
     * @param options Extension behaviour.
     * @throws std::invalid_argument When `options.page_size` is 0.
     */
    explicit SkillsExtension(Server& server, SkillsOptions options = {});

    /**
     * @brief Serve a skill.
     *
     * @throws std::invalid_argument When the definition breaks the extension's rules (see
     *         validate_skill(); the files must include SKILL.md and use plain relative paths) or its
     *         URIs are registered already.
     */
    void add_skill(SkillDefinition skill);

   private:
    struct State;
    Server& server_;
    std::shared_ptr<State> state_;
};

}  // namespace mcp::skills
