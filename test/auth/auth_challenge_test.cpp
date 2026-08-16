/**
 * @file auth_challenge_test.cpp
 * @brief Tests for WWW-Authenticate challenge parsing, RFC 9207 issuer validation, and the
 *        outbound-request policy that guards OAuth metadata discovery (Slice A / 2.E5a).
 */

#include <gtest/gtest.h>

#include <mcp/auth/challenge.hpp>
#include <mcp/auth/metadata_policy.hpp>
#include <string>
#include <vector>

namespace {

mcp::auth::AuthorizationRequest make_request(std::string issuer, bool issuer_parameter_supported) {
    mcp::auth::AuthorizationRequest request;
    request.state = "recorded-state";
    request.code_verifier = "recorded-verifier";
    request.issuer = std::move(issuer);
    request.issuer_parameter_supported = issuer_parameter_supported;
    return request;
}

mcp::auth::AuthorizationResponse make_response(std::optional<std::string> iss) {
    mcp::auth::AuthorizationResponse response;
    response.code = "auth-code";
    response.state = "recorded-state";
    response.iss = std::move(iss);
    return response;
}

mcp::auth::MetadataFetchPolicy allow_origin(std::string origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(std::move(origin));
    return policy;
}

}  // namespace

TEST(AuthChallengeParserTest, ParsesSchemeWithNoParameters) {
    const auto challenges = mcp::auth::parse_www_authenticate("Bearer");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].scheme, "Bearer");
    EXPECT_TRUE(challenges[0].is_bearer());
    EXPECT_TRUE(challenges[0].parameters.empty());
}

TEST(AuthChallengeParserTest, ParsesQuotedAndUnquotedValues) {
    const auto challenges =
        mcp::auth::parse_www_authenticate(R"(Bearer realm="mcp server", error=invalid_token)");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].realm, "mcp server");
    EXPECT_EQ(challenges[0].error, "invalid_token");
}

TEST(AuthChallengeParserTest, ExpandsBackslashEscapesInQuotedValues) {
    const auto challenges = mcp::auth::parse_www_authenticate(
        R"(Bearer error_description="the server said \"no\" \\ twice")");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].error_description, R"(the server said "no" \ twice)");
}

