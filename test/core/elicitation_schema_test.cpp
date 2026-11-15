#include "mcp/protocol/elicitation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace elicitation_schema_test {

namespace {

using nlohmann::json;

template <typename T>
T round_trip(const T& value) {
    const json encoded = value;
    return encoded.template get<T>();
}

}  // namespace

TEST(ElicitationSchema, StringFieldWritesItsConstraints) {
    mcp::StringSchema field;
    field.title = "Email";
    field.description = "Where to write";
    field.minLength = 3;
    field.maxLength = 50;
    field.format = "email";
    field.defaultValue = "user@example.com";
    EXPECT_EQ(json(field),
              json::parse(R"({"type":"string","title":"Email","description":"Where to write",
        "minLength":3,"maxLength":50,"format":"email","default":"user@example.com"})"));
    const auto decoded = round_trip(field);
    EXPECT_EQ(decoded.minLength, 3u);
    EXPECT_EQ(decoded.maxLength, 50u);
    EXPECT_EQ(decoded.format, "email");
    EXPECT_EQ(decoded.defaultValue, "user@example.com");
}

TEST(ElicitationSchema, NumberFieldIsNumberOrInteger) {
    mcp::NumberSchema number;
    number.minimum = 0.5;
    number.maximum = 9.5;
    number.defaultValue = 1.5;
    EXPECT_EQ(json(number),
              json::parse(R"({"type":"number","minimum":0.5,"maximum":9.5,"default":1.5})"));

    mcp::NumberSchema integer;
    integer.integer = true;
    integer.minimum = 18;
    integer.defaultValue = 30;
    const json written = integer;
    EXPECT_EQ(written, json::parse(R"({"type":"integer","minimum":18,"default":30})"));
    EXPECT_TRUE(written["minimum"].is_number_integer());
    const auto decoded = round_trip(integer);
    EXPECT_TRUE(decoded.integer);
    EXPECT_EQ(decoded.minimum, 18.0);
}

TEST(ElicitationSchema, BooleanFieldWritesItsDefault) {
    mcp::BooleanSchema field;
    field.title = "Subscribe";
    field.defaultValue = false;
    EXPECT_EQ(json(field), json::parse(R"({"type":"boolean","title":"Subscribe","default":false})"));
    EXPECT_EQ(round_trip(field).defaultValue, false);
}

TEST(ElicitationSchema, UntitledEnumWritesEnum) {
    mcp::EnumSchema field;
    field.enumValues = {"Red", "Green"};
    field.defaultValue = "Red";
    EXPECT_EQ(json(field), json::parse(R"({"type":"string","enum":["Red","Green"],"default":"Red"})"));
}

TEST(ElicitationSchema, TitledEnumWritesOneOf) {
    mcp::TitledEnumSchema field;
    field.options = {{"#f00", "Red"}, {"#0f0", "Green"}};
    field.defaultValue = "#f00";
    EXPECT_EQ(json(field), json::parse(R"({"type":"string","oneOf":[{"const":"#f00","title":"Red"},
        {"const":"#0f0","title":"Green"}],"default":"#f00"})"));
    const auto decoded = round_trip(field);
    ASSERT_EQ(decoded.options.size(), 2u);
    EXPECT_EQ(decoded.options[1].value, "#0f0");
    EXPECT_EQ(decoded.options[1].title, "Green");
}

