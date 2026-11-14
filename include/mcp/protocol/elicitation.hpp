#pragma once

#include <mcp/protocol/base.hpp>

#include <stdexcept>
#include <string>

namespace mcp {

/**
 * @brief Definition of an elicitation form schema.
 */
struct PrimitiveSchemaDefinition {
    std::string type;                        ///< JSON Schema primitive type name.
    std::optional<std::string> title;        ///< Optional human-readable field title.
    std::optional<std::string> description;  ///< Optional human-readable field description.
};

inline void to_json(nlohmann::json& j, const PrimitiveSchemaDefinition& def) {
    j = nlohmann::json{{"type", def.type}};
    if (def.title) {
        j["title"] = *def.title;
    }
    if (def.description) {
        j["description"] = *def.description;
    }
}
inline void from_json(const nlohmann::json& j, PrimitiveSchemaDefinition& def) {
    j.at("type").get_to(def.type);
    if (detail::has_json_value(j, "title")) {
        def.title = j.at("title").get<std::string>();
    }
    if (detail::has_json_value(j, "description")) {
        def.description = j.at("description").get<std::string>();
    }
}

/// @brief Schema alias for string-valued form fields.
using StringSchema = PrimitiveSchemaDefinition;
/// @brief Schema alias for numeric form fields.
using NumberSchema = PrimitiveSchemaDefinition;
/// @brief Schema alias for boolean form fields.
using BooleanSchema = PrimitiveSchemaDefinition;

/**
 * @brief Primitive schema with an explicit set of allowed string values.
 */
struct EnumSchema : public PrimitiveSchemaDefinition {
    std::vector<std::string> enumValues;  ///< Allowed values for the field.
};
inline void to_json(nlohmann::json& j, const EnumSchema& e) {
    to_json(j, static_cast<const PrimitiveSchemaDefinition&>(e));
    j["enum"] = e.enumValues;
}
inline void from_json(const nlohmann::json& j, EnumSchema& e) {
    from_json(j, static_cast<PrimitiveSchemaDefinition&>(e));
    if (detail::has_json_value(j, "enum")) {
        j.at("enum").get_to(e.enumValues);
    }
}

/**
 * @brief Parameters for a URL-mode elicitation request.
 */
struct ElicitRequestURLParams {
    std::string mode = "url";            ///< The elicitation mode (always "url").
    std::string message;                 ///< The message to present to the user.
    std::string url;                     ///< The URL that the user should navigate to.
    std::string elicitationId;           ///< Unique ID for this elicitation.
    std::optional<TaskMetadata> task;    ///< Optional task augmentation metadata.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitRequestURLParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode},
                              {"message", params.message},
                              {"url", params.url},
                              {"elicitationId", params.elicitationId}};
    if (params.task) {
        json_obj["task"] = *params.task;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestURLParams& params) {
    json_obj.at("mode").get_to(params.mode);
    json_obj.at("message").get_to(params.message);
    json_obj.at("url").get_to(params.url);
    json_obj.at("elicitationId").get_to(params.elicitationId);
    if (json_obj.contains("task")) {
        params.task = json_obj.at("task").get<TaskMetadata>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for a form-mode elicitation request.
 */
struct ElicitRequestFormParams {
    std::string mode = "form";           ///< The elicitation mode (always "form").
    std::string message;                 ///< The message describing what info is requested.
    nlohmann::json requestedSchema;      ///< Restricted JSON Schema for the form.
    std::optional<TaskMetadata> task;    ///< Optional task augmentation metadata.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitRequestFormParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode},
                              {"message", params.message},
                              {"requestedSchema", params.requestedSchema}};
    if (params.task) {
        json_obj["task"] = *params.task;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestFormParams& params) {
    if (detail::has_json_value(json_obj, "mode")) {
        json_obj.at("mode").get_to(params.mode);
    }
    json_obj.at("message").get_to(params.message);
    json_obj.at("requestedSchema").get_to(params.requestedSchema);
    if (json_obj.contains("task")) {
        params.task = json_obj.at("task").get<TaskMetadata>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for an elicitation request — either URL or form mode.
 */
using ElicitRequestParams = std::variant<ElicitRequestURLParams, ElicitRequestFormParams>;

inline void to_json(nlohmann::json& json_obj, const ElicitRequestParams& params) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, params);
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestParams& params) {
    // A form request may leave its mode out.
    const auto mode = detail::has_json_value(json_obj, "mode") ? json_obj.at("mode").get<std::string>()
                                                               : std::string("form");
    if (mode == "url") {
        params = json_obj.get<ElicitRequestURLParams>();
    } else if (mode == "form") {
        params = json_obj.get<ElicitRequestFormParams>();
    } else {
        throw std::invalid_argument("Unknown elicitation mode: " + mode);
    }
}

/**
 * @brief A request from the server to elicit additional information from the user.
 */
struct ElicitRequest {
    std::string method = "elicitation/create";  ///< The method name.
    ElicitRequestParams params;                 ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ElicitRequest, method, params)

/**
 * @brief The user action in response to an elicitation.
 */
enum class ElicitAction : std::uint8_t {
    eAccept,   ///< User submitted the form/confirmed the action.
    eDecline,  ///< User explicitly declined the action.
    eCancel    ///< User dismissed without making an explicit choice.
};

inline void to_json(nlohmann::json& json_obj, const ElicitAction& action) {
    switch (action) {
        case ElicitAction::eAccept:
            json_obj = "accept";
            return;
        case ElicitAction::eDecline:
            json_obj = "decline";
            return;
        case ElicitAction::eCancel:
            json_obj = "cancel";
            return;
    }
    throw std::invalid_argument("Unknown elicitation action");
}

/// Not NLOHMANN_JSON_SERIALIZE_ENUM, which gives a value it does not know the first entry: that
/// would turn a reply nobody understood into a person's consent.
inline void from_json(const nlohmann::json& json_obj, ElicitAction& action) {
    const auto text = json_obj.get<std::string>();
    if (text == "accept") {
        action = ElicitAction::eAccept;
    } else if (text == "decline") {
        action = ElicitAction::eDecline;
    } else if (text == "cancel") {
        action = ElicitAction::eCancel;
    } else {
        throw std::invalid_argument("Unknown elicitation action");
    }
}

/**
 * @brief The result returned by the client for an elicitation/create request.
 */
struct ElicitResult {
    ElicitAction action;                    ///< The user action.
    std::optional<nlohmann::json> content;  ///< Submitted form data (only when action is accept).
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitResult& result) {
    json_obj = nlohmann::json{{"action", result.action}};
    if (result.content) {
        json_obj["content"] = *result.content;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitResult& result) {
    json_obj.at("action").get_to(result.action);
    if (json_obj.contains("content")) {
        result.content = json_obj.at("content").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp
