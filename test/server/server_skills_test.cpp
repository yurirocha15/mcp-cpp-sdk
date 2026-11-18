#include "../support/extension_harness.hpp"
#include "../test_utils.hpp"

#include "mcp/client/skills.hpp"
#include "mcp/core/async.hpp"
#include "mcp/protocol/skills.hpp"
#include "mcp/server/server.hpp"
#include "mcp/server/skills.hpp"

#include <gtest/gtest.h>

#include <mcp/core/constants.hpp>
#include <nlohmann/json.hpp>

namespace server_skills_test {

namespace jsonrpc = mcp::constants::jsonrpc;

namespace {

using nlohmann::json;
namespace skills = mcp::skills;

mcp::ServerCapabilities skill_capabilities() {
    mcp::ServerCapabilities capabilities;
    capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{};
    return capabilities;
}

skills::SkillDefinition git_workflow() {
    skills::SkillDefinition skill;
    skill.path = "git-workflow";
    skill.frontmatter = {
        {"name", "git-workflow"},
        {"description", "Follow this team's Git conventions for branching and commits"}};
    skill.files.push_back(
        {"SKILL.md",
         "---\nname: git-workflow\ndescription: Follow this team's Git conventions for "
         "branching and commits\n---\n\nUse short-lived branches.\n",
         std::nullopt,
         {}});
    return skill;
}

skills::SkillDefinition refunds() {
    skills::SkillDefinition skill;
    skill.path = "acme/billing/refunds";
    skill.frontmatter = {{"name", "refunds"},
                         {"description", "Process customer refund requests per company policy"},
                         {"license", "Apache-2.0"}};
    skill.files.push_back(
        {"SKILL.md", "---\nname: refunds\n---\nSee examples/email.md.\n", std::nullopt, {}});
    skill.files.push_back({"examples/email.md", "Dear customer,\n", std::nullopt, {}});
    skill.files.push_back(
        {"assets/logo.png", std::string("\x89PNG\r\n\x1a\n\0\0", 10), std::nullopt, {}});
    return skill;
}

json read_request(int id, const std::string& uri) {
    return make_modern_request(id, "resources/read", {{"uri", uri}});
}

TEST(SkillsDigestTest, DigestsMatchSha256) {
    // SHA-256 of the empty string and of "abc", from FIPS 180-4.
    EXPECT_EQ(skills::sha256_digest(""),
              "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(skills::sha256_digest("abc"),
              "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SkillsDigestTest, VerifyContentChecksSizeThenDigest) {
    mcp::SkillResource entry{"skill://a/SKILL.md", skills::sha256_digest("abc"), 3};
    EXPECT_TRUE(skills::verify_content(entry, "abc").ok);

    const auto changed = skills::verify_content(entry, "abd");
    EXPECT_FALSE(changed.ok);
    EXPECT_NE(changed.message.find("digest"), std::string::npos);

    const auto longer = skills::verify_content(entry, "abcd");
    EXPECT_FALSE(longer.ok);
    EXPECT_NE(longer.message.find("size"), std::string::npos);
}

TEST(SkillsNameTest, FollowsTheAgentSkillsRules) {
    EXPECT_TRUE(skills::is_valid_skill_name("pdf-processing"));
    EXPECT_TRUE(skills::is_valid_skill_name("a"));
    EXPECT_TRUE(skills::is_valid_skill_name(std::string(64, 'a')));
    EXPECT_FALSE(skills::is_valid_skill_name(std::string(65, 'a')));
    EXPECT_FALSE(skills::is_valid_skill_name(""));
    EXPECT_FALSE(skills::is_valid_skill_name("PDF-Processing"));
    EXPECT_FALSE(skills::is_valid_skill_name("-pdf"));
    EXPECT_FALSE(skills::is_valid_skill_name("pdf-"));
    EXPECT_FALSE(skills::is_valid_skill_name("pdf--processing"));
    EXPECT_FALSE(skills::is_valid_skill_name("pdf_processing"));
}

// The entry the specification shows for skills/list.
TEST(SkillsWireTest, EntriesParseAndRoundTrip) {
    const auto wire = json::parse(R"({
        "uri": "skill://acme/billing/refunds/SKILL.md",
        "frontmatter": {"name": "refunds", "description": "Process customer refund requests per company policy",
                        "license": "Apache-2.0"},
        "resources": [
            {"uri": "skill://acme/billing/refunds/SKILL.md",
             "digest": "sha256:ae7bc7e45f44a6381977ef1f772cbad6e38d8ec78903ea7b87abb373815a8f89", "size": 175},
            {"uri": "skill://acme/billing/refunds/examples/email.md",
             "digest": "sha256:a724fa5ed3b9e39c0e731fe1f0e866eb46d23969af25ecf0d5f41982e3e36bea", "size": 58}
        ]
    })");
    const auto skill = wire.get<mcp::Skill>();
    EXPECT_FALSE(skill.dynamic);
    EXPECT_EQ(skill.resources.size(), 2U);
    EXPECT_EQ(skill.frontmatter["license"], "Apache-2.0");
    EXPECT_EQ(json(skill), wire);
    EXPECT_TRUE(skills::validate_skill(skill).empty());
}

TEST(SkillsWireTest, DynamicEntriesCarryNoDigests) {
    const auto wire = json::parse(R"({
        "uri": "skill://reports/daily/SKILL.md",
        "frontmatter": {"name": "daily", "description": "Assemble today's operational report"},
        "resources": "dynamic"
    })");
    const auto skill = wire.get<mcp::Skill>();
    EXPECT_TRUE(skill.dynamic);
    EXPECT_TRUE(skill.resources.empty());
    EXPECT_EQ(json(skill), wire);
    EXPECT_TRUE(skills::validate_skill(skill).empty());
}

TEST(SkillsWireTest, EntriesWithoutResourcesOrWithOtherValuesAreInvalid) {
    const json base = {{"uri", "skill://a/SKILL.md"},
                       {"frontmatter", {{"name", "a"}, {"description", "d"}}}};
    EXPECT_THROW((void)base.get<mcp::Skill>(), json::exception);

    auto other = base;
    other["resources"] = "static";
    EXPECT_THROW((void)other.get<mcp::Skill>(), std::invalid_argument);

    auto no_description = base;
    no_description["frontmatter"].erase("description");
    no_description["resources"] = json::array();
    EXPECT_THROW((void)no_description.get<mcp::Skill>(), std::invalid_argument);
}

TEST(SkillsValidationTest, ReportsEachRuleTheExtensionPutsOnAnEntry) {
    const auto digest = skills::sha256_digest("x");
    auto entry = [&] {
        mcp::Skill skill;
        skill.uri = "skill://acme/refunds/SKILL.md";
        skill.frontmatter = {{"name", "refunds"}, {"description", "d"}};
        skill.resources = {{"skill://acme/refunds/SKILL.md", digest, 1},
                           {"skill://acme/refunds/ref.md", digest, 1}};
        return skill;
    };
    EXPECT_TRUE(skills::validate_skill(entry()).empty());

    auto wrong_name = entry();
    wrong_name.frontmatter["name"] = "other";
    EXPECT_FALSE(skills::validate_skill(wrong_name).empty());

    auto no_skill_file = entry();
    no_skill_file.resources.erase(no_skill_file.resources.begin());
    EXPECT_FALSE(skills::validate_skill(no_skill_file).empty());

    auto outside = entry();
    outside.resources.push_back({"skill://acme/elsewhere/x.md", digest, 1});
    EXPECT_FALSE(skills::validate_skill(outside).empty());

    for (const std::string tail : std::vector<std::string>{
             "../../etc/cron.d/evil", "/abs", "a//b", "./x", "a/./b", "a/..", "a\\b", "%2e%2e/x",
             "%2E/x", "a/.%2e/b", std::string("a\0b", 3), "trailing/"}) {
        auto unsafe = entry();
        unsafe.resources.push_back({"skill://acme/refunds/" + tail, digest, 1});
        EXPECT_FALSE(skills::validate_skill(unsafe).empty()) << tail;
    }

    auto duplicated = entry();
    duplicated.resources.push_back(duplicated.resources.front());
    EXPECT_FALSE(skills::validate_skill(duplicated).empty());

    auto bad_digest = entry();
    bad_digest.resources[1].digest = "sha256:ABC";
    EXPECT_FALSE(skills::validate_skill(bad_digest).empty());

    auto too_many = entry();
    for (std::size_t index = 0; index < mcp::constants::skills::g_MAX_RESOURCES_PER_SKILL; ++index) {
        too_many.resources.push_back({"skill://acme/refunds/f" + std::to_string(index), digest, 1});
    }
    EXPECT_FALSE(skills::validate_skill(too_many).empty());

    auto too_big = entry();
    too_big.resources[1].size = mcp::constants::skills::g_MAX_SKILL_BYTES;
    EXPECT_FALSE(skills::validate_skill(too_big).empty());

    auto not_skill_file = entry();
    not_skill_file.uri = "skill://acme/refunds";
    EXPECT_FALSE(skills::validate_skill(not_skill_file).empty());
}

TEST(SkillsCapabilityTest, SettingsAndDetection) {
    EXPECT_EQ(skills::server_settings(false), json::object());
    EXPECT_EQ(skills::server_settings(true), (json{{"directoryRead", true}}));

    mcp::ServerCapabilities capabilities;
    EXPECT_FALSE(skills::supports_skills(capabilities));
    capabilities.extensions =
        std::map<std::string, json>{{"io.modelcontextprotocol/skills", json::object()}};
    EXPECT_TRUE(skills::supports_skills(capabilities));
    EXPECT_FALSE(skills::supports_directory_read(capabilities));
    (*capabilities.extensions)["io.modelcontextprotocol/skills"] = json{{"directoryRead", true}};
    EXPECT_TRUE(skills::supports_directory_read(capabilities));
}

TEST(SkillsServerTest, TheExtensionIsDeclaredWithTheResourcesCapability) {
    mcp::Server without("skills-server", "1.0");
    const skills::SkillsExtension declared(without);
    EXPECT_TRUE(without.capabilities().resources.has_value());

    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    const auto discover = mcp_test::dispatch_direct(server, make_discover_request(1));
    ASSERT_TRUE(discover.contains("result")) << discover;
    EXPECT_EQ(discover["result"]["capabilities"]["extensions"]["io.modelcontextprotocol/skills"],
              (json{{"directoryRead", true}}));

    mcp::Server plain({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsOptions options;
    options.directory_read = false;
    skills::SkillsExtension plain_extension(plain, options);
    const auto plain_discover = mcp_test::dispatch_direct(plain, make_discover_request(1));
    EXPECT_EQ(plain_discover["result"]["capabilities"]["extensions"]["io.modelcontextprotocol/skills"],
              json::object());
    const auto directory = mcp_test::dispatch_direct(
        plain, make_modern_request(2, "resources/directory/read", {{"uri", "skill://x"}}));
    EXPECT_EQ(directory["error"]["code"], jsonrpc::g_METHOD_NOT_FOUND) << directory;

    skills::SkillsOptions bad_page;
    bad_page.page_size = 0;
    mcp::Server other({"skills-server", "1.0"}, skill_capabilities());
    EXPECT_THROW(skills::SkillsExtension rejected(other, bad_page), std::invalid_argument);
}

TEST(SkillsServerTest, ListReturnsEntriesWithDigestsAndCacheHints) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    extension.add_skill(git_workflow());
    extension.add_skill(refunds());

    const auto response = mcp_test::dispatch_direct(server, make_modern_request(1, "skills/list"));
    ASSERT_TRUE(response.contains("result")) << response;
    const auto& result = response["result"];
    EXPECT_EQ(result["resultType"], "complete");
    EXPECT_EQ(result["ttlMs"], 300000);
    EXPECT_EQ(result["cacheScope"], "public");
    ASSERT_EQ(result["skills"].size(), 2U);

    const auto listed = result.get<mcp::ListSkillsResult>();
    const auto& git = listed.skills.at(0);
    EXPECT_EQ(git.uri, "skill://git-workflow/SKILL.md");
    ASSERT_EQ(git.resources.size(), 1U);
    EXPECT_EQ(git.resources[0].digest, skills::sha256_digest(git_workflow().files[0].content));
    EXPECT_EQ(git.resources[0].size, static_cast<std::int64_t>(git_workflow().files[0].content.size()));

    const auto& billing = listed.skills.at(1);
    EXPECT_EQ(billing.uri, "skill://acme/billing/refunds/SKILL.md");
    EXPECT_EQ(billing.resources.size(), 3U);
    EXPECT_EQ(billing.frontmatter["license"], "Apache-2.0");
    for (const auto& skill : listed.skills) {
        EXPECT_TRUE(skills::validate_skill(skill).empty());
    }
}

TEST(SkillsServerTest, ListPaginatesWithOpaqueCursors) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsOptions options;
    options.page_size = 1;
    skills::SkillsExtension extension(server, options);
    extension.add_skill(git_workflow());
    extension.add_skill(refunds());

    const auto first = mcp_test::dispatch_direct(server, make_modern_request(1, "skills/list"));
    ASSERT_TRUE(first.contains("result")) << first;
    EXPECT_EQ(first["result"]["skills"].size(), 1U);
    ASSERT_TRUE(first["result"].contains("nextCursor"));

    const auto second = mcp_test::dispatch_direct(
        server, make_modern_request(2, "skills/list", {{"cursor", first["result"]["nextCursor"]}}));
    ASSERT_TRUE(second.contains("result")) << second;
    EXPECT_EQ(second["result"]["skills"][0]["uri"], "skill://acme/billing/refunds/SKILL.md");
    EXPECT_FALSE(second["result"].contains("nextCursor"));

    for (const json& cursor : {json("junk"), json("o99"), json(7), json("o")}) {
        const auto bad = mcp_test::dispatch_direct(
            server, make_modern_request(3, "skills/list", {{"cursor", cursor}}));
        EXPECT_EQ(bad["error"]["code"], jsonrpc::g_INVALID_PARAMS) << cursor;
    }
}

TEST(SkillsServerTest, GetAnswersForAServedSkillAndRefusesOthers) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    extension.add_skill(refunds());

