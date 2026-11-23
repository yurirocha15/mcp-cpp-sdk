#pragma once

// OAuth for clients and the bearer token check for servers. Each class has a header of its
// own, which this header includes.

#include <mcp/auth/auth_middleware.hpp>
#include <mcp/auth/authenticator.hpp>
#include <mcp/auth/oauth_authenticator.hpp>
#include <mcp/auth/oauth_authorization_manager.hpp>
#include <mcp/auth/oauth_client_transport.hpp>
#include <mcp/auth/oauth_discovery.hpp>
#include <mcp/auth/oauth_http_client.hpp>
#include <mcp/auth/pkce.hpp>
#include <mcp/auth/token_store.hpp>
