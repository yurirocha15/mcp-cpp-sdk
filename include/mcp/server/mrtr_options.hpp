#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mcp/core/constants.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/// Why a request waiting for client input was given up (see MrtrOptions::on_eviction).
enum class MrtrEvictionReason : std::uint8_t {
    eExpired,   ///< The client did not retry within MrtrOptions::ttl.
    eCapacity,  ///< Room was needed for a newer request (MrtrOptions::max_parked,
                ///< max_parked_per_principal, max_parked_bytes and
                ///< max_parked_bytes_per_principal).
};

/// How the state of a request that waits for client input is kept (see MrtrOptions::state_mode).
enum class MrtrStateMode : std::uint8_t {
    /// The handler waits, parked, in the Server instance that answered the first request, and the
    /// retry must reach that instance.
    eStored,
    /// `requestState` carries the state, encrypted and authenticated with MrtrOptions::sealing_keys.
    /// Any Server instance with the key ring serves the retry by running the handler again from the
    /// start with the earlier answers (Context::is_replay()).
    eSealed,
};

/**
 * @brief Limits on the 2026-07-28 requests that wait for client input (see
 *        Server::set_mrtr_options()).
 *
 * @details A tools/call, prompts/get or resources/read of protocol revision 2026-07-28 whose handler
 * asks the client something (Context::elicit(), sample_llm(), request_roots()) is answered with an
 * `input_required` result, and the handler waits until the client retries the request with the
 * answers. Each waiting handler holds its coroutine frame; these limits bound them.
 */
struct MrtrOptions {
    /// How long a handler waits for the client's retry before it is given up.
    std::chrono::milliseconds ttl{mcp::constants::mrtr::g_DEFAULT_TTL};
    /// Handlers that may wait at once; past this the one that has waited longest is given up.
    std::size_t max_parked{mcp::constants::mrtr::g_DEFAULT_MAX_PARKED};
    /// Handlers that may wait at once for one principal (Context::principal()); past this that
    /// principal's longest-waiting handler is given up.
    std::size_t max_parked_per_principal{mcp::constants::mrtr::g_DEFAULT_MAX_PARKED_PER_PRINCIPAL};
    /// Memory the requests of the waiting handlers may take in all, counted as their parsed JSON
    /// takes it (see Server::set_max_in_flight_bytes()); past this the handlers that have waited
    /// longest are given up. The newest handler waits whatever its size.
    std::size_t max_parked_bytes{mcp::constants::mrtr::g_DEFAULT_MAX_PARKED_BYTES};
    /// Memory the requests of one principal's waiting handlers may take, counted as for
    /// max_parked_bytes; past this that principal's handlers that have waited longest are given
    /// up, so one principal's large requests do not give up the handlers of others.
    std::size_t max_parked_bytes_per_principal{
        mcp::constants::mrtr::g_DEFAULT_MAX_PARKED_BYTES_PER_PRINCIPAL};
    /// Called, on no particular thread, each time a waiting handler is given up.
    std::function<void(MrtrEvictionReason)> on_eviction;

    /// Where the state of a waiting request lives. The limits above bound stored state only.
    MrtrStateMode state_mode{MrtrStateMode::eStored};
    /// Sealed state: secrets of at least 32 bytes, newest first. The first seals; every one opens,
    /// so a key can be rotated in by putting a new one first and kept until the states sealed under
    /// the old one have expired. Each secret is hashed to the AES-256-GCM master key, and every
    /// state derives its own key from it with HKDF-SHA256. Server::set_mrtr_options() throws
    /// std::invalid_argument when sealed mode has no key or a key is too short.
    std::vector<std::string> sealing_keys;
    /// Sealed state: how long a state is accepted after the round that sealed it. The expiry is a
    /// wall-clock time, so server instances that share keys need reasonably synchronized clocks.
    std::chrono::milliseconds state_ttl{mcp::constants::mrtr::g_DEFAULT_STATE_TTL};
    /// Sealed state: the most rounds of input one request may take, at least 1; a request that
    /// asks for more is answered with an internal error.
    std::size_t max_rounds{mcp::constants::mrtr::g_DEFAULT_MAX_ROUNDS};
    /// Sealed state: the longest `requestState` the server writes or accepts, in characters, at
    /// least 512. A request whose state outgrows it is answered with an internal error.
    std::size_t max_state_bytes{mcp::constants::mrtr::g_DEFAULT_MAX_STATE_BYTES};
    /// Sealed state: called with the unique id of a state before its retry is served. Returning
    /// false (or throwing) refuses the retry as an unknown requestState, so a deployment that must
    /// not serve a state twice can record the ids it has seen. The id is not secret. May run on any
    /// thread.
    ///
    /// Optional, but without it a state can be sent again until it expires, and each time the
    /// handler runs again from the start with the same answers: the steps after its last question
    /// run once more. Make those steps idempotent, or set this hook.
    std::function<bool(std::string_view)> consume;
};

/**
 * @brief Counters of the 2026-07-28 requests that wait for client input (see Server::mrtr_stats()).
 */
struct MrtrStats {
    std::uint64_t parked = 0;       ///< Handlers waiting for a retry now.
    std::uint64_t evicted_ttl = 0;  ///< Handlers given up because the retry did not come in time.
    std::uint64_t evicted_cap = 0;  ///< Handlers given up to make room for newer ones.
    /// Handlers that went on after being given up and finished; their answers went nowhere.
    std::uint64_t abandoned_completed = 0;
    std::uint64_t resume_hit = 0;  ///< Retries that resumed their handler.
    /// Retries refused because their requestState names no waiting handler (unknown, expired or
    /// already used).
    std::uint64_t resume_unknown = 0;
    /// Retries refused because another principal sent the request that parked the handler.
    std::uint64_t resume_principal_mismatch = 0;
    /// Retries refused because they are not the request that parked the handler: another method,
    /// or other parameters.
    std::uint64_t resume_params_mismatch = 0;
};

}  // namespace mcp
