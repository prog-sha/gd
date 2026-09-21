// Validate certificate name grammars without network lookups or public runtime types.
#pragma once
#include <string>
#include <string_view>

namespace GDCrypto {
bool certificate_domain(std::string_view value, bool constraint); // Validate nonempty printable labels, with a leading constraint dot when requested.
bool certificate_mailbox(std::string_view value, std::string &local, std::string_view &domain); // Decode local quoting and preserve the domain for case-aware matching.
bool certificate_uri(std::string_view value, std::string &host); // Validate URI components and extract the decoded authority host, including an optional port.
}
