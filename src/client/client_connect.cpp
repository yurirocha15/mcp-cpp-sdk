/// @file client_connect.cpp
/// @brief How Client::Impl connects: the protocol revision it settles on with server/discover or
/// initialize, and what it then sends with each request.

#include <algorithm>

#include "client_impl.hpp"

namespace mcp {

bool Client::Impl::is_modern_error_code(int code) {
    return code == mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION ||
           code == mcp::constants::jsonrpc::g_HEADER_MISMATCH ||
           code == mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY;
}

Client::Impl::ProbePtr Client::Impl::classify_probe_error(const std::shared_ptr<Impl>& state,
                                                          const Error& error) {
    auto outcome = std::make_shared<ProbeOutcome>();
    outcome->error = error;
    if (state->closed.load(std::memory_order_acquire)) {
        outcome->kind = ProbeOutcome::Kind::eClosed;
    } else if (is_modern_error_code(error.code)) {
        outcome->kind = ProbeOutcome::Kind::eModernError;
    } else if (error.code == mcp::constants::jsonrpc::g_REQUEST_TIMEOUT) {
        outcome->kind = ProbeOutcome::Kind::eTimeout;
    }
    return outcome;
}

Client::Impl::ProbePtr Client::Impl::classify_probe_reply(const nlohmann::json& reply) {
    auto outcome = std::make_shared<ProbeOutcome>();
    try {
        auto discover = reply;
        if (discover.is_object() && !discover.contains("resultType")) {
            discover["resultType"] = "complete";
        }
        auto parsed = discover.get<DiscoverResult>();
        if (parsed.resultType == "complete") {
            outcome->kind = ProbeOutcome::Kind::eDiscover;
            outcome->discover = std::move(parsed);
        }
    } catch (const std::exception&) {
        // Not a DiscoverResult: whatever answered is not a modern server.
    }
    return outcome;
}

Task<Client::Impl::ProbePtr> Client::Impl::probe(std::shared_ptr<Impl> state, VersionPtr version,
                                                 std::chrono::milliseconds timeout) {
    ProbePtr outcome;
    nlohmann::json reply;
    bool answered = false;
    try {
        RequestOptions options;
        options.timeout = timeout;
        auto params = state->with_request_meta(*version, std::nullopt, std::nullopt);
        auto pending = request_wire(state, methods::g_SERVER_DISCOVER, params, options, true);
        reply = co_await std::move(pending);
        answered = true;
    } catch (const McpError& error) {
        outcome = classify_probe_error(state, error.error());
    } catch (...) {
        // The write failed: an HTTP server refusing the POST, or the connection going away.
        outcome = std::make_shared<ProbeOutcome>();
        if (state->closed.load(std::memory_order_acquire)) {
            outcome->kind = ProbeOutcome::Kind::eClosed;
        }
    }
    if (answered) {
        outcome = classify_probe_reply(reply);
    }
    co_return outcome;
}

std::vector<std::string> Client::Impl::advertised_versions(const Error& error) {
    std::vector<std::string> versions;
    if (error.data && error.data->is_object()) {
        const auto found = error.data->find("supported");
        if (found != error.data->end() && found->is_array()) {
            for (const auto& version : *found) {
                if (version.is_string()) {
                    versions.push_back(version.get<std::string>());
                }
            }
        }
    }
    return versions;
}

bool Client::Impl::lists_modern_version(const Error& error) {
    const auto versions = advertised_versions(error);
    return std::ranges::any_of(
        versions, [](const std::string& version) { return is_modern_protocol_version(version); });
}

void Client::Impl::report_era(ProtocolEra era, EraReason reason,
                              std::chrono::steady_clock::time_point started) const {
    if (!options.on_era_decision) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    try {
        options.on_era_decision(EraDecision{.era = era, .reason = reason, .elapsed = elapsed});
    } catch (...) {
        // Documented: the hook observes; a failure of its own has no listener.
    }
}

std::optional<nlohmann::json> Client::Impl::initialize_params(const std::shared_ptr<Impl>& state,
                                                              const std::string& version) {
    InitializeRequest initialize_request;
    initialize_request.protocolVersion = version;
    initialize_request.capabilities = state->effective_capabilities();
    std::scoped_lock lock(state->handlers_mutex);
    initialize_request.clientInfo = state->client_info;
    return nlohmann::json(std::move(initialize_request));
}

Task<Client::Impl::AttemptPtr> Client::Impl::attempt_initialize(std::shared_ptr<Impl> state,
                                                                VersionPtr version,
                                                                VersionsPtr advertised) {
    auto attempt = std::make_shared<InitializeAttempt>();
    nlohmann::json result_json;
    bool answered = false;
    try {
        auto params = initialize_params(state, *version);
        auto pending = request(state, methods::g_INITIALIZE, params, {});
        result_json = co_await std::move(pending);
        answered = true;
    } catch (const McpError& error) {
        // A transport failure, which throw_transport_failure() nests, is no answer from the
        // server, and goes to the caller whole.
        if (state->closed.load(std::memory_order_acquire) ||
            error.code() == mcp::constants::jsonrpc::g_REQUEST_TIMEOUT ||
            dynamic_cast<const std::nested_exception*>(&error) != nullptr) {
            throw;
        }
        attempt->error = error.error();
    }
    if (!answered) {
        co_return attempt;
    }

    auto initialize_result = std::make_shared<InitializeResult>(result_json.get<InitializeResult>());
    if (state->options.strict_protocol_validation &&
        !is_legacy_protocol_version(initialize_result->protocolVersion)) {
        request_close(state, "Server selected an unsupported protocol version");
        // The version is whatever the remote server put in its `initialize` result, and
        // this message reaches the application's log. Flatten and bound it, or a server
        // answering with CR/LF forges a log line the application did not write.
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Server selected unsupported protocol version: " +
                           detail::sanitize_for_diagnostics(initialize_result->protocolVersion));
    }

