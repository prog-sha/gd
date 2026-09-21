// Verify endpoint identities against subject alternative names without common-name fallback.
#pragma once
#include "extension_core.h"
#include <string_view>

namespace GDCrypto {
bool certificate_hostname(const CertConstraints &certificate, std::string_view host); // Match numeric addresses or DNS SANs with single-label wildcard rules.
}
