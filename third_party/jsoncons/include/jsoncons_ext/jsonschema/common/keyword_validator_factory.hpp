// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_EXT_JSONSCHEMA_COMMON_KEYWORD_VALIDATOR_FACTORY_HPP
#define MCP_DETAIL_JSONCONS_EXT_JSONSCHEMA_COMMON_KEYWORD_VALIDATOR_FACTORY_HPP

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <jsoncons/nonstd/compiler_support.hpp>
#include <jsoncons/utility/string_utils.hpp>

#include <jsoncons_ext/jsonschema/common/compilation_context.hpp>
#include <jsoncons_ext/jsonschema/common/schema_validator.hpp>
#include <jsoncons_ext/jsonschema/evaluation_options.hpp>
#include <jsoncons_ext/jsonschema/common/schema_validator_factory_base.hpp>

namespace mcp_detail_jsoncons {
namespace jsonschema {

    // Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): a size keyword's value. A double
    // beyond std::size_t saturates instead of converting with undefined behavior; the metaschema
    // has already checked that the value is a non-negative integer.
    template <typename Json>
    std::size_t keyword_size_value(const Json& value)
    {
        if (value.is_double())
        {
            const double number = value.template as<double>();
            const double largest = static_cast<double>((std::numeric_limits<std::size_t>::max)());
            if (!(number > 0))
            {
                return 0;
            }
            return number >= largest ? (std::numeric_limits<std::size_t>::max)()
                                     : static_cast<std::size_t>(number);
        }
        return value.template as<std::size_t>();
    }

    template <typename Json>
    using resolve_uri_type = std::function<Json(const mcp_detail_jsoncons::uri & /*id*/)>;

    template <typename Json>
    class keyword_validator_factory
    {
    public:
        using schema_store_type = std::map<mcp_detail_jsoncons::uri, schema_validator<Json>*>;
        using validator_factory_factory_type = std::function<std::unique_ptr<keyword_validator_factory<Json>>(const Json&,
            const evaluation_options&,schema_store_type*,const std::vector<resolve_uri_type<Json>>&,
            const std::unordered_map<std::string,bool>&)>;
        using keyword_validator_ptr_type = typename std::unique_ptr<keyword_validator<Json>>;
        using schema_validator_ptr_type = typename std::unique_ptr<schema_validator<Json>>;
        using ref_validator_type = ref_validator<Json>;
        using ref_type = ref<Json>;
        using anchor_uri_map_type = std::unordered_map<std::string,uri_wrapper>;

    private:
        schema_validator_factory_base<Json>* factory_;
    public:

        keyword_validator_factory(schema_validator_factory_base<Json>* factory)
            : factory_(factory)
        {
            MCP_DETAIL_JSONCONS_ASSERT(factory_ != nullptr);
        }
        
        // keyword validator factories
        
        // No dependence on data members

        std::unique_ptr<properties_validator<Json>> make_properties_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            std::string keyword = "properties";
            uri schema_location = context.get_base_uri();

            std::map<std::string, schema_validator_ptr_type,transparent_string_less<std::string>> properties;

            for (const auto& prop : sch.object_range())
            {
                mcp_detail_jsoncons::string_view sub_keys[] = {keyword, prop.key()};
                properties.emplace(std::make_pair(prop.key(), 
                    factory_->make_cross_draft_schema_validator(context, prop.value(), sub_keys, anchor_dict)));
            }

            return mcp_detail_jsoncons::make_unique<properties_validator<Json>>(parent, std::move(schema_location),
                context.get_custom_message(keyword), std::move(properties));
        }

#if defined(MCP_DETAIL_JSONCONS_HAS_STD_REGEX)
                
        std::unique_ptr<pattern_properties_validator<Json>> make_pattern_properties_validator( const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            std::string keyword = "patternProperties";
            uri schema_location = context.get_base_uri();
            std::string custom_message = context.get_custom_message(keyword);

            std::vector<std::pair<::mcp::detail::Regex, schema_validator_ptr_type>> pattern_properties;
            
            for (const auto& prop : sch.object_range())
            {
                mcp_detail_jsoncons::string_view sub_keys[] = {keyword};
                pattern_properties.emplace_back(
                    std::make_pair(
                        ::mcp::detail::Regex(prop.key()),
                        factory_->make_cross_draft_schema_validator(context, prop.value(), sub_keys, anchor_dict)));
                
            }

            return mcp_detail_jsoncons::make_unique<pattern_properties_validator<Json>>(parent, std::move(schema_location),
                custom_message,
                std::move(pattern_properties));
        }
#endif       

