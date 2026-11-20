#include <mcp/auth/oauth.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/server/skills.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include "../detail/constants.hpp"

namespace mcp::skills {

namespace {

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (true) {
        const auto slash = path.find('/', start);
        segments.emplace_back(
            path.substr(start, slash == std::string_view::npos ? slash : slash - start));
        if (slash == std::string_view::npos) {
            return segments;
        }
        start = slash + 1;
    }
}

/// Throws unless @p path is made of non-empty segments other than `.` and `..`.
void require_plain_path(const std::string& path, const std::string& what) {
    if (path.empty()) {
        throw std::invalid_argument(what + " is empty");
    }
    for (const auto& segment : split_path(path)) {
        if (segment.empty() || segment == "." || segment == "..") {
            throw std::invalid_argument(what + " has an empty, . or .. segment: " + path);
        }
    }
}

std::string base_name(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string guess_mime_type(const std::string& path) {
    static const std::map<std::string, std::string, std::less<>> s_types = {
        {"md", "text/markdown"},      {"txt", "text/plain"},       {"py", "text/x-python"},
        {"sh", "text/x-shellscript"}, {"js", "text/javascript"},   {"json", "application/json"},
        {"yaml", "application/yaml"}, {"yml", "application/yaml"}, {"xml", "application/xml"},
        {"html", "text/html"},        {"css", "text/css"},         {"csv", "text/csv"},
        {"png", "image/png"},         {"jpg", "image/jpeg"},       {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},         {"svg", "image/svg+xml"},    {"pdf", "application/pdf"}};
    const auto dot = path.rfind('.');
    if (dot != std::string::npos && path.find('/', dot) == std::string::npos) {
        const auto found = s_types.find(path.substr(dot + 1));
        if (found != s_types.end()) {
            return found->second;
        }
    }
    return "application/octet-stream";
}

bool is_text_type(const std::string& mime_type) {
    const auto ends_with = [&](std::string_view suffix) {
        return mime_type.size() >= suffix.size() &&
               mime_type.compare(mime_type.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return mime_type.rfind("text/", 0) == 0 || mime_type == "application/json" ||
           mime_type == "application/xml" || mime_type == "application/yaml" || ends_with("+json") ||
           ends_with("+xml");
}

/// A page of @p total items from the cursor in @p params; throws std::invalid_argument for a cursor
/// this server did not issue.
struct Page {
    std::size_t begin{0};
    std::size_t end{0};
    std::optional<std::string> next;
};

Page page_of(const nlohmann::json& params, std::size_t total, std::size_t page_size) {
    std::size_t begin = 0;
    const auto cursor = params.find("cursor");
    if (cursor != params.end() && !cursor->is_null()) {
        if (!cursor->is_string()) {
            throw std::invalid_argument("Invalid pagination cursor");
        }
        const auto& text = cursor->get_ref<const std::string&>();
        const bool numeric =
            text.size() > mcp::detail::constants::skills::g_CURSOR_PREFIX.size() && text.size() < 11 &&
            text.compare(0, mcp::detail::constants::skills::g_CURSOR_PREFIX.size(),
                         mcp::detail::constants::skills::g_CURSOR_PREFIX) == 0 &&
            std::all_of(
                text.begin() +
                    static_cast<std::ptrdiff_t>(mcp::detail::constants::skills::g_CURSOR_PREFIX.size()),
                text.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!numeric) {
            throw std::invalid_argument("Invalid pagination cursor");
        }
        begin = std::stoul(text.substr(mcp::detail::constants::skills::g_CURSOR_PREFIX.size()));
        if (begin > total) {
            throw std::invalid_argument("Invalid pagination cursor");
        }
    }
    Page page;
    page.begin = begin;
    page.end = std::min(total, begin + page_size);
    if (page.end < total) {
        page.next =
            std::string(mcp::detail::constants::skills::g_CURSOR_PREFIX) + std::to_string(page.end);
    }
    return page;
}

}  // namespace

struct SkillsExtension::State {
    SkillsOptions options;
    std::vector<Skill> entries;
    /// Directory URI to its direct children, sorted by URI.
    std::map<std::string, std::vector<Resource>> directories;
    std::set<std::string> file_uris;
};

SkillsExtension::SkillsExtension(Server& server, SkillsOptions options)
    : server_(server), state_(std::make_shared<State>()) {
    if (options.page_size == 0) {
        throw std::invalid_argument("SkillsOptions::page_size must be at least 1");
    }
    if (!server.capabilities().resources) {
        throw std::invalid_argument(
            "The Skills extension needs the resources capability; declare it in the server's "
            "capabilities");
    }
    state_->options = options;
    server.declare_extension(std::string(mcp::constants::skills::g_EXTENSION_ID),
                             server_settings(options.directory_read));

    auto state = state_;
    server.add_extension_method(
        "skills/list", [state](Context&, const nlohmann::json& params) -> Task<nlohmann::json> {
            const auto page = page_of(params, state->entries.size(), state->options.page_size);
            ListSkillsResult result;
            result.skills.assign(state->entries.begin() + static_cast<std::ptrdiff_t>(page.begin),
                                 state->entries.begin() + static_cast<std::ptrdiff_t>(page.end));
            result.nextCursor = page.next;
            result.ttlMs = state->options.ttl_ms;
            result.cacheScope = state->options.cache_scope;
            co_return nlohmann::json(result);
        });
    server.add_extension_method(
        "skills/get", [state](Context&, const nlohmann::json& params) -> Task<nlohmann::json> {
            const auto uri = params.find("uri");
            if (uri == params.end() || !uri->is_string()) {
                throw std::invalid_argument("skills/get needs a string uri");
            }
            const auto& wanted = uri->get_ref<const std::string&>();
            const auto found = std::find_if(state->entries.begin(), state->entries.end(),
                                            [&](const Skill& skill) { return skill.uri == wanted; });
            if (found == state->entries.end()) {
                throw std::invalid_argument("No skill is served at " + wanted);
            }
            GetSkillResult result;
            result.skill = *found;
            result.ttlMs = state->options.ttl_ms;
            result.cacheScope = state->options.cache_scope;
            co_return nlohmann::json(result);
        });
    if (options.directory_read) {
        server.add_extension_method(
            "resources/directory/read",
            [state](Context&, const nlohmann::json& params) -> Task<nlohmann::json> {
                const auto uri = params.find("uri");
                if (uri == params.end() || !uri->is_string()) {
                    throw std::invalid_argument("resources/directory/read needs a string uri");
                }
                const auto& wanted = uri->get_ref<const std::string&>();
                const auto found = state->directories.find(wanted);
                if (found == state->directories.end()) {
                    throw std::invalid_argument(state->file_uris.contains(wanted)
                                                    ? wanted + " is not a directory resource"
                                                    : "No directory resource is served at " + wanted);
                }
                const auto& children = found->second;
                const auto page = page_of(params, children.size(), state->options.page_size);
                ReadResourceDirectoryResult result;
                result.resources.assign(children.begin() + static_cast<std::ptrdiff_t>(page.begin),
                                        children.begin() + static_cast<std::ptrdiff_t>(page.end));
                result.nextCursor = page.next;
                result.ttlMs = state->options.ttl_ms;
                result.cacheScope = state->options.cache_scope;
                co_return nlohmann::json(result);
            });
    }
}

void SkillsExtension::add_skill(SkillDefinition skill) {
    require_plain_path(skill.path, "The skill path");
    if (!skill.frontmatter.is_object() || !skill.frontmatter.contains("name") ||
        !skill.frontmatter.at("name").is_string() || !skill.frontmatter.contains("description") ||
        !skill.frontmatter.at("description").is_string()) {
        throw std::invalid_argument("The frontmatter of " + skill.path +
                                    " needs a string name and description");
    }

    const std::string root = std::string(mcp::constants::skills::g_URI_PREFIX) + skill.path;
    Skill entry;
    entry.uri = root + "/" + std::string(mcp::constants::skills::g_SKILL_FILE_NAME);
    entry.frontmatter = skill.frontmatter;
    entry.meta = skill.meta;

    std::set<std::string> paths;
    for (const auto& file : skill.files) {
        require_plain_path(file.path, "A file path of " + skill.path);
        if (!paths.insert(file.path).second) {
            throw std::invalid_argument("Skill " + skill.path + " lists " + file.path + " twice");
        }
        if (file.generate) {
            entry.dynamic = true;
        }
    }
    if (!paths.contains(std::string(mcp::constants::skills::g_SKILL_FILE_NAME))) {
        throw std::invalid_argument("Skill " + skill.path + " has no SKILL.md file");
    }
    if (!entry.dynamic) {
        for (const auto& file : skill.files) {
            entry.resources.push_back({root + "/" + file.path, sha256_digest(file.content),
                                       static_cast<std::int64_t>(file.content.size())});
        }
    }
    if (const auto problems = validate_skill(entry); !problems.empty()) {
        throw std::invalid_argument("Skill " + skill.path + ": " + problems.front());
    }
    for (const auto& file : skill.files) {
        if (state_->file_uris.contains(root + "/" + file.path)) {
            throw std::invalid_argument("A skill is served at " + root + " already");
        }
    }

    // Everything that can fail has run; the registrations below mutate the server.
    std::map<std::string, std::vector<Resource>> new_directories;
    new_directories.try_emplace(root);
    for (const auto& file : skill.files) {
        const auto mime_type = file.mime_type ? *file.mime_type : guess_mime_type(file.path);
        const std::string uri = root + "/" + file.path;

        Resource resource;
        resource.uri = uri;
        const bool is_skill_file = file.path == mcp::constants::skills::g_SKILL_FILE_NAME;
        resource.name =
            is_skill_file ? skill.frontmatter.at("name").get<std::string>() : base_name(file.path);
        if (is_skill_file) {
            resource.description = skill.frontmatter.at("description").get<std::string>();
        }
        resource.mimeType = mime_type;
        if (!file.generate) {
            resource.size = static_cast<std::int64_t>(file.content.size());
        }

        const auto segments = split_path(file.path);
        std::string directory = root;
        for (std::size_t index = 0; index + 1 < segments.size(); ++index) {
            const auto parent = directory;
            directory += "/" + segments[index];
            if (new_directories.try_emplace(directory).second) {
                Resource child;
                child.uri = directory;
                child.name = segments[index];
                child.mimeType = std::string(mcp::constants::skills::g_DIRECTORY_MIME_TYPE);
                new_directories[parent].push_back(std::move(child));
            }
        }
        new_directories[directory].push_back(resource);

        server_.add_resource<ReadResourceRequestParams, nlohmann::json>(
            resource, [uri, mime_type, content = file.content,
                       generate = file.generate](const ReadResourceRequestParams&) {
                const std::string bytes = generate ? generate() : content;
                ReadResourceResult result;
                if (is_text_type(mime_type)) {
                    result.contents.emplace_back(
                        TextResourceContents{uri, bytes, mime_type, std::nullopt});
                } else {
                    result.contents.emplace_back(BlobResourceContents{
                        uri,
                        auth::detail::base64_encode(
                            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()),
                        mime_type, std::nullopt});
                }
                return nlohmann::json(result);
            });
        state_->file_uris.insert(uri);
    }
    for (auto& [directory, children] : new_directories) {
        std::sort(children.begin(), children.end(),
                  [](const Resource& left, const Resource& right) { return left.uri < right.uri; });
        state_->directories[directory] = std::move(children);
    }
    state_->entries.push_back(std::move(entry));
}

}  // namespace mcp::skills
