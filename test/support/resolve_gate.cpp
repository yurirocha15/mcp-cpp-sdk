#include "resolve_gate.hpp"

#ifdef __linux__

#include <dlfcn.h>
#include <netdb.h>

ResolveGate& resolve_gate() {
    static ResolveGate gate;
    return gate;
}

// Interposes the libc symbol for this test binary only. A definition in the executable is found
// ahead of libc by the SDK's getaddrinfo() calls, whether the SDK is linked shared or static;
// everything is forwarded to the next definition in line, which is libc or a sanitizer's own
// interceptor.
extern "C" int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                           struct addrinfo** result) {
    using GetAddrInfo = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static const auto next = reinterpret_cast<GetAddrInfo>(dlsym(RTLD_NEXT, "getaddrinfo"));
    if (next == nullptr) {
        return EAI_FAIL;
    }
    resolve_gate().hold_if_armed(service);
    return next(node, service, hints, result);
}

#endif  // __linux__
