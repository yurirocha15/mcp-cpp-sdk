#include <mcp/client/skills.hpp>
#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>

#include "../detail/text/encoding.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace methods = mcp::constants::methods;

namespace mcp::skills {

namespace {

nlohmann::json paginated_params(const std::optional<std::string>& cursor) {
    nlohmann::json params = nlohmann::json::object();
    if (cursor) {
        params["cursor"] = *cursor;
    }
    return params;
}

void require_valid(const Skill& skill) {
    if (const auto problems = validate_skill(skill); !problems.empty()) {
        throw std::invalid_argument("Invalid skill entry " + skill.uri + ": " + problems.front());
    }
}

}  // namespace

Task<ListSkillsResult> list_skills(Client& client, std::optional<std::string> cursor) {
    // Named: a temporary optional passed into the coroutine call trips GCC 11.
    const std::optional<nlohmann::json> params = paginated_params(cursor);
    const auto raw = co_await client.send_request(methods::g_SKILLS_LIST, params);
    auto result = raw.get<ListSkillsResult>();
    for (const auto& skill : result.skills) {
        require_valid(skill);
    }
    co_return result;
}

Task<GetSkillResult> get_skill(Client& client, std::string uri) {
    const std::optional<nlohmann::json> params = nlohmann::json{{"uri", uri}};
    const auto raw = co_await client.send_request(methods::g_SKILLS_GET, params);
    auto result = raw.get<GetSkillResult>();
    require_valid(result.skill);
    co_return result;
}

Task<ReadResourceDirectoryResult> read_directory(Client& client, std::string uri,
                                                 bool supports_directory_read,
                                                 std::optional<std::string> cursor) {
    if (!supports_directory_read) {
        throw std::logic_error(
            "The server did not declare directoryRead; resources/directory/read is not sent");
    }
    auto built = paginated_params(cursor);
    built["uri"] = uri;
    const std::optional<nlohmann::json> params = std::move(built);
    const auto raw = co_await client.send_request(methods::g_RESOURCES_DIRECTORY_READ, params);
    co_return raw.get<ReadResourceDirectoryResult>();
}

Task<std::string> read_skill_file(Client& client, Skill entry, std::string uri) {
    const SkillResource* listed = nullptr;
    if (!entry.dynamic) {
        const auto found = std::find_if(entry.resources.begin(), entry.resources.end(),
                                        [&](const SkillResource& item) { return item.uri == uri; });
        if (found == entry.resources.end()) {
            throw std::runtime_error("Verification failed: " + uri + " is not listed in the entry of " +
                                     entry.uri);
        }
        listed = &*found;
    }

    const auto result = co_await client.read_resource(uri);
    for (const auto& content : result.contents) {
        std::string bytes;
        if (const auto* text = std::get_if<TextResourceContents>(&content)) {
            if (text->uri != uri) {
                continue;
            }
            bytes = text->text;
        } else {
            const auto& blob = std::get<BlobResourceContents>(content);
            if (blob.uri != uri) {
                continue;
            }
            auto decoded = mcp::detail::base64_decode(blob.blob);
            if (!decoded) {
                throw std::runtime_error("Verification failed: " + uri + " carries invalid base64");
            }
            bytes = std::move(*decoded);
        }
        if (listed != nullptr) {
            const auto verdict = verify_content(*listed, bytes);
            if (!verdict.ok) {
                throw std::runtime_error("Verification failed: " + verdict.message);
            }
        }
        co_return bytes;
    }
    throw std::runtime_error("Reading " + uri + " returned no content for that URI");
}

}  // namespace mcp::skills