        std::unique_ptr<max_length_validator<Json>> make_max_length_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("maxLength");
            if (!sch.is_number())
            {
                const std::string message("maxLength must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = keyword_size_value(sch);
            return mcp_detail_jsoncons::make_unique<max_length_validator<Json>>(parent, schema_location, context.get_custom_message("maxLength"), 
                value);
        }

        std::unique_ptr<min_length_validator<Json>> make_min_length_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("minLength");
            if (!sch.is_number())
            {
                const std::string message("minLength must be an integer value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = keyword_size_value(sch);
            return mcp_detail_jsoncons::make_unique<min_length_validator<Json>>(parent, schema_location, context.get_custom_message("minLength"), value);
        }

        std::unique_ptr<not_validator<Json>> make_not_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.make_schema_location("not");
            mcp_detail_jsoncons::string_view not_key[] = { "not" };
            return mcp_detail_jsoncons::make_unique<not_validator<Json>>(parent, schema_location, context.get_custom_message("not"), 
                factory_->make_cross_draft_schema_validator(context, sch, not_key, anchor_dict));
        }

        std::unique_ptr<const_validator<Json>> make_const_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("const");
            return mcp_detail_jsoncons::make_unique<const_validator<Json>>(parent, schema_location, 
                context.get_custom_message("const"), sch);
        }

        std::unique_ptr<enum_validator<Json>> make_enum_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("enum");
            return mcp_detail_jsoncons::make_unique<enum_validator<Json>>(parent, schema_location, context.get_custom_message("enum"), sch);
        }