TEST(AuthChallengeParserTest, MatchesParameterNamesCaseInsensitively) {
    const auto challenges = mcp::auth::parse_www_authenticate(
        R"(Bearer REALM="r", Resource_Metadata="https://rs.test/.well-known/x", SCOPE="a b")");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].realm, "r");
    EXPECT_EQ(challenges[0].resource_metadata, "https://rs.test/.well-known/x");
    EXPECT_EQ(challenges[0].scope, "a b");
    ASSERT_EQ(challenges[0].parameters.size(), 3U);
    EXPECT_EQ(challenges[0].parameters[0].first, "realm");
    EXPECT_EQ(challenges[0].parameters[1].first, "resource_metadata");
    EXPECT_EQ(challenges[0].parameters[2].first, "scope");
}

TEST(AuthChallengeParserTest, AcceptsArbitraryParameterOrdering) {
    const auto forward = mcp::auth::parse_www_authenticate(
        R"(Bearer resource_metadata="https://rs.test/prm", scope="a", realm="r")");
    const auto reverse = mcp::auth::parse_www_authenticate(
        R"(Bearer realm="r", scope="a", resource_metadata="https://rs.test/prm")");
    ASSERT_EQ(forward.size(), 1U);
    ASSERT_EQ(reverse.size(), 1U);
    EXPECT_EQ(forward[0].resource_metadata, reverse[0].resource_metadata);
    EXPECT_EQ(forward[0].scope, reverse[0].scope);
    EXPECT_EQ(forward[0].realm, reverse[0].realm);
}

TEST(AuthChallengeParserTest, SplitsSeveralChallengesInOneHeader) {
    const auto challenges =
        mcp::auth::parse_www_authenticate(R"(Basic realm="legacy", Bearer realm="mcp")");
    ASSERT_EQ(challenges.size(), 2U);
    EXPECT_EQ(challenges[0].scheme, "Basic");
    EXPECT_EQ(challenges[0].realm, "legacy");
    EXPECT_FALSE(challenges[0].is_bearer());
    EXPECT_EQ(challenges[1].scheme, "Bearer");
    EXPECT_EQ(challenges[1].realm, "mcp");
}

TEST(AuthChallengeParserTest, SkipsToken68CredentialsBeforeTheNextChallenge) {
    const auto challenges =
        mcp::auth::parse_www_authenticate(R"(Negotiate a1b2c3==, Bearer realm="mcp")");
    ASSERT_EQ(challenges.size(), 2U);
    EXPECT_EQ(challenges[0].scheme, "Negotiate");
    EXPECT_EQ(challenges[1].scheme, "Bearer");
    EXPECT_EQ(challenges[1].realm, "mcp");
}

TEST(AuthChallengeParserTest, CombinesSeveralHeaderValues) {
    const std::vector<std::string> headers = {R"(Basic realm="legacy")",
                                              R"(Bearer resource_metadata="https://rs.test/prm")"};
    const auto challenges = mcp::auth::parse_www_authenticate(headers);
    ASSERT_EQ(challenges.size(), 2U);
    EXPECT_EQ(challenges[0].scheme, "Basic");
    EXPECT_EQ(challenges[1].resource_metadata, "https://rs.test/prm");
}

TEST(AuthChallengeParserTest, KeepsFirstOccurrenceOfARepeatedParameter) {
    const auto challenges =
        mcp::auth::parse_www_authenticate(R"(Bearer realm="first", realm="second")");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].realm, "first");
    EXPECT_EQ(challenges[0].parameters.size(), 2U);
}

TEST(AuthChallengeParserTest, DiscardsTrailingDamageAfterAnUnterminatedQuotedString) {
    const auto challenges = mcp::auth::parse_www_authenticate(R"(Bearer realm="unterminated)");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].scheme, "Bearer");
    EXPECT_FALSE(challenges[0].realm.has_value());
}

TEST(AuthChallengeParserTest, SelectsTheFirstBearerChallenge) {
    const auto challenges = mcp::auth::parse_www_authenticate(
        R"(Basic realm="legacy", Bearer realm="one", Bearer realm="two")");
    const auto selected = mcp::auth::select_bearer_challenge(challenges);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->realm, "one");
}

TEST(AuthChallengeParserTest, SelectsNothingWhenNoBearerChallengeIsPresent) {
    const auto challenges = mcp::auth::parse_www_authenticate(R"(Basic realm="legacy")");
    EXPECT_FALSE(mcp::auth::select_bearer_challenge(challenges).has_value());
}

TEST(AuthAuthorizationResponseParseTest, DecodesRedirectQueryParameters) {
    const auto response = mcp::auth::parse_authorization_response(
        "http://127.0.0.1:9000/callback?code=abc%2F123&state=s1&iss=https%3A%2F%2Fas.test");
    EXPECT_EQ(response.code, "abc/123");
    EXPECT_EQ(response.state, "s1");
    EXPECT_EQ(response.iss, "https://as.test");
}

TEST(AuthAuthorizationResponseParseTest, DecodesPlusAsSpaceAndBareQueryStrings) {
    const auto response =
        mcp::auth::parse_authorization_response("error=access_denied&error_description=user+said+no");
    EXPECT_EQ(response.error, "access_denied");
    EXPECT_EQ(response.error_description, "user said no");
    EXPECT_FALSE(response.code.has_value());
}

TEST(AuthAuthorizationResponseParseTest, IgnoresTheFragmentComponent) {
    const auto response =
        mcp::auth::parse_authorization_response("http://127.0.0.1/cb?code=abc#state=spoofed");
    EXPECT_EQ(response.code, "abc");
    EXPECT_FALSE(response.state.has_value());
}

// The RFC 9207 Section 2.4 decision table as adopted by MCP: four rows over
// `authorization_response_iss_parameter_supported` by the presence of `iss`.
TEST(AuthIssuerValidationTest, AppliesTheFourRowDecisionTable) {
    struct Row {
        const char* name;
        bool issuer_parameter_supported;
        bool iss_present;
        mcp::auth::AuthorizationResponseStatus expected;
    };
    const Row rows[] = {
        {"advertised and present compares equal", true, true,
         mcp::auth::AuthorizationResponseStatus::accepted},
        {"advertised and absent rejects", true, false,
         mcp::auth::AuthorizationResponseStatus::issuer_missing},
        {"not advertised but present still compares", false, true,
         mcp::auth::AuthorizationResponseStatus::accepted},
        {"not advertised and absent proceeds", false, false,
         mcp::auth::AuthorizationResponseStatus::accepted},
    };

    for (const auto& row : rows) {
        const auto request = make_request("https://as.test/tenant1", row.issuer_parameter_supported);
        const auto response = make_response(
            row.iss_present ? std::optional<std::string>("https://as.test/tenant1") : std::nullopt);
        const auto validation = mcp::auth::validate_authorization_response(request, response);
        EXPECT_EQ(validation.status, row.expected) << row.name;
    }
}

TEST(AuthIssuerValidationTest, RejectsAPresentIssuerThatDiffers) {
    for (const bool advertised : {true, false}) {
        const auto request = make_request("https://as.test/tenant1", advertised);
        const auto response = make_response("https://evil.test/tenant1");
        const auto validation = mcp::auth::validate_authorization_response(request, response);
        EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::issuer_mismatch)
            << "advertised=" << advertised;
    }
}