    const auto found = mcp_test::dispatch_direct(
        server,
        make_modern_request(1, "skills/get", {{"uri", "skill://acme/billing/refunds/SKILL.md"}}));
    ASSERT_TRUE(found.contains("result")) << found;
    EXPECT_EQ(found["result"]["skill"]["frontmatter"]["name"], "refunds");
    EXPECT_EQ(found["result"]["ttlMs"], 300000);
    EXPECT_EQ(found["result"]["resultType"], "complete");

    const auto unknown = mcp_test::dispatch_direct(
        server, make_modern_request(2, "skills/get", {{"uri", "skill://nothing/SKILL.md"}}));
    EXPECT_EQ(unknown["error"]["code"], jsonrpc::g_INVALID_PARAMS) << unknown;
    EXPECT_NE(unknown["error"]["message"].get<std::string>().find("skill://nothing/SKILL.md"),
              std::string::npos);

    const auto missing = mcp_test::dispatch_direct(server, make_modern_request(3, "skills/get"));
    EXPECT_EQ(missing["error"]["code"], jsonrpc::g_INVALID_PARAMS) << missing;
}

TEST(SkillsServerTest, FilesAreReadWithResourcesReadAndMatchTheirDigests) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    const auto definition = refunds();
    extension.add_skill(definition);

    const auto list = mcp_test::dispatch_direct(server, make_modern_request(1, "skills/list"));
    const auto entry = list["result"]["skills"][0].get<mcp::Skill>();
    for (const auto& file : definition.files) {
        const std::string uri = "skill://acme/billing/refunds/" + file.path;
        const auto response = mcp_test::dispatch_direct(server, read_request(2, uri));
        ASSERT_TRUE(response.contains("result")) << response;
        const auto& content = response["result"]["contents"][0];
        EXPECT_EQ(content["uri"], uri);
        if (file.path == "assets/logo.png") {
            EXPECT_EQ(content["mimeType"], "image/png");
            EXPECT_TRUE(content.contains("blob"));
            EXPECT_FALSE(content.contains("text"));
        } else {
            EXPECT_TRUE(content.contains("text"));
            EXPECT_EQ(content["text"], file.content);
            EXPECT_EQ(content["mimeType"], "text/markdown");
        }
    }
    const auto skill_file =
        mcp_test::dispatch_direct(server, read_request(3, "skill://acme/billing/refunds/SKILL.md"));
    ASSERT_TRUE(skill_file.contains("result")) << skill_file;
    EXPECT_TRUE(skills::verify_content(entry.resources[0],
                                       skill_file["result"]["contents"][0]["text"].get<std::string>())
                    .ok);
}

