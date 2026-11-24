/**
 * @file auth_challenge_test.cpp
 * @brief Tests for WWW-Authenticate challenge parsing, RFC 9207 issuer validation, and the
 *        outbound-request policy that guards OAuth metadata discovery.
 */

#include <gtest/gtest.h>

#include <mcp/auth/challenge.hpp>
#include <mcp/auth/metadata_policy.hpp>
#include <string>
#include <utility>
#include <vector>

namespace auth_challenge_test {

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

TEST(AuthChallengeParserTest, ReadsErrorUriAndStopsWhereATokenIsMissing) {
    const auto challenges = mcp::auth::parse_www_authenticate(
        R"(Bearer error="invalid_token", error_uri="https://as.test/errors/1", =bad, scope="s")");
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].error, "invalid_token");
    EXPECT_EQ(challenges[0].error_uri, "https://as.test/errors/1");
    // Where a parameter name is required and none is written, the rest of the header is unreadable.
    EXPECT_FALSE(challenges[0].scope.has_value());
    EXPECT_EQ(challenges[0].parameters.size(), 2U);
}

TEST(AuthChallengeParserTest, AQuotedStringEndingInABackslashIsUnterminated) {
    const auto challenges = mcp::auth::parse_www_authenticate("Bearer realm=\"r\\");
    ASSERT_EQ(challenges.size(), 1U);
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

// A '%' that does not start two hex digits is kept as written, and a field without '=' carries no
// value, so neither hides the fields around it.
TEST(AuthAuthorizationResponseParseTest, KeepsMalformedEscapesAndSkipsFieldsWithoutAValue) {
    const auto response = mcp::auth::parse_authorization_response(
        "http://127.0.0.1/cb?code=50%zz%4&iss&state=s1&error_uri=https%3A%2F%2Fas.test%2Fe");
    EXPECT_EQ(response.code, "50%zz%4");
    EXPECT_FALSE(response.iss.has_value());
    EXPECT_EQ(response.state, "s1");
    EXPECT_EQ(response.error_uri, "https://as.test/e");
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
         mcp::auth::AuthorizationResponseStatus::eAccepted},
        {"advertised and absent rejects", true, false,
         mcp::auth::AuthorizationResponseStatus::eIssuerMissing},
        {"not advertised but present still compares", false, true,
         mcp::auth::AuthorizationResponseStatus::eAccepted},
        {"not advertised and absent proceeds", false, false,
         mcp::auth::AuthorizationResponseStatus::eAccepted},
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
        EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::eIssuerMismatch)
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
        EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::eIssuerMismatch)
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
    EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::eIssuerMismatch);
    EXPECT_EQ(validation.message.find("access_denied"), std::string::npos);
    EXPECT_EQ(validation.message.find("attacker supplied text"), std::string::npos);
}

TEST(AuthIssuerValidationTest, ReportsAServerErrorOnlyWhenTheIssuerIsAuthentic) {
    auto response = make_response("https://as.test");
    response.code.reset();
    response.error = "access_denied";

    const auto validation =
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), response);
    EXPECT_EQ(validation.status, mcp::auth::AuthorizationResponseStatus::eServerError);
}

TEST(AuthIssuerValidationTest, RejectsMissingAndMismatchedState) {
    auto missing = make_response("https://as.test");
    missing.state.reset();
    EXPECT_EQ(mcp::auth::validate_authorization_response(make_request("https://as.test", true), missing)
                  .status,
              mcp::auth::AuthorizationResponseStatus::eStateMissing);

    auto mismatched = make_response("https://as.test");
    mismatched.state = "other-state";
    EXPECT_EQ(
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), mismatched)
            .status,
        mcp::auth::AuthorizationResponseStatus::eStateMismatch);
}

