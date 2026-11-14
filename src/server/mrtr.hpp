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
#include <stdexcept>
#include <string>
#include <string_view>

namespace mcp::detail {

/// Thrown from a parked handler's request for input once the request is abandoned: it expired, was
/// evicted, was cancelled or its server is gone. The handler unwinds; its answer goes nowhere.
class MrtrAbandoned : public std::runtime_error {
   public:
    MrtrAbandoned() : std::runtime_error("the request waiting for client input was abandoned") {}
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
    };

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
    /// The input requests of the current round, by key.
    std::map<std::string, nlohmann::json> requests;
    /// The client's responses of the last retry, by key; each is taken once.
    nlohmann::json responses = nlohmann::json::object();
    nlohmann::json missing_capabilities = nlohmann::json::object();
    std::string wire;
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
/// input needs.
Task<nlohmann::json> mrtr_request_input(std::shared_ptr<MrtrEntry> entry, nlohmann::json method,
                                        std::optional<nlohmann::json> params);

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

/// A digest of the parameters that identify a request across its retries: everything but `_meta`,
/// `inputResponses` and `requestState`.
std::string mrtr_params_digest(const nlohmann::json& params);

}  // namespace mcp::detail
