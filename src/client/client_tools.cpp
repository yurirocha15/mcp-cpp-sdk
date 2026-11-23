/// @file client_tools.cpp
/// @brief Client::Impl's listed tools: the output schemas it checks tool results against.

#include "client_impl.hpp"

namespace mcp {

OutputCheck Client::Impl::output_check_for(std::string_view name) const {
    if (!options.validate_tool_schemas) {
        return {};
    }
    std::lock_guard lock(listed_tools_mutex);
    const auto found = listed_tools.find(name);
    if (found == listed_tools.end()) {
        return {};
    }
    // A pattern this client cannot run says nothing about the server, so such a result goes
    // unchecked. Any other problem means the server declared a schema no client can use.
    const auto& problem = found->second.output_problem;
    if (problem && problem->category() != SchemaError::Category::eUnsupportedPattern) {
        return {std::nullopt, problem};
    }
    return {found->second.output_schema, std::nullopt};
}

std::optional<SchemaError> Client::Impl::remember_tool(const Tool& tool) {
    ListedTool listed{
        tool.outputSchema ? std::optional<std::string>(tool.outputSchema->dump()) : std::nullopt,
        std::nullopt, std::nullopt};
    {
        std::lock_guard lock(listed_tools_mutex);
        const auto found = listed_tools.find(tool.name);
        if (found != listed_tools.end() && found->second.output_text == listed.output_text) {
            return found->second.output_problem;
        }
    }
    try {
        if (tool.outputSchema) {
            listed.output_schema = CompiledSchema::compile(*tool.outputSchema);
        }
    } catch (const SchemaError& error) {
        listed.output_problem =
            SchemaError(error.category(), std::string("outputSchema: ") + error.what());
    }
    auto problem = listed.output_problem;
    std::lock_guard lock(listed_tools_mutex);
    listed_tools.insert_or_assign(tool.name, std::move(listed));
    return problem;
}

void Client::Impl::accept_listed_tools(const ListToolsResult& result) {
    if (!options.validate_tool_schemas) {
        return;
    }
    for (const auto& tool : result.tools) {
        const auto problem = remember_tool(tool);
        if (problem && options.on_tool_schema_error) {
            try {
                options.on_tool_schema_error(tool, *problem);
            } catch (...) {
                // Documented: the callback reports; a failure of its own has no listener.
            }
        }
    }
}

Task<ListToolsResult> Client::Impl::accepted_tools(std::shared_ptr<Impl> state,
                                                   Task<ListToolsResult> request) {
    auto result = co_await std::move(request);
    state->accept_listed_tools(result);
    co_return result;
}

}  // namespace mcp
