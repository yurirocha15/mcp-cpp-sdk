#include <mcp/auth/authenticator.hpp>
#include <mcp/core/async.hpp>

#include <string>

namespace mcp::auth {

Task<bool> Authenticator::try_handle_challenge(const std::string& www_authenticate) {
    static_cast<void>(www_authenticate);
    co_return false;
}

}  // namespace mcp::auth
