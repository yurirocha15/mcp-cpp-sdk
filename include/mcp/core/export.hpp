#pragma once

#ifdef MCP_STATIC
#define MCP_API
#elif defined(MCP_BUILD_DLL)
#ifdef _WIN32
#define MCP_API __declspec(dllexport)
#else
#define MCP_API __attribute__((visibility("default")))
#endif
#elif defined(MCP_DLL)
#ifdef _WIN32
#define MCP_API __declspec(dllimport)
#else
#define MCP_API
#endif
#else
#define MCP_API
#endif

// Marks a declaration deprecated. The message names the proposal that deprecates the feature and
// its earliest removal, or the replacement and the earliest release that may remove it (see
// VERSIONING.md). Defining MCP_DISABLE_DEPRECATION_ATTRIBUTES before including any SDK header turns
// the attribute off, for a build that must use a deprecated feature with -Werror.
#ifdef MCP_DISABLE_DEPRECATION_ATTRIBUTES
#define MCP_DEPRECATED(message)
#else
#define MCP_DEPRECATED(message) [[deprecated(message)]]
#endif
