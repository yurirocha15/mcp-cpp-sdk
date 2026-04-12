#pragma once

#include <mcp/export.hpp>
#include <mcp/transport.hpp>

#include <memory>
#include <string>

namespace mcp {

class Runtime;

/**
 * @brief Factory for creating MCP transport instances.
 *
 * Use this class to instantiate transports that are bound to a specific
 * Runtime for asynchronous execution.
 */
class MCP_API TransportFactory {
   public:
    /**
     * @brief Construct a factory for a specific runtime.
     *
     * @param runtime The runtime instance that will drive the created transports.
     */
    explicit TransportFactory(Runtime& runtime);

    /**
     * @brief Create a stdio-based transport.
     *
     * @return A unique pointer to the configured stdio transport.
     */
    [[nodiscard]] std::unique_ptr<ITransport> create_stdio() const;

    /**
     * @brief Create an HTTP client transport.
     *
     * @param url The HTTP or HTTPS endpoint URL of the MCP server.
     * @return A unique pointer to the configured HTTP client transport.
     *
     * @throws std::invalid_argument If the provided URL is malformed.
     */
    [[nodiscard]] std::unique_ptr<ITransport> create_http_client(const std::string& url) const;

   private:
    Runtime& runtime_;
};

/**
 * @brief Legacy free-function factory for stdio transport.
 * @deprecated Use TransportFactory::create_stdio instead.
 */
MCP_API std::unique_ptr<ITransport> make_stdio_transport(Runtime& runtime);

/**
 * @brief Legacy free-function factory for HTTP client transport.
 * @deprecated Use TransportFactory::create_http_client instead.
 */
MCP_API std::unique_ptr<ITransport> make_http_client_transport(Runtime& runtime,
                                                               const std::string& url);

}  // namespace mcp
