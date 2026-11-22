#include <mcp/client/apps.hpp>
#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>

#include "../detail/text/encoding.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace mcp::apps {

Task<View> read_view(Client& client, std::string uri, std::optional<Resource> listed) {
    if (!is_view_uri(uri)) {
        throw std::invalid_argument("A view URI must start with ui://: " + uri);
    }
    const auto result = co_await client.read_resource(uri);

    for (const auto& content : result.contents) {
        View view;
        std::optional<nlohmann::json> content_meta;
        std::optional<std::string> mime_type;
        std::visit(
            [&](const auto& item) {
                view.uri = item.uri;
                mime_type = item.mimeType;
                content_meta = item.meta;
            },
            content);
        if (view.uri != uri) {
            continue;
        }
        if (!mime_type || *mime_type != mcp::constants::apps::g_RESOURCE_MIME_TYPE) {
            throw std::runtime_error("View " + uri + " is not " +
                                     std::string(mcp::constants::apps::g_RESOURCE_MIME_TYPE));
        }
        view.mimeType = *mime_type;
        if (const auto* text = std::get_if<TextResourceContents>(&content)) {
            view.html = text->text;
        } else {
            auto decoded = mcp::detail::base64_decode(std::get<BlobResourceContents>(content).blob);
            if (!decoded) {
                throw std::runtime_error("View " + uri + " carries a blob that is not valid base64");
            }
            view.html = std::move(*decoded);
        }
        view.meta = resource_meta(content_meta);
        if (!view.meta && listed) {
            view.meta = resource_meta(listed->meta);
        }
        co_return view;
    }
    throw std::runtime_error("Reading " + uri + " returned no content for that URI");
}

Task<View> read_tool_view(Client& client, Tool tool, std::optional<Resource> listed) {
    auto uri = tool_view_uri(tool);
    if (!uri) {
        throw std::invalid_argument("Tool " + tool.name + " links no view");
    }
    co_return co_await read_view(client, std::move(*uri), std::move(listed));
}

}  // namespace mcp::apps
