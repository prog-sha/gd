// Share address spelling and interface indices between native networking and permission checks.
#pragma once

#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "cli/sys/std.h"

struct sockaddr;
struct sockaddr_storage;
namespace GDAddress {
// Enumerate IPv4 and IPv6 addresses from OS interfaces.
Ref<R> local();
bool equal(const String &p_left, const String &p_right); // Compare address identity, including mapped IPv4.
bool loopback(const String &p_host); // Recognize IPv4 127/8 and IPv6 ::1.
// Convert a numeric IP into a native address, returning its length or zero.
int parse(const String &p_host, int p_port, sockaddr_storage &r_addr, bool p_v6 = false);
// Interpret an IPv6 zone as an index or an OS interface name.
bool zone(const String &p_name, uint32_t &r_index);
// Format a native address as numeric IP, retaining its IPv6 scope.
String text(const sockaddr *p_addr);
}
