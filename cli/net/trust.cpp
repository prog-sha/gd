// Authenticate explicit certificate paths independently while preserving platform system-trust policy.
#include "cli/net/tls.h"
#include "cli/net/chain_core.h"
#include "cli/net/hostname_core.h"
#include "cli/data/pem_core.h"
#include "cli/sys/clock.h"
#include "cli/sys/os.h"
#include "cli/sys/perm.h"
#include "cli/sys/system.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include <mutex>
#ifdef WINDOWS_ENABLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#elif defined(MACOS_ENABLED)
#include <arpa/inet.h>
#include <Security/Security.h>
#else
#include <arpa/inet.h>
#include <cstdlib>
#include <dirent.h>
#endif
#ifndef WINDOWS_ENABLED
#include <sys/stat.h>
#endif


namespace {
std::once_flag roots_once; // Initialize immutable system trust once per process.
Ref<R> roots_cache; // Retain shared trust until all cryptographic workers have stopped.

// Append independently parsed anchors, accepting exact DER only when no textual certificate was found.
bool anchors(GDCrypto::CertStore &store, GDCrypto::Bytes input) {
	std::string_view text(reinterpret_cast<const char *>(input.data),input.size); GDCrypto::PEM block; bool found = false;
	while (GDCrypto::decode_pem(text,block)) {
		if (block.label != "CERTIFICATE" || block.headers) continue;
		auto cert = GDCrypto::Cert::read({block.bytes.data(),block.bytes.size()});
		if (cert) {store.add(std::move(cert)); found = true;}
	}
	if (!found) {auto cert = GDCrypto::Cert::read(input); if (cert) {store.add(std::move(cert)); found = true;}}
	return found;
}

// Accept only regular certificate files before entering a synchronous reader.
bool regular_file(const String &path) {
#ifdef WINDOWS_ENABLED
	const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16().get_data()));
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_DIRECTORY));
#else
	struct stat info = {};
	return ::stat(path.utf8().get_data(),&info) == 0 && S_ISREG(info.st_mode);
#endif
}

// Read an OS-selected root file without imposing application-file permissions on system configuration.
bool root_file(GDCrypto::CertStore &store, const String &path) {
	if (!regular_file(path)) return false;
	Error error = OK; const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path,&error);
	if (error != OK) return false;
	anchors(store,{bytes.ptr(),size_t(bytes.size())}); return true;
}

// Visit distinct root files while omitting aliases that point to another entry in the same directory.
void root_directory(GDCrypto::CertStore &store, const String &path) {
	Ref<DirAccess> dir = DirAccess::open(path); if (dir.is_null() || dir->list_dir_begin() != OK) return;
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		const bool directory = dir->current_is_dir();
		if (dir->is_link(name)) {const String target = dir->read_link(name); if (!target.is_empty() && target.get_base_dir().is_empty()) continue;}
		if (!directory) root_file(store,path.path_join(name));
	}
	dir->list_dir_end();
}

// Split configured directory lists using native separators and quoted path components where supported.
void root_directories(GDCrypto::CertStore &store, const String &directories) {
	const CharString encoded = directories.utf8(); const std::string_view text(encoded.get_data(),encoded.length());
	std::string path; bool quoted = false;
	for (size_t at = 0; at <= text.size(); ++at) {
#ifdef WINDOWS_ENABLED
		if (at < text.size() && text[at] == '"') {quoted = !quoted; continue;}
		const bool separator = at == text.size() || (text[at] == ';' && !quoted);
#else
		(void)quoted; const bool separator = at == text.size() || text[at] == ':';
#endif
		if (separator) {if (!path.empty()) root_directory(store,String::utf8(path.data(),path.size())); path.clear();}
		else path.push_back(text[at]);
	}
}

// Validate the certificate chain and hostname presented to the native trust engine.
bool system_verify(const std::vector<GDCrypto::Cert::Ptr> &p_chain, const String &p_host, bool p_server) {
#ifdef MACOS_ENABLED
	CFMutableArrayRef certs = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
	if (!certs) return false;
	bool valid = true;
	for (const auto &cert : p_chain) {
		const GDCrypto::Bytes raw = cert->info().raw;
		CFDataRef bytes = CFDataCreate(nullptr, raw.data, raw.size);
		SecCertificateRef item = bytes ? SecCertificateCreateWithData(nullptr, bytes) : nullptr;
		if (bytes) CFRelease(bytes);
		if (!item) { valid = false; break; }
		CFArrayAppendValue(certs, item);
		CFRelease(item);
	}
	const CharString host = p_host.utf8();
	CFStringRef name = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(host.get_data()), host.length(), kCFStringEncodingUTF8, false);
	SecPolicyRef policy = !p_server || name ? SecPolicyCreateSSL(p_server, p_server ? name : nullptr) : nullptr;
	SecTrustRef trust = nullptr;
	valid = valid && policy && SecTrustCreateWithCertificates(certs, policy, &trust) == errSecSuccess;
	if (valid) {
		if (__builtin_available(macOS 10.14, *)) valid = SecTrustEvaluateWithError(trust,nullptr);
		else {
			SecTrustResultType result = kSecTrustResultInvalid;
			#pragma clang diagnostic push
			#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			valid = SecTrustEvaluate(trust,&result) == errSecSuccess && (result == kSecTrustResultProceed || result == kSecTrustResultUnspecified);
			#pragma clang diagnostic pop
		}
	}
	if (trust) CFRelease(trust);
	if (policy) CFRelease(policy);
	if (name) CFRelease(name);
	CFRelease(certs);
	return valid;
