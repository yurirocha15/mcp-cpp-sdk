#pragma once

// Included ahead of every source of the shared library (see CMakeLists.txt).
//
// Before Boost 1.82, Beast's error categories have no id, so they compare by address, and each
// is a function-local static. Built with hidden visibility, the shared library would keep its
// own copy of each category, and an error it throws would not compare equal to the same Beast
// enumerator in the caller: `error.code() == boost::beast::error::timeout` would be false.
// Declared with default visibility, each category is one object in the whole process, as Asio's
// categories already are (Asio declares its headers with default visibility).

#include <boost/version.hpp>

#if BOOST_VERSION < 108200

// The headers the error headers depend on keep the library's hidden visibility.
#include <boost/beast/core/detail/config.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <type_traits>

#pragma GCC visibility push(default)
#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/zlib/error.hpp>
#pragma GCC visibility pop

#endif