// Comparison is plain string equality. Each pair below is equivalent under RFC 3986 syntax-based
// normalization and must still be rejected.
TEST(AuthIssuerValidationTest, ComparisonIsNotCanonicalizing) {
    struct Pair {
        const char* name;
        const char* recorded;
        const char* returned;
    };
    const Pair pairs[] = {
        {"trailing slash", "https://as.test/tenant1", "https://as.test/tenant1/"},
        {"host case folding", "https://as.test/tenant1", "https://AS.test/tenant1"},
        {"scheme case folding", "https://as.test/tenant1", "HTTPS://as.test/tenant1"},
        {"default port elision", "https://as.test/tenant1", "https://as.test:443/tenant1"},
        {"percent-encoding", "https://as.test/tenant~1", "https://as.test/tenant%7E1"},
    };

    for (const auto& pair : pairs) {
        const auto request = make_request(pair.recorded, true);
        const auto response = make_response(pair.returned);
        const auto validation = mcp::auth::validate_authorization_response(request, response);
        EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::issuer_mismatch)
            << pair.name;
    }
}

TEST(AuthIssuerValidationTest, RejectsErrorResponsesBeforeActingOnTheirErrorValues) {
    auto response = make_response("https://evil.test");
    response.code.reset();
    response.error = "access_denied";
    response.error_description = "attacker supplied text";
    response.error_uri = "https://evil.test/explain";

    const auto validation =
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), response);
    EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::issuer_mismatch);
    EXPECT_EQ(validation.message.find("access_denied"), std::string::npos);
    EXPECT_EQ(validation.message.find("attacker supplied text"), std::string::npos);
}

TEST(AuthIssuerValidationTest, ReportsAServerErrorOnlyWhenTheIssuerIsAuthentic) {
    auto response = make_response("https://as.test");
    response.code.reset();
    response.error = "access_denied";

    const auto validation =
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), response);
    EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::server_error);
}

TEST(AuthIssuerValidationTest, RejectsMissingAndMismatchedState) {
    auto missing = make_response("https://as.test");
    missing.state.reset();
    EXPECT_EQ(mcp::auth::validate_authorization_response(make_request("https://as.test", true), missing)
                  .status,
              mcp::auth::AuthorizationResponseStatus::state_missing);

    auto mismatched = make_response("https://as.test");
    mismatched.state = "other-state";
    EXPECT_EQ(
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), mismatched)
            .status,
        mcp::auth::AuthorizationResponseStatus::state_mismatch);
}

TEST(AuthIssuerValidationTest, RejectsAnAuthenticResponseWithNoCode) {
    auto response = make_response("https://as.test");
    response.code.reset();
    EXPECT_EQ(
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), response)
            .status,
        mcp::auth::AuthorizationResponseStatus::code_missing);
}

TEST(AuthMetadataPolicyTest, DefaultPolicyDeniesEveryOrigin) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/.well-known/x"),
              mcp::auth::MetadataUrlDecision::origin_not_allowed);
}

TEST(AuthMetadataPolicyTest, DenyListWinsOverAllowList) {
    auto policy = allow_origin("https://as.test");
    policy.denied_origins.emplace_back("https://as.test");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
              mcp::auth::MetadataUrlDecision::origin_denied);
}

TEST(AuthMetadataPolicyTest, OriginComparisonIsExact) {
    const auto policy = allow_origin("https://as.test");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
              mcp::auth::MetadataUrlDecision::allowed);
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test:443/prm"),
              mcp::auth::MetadataUrlDecision::origin_not_allowed);
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://sub.as.test/prm"),
              mcp::auth::MetadataUrlDecision::origin_not_allowed);
}

TEST(AuthMetadataPolicyTest, RejectsPlainHttpForNonLoopbackHosts) {
    const auto policy = allow_origin("http://as.test");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://as.test/prm"),
              mcp::auth::MetadataUrlDecision::scheme_not_allowed);
}

TEST(AuthMetadataPolicyTest, LoopbackOptOutIsNeverImplicit) {
    auto policy = allow_origin("http://127.0.0.1:9000");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:9000/prm"),
              mcp::auth::MetadataUrlDecision::scheme_not_allowed);

    policy.allow_plain_http_loopback = true;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:9000/prm"),
              mcp::auth::MetadataUrlDecision::allowed);
}

TEST(AuthMetadataPolicyTest, LoopbackOptOutDoesNotRelaxNonLoopbackHosts) {
    auto policy = allow_origin("http://as.test");
    policy.allow_plain_http_loopback = true;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://as.test/prm"),
              mcp::auth::MetadataUrlDecision::scheme_not_allowed);
}

TEST(AuthMetadataPolicyTest, RejectsUserinfoInTheAuthority) {
    auto policy = allow_origin("https://as.test");
    policy.allowed_origins.emplace_back("https://as.test@169.254.169.254");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test@169.254.169.254/prm"),
              mcp::auth::MetadataUrlDecision::malformed_url);
}

