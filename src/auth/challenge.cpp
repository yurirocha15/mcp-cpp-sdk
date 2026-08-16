#include <mcp/auth/challenge.hpp>

#include <mcp/core/constants.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::auth {

namespace {

constexpr std::string_view g_token_specials = "!#$%&'*+-.^_`|~";
constexpr int g_hex_base = 16;

bool is_token_char(unsigned char character) {
    return std::isalnum(character) != 0 ||
           g_token_specials.find(static_cast<char>(character)) != std::string_view::npos;
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) ==
                      std::tolower(static_cast<unsigned char>(rhs));
           });
}

void skip_whitespace(std::string_view text, std::size_t& position) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) {
        ++position;
    }
}

void skip_separators(std::string_view text, std::size_t& position) {
    while (position < text.size() &&
           (text[position] == ' ' || text[position] == '\t' || text[position] == ',')) {
        ++position;
    }
}

std::string read_token(std::string_view text, std::size_t& position) {
    const auto start = position;
    while (position < text.size() && is_token_char(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    return std::string(text.substr(start, position - start));
}

/// Read a quoted-string, expanding backslash escapes. Returns nullopt when unterminated.
std::optional<std::string> read_quoted_string(std::string_view text, std::size_t& position) {
    ++position;
    std::string value;
    while (position < text.size()) {
        const char character = text[position++];
        if (character == '\\') {
            if (position >= text.size()) {
                return std::nullopt;
            }
            value.push_back(text[position++]);
        } else if (character == '"') {
            return value;
        } else {
            value.push_back(character);
        }
    }
    return std::nullopt;
}

/// Record a parameter, keeping the first occurrence of any repeated name.
void assign_parameter(BearerChallenge& challenge, std::string name, std::string value) {
    const auto assign_once = [&value](std::optional<std::string>& field) {
        if (!field) {
            field = value;
        }
    };
    if (name == "realm") {
        assign_once(challenge.realm);
    } else if (name == "resource_metadata") {
        assign_once(challenge.resource_metadata);
    } else if (name == "scope") {
        assign_once(challenge.scope);
    } else if (name == "error") {
        assign_once(challenge.error);
    } else if (name == "error_description") {
        assign_once(challenge.error_description);
    } else if (name == "error_uri") {
        assign_once(challenge.error_uri);
    }
    challenge.parameters.emplace_back(std::move(name), std::move(value));
}

/// Decode one `application/x-www-form-urlencoded` component.
std::string form_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '+') {
            result.push_back(' ');
        } else if (character == '%' && index + 2 < value.size()) {
            const auto high = mcp::constants::g_hex_digits_upper.find(
                static_cast<char>(std::toupper(static_cast<unsigned char>(value[index + 1]))));
            const auto low = mcp::constants::g_hex_digits_upper.find(
                static_cast<char>(std::toupper(static_cast<unsigned char>(value[index + 2]))));
            if (high == std::string_view::npos || low == std::string_view::npos) {
                result.push_back(character);
                continue;
            }
            result.push_back(static_cast<char>((high * g_hex_base) + low));
            index += 2;
        } else {
            result.push_back(character);
        }
    }
    return result;
}

void assign_response_field(AuthorizationResponse& response, const std::string& name,
                           std::string value) {
    const auto assign_once = [&value](std::optional<std::string>& field) {
        if (!field) {
            field = value;
        }
    };
    if (name == "code") {
        assign_once(response.code);
    } else if (name == "state") {
        assign_once(response.state);
    } else if (name == "iss") {
        assign_once(response.iss);
    } else if (name == "error") {
        assign_once(response.error);
    } else if (name == "error_description") {
        assign_once(response.error_description);
    } else if (name == "error_uri") {
        assign_once(response.error_uri);
    }
}

}  // namespace

bool BearerChallenge::is_bearer() const { return equals_ignore_case(scheme, "Bearer"); }