TEST(SkillsServerTest, ResourceMetadataNamesTheSkill) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    extension.add_skill(refunds());
    const auto listed = mcp_test::dispatch_direct(server, make_modern_request(1, "resources/list"));
    ASSERT_TRUE(listed.contains("result")) << listed;
    const auto& resources = listed["result"]["resources"];
    ASSERT_EQ(resources.size(), 3U);
    EXPECT_EQ(resources[0]["uri"], "skill://acme/billing/refunds/SKILL.md");
    EXPECT_EQ(resources[0]["name"], "refunds");
    EXPECT_EQ(resources[0]["description"], "Process customer refund requests per company policy");
    EXPECT_EQ(resources[0]["mimeType"], "text/markdown");
}

TEST(SkillsServerTest, DynamicSkillsAreListedWithoutDigestsAndGenerateOnEveryRead) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    int reads = 0;
    skills::SkillDefinition daily;
    daily.path = "reports/daily";
    daily.frontmatter = {{"name", "daily"}, {"description", "Assemble today's report"}};
    daily.files.push_back(
        {"SKILL.md", "", std::nullopt, [&reads] { return "Report " + std::to_string(++reads); }});
    extension.add_skill(daily);

    const auto get = mcp_test::dispatch_direct(
        server, make_modern_request(1, "skills/get", {{"uri", "skill://reports/daily/SKILL.md"}}));
    ASSERT_TRUE(get.contains("result")) << get;
    EXPECT_EQ(get["result"]["skill"]["resources"], "dynamic");

    EXPECT_EQ(
        mcp_test::dispatch_direct(
            server, read_request(2, "skill://reports/daily/SKILL.md"))["result"]["contents"][0]["text"],
        "Report 1");
    EXPECT_EQ(
        mcp_test::dispatch_direct(
            server, read_request(3, "skill://reports/daily/SKILL.md"))["result"]["contents"][0]["text"],
        "Report 2");
}

