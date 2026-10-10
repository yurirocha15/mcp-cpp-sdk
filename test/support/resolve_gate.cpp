#include "resolve_gate.hpp"

#ifdef __linux__

#include <dlfcn.h>
#include <netdb.h>

ResolveGate& resolve_gate() {
    static ResolveGate gate;
    return gate;
}

// A sanitizer runtime linked into the executable itself, which is how Clang links them, defines
// its interceptor as a weak getaddrinfo(). The definition below replaces it, and RTLD_NEXT then
// finds libc and skips the interceptor. The runtime exports the interceptor under this second name
// for that case; the reference is null when no such runtime is linked. MemorySanitizer depends on
// it: it learns that the returned list is initialised only by seeing the call.
extern "C" int __interceptor_getaddrinfo(  // NOLINT(bugprone-reserved-identifier)
    const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result)
    __attribute__((weak));

// Interposes the libc symbol for this test binary only. A definition in the executable is found
// ahead of libc by the SDK's getaddrinfo() calls, whether the SDK is linked shared or static;
// everything is forwarded to a sanitizer's interceptor if there is one, and otherwise to the next
// definition in line.
extern "C" int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                           struct addrinfo** result) {
    using GetAddrInfo = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static const auto next = __interceptor_getaddrinfo != nullptr
                                 ? &__interceptor_getaddrinfo
                                 : reinterpret_cast<GetAddrInfo>(dlsym(RTLD_NEXT, "getaddrinfo"));
    if (next == nullptr) {
        return EAI_FAIL;
    }
    resolve_gate().hold_if_armed(service);
    return next(node, service, hints, result);
}

#endif  // __linux__
