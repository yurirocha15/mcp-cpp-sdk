#include <mcp/detail/runtime_access.hpp>
#include <mcp/runtime.hpp>

#include <boost/asio/io_context.hpp>

namespace mcp {

struct Runtime::Impl {
    boost::asio::io_context io_ctx;
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::run() { impl_->io_ctx.run(); }
void Runtime::stop() { impl_->io_ctx.stop(); }

struct RuntimeAccess {
    static boost::asio::any_io_executor get_executor(Runtime& r) {
        return r.impl_->io_ctx.get_executor();
    }
};

namespace detail {

boost::asio::any_io_executor get_executor(Runtime& runtime) {
    return RuntimeAccess::get_executor(runtime);
}

}  // namespace detail
}  // namespace mcp