TEST(ElicitationSchema, MultiSelectEnumsWriteArrays) {
    mcp::MultiSelectEnumSchema plain;
    plain.minItems = 1;
    plain.maxItems = 2;
    plain.enumValues = {"Red", "Green", "Blue"};
    plain.defaultValue = std::vector<std::string>{"Red"};
    EXPECT_EQ(json(plain), json::parse(R"({"type":"array","minItems":1,"maxItems":2,
        "items":{"type":"string","enum":["Red","Green","Blue"]},"default":["Red"]})"));

    mcp::TitledMultiSelectEnumSchema titled;
    titled.options = {{"#f00", "Red"}};
    EXPECT_EQ(json(titled),
              json::parse(R"({"type":"array","items":{"anyOf":[{"const":"#f00","title":"Red"}]}})"));
    EXPECT_EQ(round_trip(titled).options.size(), 1u);
    EXPECT_EQ(round_trip(plain).defaultValue, (std::vector<std::string>{"Red"}));
}

TEST(ElicitationSchema, LegacyEnumNamesAreKeptAndConvert) {
    const auto legacy = json::parse(
        R"({"type":"string","title":"Color","enum":["r","g","b"],"enumNames":["Red","Green"],"default":"g"})");
    const auto field = legacy.get<mcp::PrimitiveSchemaDefinition>();
    ASSERT_TRUE(std::holds_alternative<mcp::EnumSchema>(field));
    const auto& enum_field = std::get<mcp::EnumSchema>(field);
    EXPECT_EQ(json(enum_field), legacy);

    const auto titled = mcp::to_titled(enum_field);
    EXPECT_EQ(titled.title, "Color");
    EXPECT_EQ(titled.defaultValue, "g");
    ASSERT_EQ(titled.options.size(), 3u);
    EXPECT_EQ(titled.options[0].title, "Red");
    EXPECT_EQ(titled.options[1].title, "Green");
    // A value without a name is its own label.
    EXPECT_EQ(titled.options[2].title, "b");
}

TEST(ElicitationSchema, FieldPicksItsAlternativeFromTheDocument) {
    const auto pick = [](const char* text) {
        return json::parse(text).get<mcp::PrimitiveSchemaDefinition>();
    };
    EXPECT_TRUE(std::holds_alternative<mcp::StringSchema>(pick(R"({"type":"string"})")));
    EXPECT_TRUE(std::holds_alternative<mcp::NumberSchema>(pick(R"({"type":"integer"})")));
    EXPECT_TRUE(std::holds_alternative<mcp::BooleanSchema>(pick(R"({"type":"boolean"})")));
    EXPECT_TRUE(std::holds_alternative<mcp::EnumSchema>(pick(R"({"type":"string","enum":["a"]})")));
    EXPECT_TRUE(std::holds_alternative<mcp::TitledEnumSchema>(
        pick(R"({"type":"string","oneOf":[{"const":"a","title":"A"}]})")));
    EXPECT_TRUE(std::holds_alternative<mcp::MultiSelectEnumSchema>(
        pick(R"({"type":"array","items":{"type":"string","enum":["a"]}})")));
    EXPECT_TRUE(std::holds_alternative<mcp::TitledMultiSelectEnumSchema>(
        pick(R"({"type":"array","items":{"anyOf":[{"const":"a","title":"A"}]}})")));
}

TEST(ElicitationSchema, FieldRejectsWhatTheSpecificationDoesNotDefine) {
    const auto pick = [](const char* text) {
        return json::parse(text).get<mcp::PrimitiveSchemaDefinition>();
    };
    EXPECT_THROW(pick(R"({"type":"object"})"), std::invalid_argument);
    EXPECT_THROW(pick(R"({"title":"no type"})"), std::invalid_argument);
    EXPECT_THROW(pick(R"({"type":"array","items":{"type":"number"}})"), std::invalid_argument);
    EXPECT_THROW(pick(R"({"type":"array"})"), std::invalid_argument);
}

TEST(ElicitationSchema, BuilderWritesAFlatObjectSchema) {
    mcp::ElicitationSchema form;
    form.add("name", mcp::StringSchema{.title = "Name", .minLength = 1}, true)
        .add("age", mcp::NumberSchema{.integer = true, .minimum = 18})
        .add("color", mcp::TitledEnumSchema{.options = {{"#f00", "Red"}}});
    EXPECT_EQ(json(form), json::parse(R"({"type":"object","required":["name"],"properties":{
        "name":{"type":"string","title":"Name","minLength":1},
        "age":{"type":"integer","minimum":18},
        "color":{"type":"string","oneOf":[{"const":"#f00","title":"Red"}]}}})"));

    mcp::ElicitRequestFormParams params;
    params.message = "Tell us";
    params.requestedSchema = form;
    EXPECT_EQ(json(params)["requestedSchema"]["properties"]["name"]["minLength"], 1);
    EXPECT_THROW(form.add("name", mcp::BooleanSchema{}), std::invalid_argument);
}

TEST(ElicitationSchema, BuilderWritesTheDialectWhenSet) {
    mcp::ElicitationSchema form;
    form.dialect = "https://json-schema.org/draft/2020-12/schema";
    EXPECT_EQ(json(form), json::parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema",
        "type":"object","properties":{}})"));
}