TEST(AuthMetadataPolicyTest, RejectsBlockedAddressesWrittenAsUrlLiterals) {
    struct Case {
        const char* url;
        const char* origin;
        mcp::auth::MetadataUrlDecision expected;
    };
    const Case cases[] = {
        {"https://169.254.169.254/latest/meta-data", "https://169.254.169.254",
         mcp::auth::MetadataUrlDecision::address_link_local},
        {"https://10.0.0.1/prm", "https://10.0.0.1", mcp::auth::MetadataUrlDecision::address_private},
        {"https://172.16.0.1/prm", "https://172.16.0.1",
         mcp::auth::MetadataUrlDecision::address_private},
        {"https://192.168.1.1/prm", "https://192.168.1.1",
         mcp::auth::MetadataUrlDecision::address_private},
        {"https://127.0.0.1/prm", "https://127.0.0.1",
         mcp::auth::MetadataUrlDecision::address_loopback},
        {"https://224.0.0.1/prm", "https://224.0.0.1",
         mcp::auth::MetadataUrlDecision::address_multicast},
        {"https://0.0.0.0/prm", "https://0.0.0.0", mcp::auth::MetadataUrlDecision::address_reserved},
        {"https://100.64.0.1/prm", "https://100.64.0.1",
         mcp::auth::MetadataUrlDecision::address_reserved},
        {"https://[fe80::1]/prm", "https://[fe80::1]",
         mcp::auth::MetadataUrlDecision::address_link_local},
        {"https://[fc00::1]/prm", "https://[fc00::1]", mcp::auth::MetadataUrlDecision::address_private},
        {"https://[::1]/prm", "https://[::1]", mcp::auth::MetadataUrlDecision::address_loopback},
    };

    for (const auto& item : cases) {
        EXPECT_EQ(mcp::auth::validate_metadata_url(allow_origin(item.origin), item.url), item.expected)
            << item.url;
    }
}

TEST(AuthMetadataPolicyTest, ClassifiesIpv4MappedIpv6Addresses) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "::ffff:169.254.169.254"),
              mcp::auth::MetadataUrlDecision::address_link_local);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "::ffff:10.0.0.1"),
              mcp::auth::MetadataUrlDecision::address_private);
}

TEST(AuthMetadataPolicyTest, AllowsRoutablePublicAddresses) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "93.184.216.34"),
              mcp::auth::MetadataUrlDecision::allowed);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "2606:2800:220:1::1"),
              mcp::auth::MetadataUrlDecision::allowed);
}

TEST(AuthMetadataPolicyTest, DescribesEveryDecision) {
    EXPECT_EQ(mcp::auth::describe(mcp::auth::MetadataUrlDecision::allowed), "allowed");
    for (const auto decision : {mcp::auth::MetadataUrlDecision::malformed_url,
                                mcp::auth::MetadataUrlDecision::scheme_not_allowed,
                                mcp::auth::MetadataUrlDecision::origin_denied,
                                mcp::auth::MetadataUrlDecision::origin_not_allowed,
                                mcp::auth::MetadataUrlDecision::address_link_local,
                                mcp::auth::MetadataUrlDecision::address_private,
                                mcp::auth::MetadataUrlDecision::address_loopback,
                                mcp::auth::MetadataUrlDecision::address_multicast,
                                mcp::auth::MetadataUrlDecision::address_reserved,
                                mcp::auth::MetadataUrlDecision::redirect_limit_exceeded,
                                mcp::auth::MetadataUrlDecision::response_too_large}) {
        EXPECT_FALSE(mcp::auth::describe(decision).empty());
        EXPECT_NE(mcp::auth::describe(decision), "allowed");
    }
}

TEST(AuthMetadataPolicyTest, PolicyErrorCarriesItsDecisionAndTarget) {
    const mcp::auth::MetadataPolicyError error(mcp::auth::MetadataUrlDecision::address_link_local,
                                               "169.254.169.254");
    EXPECT_EQ(error.decision(), mcp::auth::MetadataUrlDecision::address_link_local);
    EXPECT_EQ(error.target(), "169.254.169.254");
    EXPECT_NE(std::string(error.what()).find("link-local"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("169.254.169.254"), std::string::npos);
}

TEST(AuthMetadataPolicyTest, ExtractsOriginsWithoutNormalizing) {
    EXPECT_EQ(mcp::auth::metadata_url_origin("https://AS.test:8443/a/b?c=d"), "https://AS.test:8443");
    EXPECT_EQ(mcp::auth::metadata_url_origin("http://127.0.0.1:9000"), "http://127.0.0.1:9000");
    EXPECT_TRUE(mcp::auth::metadata_url_origin("not-a-url").empty());
}
