// The certificate authorities the operating system trusts, as OpenSSL certificates. Each platform
// has its own section: the CA bundle files of Linux and the BSDs, the Keychain trust settings on
// macOS, the ROOT store on Windows. SSL_CERT_FILE and SSL_CERT_DIR, when set, replace them all.

#include "mcp/detail/net/tls.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
// After the OpenSSL headers: wincrypt.h defines names OpenSSL also uses.
#include <wincrypt.h>
#include <windows.h>
#endif

namespace mcp::detail {

namespace {

/// Adds every certificate of the PEM file @p path to @p roots. Returns whether there was one.
bool read_pem_file(const std::string& path, SystemRoots& roots) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_file(path.c_str(), "r"), &BIO_free);
    if (!bio) {
        ERR_clear_error();
        return false;
    }
    bool any = false;
    for (;;) {
        // _AUX also reads the "TRUSTED CERTIFICATE" form some distributions ship.
        X509* certificate = PEM_read_bio_X509_AUX(bio.get(), nullptr, nullptr, nullptr);
        if (certificate == nullptr) {
            break;
        }
        roots.certificates.push_back(certificate);
        any = true;
    }
    ERR_clear_error();
    return any;
}

bool is_directory(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

/// SSL_CERT_FILE and SSL_CERT_DIR (a list, separated as PATH is). Returns whether either is set.
bool read_environment(SystemRoots& roots) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, under the cache's mutex.
    const char* file = std::getenv(X509_get_default_cert_file_env());
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* directories = std::getenv(X509_get_default_cert_dir_env());
    const bool has_file = file != nullptr && *file != '\0';
    const bool has_directories = directories != nullptr && *directories != '\0';
    if (has_file) {
        read_pem_file(file, roots);
    }
    if (has_directories) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        const std::string list = directories;
        std::size_t begin = 0;
        while (begin <= list.size()) {
            const auto end = std::min(list.find(separator, begin), list.size());
            const auto entry = list.substr(begin, end - begin);
            if (!entry.empty() && is_directory(entry)) {
                roots.directories.push_back(entry);
            }
            begin = end + 1;
        }
    }
    return has_file || has_directories;
}

#ifdef __APPLE__

template <typename T>
struct CfRelease {
    void operator()(T ref) const {
        if (ref != nullptr) {
            CFRelease(ref);
        }
    }
};
template <typename T>
using CfHolder = std::unique_ptr<std::remove_pointer_t<T>, CfRelease<T>>;

/// Whether the trust settings of a certificate in the admin or user domain make it a trusted root
/// for TLS: settings that are empty, or name TLS (or no policy) with a root result, and none that
/// deny it.
bool trusted_as_root(SecCertificateRef certificate, SecTrustSettingsDomain domain) {
    CFArrayRef settings = nullptr;
    if (SecTrustSettingsCopyTrustSettings(certificate, domain, &settings) != errSecSuccess) {
        return false;
    }
    const CfHolder<CFArrayRef> holder(settings);
    const CFIndex count = CFArrayGetCount(settings);
    if (count == 0) {
        return true;
    }
    bool trusted = false;
    for (CFIndex index = 0; index < count; ++index) {
        const auto* entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(settings, index));
        auto* policy = static_cast<SecPolicyRef>(
            const_cast<void*>(CFDictionaryGetValue(entry, kSecTrustSettingsPolicy)));
        if (policy != nullptr) {
            const CfHolder<CFDictionaryRef> properties(SecPolicyCopyProperties(policy));
            const auto* oid =
                properties
                    ? static_cast<CFStringRef>(CFDictionaryGetValue(properties.get(), kSecPolicyOid))
                    : nullptr;
            if (oid == nullptr || CFStringCompare(oid, kSecPolicyAppleSSL, 0) != kCFCompareEqualTo) {
                continue;
            }
        }
        SInt32 result = kSecTrustSettingsResultTrustRoot;
        const auto* number =
            static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kSecTrustSettingsResult));
        if (number != nullptr) {
            CFNumberGetValue(number, kCFNumberSInt32Type, &result);
        }
        if (result == kSecTrustSettingsResultDeny) {
            return false;
        }
        if (result == kSecTrustSettingsResultTrustRoot ||
            result == kSecTrustSettingsResultTrustAsRoot) {
            trusted = true;
        }
    }
    return trusted;
}

