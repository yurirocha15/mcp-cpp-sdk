// Calls a deprecated SDK function. Built only by the mcp-deprecated-call tests: with the
// deprecation attributes on, the call must fail to compile and show the attribute's message.
#include <mcp/server/server.hpp>

mcp::LoggingLevel current_level(const mcp::Server& server) { return server.get_log_level(); }
