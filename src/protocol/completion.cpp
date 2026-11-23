#include <mcp/protocol/completion.hpp>

#include <map>
#include <string>
#include <variant>

namespace mcp {

void to_json(nlohmann::json& json_obj, const PaginatedRequestParams& params) {
    json_obj = nlohmann::json::object();
    if (params.cursor) {
        json_obj["cursor"] = *params.cursor;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, PaginatedRequestParams& params) {
    params = PaginatedRequestParams{};
    if (detail::has_json_value(json_obj, "cursor")) {
        params.cursor = json_obj.at("cursor").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const PromptReference& value) {
    json_obj["type"] = value.type;
    json_obj["name"] = value.name;
}

void from_json(const nlohmann::json& json_obj, PromptReference& value) {
    json_obj.at("type").get_to(value.type);
    json_obj.at("name").get_to(value.name);
}

void to_json(nlohmann::json& json_obj, const ResourceTemplateReference& value) {
    json_obj["type"] = value.type;
    json_obj["uri"] = value.uri;
}

void from_json(const nlohmann::json& json_obj, ResourceTemplateReference& value) {
    json_obj.at("type").get_to(value.type);
    json_obj.at("uri").get_to(value.uri);
}

void to_json(nlohmann::json& json_obj, const CompleteReference& ref) {
    std::visit([&json_obj](auto&& arg) { to_json(json_obj, arg); }, ref);
}

void from_json(const nlohmann::json& json_obj, CompleteReference& ref) {
    if (json_obj.at("type") == "ref/prompt") {
        ref = json_obj.get<PromptReference>();
    } else {
        ref = json_obj.get<ResourceTemplateReference>();
    }
}

void to_json(nlohmann::json& json_obj, const CompleteParamsArgument& value) {
    json_obj["name"] = value.name;
    json_obj["value"] = value.value;
}

void from_json(const nlohmann::json& json_obj, CompleteParamsArgument& value) {
    json_obj.at("name").get_to(value.name);
    json_obj.at("value").get_to(value.value);
}

void to_json(nlohmann::json& json_obj, const CompleteContext& context) {
    json_obj = nlohmann::json::object();
    if (context.arguments) {
        json_obj["arguments"] = *context.arguments;
    }
}

void from_json(const nlohmann::json& json_obj, CompleteContext& context) {
    context = CompleteContext{};
    if (detail::has_json_value(json_obj, "arguments")) {
        context.arguments = json_obj.at("arguments").get<std::map<std::string, std::string>>();
    }
}

void to_json(nlohmann::json& json_obj, const CompleteParams& params) {
    json_obj = nlohmann::json{{"ref", params.ref}, {"argument", params.argument}};
    if (params.context) {
        json_obj["context"] = *params.context;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, CompleteParams& params) {
    params = CompleteParams{};
    json_obj.at("ref").get_to(params.ref);
    json_obj.at("argument").get_to(params.argument);
    if (json_obj.contains("context") && !json_obj.at("context").is_null()) {
        params.context = json_obj.at("context").get<CompleteContext>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const CompletionResultDetails& details) {
    json_obj = nlohmann::json{{"values", details.values}};
    if (details.total) {
        json_obj["total"] = *details.total;
    }
    if (details.hasMore) {
        json_obj["hasMore"] = *details.hasMore;
    }
}

void from_json(const nlohmann::json& json_obj, CompletionResultDetails& details) {
    details = CompletionResultDetails{};
    json_obj.at("values").get_to(details.values);
    if (detail::has_json_value(json_obj, "total")) {
        details.total = json_obj.at("total").get<int64_t>();
    }
    if (detail::has_json_value(json_obj, "hasMore")) {
        details.hasMore = json_obj.at("hasMore").get<bool>();
    }
}

void to_json(nlohmann::json& json_obj, const CompleteResult& result) {
    json_obj = nlohmann::json{{"completion", result.completion}};
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

void from_json(const nlohmann::json& json_obj, CompleteResult& result) {
    result = CompleteResult{};
    json_obj.at("completion").get_to(result.completion);
    if (detail::has_json_value(json_obj, "_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp
