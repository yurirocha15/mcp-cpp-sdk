#include "server_impl.hpp"

namespace methods = mcp::constants::methods;
namespace sealed_token = mcp::detail::constants::sealed_token;

namespace mcp {

Task<std::string> Server::Impl::dispatch_mrtr(const nlohmann::json& json_msg, std::string_view method,
                                              RequestScope scope) {
    const auto& params = json_msg.at("params");
    const auto state = params.find("requestState");
    const auto responses = params.find("inputResponses");
    if ((state != params.end() && !state->is_string()) ||
        (responses != params.end() && !responses->is_object())) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "requestState must be a string and inputResponses an object");
    }
    const bool retry = state != params.end();
    if (!retry && !detail::client_can_answer_input(*scope.meta->meta)) {
        // Such a client can never be asked for input, so the handler never parks.
        co_return co_await serve_without_parking(json_msg, std::move(scope));
    }

    std::optional<nlohmann::json> progress_token;
    nlohmann::json client_capabilities = nlohmann::json::object();
    {
        const auto& meta = *scope.meta->meta;
        if (const auto token = meta.find("progressToken"); token != meta.end()) {
            progress_token = *token;
        }
        const auto declared = meta.find(std::string(mcp::constants::meta::g_META_CLIENT_CAPABILITIES));
        if (declared != meta.end() && declared->is_object()) {
            client_capabilities = *declared;
        }
    }
    const auto request_cancelled = scope.cancelled;
    auto route = notification_route(request_cancelled, scope.meta, scope.session_request,
                                    scope.notifications, session_snapshot(), nullptr, nullptr);

    // What the answer still needs from the request once the handler has taken it over.
    const auto principal = scope.principal;
    const auto request_meta = scope.meta;
    std::shared_ptr<const MrtrOptions> options;
    std::shared_ptr<const detail::sealed_state::Codec> codec;
    {
        std::scoped_lock lock(mrtr_mutex);
        options = mrtr_options;
        codec = mrtr_codec;
    }
    const bool sealed = codec != nullptr;
    std::optional<detail::MrtrSealedState> earlier;
    std::shared_ptr<detail::MrtrEntry> entry;
    if (retry) {
        // Checked before the state is taken, so that a refused retry can be sent again.
        if (auto mismatch = retry_header_mismatch(json_msg, method, scope)) {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_HEADER_MISMATCH,
                                      "Header mismatch: " + *mismatch);
        }
        if (sealed) {
            earlier = open_sealed_state(*codec, *options, state->get_ref<const std::string&>(),
                                        principal, method, detail::mrtr_params_digest(params));
            if (!earlier) {
                co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                          mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                          "Unknown or expired requestState");
            }
        } else {
            entry = take_parked(state->get_ref<const std::string&>(), principal, method,
                                detail::mrtr_params_digest(params));
            if (!entry) {
                co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                          mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                          "Unknown or expired requestState");
            }
        }
    }
    if (entry) {
        {
            // The resumed handler counts as this retry, until it ends or parks again.
            std::scoped_lock lock(mrtr_mutex);
            entry->in_flight = scope.in_flight;
            entry->count_again = scope.count_again;
        }
        // [gcc11-sso: named-aggregate-arg] DO NOT build the responses inside the co_await.
        nlohmann::json input_responses =
            responses != params.end() ? *responses : nlohmann::json::object();
        co_await detail::mrtr_resume(entry, std::move(input_responses), std::move(route),
                                     std::move(progress_token), std::move(client_capabilities));
    } else {
        auto executor = co_await boost::asio::this_coro::executor;
        // A sealed handler is released when its request ends, which must not mark the request
        // cancelled, so it has a flag of its own that the request's cancellation is passed on to.
        auto cancelled = request_cancelled && !sealed ? request_cancelled
                                                      : std::make_shared<std::atomic<bool>>(false);
        entry = std::make_shared<detail::MrtrEntry>(executor, cancelled, mrtr_counters);
        entry->sink = std::move(route);
        entry->progress_token = std::move(progress_token);
        entry->client_capabilities = std::move(client_capabilities);
        if (earlier) {
            // A sealed retry runs the handler again; the answers of the earlier rounds, and of
            // the questions this retry answers, are given back to it as it asks them again.
            entry->sealed = true;
            entry->replay = std::move(earlier->answers);
            if (responses != params.end()) {
                for (const auto& [key, asked_method] : earlier->asked) {
                    if (const auto answer = responses->find(key); answer != responses->end()) {
                        entry->replay[key] = {{"m", asked_method}, {"r", *answer}};
                    }
                }
            }
            entry->replaying->store(!entry->replay.empty(), std::memory_order_relaxed);
        } else if (sealed) {
            entry->sealed = true;
        }
        {
            std::scoped_lock lock(mrtr_mutex);
            entry->in_flight = scope.in_flight;
            entry->count_again = scope.count_again;
        }
        auto message = scope.message ? scope.message : std::make_shared<const nlohmann::json>(json_msg);
        RequestScope handler_scope = std::move(scope);
        handler_scope.in_flight.reset();
        handler_scope.count_again = nullptr;
        handler_scope.cancelled = std::move(cancelled);
        handler_scope.mrtr = entry;
        // The handler runs apart from this request, which ends when the handler parks.
        boost::asio::co_spawn(
            entry->strand,
            run_mrtr_handler(shared_from_this(), entry, std::move(message), std::move(handler_scope)),
            boost::asio::detached);
    }

    const bool resumed_parked = retry && !sealed;
    // A sealed handler that is released is destroyed where it waits and never runs its own ending,
    // so the request gives up what the handler held.
    const auto end_sealed_handler = [this, &entry, sealed] {
        if (sealed) {
            std::scoped_lock lock(mrtr_mutex);
            entry->in_flight.reset();
            entry->ended = true;
        }
    };
    auto outcome = co_await detail::mrtr_await(entry, request_cancelled, resumed_parked || sealed);
    if (outcome.event == detail::MrtrEntry::Event::eCompleted) {
        if (!resumed_parked) {
            co_return std::move(outcome.wire);
        }
        // The handler answered with the id of the request that started it.
        auto answer = nlohmann::json::parse(outcome.wire);
        answer["id"] = json_msg.at("id");
        co_return mcp::detail::dump_json(answer);
    }
    if (outcome.event == detail::MrtrEntry::Event::eMissingCapability) {
        detail::mrtr_abandon(entry);
        end_sealed_handler();
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY,
                                  "The request needs a capability the client did not declare",
                                  {{"requiredCapabilities", std::move(outcome.missing_capabilities)}});
    }
    if (request_cancelled && request_cancelled->load(std::memory_order_relaxed)) {
        // Nothing is sent for a cancelled request, and nothing will retry it.
        detail::mrtr_abandon(entry);
        end_sealed_handler();
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INTERNAL_ERROR, "Request cancelled");
    }
    std::string token;
    if (sealed) {
        // Nothing waits in this server: the handler is released and the state goes to the client.
        detail::mrtr_abandon(entry);
        end_sealed_handler();
        detail::MrtrSealedState next;
        next.id = detail::generate_secure_session_id();
        next.expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch() + options->state_ttl)
                              .count();
        next.principal_digest = detail::mrtr_principal_digest(principal ? *principal : std::string{});
        next.method = std::string(method);
        next.params_digest = detail::mrtr_params_digest(params);
        next.round = (earlier ? earlier->round : 0U) + 1U;
        for (const auto& item : outcome.requests.items()) {
            next.asked[item.key()] = item.value().value("method", std::string{});
        }
        next.answers = std::move(outcome.answers);
        if (next.round > options->max_rounds) {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                      "The request needs more rounds of input than the server allows");
        }
        token = detail::mrtr_seal_state(*codec, next);
        if (token.empty()) {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                      "The state of the request is larger than the server allows");
        }
    } else {
        token = park(entry, principal, method, detail::mrtr_params_digest(params),
                     detail::json_memory_cost(json_msg));
    }
    nlohmann::json result = {
        {"resultType", "input_required"},
        {"inputRequests", std::move(outcome.requests)},
        {"requestState", token},
    };
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), request_meta, std::move(result),
                               std::nullopt);
}

Task<std::string> Server::Impl::serve_without_parking(const nlohmann::json& json_msg,
                                                      RequestScope scope) {
    const auto input = std::make_shared<detail::InlineInput>(scope.meta);
    scope.inline_input = input;
    if (!scope.cancelled) {
        scope.cancelled = std::make_shared<std::atomic<bool>>(false);
    }
    // [gcc11-sso: string-in-frame] The answer is held as json until no suspension is left.
    nlohmann::json answer;
    std::exception_ptr failure;
    try {
        answer = co_await serve_mrtr_method(json_msg, scope);
    } catch (...) {
        failure = std::current_exception();
    }
    // A question the handler asked decides the answer, as for a parked handler, whatever the
    // handler did with the refusal.
    auto missing = input->missing_capabilities();
    if (!missing.empty()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY,
                                  "The request needs a capability the client did not declare",
                                  {{"requiredCapabilities", std::move(missing)}});
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return std::move(answer.get_ref<nlohmann::json::string_t&>());
}

Task<std::string> Server::Impl::serve_mrtr_method(const nlohmann::json& json_msg,
                                                  const RequestScope& scope) {
    const auto& method = json_msg.at("method").get_ref<const std::string&>();
    if (method == methods::g_TOOLS_CALL) {
        co_return co_await handle_tools_call_wire(json_msg, scope);
    }
    if (method == methods::g_RESOURCES_READ) {
        co_return co_await handle_resources_read_wire(json_msg, scope);
    }
    co_return co_await handle_prompts_get_wire(json_msg, scope);
}