TEST(AuthIssuerValidationTest, RejectsAnAuthenticResponseWithNoCode) {
    auto response = make_response("https://as.test");
    response.code.reset();
    EXPECT_EQ(
        mcp::auth::validate_authorization_response(make_request("https://as.test", true), response)
            .status,
        mcp::auth::AuthorizationResponseStatus::eCodeMissing);
}

TEST(AuthMetadataPolicyTest, DefaultPolicyDeniesEveryOrigin) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/.well-known/x"),
              mcp::auth::MetadataUrlDecision::eOriginNotAllowed);
}

TEST(AuthMetadataPolicyTest, DenyListWinsOverAllowList) {
    auto policy = allow_origin("https://as.test");
    policy.denied_origins.emplace_back("https://as.test");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
              mcp::auth::MetadataUrlDecision::eOriginDenied);
}

TEST(AuthMetadataPolicyTest, OriginComparisonIgnoresExplicitDefaultPort) {
    const auto policy = allow_origin("https://as.test");
    // The deny list is empty, the common case, and that is not a policy error.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
    // An explicit default port canonicalizes to the same origin as no port at all.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test:443/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
    // A subdomain is a genuinely different origin and must stay refused.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://sub.as.test/prm"),
              mcp::auth::MetadataUrlDecision::eOriginNotAllowed);
    // A non-default port is preserved and still distinguishes origins.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test:8443/prm"),
              mcp::auth::MetadataUrlDecision::eOriginNotAllowed);
}

// Each spelling of a denied origin canonicalizes to it: scheme and host case, an explicit default
// port with or without leading zeros, and trailing dots on the host.
TEST(AuthMetadataPolicyTest, DenyListCanonicalizesEverySpellingOfTheOrigin) {
    auto policy = allow_origin("https://evil.example");
    policy.denied_origins.emplace_back("https://evil.example");
    for (const char* url :
         {"HTTPS://Evil.example/prm", "https://evil.example:443/prm", "https://evil.example:00443/prm",
          "https://evil.example:0443/prm", "https://evil.example./prm", "https://evil.example../prm"}) {
        EXPECT_EQ(mcp::auth::validate_metadata_url(policy, url),
                  mcp::auth::MetadataUrlDecision::eOriginDenied)
            << url;
    }
}

TEST(AuthMetadataPolicyTest, AllowListEntryCaseDoesNotHaveToMatchTheUrl) {
    const auto policy = allow_origin("HTTPS://AS.TEST");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, OriginAllowanceCallbackReceivesTheCanonicalOrigin) {
    mcp::auth::MetadataFetchPolicy policy;
    std::string observed;
    policy.origin_allowance = [&observed](const std::string& origin) {
        observed = origin;
        return true;
    };
    // The scheme is kept lowercase here because the separate https-required check further down is
    // deliberately byte-exact; only the host case, the explicit default port and the origin passed to
    // the callback are under test.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://Evil.example:443/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
    EXPECT_EQ(observed, "https://evil.example");
}