TEST(SkillsServerTest, DirectoryReadListsDirectChildrenOnly) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    skills::SkillDefinition pdf;
    pdf.path = "pdf-processing";
    pdf.frontmatter = {{"name", "pdf-processing"}, {"description", "Extract, fill, and assemble PDFs"}};
    pdf.files.push_back({"SKILL.md", "skill", std::nullopt, {}});
    pdf.files.push_back({"templates/invoice.md", "# Invoice", std::nullopt, {}});
    pdf.files.push_back({"templates/purchase-order.md", "# PO", std::nullopt, {}});
    pdf.files.push_back({"templates/regional/uk.md", "# UK", std::nullopt, {}});
    extension.add_skill(pdf);

    const auto root = mcp_test::dispatch_direct(
        server,
        make_modern_request(1, "resources/directory/read", {{"uri", "skill://pdf-processing"}}));
    ASSERT_TRUE(root.contains("result")) << root;
    EXPECT_EQ(root["result"]["resultType"], "complete");
    EXPECT_EQ(root["result"]["ttlMs"], 300000);
    const auto top = root["result"].get<mcp::ReadResourceDirectoryResult>();
    ASSERT_EQ(top.resources.size(), 2U);
    EXPECT_EQ(top.resources[0].uri, "skill://pdf-processing/SKILL.md");
    EXPECT_EQ(top.resources[1].uri, "skill://pdf-processing/templates");
    EXPECT_EQ(top.resources[1].mimeType, "inode/directory");

    const auto templates = mcp_test::dispatch_direct(
        server, make_modern_request(2, "resources/directory/read",
                                    {{"uri", "skill://pdf-processing/templates"}}));
    ASSERT_TRUE(templates.contains("result")) << templates;
    const auto children = templates["result"].get<mcp::ReadResourceDirectoryResult>();
    ASSERT_EQ(children.resources.size(), 3U);
    EXPECT_EQ(children.resources[0].uri, "skill://pdf-processing/templates/invoice.md");
    EXPECT_EQ(children.resources[0].mimeType, "text/markdown");
    EXPECT_EQ(children.resources[2].uri, "skill://pdf-processing/templates/regional");
    EXPECT_EQ(children.resources[2].mimeType, "inode/directory");
}

