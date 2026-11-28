#include "restarting_stdio_transport.hpp"

#include <mcp/auth/oauth.hpp>
#include <mcp/client/endpoint.hpp>
#include <mcp/core/async.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/sse_legacy.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

Endpoint::Endpoint(TransportFactory factory) : factory_(std::move(factory)) {}

namespace {

StdioProcessOptions process_for(std::vector<std::string> command) {
    if (command.empty() || command.front().empty()) {
        throw std::invalid_argument("Endpoint::stdio needs a command");
    }
    StdioProcessOptions process;
    process.command = std::move(command.front());
    process.arguments.assign(std::make_move_iterator(command.begin() + 1),
                             std::make_move_iterator(command.end()));
    return process;
}

}  // namespace

Endpoint Endpoint::stdio(std::vector<std::string> command) {
    return stdio(process_for(std::move(command)));
}

Endpoint Endpoint::stdio(std::vector<std::string> command, StdioRestartPolicy restart) {
    return stdio(process_for(std::move(command)), restart);
}

Endpoint Endpoint::stdio(std::initializer_list<std::string> command, StdioRestartPolicy restart) {
    return stdio(std::vector<std::string>(command), restart);
}

Endpoint Endpoint::stdio(StdioProcessOptions process, StdioRestartPolicy restart) {
    if (process.command.empty()) {
        throw std::invalid_argument("Endpoint::stdio needs a command");
    }
    if (restart.max_restarts == 0) {
        return stdio(std::move(process));
    }
    const auto close_wait = process.stdin_close_wait + process.terminate_wait;
    Endpoint endpoint([process = std::move(process), restart](const Executor& executor) {
        return std::make_shared<detail::RestartingStdioTransport>(executor, process, restart);
    });
    endpoint.close_wait_ = close_wait;
    return endpoint;
}

Endpoint Endpoint::stdio(std::initializer_list<std::string> command) {
    return stdio(std::vector<std::string>(command));
}

Endpoint Endpoint::stdio(StdioProcessOptions process) {
    if (process.command.empty()) {
        throw std::invalid_argument("Endpoint::stdio needs a command");
    }
    const auto close_wait = process.stdin_close_wait + process.terminate_wait;
    Endpoint endpoint([process = std::move(process)](const Executor& executor) {
        return std::make_shared<StdioClientTransport>(executor, process);
    });
    endpoint.close_wait_ = close_wait;
    return endpoint;
}

Endpoint Endpoint::http(std::string url, HttpEndpointOptions options) {
    return Endpoint([url = std::move(url), options = std::move(options)](
                        const Executor& executor) -> std::shared_ptr<ITransport> {
        auto transport = std::make_shared<HttpClientTransport>(executor, url);
        if (options.configure) {
            options.configure(*transport);
        }
        if (!options.authenticator) {
            return transport;
        }
        auto authenticator = options.authenticator(executor);
        if (!authenticator) {
            throw std::invalid_argument("HttpEndpointOptions::authenticator returned null");
        }
        return std::make_shared<auth::OAuthClientTransport>(std::move(transport),
                                                            std::move(authenticator));
    });
}

Endpoint Endpoint::legacy_sse(std::string url) {
    return Endpoint([url = std::move(url)](const Executor& executor) -> std::shared_ptr<ITransport> {
        return std::make_shared<LegacySseClientTransport>(executor, url);
    });
}

Endpoint Endpoint::custom(TransportFactory factory) {
    if (!factory) {
        throw std::invalid_argument("Endpoint::custom needs a transport factory");
    }
    return Endpoint(std::move(factory));
}

std::shared_ptr<ITransport> Endpoint::make_transport(const Executor& executor) const {
    auto transport = factory_(executor);
    if (!transport) {
        throw std::invalid_argument("Endpoint transport factory returned null");
    }
    return transport;
}

}  // namespace mcp