TEST(AuthMetadataPolicyTest, IpLiteralAndIpv6OriginsAreUnaffectedByCanonicalization) {
    // An IP literal has no FQDN trailing dot to strip, and dotted decimal notation is not affected
    // by lowercasing.
    const auto v4_policy = allow_origin("https://93.184.216.34");
    EXPECT_EQ(mcp::auth::validate_metadata_url(v4_policy, "https://93.184.216.34/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);

    // An IPv6 bracketed literal keeps its brackets; only the hex digits are lowercased.
    auto v6_policy = allow_origin("https://[2606:2800:220:1::1]");
    EXPECT_EQ(mcp::auth::validate_metadata_url(v6_policy, "https://[2606:2800:220:1::1]/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
    EXPECT_EQ(mcp::auth::validate_metadata_url(v6_policy, "https://[2606:2800:220:1::1]:443/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);

    v6_policy.denied_origins.emplace_back("https://[2606:2800:220:1::1]");
    EXPECT_EQ(mcp::auth::validate_metadata_url(v6_policy, "https://[2606:2800:220:1::1]/prm"),
              mcp::auth::MetadataUrlDecision::eOriginDenied);
}

TEST(AuthMetadataPolicyTest, NonDefaultPortCanonicalizesNumericallyDespiteLeadingZeros) {
    const auto policy = allow_origin("https://evil.example:8443");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://evil.example:08443/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, RejectsAPortThatIsNotAPlainDecimalNumber) {
    const auto policy = allow_origin("https://evil.example");
    const char* urls[] = {
        "https://evil.example:+443/prm",
        "https://evil.example: 443/prm",
        "https://evil.example:44a3/prm",
        "https://evil.example:/prm",
    };
    for (const auto* url : urls) {
        EXPECT_EQ(mcp::auth::validate_metadata_url(policy, url),
                  mcp::auth::MetadataUrlDecision::eMalformedUrl)
            << url;
    }
}

TEST(AuthMetadataPolicyTest, DenyListMatchesAnAlternateIpv6TextualForm) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.denied_origins.emplace_back("https://[2606:2800:220:1::1]");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy,
                                               "https://[2606:2800:0220:0001:0000:0000:0000:0001]/prm"),
              mcp::auth::MetadataUrlDecision::eOriginDenied);
}

// An allow entry that is not a bare origin grants nothing, and says so by returning a decision
// rather than throwing: dropping it is fail-closed, so the quiet refusal is the safe outcome and
// stays the documented one. The deny list, where dropping an entry would be fail-open, is loud
// instead; see the deny-list tests below.
TEST(AuthMetadataPolicyTest, AllowListEntryThatIsNotABareOriginStillFailsClosedQuietly) {
    const char* entries[] = {
        "https://as.test/",     "https://as.test/realms/foo", "https://as.test?tenant=1",
        "https://as.test#frag", "https://as.test:44a3",
    };
    for (const auto* entry : entries) {
        const auto policy = allow_origin(entry);
        mcp::auth::MetadataUrlDecision decision{};
        EXPECT_NO_THROW(decision = mcp::auth::validate_metadata_url(policy, "https://as.test/prm"))
            << entry;
        EXPECT_EQ(decision, mcp::auth::MetadataUrlDecision::eOriginNotAllowed) << entry;
    }
}

// A deny entry that is not a bare origin, even one that differs only by a trailing slash, must not
// be discarded in silence, which would admit the origin the author meant to block. It is refused
// as a policy error instead.
TEST(AuthMetadataPolicyTest, DenyListEntryWithAPathQueryOrFragmentIsRejected) {
    const char* entries[] = {
        "https://evil.example/",          "https://evil.example/realms/foo",
        "https://evil.example/?tenant=1", "https://evil.example?tenant=1",
        "https://evil.example#frag",      "https://evil.example:44a3",
    };
    for (const auto* entry : entries) {
        auto policy = allow_origin("https://evil.example");
        policy.denied_origins.emplace_back(entry);
        try {
            const auto decision = mcp::auth::validate_metadata_url(policy, "https://evil.example/prm");
            ADD_FAILURE() << entry << " was admitted with decision " << static_cast<int>(decision);
        } catch (const mcp::auth::MetadataPolicyError& error) {
            EXPECT_EQ(error.decision(), mcp::auth::MetadataUrlDecision::eDeniedOriginEntryMalformed)
                << entry;
            EXPECT_EQ(error.target(), entry);
        }
    }
}

// The refusal is a property of the policy, not of the URL: a malformed deny entry refuses every
// target, including one that no entry in the list was ever meant to describe.
TEST(AuthMetadataPolicyTest, MalformedDenyEntryRefusesAnUnrelatedTargetToo) {
    auto policy = allow_origin("https://as.test");
    policy.denied_origins.emplace_back("https://evil.example/");
    EXPECT_THROW((void)mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
                 mcp::auth::MetadataPolicyError);
}

// A malformed entry is reported wherever it sits in the list, even behind an entry that already
// matched, so a single bad rule cannot hide behind a working one.
TEST(AuthMetadataPolicyTest, MalformedDenyEntryIsReportedEvenAfterAMatchingEntry) {
    auto policy = allow_origin("https://evil.example");
    policy.denied_origins.emplace_back("https://evil.example");
    policy.denied_origins.emplace_back("https://other.example/");
    try {
        (void)mcp::auth::validate_metadata_url(policy, "https://evil.example/prm");
        FAIL() << "expected MetadataPolicyError";
    } catch (const mcp::auth::MetadataPolicyError& error) {
        EXPECT_EQ(error.decision(), mcp::auth::MetadataUrlDecision::eDeniedOriginEntryMalformed);
        EXPECT_EQ(error.target(), "https://other.example/");
    }
}

// A malformed entry must not turn every deny list into an error: a well-formed list still denies
// exactly what it names, and still admits everything else the allow list permits.
TEST(AuthMetadataPolicyTest, WellFormedDenyListStillDeniesAndStillAdmits) {
    auto policy = allow_origin("https://as.test");
    policy.allowed_origins.emplace_back("https://evil.example");
    policy.denied_origins.emplace_back("https://evil.example");
    policy.denied_origins.emplace_back("https://[2606:2800:220:1::1]:8443");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://evil.example/prm"),
              mcp::auth::MetadataUrlDecision::eOriginDenied);
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, LoopbackOptOutIsCaseInsensitiveOnSchemeAndHost) {
    auto policy = allow_origin("http://localhost:9000");
    policy.allow_plain_http_loopback = true;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "HTTP://localhost:9000/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://LOCALHOST:9000/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, RejectsAHostThatIsNothingButDots) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://.../prm"),
              mcp::auth::MetadataUrlDecision::eMalformedUrl);
}