TEST(SkillsServerTest, DirectoryReadRefusesFilesAndUnknownUris) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    extension.add_skill(git_workflow());

    const auto file = mcp_test::dispatch_direct(
        server,
        make_modern_request(1, "resources/directory/read", {{"uri", "skill://git-workflow/SKILL.md"}}));
    EXPECT_EQ(file["error"]["code"], jsonrpc::g_INVALID_PARAMS) << file;
    EXPECT_NE(file["error"]["message"].get<std::string>().find("is not a directory resource"),
              std::string::npos);

    const auto unknown = mcp_test::dispatch_direct(
        server, make_modern_request(2, "resources/directory/read", {{"uri", "skill://nothing"}}));
    EXPECT_EQ(unknown["error"]["code"], jsonrpc::g_INVALID_PARAMS) << unknown;
}

TEST(SkillsServerTest, DirectoryReadPaginates) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsOptions options;
    options.page_size = 1;
    skills::SkillsExtension extension(server, options);
    extension.add_skill(refunds());

    const auto first = mcp_test::dispatch_direct(
        server,
        make_modern_request(1, "resources/directory/read", {{"uri", "skill://acme/billing/refunds"}}));
    ASSERT_TRUE(first.contains("result")) << first;
    EXPECT_EQ(first["result"]["resources"].size(), 1U);
    ASSERT_TRUE(first["result"].contains("nextCursor"));
}