        std::unique_ptr<required_validator<Json>> make_required_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("required");
            return mcp_detail_jsoncons::make_unique<required_validator<Json>>(parent, schema_location, 
                context.get_custom_message("required"), sch.template as<std::vector<std::string>>());
        }

        std::unique_ptr<maximum_validator<Json>> make_maximum_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("maximum");
            if (!sch.is_number())
            {
                const std::string message("maximum must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            return mcp_detail_jsoncons::make_unique<maximum_validator<Json>>(parent, schema_location, 
                context.get_custom_message("maximum"), sch);
        }

        std::unique_ptr<exclusive_maximum_validator<Json>> make_exclusive_maximum_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("exclusiveMaximum");
            if (!sch.is_number())
            {
                const std::string message("exclusiveMaximum must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            return mcp_detail_jsoncons::make_unique<exclusive_maximum_validator<Json>>(parent, schema_location, 
                context.get_custom_message("exclusiveMaximum"), sch);
        }

        std::unique_ptr<keyword_validator<Json>> make_minimum_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("minimum");
                
            if (!sch.is_number())
            {
                const std::string message("minimum must be an integer");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            return mcp_detail_jsoncons::make_unique<minimum_validator<Json>>(parent, schema_location, 
                context.get_custom_message("minimum"), sch);
        }

        std::unique_ptr<exclusive_minimum_validator<Json>> make_exclusive_minimum_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("exclusiveMinimum");
            if (!sch.is_number())
            {
                const std::string message("exclusiveMinimum must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            return mcp_detail_jsoncons::make_unique<exclusive_minimum_validator<Json>>(parent, schema_location, 
                context.get_custom_message("exclusiveMinimum"), sch);
        }

        std::unique_ptr<multiple_of_validator<Json>> make_multiple_of_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("multipleOf");
            if (!sch.is_number())
            {
                const std::string message("multipleOf must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = sch.template as<double>();
            return mcp_detail_jsoncons::make_unique<multiple_of_validator<Json>>(parent, schema_location, 
                context.get_custom_message("multipleOf"), value);
        }


        std::unique_ptr<type_validator<Json>> make_type_validator(const compilation_context<Json>& context,
            const Json& sch, const Json& parent)
        {
            std::string keyword = "type";
            uri schema_location = context.get_base_uri();
            
            std::vector<json_schema_type> expected_types;

            switch (sch.type()) 
            { 
                case json_type::string: 
                {
                    auto type = sch.as_string_view();
                    if (type == "null")
                    {
                        expected_types.push_back(json_schema_type::null);
                    }
                    else if (type == "object")
                    {
                        expected_types.push_back(json_schema_type::object);
                    }
                    else if (type == "array")
                    {
                        expected_types.push_back(json_schema_type::array);
                    }
                    else if (type == "string")
                    {
                        expected_types.push_back(json_schema_type::string);
                    }
                    else if (type == "boolean")
                    {
                        expected_types.push_back(json_schema_type::boolean);
                    }
                    else if (type == "integer")
                    {
                        expected_types.push_back(json_schema_type::integer);
                    }
                    else if (type == "number")
                    {
                        expected_types.push_back(json_schema_type::number);
                    }
                    else
                    {
                        std::string message{schema_location.string()};
                        message.append(": Invalid type '");
                        message.append(type.data(), type.size());
                        message.append("'");
                        MCP_DETAIL_JSONCONS_THROW(schema_error(message));
                    }
                    break;
                } 

                case json_type::array: // "type": ["type1", "type2"]
                {
                    for (const auto& item : sch.array_range())
                    {
                        auto type = item.as_string_view();
                        if (type == "null")
                        {
                            expected_types.push_back(json_schema_type::null);
                        }
                        else if (type == "object")
                        {
                            expected_types.push_back(json_schema_type::object);
                        }
                        else if (type == "array")
                        {
                            expected_types.push_back(json_schema_type::array);
                        }
                        else if (type == "string")
                        {
                            expected_types.push_back(json_schema_type::string);
                        }
                        else if (type == "boolean")
                        {
                            expected_types.push_back(json_schema_type::boolean);
                        }
                        else if (type == "integer")
                        {
                            expected_types.push_back(json_schema_type::integer);
                        }
                        else if (type == "number")
                        {
                            expected_types.push_back(json_schema_type::number);
                        }
                        else
                        {
                            std::string message{schema_location.string()};
                            message.append(": Invalid type '");
                            message.append(type.data(), type.size());
                            message.append("'");
                            MCP_DETAIL_JSONCONS_THROW(schema_error(message));
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            
            return mcp_detail_jsoncons::make_unique<type_validator<Json>>(parent, std::move(schema_location), context.get_custom_message(keyword), 
                std::move(expected_types));
        }

        std::unique_ptr<content_encoding_validator<Json>> make_content_encoding_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            std::string keyword = "contentEncoding";
            uri schema_location = context.make_schema_location(keyword);
            if (!sch.is_string())
            {
                const std::string message("contentEncoding must be a string");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = sch.template as<std::string>();
            return mcp_detail_jsoncons::make_unique<content_encoding_validator<Json>>(parent, schema_location, 
                context.get_custom_message(keyword), value);
        }

        std::unique_ptr<content_media_type_validator<Json>> make_content_media_type_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            std::string keyword = "contentMediaType";
            uri schema_location = context.make_schema_location(keyword);
            if (!sch.is_string())
            {
                const std::string message("contentMediaType must be a string");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            
            std::string content_encoding;
            auto it = parent.find("contentEncoding");
            if (it != parent.object_range().end())
            {
                if (!(*it).value().is_string())
                {
                    const std::string message("contentEncoding must be a string");
                    MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
                }

                content_encoding = (*it).value().as_string();
            }
            
            auto value = sch.template as<std::string>();
            return mcp_detail_jsoncons::make_unique<content_media_type_validator<Json>>(parent, schema_location, 
                context.get_custom_message(keyword), value, content_encoding);
        }

        std::unique_ptr<format_validator<Json>> make_format_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            auto schema_location = context.make_schema_location("format");

            std::string format = sch.template as<std::string>();

            std::string message_key;
            validate_format<Json> validate;
            if (format == "date-time")
            {
                message_key = "format.date-time";
                validate = rfc3339_date_time_check<Json>;
            }
            else if (format == "date") 
            {
                message_key = "format.date";
                validate = rfc3339_date_check<Json>;
            } 
            else if (format == "time") 
            {
                message_key = "format.time";
                validate = rfc3339_time_check<Json>;
            } 
            else if (format == "email") 
            {
                message_key = "format.email";
                validate = email_check<Json>;
            } 
            else if (format == "hostname") 
            {
                message_key = "format.hostname";
                validate = hostname_check<Json>;
            } 
            else if (format == "ipv4") 
            {
                message_key = "format.ipv4";
                validate = ipv4_check<Json>;
            } 
            else if (format == "ipv6") 
            {
                message_key = "format.ipv6";
                validate = ipv6_check<Json>;
            } 
            else if (format == "regex") 
            {
                message_key = "format.regex";
                validate = regex_check<Json>;
            } 
            else if (format == "json-pointer") 
            {
                message_key = "format.json-pointer";
                validate = jsonpointer_check<Json>;
            } 
            else if (format == "uri") 
            {
                message_key = "format.uri";
                validate = uri_check<Json>;
            } 
            else if (format == "uri-reference") 
            {
                message_key = "format.uri-reference";
                validate = uri_reference_check<Json>;
            } 
            else
            {
                // Not supported - ignore
                validate = nullptr;
            }       

            return mcp_detail_jsoncons::make_unique<format_validator<Json>>(parent, schema_location, context.get_custom_message(message_key), 
                validate);
        }

        std::unique_ptr<pattern_validator<Json>> make_pattern_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("pattern");
            auto pattern_string = sch.template as<std::string>();
            auto regex = ::mcp::detail::Regex(pattern_string);
            return mcp_detail_jsoncons::make_unique<pattern_validator<Json>>(parent, schema_location, context.get_custom_message("pattern"), 
                pattern_string, regex);
        }

        std::unique_ptr<max_items_validator<Json>> make_max_items_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            std::string keyword = "maxItems";
            uri schema_location = context.make_schema_location(keyword);

            if (!sch.is_number())
            {
                const std::string message("maxItems must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = keyword_size_value(sch);
            return mcp_detail_jsoncons::make_unique<max_items_validator<Json>>(parent, schema_location, context.get_custom_message(keyword), value);
        }

        std::unique_ptr<min_items_validator<Json>> make_min_items_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            std::string keyword = "minItems";
            uri schema_location = context.make_schema_location(keyword);

            if (!sch.is_number())
            {
                const std::string message("minItems must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = keyword_size_value(sch);
            return mcp_detail_jsoncons::make_unique<min_items_validator<Json>>(parent, schema_location, context.get_custom_message(keyword), value);
        }

        std::unique_ptr<max_properties_validator<Json>> make_max_properties_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            std::string keyword = "maxProperties";
            uri schema_location = context.make_schema_location(keyword);
            if (!sch.is_number())
            {
                const std::string message("maxProperties must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = keyword_size_value(sch);
            return mcp_detail_jsoncons::make_unique<max_properties_validator<Json>>(parent, schema_location, context.get_custom_message(keyword), value);
        }

        std::unique_ptr<min_properties_validator<Json>> make_min_properties_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            std::string keyword = "minProperties";
            uri schema_location = context.make_schema_location(keyword);
            if (!sch.is_number())
            {
                const std::string message("minProperties must be a number value");
                MCP_DETAIL_JSONCONS_THROW(schema_error(schema_location.string() + ": " + message));
            }
            auto value = keyword_size_value(sch);
            return mcp_detail_jsoncons::make_unique<min_properties_validator<Json>>(parent, schema_location, context.get_custom_message(keyword), value);
        }

        std::unique_ptr<contains_validator<Json>> make_contains_validator(const compilation_context<Json>& context,
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.make_schema_location("contains");

            mcp_detail_jsoncons::string_view sub_keys[] = { "contains" };

            std::unique_ptr<max_contains_keyword<Json>> max_contains;
            auto it = parent.find("maxContains");
            if (it != parent.object_range().end()) 
            {
                uri path = context.make_schema_location("maxContains");
                auto value = keyword_size_value((*it).value());
                max_contains = mcp_detail_jsoncons::make_unique<max_contains_keyword<Json>>(parent, path, context.get_custom_message("maxContains"), value);
            }
            else
            {
                uri path = context.make_schema_location("maxContains");
                max_contains = mcp_detail_jsoncons::make_unique<max_contains_keyword<Json>>(parent, path, context.get_custom_message("maxContains"), (std::numeric_limits<std::size_t>::max)());
            }

            std::unique_ptr<min_contains_keyword<Json>> min_contains;
            it = parent.find("minContains");
            if (it != parent.object_range().end()) 
            {
                uri path = context.make_schema_location("minContains");
                auto value = keyword_size_value((*it).value());
                min_contains = mcp_detail_jsoncons::make_unique<min_contains_keyword<Json>>(parent, path, context.get_custom_message("minContains"), value);
            }
            else
            {
                uri path = context.make_schema_location("minContains");
                min_contains = mcp_detail_jsoncons::make_unique<min_contains_keyword<Json>>(parent, path, context.get_custom_message("minContains"), 1);
            }

            return mcp_detail_jsoncons::make_unique<contains_validator<Json>>(parent, schema_location, context.get_custom_message("contains"), 
                factory_->make_cross_draft_schema_validator(context, sch, sub_keys, anchor_dict), std::move(max_contains), std::move(min_contains));
        }

        std::unique_ptr<unique_items_validator<Json>> make_unique_items_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent)
        {
            uri schema_location = context.make_schema_location("uniqueItems");
            bool are_unique = sch.template as<bool>();
            return mcp_detail_jsoncons::make_unique<unique_items_validator<Json>>(parent, schema_location, 
                context.get_custom_message("uniqueItems"), 
                are_unique);
        }

        std::unique_ptr<all_of_validator<Json>> make_all_of_validator(const compilation_context<Json>& context,
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.make_schema_location("allOf");
            std::vector<schema_validator_ptr_type> subschemas;

            std::size_t c = 0;
            for (const auto& subsch : sch.array_range())
            {
                auto temp_str = std::to_string(c++);
                mcp_detail_jsoncons::string_view sub_keys[] = { "allOf", temp_str };
                subschemas.emplace_back(factory_->make_cross_draft_schema_validator(context, subsch, sub_keys, anchor_dict));
            }
            return mcp_detail_jsoncons::make_unique<all_of_validator<Json>>(parent, std::move(schema_location), 
                context.get_custom_message("allOf"), std::move(subschemas));
        }

        std::unique_ptr<any_of_validator<Json>> make_any_of_validator(const compilation_context<Json>& context,
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.make_schema_location("anyOf");
            std::vector<schema_validator_ptr_type> subschemas;

            std::size_t c = 0;
            for (const auto& subsch : sch.array_range())
            {
                auto temp_str = std::to_string(c++);
                mcp_detail_jsoncons::string_view sub_keys[] = { "anyOf", temp_str };
                subschemas.emplace_back(factory_->make_cross_draft_schema_validator(context, subsch, sub_keys, anchor_dict));
            }
            return mcp_detail_jsoncons::make_unique<any_of_validator<Json>>(parent, std::move(schema_location), 
                context.get_custom_message("anyOf"), std::move(subschemas));
        }

        std::unique_ptr<one_of_validator<Json>> make_one_of_validator(const compilation_context<Json>& context,
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location{ context.make_schema_location("oneOf") };
            std::vector<schema_validator_ptr_type> subschemas;

            std::size_t c = 0;
            for (const auto& subsch : sch.array_range())
            {
                auto temp_str = std::to_string(c++);
                mcp_detail_jsoncons::string_view sub_keys[] = { "oneOf", temp_str };
                subschemas.emplace_back(factory_->make_cross_draft_schema_validator(context, subsch, sub_keys, anchor_dict));
            }
            return mcp_detail_jsoncons::make_unique<one_of_validator<Json>>(parent, std::move(schema_location), 
                context.get_custom_message("oneOf"), std::move(subschemas));
        }

        std::unique_ptr<dependencies_validator<Json>> make_dependencies_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.get_base_uri();
            std::map<std::string, keyword_validator_ptr_type,transparent_string_less<std::string>> dependent_required;
            std::map<std::string, schema_validator_ptr_type,transparent_string_less<std::string>> dependent_schemas;

            //std::cout << "dependencies" << "\n" << pretty_print(sch) << "\n";

            for (const auto& dep : sch.object_range())
            {
                switch (dep.value().type()) 
                {
                    case json_type::array:
                    {
                        auto location = context.make_schema_location("dependencies");
                        dependent_required.emplace(dep.key(), 
                            this->make_required_validator(compilation_context<Json>(std::vector<uri_wrapper>{{uri_wrapper{ location }}}),
                                dep.value(), sch));
                        break;
                    }
                    case json_type::boolean:
                    case json_type::object:
                    {
                        mcp_detail_jsoncons::string_view sub_keys[] = {"dependencies"};
                        dependent_schemas.emplace(dep.key(),
                            factory_->make_cross_draft_schema_validator(context, dep.value(), sub_keys, anchor_dict));
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
            }         

            return mcp_detail_jsoncons::make_unique<dependencies_validator<Json>>(parent, std::move(schema_location),
                std::move(dependent_required), std::move(dependent_schemas));
        }

        std::unique_ptr<property_names_validator<Json>> make_property_names_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.get_base_uri();
            schema_validator_ptr_type property_names_schema_validator;

            mcp_detail_jsoncons::string_view sub_keys[] = { "propertyNames"};
            property_names_schema_validator = factory_->make_cross_draft_schema_validator(context, sch, sub_keys, anchor_dict);

            return mcp_detail_jsoncons::make_unique<property_names_validator<Json>>(parent, std::move(schema_location), context.get_custom_message("propertyNames"),
                std::move(property_names_schema_validator));
        }

        // 201909 and later
                
        std::unique_ptr<dependent_required_validator<Json>> make_dependent_required_validator( 
            const compilation_context<Json>& context, const Json& sch, const Json& parent)
        {
            uri schema_location = context.get_base_uri();
            std::map<std::string, keyword_validator_ptr_type,transparent_string_less<std::string>> dependent_required;

            for (const auto& dep : sch.object_range())
            {
                switch (dep.value().type()) 
                {
                    case json_type::array:
                    {
                        auto location = context.make_schema_location("dependentRequired");
                        dependent_required.emplace(dep.key(), 
                            this->make_required_validator(compilation_context<Json>(std::vector<uri_wrapper>{{uri_wrapper{ location }}}),
                                dep.value(), sch));
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
            }

            return mcp_detail_jsoncons::make_unique<dependent_required_validator<Json>>(parent, std::move(schema_location),
                std::move(dependent_required));
        }

        std::unique_ptr<dependent_schemas_validator<Json>> make_dependent_schemas_validator( const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.get_base_uri();
            std::map<std::string, schema_validator_ptr_type,transparent_string_less<std::string>> dependent_schemas;

            for (const auto& dep : sch.object_range())
            {
                switch (dep.value().type()) 
                {
                    case json_type::boolean:
                    case json_type::object:
                    {
                        mcp_detail_jsoncons::string_view sub_keys[] = {"dependentSchemas"};
                        dependent_schemas.emplace(dep.key(),
                            factory_->make_cross_draft_schema_validator(context, dep.value(), sub_keys, anchor_dict));
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
            }
            

            return mcp_detail_jsoncons::make_unique<dependent_schemas_validator<Json>>(parent, std::move(schema_location), context.get_custom_message("dependentSchemas"),
                std::move(dependent_schemas));
        }

        std::unique_ptr<prefix_items_validator<Json>> make_prefix_items_validator_07(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            std::vector<schema_validator_ptr_type> prefix_item_validators;
            std::unique_ptr<items_keyword<Json>> items_val;

            uri schema_location{context.make_schema_location("items")};

            if (sch.type() == json_type::array) 
            {
                std::size_t c = 0;
                for (const auto& subsch : sch.array_range())
                {
                    auto temp_str = std::to_string(c++);
                    mcp_detail_jsoncons::string_view sub_keys[] = {"items", temp_str};

                    prefix_item_validators.emplace_back(factory_->make_cross_draft_schema_validator(context, subsch, sub_keys, anchor_dict));
                }

                auto it = parent.find("additionalItems");
                if (it != parent.object_range().end()) 
                {
                    uri items_location{context.make_schema_location("additionalItems")};
                    mcp_detail_jsoncons::string_view sub_keys[] = {"additionalItems"};
                    items_val = mcp_detail_jsoncons::make_unique<items_keyword<Json>>("additionalItems", parent, items_location, 
                        context.get_custom_message("additionalItems"),
                        factory_->make_cross_draft_schema_validator(context, (*it).value(), sub_keys, anchor_dict));
                }
            }

            return mcp_detail_jsoncons::make_unique<prefix_items_validator<Json>>("items", parent, schema_location, context.get_custom_message("items"),
                std::move(prefix_item_validators), std::move(items_val));
        }
        
        std::unique_ptr<items_validator<Json>> make_items_validator(const std::string& keyword_name,
            const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location{context.make_schema_location(keyword_name)};

            mcp_detail_jsoncons::string_view sub_keys[] = {keyword_name};

            return mcp_detail_jsoncons::make_unique<items_validator<Json>>(keyword_name, parent, schema_location,
                context.get_custom_message(keyword_name), 
                factory_->make_cross_draft_schema_validator(context, sch, sub_keys, anchor_dict));
        }

        std::unique_ptr<unevaluated_properties_validator<Json>> make_unevaluated_properties_validator(
            const compilation_context<Json>& context, const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            std::string keyword = "unevaluatedProperties";
            uri schema_location = context.get_base_uri();

            mcp_detail_jsoncons::string_view sub_keys[] = {keyword};

            return mcp_detail_jsoncons::make_unique<unevaluated_properties_validator<Json>>(parent, std::move(schema_location),
                context.get_custom_message(keyword),
                factory_->make_cross_draft_schema_validator(context, sch, sub_keys, anchor_dict));
        }

        std::unique_ptr<unevaluated_items_validator<Json>> make_unevaluated_items_validator(
            const compilation_context<Json>& context, const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            uri schema_location = context.get_base_uri();

            mcp_detail_jsoncons::string_view sub_keys[] = {"unevaluatedItems"};

            return mcp_detail_jsoncons::make_unique<unevaluated_items_validator<Json>>(parent, std::move(schema_location), context.get_custom_message("unevaluatedItems"),               
                factory_->make_cross_draft_schema_validator(context, sch, sub_keys, anchor_dict));
        }

        std::unique_ptr<additional_properties_validator<Json>> make_additional_properties_validator( 
            const compilation_context<Json>& context, const Json& sch, const Json& parent,
            std::unique_ptr<properties_validator<Json>>&& properties, std::unique_ptr<pattern_properties_validator<Json>>&& pattern_properties,
            anchor_uri_map_type& anchor_dict)
        {
            std::string keyword = "additionalProperties";
            uri schema_location = context.get_base_uri();

            std::vector<keyword_validator_ptr_type> validators;
            schema_validator_ptr_type additional_properties;

            mcp_detail_jsoncons::string_view sub_keys[] = {keyword};
            additional_properties = factory_->make_cross_draft_schema_validator(context, sch, sub_keys, anchor_dict);

            return mcp_detail_jsoncons::make_unique<additional_properties_validator<Json>>(parent, additional_properties->schema_location(),
                context.get_custom_message(keyword),
                std::move(properties), std::move(pattern_properties),
                std::move(additional_properties));
        }

        // Since 202012
        std::unique_ptr<prefix_items_validator<Json>> make_prefix_items_validator(const compilation_context<Json>& context, 
            const Json& sch, const Json& parent, anchor_uri_map_type& anchor_dict)
        {
            std::vector<schema_validator_ptr_type> prefix_item_validators;
            std::unique_ptr<items_keyword<Json>> items_val;

            uri schema_location{context.make_schema_location("prefixItems")};

            if (sch.type() == json_type::array) 
            {
                std::size_t c = 0;
                for (const auto& subsch : sch.array_range())
                {
                    auto temp_str = std::to_string(c++);
                    mcp_detail_jsoncons::string_view sub_keys[] = {"prefixItems", temp_str};

                    prefix_item_validators.emplace_back(factory_->make_cross_draft_schema_validator(context, subsch, sub_keys, anchor_dict));
                }

                auto it = parent.find("items");
                if (it != parent.object_range().end()) 
                {
                    uri items_location{context.make_schema_location("items")};
                    mcp_detail_jsoncons::string_view sub_keys[] = { "additionalItems" };

                    items_val = mcp_detail_jsoncons::make_unique<items_keyword<Json>>("items", parent, items_location,
                        context.get_custom_message("items"),
                        factory_->make_cross_draft_schema_validator(context, (*it).value(), sub_keys, anchor_dict));
                }
            }

            return mcp_detail_jsoncons::make_unique<prefix_items_validator<Json>>("prefixItems", parent, schema_location,  
                context.get_custom_message("prefixItems"),
                std::move(prefix_item_validators), std::move(items_val));
        }

    };

} // namespace jsonschema
} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_EXT_JSONSCHEMA_COMMON_SCHEMA_HPP
