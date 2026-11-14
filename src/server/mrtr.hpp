#pragma once

// Multi round-trip requests (protocol revision 2026-07-28): a tools/call, prompts/get or
// resources/read whose handler asks the client for input is answered with an InputRequiredResult,
// and the handler waits, parked, until the client retries the request with the input.
//
// Everything about one parked handler lives in an MrtrEntry, and every member of the entry is
// read and written only on the entry's strand, where the handler itself runs.

#include <mcp/core/context.hpp>
#include <mcp/core/core.hpp>

#include <mcp/protocol/request_meta.hpp>

#include "../detail/sealed_state.hpp"

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::detail {

/// Thrown from a parked handler's request for input once the request is abandoned: it expired, was
/// evicted, was cancelled or its server is gone. The handler unwinds; its answer goes nowhere.
class MrtrAbandoned : public std::runtime_error {
   public:
    MrtrAbandoned() : std::runtime_error("the request waiting for client input was abandoned") {}
};

/// A completion handler kept without being invoked. Destroying it destroys the coroutines that
/// wait for it, running no more of their code.
class HeldHandler {
   public:
    HeldHandler() = default;
    HeldHandler(const HeldHandler&) = delete;
    HeldHandler& operator=(const HeldHandler&) = delete;
    virtual ~HeldHandler() = default;
};

/// Counters the entries of one server share with it.
struct MrtrCounters {
    std::atomic<std::uint64_t> evicted_ttl{0};
    std::atomic<std::uint64_t> evicted_cap{0};
    std::atomic<std::uint64_t> abandoned_completed{0};
    std::atomic<std::uint64_t> resume_hit{0};
    std::atomic<std::uint64_t> resume_unknown{0};
    std::atomic<std::uint64_t> resume_principal_mismatch{0};
    std::atomic<std::uint64_t> resume_params_mismatch{0};
};

/// The handler of one multi round-trip request and the state it shares with the requests that
/// carry it: the first one and each retry.
class MrtrEntry {
   public:
    /// What the request carrying the handler is waiting for.
    enum class Event : std::uint8_t {
        ePending,            ///< Nothing yet: the handler runs.
        eCompleted,          ///< The handler returned; `wire` is its answer.
        eParked,             ///< The handler waits for the input in `requests`.
        eMissingCapability,  ///< The handler asked for input the client cannot give.
    };

    /// What the request carrying the handler is told when it stops waiting.
    struct Outcome {
        Event event{Event::ePending};
        std::string wire;
        nlohmann::json requests;
        nlohmann::json missing_capabilities;
        /// The answers the handler took in this run, by key (sealed mode).
        nlohmann::json answers;
    };

    /// Keeps @p handler, the completion handler of a question, until `held` is cleared.
    template <typename Handler>
    void hold(Handler handler) {
        struct Holder final : HeldHandler {
            explicit Holder(Handler&& value) : handler(std::move(value)) {}
            Handler handler;
        };
        held.push_back(std::make_unique<Holder>(std::move(handler)));
    }

    MrtrEntry(const boost::asio::any_io_executor& executor,
              std::shared_ptr<std::atomic<bool>> cancelled, std::shared_ptr<MrtrCounters> counters);

    boost::asio::strand<boost::asio::any_io_executor> strand;
    /// The handler's cancellation flag. A retry that is cancelled sets it.
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<MrtrCounters> counters;

    Event event{Event::ePending};
    /// Cancelled to wake the request waiting for `event` to leave ePending.
    boost::asio::steady_timer event_signal;
    /// Cancelled to wake the handler's requests for input when `round` moves on or the entry is
    /// abandoned.
    boost::asio::steady_timer resume_signal;
    std::uint64_t round{0};
    bool abandoned{false};
    bool park_scheduled{false};
    std::uint64_t next_key{0};
    /// The keys handlers chose for their questions in this run, across rounds.
    std::set<std::string> named_keys;
    /// The input requests of the current round, by key.
    std::map<std::string, nlohmann::json> requests;
    /// The client's responses of the last retry, by key; each is taken once.
    nlohmann::json responses = nlohmann::json::object();
    nlohmann::json missing_capabilities = nlohmann::json::object();
    std::string wire;
    /// Sealed mode: the handler runs again from the start on each retry, and asks the earlier
    /// rounds answered return at once from `replay`.
    bool sealed{false};
    /// Answers given in earlier rounds that the handler has not asked for again yet, by key, each
    /// as `{"m": method of the question, "r": response}`. A question of another method is new.
    nlohmann::json replay = nlohmann::json::object();
    /// Every answer the handler has taken in this run, by key, replayed or fresh, in the form of
    /// `replay`.
    nlohmann::json answers = nlohmann::json::object();
    /// True while the handler re-executes what an earlier run already did (see
    /// Context::is_replay()).
    std::shared_ptr<std::atomic<bool>> replaying = std::make_shared<std::atomic<bool>>(false);
    /// Sealed mode: the completion handlers of the questions that wait for the server to release
    /// the handler. Destroyed, not invoked, on release.
    std::vector<std::unique_ptr<HeldHandler>> held;
    /// The handler finished while parked, so the next retry gets `wire` instead of resuming it.
    bool answered_while_parked{false};

    /// Where the handler's notifications go: the stream of the request that currently carries it.
    MessageSender sink;
    /// The progress token of the request that currently carries the handler.
    std::optional<nlohmann::json> progress_token;
    /// The `io.modelcontextprotocol/clientCapabilities` of that request.
    nlohmann::json client_capabilities = nlohmann::json::object();
    /// The share of the server's in-flight limits the handler holds while it runs, taken from the
    /// request that carries it and given up while it is parked. Guarded by the server's lock on
    /// parked handlers.
    std::shared_ptr<void> in_flight;
    /// Makes a new share, of the given size, of the limits the request that carries the handler
    /// counts against. A handler that goes on after it was given up holds one until it ends.
    /// Guarded like `in_flight`.
    std::function<std::shared_ptr<void>(std::size_t)> count_again;
    /// The handler ended. Guarded like `in_flight`.
    bool ended{false};
};

/// Whether the client declared a capability that lets it answer a request for input: elicitation,
/// sampling or roots, each as an object. @p request_meta is the `_meta` object of the request.
/// Without one, every request for input ends in a missing-capability answer, so the handler can
/// never park.
bool client_can_answer_input(const nlohmann::json& request_meta);

/// The state of a request whose handler runs inside the request, because the client declared
/// nothing that lets it answer a request for input. The handler's questions are refused the way a
/// parked handler's are: what the client would have had to declare is recorded here, and the
/// request is answered with it.
class InlineInput {
   public:
    explicit InlineInput(std::shared_ptr<const RequestMetadata> request_meta)
        : request_meta_(std::move(request_meta)) {}

    /// Records what the client must declare before @p request, a `{"method", "params"}` input
    /// request, may be sent. Returns false when the client declared it, which cannot happen for a
    /// request this class serves.
    bool refuse(const nlohmann::json& request);

    /// What the refused requests needed, or an empty object when none was refused.
    [[nodiscard]] nlohmann::json missing_capabilities() const;

   private:
    std::shared_ptr<const RequestMetadata> request_meta_;
    mutable std::mutex mutex_;
    nlohmann::json missing_ = nlohmann::json::object();
};

/// The capabilities a client must declare before a server may send it @p request, a
/// `{"method", "params"}` input request, or an empty object when it declared them.
nlohmann::json missing_client_capabilities(const nlohmann::json& client_capabilities,
                                           const nlohmann::json& request);

/// Asks the client for input on behalf of the handler of @p entry, and returns the client's
/// response once a retry brings it. Runs on the entry's strand. Throws MrtrAbandoned when the
/// request is abandoned, and also, after reporting it, when the client lacks the capability the
/// input needs. @p chosen_key is the key the handler named the question with, or null for a
/// generated one; a key that is empty, generated-looking or already used in this run throws
/// std::invalid_argument.
Task<nlohmann::json> mrtr_request_input(std::shared_ptr<MrtrEntry> entry, nlohmann::json method,
                                        std::optional<nlohmann::json> params,
                                        nlohmann::json chosen_key);

/// Sends a notification of the handler of @p entry to the request that currently carries it. A
/// progress notification is given that request's progress token, or dropped when it has none.
Task<void> mrtr_notify(std::shared_ptr<MrtrEntry> entry, std::shared_ptr<const std::string> message);

/// Records the handler's answer. Runs on the entry's strand.
void mrtr_complete(const std::shared_ptr<MrtrEntry>& entry, std::string wire);

/// Waits on the entry's strand until the handler completes, parks or needs a capability. When
/// @p forward_cancel is set, a cancellation of @p request_cancelled is passed on to the handler.
Task<MrtrEntry::Outcome> mrtr_await(std::shared_ptr<MrtrEntry> entry,
                                    std::shared_ptr<std::atomic<bool>> request_cancelled,
                                    bool forward_cancel);

/// Hands the client's @p responses to the parked handler and binds it to the retry that carries
/// them. Runs on the entry's strand.
Task<void> mrtr_resume(std::shared_ptr<MrtrEntry> entry, nlohmann::json responses, MessageSender sink,
                       std::optional<nlohmann::json> progress_token,
                       nlohmann::json client_capabilities);

/// Abandons the handler: its pending and later requests for input throw MrtrAbandoned. Safe from
/// any thread.
void mrtr_abandon(const std::shared_ptr<MrtrEntry>& entry);

/// What a sealed `requestState` carries. Sealed with sealed_state::Codec, so only a server with the
/// key ring can write or read it.
struct MrtrSealedState {
    /// Unique to the state, for the single-use hook.
    std::string id;
    /// Milliseconds since the Unix epoch after which the state is refused.
    std::int64_t expires_ms{0};
    /// Digest of the principal that made the request.
    std::string principal_digest;
    std::string method;
    /// mrtr_params_digest() of the originating request.
    std::string params_digest;
    /// Rounds that ended in an input_required result, this state's included.
    std::uint32_t round{0};
    /// The input requests of the last round: each key with the method of its question.
    std::map<std::string, std::string> asked;
    /// The answers of the earlier rounds, by key, each as `{"m": method, "r": response}`.
    nlohmann::json answers = nlohmann::json::object();
};

/// The shortest `requestState` limit (MrtrOptions::max_state_bytes) that set_mrtr_options() accepts.
/// A state that carries one question, a principal digest and a parameters digest seals to fewer
/// characters than this, so a smaller limit would fail every question.
inline constexpr std::size_t g_min_state_chars = 512;

/// A digest of a principal for MrtrSealedState::principal_digest; empty for an unknown one.
std::string mrtr_principal_digest(std::string_view principal);

/// Seals @p state; an empty string when it does not fit the codec's size cap.
std::string mrtr_seal_state(const sealed_state::Codec& codec, const MrtrSealedState& state);

/// Opens @p token; nothing when it was not sealed by a key of the ring, is malformed or has
/// outgrown the size cap.
std::optional<MrtrSealedState> mrtr_open_state(const sealed_state::Codec& codec,
                                               std::string_view token);

/// A digest of the parameters that identify a request across its retries: everything but `_meta`,
/// `inputResponses` and `requestState`.
std::string mrtr_params_digest(const nlohmann::json& params);

}  // namespace mcp::detail
