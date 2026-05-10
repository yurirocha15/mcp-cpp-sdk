#pragma once

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
#define MCP_API
#endif