std::optional<std::string> Server::Impl::retry_header_mismatch(const nlohmann::json& json_msg,
                                                               std::string_view method,
                                                               const RequestScope& scope) const {
    if (method != methods::g_TOOLS_CALL || scope.param_headers == nullptr) {
        return std::nullopt;
    }
    const auto params = deserialize_request_params<CallToolParams>(json_msg, methods::g_TOOLS_CALL);
    const auto tool = tool_handlers.find(params.name);
    if (tool == tool_handlers.end()) {
        return std::nullopt;
    }
    return detail::param_header_mismatch(tool->second.schemas.param_headers->headers, params.arguments,
                                         *scope.param_headers);
}

Task<void> Server::Impl::run_mrtr_handler(std::shared_ptr<Impl> impl,
                                          std::shared_ptr<detail::MrtrEntry> entry,
                                          std::shared_ptr<const nlohmann::json> message,
                                          RequestScope scope) {
    const nlohmann::json& json_msg = *message;
    // [gcc11-sso: string-in-frame] The answer is held as json until no suspension is left.
    nlohmann::json answer;
    int error_code = mcp::constants::jsonrpc::g_INTERNAL_ERROR;
    nlohmann::json error_payload;
    try {
        answer = co_await impl->serve_mrtr_method(json_msg, scope);
    } catch (const InvalidParamsError& error) {
        error_code = mcp::constants::jsonrpc::g_INVALID_PARAMS;
        error_payload = error.what();
    } catch (const nlohmann::json::exception&) {
        // Its text describes the library, and quotes bytes of the value that could not be encoded.
        error_payload = "The result could not be encoded as JSON";
    } catch (const std::exception& error) {
        error_payload = detail::sanitize_for_diagnostics(error.what());
    } catch (...) {
        // This coroutine is detached, so a throw that left it would leave the entry incomplete and
        // the request waiting for an answer that never comes.
        error_payload = "Internal error";
    }
    {
        std::scoped_lock lock(impl->mrtr_mutex);
        entry->in_flight.reset();
        entry->ended = true;
    }
    if (!error_payload.is_null()) {
        detail::mrtr_complete(entry, make_error_wire(json_msg.at("id").get<RequestId>(), error_code,
                                                     error_payload.get<std::string>()));
        co_return;
    }
    detail::mrtr_complete(entry, std::move(answer.get_ref<nlohmann::json::string_t&>()));
}