    co_await notification(state, methods::g_NOTIFICATIONS_INITIALIZED, std::nullopt);

    ConnectResult connected;
    connected.era = ProtocolEra::eLegacy;
    connected.protocolVersion = std::move(initialize_result->protocolVersion);
    connected.capabilities = std::move(initialize_result->capabilities);
    connected.serverInfo = std::move(initialize_result->serverInfo);
    connected.instructions = std::move(initialize_result->instructions);
    connected.supportedVersions = *advertised;
    attempt->result = std::move(connected);
    co_return attempt;
}

Task<ConnectResult> Client::Impl::initialize_session(std::shared_ptr<Impl> state, VersionPtr version,
                                                     VersionsPtr advertised) {
    auto attempt =
        co_await attempt_initialize(std::move(state), std::move(version), std::move(advertised));
    if (attempt->error) {
        throw McpError(*attempt->error);
    }
    co_return std::move(*attempt->result);
}

ConnectResult Client::Impl::modern_result(const DiscoverResult& discover, const std::string& version) {
    ConnectResult connected;
    connected.era = ProtocolEra::eModern;
    connected.protocolVersion = version;
    connected.capabilities = discover.capabilities;
    connected.serverInfo = discover.serverInfo;
    connected.instructions = discover.instructions;
    connected.supportedVersions = discover.supportedVersions;
    return connected;
}

Task<ConnectResult> Client::Impl::adopt_discover(std::shared_ptr<Impl> state, ProbePtr outcome,
                                                 EraReason reason,
                                                 std::chrono::steady_clock::time_point started) {
    const auto modern = newest_mutual(mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS,
                                      outcome->discover->supportedVersions);
    if (modern) {
        state->become_modern(*modern);
        state->report_era(ProtocolEra::eModern, reason, started);
        co_return modern_result(*outcome->discover, *modern);
    }
    const auto legacy = newest_mutual(mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS,
                                      outcome->discover->supportedVersions);
    if (!legacy) {
        throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                       "The server shares no protocol version with this client",
                       nlohmann::json{{"supported", outcome->discover->supportedVersions}});
    }
    state->report_era(ProtocolEra::eLegacy, reason, started);
    co_return co_await initialize_session(
        state, legacy,
        std::make_shared<const std::vector<std::string>>(outcome->discover->supportedVersions));
}