void read_platform(SystemRoots& roots) {
    const SecTrustSettingsDomain domains[] = {
        kSecTrustSettingsDomainSystem, kSecTrustSettingsDomainAdmin, kSecTrustSettingsDomainUser};
    for (const auto domain : domains) {
        CFArrayRef certificates = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certificates) != errSecSuccess) {
            continue;
        }
        const CfHolder<CFArrayRef> holder(certificates);
        const CFIndex count = CFArrayGetCount(certificates);
        for (CFIndex index = 0; index < count; ++index) {
            auto* certificate = static_cast<SecCertificateRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(certificates, index)));
            if (domain != kSecTrustSettingsDomainSystem && !trusted_as_root(certificate, domain)) {
                continue;
            }
            const CfHolder<CFDataRef> der(SecCertificateCopyData(certificate));
            if (!der) {
                continue;
            }
            const unsigned char* bytes = CFDataGetBytePtr(der.get());
            X509* parsed = d2i_X509(nullptr, &bytes, CFDataGetLength(der.get()));
            if (parsed != nullptr) {
                roots.certificates.push_back(parsed);
            }
        }
    }
    ERR_clear_error();
    if (roots.certificates.empty()) {
        read_pem_file("/etc/ssl/cert.pem", roots);
    }
}

#elif defined(_WIN32)

void read_platform(SystemRoots& roots) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr) {
        return;
    }
    PCCERT_CONTEXT context = nullptr;
    while ((context = CertEnumCertificatesInStore(store, context)) != nullptr) {
        const unsigned char* bytes = context->pbCertEncoded;
        X509* parsed = d2i_X509(nullptr, &bytes, static_cast<long>(context->cbCertEncoded));
        if (parsed != nullptr) {
            roots.certificates.push_back(parsed);
        }
    }
    CertCloseStore(store, 0);
    ERR_clear_error();
}

#else

void read_platform(SystemRoots& roots) {
    // The CA bundles of the common distributions; the first one present is used.
    static constexpr std::array s_bundles{
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS, RHEL 7
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/pki/tls/cacert.pem",                            // OpenELEC
        "/etc/ssl/cert.pem",                                  // Alpine, the BSDs
        "/usr/local/etc/ssl/cert.pem",                        // FreeBSD ports
    };
    bool found = read_pem_file(X509_get_default_cert_file(), roots);
    for (const char* bundle : s_bundles) {
        if (found) {
            break;
        }
        found = read_pem_file(bundle, roots);
    }
    for (const char* directory :
         {X509_get_default_cert_dir(), "/etc/ssl/certs", "/etc/pki/tls/certs"}) {
        if (is_directory(directory)) {
            roots.directories.emplace_back(directory);
        }
    }
}

#endif

}  // namespace

SystemRoots::~SystemRoots() {
    for (X509* certificate : certificates) {
        X509_free(certificate);
    }
}

std::shared_ptr<const SystemRoots> system_roots() {
    static std::mutex s_mutex;
    // Never destroyed: contexts may still be created while the process exits.
    static auto* s_cached = new std::shared_ptr<const SystemRoots>();
    std::scoped_lock lock(s_mutex);
    if (*s_cached) {
        return *s_cached;
    }
    auto roots = std::make_shared<SystemRoots>();
    roots->from_environment = read_environment(*roots);
    if (!roots->from_environment) {
        read_platform(*roots);
    }
    if (!roots->certificates.empty() || !roots->directories.empty()) {
        *s_cached = roots;
    }
    return roots;
}

}  // namespace mcp::detail