TEST(AuthMetadataPolicyTest, RejectsPlainHttpForNonLoopbackHosts) {
    const auto policy = allow_origin("http://as.test");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://as.test/prm"),
              mcp::auth::MetadataUrlDecision::eSchemeNotAllowed);
}

TEST(AuthMetadataPolicyTest, LoopbackOptOutIsNeverImplicit) {
    auto policy = allow_origin("http://127.0.0.1:9000");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:9000/prm"),
              mcp::auth::MetadataUrlDecision::eSchemeNotAllowed);

    policy.allow_plain_http_loopback = true;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:9000/prm"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, LoopbackOptOutDoesNotRelaxNonLoopbackHosts) {
    auto policy = allow_origin("http://as.test");
    policy.allow_plain_http_loopback = true;
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://as.test/prm"),
              mcp::auth::MetadataUrlDecision::eSchemeNotAllowed);
}

TEST(AuthMetadataPolicyTest, RejectsUserinfoInTheAuthority) {
    auto policy = allow_origin("https://as.test");
    policy.allowed_origins.emplace_back("https://as.test@169.254.169.254");
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://as.test@169.254.169.254/prm"),
              mcp::auth::MetadataUrlDecision::eMalformedUrl);
}

TEST(AuthMetadataPolicyTest, RejectsBlockedAddressesWrittenAsUrlLiterals) {
    struct Case {
        const char* url;
        const char* origin;
        mcp::auth::MetadataUrlDecision expected;
    };
    const Case cases[] = {
        {"https://169.254.169.254/latest/meta-data", "https://169.254.169.254",
         mcp::auth::MetadataUrlDecision::eAddressLinkLocal},
        {"https://10.0.0.1/prm", "https://10.0.0.1", mcp::auth::MetadataUrlDecision::eAddressPrivate},
        {"https://172.16.0.1/prm", "https://172.16.0.1",
         mcp::auth::MetadataUrlDecision::eAddressPrivate},
        {"https://192.168.1.1/prm", "https://192.168.1.1",
         mcp::auth::MetadataUrlDecision::eAddressPrivate},
        {"https://127.0.0.1/prm", "https://127.0.0.1",
         mcp::auth::MetadataUrlDecision::eAddressLoopback},
        {"https://224.0.0.1/prm", "https://224.0.0.1",
         mcp::auth::MetadataUrlDecision::eAddressMulticast},
        {"https://0.0.0.0/prm", "https://0.0.0.0", mcp::auth::MetadataUrlDecision::eAddressReserved},
        {"https://100.64.0.1/prm", "https://100.64.0.1",
         mcp::auth::MetadataUrlDecision::eAddressReserved},
        {"https://[fe80::1]/prm", "https://[fe80::1]",
         mcp::auth::MetadataUrlDecision::eAddressLinkLocal},
        {"https://[fc00::1]/prm", "https://[fc00::1]", mcp::auth::MetadataUrlDecision::eAddressPrivate},
        {"https://[::1]/prm", "https://[::1]", mcp::auth::MetadataUrlDecision::eAddressLoopback},
    };

    for (const auto& item : cases) {
        EXPECT_EQ(mcp::auth::validate_metadata_url(allow_origin(item.origin), item.url), item.expected)
            << item.url;
    }
}

