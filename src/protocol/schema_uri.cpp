#include "mcp/detail/protocol/schema_uri.hpp"

#include <cstddef>
#include <utility>

namespace mcp::detail {

namespace {

/// RFC 3986 section 5.2.4.
std::string remove_dot_segments(std::string_view input) {
    std::string output;
    while (!input.empty()) {
        if (input.substr(0, 3) == "../") {
            input.remove_prefix(3);
        } else if (input.substr(0, 2) == "./") {
            input.remove_prefix(2);
        } else if (input.substr(0, 3) == "/./") {
            input.remove_prefix(2);
        } else if (input == "/.") {
            input = "/";
        } else if (input.substr(0, 4) == "/../" || input == "/..") {
            if (input == "/..") {
                input = "/";
            } else {
                input.remove_prefix(3);
            }
            const auto last = output.rfind('/');
            output.erase(last == std::string::npos ? 0 : last);
        } else if (input == "." || input == "..") {
            input = {};
        } else {
            const auto start = input.front() == '/' ? 1 : 0;
            const auto end = input.find('/', start);
            const auto segment = input.substr(0, end);
            output.append(segment);
            input.remove_prefix(segment.size());
        }
    }
    return output;
}

/// RFC 3986 section 5.2.3.
std::string merge_paths(const UriReference& base, const std::string& reference_path) {
    if (base.authority && base.path.empty()) {
        return "/" + reference_path;
    }
    const auto last = base.path.rfind('/');
    if (last == std::string::npos) {
        return reference_path;
    }
    return base.path.substr(0, last + 1) + reference_path;
}

}  // namespace

std::string UriReference::text() const {
    std::string result;
    if (scheme) {
        result += *scheme;
        result += ':';
    }
    if (authority) {
        result += "//";
        result += *authority;
    }
    result += path;
    if (query) {
        result += '?';
        result += *query;
    }
    if (fragment) {
        result += '#';
        result += *fragment;
    }
    return result;
}

UriReference parse_uri_reference(std::string_view text) {
    UriReference reference;
    const auto hash = text.find('#');
    if (hash != std::string_view::npos) {
        reference.fragment = std::string(text.substr(hash + 1));
        text = text.substr(0, hash);
    }
    const auto question = text.find('?');
    if (question != std::string_view::npos) {
        reference.query = std::string(text.substr(question + 1));
        text = text.substr(0, question);
    }
    // A scheme is letters, digits, '+', '-' and '.' starting with a letter, before the first ':'
    // and before any '/'.
    const auto colon = text.find(':');
    if (colon != std::string_view::npos && colon > 0 &&
        text.substr(0, colon).find('/') == std::string_view::npos) {
        const auto candidate = text.substr(0, colon);
        bool valid = (candidate.front() >= 'a' && candidate.front() <= 'z') ||
                     (candidate.front() >= 'A' && candidate.front() <= 'Z');
        for (const char character : candidate) {
            const bool letter =
                (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            valid =
                valid && (letter || digit || character == '+' || character == '-' || character == '.');
        }
        if (valid) {
            reference.scheme = std::string(candidate);
            text.remove_prefix(colon + 1);
        }
    }
    if (text.substr(0, 2) == "//") {
        text.remove_prefix(2);
        const auto slash = text.find('/');
        reference.authority = std::string(text.substr(0, slash));
        text = slash == std::string_view::npos ? std::string_view{} : text.substr(slash);
    }
    reference.path = std::string(text);
    return reference;
}

std::string resolve_uri(std::string_view base_text, std::string_view reference_text) {
    const auto reference = parse_uri_reference(reference_text);
    const auto base = parse_uri_reference(base_text);
    UriReference target;
    if (reference.scheme) {
        target = reference;
        target.path = remove_dot_segments(reference.path);
    } else {
        if (reference.authority) {
            target.authority = reference.authority;
            target.path = remove_dot_segments(reference.path);
            target.query = reference.query;
        } else {
            if (reference.path.empty()) {
                target.path = base.path;
                target.query = reference.query ? reference.query : base.query;
            } else {
                if (reference.path.front() == '/') {
                    target.path = remove_dot_segments(reference.path);
                } else {
                    target.path = remove_dot_segments(merge_paths(base, reference.path));
                }
                target.query = reference.query;
            }
            target.authority = base.authority;
        }
        target.scheme = base.scheme;
    }
    target.fragment = reference.fragment;
    return target.text();
}

std::pair<std::string, std::string> split_fragment(std::string_view uri) {
    const auto hash = uri.find('#');
    if (hash == std::string_view::npos) {
        return {std::string(uri), std::string()};
    }
    return {std::string(uri.substr(0, hash)), std::string(uri.substr(hash + 1))};
}

std::optional<std::vector<std::string>> json_pointer_tokens(std::string_view pointer) {
    std::vector<std::string> tokens;
    if (pointer.empty()) {
        return tokens;
    }
    if (pointer.front() != '/') {
        return std::nullopt;
    }
    pointer.remove_prefix(1);
    while (true) {
        const auto slash = pointer.find('/');
        const auto raw = pointer.substr(0, slash);
        std::string token;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '~') {
                token += raw[i];
            } else if (i + 1 < raw.size() && raw[i + 1] == '0') {
                token += '~';
                ++i;
            } else if (i + 1 < raw.size() && raw[i + 1] == '1') {
                token += '/';
                ++i;
            } else {
                return std::nullopt;
            }
        }
        tokens.push_back(std::move(token));
        if (slash == std::string_view::npos) {
            return tokens;
        }
        pointer.remove_prefix(slash + 1);
    }
}

std::string escape_pointer_token(std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char character : token) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

}  // namespace mcp::detail