std::string Server::Impl::park(const std::shared_ptr<detail::MrtrEntry>& entry,
                               const std::shared_ptr<const std::string>& principal,
                               std::string_view method, std::string digest, std::size_t bytes) {
    auto token = detail::generate_secure_session_id();
    std::vector<ParkedRequest> evicted;
    std::shared_ptr<const MrtrOptions> options;
    auto expiry = std::make_shared<boost::asio::steady_timer>(entry->strand);
    {
        std::scoped_lock lock(mrtr_mutex);
        options = mrtr_options;
        const std::string who = principal ? *principal : std::string{};
        // Gives up the longest-waiting handler that @p matches selects, if any.
        const auto evict_oldest = [this, &evicted](const auto& matches) {
            auto victim = parked_requests.end();
            std::size_t matching = 0;
            for (auto it = parked_requests.begin(); it != parked_requests.end(); ++it) {
                if (!matches(it->second)) {
                    continue;
                }
                ++matching;
                if (victim == parked_requests.end() || it->second.sequence < victim->second.sequence) {
                    victim = it;
                }
            }
            if (victim != parked_requests.end()) {
                evicted.push_back(std::move(victim->second));
                parked_requests.erase(victim);
            }
            return matching;
        };
        const auto per_principal = std::max<std::size_t>(options->max_parked_per_principal, 1);
        const auto same_principal = [&who](const ParkedRequest& parked) {
            return parked.principal == who;
        };
        std::size_t waiting = 0;
        for (const auto& [key, parked] : parked_requests) {
            waiting += same_principal(parked) ? 1 : 0;
        }
        for (; waiting >= per_principal; --waiting) {
            evict_oldest(same_principal);
        }
        while (parked_requests.size() >= std::max<std::size_t>(options->max_parked, 1)) {
            evict_oldest([](const ParkedRequest&) { return true; });
        }
        // Each waiting handler keeps its own copy of the request, which a peer can make large.
        std::size_t principal_bytes = 0;
        for (const auto& [key, parked] : parked_requests) {
            principal_bytes += same_principal(parked) ? parked.bytes : 0;
        }
        while (principal_bytes > 0 &&
               principal_bytes + bytes > options->max_parked_bytes_per_principal) {
            evict_oldest(same_principal);
            principal_bytes -= evicted.back().bytes;
        }
        std::size_t parked_bytes = 0;
        for (const auto& [key, parked] : parked_requests) {
            parked_bytes += parked.bytes;
        }
        while (!parked_requests.empty() && parked_bytes + bytes > options->max_parked_bytes) {
            evict_oldest([](const ParkedRequest&) { return true; });
            parked_bytes -= evicted.back().bytes;
        }
        parked_requests.emplace(token, ParkedRequest{
                                           .entry = entry,
                                           .principal = who,
                                           .method = std::string(method),
                                           .digest = std::move(digest),
                                           .sequence = ++next_park_sequence,
                                           .expiry = expiry,
                                           .bytes = bytes,
                                       });
        // A parked handler is bounded by the limits above instead of the in-flight limits.
        entry->in_flight.reset();
    }
    for (auto& victim : evicted) {
        finish_eviction(std::move(victim), MrtrEvictionReason::eCapacity, *options);
    }
    expiry->expires_after(options->ttl);
    expiry->async_wait(
        [weak = weak_from_this(), token, expiry](const boost::system::error_code& error) {
            if (error) {
                return;
            }
            if (auto impl = weak.lock()) {
                impl->evict_parked(token, MrtrEvictionReason::eExpired);
            }
        });
    return token;
}

