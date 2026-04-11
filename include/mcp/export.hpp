#pragma once

// MCP_API: Platform-aware export/import macro for DLL visibility
// For static builds (default): empty (no export/import needed)
// For shared builds: visibility attributes or dllexport/dllimport
#if defined(MCP_BUILD_DLL)
#if defined(_WIN32)
#define MCP_API __declspec(dllexport)
#else
#define MCP_API __attribute__((visibility("default")))
#endif
#elif defined(MCP_DLL)
#if defined(_WIN32)
#define MCP_API __declspec(dllimport)
#else
#define MCP_API
#endif
#else
#define MCP_API  // Static build: no export/import needed
#endif

// MCP_INLINE: Empty for compiled mode (functions defined in .cpp files)
#define MCP_INLINE
