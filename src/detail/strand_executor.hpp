#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/detail/config.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <typeinfo>

namespace mcp::detail {

/// Whether @p executor holds a target of exactly the type @p Target.
///
/// any_executor::target<T>() cannot answer this alone: with Boost 1.74 it returns the stored
/// pointer for every T, not only for the type that is stored, so the type is compared first.
/// Without type information (BOOST_ASIO_NO_TYPEID) the answer is always no.
template <typename Target>
[[nodiscard]] inline bool holds_executor_of_type(const boost::asio::any_io_executor& executor) {
#if !defined(BOOST_ASIO_NO_TYPEID)
    return executor.target_type() == typeid(Target) && executor.target<Target>() != nullptr;
#else
    (void)executor;
    return false;
#endif
}

/// Whether @p executor is a strand: one that runs the handlers it is given one at a time.
[[nodiscard]] inline bool is_strand_executor(const boost::asio::any_io_executor& executor) {
    return holds_executor_of_type<boost::asio::strand<boost::asio::io_context::executor_type>>(
               executor) ||
           holds_executor_of_type<boost::asio::strand<boost::asio::any_io_executor>>(executor);
}

/// A strand over @p base, as the any_io_executor that the SDK's coroutines, timers and sockets take.
///
/// An any_io_executor stores a target of up to three words inline and puts a larger one in a
/// reference-counted heap block. A boost::asio::strand<any_io_executor> is 72 bytes with Boost
/// 1.83, so converting it allocates, and so does every prefer() and require() on the result, which
/// Boost.Asio makes for each asynchronous operation it starts and completes. A strand over the
/// executor of an io_context is 24 bytes with that Boost version, where the executor is one word,
/// so this builds that strand when @p base holds the executor of an io_context: where it fits, the
/// result copies, prefers and requires without allocating. It is the same strand over the same
/// context, with the same semantics, as make_strand() gives. Any other executor gets a
/// boost::asio::strand<any_io_executor>, converted once. With Boost 1.74 both strands are over the
/// inline limit (32 and 64 bytes), so either is held in the heap block there.
///
/// A @p base that is already a strand built here is returned as it is. A strand over a strand runs
/// its handlers inside the inner strand's, one at a time already, so the outer one adds no
/// exclusion, only another queue for every handler to pass through and another heap block for
/// every copy.
inline boost::asio::any_io_executor make_strand_executor(const boost::asio::any_io_executor& base) {
    using IoContextExecutor = boost::asio::io_context::executor_type;
    using IoContextStrand = boost::asio::strand<IoContextExecutor>;

    if (holds_executor_of_type<IoContextStrand>(base)) {
        return base;
    }
    if (holds_executor_of_type<IoContextExecutor>(base)) {
        const auto& io_context_executor = *base.target<IoContextExecutor>();
        return boost::asio::any_io_executor(boost::asio::make_strand(io_context_executor));
    }
    return boost::asio::any_io_executor(boost::asio::make_strand(base));
}

}  // namespace mcp::detail