TEST(SkillsServerTest, AddSkillRejectsDefinitionsThatBreakTheRules) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);

    auto mismatched = git_workflow();
    mismatched.frontmatter["name"] = "other";
    EXPECT_THROW(extension.add_skill(mismatched), std::invalid_argument);

    auto no_skill_file = git_workflow();
    no_skill_file.files.clear();
    no_skill_file.files.push_back({"README.md", "x", std::nullopt, {}});
    EXPECT_THROW(extension.add_skill(no_skill_file), std::invalid_argument);

    auto traversal = git_workflow();
    traversal.files.push_back({"../escape.md", "x", std::nullopt, {}});
    EXPECT_THROW(extension.add_skill(traversal), std::invalid_argument);

    auto twice = git_workflow();
    twice.files.push_back({"SKILL.md", "again", std::nullopt, {}});
    EXPECT_THROW(extension.add_skill(twice), std::invalid_argument);

    auto no_description = git_workflow();
    no_description.frontmatter.erase("description");
    EXPECT_THROW(extension.add_skill(no_description), std::invalid_argument);

    auto bad_path = git_workflow();
    bad_path.path = "git-workflow/";
    EXPECT_THROW(extension.add_skill(bad_path), std::invalid_argument);

    extension.add_skill(git_workflow());
    EXPECT_THROW(extension.add_skill(git_workflow()), std::invalid_argument);
}