TEST(ElicitationSchema, SchemaParsesBackIntoFields) {
    const auto document = json::parse(R"({"type":"object","required":["a"],
        "properties":{"a":{"type":"boolean"},"b":{"type":"string","maxLength":4}}})");
    const auto form = document.get<mcp::ElicitationSchema>();
    ASSERT_EQ(form.fields().size(), 2u);
    EXPECT_EQ(form.required(), (std::vector<std::string>{"a"}));
    EXPECT_EQ(json(form), document);
    EXPECT_THROW(json::parse(R"({"type":"array"})").get<mcp::ElicitationSchema>(),
                 std::invalid_argument);
}

namespace {

mcp::ElicitationSchema contact_form() {
    mcp::ElicitationSchema form;
    form.add("name", mcp::StringSchema{.minLength = 1}, true)
        .add("age", mcp::NumberSchema{.integer = true, .minimum = 18})
        .add("score", mcp::NumberSchema{.maximum = 10})
        .add("news", mcp::BooleanSchema{})
        .add("color", mcp::TitledEnumSchema{.options = {{"#f00", "Red"}, {"#0f0", "Green"}}})
        .add("tags", mcp::MultiSelectEnumSchema{.maxItems = 2, .enumValues = {"a", "b", "c"}});
    return form;
}

}  // namespace

TEST(ElicitationSchema, DecodeReturnsTypedValues) {
    const auto content = contact_form().decode(json::parse(
        R"({"name":"Ada","age":36,"score":9.5,"news":true,"color":"#0f0","tags":["a","c"]})"));
    EXPECT_EQ(content.get<std::string>("name"), "Ada");
    EXPECT_EQ(content.get<std::int64_t>("age"), 36);
    EXPECT_EQ(content.get<double>("age"), 36.0);
    EXPECT_EQ(content.get<double>("score"), 9.5);
    EXPECT_EQ(content.get<bool>("news"), true);
    EXPECT_EQ(content.get<std::string>("color"), "#0f0");
    EXPECT_EQ(content.get<std::vector<std::string>>("tags"), (std::vector<std::string>{"a", "c"}));
    // A field the user left out is absent, and a field of another type does not read.
    EXPECT_FALSE(contact_form().decode(json{{"name", "Ada"}}).contains("age"));
    EXPECT_FALSE(content.get<bool>("name").has_value());
    EXPECT_FALSE(content.get<std::string>("missing").has_value());
}

TEST(ElicitationSchema, DecodeRejectsContentThatDoesNotConform) {
    const auto form = contact_form();
    EXPECT_THROW(form.decode(json::parse(R"({})")), std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"({"name":"Ada","age":17})")), std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"({"name":"Ada","age":18.5})")), std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"({"name":"Ada","color":"blue"})")), std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"({"name":"Ada","tags":["a","b","c"]})")),
                 std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"({"name":"Ada","tags":["z"]})")), std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"({"name":"Ada","extra":1})")), std::invalid_argument);
    EXPECT_THROW(form.decode(json::parse(R"(["Ada"])")), std::invalid_argument);
    try {
        static_cast<void>(form.decode(json::parse(R"({"name":"Ada","age":17})")));
        FAIL() << "the content should not decode";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("/age"), std::string::npos) << error.what();
    }
}

