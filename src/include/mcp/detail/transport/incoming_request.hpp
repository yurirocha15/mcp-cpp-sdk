#pragma once

#include "mcp/detail/constants.hpp"

#include <mcp/transport/http_beast.hpp>

#include <boost/beast/http.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

namespace mcp::detail {

/// Memory for the header fields of the requests one connection reads. Each request takes from a
/// block the arena holds and gives it all back at once when the connection reads the next one, so
/// reading a request allocates nothing for its fields. A header too large for the block takes
/// blocks from the heap, which go back with it.
class HeaderArena {
   public:
    HeaderArena() = default;
    ~HeaderArena() = default;
    HeaderArena(const HeaderArena&) = delete;
    HeaderArena& operator=(const HeaderArena&) = delete;
    HeaderArena(HeaderArena&&) = delete;
    HeaderArena& operator=(HeaderArena&&) = delete;

    /// @p bytes aligned to @p alignment, which is at most that of std::max_align_t.
    void* allocate(std::size_t bytes, std::size_t alignment) {
        void* at = block_.data() + used_;
        std::size_t space = block_.size() - used_;
        if (std::align(alignment, bytes, at, space) != nullptr) {
            used_ = block_.size() - space + bytes;
            return at;
        }
        const std::size_t units = (bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
        return overflow_.emplace_back(units).data();
    }

    /// Gives back everything allocated since the last release. Nothing allocated from the arena
    /// may still be in use.
    void release() noexcept {
        used_ = 0;
        overflow_.clear();
    }

   private:
    alignas(
        std::max_align_t) std::array<std::byte, constants::http_request::g_HEADER_ARENA_BYTES> block_{};
    std::size_t used_{0};
    std::vector<std::vector<std::max_align_t>> overflow_;
};

/// Allocates from a HeaderArena, or from the heap when made without one. Memory from the arena is
/// given back by HeaderArena::release(), not one allocation at a time.
template <class T>
class ArenaAllocator {
   public:
    using value_type = T;
    // A copy of a container, such as a copy of a request, takes heap memory: it may outlive the
    // request the arena serves.
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    ArenaAllocator() noexcept = default;
    explicit ArenaAllocator(HeaderArena& arena) noexcept : arena_(&arena) {}
    template <class U>
    // NOLINTNEXTLINE(google-explicit-constructor): allocators convert implicitly when rebound.
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena_) {}

    T* allocate(std::size_t count) {
        if (arena_ == nullptr) {
            return std::allocator<T>{}.allocate(count);
        }
        static_assert(alignof(T) <= alignof(std::max_align_t));
        return static_cast<T*>(arena_->allocate(count * sizeof(T), alignof(T)));
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        if (arena_ == nullptr) {
            std::allocator<T>{}.deallocate(pointer, count);
        }
    }

    [[nodiscard]] ArenaAllocator select_on_container_copy_construction() const noexcept { return {}; }

    template <class U>
    bool operator==(const ArenaAllocator<U>& other) const noexcept {
        return arena_ == other.arena_;
    }

   private:
    template <class U>
    friend class ArenaAllocator;

    HeaderArena* arena_{nullptr};
};

/// The fields of a request a server reads, allocated from its connection's HeaderArena.
using RequestFields = boost::beast::http::basic_fields<ArenaAllocator<char>>;

/// A request's start line and fields without its body.
using RequestHead = boost::beast::http::request_header<RequestFields>;

/// A request a server reads, its fields in its connection's HeaderArena until the connection reads
/// the next request.
using IncomingRequest = boost::beast::http::request<boost::beast::http::string_body, RequestFields>;

/// Whether the request with @p head asks to keep its connection open, by the rule
/// message::keep_alive() applies, which Beast does not offer for a header alone.
template <class Fields>
bool keeps_alive(const boost::beast::http::header<true, Fields>& head) {
    namespace http = boost::beast::http;
    constexpr unsigned http_1_1 = 11;
    const auto connection = head.find(http::field::connection);
    if (head.version() < http_1_1) {
        return connection != head.end() && http::token_list{connection->value()}.exists("keep-alive");
    }
    return connection == head.end() || !http::token_list{connection->value()}.exists("close");
}

/// @p request with its fields on the heap, for code given a StringRequest.
inline StringRequest to_string_request(const IncomingRequest& request) {
    StringRequest copy;
    copy.method_string(request.method_string());
    copy.target(request.target());
    copy.version(request.version());
    for (const auto& field : request) {
        copy.insert(field.name_string(), field.value());
    }
    copy.body() = request.body();
    return copy;
}

}  // namespace mcp::detail