// The special-purpose ranges of RFC 6890 that no metadata document lives at are refused, and the
// addresses just outside each range are not.
TEST(AuthMetadataPolicyTest, RefusesSpecialPurposeRangesUpToTheirEdges) {
    using Decision = mcp::auth::MetadataUrlDecision;
    const std::vector<std::pair<const char*, Decision>> cases = {
        {"255.255.255.255", Decision::eAddressMulticast},
        {"240.0.0.1", Decision::eAddressReserved},
        {"192.0.0.8", Decision::eAddressReserved},
        {"192.0.2.1", Decision::eAddressReserved},
        {"198.18.0.1", Decision::eAddressReserved},
        {"198.19.255.255", Decision::eAddressReserved},
        {"198.51.100.7", Decision::eAddressReserved},
        {"203.0.113.9", Decision::eAddressReserved},
        {"100.127.255.255", Decision::eAddressReserved},
        {"172.31.255.255", Decision::eAddressPrivate},
        {"::", Decision::eAddressReserved},
        {"ff02::1", Decision::eAddressMulticast},
        {"fec0::1", Decision::eAddressPrivate},
        {"fd12:3456::1", Decision::eAddressPrivate},
        {"192.0.1.1", Decision::eAllowed},
        {"192.0.3.1", Decision::eAllowed},
        {"198.17.255.255", Decision::eAllowed},
        {"198.20.0.1", Decision::eAllowed},
        {"198.51.101.1", Decision::eAllowed},
        {"203.0.114.1", Decision::eAllowed},
        {"100.128.0.1", Decision::eAllowed},
        {"172.32.0.1", Decision::eAllowed},
        {"223.255.255.255", Decision::eAllowed},
        {"100.64.0.0", Decision::eAddressReserved},
        {"100.63.255.255", Decision::eAllowed},
        {"172.16.0.0", Decision::eAddressPrivate},
        {"172.15.255.255", Decision::eAllowed},
        {"192.168.0.1", Decision::eAddressPrivate},
        {"192.169.0.1", Decision::eAllowed},
        {"169.254.0.1", Decision::eAddressLinkLocal},
        {"169.253.255.255", Decision::eAllowed},
        {"192.1.0.1", Decision::eAllowed},
        {"198.51.99.1", Decision::eAllowed},
        {"203.1.113.1", Decision::eAllowed},
        {"0.0.0.0", Decision::eAddressReserved},
        {"1.0.0.1", Decision::eAllowed},
    };
    const mcp::auth::MetadataFetchPolicy policy;
    for (const auto& [address, expected] : cases) {
        EXPECT_EQ(mcp::auth::validate_metadata_address(policy, address), expected) << address;
    }
}