#elif defined(WINDOWS_ENABLED)
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
	if (!store) return false;
	PCCERT_CONTEXT leaf = nullptr;
	bool valid = true;
	for (const auto &cert : p_chain) {
		const GDCrypto::Bytes raw = cert->info().raw;
		if (raw.size > MAXDWORD || !CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, raw.data, DWORD(raw.size), CERT_STORE_ADD_ALWAYS, &cert == &p_chain[0] ? &leaf : nullptr)) {
			valid = false;
			break;
		}
	}
	CERT_CHAIN_PARA params = {};
	params.cbSize = sizeof(params);
	LPSTR usage = const_cast<LPSTR>(p_server ? szOID_PKIX_KP_SERVER_AUTH : szOID_PKIX_KP_CLIENT_AUTH);
	params.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
	params.RequestedUsage.Usage.cUsageIdentifier = 1;
	params.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
	PCCERT_CHAIN_CONTEXT chain = nullptr;
	valid = valid && leaf && CertGetCertificateChain(nullptr, leaf, nullptr, store, &params, 0, nullptr, &chain);
	const Char16String host = p_host.utf16();
	SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = {};
	ssl.cbSize = sizeof(ssl);
	ssl.dwAuthType = p_server ? AUTHTYPE_SERVER : AUTHTYPE_CLIENT;
	ssl.pwszServerName = p_server ? reinterpret_cast<wchar_t *>(const_cast<char16_t *>(host.get_data())) : nullptr;
	CERT_CHAIN_POLICY_PARA policy = {};
	policy.cbSize = sizeof(policy);
	policy.pvExtraPolicyPara = &ssl;
	CERT_CHAIN_POLICY_STATUS status = {};
	status.cbSize = sizeof(status);
	if (valid) valid = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) && status.dwError == 0;
	if (chain) CertFreeCertificateChain(chain);
	if (leaf) CertFreeCertificateContext(leaf);
	CertCloseStore(store, 0);
	return valid;
#else
	(void)p_chain;
	(void)p_host;
	(void)p_server;
	return false;
#endif
}
}


// Own the independent anchor index without loading process-global cryptographic configuration.
GDTrust::GDTrust() : roots(std::make_unique<GDCrypto::CertStore>()) {}

// Release owned anchors after the last verification worker has relinquished the store.
GDTrust::~GDTrust() = default;

// Keep explicit CA authority separate from cached platform trust and parse files only on the worker.
Ref<R> GDTrust::load(const String &p_path) {
	if (p_path.is_empty()) {
		std::call_once(roots_once,[]() {
			const Perm::Trusted authority; // Read operator-selected system roots without granting script file access.
			Ref<GDTrust> trust; trust.instantiate();
			const String file = GDSystem::env("SSL_CERT_FILE"), dir = GDSystem::env("SSL_CERT_DIR");
#if defined(MACOS_ENABLED) || defined(WINDOWS_ENABLED)
			if (file.is_empty() && dir.is_empty()) {roots_cache = R::ok(trust); return;}
#endif
			trust->system = false;
			std::vector<String> files;
			String directories = dir;
#if !defined(MACOS_ENABLED) && !defined(WINDOWS_ENABLED)
			files = {"/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt","/etc/ssl/ca-bundle.pem","/etc/pki/tls/cacert.pem","/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem","/etc/ssl/cert.pem"}; // Ordered distribution-provided root bundles.
			if (directories.is_empty()) directories = "/etc/ssl/certs:/etc/pki/tls/certs";
#endif
			if (!file.is_empty()) files = {file};
			for (const auto &path : files) if (root_file(*trust->roots,path)) break;
			root_directories(*trust->roots,directories);
			roots_cache = R::ok(trust);
		});
		return roots_cache;
	}
	if (!Perm::check(Perm::READ,p_path)) return R::err("TLS CA read is not allowed",Err::PERMISSION_DENIED);
	const Ref<R> loaded = Os::read_bytes(p_path); if (!loaded->get_ok()) return loaded;
	const PackedByteArray bytes = loaded->get_v(); Ref<GDTrust> trust; trust.instantiate(); trust->system = false;
	return anchors(*trust->roots,{bytes.ptr(),size_t(bytes.size())}) ? R::ok(trust) : R::err("invalid TLS CA",Err::INVALID_DATA);
}

// Drop cached configuration only after active workers have completed.
void GDTrust::shutdown() { roots_cache.unref(); }
// Publish configured trust names without substituting root enumeration for platform trust decisions.
std::vector<std::vector<uint8_t>> GDTrust::names() const { return roots->names(); }

// Authenticate names and full explicit paths without exposing unverified peers to application I/O.
Ref<R> GDTrust::verify(const std::vector<GDCrypto::Cert::Ptr> &p_chain, const String &p_host, bool p_server) const {
	GDCrypto::Cert::Ptr leaf; GDCrypto::CertStore intermediates;
	bool valid = !p_chain.empty() && (!p_server || !p_host.is_empty());
	for (const auto &cert : p_chain) {
		if (!cert) {valid = false; break;}
		if (!leaf) leaf = cert; else intermediates.add(cert);
	}
	const CharString host = p_host.utf8(); const std::string_view name(host.get_data(),host.length());
	if (valid && system) valid = leaf->key() && (!p_server || GDCrypto::certificate_hostname(leaf->constraints(),name)) && system_verify(p_chain,p_host,p_server);
	else if (valid) {
		GDCrypto::ChainOptions options; options.now = int64_t(GDClock::unix_time()); if (p_server) options.hostname = name;
		const uint8_t client_auth[] = {0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x02}; // Client-authentication extended key usage.
		if (!p_server) options.usages.push_back({client_auth,sizeof(client_auth)});
		GDCrypto::ChainResult result; valid = GDCrypto::verify_chain(std::move(leaf),*roots,intermediates,options,result);
	}
	return valid ? R::ok() : R::err("TLS certificate verification failed",Err::UNAUTHENTICATED);
}
