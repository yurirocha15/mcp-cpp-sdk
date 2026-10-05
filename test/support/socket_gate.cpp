#include "socket_gate.hpp"

#ifdef __linux__

#include <dlfcn.h>
#include <cerrno>

SocketGate& socket_gate() {
    static SocketGate gate;
    return gate;
}

// Interposed the same way as getaddrinfo() in resolve_gate.cpp. noexcept because glibc declares
// it so.
extern "C" int socket(int domain, int type, int protocol) noexcept {
    using Socket = int (*)(int, int, int);
    static const auto next = reinterpret_cast<Socket>(dlsym(RTLD_NEXT, "socket"));
    if (next == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    socket_gate().hold_if_armed(domain, type);
    return next(domain, type, protocol);
}

#endif  // __linux__