TEST(AuthMetadataPolicyTest, ClassifiesIpv4MappedIpv6Addresses) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "::ffff:169.254.169.254"),
              mcp::auth::MetadataUrlDecision::eAddressLinkLocal);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "::ffff:10.0.0.1"),
              mcp::auth::MetadataUrlDecision::eAddressPrivate);
}

TEST(AuthMetadataPolicyTest, ClassifiesTheIpv4AddressEmbeddedInNat64And6to4Addresses) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allow_plain_http_loopback = true;
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "64:ff9b::10.0.0.1"),
              mcp::auth::MetadataUrlDecision::eAddressPrivate);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "64:ff9b::a9fe:a9fe"),
              mcp::auth::MetadataUrlDecision::eAddressLinkLocal);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "2002:c0a8:0101::1"),
              mcp::auth::MetadataUrlDecision::eAddressPrivate);
    // An embedded loopback address is not the local host, so the loopback opt-out does not apply.
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "2002:7f00:0001::1"),
              mcp::auth::MetadataUrlDecision::eAddressLoopback);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "64:ff9b::5db8:d822"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, AllowsRoutablePublicAddresses) {
    const mcp::auth::MetadataFetchPolicy policy;
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "93.184.216.34"),
              mcp::auth::MetadataUrlDecision::eAllowed);
    EXPECT_EQ(mcp::auth::validate_metadata_address(policy, "2606:2800:220:1::1"),
              mcp::auth::MetadataUrlDecision::eAllowed);
}

TEST(AuthMetadataPolicyTest, DescribesEveryDecision) {
    EXPECT_EQ(mcp::auth::describe(mcp::auth::MetadataUrlDecision::eAllowed), "allowed");
    for (const auto decision : {mcp::auth::MetadataUrlDecision::eMalformedUrl,
                                mcp::auth::MetadataUrlDecision::eSchemeNotAllowed,
                                mcp::auth::MetadataUrlDecision::eOriginDenied,
                                mcp::auth::MetadataUrlDecision::eOriginNotAllowed,
                                mcp::auth::MetadataUrlDecision::eAddressLinkLocal,
                                mcp::auth::MetadataUrlDecision::eAddressPrivate,
                                mcp::auth::MetadataUrlDecision::eAddressLoopback,
                                mcp::auth::MetadataUrlDecision::eAddressMulticast,
                                mcp::auth::MetadataUrlDecision::eAddressReserved,
                                mcp::auth::MetadataUrlDecision::eRedirectLimitExceeded,
                                mcp::auth::MetadataUrlDecision::eResponseTooLarge}) {
        EXPECT_FALSE(mcp::auth::describe(decision).empty());
        EXPECT_NE(mcp::auth::describe(decision), "allowed");
    }
}

TEST(AuthMetadataPolicyTest, PolicyErrorCarriesItsDecisionAndTarget) {
    const mcp::auth::MetadataPolicyError error(mcp::auth::MetadataUrlDecision::eAddressLinkLocal,
                                               "169.254.169.254");
    EXPECT_EQ(error.decision(), mcp::auth::MetadataUrlDecision::eAddressLinkLocal);
    EXPECT_EQ(error.target(), "169.254.169.254");
    EXPECT_NE(std::string(error.what()).find("link-local"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("169.254.169.254"), std::string::npos);
}

TEST(AuthMetadataPolicyTest, ExtractsOriginsWithoutNormalizing) {
    EXPECT_EQ(mcp::auth::metadata_url_origin("https://AS.test:8443/a/b?c=d"), "https://AS.test:8443");
    EXPECT_EQ(mcp::auth::metadata_url_origin("http://127.0.0.1:9000"), "http://127.0.0.1:9000");
    EXPECT_TRUE(mcp::auth::metadata_url_origin("not-a-url").empty());
}

}  // namespace auth_challenge_test
