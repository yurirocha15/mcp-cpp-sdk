#pragma once

// Included ahead of every source of the shared library (see CMakeLists.txt).
//
// nlohmann_json reports errors with exception classes defined in its headers. Built with hidden
// visibility, the shared library would keep its own type_info for each of them. libc++ compares
// type_info by address, so there a caller could not catch by type an exception the library throws,
// such as the `nlohmann::json::type_error` 316 that mcp::detail::dump_json() throws for text that
// is not valid UTF-8. Declared with default visibility, each class is one type in the whole
// process, as Beast's error categories are (see beast_error_visibility.hpp).

#pragma GCC visibility push(default)
#include <nlohmann/json.hpp>
#pragma GCC visibility pop