TEST(ElicitationSchema, ValidationChecksAnAbsentContentAsAnEmptyObject) {
    const json required =
        json::parse(R"({"type":"object","properties":{"a":{"type":"string"}},"required":["a"]})");
    EXPECT_FALSE(mcp::validate_elicitation_content(required, std::nullopt).valid);
    const json open = json::parse(R"({"type":"object","properties":{"a":{"type":"string"}}})");
    EXPECT_TRUE(mcp::validate_elicitation_content(open, std::nullopt).valid);
    EXPECT_TRUE(mcp::validate_elicitation_content(required, json{{"a", "x"}}).valid);
    const auto bad = mcp::validate_elicitation_content(required, json{{"a", 1}});
    EXPECT_FALSE(bad.valid);
    EXPECT_NE(mcp::describe_violations(bad).find("/a"), std::string::npos);
}

TEST(ElicitationSchema, IntegerBoundsAreWrittenAsExactIntegers) {
    mcp::NumberSchema field;
    field.integer = true;
    field.minimum = 0.5;
    field.maximum = 9.5;
    field.defaultValue = 3.0;
    EXPECT_EQ(json(field), json::parse(R"({"type":"integer","minimum":1,"maximum":9,"default":3})"));

    // A bound beyond the 64-bit range on its open side leaves that side unbounded.
    field.minimum = -1e30;
    field.maximum = 1e30;
    const json wide = field;
    EXPECT_EQ(wide.at("minimum").get<std::int64_t>(), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(wide.at("maximum").get<std::int64_t>(), std::numeric_limits<std::int64_t>::max());

    // A bound no 64-bit integer can satisfy, a fractional default and NaN are refused.
    mcp::NumberSchema above;
    above.integer = true;
    above.minimum = 1e30;
    EXPECT_THROW(json{above}, std::invalid_argument);
    mcp::NumberSchema below;
    below.integer = true;
    below.maximum = -1e30;
    EXPECT_THROW(json{below}, std::invalid_argument);
    mcp::NumberSchema fractional;
    fractional.integer = true;
    fractional.defaultValue = 0.5;
    EXPECT_THROW(json{fractional}, std::invalid_argument);
    mcp::NumberSchema not_a_number;
    not_a_number.integer = true;
    not_a_number.maximum = std::nan("");
    EXPECT_THROW(json{not_a_number}, std::invalid_argument);

    // A `number` field keeps its bounds as they are.
    mcp::NumberSchema real;
    real.minimum = 0.5;
    real.maximum = 1e30;
    EXPECT_EQ(json(real).at("minimum"), 0.5);
    EXPECT_EQ(json(real).at("maximum"), 1e30);
}

TEST(ElicitationSchema, CountKeywordsMustBeNonNegativeIntegers) {
    EXPECT_THROW(
        json::parse(R"({"type":"string","minLength":-1})").get<mcp::PrimitiveSchemaDefinition>(),
        std::invalid_argument);
    EXPECT_THROW(
        json::parse(R"({"type":"string","maxLength":1.5})").get<mcp::PrimitiveSchemaDefinition>(),
        std::invalid_argument);
    EXPECT_THROW(json::parse(R"({"type":"array","maxItems":-2,"items":{"enum":["a"]}})")
                     .get<mcp::PrimitiveSchemaDefinition>(),
                 std::invalid_argument);
    EXPECT_THROW(mcp::ElicitationSchema::from_json_schema(json::parse(
                     R"({"type":"object","properties":{"a":{"type":"string","minLength":-1}}})")),
                 std::invalid_argument);
    const auto zero =
        json::parse(R"({"type":"string","minLength":0,"maxLength":4})").get<mcp::StringSchema>();
    EXPECT_EQ(zero.minLength, 0u);
    EXPECT_EQ(zero.maxLength, 4u);
}

TEST(ElicitationSchema, JsonFormDoesNotKeepTheOrderFieldsWereAddedIn) {
    mcp::ElicitationSchema form;
    form.add("b", mcp::BooleanSchema{}).add("a", mcp::StringSchema{});
    ASSERT_EQ(form.fields().size(), 2u);
    EXPECT_EQ(form.fields()[0].first, "b");
    const auto parsed = mcp::ElicitationSchema::from_json_schema(json(form));
    ASSERT_EQ(parsed.fields().size(), 2u);
    EXPECT_EQ(parsed.fields()[0].first, "a");
}

}  // namespace elicitation_schema_test