[[noreturn]] void Client::Impl::throw_probe_failure(const ProbeOutcome& outcome,
                                                    const std::string& version) {
    if (outcome.kind == ProbeOutcome::Kind::eModernError) {
        const auto advertised = advertised_versions(*outcome.error);
        if (outcome.error->code != mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION ||
            advertised.empty()) {
            throw McpError(*outcome.error);
        }
        std::string listed;
        for (const auto& entry : advertised) {
            listed += (listed.empty() ? "" : ", ") + detail::sanitize_for_diagnostics(entry);
        }
        // A server can refuse a request whose version it lists, for instance when the
        // request is otherwise unacceptable; naming the version as unsupported would then
        // contradict the list.
        const bool listed_by_server = std::ranges::find(advertised, version) != advertised.end();
        if (listed_by_server) {
            throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                           "The server refused protocol version " + version +
                               " although it lists it among the versions it supports (" + listed + ")",
                           outcome.error->data);
        }
        throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                       "The server does not support protocol version " + version + "; it supports " +
                           listed +
                           ". Use ClientProtocol::automatic() to connect with a version both sides "
                           "speak",
                       outcome.error->data);
    }
    if (outcome.kind == ProbeOutcome::Kind::eClosed) {
        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                       "The server closed the connection while the client probed it with "
                       "server/discover; a server that exits on an unknown request needs "
                       "ClientProtocol::legacy()");
    }
    const std::string reason = outcome.error
                                   ? ": " + detail::sanitize_for_diagnostics(outcome.error->message)
                                   : " without a usable reply";
    throw McpError(outcome.error ? outcome.error->code : mcp::constants::jsonrpc::g_INVALID_REQUEST,
                   "The server does not speak protocol version " + version +
                       "; server/discover failed" + reason +
                       ". Use ClientProtocol::automatic() or ClientProtocol::legacy() for a server "
                       "that predates it",
                   outcome.error ? outcome.error->data : std::nullopt);
}

bool Client::Impl::refuses_listed_version(const ProbeOutcome& outcome, const std::string& version) {
    if (outcome.kind != ProbeOutcome::Kind::eModernError ||
        outcome.error->code != mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION) {
        return false;
    }
    const auto advertised = advertised_versions(*outcome.error);
    return std::ranges::find(advertised, version) != advertised.end();
}

Task<ConnectResult> Client::Impl::connect_pinned_modern(std::shared_ptr<Impl> state,
                                                        std::chrono::steady_clock::time_point started) {
    auto version = std::make_shared<const std::string>(state->options.protocol.version());
    auto outcome = co_await probe(state, version, state->options.request_timeout);
    // A client whose version is refused retries with a version from the server's list that both
    // sides support. For a pinned client that can only be the pinned version, so a refusal that
    // lists it is asked again, once.
    if (refuses_listed_version(*outcome, *version)) {
        outcome = co_await probe(state, version, state->options.request_timeout);
    }
    if (outcome->kind != ProbeOutcome::Kind::eDiscover) {
        throw_probe_failure(*outcome, *version);
    }
    state->become_modern(*version);
    state->report_era(ProtocolEra::eModern, EraReason::ePinned, started);
    co_return modern_result(*outcome->discover, *version);
}

