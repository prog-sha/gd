// Encode and inspect length-prefixed handshake fields without coupling them to record boundaries.
#pragma once
#include "cli/data/der_core.h"
#include <map>
#include <vector>

namespace GDCrypto {
// Borrow a bounded message region and consume each field transactionally.
class TLSView {
	Bytes rest; // Unconsumed field bytes, never enlarged by a peer-supplied length.
public:
	explicit TLSView(Bytes bytes) : rest(bytes) {} // Borrow a complete message or nested vector.
	bool empty() const { return rest.size == 0; } // Require complete message consumption after parsing.
	Bytes remaining() const { return rest; } // Borrow the unread suffix without changing cursor position.
	bool number(unsigned width, uint32_t &value); // Consume a one-to-four-byte network-order integer.
	bool take(size_t count, Bytes &value); // Consume a fixed-width field only when the entire value is available.
	bool vector(unsigned width, Bytes &value); // Consume a length-prefixed vector as one transaction.
};

// Accumulate one unpublished handshake structure under its encoded field-width constraints.
class TLSWriter {
	std::vector<uint8_t> bytes; // Unpublished message bytes.
	bool valid = true; // Sticky field or length failure, preventing partial publication.
	bool stabilize(Bytes &value, std::vector<uint8_t> &copy); // Preserve aliased input before any vector growth invalidates its address.
public:
	bool number(unsigned width, uint32_t value); // Append an integer only when it fits the selected encoded width.
	bool append(Bytes value); // Append an existing field without retaining its source lifetime.
	bool vector(unsigned width, Bytes value); // Append a vector only when its length fits the protocol prefix.
	bool finish(std::vector<uint8_t> &output); // Publish a complete message and reset successful construction state.
	Bytes view() const { return valid ? Bytes{bytes.data(),bytes.size()} : Bytes{}; } // Inspect an unpublished valid nested structure.
	bool ok() const { return valid; } // Propagate construction failures through enclosing messages.
};

// Borrow exact encoded extensions while rejecting duplicate identifiers, including unknown ones.
using TLSExtensions = std::map<uint16_t,Bytes>;
bool tls_extensions(Bytes bytes, TLSExtensions &output); // Validate complete extension-vector contents without assigning a message-specific policy.

// Retain parsed hello fields until their owning encoded message has been consumed.
struct TLSHello {
	uint16_t version = 0, suite = 0; // Legacy wire version and server-selected suite.
	Bytes random, session, suites, compression; // Borrowed fixed and variable negotiation fields.
	TLSExtensions extensions; // Exact extension contents, without reordered transcript bytes.
	static bool read(Bytes body, bool client, TLSHello &output); // Decode a complete client or server hello without selecting a protocol.
};
}
