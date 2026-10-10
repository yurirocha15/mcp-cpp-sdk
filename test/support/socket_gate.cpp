#include "socket_gate.hpp"

#ifdef __linux__

#include <dlfcn.h>
#include <cerrno>

SocketGate& socket_gate() {
    static SocketGate gate;
    return gate;
}

// Interposed the same way as getaddrinfo() in resolve_gate.cpp, including the forward to a
// sanitizer's interceptor. noexcept because glibc declares it so.
extern "C" int __interceptor_socket(  // NOLINT(bugprone-reserved-identifier)
    int domain, int type, int protocol) noexcept __attribute__((weak));

extern "C" int socket(int domain, int type, int protocol) noexcept {
    using Socket = int (*)(int, int, int) noexcept;
    static const auto next = __interceptor_socket != nullptr
                                 ? &__interceptor_socket
                                 : reinterpret_cast<Socket>(dlsym(RTLD_NEXT, "socket"));
    if (next == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    socket_gate().hold_if_armed(domain, type);
    return next(domain, type, protocol);
}

#endif  // __linux__
