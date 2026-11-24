#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/server/apps.hpp>

#include <stdexcept>
#include <utility>

namespace mcp::apps {

void add_view_resource(Server& server, const ViewResource& view) {
    if (!is_view_uri(view.uri)) {
        throw std::invalid_argument("A view resource URI must start with ui://: " + view.uri);
    }
    if (view.html.empty()) {
        throw std::invalid_argument("A view resource needs HTML content: " + view.uri);
    }

    Resource resource;
    resource.uri = view.uri;
    resource.name = view.name;
    resource.title = view.title;
    resource.description = view.description;
    resource.mimeType = std::string(mcp::constants::apps::g_RESOURCE_MIME_TYPE);
    if (view.meta && view.list_meta) {
        resource.meta = make_meta(*view.meta);
    }

    TextResourceContents contents;
    contents.uri = view.uri;
    contents.text = view.html;
    contents.mimeType = std::string(mcp::constants::apps::g_RESOURCE_MIME_TYPE);
    if (view.meta) {
        contents.meta = make_meta(*view.meta);
    }
    ReadResourceResult read;
    read.contents.emplace_back(std::move(contents));
    const nlohmann::json result = read;

    server.add_resource<ReadResourceRequestParams, nlohmann::json>(
        resource, [result](const ReadResourceRequestParams&) {
            // Each read answers with its own copy.
            return result;  // NOLINT(performance-no-automatic-move)
        });
}

Tool link_tool_to_view(Tool tool, const ToolMeta& meta) {
    if (meta.resourceUri && !is_view_uri(*meta.resourceUri)) {
        throw std::invalid_argument("A tool's view URI must start with ui://: " + *meta.resourceUri);
    }
    if (!tool.meta || !tool.meta->is_object()) {
        tool.meta = nlohmann::json::object();
    }
    (*tool.meta)["ui"] = meta;
    return tool;
}

void add_view_tool(Server& server, Tool tool, const ToolMeta& meta,
                   std::function<nlohmann::json(const nlohmann::json&)> handler) {
    server.add_raw_tool(link_tool_to_view(std::move(tool), meta), std::move(handler));
}

void add_view_tool(Server& server, Tool tool, const ToolMeta& meta, TypeErasedHandler handler) {
    server.add_raw_tool(link_tool_to_view(std::move(tool), meta), std::move(handler));
}

}  // namespace mcp::apps