std::vector<BearerChallenge> parse_www_authenticate(std::string_view header_value) {
    std::vector<BearerChallenge> challenges;
    std::optional<BearerChallenge> current;

    const auto flush = [&challenges, &current]() {
        if (current) {
            challenges.push_back(std::move(*current));
            current.reset();
        }
    };

    std::size_t position = 0;
    skip_separators(header_value, position);
    while (position < header_value.size()) {
        auto name = read_token(header_value, position);
        if (name.empty()) {
            break;  // Not a token where one is required; discard the remainder.
        }

        const auto after_name = position;
        skip_whitespace(header_value, position);
        if (position >= header_value.size() || header_value[position] != '=') {
            // A bare token introduces the next challenge rather than a parameter.
            position = after_name;
            flush();
            current.emplace();
            current->scheme = std::move(name);
            skip_separators(header_value, position);
            continue;
        }

        ++position;
        // `token68` credentials may carry `=` padding; that belongs to the preceding scheme.
        auto padding = position;
        while (padding < header_value.size() && header_value[padding] == '=') {
            ++padding;
        }
        auto probe = padding;
        skip_whitespace(header_value, probe);
        if (probe >= header_value.size() || header_value[probe] == ',') {
            position = probe;
            skip_separators(header_value, position);
            continue;
        }

        skip_whitespace(header_value, position);
        std::string value;
        if (header_value[position] == '"') {
            auto quoted = read_quoted_string(header_value, position);
            if (!quoted) {
                break;  // Unterminated quoted-string; nothing after it can be trusted.
            }
            value = std::move(*quoted);
        } else {
            value = read_token(header_value, position);
        }

        if (current) {
            assign_parameter(*current, to_lower_ascii(std::move(name)), std::move(value));
        }
        skip_separators(header_value, position);
    }

    flush();
    return challenges;
}

std::vector<BearerChallenge> parse_www_authenticate(const std::vector<std::string>& header_values) {
    std::vector<BearerChallenge> challenges;
    for (const auto& header_value : header_values) {
        auto parsed = parse_www_authenticate(header_value);
        challenges.insert(challenges.end(), std::make_move_iterator(parsed.begin()),
                          std::make_move_iterator(parsed.end()));
    }
    return challenges;
}

std::optional<BearerChallenge> select_bearer_challenge(const std::vector<BearerChallenge>& challenges) {
    const auto match =
        std::find_if(challenges.begin(), challenges.end(),
                     [](const BearerChallenge& challenge) { return challenge.is_bearer(); });
    if (match == challenges.end()) {
        return std::nullopt;
    }
    return *match;
}

AuthorizationResponse parse_authorization_response(const std::string& redirect_url) {
    AuthorizationResponse response;

    auto query = std::string_view(redirect_url);
    if (const auto question = query.find('?'); question != std::string_view::npos) {
        query.remove_prefix(question + 1);
    }
    if (const auto fragment = query.find('#'); fragment != std::string_view::npos) {
        query = query.substr(0, fragment);
    }

    while (!query.empty()) {
        auto field = query;
        if (const auto separator = query.find('&'); separator != std::string_view::npos) {
            field = query.substr(0, separator);
            query.remove_prefix(separator + 1);
        } else {
            query = {};
        }

        const auto equals = field.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        assign_response_field(response, to_lower_ascii(form_decode(field.substr(0, equals))),
                              form_decode(field.substr(equals + 1)));
    }
    return response;
}

AuthorizationResponseValidation validate_authorization_response(const AuthorizationRequest& request,
                                                                const AuthorizationResponse& response) {
    if (!request.state.empty()) {
        if (!response.state) {
            return {AuthorizationResponseStatus::state_missing,
                    "authorization response omitted the state parameter"};
        }
        if (*response.state != request.state) {
            return {AuthorizationResponseStatus::state_mismatch,
                    "authorization response state did not match the recorded value"};
        }
    }

    // RFC 9207 Section 2.4 as adopted by MCP. Plain string comparison only: no case folding, no
    // default-port elision, no trailing-slash or percent-encoding normalization.
    if (response.iss) {
        if (*response.iss != request.issuer) {
            return {AuthorizationResponseStatus::issuer_mismatch,
                    "authorization response iss did not match the recorded issuer"};
        }
    } else if (request.issuer_parameter_supported) {
        return {AuthorizationResponseStatus::issuer_missing,
                "authorization server advertises iss support but omitted the parameter"};
    }

    // Only an issuer-authentic response may have its error values acted on or displayed.
    if (response.error) {
        return {AuthorizationResponseStatus::server_error,
                "authorization server returned error " + *response.error};
    }
    if (!response.code || response.code->empty()) {
        return {AuthorizationResponseStatus::code_missing,
                "authorization response carried no authorization code"};
    }
    return {AuthorizationResponseStatus::accepted, {}};
}

}  // namespace mcp::auth