TEST(SkillsClientTest, AClientListsGetsAndReadsWithVerification) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    extension.add_skill(git_workflow());
    extension.add_skill(refunds());

    mcp_test::run_modern_client(
        server, {}, [](mcp::Client& client, const mcp::ConnectResult& connected) -> mcp::Task<void> {
            EXPECT_TRUE(skills::supports_skills(connected.capabilities));
            EXPECT_TRUE(skills::supports_directory_read(connected.capabilities));

            const auto listed = co_await skills::list_skills(client);
            EXPECT_EQ(listed.skills.size(), 2U);
            EXPECT_EQ(listed.ttlMs, 300000);

            const auto fetched =
                co_await skills::get_skill(client, "skill://acme/billing/refunds/SKILL.md");
            EXPECT_EQ(fetched.skill.frontmatter["name"], "refunds");

            const auto text = co_await skills::read_skill_file(
                client, fetched.skill, "skill://acme/billing/refunds/examples/email.md");
            EXPECT_EQ(text, "Dear customer,\n");

            // A blob is decoded before it is compared with the entry.
            const auto png = co_await skills::read_skill_file(
                client, fetched.skill, "skill://acme/billing/refunds/assets/logo.png");
            EXPECT_EQ(png, std::string("\x89PNG\r\n\x1a\n\0\0", 10));

            const auto directory =
                co_await skills::read_directory(client, "skill://acme/billing/refunds", true);
            EXPECT_EQ(directory.resources.size(), 3U);

            bool refused = false;
            try {
                (void)co_await skills::get_skill(client, "skill://nothing/SKILL.md");
            } catch (const mcp::McpError& error) {
                refused = error.code() == jsonrpc::g_INVALID_PARAMS;
            }
            EXPECT_TRUE(refused);

            bool not_sent = false;
            try {
                (void)co_await skills::read_directory(client, "skill://acme/billing/refunds", false);
            } catch (const std::logic_error&) {
                not_sent = true;
            }
            EXPECT_TRUE(not_sent);
        });
}

TEST(SkillsClientTest, ContentThatDiffersFromTheEntryIsNotUsed) {
    mcp::Server server({"skills-server", "1.0"}, skill_capabilities());
    skills::SkillsExtension extension(server);
    extension.add_skill(git_workflow());

    mcp_test::run_modern_client(
        server, {}, [](mcp::Client& client, const mcp::ConnectResult&) -> mcp::Task<void> {
            auto entry = (co_await skills::get_skill(client, "skill://git-workflow/SKILL.md")).skill;

            // An entry whose digest no longer matches what the server serves.
            auto stale = entry;
            stale.resources[0].digest = skills::sha256_digest("something else");
            bool mismatch = false;
            try {
                (void)co_await skills::read_skill_file(client, stale, "skill://git-workflow/SKILL.md");
            } catch (const std::runtime_error& error) {
                mismatch = std::string(error.what()).find("digest") != std::string::npos;
            }
            EXPECT_TRUE(mismatch);

            // A file the held entry does not list is a verification failure, even though the server
            // would serve it.
            auto unlisted = entry;
            unlisted.resources.clear();
            unlisted.resources.push_back(entry.resources[0]);
            bool refused = false;
            try {
                (void)co_await skills::read_skill_file(client, unlisted,
                                                       "skill://git-workflow/other.md");
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()).find("not listed") != std::string::npos;
            }
            EXPECT_TRUE(refused);
        });
}

}  // namespace

}  // namespace server_skills_test