std::optional<detail::MrtrSealedState> Server::Impl::open_sealed_state(
    const detail::sealed_state::Codec& codec, const MrtrOptions& options, const std::string& token,
    const std::shared_ptr<const std::string>& principal, std::string_view method,
    const std::string& digest) {
    auto state = detail::mrtr_open_state(codec, token);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (!state || state->expires_ms <= now || state->round > options.max_rounds) {
        mrtr_counters->resume_unknown.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (state->principal_digest != detail::mrtr_principal_digest(principal ? *principal : "")) {
        mrtr_counters->resume_principal_mismatch.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (state->method != method || state->params_digest != digest) {
        mrtr_counters->resume_params_mismatch.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (options.consume) {
        bool fresh = false;
        try {
            fresh = options.consume(state->id);
        } catch (...) {
            // A hook that cannot tell whether the state was used refuses it.
        }
        if (!fresh) {
            mrtr_counters->resume_unknown.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }
    mrtr_counters->resume_hit.fetch_add(1, std::memory_order_relaxed);
    return state;
}

std::shared_ptr<detail::MrtrEntry> Server::Impl::take_parked(
    const std::string& token, const std::shared_ptr<const std::string>& principal,
    std::string_view method, const std::string& digest) {
    std::shared_ptr<boost::asio::steady_timer> expiry;
    std::shared_ptr<detail::MrtrEntry> entry;
    {
        std::scoped_lock lock(mrtr_mutex);
        const auto found = parked_requests.find(token);
        if (found == parked_requests.end()) {
            mrtr_counters->resume_unknown.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        // A mismatch leaves the handler waiting for the retry it was parked for.
        if (found->second.principal != (principal ? *principal : std::string{})) {
            mrtr_counters->resume_principal_mismatch.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (found->second.method != method || found->second.digest != digest) {
            mrtr_counters->resume_params_mismatch.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        entry = std::move(found->second.entry);
        expiry = std::move(found->second.expiry);
        parked_requests.erase(found);
    }
    mrtr_counters->resume_hit.fetch_add(1, std::memory_order_relaxed);
    boost::asio::post(entry->strand, [expiry] { expiry->cancel(); });
    return entry;
}

void Server::Impl::evict_parked(const std::string& token, MrtrEvictionReason reason) {
    ParkedRequest victim;
    std::shared_ptr<const MrtrOptions> options;
    {
        std::scoped_lock lock(mrtr_mutex);
        const auto found = parked_requests.find(token);
        if (found == parked_requests.end()) {
            return;
        }
        victim = std::move(found->second);
        parked_requests.erase(found);
        options = mrtr_options;
    }
    finish_eviction(std::move(victim), reason, *options);
}

void Server::Impl::finish_eviction(ParkedRequest victim, MrtrEvictionReason reason,
                                   const MrtrOptions& options) {
    auto& counter = reason == MrtrEvictionReason::eExpired ? mrtr_counters->evicted_ttl
                                                           : mrtr_counters->evicted_cap;
    counter.fetch_add(1, std::memory_order_relaxed);
    {
        // A handler that goes on without its answers works for no request, and counts as one.
        std::scoped_lock lock(mrtr_mutex);
        if (!victim.entry->ended && victim.entry->count_again) {
            victim.entry->in_flight = victim.entry->count_again(victim.bytes);
        }
    }
    detail::mrtr_abandon(victim.entry);
    boost::asio::post(victim.entry->strand, [expiry = victim.expiry] { expiry->cancel(); });
    if (options.on_eviction) {
        try {
            options.on_eviction(reason);
        } catch (...) {
            // An observer's failure must not keep the handler waiting.
        }
    }
}

void Server::Impl::abandon_all_parked() {
    std::unordered_map<std::string, ParkedRequest> abandoned;
    {
        std::scoped_lock lock(mrtr_mutex);
        abandoned.swap(parked_requests);
    }
    for (auto& [token, parked] : abandoned) {
        detail::mrtr_abandon(parked.entry);
        boost::asio::post(parked.entry->strand, [expiry = parked.expiry] { expiry->cancel(); });
    }
}

void Server::abandon_waiting_handlers() { impl_->abandon_all_parked(); }

void Server::set_mrtr_options(MrtrOptions options) {
    // The secrets of the caller's list are overwritten once the key ring is built, or on a throw.
    const auto wipe = [](std::vector<std::string>& secrets) {
        for (auto& secret : secrets) {
            OPENSSL_cleanse(secret.data(), secret.size());
        }
        secrets.clear();
    };
    std::shared_ptr<const detail::sealed_state::Codec> codec;
    try {
        if (options.state_ttl <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("state_ttl must be positive");
        }
        if (options.max_rounds == 0) {
            throw std::invalid_argument("max_rounds must be at least 1");
        }
        if (options.max_state_bytes < mcp::detail::constants::mrtr::g_MIN_STATE_CHARS) {
            throw std::invalid_argument("max_state_bytes is too small for any request state");
        }
        if (options.state_mode == MrtrStateMode::eSealed) {
            if (options.sealing_keys.empty()) {
                throw std::invalid_argument("sealed request state needs at least one sealing key");
            }
            std::vector<detail::sealed_state::SealKey> keys;
            for (const auto& material : options.sealing_keys) {
                if (material.size() < sealed_token::g_KEY_SIZE) {
                    throw std::invalid_argument("a sealing key must be at least 32 bytes");
                }
                // The master key and its id are one-way derivations of the supplied secret.
                auto secret = mcp::detail::sha256(material);
                const auto id = mcp::detail::sha256(mcp::detail::digest_bytes(secret));
                std::array<std::uint8_t, sealed_token::g_KID_SIZE> kid{};
                std::copy_n(id.begin(), kid.size(), kid.begin());
                keys.emplace_back(kid, secret);
                OPENSSL_cleanse(secret.data(), secret.size());
            }
            codec = std::make_shared<const detail::sealed_state::Codec>(std::move(keys),
                                                                        options.max_state_bytes);
        }
    } catch (...) {
        wipe(options.sealing_keys);
        throw;
    }
    wipe(options.sealing_keys);
    auto snapshot = std::make_shared<const MrtrOptions>(std::move(options));
    std::scoped_lock lock(impl_->mrtr_mutex);
    impl_->mrtr_codec = std::move(codec);
    impl_->mrtr_options = std::move(snapshot);
}

MrtrStats Server::mrtr_stats() const {
    MrtrStats stats;
    {
        std::scoped_lock lock(impl_->mrtr_mutex);
        stats.parked = impl_->parked_requests.size();
    }
    const auto& counters = *impl_->mrtr_counters;
    stats.evicted_ttl = counters.evicted_ttl.load(std::memory_order_relaxed);
    stats.evicted_cap = counters.evicted_cap.load(std::memory_order_relaxed);
    stats.abandoned_completed = counters.abandoned_completed.load(std::memory_order_relaxed);
    stats.resume_hit = counters.resume_hit.load(std::memory_order_relaxed);
    stats.resume_unknown = counters.resume_unknown.load(std::memory_order_relaxed);
    stats.resume_principal_mismatch =
        counters.resume_principal_mismatch.load(std::memory_order_relaxed);
    stats.resume_params_mismatch = counters.resume_params_mismatch.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace mcp
