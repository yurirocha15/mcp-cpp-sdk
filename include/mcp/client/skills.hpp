#pragma once

#include <mcp/client/client.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/skills.hpp>

#include <optional>
#include <string>

/**
 * @file skills.hpp
 * @brief Client-side calls of the Skills extension.
 *
 * @details These send `skills/list`, `skills/get` and `resources/directory/read` and read skill
 * files with the checks the extension requires of a host before it uses content. Approval,
 * frontmatter comparison, caching and presenting a skill to a model are the host application's
 * work; see the Security Considerations of the specification.
 */

namespace mcp::skills {

/**
 * @brief List the skills a server serves, one page.
 *
 * @details Call only when supports_skills() holds for the server's capabilities. An empty or
 * partial listing does not show that the server has no other skills; get_skill() answers for any
 * skill the server serves.
 *
 * @throws std::invalid_argument When an entry is invalid (see validate_skill()).
 * @throws McpError When the server refuses the request.
 */
MCP_API Task<ListSkillsResult> list_skills(Client& client,
                                           std::optional<std::string> cursor = std::nullopt);

/**
 * @brief Fetch the entry of one skill by the URI of its SKILL.md.
 *
 * @throws std::invalid_argument When the entry is invalid.
 * @throws McpError When the server does not serve that skill (-32602).
 */
MCP_API Task<GetSkillResult> get_skill(Client& client, std::string uri);

/**
 * @brief List the direct children of a directory resource, one page.
 *
 * @throws std::logic_error When @p supports_directory_read is false; a client does not send
 *         `resources/directory/read` to a server that did not declare `directoryRead: true`.
 * @throws McpError When the URI is not a directory resource the server serves (-32602).
 */
MCP_API Task<ReadResourceDirectoryResult> read_directory(
    Client& client, std::string uri, bool supports_directory_read,
    std::optional<std::string> cursor = std::nullopt);

/**
 * @brief Read one file of a skill and verify it against the entry the host holds.
 *
 * @details The file must be listed in @p entry; a URI the entry does not list is a verification
 * failure. The bytes (decoded from base64 for a blob) must match the listed size and SHA-256
 * digest. A dynamic entry carries neither, so its files are returned unverified.
 *
 * @return The file's bytes.
 * @throws std::runtime_error On a verification failure or when the read returns no content for the
 *         URI. The host then refreshes the entry with get_skill() and treats a changed `resources`
 *         set as a revoked approval.
 * @throws McpError When the server refuses the read.
 */
MCP_API Task<std::string> read_skill_file(Client& client, Skill entry, std::string uri);

}  // namespace mcp::skills
