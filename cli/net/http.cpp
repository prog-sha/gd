/**************************************************************************/
/*  http.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement the HTTP/1.1 listener declared in http.h.
//
// Reduce transport overhead with three techniques.
// Reuse connections instead of repeating a handshake for every response.
// Keep parsed values as offsets and lengths instead of allocating strings.
// Coalesce response framing and cache the date and 200 status line.

#include "modules/gdscript/gdscript_function.h"
#include "cli/net/http.h"
#include "cli/net/http_fields.h"
#include "cli/sys/clock.h"
#include "cli/net/datagram.h"

#include "cli/api/text.h"
#include "cli/data/bytes.h"
#include "cli/sys/limit.h"
#include "cli/sys/perm.h"
#include "cli/sys/sched.h"
#include "cli/sys/task.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/char_utils.h"

#include <stdio.h>
#include <string.h>

namespace {

// Map native TCP EOF to HTTP half-close state while retaining the response direction.
Error http_read(GDServeConn *p_socket, uint8_t *p_data, int p_size, int &r_got) {
	const Error error = p_socket->read(p_data, p_size, r_got);
	return error == ERR_FILE_EOF ? OK : error;
}

const char OK_LINE[] = "HTTP/1.1 200 OK\r\n"; // Prebuilt line for the most common response.
constexpr int OK_LINE_LEN = sizeof(OK_LINE) - 1; // Status-line length excluding the terminating NUL.
// Construct an absolute deadline without overflow.
uint64_t after_ms(uint64_t p_at, uint64_t p_wait) {
	return p_wait > UINT64_MAX - p_at ? UINT64_MAX : p_at + p_wait;
}

// Convert a received UTF-8 slice to String while preserving a leading BOM as content.
String request_text(const uint8_t *p_data, int p_len) {
	if (p_len < 3 || p_data[0] != 0xef || p_data[1] != 0xbb || p_data[2] != 0xbf) {
		return String::utf8((const char *)p_data, p_len);
	}
	CharString guarded;
	guarded.resize_uninitialized(p_len + 2);
	guarded.ptrw()[0] = 'x';
	memcpy(guarded.ptrw() + 1, p_data, p_len);
	guarded.ptrw()[p_len + 1] = 0;
	return String::utf8(guarded.get_data(), guarded.length()).substr(1);
}

// English three-letter names required for Date by RFC 9110.
const char *DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };


// Check characters allowed in a registered Host name.
bool host_reg_char(uint8_t p_c) {
	const bool alpha = (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z');
	const bool digit = p_c >= '0' && p_c <= '9';
	return alpha || digit || strchr("!$&'()*+,-._~;=", p_c) != nullptr;
}

// Accept a registered name, IPv4, or bracketed IPv6, with an optional port.
bool host_value_ok(const uint8_t *p_s, int p_len) {
	if (p_len <= 0) {
		return false;
	}
	int host_end = p_len;
	int port_at = -1;
	if (p_s[0] == '[') {
		int close = 1;
		while (close < p_len && p_s[close] != ']') {
			close++;
		}
		const String inside = close < p_len ? String::utf8((const char *)p_s + 1, close - 1) : String();
		if (close >= p_len || !inside.contains(":") || inside.contains("%") || !GDDatagram::is_ip(inside)) {
			return false;
		}
		host_end = close + 1;
		if (host_end < p_len) {
			if (p_s[host_end] != ':') {
				return false;
			}
			port_at = host_end + 1;
		}
	} else {
		for (int i = 0; i < p_len; i++) {
			if (p_s[i] == ':') {
				if (port_at >= 0) {
					return false; // IPv6 requires brackets.
				}
				host_end = i;
				port_at = i + 1;
			}
		}
		if (host_end <= 0) {
			return false;
		}
		for (int i = 0; i < host_end; i++) {
			const uint8_t c = p_s[i];
			if (c == '%' && i + 2 < host_end && is_hex_digit(p_s[i + 1]) && is_hex_digit(p_s[i + 2])) {
				i += 2;
				continue;
			}
			if (!host_reg_char(c)) {
				return false;
			}
		}
	}
	if (port_at < 0) {
		return true;
	}
	if (port_at >= p_len) {
		return false;
	}
	int port = 0;
	for (int i = port_at; i < p_len; i++) {
		if (p_s[i] < '0' || p_s[i] > '9') {
			return false;
		}
		port = port * 10 + (p_s[i] - '0');
		if (port > Limit::PORT_MAX) {
			return false;
		}
	}
	return port > 0;
}

// Fold one ASCII character to lowercase for case-insensitive comparison.
inline uint8_t lower(uint8_t c) {
	return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

// Compare wire names directly with script text without allocating normalized copies.
// Wire names are ASCII tokens. Fold non-ASCII search characters individually to retain
// existing lookup aliases, while keeping ordinary names out of Unicode conversion.
inline bool header_is(const uint8_t *p_wire, size_t p_len, const String &p_name) {
	if (p_len != (size_t)p_name.length()) return false;
	const char32_t *name = p_name.ptr();
	for (size_t i = 0; i < p_len; i++) {
		const char32_t c = name[i];
		const char32_t folded = c < 128 ? lower((uint8_t)c) : String::char_lowercase(c);
		if (lower(p_wire[i]) != folded) return false;
	}
	return true;
}

// Append bytes to the output buffer.
inline void push(LocalVector<uint8_t> &p_out, const char *p_txt, int p_len) {
	const int at = p_out.size();
	p_out.resize(at + p_len);
	memcpy(p_out.ptr() + at, p_txt, p_len);
}

} // namespace

// Resume the HTTP header-terminator search from its previous position.
int GDWebServer::find_head_end(const uint8_t *p_buf, int p_size, int &r_scan) {
	// Use memchr to find CR and inspect the following three bytes only at matches.
	while (r_scan + 3 < p_size) {
		const uint8_t *hit = (const uint8_t *)memchr(p_buf + r_scan, '\r', p_size - r_scan - 3);
		if (!hit) {
			r_scan = p_size - 3;
			return -1;
		}
		const int at = (int)(hit - p_buf);
		if (p_buf[at + 1] == '\n' && p_buf[at + 2] == '\r' && p_buf[at + 3] == '\n') {
			return at;
		}
		r_scan = at + 1;
	}
	return -1;
}

// Locate a named value in raw HTTP headers.
bool GDWebServer::find_header(const uint8_t *p_buf, int p_at, int p_len, const String &p_name, int &r_at, int &r_len) {
	int i = p_at;
	const int end = p_at + p_len;
	const int name_len = p_name.length();
	bool found = false;
	while (i < end) {
		// Find the line ending.
		int line_end = i;
		while (line_end + 1 < end && !(p_buf[line_end] == '\r' && p_buf[line_end + 1] == '\n')) {
			line_end++;
		}
		if (line_end + 1 >= end) {
			line_end = end;
		}
		// Compare the header name.
		if (line_end - i > name_len && p_buf[i + name_len] == ':') {
			if (header_is(p_buf + i, name_len, p_name)) {
				int v = i + name_len + 1;
				while (v < line_end && (p_buf[v] == ' ' || p_buf[v] == '\t')) {
					v++;
				}
				int ve = line_end;
				while (ve > v && (p_buf[ve - 1] == ' ' || p_buf[ve - 1] == '\t')) {
					ve--;
				}
				r_at = v;
				r_len = ve - v;
				found = true; // Return the last value, matching headers().
			}
		}
		i = line_end + 2;
	}
	return found;
}

// Compare header names case-insensitively.
inline bool name_is(const uint8_t *p_buf, int p_len, const char *p_name, int p_name_len) {
	if (p_len != p_name_len) {
		return false;
	}
	for (int k = 0; k < p_name_len; k++) {
		if (lower(p_buf[k]) != (uint8_t)p_name[k]) {
			return false;
		}
	}
	return true;
}

// Find a token in comma-separated header values.
bool has_token(const uint8_t *p_buf, int p_len, const char *p_name, int p_name_len) {
	int from = 0;
	for (int i = 0; i <= p_len; i++) {
		if (i < p_len && p_buf[i] != ',') {
			continue;
		}
		int a = from;
		int z = i;
		while (a < z && (p_buf[a] == ' ' || p_buf[a] == '\t')) {
			a++;
		}
		while (z > a && (p_buf[z - 1] == ' ' || p_buf[z - 1] == '\t')) {
			z--;
		}
		if (name_is(p_buf + a, z - a, p_name, p_name_len)) {
			return true;
		}
		from = i + 1;
	}
	return false;
}

// Return shared response headers containing the server name and current date.
const char *GDWebServer::now() {
	const int64_t t = (int64_t)GDClock::unix_time();
	if (t == date_at) {
		return date_txt;
	}
	date_at = t;

	// Convert seconds to calendar fields without a dictionary.
	int64_t days = t / 86400;
	int64_t rem = t % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	const int hour = (int)(rem / 3600);
	const int minute = (int)((rem % 3600) / 60);
	const int second = (int)(rem % 60);
	const int wday = (int)((days + 4) % 7); // 1970-01-01 was Thursday.

	// Use a March-based year to avoid a leap-day branch.
	int64_t z = days + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int day = (int)(doy - (153 * mp + 2) / 5 + 1);
	const int month = (int)(mp < 10 ? mp + 3 : mp - 9);
	const int year = (int)(yoe + era * 400 + (month <= 2 ? 1 : 0));

	// Cache server identification and date together for a single copy during output.
	snprintf(date_txt, sizeof(date_txt), "Server: gd\r\nDate: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n",
			DAYS[wday], day, MONTHS[month - 1], year, hour, minute, second);
	return date_txt;
}

// Release per-connection input storage through one shared cleanup path.
// Leave the decision to close the connection itself to the caller.
void GDWebServer::drop(Conn *p_c) {
	p_c->keep = false;
	p_c->half = true;
	p_c->ready = false;
	p_c->body_ready = Callable();
	p_c->buf.reset(); // Release capacity too, avoiding retained memory after repeated disconnects.
	p_c->body_got = 0;
}

// Drain unread data after a complete response and close without resetting the response away.
void GDWebServer::close_sent(Conn *p_c) {
	if (p_c->sock.is_null()) {
		return;
	}
	if (p_c->sock->is_open()) {
		uint8_t buf[4096]; // One nonblocking discard buffer.
		for (int n = 0; n < DRAIN_MAX / int(sizeof(buf)); n++) {
			int got = 0;
			const Error err = http_read(p_c->sock.ptr(), buf, sizeof(buf), got);
			if (err != OK || got <= 0) {
				break;
			}
		}
	}
	p_c->sock->close();
}

// Parse one connection's receive buffer and determine whether a request is ready.
bool GDWebServer::advance(Conn *p_c, uint64_t p_now_ms) {
	const bool had = !p_c->buf.is_empty();
	int head_scan = p_c->scan;
	const int ready_head = p_c->head_end < 0 && had ? find_head_end(p_c->buf.ptr(), p_c->buf.size(), head_scan) : p_c->head_end;
	int read_n = 0;
	// Read available data without first querying its byte count.
	// A separate availability query would double per-connection system calls.
	// A nonblocking read already reports when no data is available.
	// Keep buffer capacity across resizes to avoid allocating on each read.
	const int read_budget = ready_head < 0 ? HEAD_SLOP : 0;
	while (!p_c->half && read_n < read_budget) {
		const int at = p_c->buf.size();
		int64_t room = read_budget - read_n;
		if (p_c->head_end < 0) {
			room = MIN(room, (int64_t)max_head + HEAD_SLOP - at);
		}
		const int want = (int)MIN((int64_t)MIN(READ_CHUNK, read_budget - read_n), room);
		if (want <= 0) {
			break;
		}
		p_c->buf.resize(at + want);
		int got = 0;
		const Error err = http_read(p_c->sock.ptr(), p_c->buf.ptr() + at, want, got);
		if (err == OK && got == 0) {
			// Zero means the peer closed its send side, not that input is temporarily unavailable.
			// Detect EOF promptly instead of retaining a dead peer until timeout.
			// Keep the connection until its response has been flushed:
			// the peer may still be reading while a pending request awaits its response.
			p_c->buf.resize(at);
			p_c->half = true;
			break;
		}
		if (err != OK && err != ERR_BUSY) {
			p_c->buf.resize(at);
			if (!had) {
				p_c->buf.reset(); // Do not retain a large consumed input buffer on the connection.
			}
			drop(p_c); // Clean up the connection after fatal read errors such as RST.
			if (p_c->sock.is_valid()) {
				p_c->sock->close();
			}
			return false;
		}
		if (err != OK || got <= 0) {
			p_c->buf.resize(at);
			break;
		}
		p_c->buf.resize(at + got);
		read_n += got;
		p_c->last = p_now_ms;
		if (got < want) {
			break; // A short read means more input has not arrived yet.
		}
	}

	if (!had && !p_c->buf.is_empty()) {
		if (p_c->reused || p_c->began == 0) {
			p_c->began = p_now_ms; // Start the next keepalive header deadline at its first byte.
		}
	}

	if (p_c->head_end < 0) {
		p_c->head_end = find_head_end(p_c->buf.ptr(), p_c->buf.size(), p_c->scan);
		if (p_c->head_end < 0) {
			if (p_c->half) {
				drop(p_c); // Immediately release a peer that closed midway through its headers.
				if (p_c->sock.is_valid()) {
					p_c->sock->close();
				}
				return false;
			}
			// Enforce both size and configured time boundaries before the terminator arrives.
			// These cover unending headers and slow byte-at-a-time senders.
			if ((int)p_c->buf.size() > max_head) {
				p_c->bad = 431;
				p_c->keep = false;
				return true;
			}
			if (head_ms > 0 && (!p_c->reused || !p_c->buf.is_empty()) && p_now_ms - p_c->began >= head_ms) {
				drop(p_c);
				if (p_c->sock.is_valid()) {
					p_c->sock->close();
				}
			}
			return false;
		}
		if (p_c->head_end > max_head) {
			p_c->bad = 431;
			p_c->keep = false;
			return true;
		}

		// Retain the request line as slices.
		const uint8_t *b = p_c->buf.ptr();
		int i = 0;
		while (i < p_c->head_end && b[i] != ' ') {
			i++;
		}
		p_c->m_at = 0;
		p_c->m_len = i;
		// Do not send a body for HEAD, which would shift connection framing.
		p_c->head_only = (i == 4 && lower(b[0]) == 'h' && lower(b[1]) == 'e' && lower(b[2]) == 'a' && lower(b[3]) == 'd');
		i++;
		int t0 = i;
		while (i < p_c->head_end && b[i] != ' ') {
			i++;
		}
		p_c->t_at = t0;
		p_c->t_len = i - t0;
		// The path precedes the query delimiter.
		int q = t0;
		while (q < t0 + p_c->t_len && b[q] != '?') {
			q++;
		}
		p_c->p_len = q - t0;
		// Validate the routed path separately from query values and their recoverable parsing errors.
		bool target_bad = !Url::decode_check(b + t0, p_c->p_len, false, nullptr);
		for (int k = q + 1; k < t0 + p_c->t_len; k++) {
			if (b[k] < 0x20 || b[k] == 0x7f) { target_bad = true; break; }
		}
		// Locate the version-line ending and accept only HTTP/1.0 or HTTP/1.1.
		i++;
		int line_end = i;
		while (line_end < p_c->head_end && b[line_end] != '\r') {
			line_end++;
		}
		const bool v10 = line_end - i == 8 && memcmp(b + i, "HTTP/1.0", 8) == 0;
		const bool v11 = line_end - i == 8 && memcmp(b + i, "HTTP/1.1", 8) == 0;
		p_c->http10 = v10;
		// The header section starts after the request line.
		p_c->h_at = MIN(line_end + 2, p_c->head_end);
		p_c->h_len = p_c->head_end - p_c->h_at;

		// Validate request-line and header endings in one pass over the same block.
		// A preceding parser that accepts bare CR or LF can disagree about field and body boundaries.
		// That disagreement can split one request into multiple requests.
		bool loose_eol = false; // A non-CRLF ending or folded continuation line was found.
		for (int k = 0; k < p_c->head_end; k++) {
			if (b[k] == '\r' && k + 1 < p_c->head_end && b[k + 1] == '\n') {
				k++; // Treat CRLF as one pair rather than counting LF again.
				continue;
			}
			if (b[k] == '\r' || b[k] == '\n') {
				loose_eol = true;
				break;
			}
		}

		// Extract all relevant headers in one pass.
		// Searching separately by name multiplies field-count and header-kind comparison costs.
		int cl_at = 0, cl_len = 0; // First textual Content-Length value.
		bool has_cl = false; // Whether Content-Length is present.
		bool ws_name = false; // Whitespace before the colon, forbidden by RFC 7230 section 3.2.4.
		int te_n = 0, te_at = 0, te_len = 0; // Transfer-Encoding, restricted to supported chunked framing.
		int host_n = 0, host_at = 0, host_len = 0; // Host value, checked for valid syntax and duplicates.
		int auth_n = 0; // Authentication fields, preventing inconsistent values across APIs.
		int cookie_n = 0; // Session authentication fields, preventing duplicate-dependent interpretation.
		int expect_n = 0, expect_at = 0, expect_len = 0; // Expectation requiring 100 Continue before body reception.
		int field_n = 0; // Header fields to expose in the dictionary.
		bool too_many_fields = false; // Whether the field-count budget requires a 431 response.
		bool bad_head = false; // Whether a field name or value is malformed.
		bool conn_close = false, conn_keep = false; // Aggregate tokens from every Connection field.
		{
			int i2 = p_c->h_at;
			const int hend = p_c->h_at + p_c->h_len;
			while (i2 < hend) {
				if (++field_n > max_fields) {
					too_many_fields = true;
					break;
				}
				// Leading whitespace denotes an obsolete folded continuation.
				// Parsers disagreeing about folding can disagree about request framing.
				if (b[i2] == ' ' || b[i2] == '\t') {
					loose_eol = true;
				}
				int le = i2;
				while (le + 1 < hend && !(b[le] == '\r' && b[le + 1] == '\n')) {
					le++;
				}
				if (le + 1 >= hend) {
					le = hend;
				}
				int colon = i2;
				while (colon < le && b[colon] != ':') {
					colon++;
				}
				if (colon < le) {
					const int nlen = colon - i2;
					if (!http_field_name((const char *)b + i2, nlen)) {
						bad_head = true;
					}
					// Reject whitespace between a header name and colon as required by RFC 7230 section 3.2.4.
					// Ignoring Content-Length : 34 could reinterpret its body as another request.
					// A preceding parser that trims the whitespace would instead honor the length.
					// The resulting boundary disagreement permits request smuggling.
					if (nlen > 0 && (b[colon - 1] == ' ' || b[colon - 1] == '\t')) {
						ws_name = true;
					}
					int v = colon + 1;
					while (v < le && (b[v] == ' ' || b[v] == '\t')) {
						v++;
					}
					int ve = le;
					while (ve > v && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) {
						ve--;
					}
					if (!http_field_value((const char *)b + v, ve - v)) {
						bad_head = true;
					}
					if (name_is(b + i2, nlen, "content-length", 14)) {
						if (!has_cl) {
							cl_at = v;
							cl_len = ve - v;
							has_cl = true;
						} else if (cl_len != ve - v || memcmp(b + cl_at, b + v, cl_len) != 0) {
							bad_head = true; // Reject length values whose trimmed spellings differ.
						}
					} else if (name_is(b + i2, nlen, "transfer-encoding", 17)) {
						te_n++;
						te_at = v;
						te_len = ve - v;
					} else if (name_is(b + i2, nlen, "host", 4)) {
						host_n++;
						host_at = v;
						host_len = ve - v;
					} else if (name_is(b + i2, nlen, "authorization", 13)) {
						auth_n++;
					} else if (name_is(b + i2, nlen, "cookie", 6)) {
						cookie_n++;
					} else if (name_is(b + i2, nlen, "expect", 6)) {
						expect_n++;
						expect_at = v;
						expect_len = ve - v;
					} else if (name_is(b + i2, nlen, "connection", 10)) {
						conn_close = conn_close || has_token(b + v, ve - v, "close", 5);
						conn_keep = conn_keep || has_token(b + v, ve - v, "keep-alive", 10);
					}
				} else {
					bad_head = true; // A line without a colon is not a valid header.
				}
				i2 = le + 2;
			}
		}

		// Validate body lengths strictly because ambiguous framing enables request smuggling.
		// Reject questionable forms with 400 and check representation bounds before narrowing.
		p_c->need = 0;
		p_c->bad = 0;
		if (!v10 && !v11) {
			p_c->bad = 400; // Do not interpret an unknown version as HTTP/1.1.
		} else if (too_many_fields) {
			p_c->bad = 431; // Physical header lines exceeded the configured budget.
		} else if (bad_head || auth_n > 1 || cookie_n > 1 || expect_n > 1) {
			p_c->bad = 400; // Malformed headers or ambiguous authentication values.
		} else if (ws_name) {
			p_c->bad = 400; // Whitespace before the colon could cause framing headers to be ignored.
		} else if (loose_eol) {
			p_c->bad = 400; // Non-CRLF line ending or folded continuation.
		} else if (te_n > 1 || (te_n == 1 && has_cl)) {
			p_c->bad = 400; // Repeated transfer encodings or TE plus CL make body framing ambiguous.
		} else if (te_n == 1 && (!v11 || !name_is(b + te_at, te_len, "chunked", 7))) {
			p_c->bad = 501; // Do not reinterpret unsupported transfer coding as another body format.
		} else if (te_n == 1) {
			p_c->chunked = true;
			p_c->chunk_at = p_c->head_end + 4;
			p_c->chunk_state = 0;
			p_c->chunk_left = 0;
			p_c->chunk_fields = 0;
			p_c->chunk_excess = 0;
		} else if (has_cl) {
			if (cl_len == 0) {
				p_c->bad = 400;
			} else {
				int64_t n = 0;
			for (int k = 0; k < cl_len; k++) {
				if (b[cl_at + k] < '0' || b[cl_at + k] > '9') {
					p_c->bad = 400; // Reject non-digits rather than accepting a numeric prefix.
					break;
				}
				const int digit = b[cl_at + k] - '0';
				// A length outside int64 cannot represent a body boundary.
				if (n > (INT64_MAX - digit) / 10) {
					p_c->bad = 400;
					break;
				}
				n = n * 10 + digit;
			}
			if (p_c->bad == 0) {
				p_c->need = n;
			}
		}
		}

		// Require a complete method, target, and version in the request line.
		if (p_c->bad == 0 && (!http_field_name((const char *)b + p_c->m_at, p_c->m_len) || p_c->t_len == 0 || b[p_c->t_at] != '/' || target_bad)) {
			p_c->bad = 400;
		}
		// Require Host for HTTP/1.1 and validate any Host supplied with HTTP/1.0.
		if (p_c->bad == 0 && (host_n > 1 || (host_n == 1 && !host_value_ok(b + host_at, host_len)) || (!v10 && host_n != 1))) {
			p_c->bad = 400;
		}

		// Give close precedence across all fields; retain HTTP/1.0 only with explicit keepalive.
		p_c->keep = !conn_close && (!v10 || conn_keep);
		if (p_c->bad == 0 && expect_n == 1 && expect_len > 0) {
			if (!v11 || !has_token(b + expect_at, expect_len, "100-continue", 12)) {
				p_c->bad = 417;
				p_c->keep = false;
			} else if (p_c->chunked || p_c->need > 0) {
				p_c->expect_continue = true;
			}
		}
	}
	if (p_c->bad != 0) {
		p_c->need = 0; // Do not wait for the body or reuse a connection with untrusted framing.
		p_c->keep = false;
		return true;
	}
	p_c->body_limit = max_body;
	p_c->body_done = !p_c->chunked && p_c->need == 0;
	p_c->chunk_at = p_c->head_end + 4;
	p_c->began = p_now_ms;
	return true; // Dispatch after headers without waiting for the body.
}

// Send 100 Continue only when body reading starts, releasing the client's send wait.
void GDWebServer::send_continue(Conn *p_c) {
	static const char text[] = "HTTP/1.1 100 Continue\r\n\r\n";
	while (!p_c->continue_sent && p_c->sock.is_valid()) {
		int sent = 0;
		const Error err = p_c->sock->write((const uint8_t *)text + p_c->continue_at, sizeof(text) - 1 - p_c->continue_at, sent);
		if (err == ERR_BUSY || (err == OK && sent == 0)) {
			p_c->sock->write_wait(true);
			return;
		}
		if (err != OK || sent <= 0) {
			p_c->half = true;
			return;
		}
		p_c->continue_at += sent;
		p_c->last = GDClock::msec();
		p_c->continue_sent = p_c->continue_at == int(sizeof(text) - 1);
	}
	if (p_c->continue_sent && p_c->sock.is_valid()) {
		p_c->sock->write_wait(false);
	}
}

// Read requested portions of fixed-length or chunked bodies asynchronously.
int GDWebServer::read_body(Conn *p_c, int64_t p_max, PackedByteArray &r_data, bool p_limit) {
	if (p_c->request) return h2_body(p_c, p_max, r_data, p_limit);
	r_data.clear();
	if (p_limit && p_c->body_limited) {
		return BODY_READ_LIMIT;
	}
	if (p_c->body_done) {
		p_c->body_reading = false;
		return BODY_READ_EOF;
	}
	p_c->body_reading = true;
	if (p_limit && p_c->expect_continue && !p_c->continue_sent) {
		send_continue(p_c);
		if (!p_c->continue_sent) {
			return p_c->half ? BODY_READ_BAD : BODY_READ_WAIT;
		}
	}

	// Compact unread wire bytes behind the header instead of retaining the entire body.
	auto compact = [&]() {
		const int base = p_c->head_end + 4;
		if (p_c->chunk_at <= base) {
			return;
		}
		const int left = p_c->buf.size() - p_c->chunk_at;
		if (left > 0) {
			memmove(p_c->buf.ptr() + base, p_c->buf.ptr() + p_c->chunk_at, left);
		}
		p_c->buf.resize(base + left);
		p_c->chunk_at = base;
	};
	// Read one socket chunk only when needed; yield ERR_BUSY to the next poll.
	auto fill = [&](int p_size = READ_CHUNK) -> int {
		compact();
		const int at = p_c->buf.size();
		p_c->buf.resize(at + p_size);
		int got = 0;
		const Error err = http_read(p_c->sock.ptr(), p_c->buf.ptr() + at, p_size, got);
		if (err == ERR_BUSY) {
			p_c->buf.resize(at);
			return BODY_READ_WAIT;
		}
		if (err != OK || got <= 0) {
			p_c->buf.resize(at);
			p_c->half = true;
			return BODY_READ_BAD;
		}
		p_c->buf.resize(at + got);
		p_c->last = GDClock::msec();
		return BODY_READ_DATA;
	};
	// Batch payload transfers without reading beyond the current body or chunk.
	auto fill_body = [&](int64_t p_left) -> int {
		constexpr int width = 32 * 1024; // Transfer-buffer width; larger bodies continue across reads.
		const int room = INT_MAX - p_c->head_end - 4;
		const int64_t want = MAX(p_max, int64_t(READ_CHUNK)); // Preserve read-ahead for small consumer reads.
		return fill((int)MIN(MIN(want, p_left), int64_t(MIN(width, room))));
	};
	auto fail = [&](int p_status) -> int {
		p_c->bad = p_status;
		p_c->keep = false;
		p_c->body_reading = false;
		return BODY_READ_BAD;
	};

	while (true) {
		if (body_ms > 0 && GDClock::msec() - p_c->began >= body_ms) {
			return fail(408);
		}
		if (!p_c->chunked) {
			if (p_c->body_got == p_c->need) {
				p_c->body_done = true;
				p_c->body_reading = false;
				return BODY_READ_EOF;
			}
			if ((int)p_c->buf.size() == p_c->chunk_at) {
				const int state = fill_body(p_c->need - p_c->body_got);
				if (state != BODY_READ_DATA) {
					return state;
				}
			}
			if (p_limit && p_c->body_got >= p_c->body_limit) {
				p_c->chunk_at++;
				p_c->body_got++;
				p_c->body_limited = true;
				p_c->keep = false;
				p_c->body_reading = false;
				return BODY_READ_LIMIT;
			}
			int64_t take = MIN(int64_t(p_max), p_c->need - p_c->body_got);
			take = MIN(take, int64_t(p_c->buf.size() - p_c->chunk_at));
			if (p_limit) {
				take = MIN(take, p_c->body_limit - p_c->body_got);
			}
			if (r_data.resize_uninitialized(take) != OK) return fail(500);
			memcpy(r_data.ptrw(), p_c->buf.ptr() + p_c->chunk_at, (size_t)take);
			p_c->chunk_at += (int)take;
			p_c->body_got += take;
			return BODY_READ_DATA;
		}

		const int available = p_c->buf.size() - p_c->chunk_at;
		const uint8_t *b = p_c->buf.ptr();
		if (p_c->chunk_state == 0) {
			int end = -1;
			for (int i = p_c->chunk_at; i < (int)p_c->buf.size(); i++) {
				if (b[i] == '\n') {
					return fail(400);
				}
				if (b[i] == '\r') {
					if (i + 1 == (int)p_c->buf.size()) {
						break;
					}
					if (b[i + 1] != '\n') {
						return fail(400);
					}
					end = i;
					break;
				}
			}
			if (end < 0) {
				if (available >= CHUNK_LINE_MAX) {
					return fail(400);
				}
				const int state = fill();
				if (state != BODY_READ_DATA) {
					return state;
				}
				continue;
			}
			const int line = end - p_c->chunk_at;
			if (line <= 0 || line >= CHUNK_LINE_MAX) {
				return fail(400);
			}
			int hex_end = p_c->chunk_at;
			while (hex_end < end && b[hex_end] != ';') {
				hex_end++;
			}
			if (hex_end == p_c->chunk_at || hex_end - p_c->chunk_at > CHUNK_HEX_MAX) {
				return fail(400);
			}
			uint64_t n = 0;
			for (int i = p_c->chunk_at; i < hex_end; i++) {
				const uint8_t c = b[i];
				const int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
				if (d < 0) {
					return fail(400);
				}
				n = (n << 4) | uint64_t(d);
			}
			if (n > uint64_t(INT64_MAX - p_c->body_got)) {
				return fail(413);
			}
			const int64_t credit = n > uint64_t(INT64_MAX / 2) ? INT64_MAX : int64_t(n) * 2;
			const int64_t excess = MAX(int64_t(0), int64_t(p_c->chunk_excess) + line + 2 - 16 - credit);
			if (excess > CHUNK_EXCESS_MAX) {
				return fail(400);
			}
			p_c->chunk_excess = (int)excess;
			p_c->chunk_at = end + 2;
			p_c->chunk_left = (int64_t)n;
			p_c->chunk_state = n == 0 ? 3 : 1;
			if (n == 0) {
				p_c->chunk_trailer = 0;
			}
			continue;
		}
		if (p_c->chunk_state == 1) {
			if (available == 0) {
				const int state = fill_body(p_c->chunk_left);
				if (state != BODY_READ_DATA) {
					return state;
				}
				continue;
			}
			if (p_limit && p_c->body_got >= p_c->body_limit) {
				p_c->chunk_at++;
				p_c->chunk_left--;
				p_c->body_got++;
				p_c->body_limited = true;
				p_c->keep = false;
				p_c->body_reading = false;
				return BODY_READ_LIMIT;
			}
			int64_t take = MIN(int64_t(p_max), p_c->chunk_left);
			take = MIN(take, int64_t(available));
			if (p_limit) {
				take = MIN(take, p_c->body_limit - p_c->body_got);
			}
			if (r_data.resize_uninitialized(take) != OK) return fail(500);
			memcpy(r_data.ptrw(), b + p_c->chunk_at, (size_t)take);
			p_c->chunk_at += (int)take;
			p_c->chunk_left -= take;
			p_c->body_got += take;
			if (p_c->chunk_left == 0) {
				p_c->chunk_state = 2;
			}
			return BODY_READ_DATA;
		}
		if (p_c->chunk_state == 2) {
			if (available < 2) {
				const int state = fill();
				if (state != BODY_READ_DATA) {
					return state;
				}
				continue;
			}
			if (b[p_c->chunk_at] != '\r' || b[p_c->chunk_at + 1] != '\n') {
				return fail(400);
			}
			p_c->chunk_at += 2;
			p_c->chunk_state = 0;
			continue;
		}

		int end = -1;
		for (int i = p_c->chunk_at; i < (int)p_c->buf.size(); i++) {
			if (b[i] == '\n') {
				return fail(400);
			}
			if (b[i] == '\r') {
				if (i + 1 == (int)p_c->buf.size()) {
					break;
				}
				if (b[i + 1] != '\n') {
					return fail(400);
				}
				end = i;
				break;
			}
		}
		if (end < 0) {
			if (p_c->chunk_trailer + available >= TRAILER_MAX) {
				return fail(400);
			}
			const int state = fill();
			if (state != BODY_READ_DATA) {
				return state;
			}
			continue;
		}
		const int line = end - p_c->chunk_at;
		p_c->chunk_trailer += line + 2;
		if (p_c->chunk_trailer > TRAILER_MAX) {
			return fail(400);
		}
		if (line == 0) {
			p_c->chunk_at = end + 2;
			p_c->need = p_c->body_got;
			p_c->body_done = true;
			p_c->body_reading = false;
			return BODY_READ_EOF;
		}
		if (++p_c->chunk_fields > max_fields) {
			return fail(400);
		}
		int colon = p_c->chunk_at;
		while (colon < end && b[colon] != ':') {
			colon++;
		}
		int value = colon + 1;
		while (value < end && (b[value] == ' ' || b[value] == '\t')) {
			value++;
		}
		if (colon == end || !http_field_name((const char *)b + p_c->chunk_at, colon - p_c->chunk_at) || !http_field_value((const char *)b + value, end - value)) {
			return fail(400);
		}
		p_c->chunk_at = end + 2;
	}
}

// Discard a completed request and advance to the next one on the same connection.
void GDWebServer::consume(Conn *p_c) {
	// Do not parse bytes after a rejected request whose framing is untrusted.
	// Treating that remainder as another request could enable request smuggling.
	const int total = p_c->body_done ? p_c->chunk_at : p_c->buf.size();
	const int left = p_c->bad != 0 || !p_c->keep || !p_c->body_done ? 0 : (int)p_c->buf.size() - total;
	if (left > 0) {
		keep_tail(p_c->buf, total, left);
	} else {
		// Reuse small request buffers but release large body capacity even on live connections.
		if (p_c->buf.get_capacity() > BUFFER_REUSE_MAX) {
			p_c->buf.reset();
		} else {
			p_c->buf.clear();
		}
	}
	p_c->scan = 0;
	p_c->head_end = -1;
	p_c->need = 0;
	p_c->body_got = 0;
	p_c->body_limit = max_body;
	p_c->drained = 0;
	p_c->body_done = false;
	p_c->body_limited = false;
	p_c->draining = false;
	p_c->expect_continue = false;
	p_c->continue_sent = false;
	p_c->continue_at = 0;
	p_c->chunked = false;
	p_c->chunk_at = 0;
	p_c->chunk_left = 0;
	p_c->chunk_state = 0;
	p_c->chunk_fields = 0;
	p_c->chunk_trailer = 0;
	p_c->chunk_excess = 0;
	p_c->reply_status = 0;
	p_c->reply_body = PackedByteArray();
	p_c->reply_type = String();
	p_c->reply_headers = Dictionary();
	p_c->reply_waiting = false;
	p_c->bad = 0;
	p_c->head_only = false;
	p_c->ready = false;
	p_c->body_ready = Callable();
	// Clear request-line and header slices after completion.
	// Otherwise later method or target queries would use stale offsets into shifted storage.
	p_c->m_at = 0;
	p_c->m_len = 0;
	p_c->t_at = 0;
	p_c->t_len = 0;
	p_c->p_len = 0;
	p_c->h_at = 0;
	p_c->h_len = 0;
	p_c->reused = true;
	p_c->began = left > 0 ? GDClock::msec() : 0;
}

bool GDWebServer::share_port = false;

// Enqueue the application's serve task once in the runtime ready queue.
void GDWebServer::post_ready() {
	if (polling || ready_posted || !ready_call.is_valid()) {
		return;
	}
	ready_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebServer::dispatch_ready));
}

// Start application processing from the runtime ready queue.
void GDWebServer::dispatch_ready() {
	ready_posted = false;
	if (ready_call.is_valid()) {
		ready_call.call();
	}
}

// Record that the listener has an incoming connection.
void GDWebServer::listener_ready() {
	accept_ready = true;
	post_ready();
}

// Enqueue a connection once in the runnable FIFO.
void GDWebServer::queue_ready(Conn *p_c) {
	if (!p_c || p_c->ready_queued) {
		return;
	}
	p_c->ready_queued = true;
	readyq.push_back(p_c->id);
}

// Mark only kernel-reported ready connections runnable.
void GDWebServer::socket_ready(int p_id) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		return;
	}
	if ((*found)->request) h2_schedule(*found);
	queue_ready(*found);
	post_ready();
}

// Inspect only deadline-bearing connections when the earliest deadline arrives.
void GDWebServer::deadline_ready() {
	due = 0;
	const uint64_t now = GDClock::msec();
	if (!conn_times.is_empty() && conn_times.front()->get().due <= now) {
		const ConnTime timed = conn_times.front()->get();
		conn_times.erase(timed);
		Conn **found = conns.getptr(timed.id);
		if (!found || (*found)->due != timed.due) {
			arm_deadline();
			post_ready();
			return;
		}
		(*found)->due = 0;
		queue_ready(*found);
	}
	arm_deadline();
	post_ready();
}

// Return the header or body deadline appropriate to connection state.
uint64_t GDWebServer::deadline_of(const Conn *p_c) const {
	if (!p_c || p_c->sock.is_null() || !p_c->sock->is_open()) {
		return 0;
	}
	if (!p_c->request && !p_c->ready && p_c->head_end < 0 && head_ms > 0 && (!p_c->reused || !p_c->buf.is_empty())) {
		return after_ms(p_c->began, head_ms);
	}
	if (p_c->ready && !p_c->body_done && p_c->body_reading && body_ms > 0) {
		return after_ms(p_c->began, body_ms);
	}
	return 0;
}

// Update the deadline index only when the connection's scheduling state changes.
void GDWebServer::refresh_deadline(Conn *p_c) {
	if (!p_c) {
		return;
	}
	const uint64_t next = deadline_of(p_c);
	if (next == p_c->due) return;
	if (p_c->due > 0) {
		conn_times.erase(ConnTime{ p_c->due, p_c->id });
	}
	p_c->due = next;
	if (p_c->due > 0) {
		conn_times.insert(ConnTime{ p_c->due, p_c->id });
	}
}

// Rebuild every connection deadline only when timeout settings change.
void GDWebServer::rebuild_deadlines() {
	conn_times.clear();
	for (KeyValue<int, Conn *> &kv : conns) {
		kv.value->due = 0;
		refresh_deadline(kv.value);
	}
	arm_deadline();
}

// Set the application entry point for processing ready connections.
void GDWebServer::set_ready_callback(const Callable &p_call) {
	ready_call = p_call;
	if (accept_ready || ready_at < readyq.size()) {
		post_ready();
	}
}

// Release a connection together with its kernel wait registration.
void GDWebServer::forget(int p_id) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		return;
	}
	Conn *c = *found;
	if (c->h2) {
		while (!c->h2->requests.empty()) forget(c->h2->requests.begin()->second);
	} else if (c->request) {
		Conn *parent = h2_parent(c);
		if (parent) parent->h2->requests.erase(c->request->stream);
		ended_ids.push_back(c->id);
	}
	if (c->due > 0) {
		conn_times.erase(ConnTime{ c->due, c->id });
		c->due = 0;
	}
	abort_files(c);
	if (!c->request && c->sock.is_valid()) {
		c->sock->set_callback(Callable());
		c->sock->close();
	}
	memdelete(c);
	conns.erase(p_id);
}

// Reclaim connections closed after a flushed response without leaving notification waits.
bool GDWebServer::reap_closed(Conn *p_c) {
	if (!p_c || has_output(p_c) || (p_c->sock.is_valid() && p_c->sock->is_open())) {
		return false;
	}
	forget(p_c->id);
	return true;
}

// Start an HTTP listener on the specified host and port.
Ref<R> GDWebServer::listen(int64_t p_port, const String &p_host) {
	// Match the permission-checked port exactly to the 16-bit value used by bind.
	if (p_port < 0 || p_port > Limit::PORT_MAX) {
		return R::err("HTTP listen port must be 0..65535", Err::INVALID_DATA);
	}
	// Check listen permissions before opening the native socket.
	if (p_port == 0) {
		if (!Perm::check_net_any_port(p_host)) {
			return R::err("HTTP listen address is not allowed", Err::PERMISSION_DENIED);
		}
	} else if (!Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		return R::err("HTTP listen address is not allowed", Err::PERMISSION_DENIED);
	}
	stop();
	srv.instantiate();
	const String host = p_host == "*" || p_host.is_empty() ? String("0.0.0.0") : p_host;
	const Error err = srv->listen(host, int(p_port), share_port);
	if (err != OK) {
		srv.unref();
		return R::err(vformat("cannot listen on %s:%d", p_host, p_port), Err::of(err));
	}
	if (p_port == 0 && !Perm::check(Perm::NET, vformat("%s:%d", p_host, get_port()))) {
		srv->close();
		srv.unref();
		return R::err("HTTP listen address is not allowed", Err::PERMISSION_DENIED);
	}
	// Replace the opening callback with the application's listener task.
	srv->read_wait(false);
	srv->set_callback(callable_mp(this, &GDWebServer::listener_ready));
	srv->read_wait(true);
	const Ref<R> failed = srv->wait_error();
	if (failed.is_valid()) {
		srv.unref();
		return failed->note("cannot start HTTP listener");
	}
	return R::ok();
}

// Return the actual listen port, including a kernel-assigned value.
int GDWebServer::get_port() const {
	ERR_FAIL_COND_V(srv.is_null(), 0);
	return srv->addr().get("port", 0);
}

// Keep the response writable after FIN while notifying work that observes request cancellation.
bool GDWebServer::request_alive(int p_id, const Ref<GDAsyncContext> &p_context) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		return false;
	}
	Conn *c = *found;
	if (c->request) return !c->request->dead && h2_parent(c) && c->sock->is_open();
	if (c->sock.is_null() || !c->sock->is_open()) return false;
	if (!c->ready || !c->body_done) {
		return true;
	}
	if (!background_read(c, c->chunk_at, true)) return false;
	if (c->half && p_context.is_valid()) p_context->cancel("peer closed", Err::INTERRUPTED);
	return true;
}

// Keep one background read active without buffering an unbounded pipelined request.
bool GDWebServer::background_read(Conn *c, int p_end, bool p_write_open) {
	if (p_write_open && c->half) return true;
	if ((int)c->buf.size() > p_end) {
		c->sock->read_wait(false);
		return true;
	}
	const int at = c->buf.size();
	c->buf.resize(at + 1);
	int got = 0;
	const Error err = http_read(c->sock.ptr(), c->buf.ptr() + at, 1, got);
	if (err == ERR_BUSY) {
		c->buf.resize(at);
		c->sock->read_wait(true); // Use kernel notifications to detect disconnect during a suspended handler.
		return true;
	}
	if (err == OK && got == 1) {
		c->last = GDClock::msec();
		c->sock->read_wait(false);
		return true; // Retain pipelined input until the current handler finishes.
	}
	c->buf.resize(at);
	c->half = true;
	c->keep = false;
	if (p_write_open && err == OK && got == 0) {
		c->shut = true;
		c->sock->read_wait(false);
		const Ref<GDBodySource> source = c->out_file;
		if (source.is_valid()) source->peer_closed();
		return true;
	}
	if (c->sock.is_valid()) {
		c->sock->close();
	}
	return false;
}

// Report whether response output remains pending.
bool GDWebServer::has_output(const Conn *p_c) const {
	return !p_c->out_buf.is_empty() || !p_c->out_body.is_empty() || p_c->out_file.is_valid();
}

// Detach a successful producer before publishing its response completion.
void GDWebServer::finish_body(Conn *p_c) {
	if (p_c->out_file.is_null()) return;
	p_c->out_file->set_ready_callback(Callable());
	p_c->out_file->finish();
	p_c->out_file.unref();
}

// Cancel connection-owned file reads on their workers.
void GDWebServer::abort_files(Conn *p_c) {
	if (p_c->out_file.is_valid()) {
		p_c->out_file->set_ready_callback(Callable());
		p_c->out_file->abort_sent(p_c->request ? p_c->request->written : p_c->out_body_at);
		p_c->out_file.unref();
	}
	if (p_c->reply_file.is_valid()) {
		p_c->reply_file->set_ready_callback(Callable());
		p_c->reply_file->abort();
		p_c->reply_file.unref();
	}
}

// Close the listener and all connections, releasing retained buffers.
void GDWebServer::stop() {
	Async::drop_deadline(this, due);
	due = 0;
	conn_times.clear();
	// Remove pending connection IDs first to avoid later access to destroyed connections.
	readyq.clear();
	ready_at = 0;
	accept_ready = false;
	accept_turn = true;
	ready_posted = false;
	for (KeyValue<int, Conn *> &kv : conns) {
		abort_files(kv.value);
		if (!kv.value->request && kv.value->sock.is_valid()) {
			kv.value->sock->set_callback(Callable());
			kv.value->sock->close();
		}
		memdelete(kv.value);
	}
	conns.clear();
	ended_ids.clear();
	if (srv.is_valid()) {
		srv->set_callback(Callable());
		srv->close();
		srv.unref();
	}
}

// Close the listener and idle connections while preserving active requests through their responses.
void GDWebServer::begin_shutdown() {
	if (srv.is_valid()) {
		srv->close();
		srv.unref();
	}
	LocalVector<int> idle;
	for (KeyValue<int, Conn *> &kv : conns) {
		Conn *c = kv.value;
		c->keep = false;
		if (c->request) continue;
		if (c->h2 && c->sock->is_open()) {
			c->h2->ending = true;
			c->h2->connection.shutdown();
			queue_ready(c);
			continue;
		}
		if (c->sock.is_null() || !c->sock->is_open()) {
			idle.push_back(c->id); // Disconnected requests cannot respond and are not active shutdown work.
			continue;
		}
		if (!c->ready) {
			c->shut = true;
			if (!has_output(c)) {
				idle.push_back(c->id); // Close already-idle connections without making shutdown wait.
			}
		}
	}
	for (const int id : idle) {
		forget(id);
	}
	arm_deadline();
}

// Report whether the HTTP listener is active.
bool GDWebServer::is_listening() const {
	return srv.is_valid() && srv->is_open();
}

// Advance connections and return IDs whose requests are ready.
PackedInt32Array GDWebServer::poll() {
	PackedInt32Array ready;
	if (polling) {
		return ready;
	}
	polling = true;
	const uint64_t now_us = GDClock::usec();
	const uint64_t now_ms = now_us / 1000;
	const uint64_t outer = GDScriptFunction::native_time_slice_deadline();
	const uint64_t until = outer ? outer : now_us + GD_SCHED_SLICE_USEC;
	// Drain pending accepts within the shared scheduler time slice.
	if (accept_ready && (!ready_call.is_valid() || ready_at >= readyq.size() || accept_turn)) {
		accept_turn = false;
		accept_ready = false;
		while (srv.is_valid()) {
			Ref<GDStream> s;
			if (srv->accept(s) != OK) {
				break;
			}
			Conn *c = memnew(Conn);
			c->sock.instantiate();
			c->sock->start(s, identity);
			c->ip = s->addr(true).get("host", "");
			c->last = now_ms;
			c->began = now_ms;
			c->id = next_id++;
			conns.insert(c->id, c);
			refresh_deadline(c);
			// Replace the empty accept callback with this connection's task.
			c->sock->read_wait(false);
			c->sock->set_callback(callable_mp(this, &GDWebServer::socket_ready).bind(c->id));
			c->sock->read_wait(true);
			queue_ready(c); // Schedule already-arrived request bytes as the next runtime task.
			if (ready_call.is_valid() && GDClock::usec() >= until) {
				accept_ready = true; // Yield remaining backlog entries to the shared ready FIFO.
				break;
			}
		}
	} else {
		accept_turn = true;
	}

	// Rotate runnable connections by time; manual polling retains its full ready snapshot.
	const uint32_t ready_end = readyq.size();
	for (; ready_at < ready_end; ready_at++) {
		if (ready_call.is_valid() && GDClock::usec() >= until) break;
		const int id = readyq[ready_at];
		Conn **found = conns.getptr(id);
		if (!found) {
			continue;
		}
		Conn *c = *found;
		c->ready_queued = false;
		if (c->request) {
			if (c->ready && c->body_reading && !c->body_done && body_ms > 0 && now_ms - c->began >= body_ms) {
				Conn *parent = h2_parent(c);
				if (parent) { parent->h2->connection.reset(c->request->stream); queue_ready(parent); }
				c->request->dead = true;
				c->bad = 408;
				c->body_reading = false;
			}
			const bool dead = c->request->dead || !c->sock->is_open();
			if ((dead || c->body_reading) && c->body_ready.is_valid()) {
				const Callable call = c->body_ready;
				call.call();
			}
			if (dead) { forget(id); continue; }
			if (c->ready) ready.push_back(id);
			refresh_deadline(c);
			continue;
		}
		const bool negotiating = c->sock->handshaking();
		if (negotiating && head_ms > 0 && now_ms - c->began >= head_ms) {
			forget(id);
			continue;
		}
		if (!c->sock->handshake()) {
			if (!c->sock->is_open()) forget(id);
			continue;
		}
		if (negotiating) {
			c->began = now_ms; // Give HTTP headers their own deadline after TLS completes.
			refresh_deadline(c);
		}
		if (!c->h2 && c->sock->protocol() == "h2") {
			GDH2::Config config;
			config.header_size = uint32_t(max_head) + 320;
			config.header_count = max_fields;
			c->h2 = std::make_unique<H2Session>(config);
			if (!c->sock->multiplex_compatible()) c->h2->connection.shutdown(GDH2::INADEQUATE_SECURITY);
		}
		if (c->h2) { h2_poll(c, ready); continue; }
		if (has_output(c)) {
			flush_conn(c);
		}
		const bool spent = c->half && !has_output(c) && !c->ready && c->buf.is_empty();
		if (c->sock.is_null() || !c->sock->is_open() || spent) {
			forget(id);
			continue;
		}
		if (c->ready && c->body_reading && !c->body_done && body_ms > 0 && now_ms - c->began >= body_ms) {
			// Consume deadlines even when a low-level caller stops reading, avoiding a zero-time wait loop.
			c->bad = 408;
			c->keep = false;
			c->body_reading = false;
			if (c->sock.is_valid()) {
				c->sock->close();
			}
			refresh_deadline(c);
			continue;
		}
		if (c->draining) {
			drain_body(c);
			refresh_deadline(c);
			continue;
		}
		if (c->ready) {
			if (c->body_reading && c->body_ready.is_valid()) {
				const Callable call = c->body_ready;
				call.call();
			}
			ready.push_back(id); // Notify a suspended handler of disconnect or pipelined input.
			refresh_deadline(c);
			continue; // Do not parse another HTTP/1 request concurrently with an active handler.
		}
		// Flush the preceding response before starting the next request.
		if (has_output(c)) {
			refresh_deadline(c);
			continue;
		}
		if (!c->ready && advance(c, now_ms)) {
			c->ready = true;
			c->sock->read_wait(false);
			ready.push_back(id);
		} else if (c->half && !has_output(c) && !c->ready && c->buf.is_empty()) {
			forget(id); // Do not retain RST or incomplete-header FIN until another notification.
			continue;
		}
		refresh_deadline(c);
	}
	gd_ready_compact(readyq, ready_at);
	for (const int id : ended_ids) ready.push_back(id);
	ended_ids.clear();
	polling = false;
	arm_deadline();
	if (accept_ready || ready_at < readyq.size()) {
		post_ready();
	}
	return ready;
}

// Pass the earliest header or active body-read deadline to the event loop.
void GDWebServer::arm_deadline() {
	const uint64_t nearest = conn_times.is_empty() ? 0 : conn_times.front()->get().due;
	if (nearest == due) {
		return; // Avoid waking the poller by re-registering an unchanged deadline.
	}
	Async::drop_deadline(this, due);
	due = nearest;
	Async::track_deadline(this, due, callable_mp(this, &GDWebServer::deadline_ready));
}

// Return the HTTP status assigned to a malformed request.
int GDWebServer::bad_of(int p_id) const {
	Conn *const *found = conns.getptr(p_id);
	return found ? (*found)->bad : 0;
}

// Return the request method.
String GDWebServer::get_method(int p_id) const {
	Conn *const *found = conns.getptr(p_id);
	ERR_FAIL_NULL_V(found, String());
	const Conn *c = *found;
	if (c->request) return c->request->method;
	return request_text(c->buf.ptr() + c->m_at, c->m_len);
}

// Recognize the exact HEAD method without allocating its decoded representation.
bool GDWebServer::is_head(int p_id) const {
	Conn *const *found = conns.getptr(p_id);
	if (!found) return false;
	const Conn *c = *found;
	if (c->request) return c->request->method == "HEAD";
	return c->m_len == 4 && memcmp(c->buf.ptr() + c->m_at, "HEAD", 4) == 0;
}

// Return the path component of the request target.
String GDWebServer::get_path(int p_id) const {
	Conn *const *found = conns.getptr(p_id);
	ERR_FAIL_NULL_V(found, String());
	const Conn *c = *found;
	if (c->request) return c->request->path;
	return request_text(c->buf.ptr() + c->t_at, c->p_len);
}

// Return the query component of the request target.
String GDWebServer::get_query(int p_id) const {
	Conn *const *found = conns.getptr(p_id);
	ERR_FAIL_NULL_V(found, String());
	const Conn *c = *found;
	if (c->request) return c->request->query;
	if (c->p_len >= c->t_len) {
		return String();
	}
	return request_text(c->buf.ptr() + c->t_at + c->p_len + 1, c->t_len - c->p_len - 1);
}

// Return the actual peer IP; applications determine which forwarding proxies to trust.
String GDWebServer::get_ip(int p_id) const {
	HashMap<int, Conn *>::ConstIterator it = conns.find(p_id);
	return it ? it->value->ip : String();
}

// Return the named request-header value.
String GDWebServer::get_header(int p_id, const String &p_name) const {
	Conn *const *found = conns.getptr(p_id);
	ERR_FAIL_NULL_V(found, String());
	const Conn *c = *found;
	if (c->request) {
		// Compare the full alias length so a NUL cannot conceal a suffix.
		const bool host = p_name.length() == 4 && p_name.nocasecmp_to("host") == 0;
		for (const GDH2::Field &field : c->request->fields) {
			if (header_is((const uint8_t *)field.name.data(), field.name.size(), p_name) || (host && field.name == ":authority")) return String::utf8(field.value.data(), field.value.size());
		}
		return String();
	}
	int v_at = 0, v_len = 0;
	if (!find_header(c->buf.ptr(), c->h_at, c->h_len, p_name, v_at, v_len)) {
		return String();
	}
	return String::utf8((const char *)c->buf.ptr() + v_at, v_len);
}

// Return request headers as a dictionary with lowercase names.
Dictionary GDWebServer::get_headers(int p_id) const {
	Dictionary out_map;
	Conn *const *found = conns.getptr(p_id);
	ERR_FAIL_NULL_V(found, out_map);
	const Conn *c = *found;
	if (c->request) {
		for (const GDH2::Field &field : c->request->fields) {
			if (field.name == ":authority") out_map["host"] = String::utf8(field.value.data(), field.value.size());
			else if (field.name[0] != ':') out_map[String::utf8(field.name.data(), field.name.size())] = String::utf8(field.value.data(), field.value.size());
		}
		return out_map;
	}
	const uint8_t *b = c->buf.ptr();
	int i = c->h_at;
	const int end = c->h_at + c->h_len;
	while (i < end) {
		int line_end = i;
		while (line_end + 1 < end && !(b[line_end] == '\r' && b[line_end + 1] == '\n')) {
			line_end++;
		}
		if (line_end + 1 >= end) {
			line_end = end;
		}
		int colon = i;
		while (colon < line_end && b[colon] != ':') {
			colon++;
		}
		if (colon < line_end) {
			String key = String::utf8((const char *)b + i, colon - i).to_lower();
			int v = colon + 1;
			while (v < line_end && (b[v] == ' ' || b[v] == '\t')) {
				v++;
			}
			int ve = line_end;
			while (ve > v && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) {
				ve--;
			}
			out_map[key] = String::utf8((const char *)b + v, ve - v);
		}
		i = line_end + 2;
	}
	return out_map;
}

// Read one handler-requested body chunk, distinguishing a wait from EOF.
int GDWebServer::read_body(int p_id, int64_t p_max, PackedByteArray &r_data) {
	Conn **found = conns.getptr(p_id);
	if (!found || !(*found)->ready || (*found)->draining || p_max < 1) {
		r_data.clear();
		return BODY_READ_BAD;
	}
	const int out = read_body(*found, p_max, r_data, true);
	if (out == BODY_READ_WAIT && (*found)->sock.is_valid()) {
		(*found)->sock->read_wait(true);
	}
	refresh_deadline(*found);
	arm_deadline();
	return out;
}

// Attach the internal continuation for the next body arrival.
bool GDWebServer::wait_body(int p_id, const Callable &p_call) {
	Conn **found = conns.getptr(p_id);
	if (!found || !(*found)->ready || !p_call.is_valid()) {
		return false;
	}
	(*found)->body_ready = p_call;
	if ((*found)->sock.is_valid()) {
		(*found)->sock->read_wait(true);
	}
	return true;
}

// Detach a continuation that no longer awaits body data.
void GDWebServer::clear_body_wait(int p_id) {
	Conn **found = conns.getptr(p_id);
	if (found) {
		(*found)->body_ready = Callable();
	}
}

// Read one low-level body chunk, distinguishing wait, data, EOF, and failure.
Dictionary GDWebServer::body_part(int p_id, int64_t p_max) {
	Dictionary out;
	PackedByteArray data;
	if (p_max < 1) {
		out["state"] = "bad";
		return out;
	}
	const int state = read_body(p_id, p_max, data);
	static const char *names[] = { "wait", "data", "eof", "limited", "bad" };
	out["state"] = names[state >= BODY_READ_WAIT && state <= BODY_READ_BAD ? state : BODY_READ_BAD];
	out["data"] = data;
	return out;
}

// Set a per-request body limit, confirming overflow by reading one extra byte.
bool GDWebServer::set_request_body_limit(int p_id, int64_t p_bytes) {
	Conn **found = conns.getptr(p_id);
	if (!found || !(*found)->ready || (*found)->draining || p_bytes < 0) {
		return false;
	}
	(*found)->body_limit = p_bytes;
	return true;
}

// Return the fixed body length, or -1 for an unknown chunked length.
int64_t GDWebServer::get_body_size(int p_id) const {
	Conn *const *found = conns.getptr(p_id);
	return found ? ((*found)->chunked ? -1 : (*found)->need) : 0;
}

// Drain at most 256 KiB left unread by the handler and retain only reusable connections.
void GDWebServer::drain_body(Conn *p_c) {
	if (!p_c->draining) {
		return;
	}
	PackedByteArray data;
	int state = BODY_READ_DATA;
	// Drain already-arrived unread body bytes without returning to a readiness wait.
	// Return to kernel notifications only when socket input is exhausted.
	while (state == BODY_READ_DATA && p_c->drained <= DRAIN_MAX) {
		state = read_body(p_c, DRAIN_MAX + 1 - p_c->drained, data, false);
		if (state == BODY_READ_DATA) {
			p_c->drained += data.size();
		}
	}
	if (state == BODY_READ_WAIT) {
		if (p_c->sock.is_valid()) {
			p_c->sock->read_wait(true);
		}
		return;
	}
	if (state != BODY_READ_EOF || p_c->drained > DRAIN_MAX) {
		p_c->keep = false;
	}
	p_c->draining = false;
	if (p_c->reply_waiting) {
		build_out(p_c, p_c->reply_status, p_c->reply_body, p_c->reply_type, &p_c->reply_headers, true, p_c->reply_file);
		p_c->reply_status = 0;
		p_c->reply_body = PackedByteArray();
		p_c->reply_file.unref();
		p_c->reply_type = String();
		p_c->reply_headers = Dictionary();
		p_c->reply_waiting = false;
	} else {
		consume(p_c);
	}
}

// Drain a small unread body before sending the response.
void GDWebServer::queue_reply(Conn *p_c, int64_t p_status, const PackedByteArray &p_body, const String &p_type, const Dictionary *p_extra) {
	if (p_c->request) { h2_reply(p_c, p_status, p_body, p_type, p_extra, Ref<GDBodySource>()); return; }
	if (p_c->draining) {
		return;
	}
	// If no draining is needed, transmit using the caller's shared body storage.
	if (p_c->body_done || !p_c->keep || p_c->expect_continue || (!p_c->chunked && p_c->need - p_c->body_got >= DRAIN_MAX)) {
		if (!p_c->body_done) {
			p_c->keep = false;
		}
		build_out(p_c, p_status, p_body, p_type, p_extra);
		if (reap_closed(p_c)) {
			arm_deadline();
			return;
		}
		refresh_deadline(p_c);
		arm_deadline();
		return;
	}

	p_c->draining = true;
	p_c->reply_status = p_status;
	p_c->reply_body = p_body;
	p_c->reply_type = p_type;
	p_c->reply_headers = p_extra ? *p_extra : Dictionary();
	p_c->reply_waiting = true;
	drain_body(p_c);
	if (reap_closed(p_c)) {
		arm_deadline();
		return;
	}
	refresh_deadline(p_c);
	arm_deadline();
}

// Drain a small unread body before sending a file response.
void GDWebServer::queue_file_reply(Conn *p_c, int64_t p_status, const Ref<GDBodySource> &p_file, const String &p_type, const Dictionary *p_extra) {
	if (p_c->request) { h2_reply(p_c, p_status, PackedByteArray(), p_type, p_extra, p_file); return; }
	if (p_c->draining) {
		p_file->abort();
		return;
	}
	if (p_c->body_done || !p_c->keep || p_c->expect_continue || (!p_c->chunked && p_c->need - p_c->body_got >= DRAIN_MAX)) {
		if (!p_c->body_done) {
			p_c->keep = false;
		}
		build_out(p_c, p_status, PackedByteArray(), p_type, p_extra, true, p_file);
		if (reap_closed(p_c)) {
			arm_deadline();
			return;
		}
		refresh_deadline(p_c);
		arm_deadline();
		return;
	}

	p_c->draining = true;
	p_c->reply_status = p_status;
	p_c->reply_file = p_file;
	p_c->reply_type = p_type;
	p_c->reply_headers = p_extra ? *p_extra : Dictionary();
	p_c->reply_waiting = true;
	drain_body(p_c);
	if (reap_closed(p_c)) {
		arm_deadline();
		return;
	}
	refresh_deadline(p_c);
	arm_deadline();
}

// Assemble status and headers while preparing the response body for transmission.
void GDWebServer::build_out(Conn *p_c, int64_t p_status, const PackedByteArray &p_body, const String &p_type, const Dictionary *p_extra, bool p_consume, const Ref<GDBodySource> &p_file) {
	// Build only headers in the output buffer and share the original body byte array.
	LocalVector<uint8_t> &out = p_c->out_buf;
	if (p_c->half && p_c->body_done && (int)p_c->buf.size() <= p_c->chunk_at) {
		p_c->keep = false; // Close a half-closed connection after this final response.
	}
	const int64_t body_len = p_file.is_valid() ? p_file->size() : p_body.size();
	int64_t estimate = 512; // Estimated fixed-header size; file bodies are not buffered here.
	bool too_large = false;
	estimate += (int64_t)p_type.length() * 4;
	if (p_extra) {
		for (const KeyValue<Variant, Variant> &kv : *p_extra) {
			const String key = kv.key;
			estimate += (int64_t)key.length() * 4 + 4;
			const Variant::Type vt = kv.value.get_type();
			if (vt == Variant::ARRAY) {
				const Array many = kv.value;
				for (const Variant &one : many) {
					estimate += (int64_t)String(one).length() * 4 + 2;
				}
			} else if (vt == Variant::PACKED_STRING_ARRAY) {
				const PackedStringArray many = kv.value;
				for (const String &one : many) {
					estimate += (int64_t)one.length() * 4 + 2;
				}
			} else {
				estimate += (int64_t)String(kv.value).length() * 4 + 2;
			}
			if (estimate > INT_MAX) {
				too_large = true;
				break;
			}
		}
	}
	too_large = too_large || estimate > INT_MAX;
	if (too_large) {
		// Reject an unrepresentable response before constructing it, closing its connection.
		if (p_file.is_valid()) {
			p_file->abort();
		}
		out.reset();
		consume(p_c);
		p_c->keep = false;
		p_c->shut = true;
		if (p_c->sock.is_valid()) {
			p_c->sock->close();
		}
		return;
	}
	// Reject out-of-range status values without corrupting the status line.
	const int status = p_status >= Limit::RESPONSE_STATUS_MIN && p_status <= Limit::RESPONSE_STATUS_MAX ? int(p_status) : Limit::RESPONSE_STATUS_FALLBACK;
	// Reuse the prebuilt line for the common 200 status.
	if (status == 200) {
		push(out, OK_LINE, OK_LINE_LEN);
	} else {
		push(out, "HTTP/1.1 ", 9);
		push_digits(out, status);
		push(out, " ", 1);
		const char *r = http_reason(status);
		push(out, r, (int)strlen(r));
		push(out, "\r\n", 2);
	}

	bool has_type = false;
	bool has_server = false;
	bool has_date = false;
	uint64_t dropped = 0; // Count omissions without per-field logging.
	http_response_fields(p_extra, dropped, [&](const CharString &key, const String &low, const CharString &value) {
		push(out, key.get_data(), key.length());
		push(out, ": ", 2);
		push(out, value.get_data(), value.length());
		push(out, "\r\n", 2);
		if (low == "content-type") has_type = true;
		else if (low == "server") has_server = true;
		else if (low == "date") has_date = true;
	});
	// Informational headers precede the final response without carrying body framing.
	if (status < 200 && status != 101) {
		push(out, "\r\n", 2);
		build_out(p_c, 200, p_body, p_type, p_extra, p_consume, p_file);
		return;
	}
	// For HEAD, omit the body but report the length it would have had.
	// Omit content type and length for bodyless statuses 204, 205, and 304.
	const bool no_body = p_c->head_only || status < 200 || status == 204 || status == 205 || status == 304;
	p_c->out_suppressed = no_body;
	const bool no_len = status < 200 || status == 204 || status == 205 || status == 304;
	p_c->out_chunked = body_len < 0 && !no_body && !p_c->http10;
	p_c->out_tail = 0;
	if (body_len < 0 && !no_body && p_c->http10) p_c->keep = false;
	if (!has_type && !p_type.is_empty() && !no_len) {
		// Cache the last content type's UTF-8 bytes to avoid repeated wide-string conversion.
		// Convert again only when the content type changes.
		if (p_type != type_memo) {
			type_memo = p_type;
			type_memo_utf = p_type.utf8();
		}
		// Omit the entire content-type field if it contains a line break.
		if (http_field_value(type_memo_utf.get_data(), type_memo_utf.length())) {
			push(out, "Content-Type: ", 14);
			push(out, type_memo_utf.get_data(), type_memo_utf.length());
			push(out, "\r\n", 2);
		} else {
			WARN_PRINT("dropped unsafe Content-Type");
		}
	}

	if (dropped > 0) {
		drop_n += dropped; // Count omissions without attacker-amplifiable per-request logging.
	}

	if (!no_len && body_len >= 0) {
		push(out, "Content-Length: ", 16);
		push_digits(out, body_len);
		push(out, "\r\n", 2);
	}
	if (p_c->out_chunked) push(out, "Transfer-Encoding: chunked\r\n", 28);

	// Copy required server and date fields from the once-per-second cache.
	// The cached prefix is the Server field, followed by the Date field.
	const char *d = now();
	if (!has_server && !has_date) {
		push(out, d, (int)strlen(d));
	} else if (!has_date) {
		push(out, d + SERVER_LEN, (int)strlen(d) - SERVER_LEN);
	} else if (!has_server) {
		push(out, d, SERVER_LEN);
	}

	if (p_c->keep) {
		push(out, "Connection: keep-alive\r\n\r\n", 26);
	} else {
		push(out, "Connection: close\r\n\r\n", 21);
	}

	if ((body_len != 0 || p_file.is_valid()) && !no_body) {
		if (p_file.is_valid()) {
			p_c->out_file = p_file;
			p_c->out_file->set_ready_callback(callable_mp(this, &GDWebServer::socket_ready).bind(p_c->id));
			p_c->out_file->set_write_callback(callable_mp(this, &GDWebServer::flush_source).bind(p_c->id));
		} else {
			p_c->out_body = p_body;
			p_c->out_body_at = 0;
		}
	} else if (p_file.is_valid()) {
		p_file->suppress(); // Stop unused production without completing the response before its headers.
		p_c->out_file = p_file;
	}
	if (p_consume) {
		consume(p_c);
		if (!p_c->keep) {
			p_c->shut = true; // Close after all output has been sent.
		}
	}
	flush_conn(p_c);
}

// Resolve connection ownership before a running producer attempts synchronous transport progress.
void GDWebServer::flush_source(int p_id) {
	Conn **found = conns.getptr(p_id);
	if (found) flush_conn(*found, true);
}

// Flush buffered response data, retaining the remainder when socket capacity is exhausted.
void GDWebServer::flush_conn(Conn *p_c, bool p_producer) {
	if (!has_output(p_c)) {
		if (p_c->sock.is_valid()) {
			p_c->sock->write_wait(false);
		}
		if (p_c->shut && p_c->sock.is_valid()) {
			close_sent(p_c); // Output has been fully sent; close the connection.
		}
		return;
	}
	const uint64_t outer = GDScriptFunction::native_time_slice_deadline();
	const uint64_t slice_due = outer ? outer : GDClock::usec() + GD_SCHED_SLICE_USEC;
	// A borrowed turn may already be exhausted before the first transport attempt.
	if (outer && GDClock::usec() >= outer) {
		queue_ready(p_c);
		post_ready();
		return;
	}
	uint32_t at = 0;
	bool yielded = false;
	// Keep response headers alongside the first producer batch until bytes or EOF are available.
	if (p_c->out_file.is_valid() && !p_c->out_taken && !p_c->out_suppressed) goto next_body;
	// The fresh slice permits the first send; only continuation needs another deadline check.
	goto flush_now;
flush_headers:
	if (GDClock::usec() >= slice_due) {
		queue_ready(p_c);
		post_ready();
		return;
	}
flush_now:
	at = 0;
	yielded = false;
	while (at < p_c->out_buf.size() || p_c->out_body_at < p_c->out_body.size() || p_c->out_tail) {
		int64_t sent = 0;
		const uint32_t head_size = p_c->out_buf.size() - at;
		const int64_t body_size = p_c->out_body.size() - p_c->out_body_at;
		const uint8_t *head = head_size ? p_c->out_buf.ptr() + at : nullptr;
		const uint8_t *tail = reinterpret_cast<const uint8_t *>(p_c->out_final ? "\r\n0\r\n\r\n" : "\r\n") + (p_c->out_final ? 7 : 2) - p_c->out_tail;
		const Error err = p_c->sock->write_parts(GDWrites({head, head_size}, &p_c->out_body, p_c->out_body_at, {tail, p_c->out_tail}), sent);
		if (err == ERR_BUSY || (err == OK && sent == 0)) {
			// The peer's receive buffer is full; repeated immediate retries would stall all serving.
			// Retain unsent output and resume on a later readiness notification.
			p_c->sock->write_wait(true);
			break;
		}
		if (err != OK) {
			p_c->keep = false;
			p_c->shut = true;
			abort_files(p_c);
			p_c->out_body = PackedByteArray();
			p_c->out_body_at = 0;
			p_c->out_tail = 0;
			if (p_c->sock.is_valid()) {
				p_c->sock->close();
			}
			at = p_c->out_buf.size(); // Do not retain bytes that can no longer be transmitted.
			break;
		}
		const uint32_t head_sent = uint32_t(MIN(sent, int64_t(head_size)));
		at += head_sent;
		const int64_t body_sent = MIN(sent - head_sent, body_size);
		p_c->out_body_at += body_sent;
		p_c->out_tail -= int(sent - head_sent - body_sent);
		// A completed batch needs no retry budget; producers are checked before continuing.
		if ((at < p_c->out_buf.size() || p_c->out_body_at < p_c->out_body.size() || p_c->out_tail) && GDClock::usec() >= slice_due) {
			yielded = true;
			break; // Yield to another connection at the scheduler's time-slice boundary.
		}
	}
	if (at >= p_c->out_buf.size()) {
		// Reuse small response buffers and release large capacity when reading resumes.
		if (p_c->out_buf.get_capacity() > BUFFER_REUSE_MAX) {
			p_c->out_buf.reset();
		} else {
			p_c->out_buf.clear();
		}
	} else {
		// Compact only sent headers and retain the remainder for the next turn.
		const uint32_t left = p_c->out_buf.size() - at;
		keep_tail(p_c->out_buf, at, left);
	}
	if (!p_c->out_buf.is_empty() || p_c->out_body_at < p_c->out_body.size() || p_c->out_tail) {
		if (yielded) {
			queue_ready(p_c);
			post_ready();
		}
		return;
	}
	if ((p_c->out_body_at < p_c->out_body.size() || p_c->out_file.is_valid()) && GDClock::usec() >= slice_due) {
		queue_ready(p_c);
		post_ready();
		return;
	}
next_body:
	for (;;) {
		p_c->out_taken = false;
		p_c->out_body.clear();
		p_c->out_body_at = 0;
		if (p_c->out_file.is_null() || p_c->out_suppressed) {
			break;
		}
		BodyChunk chunk;
		if (p_c->out_file->take(chunk)) {
			p_c->out_taken = true;
			p_c->out_body = std::move(chunk);
			p_c->out_final = false;
			if (p_c->out_chunked && !p_c->out_body.is_empty()) {
				char digits[sizeof(uint64_t) * 2]; // One hexadecimal digit for each four length bits.
				int at = sizeof(digits);
				uint64_t left = p_c->out_body.size();
				do {
					digits[--at] = "0123456789abcdef"[left & 15];
					left >>= 4;
				} while (left);
				push(p_c->out_buf, digits + at, sizeof(digits) - at);
				push(p_c->out_buf, "\r\n", 2);
				p_c->out_final = p_c->out_file->last();
				p_c->out_tail = p_c->out_final ? 7 : 2;
				if (p_c->out_final) p_c->out_chunked = false;
			}
			goto flush_headers;
		}
		if (!p_c->out_file->error().is_empty()) {
			p_c->keep = false;
			p_c->shut = true;
			abort_files(p_c);
			if (p_c->sock.is_valid()) {
				p_c->sock->close(); // Close to signal a read failure after a body length was advertised.
			}
			break;
		}
		if (p_c->out_file->done()) {
			if (p_c->out_chunked) {
				p_c->out_chunked = false;
				push(p_c->out_buf, "0\r\n\r\n", 5);
				goto flush_headers;
			}
			break;
		}
		if (!p_producer && p_c->out_file->watch_disconnect() && !background_read(p_c, 0, true)) {
			abort_files(p_c);
			return;
		}
		p_c->sock->write_wait(false); // Disable write polling until producer completion wakes the connection.
		return;
	}
	if (!p_c->out_buf.is_empty()) goto flush_headers;
	finish_body(p_c);
	if (p_c->sock.is_valid()) {
		p_c->sock->write_wait(false);
	}
	if (p_c->shut && p_c->sock.is_valid()) {
		close_sent(p_c);
	} else if (p_c->sock.is_valid()) {
		if (!p_c->buf.is_empty()) {
			queue_ready(p_c); // Return the next pipelined request to this connection's task.
			post_ready();
		} else {
			p_c->sock->read_wait(true);
		}
	}
}

// Queue a byte-array HTTP response on a connection.
void GDWebServer::respond(int p_id, int64_t p_status, const PackedByteArray &p_body, const String &p_type) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		return; // The peer disconnected while the handler was waiting.
	}
	queue_reply(*found, p_status, p_body, p_type, nullptr);
}

// Copy existing memory into a queued HTTP response.
void GDWebServer::respond_bytes(int p_id, int p_status, const uint8_t *p_body, int p_len, const String &p_type) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		return; // The peer disconnected while the handler was waiting.
	}
	PackedByteArray body;
	if (p_len > 0) {
		body.resize(p_len);
		memcpy(body.ptrw(), p_body, p_len);
	}
	queue_reply(*found, p_status, body, p_type, nullptr);
}

// Validate caller-supplied headers and add them to the HTTP response.
void GDWebServer::respond_with(int p_id, int64_t p_status, const Dictionary &p_headers, const PackedByteArray &p_body) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		return; // The peer disconnected while the handler was waiting.
	}
	queue_reply(*found, p_status, p_body, String(), &p_headers);
}

// Stream worker-delivered file chunks as socket capacity becomes available.
void GDWebServer::respond_file(int p_id, int64_t p_status, const Dictionary &p_headers, const Ref<GDBodySource> &p_file, const String &p_type) {
	Conn **found = conns.getptr(p_id);
	if (!found) {
		p_file->abort();
		return; // The peer disconnected while file opening was pending.
	}
	queue_file_reply(*found, p_status, p_file, p_type, &p_headers);
}

// Return the number of retained connections.
int GDWebServer::connection_count() const {
	return (int)conns.size();
}

// Set the body-reader limit applied to all requests.
void GDWebServer::set_body_limit(int64_t p_bytes) {
	ERR_FAIL_COND_MSG(p_bytes < 0 || p_bytes > BODY_LIMIT_MAX, "body limit must be zero or a positive int64");
	max_body = p_bytes;
}

// Limit total request-header bytes and physical field lines.
void GDWebServer::set_header_limits(int64_t p_bytes, int64_t p_values) {
	ERR_FAIL_COND_MSG(p_bytes < 1 || p_bytes > HTTP_HEADER_LIMIT_MAX, "header limit must be between 1 and 2147479551");
	ERR_FAIL_COND_MSG(p_values < 1 || p_values > INT_MAX, "header values must be between 1 and 2147483647");
	max_head = int(p_bytes);
	max_fields = int(p_values);
}

// Set the complete-header deadline; zero explicitly selects no deadline.
void GDWebServer::set_header_timeout(double p_seconds) {
	uint64_t parsed = 0;
	ERR_FAIL_COND_MSG(!Limit::seconds_ms(p_seconds, parsed), "header timeout must be zero or a positive number of seconds");
	head_ms = parsed;
	rebuild_deadlines();
}

// Set the complete-body deadline; zero explicitly selects no deadline.
void GDWebServer::set_body_timeout(double p_seconds) {
	uint64_t parsed = 0;
	ERR_FAIL_COND_MSG(!Limit::seconds_ms(p_seconds, parsed), "body timeout must be zero or a positive number of seconds");
	body_ms = parsed;
	rebuild_deadlines();
}

GDWebServer::~GDWebServer() {
	stop();
}

// Register public script methods and properties.
void GDWebServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("body_limit", "bytes"), &GDWebServer::set_body_limit);
	ClassDB::bind_method(D_METHOD("header_limits", "bytes", "values"), &GDWebServer::set_header_limits);
	ClassDB::bind_method(D_METHOD("listen", "port", "host"), &GDWebServer::listen, DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("port"), &GDWebServer::get_port);
	ClassDB::bind_method(D_METHOD("stop"), &GDWebServer::stop);
	ClassDB::bind_method(D_METHOD("is_listening"), &GDWebServer::is_listening);
	ClassDB::bind_method(D_METHOD("poll"), &GDWebServer::poll);
	ClassDB::bind_method(D_METHOD("get_method", "id"), &GDWebServer::get_method);
	ClassDB::bind_method(D_METHOD("get_path", "id"), &GDWebServer::get_path);
	ClassDB::bind_method(D_METHOD("get_query", "id"), &GDWebServer::get_query);
	ClassDB::bind_method(D_METHOD("get_header", "id", "name"), &GDWebServer::get_header);
	ClassDB::bind_method(D_METHOD("get_headers", "id"), &GDWebServer::get_headers);
	ClassDB::bind_method(D_METHOD("read_body", "id", "bytes"), &GDWebServer::body_part, DEFVAL(32768));
	ClassDB::bind_method(D_METHOD("respond", "id", "status", "body", "content_type"), &GDWebServer::respond, DEFVAL("text/plain; charset=utf-8"));
	ClassDB::bind_method(D_METHOD("respond_with", "id", "status", "headers", "body"), &GDWebServer::respond_with);
	ClassDB::bind_method(D_METHOD("connection_count"), &GDWebServer::connection_count);
	ClassDB::bind_method(D_METHOD("dropped_headers"), &GDWebServer::dropped_headers);
}

// Return a status reason, falling back to its status class for unlisted codes.
// Keep this outside the anonymous namespace because routes also use it.
const char *http_reason(int p_status) {
	switch (p_status) {
		case 200:
			return "OK";
		case 201:
			return "Created";
		case 204:
			return "No Content";
		case 301:
			return "Moved Permanently";
		case 302:
			return "Found";
		case 304:
			return "Not Modified";
		case 400:
			return "Bad Request";
		case 408:
			return "Request Timeout";
		case 401:
			return "Unauthorized";
		case 403:
			return "Forbidden";
		case 404:
			return "Not Found";
		case 405:
			return "Method Not Allowed";
		case 409:
			return "Conflict";
		case 413:
			return "Payload Too Large";
		case 417:
			return "Expectation Failed";
		case 431:
			return "Request Header Fields Too Large";
		case 500:
			return "Internal Server Error";
		case 501:
			return "Not Implemented";
		case 502:
			return "Bad Gateway";
		case 503:
			return "Service Unavailable";
		case 504:
			return "Gateway Timeout";
	}
	// Describe an unlisted code by its status class rather than incorrectly calling it OK.
	switch (p_status / 100) {
		case 1:
			return "Informational";
		case 2:
			return "Success";
		case 3:
			return "Redirection";
		case 4:
			return "Client Error";
		case 5:
			return "Server Error";
	}
	return "Unknown";
}