Task<ConnectResult> Client::Impl::connect_auto(std::shared_ptr<Impl> state,
                                               std::chrono::steady_clock::time_point started) {
    const auto no_versions = std::make_shared<const std::vector<std::string>>();
    const auto preferred = std::make_shared<const std::string>(
        mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.back());
    auto outcome = co_await probe(state, preferred, state->options.probe_timeout);
    auto reason = EraReason::eDiscoverOk;

    if (outcome->kind == ProbeOutcome::Kind::eModernError &&
        outcome->error->code == mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION) {
        const auto advertised =
            std::make_shared<const std::vector<std::string>>(advertised_versions(*outcome->error));
        const auto modern =
            newest_mutual(mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS, *advertised);
        const auto legacy =
            newest_mutual(mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS, *advertised);
        if (modern) {
            // The server is modern and names a version this client speaks: ask again with it.
            reason = EraReason::eModernErrorRetry;
            outcome = co_await probe(state, modern, state->options.probe_timeout);
        } else if (legacy) {
            state->report_era(ProtocolEra::eLegacy, EraReason::eModernErrorRetry, started);
            co_return co_await initialize_session(state, legacy, advertised);
        } else {
            throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                           "The server shares no protocol version with this client",
                           outcome->error->data);
        }
    }

    if (outcome->kind == ProbeOutcome::Kind::eDiscover) {
        co_return co_await adopt_discover(state, outcome, reason, started);
    }
    if (outcome->kind == ProbeOutcome::Kind::eModernError ||
        outcome->kind == ProbeOutcome::Kind::eClosed) {
        throw_probe_failure(*outcome, *preferred);
    }

    // Legacy: any other error, a reply that is not JSON, or silence.
    state->report_era(ProtocolEra::eLegacy,
                      outcome->kind == ProbeOutcome::Kind::eTimeout
                          ? EraReason::eFallbackTimeout
                          : EraReason::eFallbackNonModernError,
                      started);
    auto attempt = co_await attempt_initialize(
        state,
        std::make_shared<const std::string>(mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION),
        no_versions);
    if (attempt->result) {
        co_return std::move(*attempt->result);
    }

    // A server that answered nothing in time can still be a modern one that was starting up. If
    // it now refuses initialize naming modern versions, probe it again, once.
    if (lists_modern_version(*attempt->error)) {
        auto reprobe = co_await probe(state, preferred, state->options.probe_timeout);
        if (reprobe->kind == ProbeOutcome::Kind::eDiscover) {
            co_return co_await adopt_discover(state, reprobe,
                                              EraReason::eReprobeAfterInitializeRejected, started);
        }
    }
    throw McpError(*attempt->error);
}

Task<ConnectResult> Client::Impl::connect_session(std::shared_ptr<Impl> state) {
    const auto started = std::chrono::steady_clock::now();
    const auto& protocol = state->options.protocol;
    const bool pinned_legacy = protocol.kind() == ClientProtocol::Kind::eLegacy ||
                               (protocol.kind() == ClientProtocol::Kind::eVersion &&
                                is_legacy_protocol_version(protocol.version()));
    try {
        if (pinned_legacy) {
            state->report_era(ProtocolEra::eLegacy, EraReason::ePinned, started);
            co_return co_await initialize_session(
                state, std::make_shared<const std::string>(protocol.version()),
                std::make_shared<const std::vector<std::string>>());
        }
        if (protocol.kind() == ClientProtocol::Kind::eVersion) {
            co_return co_await connect_pinned_modern(state, started);
        }
        co_return co_await connect_auto(state, started);
    } catch (...) {
        request_close(state, "Client initialization failed");
        throw;
    }
}

void Client::Impl::fail_probe(const std::shared_ptr<Impl>& state) {
    std::scoped_lock lock(state->pending_requests_mutex);
    for (auto& [id, pending_request] : state->pending_requests) {
        static_cast<void>(id);
        if (pending_request->probe && !pending_request->completed) {
            pending_request->error = Error{
                .code = mcp::constants::jsonrpc::g_PARSE_ERROR,
                .message = "Reply to server/discover is not JSON-RPC",
            };
            pending_request->completed = true;
            signal_pending_request(*pending_request);
        }
    }
}

void Client::Impl::become_modern(const std::string& version) {
    std::scoped_lock lock(era_mutex);
    modern_protocol_version = version;
}

std::optional<std::string> Client::Impl::modern_version() const {
    std::scoped_lock lock(era_mutex);
    return modern_protocol_version;
}

ClientCapabilities Client::Impl::effective_capabilities() const {
    std::scoped_lock lock(handlers_mutex);
    auto capabilities = client_capabilities;
    if (request_handlers.contains(methods::g_ROOTS_LIST) && !capabilities.roots) {
        capabilities.roots = ClientCapabilities::RootsCapability{};
    }
    if (request_handlers.contains(methods::g_SAMPLING_CREATE_MESSAGE) && !capabilities.sampling) {
        capabilities.sampling = ClientCapabilities::SamplingCapability{};
    }
    if (request_handlers.contains(methods::g_ELICITATION_CREATE) && !capabilities.elicitation) {
        capabilities.elicitation = ClientCapabilities::ElicitationCapability{
            .form = nlohmann::json::object(),
            .url = std::nullopt,
        };
    }
    return capabilities;
}

}  // namespace mcp
