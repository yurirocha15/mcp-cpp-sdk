#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/capabilities.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

/** @brief The protocol era a client session speaks. */
enum class ProtocolEra : std::uint8_t {
    eLegacy,  ///< 2025-11-25 and earlier: an initialize handshake starts a session.
    eModern,  ///< 2026-07-28 and later: every request carries its protocol fields in `_meta`.
};

/**
 * @brief Which protocol a Client speaks, chosen with one of the factory functions.
 *
 * @details One value instead of a mode plus a version, so a contradictory combination (a modern
 * mode with a legacy version) cannot be written.
 */
class MCP_API ClientProtocol {
   public:
    /** @brief What the value selects. */
    enum class Kind : std::uint8_t {
        eAuto,     ///< Probe with server/discover, fall back to initialize.
        eLegacy,   ///< initialize with the newest legacy version, no probe.
        eVersion,  ///< Exactly the version named, no probe.
    };

    /** @brief Equivalent to automatic(). */
    ClientProtocol() = default;

    /**
     * @brief Detect the server's era: send server/discover first, fall back to initialize.
     *
     * @details The server is modern when it answers with a DiscoverResult or with a modern
     * protocol error (mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
     * mcp::constants::jsonrpc::g_HEADER_MISMATCH,
     * mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY); any other error, a reply that is
     * not JSON, or no reply within ClientOptions::probe_timeout makes it legacy, and the client sends
     * initialize. A server that exits when it receives the probe fails the connect with an error that
     * names legacy() as the remedy.
     *
     * Over HTTP the probe is a 2026-07-28 POST. A legacy server answers it with an error status
     * (400 without a modern protocol error in the body, 404 or 405), and the client falls back to
     * initialize.
     */
    [[nodiscard]] static ClientProtocol automatic();

    /** @brief initialize with the newest legacy version (2025-11-25); sends no probe. */
    [[nodiscard]] static ClientProtocol legacy();

    /**
     * @brief Speak exactly one version.
     *
     * @details "2025-11-25", "2025-06-18", "2025-03-26" and "2024-11-05" start a legacy session
     * with initialize naming that version. "2026-07-28" speaks the modern protocol; connect()
     * then sends one server/discover to fill ConnectResult and never falls back to initialize.
     *
     * @throws std::invalid_argument For any other string.
     */
    [[nodiscard]] static ClientProtocol pinned(std::string version);

    /** @brief What this value selects. */
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    /** @brief The pinned version for pinned(), the newest legacy version for legacy(), else empty. */
    [[nodiscard]] const std::string& version() const noexcept { return version_; }

   private:
    ClientProtocol(Kind kind, std::string version) : kind_(kind), version_(std::move(version)) {}

    Kind kind_{Kind::eAuto};
    std::string version_;
};

/** @brief Why connect() settled on an era. */
enum class EraReason : std::uint8_t {
    ePinned,                  ///< ClientOptions::protocol named the era; nothing was probed.
    eDiscoverOk,              ///< server/discover returned a DiscoverResult.
    eModernErrorRetry,        ///< server/discover returned a modern protocol error.
    eFallbackNonModernError,  ///< server/discover failed with an error that is not a modern one.
    eFallbackTimeout,         ///< The server did not answer server/discover in time.
    eReprobeAfterInitializeRejected,  ///< initialize was rejected naming modern versions; probed again.
};

/** @brief One era decision of connect(), as reported to ClientOptions::on_era_decision. */
struct EraDecision {
    ProtocolEra era;                    ///< The era the client goes on to speak.
    EraReason reason;                   ///< What led to it.
    std::chrono::milliseconds elapsed;  ///< Time from the start of connect() to the decision.
};

/** @brief What connect() learned about the server. */
struct ConnectResult {
    ProtocolEra era{
        ProtocolEra::eLegacy};  ///< The era of this session; fixed for the client's lifetime.
    /// The version in use: the one initialize negotiated (legacy) or the one every request names.
    std::string protocolVersion;
    ServerCapabilities capabilities;           ///< Capabilities the server declared.
    std::optional<Implementation> serverInfo;  ///< The server's identity, when it sent one.
    std::optional<std::string> instructions;   ///< The server's instructions, when it sent any.
    /// Versions the server listed in server/discover or in a protocol error; empty when it was
    /// never asked or did not say.
    std::vector<std::string> supportedVersions;
};

}  // namespace mcp
