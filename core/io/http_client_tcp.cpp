/**************************************************************************/
/*  http_client_tcp.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// Handle HTTP/1 connections over TCP and validate message framing.

#ifndef WEB_ENABLED

#include "http_client_tcp.h"

#include "cli/sys/perm.h"

#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_tls.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/version.h"

// Parse complete decimal Content-Length values and reject overflow.
static bool read_length(const String &p_text, int64_t &r_value) {
	const String text = p_text.strip_edges();
	if (text.is_empty()) {
		return false;
	}
	int64_t value = 0;
	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		if (c < '0' || c > '9' || value > (INT64_MAX - (c - '0')) / 10) {
			return false;
		}
		value = value * 10 + (c - '0');
	}
	r_value = value;
	return true;
}

// Determine whether an ASCII character is valid in an HTTP header-name token.
static bool response_name_char(uint8_t p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || (p_c >= '0' && p_c <= '9') ||
			p_c == '!' || p_c == '#' || p_c == '$' || p_c == '%' || p_c == '&' || p_c == '\'' || p_c == '*' ||
			p_c == '+' || p_c == '-' || p_c == '.' || p_c == '^' || p_c == '_' || p_c == '`' || p_c == '|' || p_c == '~';
}

// Compare a byte header name with a fixed name without case sensitivity.
static bool response_name_is(const uint8_t *p_text, int p_len, const char *p_name) {
	const int name_len = (int)strlen(p_name);
	if (p_len != name_len) {
		return false;
	}
	for (int i = 0; i < p_len; i++) {
		const uint8_t c = p_text[i] >= 'A' && p_text[i] <= 'Z' ? p_text[i] + 32 : p_text[i];
		if (c != (uint8_t)p_name[i]) {
			return false;
		}
	}
	return true;
}

// Parse trimmed Content-Length values without overflow.
static bool response_length(const uint8_t *p_text, int p_len, int64_t &r_value) {
	int from = 0;
	int to = p_len;
	while (from < to && (p_text[from] == ' ' || p_text[from] == '\t')) {
		from++;
	}
	while (to > from && (p_text[to - 1] == ' ' || p_text[to - 1] == '\t')) {
		to--;
	}
	if (from == to) {
		return false;
	}
	int64_t value = 0;
	for (int i = from; i < to; i++) {
		if (p_text[i] < '0' || p_text[i] > '9' || value > (INT64_MAX - (p_text[i] - '0')) / 10) {
			return false;
		}
		value = value * 10 + (p_text[i] - '0');
	}
	r_value = value;
	return true;
}

// Determine whether trimmed Transfer-Encoding contains only chunked.
static bool response_chunked(const uint8_t *p_text, int p_len) {
	int from = 0;
	int to = p_len;
	while (from < to && (p_text[from] == ' ' || p_text[from] == '\t')) {
		from++;
	}
	while (to > from && (p_text[to - 1] == ' ' || p_text[to - 1] == '\t')) {
		to--;
	}
	return response_name_is(p_text + from, to - from, "chunked");
}

HTTPClient *HTTPClientTCP::_create_func(bool p_notify_postinitialize) {
	return static_cast<HTTPClient *>(ClassDB::creator<HTTPClientTCP>(p_notify_postinitialize));
}

Error HTTPClientTCP::connect_to_host(const String &p_host, int p_port, Ref<TLSOptions> p_options) {
	close();

	conn_port = p_port;
	conn_host = p_host;
	tls_options = p_options;

	ip_candidates.clear();

	String host_lower = conn_host.to_lower();
	if (host_lower.begins_with("http://")) {
		conn_host = conn_host.substr(7);
		tls_options.unref();
	} else if (host_lower.begins_with("https://")) {
		if (tls_options.is_null()) {
			tls_options = TLSOptions::client();
		}
		conn_host = conn_host.substr(8);
	}

	ERR_FAIL_COND_V(tls_options.is_valid() && tls_options->is_server(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(tls_options.is_valid() && !StreamPeerTLS::is_available(), ERR_UNAVAILABLE, "HTTPS is not available in this build.");
	ERR_FAIL_COND_V(conn_host.length() < HOST_MIN_LEN, ERR_INVALID_PARAMETER);

	if (conn_port < 0) {
		if (tls_options.is_valid()) {
			conn_port = PORT_HTTPS;
		} else {
			conn_port = PORT_HTTP;
		}
	}

	connection = tcp_connection;

	if (tls_options.is_valid() && https_proxy_port != -1) {
		proxy_client.instantiate(); // Needs proxy negotiation.
		server_host = https_proxy_host;
		server_port = https_proxy_port;
	} else if (tls_options.is_null() && http_proxy_port != -1) {
		server_host = http_proxy_host;
		server_port = http_proxy_port;
	} else {
		server_host = conn_host;
		server_port = conn_port;
	}

	// Authorize the destination before resolution; proxies require both final-target and proxy authorization.
	GD_PERM_FAIL_V(NET, vformat("%s:%d", conn_host, conn_port), ERR_UNAUTHORIZED); // Authorize the final destination.
	if (server_host != conn_host || server_port != conn_port) {
		GD_PERM_FAIL_V(NET, vformat("%s:%d", server_host, server_port), ERR_UNAUTHORIZED); // Authorize the proxy destination.
	}

	if (server_host.is_valid_ip_address()) {
		// Host contains valid IP.
		Error err = tcp_connection->connect_to_host(IPAddress(server_host), server_port);
		if (err) {
			status = STATUS_CANT_CONNECT;
			return err;
		}
		tcp_connection->set_write_wait_enabled(true); // Await asynchronous connection completion.

		status = STATUS_CONNECTING;
	} else {
		// Host contains hostname and needs to be resolved to IP.
		resolving = IP::get_singleton()->resolve_hostname_queue_item(server_host);
		if (resolving == IP::RESOLVER_INVALID_ID) {
			status = STATUS_CANT_RESOLVE;
			return ERR_CANT_RESOLVE;
		}
		status = STATUS_RESOLVING;
	}

	return OK;
}

void HTTPClientTCP::set_connection(const Ref<StreamPeer> &p_connection) {
	ERR_FAIL_COND_MSG(p_connection.is_null(), "Connection is not a reference to a valid StreamPeer object.");

	if (tls_options.is_valid()) {
		ERR_FAIL_NULL_MSG(Object::cast_to<StreamPeerTLS>(p_connection.ptr()),
				"Connection is not a reference to a valid StreamPeerTLS object.");
	}

	if (connection == p_connection) {
		return;
	}

	close();
	connection = p_connection;
	status = STATUS_CONNECTED;
}

Ref<StreamPeer> HTTPClientTCP::get_connection() const {
	return connection;
}

Error HTTPClientTCP::_read_response_head(bool &r_complete) {
	r_complete = false;
	int scan = MAX(0, response_str.size() - 3);
	while (true) {
		// Scan from the previous block's last three bytes to detect a terminator across the boundary.
		const int size = response_str.size();
		int end = -1;
		for (int i = scan; i < size; i++) {
			if (i + 3 < size && response_str[i] == '\r' && response_str[i + 1] == '\n' && response_str[i + 2] == '\r' && response_str[i + 3] == '\n') {
				end = i + 4;
				break;
			}
			if (i + 1 < size && response_str[i] == '\n' && response_str[i + 1] == '\n') {
				end = i + 2;
				break;
			}
		}
		if (end >= 0) {
			response_header_bytes += end;
			if (response_header_bytes > RESPONSE_HEADER_MAX) {
				return ERR_INVALID_DATA;
			}
			// Preserve body bytes received with the header for the body-reading entry point.
			for (int i = end; i < size; i++) {
				response_pending.push_back(response_str[i]);
			}
			response_pending_at = 0;
			response_str.resize(end);
			r_complete = true;
			return OK;
		}
		if (response_header_bytes + size > RESPONSE_HEADER_MAX) {
			return ERR_INVALID_DATA;
		}
		scan = MAX(0, size - 3);
		// Use the normal body read width to reduce polling for large headers.
		uint8_t block[65536];
		// Retain only the one extra byte needed to establish a limit violation.
		const int want = MIN((int)sizeof(block), RESPONSE_HEADER_MAX - response_header_bytes - size + 1);
		int received = 0;
		// Advance threaded reads only through the header terminator.
		const Error err = _get_http_data(block, want, received, false);
		if (err != OK) {
			return err;
		}
		if (received == 0) {
			return OK;
		}
		const int at = response_str.size();
		response_str.resize(at + received);
		memcpy(response_str.ptrw() + at, block, received);
	}
}

Error HTTPClientTCP::_parse_response_head() {
	const uint8_t *bytes = response_str.ptr();
	const int size = response_str.size();
	body_size = -1;
	chunked = false;
	body_left = 0;
	chunk_left = 0;
	chunk_excess = 0;
	chunk_data_part = false;
	chunk_trailer_part = false;
	chunk_trailer_bytes = 0;
	read_until_eof = false;
	response_headers.clear();
	response_num = RESPONSE_OK;
	response_http_10 = false;

	// Require CRLF line endings to prevent differing HTTP interpretations across peers.
	for (int i = 0; i < size; i++) {
		if ((bytes[i] == '\n' && (i == 0 || bytes[i - 1] != '\r')) || (bytes[i] == '\r' && (i + 1 >= size || bytes[i + 1] != '\n'))) {
			return ERR_INVALID_DATA;
		}
	}
	int line_end = 0;
	while (line_end + 1 < size && !(bytes[line_end] == '\r' && bytes[line_end + 1] == '\n')) {
		line_end++;
	}
	const bool version_ok = line_end >= 12 && (memcmp(bytes, "HTTP/1.0 ", 9) == 0 || memcmp(bytes, "HTTP/1.1 ", 9) == 0);
	const bool code_ok = version_ok && bytes[9] >= '0' && bytes[9] <= '9' && bytes[10] >= '0' && bytes[10] <= '9' && bytes[11] >= '0' && bytes[11] <= '9' && (line_end == 12 || bytes[12] == ' ');
	if (!code_ok) {
		return ERR_INVALID_DATA;
	}
	response_num = (bytes[9] - '0') * 100 + (bytes[10] - '0') * 10 + bytes[11] - '0';
	response_http_10 = bytes[7] == '0';
	if (response_num < RESPONSE_STATUS_MIN || response_num > RESPONSE_STATUS_MAX) {
		return ERR_INVALID_DATA;
	}

	int length_n = 0;
	int encoding_n = 0;
	for (int at = line_end + 2; at + 1 < size;) {
		int end = at;
		while (end + 1 < size && !(bytes[end] == '\r' && bytes[end + 1] == '\n')) {
			end++;
		}
		if (end == at) {
			break;
		}
		int colon = at;
		while (colon < end && bytes[colon] != ':') {
			if (!response_name_char(bytes[colon])) {
				return ERR_INVALID_DATA;
			}
			colon++;
		}
		if (colon == at || colon == end) {
			return ERR_INVALID_DATA;
		}
		for (int i = colon + 1; i < end; i++) {
			if ((bytes[i] < 0x20 && bytes[i] != '\t') || bytes[i] == 0x7f) {
				return ERR_INVALID_DATA;
			}
		}
		const uint8_t *value = bytes + colon + 1;
		const int value_len = end - colon - 1;
		if (response_name_is(bytes + at, colon - at, "content-length")) {
			int64_t length = 0;
			if (++length_n > 1 || !response_length(value, value_len, length)) {
				return ERR_INVALID_DATA;
			}
			body_size = length;
			body_left = length;
		} else if (response_name_is(bytes + at, colon - at, "transfer-encoding")) {
			if (++encoding_n > 1 || !response_chunked(value, value_len)) {
				return ERR_INVALID_DATA;
			}
			chunked = true;
		}
		response_headers.push_back(String::utf8((const char *)bytes + at, end - at));
		at = end + 2;
	}
	return length_n > 0 && encoding_n > 0 ? ERR_INVALID_DATA : OK;
}

static bool _check_request_url(HTTPClientTCP::Method p_method, const String &p_url) {
	for (int i = 0; i < p_url.length(); i++) {
		if (p_url[i] <= 0x20 || p_url[i] == 0x7f) {
			return false; // Prevent request-line injection into headers or subsequent requests.
		}
	}
	switch (p_method) {
		case HTTPClientTCP::METHOD_CONNECT: {
			// Authority in host:port format, as in RFC7231.
			int pos = p_url.find_char(':');
			return 0 < pos && pos < p_url.length() - 1;
		}
		case HTTPClientTCP::METHOD_OPTIONS: {
			if (p_url == "*") {
				return true;
			}
			[[fallthrough]];
		}
		default:
			// Absolute path or absolute URL.
			return p_url.begins_with("/") || p_url.begins_with("http://") || p_url.begins_with("https://");
	}
}

Error HTTPClientTCP::request(Method p_method, const String &p_url, const Vector<String> &p_headers, const uint8_t *p_body, int p_body_size) {
	ERR_FAIL_INDEX_V(p_method, METHOD_MAX, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!_check_request_url(p_method, p_url), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(status != STATUS_CONNECTED, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(connection.is_null(), ERR_INVALID_DATA);

	Error err = verify_headers(p_headers);
	if (err) {
		return err;
	}

	String uri = p_url;
	if (tls_options.is_null() && http_proxy_port != -1) {
		uri = vformat("http://%s:%d%s", conn_host, conn_port, p_url);
	}

	String request = String(_methods[p_method]) + " " + uri + " HTTP/1.1\r\n";
	bool add_host = true;
	bool add_clen = p_body_size > 0;
	bool add_uagent = true;
	bool add_accept = true;
	int host_n = 0;
	int length_n = 0;
	for (int i = 0; i < p_headers.size(); i++) {
		const int colon = p_headers[i].find_char(':');
		const String name = p_headers[i].substr(0, colon).to_lower();
		const String value = p_headers[i].substr(colon + 1);
		if (name == "transfer-encoding") {
			return ERR_INVALID_PARAMETER; // Reject chunked requests because this path does not encode request chunks.
		}
		if (name == "content-length") {
			int64_t given = 0;
			if (++length_n > 1 || !read_length(value, given) || given != p_body_size) {
				return ERR_INVALID_PARAMETER;
			}
		}
		if (name == "host" && ++host_n > 1) {
			return ERR_INVALID_PARAMETER;
		}
		request += p_headers[i] + "\r\n";
		if (add_host && name == "host") {
			add_host = false;
		}
		if (add_clen && name == "content-length") {
			add_clen = false;
		}
		if (add_uagent && name == "user-agent") {
			add_uagent = false;
		}
		if (add_accept && name == "accept") {
			add_accept = false;
		}
	}
	if (add_host) {
		// An IPv6 literal must be bracketed, or the port separator is ambiguous
		// ("::1:8080" is not a valid Host value).
		const String host_txt = conn_host.contains(":") ? "[" + conn_host + "]" : conn_host;
		if ((tls_options.is_valid() && conn_port == PORT_HTTPS) || (tls_options.is_null() && conn_port == PORT_HTTP)) {
			// Don't append the standard ports.
			request += "Host: " + host_txt + "\r\n";
		} else {
			request += "Host: " + host_txt + ":" + itos(conn_port) + "\r\n";
		}
	}
	if (add_clen) {
		request += "Content-Length: " + itos(p_body_size) + "\r\n";
		// Should it add utf8 encoding?
	}
	if (add_uagent) {
		request += "User-Agent: GodotEngine/" + String(GODOT_VERSION_FULL_BUILD) + " (" + OS::get_singleton()->get_name() + ")\r\n";
	}
	if (add_accept) {
		request += "Accept: */*\r\n";
	}
	request += "\r\n";
	CharString cs = request.utf8();

	request_buffer->clear();
	request_buffer->put_data((const uint8_t *)cs.get_data(), cs.length());
	if (p_body_size > 0) {
		request_buffer->put_data(p_body, p_body_size);
	}
	request_buffer->seek(0);
	tcp_connection->set_write_wait_enabled(true); // Monitor write readiness until the request is fully sent.

	// Clear the previous response state before reusing the connection.
	response_num = 0;
	response_headers.clear();
	response_str.clear();
	response_pending.clear();
	response_pending_at = 0;
	response_http_10 = false;
	status = STATUS_REQUESTING;
	head_request = p_method == METHOD_HEAD;
	connect_request = p_method == METHOD_CONNECT;
	response_header_bytes = 0;
	response_http_10 = false;

	return OK;
}

bool HTTPClientTCP::has_response() const {
	return response_num != 0; // A parsed status line establishes a response even with no headers.
}

bool HTTPClientTCP::is_response_chunked() const {
	return chunked;
}

int HTTPClientTCP::get_response_code() const {
	return response_num;
}

Error HTTPClientTCP::get_response_headers(List<String> *r_response) {
	if (!response_headers.size()) {
		return ERR_INVALID_PARAMETER;
	}

	for (int i = 0; i < response_headers.size(); i++) {
		r_response->push_back(response_headers[i]);
	}

	response_headers.clear();

	return OK;
}

void HTTPClientTCP::close() {
	tcp_connection->set_write_wait_enabled(false); // Remove write monitoring for the closed socket.
	if (tcp_connection->get_status() != StreamPeerTCP::STATUS_NONE) {
		tcp_connection->disconnect_from_host();
	}

	connection.unref();
	proxy_client.unref();
	status = STATUS_DISCONNECTED;
	head_request = false;
	connect_request = false;
	if (resolving != IP::RESOLVER_INVALID_ID) {
		IP::get_singleton()->erase_resolve_item(resolving);
		resolving = IP::RESOLVER_INVALID_ID;
	}

	ip_candidates.clear();
	response_headers.clear();
	response_str.clear();
	response_pending.clear();
	response_pending_at = 0;
	request_buffer->clear();
	body_size = -1;
	body_left = 0;
	chunk_left = 0;
	chunk_excess = 0;
	chunk_data_part = false;
	chunk_trailer_part = false;
	chunk_trailer_bytes = 0;
	read_until_eof = false;
	clean_eof = false;
	response_header_bytes = 0;
	response_num = 0;
	handshaking = false;
}

Error HTTPClientTCP::poll() {
	if (tcp_connection.is_valid()) {
		const Error tcp_err = tcp_connection->poll();
		if (status == STATUS_BODY && read_until_eof && !tls_options.is_valid() && tcp_err == OK && tcp_connection->get_status() == StreamPeerTCP::STATUS_NONE) {
			clean_eof = true;
		}
	}
	switch (status) {
		case STATUS_RESOLVING: {
			ERR_FAIL_COND_V(resolving == IP::RESOLVER_INVALID_ID, ERR_BUG);

			IP::ResolverStatus rstatus = IP::get_singleton()->get_resolve_item_status(resolving);
			switch (rstatus) {
				case IP::RESOLVER_STATUS_WAITING:
					return OK; // Still resolving.

				case IP::RESOLVER_STATUS_DONE: {
					ip_candidates = IP::get_singleton()->get_resolve_item_addresses(resolving);
					IP::get_singleton()->erase_resolve_item(resolving);
					resolving = IP::RESOLVER_INVALID_ID;

					Error err = ERR_BUG; // Should be at least one entry.
					while (ip_candidates.size() > 0) {
						err = tcp_connection->connect_to_host(ip_candidates.pop_front(), server_port);
						if (err == OK) {
							tcp_connection->set_write_wait_enabled(true); // Await asynchronous connection completion.
							break;
						}
					}
					if (err) {
						status = STATUS_CANT_CONNECT;
						return err;
					}

					status = STATUS_CONNECTING;
				} break;
				case IP::RESOLVER_STATUS_NONE:
				case IP::RESOLVER_STATUS_ERROR: {
					IP::get_singleton()->erase_resolve_item(resolving);
					resolving = IP::RESOLVER_INVALID_ID;
					close();
					status = STATUS_CANT_RESOLVE;
					return ERR_CANT_RESOLVE;
				} break;
			}
		} break;
		case STATUS_CONNECTING: {
			StreamPeerTCP::Status s = tcp_connection->get_status();
			switch (s) {
				case StreamPeerTCP::STATUS_CONNECTING: {
					return OK;
				} break;
				case StreamPeerTCP::STATUS_CONNECTED: {
					tcp_connection->set_write_wait_enabled(false); // Avoid write-ready spinning while waiting for input.
					if (tls_options.is_valid() && proxy_client.is_valid()) {
						Error err = proxy_client->poll();
						if (err == ERR_UNCONFIGURED) {
							proxy_client->set_connection(tcp_connection);
							const Vector<String> headers;
							err = proxy_client->request(METHOD_CONNECT, vformat("%s:%d", conn_host, conn_port), headers, nullptr, 0);
							if (err != OK) {
								status = STATUS_CANT_CONNECT;
								return err;
							}
						} else if (err != OK) {
							status = STATUS_CANT_CONNECT;
							return err;
						}
						switch (proxy_client->get_status()) {
							case STATUS_REQUESTING: {
								tcp_connection->set_write_wait_enabled(proxy_client->request_buffer->get_available_bytes() > 0); // Monitor writes only while proxy request bytes remain.
								return OK;
							} break;
							case STATUS_BODY: {
								tcp_connection->set_write_wait_enabled(false); // Monitor only reads for the proxy response.
								proxy_client->read_response_body_chunk();
								return OK;
							} break;
							case STATUS_CONNECTED: {
								tcp_connection->set_write_wait_enabled(false); // Stop write monitoring after proxy negotiation is sent.
								if (proxy_client->get_response_code() != RESPONSE_OK) {
									status = STATUS_CANT_CONNECT;
									return ERR_CANT_CONNECT;
								}
								proxy_client.unref();
								return OK;
							}
							case STATUS_DISCONNECTED:
							case STATUS_RESOLVING:
							case STATUS_CONNECTING: {
								status = STATUS_CANT_CONNECT;
								ERR_FAIL_V(ERR_BUG);
							} break;
							default: {
								status = STATUS_CANT_CONNECT;
								return ERR_CANT_CONNECT;
							} break;
						}
					} else if (tls_options.is_valid()) {
						Ref<StreamPeerTLS> tls_conn;
						if (!handshaking) {
							// Connect the StreamPeerTLS and start handshaking.
							tls_conn = Ref<StreamPeerTLS>(StreamPeerTLS::create());
							Error err = tls_conn->connect_to_stream(tcp_connection, conn_host, tls_options);
							if (err != OK) {
								close();
								status = STATUS_TLS_HANDSHAKE_ERROR;
								return ERR_CANT_CONNECT;
							}
							connection = tls_conn;
							handshaking = true;
						} else {
							// We are already handshaking, which means we can use your already active TLS connection.
							tls_conn = static_cast<Ref<StreamPeerTLS>>(connection);
							if (tls_conn.is_null()) {
								close();
								status = STATUS_TLS_HANDSHAKE_ERROR;
								return ERR_CANT_CONNECT;
							}

							tls_conn->poll(); // Try to finish the handshake.
						}

						if (tls_conn->get_status() == StreamPeerTLS::STATUS_CONNECTED) {
							// Handshake has been successful.
							handshaking = false;
							ip_candidates.clear();
							status = STATUS_CONNECTED;
							return OK;
						} else if (tls_conn->get_status() != StreamPeerTLS::STATUS_HANDSHAKING) {
							// Handshake has failed.
							close();
							status = STATUS_TLS_HANDSHAKE_ERROR;
							return ERR_CANT_CONNECT;
						}
						// ... we will need to poll more for handshake to finish.
					} else {
						ip_candidates.clear();
						status = STATUS_CONNECTED;
					}
					return OK;
				} break;
				case StreamPeerTCP::STATUS_ERROR:
				case StreamPeerTCP::STATUS_NONE: {
					Error err = ERR_CANT_CONNECT;
					while (ip_candidates.size() > 0) {
						tcp_connection->disconnect_from_host();
						err = tcp_connection->connect_to_host(ip_candidates.pop_front(), server_port);
						if (err == OK) {
							tcp_connection->set_write_wait_enabled(true); // Await connection readiness for the next address candidate.
							return OK;
						}
					}
					close();
					status = STATUS_CANT_CONNECT;
					return err;
				} break;
			}
		} break;
		case STATUS_BODY: {
			// Deliver body bytes already received with the header even after disconnection.
			if (read_until_eof || !response_pending.is_empty() || (!chunked && body_left == 0)) {
				return OK;
			}
			[[fallthrough]];
		}
		case STATUS_CONNECTED: {
			// Check if we are still connected.
			if (tls_options.is_valid()) {
				Ref<StreamPeerTLS> tmp = connection;
				tmp->poll();
				if (tmp->get_status() != StreamPeerTLS::STATUS_CONNECTED) {
					status = STATUS_CONNECTION_ERROR;
					return ERR_CONNECTION_ERROR;
				}
			} else if (tcp_connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
				status = STATUS_CONNECTION_ERROR;
				return ERR_CONNECTION_ERROR;
			}
			// Connection established, requests can now be made.
			return OK;
		} break;
		case STATUS_REQUESTING: {
			if (request_buffer->get_available_bytes()) {
				int avail = request_buffer->get_available_bytes();
				int pos = request_buffer->get_position();
				const Vector<uint8_t> data = request_buffer->get_data_array();
				int wrote = 0;
				Error err;
				if (blocking) {
					err = connection->put_data(data.ptr() + pos, avail);
					wrote += avail;
				} else {
					err = connection->put_partial_data(data.ptr() + pos, avail, wrote);
				}
				if (err != OK) {
					close();
					status = STATUS_CONNECTION_ERROR;
					return ERR_CONNECTION_ERROR;
				}
				pos += wrote;
				request_buffer->seek(pos);
				if (avail - wrote > 0) {
					tcp_connection->set_write_wait_enabled(true); // Await readiness to send the remaining bytes.
					return OK;
				}
				request_buffer->clear();
				tcp_connection->set_write_wait_enabled(false); // Monitor only response reads after sending completes.
			}
			while (true) {
				bool complete = false;
				const Error head_err = _read_response_head(complete);
				if (head_err != OK) {
					close();
					status = STATUS_CONNECTION_ERROR;
					return head_err == ERR_INVALID_DATA ? ERR_INVALID_DATA : ERR_CONNECTION_ERROR;
				}
				if (!complete) {
					return OK;
				}
				if (_parse_response_head() != OK) {
					close();
					status = STATUS_CONNECTION_ERROR;
					return ERR_INVALID_DATA;
				}
				response_str.clear();
				if (response_num >= 100 && response_num < 200) {
					// Reject protocol switching at 101; continue other informational responses until a final response.
					if (response_num == 101) {
						close();
						status = STATUS_CONNECTION_ERROR;
						return ERR_INVALID_DATA;
					}
					response_num = 0;
					response_headers.clear();
					continue;
				}

				// Treat HEAD, 204, 205, and 304 responses as bodyless.
				if (head_request || (connect_request && response_num >= 200 && response_num < 300) || response_num == 204 || response_num == 205 || response_num == 304) {
					body_size = 0;
					body_left = 0;
					chunked = false;
				}

				if (body_size != -1 || chunked) {
					status = STATUS_BODY;
				} else {
					// Read an ordinary response without a declared length until disconnect; do not reuse its connection.
					read_until_eof = true;
					status = STATUS_BODY;
				}
				return OK;
			}
		} break;
		case STATUS_DISCONNECTED: {
			return ERR_UNCONFIGURED;
		} break;
		case STATUS_CONNECTION_ERROR:
		case STATUS_TLS_HANDSHAKE_ERROR: {
			return ERR_CONNECTION_ERROR;
		} break;
		case STATUS_CANT_CONNECT: {
			return ERR_CANT_CONNECT;
		} break;
		case STATUS_CANT_RESOLVE: {
			return ERR_CANT_RESOLVE;
		} break;
	}

	return OK;
}

int64_t HTTPClientTCP::get_response_body_length() const {
	return body_size;
}

// Read the response body in bounded read widths while validating chunk boundaries.
PackedByteArray HTTPClientTCP::read_response_body_chunk() {
	ERR_FAIL_COND_V(status != STATUS_BODY, PackedByteArray());

	PackedByteArray ret;
	Error err = OK;

	if (chunked) {
		while (true) {
			if (chunk_trailer_part) {
				// We need to consume the trailer part too or keep-alive will break.
				uint8_t b;
				int rec = 0;
				err = _get_http_data(&b, 1, rec);

				if (rec == 0) {
					break;
				}

				chunk.push_back(b);
				chunk_trailer_bytes++;
				if (chunk_trailer_bytes > CHUNK_TRAILER_MAX) {
					// Bound memory growth for trailers without a terminator.
					chunk.clear();
					status = STATUS_CONNECTION_ERROR;
					break;
				}
				int cs = chunk.size();
				if ((cs >= 2 && chunk[cs - 2] == '\r' && chunk[cs - 1] == '\n')) {
					if (cs == 2) {
						// Finally over.
						chunk_trailer_part = false;
						chunk_trailer_bytes = 0;
						status = STATUS_CONNECTED;
						chunk.clear();
						break;
					} else {
						// We do not process nor return the trailer data.
						chunk.clear();
					}
				}
			} else if (chunk_data_part) {
				if (chunk_left > 0) {
					// Return data in read-width pieces instead of allocating the declared length.
					const int want = (int)MIN(chunk_left, (int64_t)read_chunk_size);
					ret.resize(want);
					int rec = 0;
					err = _get_http_data(ret.ptrw(), want, rec);
					ret.resize(rec);
					chunk_left -= rec;
					break;
				}

				// Require CRLF after each chunk body.
				uint8_t b;
				int rec = 0;
				err = _get_http_data(&b, 1, rec);
				if (rec == 0) {
					break;
				}
				chunk.push_back(b);
				if (chunk.size() == 2) {
					if (chunk[0] != '\r' || chunk[1] != '\n') {
						ERR_PRINT("HTTP Invalid chunk terminator (not \\r\\n)");
						status = STATUS_CONNECTION_ERROR;
						break;
					}
					chunk.clear();
					chunk_data_part = false;
				}
			} else {
				// Reading length.
				uint8_t b;
				int rec = 0;
				err = _get_http_data(&b, 1, rec);

				if (rec == 0) {
					break;
				}

				chunk.push_back(b);

				if (b == '\n' && (chunk.size() < 2 || chunk[chunk.size() - 2] != '\r')) {
					ERR_PRINT("HTTP Invalid bare LF in chunk length");
					status = STATUS_CONNECTION_ERROR;
					break;
				}
				if (chunk.size() > CHUNK_LINE_MAX + 1) {
					ERR_PRINT("HTTP Invalid chunk hex len");
					status = STATUS_CONNECTION_ERROR;
					break;
				}

				if (chunk.size() > 2 && chunk[chunk.size() - 2] == '\r' && chunk[chunk.size() - 1] == '\n') {
					int line_end = chunk.size() - 2;
					const int wire_line = line_end;
					if (line_end >= CHUNK_LINE_MAX) {
						ERR_PRINT("HTTP Invalid chunk hex len");
						status = STATUS_CONNECTION_ERROR;
						break;
					}
					for (int i = 0; i < line_end; i++) {
						if (chunk[i] == '\r') {
							ERR_PRINT("HTTP Invalid CR in chunk length");
							status = STATUS_CONNECTION_ERROR;
							break;
						}
					}
					if (status == STATUS_CONNECTION_ERROR) {
						break;
					}
					while (line_end > 0 && (chunk[line_end - 1] == ' ' || chunk[line_end - 1] == '\t')) {
						line_end--;
					}
					int hex_end = 0;
					while (hex_end < line_end && chunk[hex_end] != ';') {
						hex_end++;
					}
					int64_t len = 0;
					bool valid = hex_end > 0 && hex_end <= CHUNK_HEX_MAX;
					for (int i = 0; valid && i < hex_end; i++) {
						char c = chunk[i];
						int v = 0;
						if (is_digit(c)) {
							v = c - '0';
						} else if (c >= 'a' && c <= 'f') {
							v = c - 'a' + 10;
						} else if (c >= 'A' && c <= 'F') {
							v = c - 'A' + 10;
						} else {
							ERR_PRINT("HTTP Chunk len not in hex!!");
							valid = false;
							break;
						}
						if (len > (INT64_MAX - v) / 16) {
							ERR_PRINT("HTTP Chunk length overflow!!");
							valid = false;
							break;
						}
						len = len * 16 + v;
					}
					if (!valid) {
						// Reject invalid lengths before allocation.
						chunk.clear();
						status = STATUS_CONNECTION_ERROR;
						break;
					}
					// Allow small chunks while accumulating excessive framing caused by extensions.
					const int64_t overhead = chunk_excess + wire_line + 2 - CHUNK_FRAME_ALLOWANCE;
					// Credit large payloads against framing without overflowing the signed range when doubling.
					chunk_excess = overhead <= 0 || len >= (overhead + 1) / 2 ? 0 : overhead - 2 * len;
					if (chunk_excess > CHUNK_EXCESS_MAX) {
						ERR_PRINT("HTTP Chunk encoding contains too much non-data");
						chunk.clear();
						status = STATUS_CONNECTION_ERROR;
						break;
					}

					if (len == 0) {
						// End reached!
						chunk_trailer_part = true;
						chunk_trailer_bytes = 0;
						chunk.clear();
						break;
					}

					chunk_left = len;
					chunk_data_part = true;
					chunk.clear();
				}
			}
		}

	} else {
		int to_read = !read_until_eof ? MIN(body_left, read_chunk_size) : read_chunk_size;
		ret.resize(to_read);
		int _offset = 0;
		while (to_read > 0) {
			int rec = 0;
			{
				uint8_t *w = ret.ptrw();
				err = _get_http_data(w + _offset, to_read, rec);
			}
			if (rec <= 0) { // Ended up reading less.
				ret.resize(_offset);
				break;
			} else {
				_offset += rec;
				to_read -= rec;
				if (!read_until_eof) {
					body_left -= rec;
				}
			}
			if (err != OK) {
				ret.resize(_offset);
				break;
			}
		}
	}

	if (err != OK) {
		// Treat any termination other than clean EOF as corruption for an unbounded body.
		const bool eof_body = read_until_eof && (err == ERR_FILE_EOF || (clean_eof && err == FAILED));
		close();

		if (eof_body) {
			status = STATUS_DISCONNECTED; // Server disconnected.
		} else {
			status = STATUS_CONNECTION_ERROR;
		}
	} else if (body_left == 0 && !chunked && !read_until_eof) {
		status = STATUS_CONNECTED;
	}

	return ret;
}

HTTPClientTCP::Status HTTPClientTCP::get_status() const {
	return status;
}

void HTTPClientTCP::set_blocking_mode(bool p_enable) {
	blocking = p_enable;
}

bool HTTPClientTCP::is_blocking_mode_enabled() const {
	return blocking;
}

Error HTTPClientTCP::_get_http_data(uint8_t *p_buffer, int p_bytes, int &r_received, bool p_fill) {
	if (response_pending_at < response_pending.size()) {
		// Advance an offset rather than compacting the prefix to keep small chunk reads linear.
		r_received = MIN(p_bytes, response_pending.size() - response_pending_at);
		memcpy(p_buffer, response_pending.ptr() + response_pending_at, r_received);
		response_pending_at += r_received;
		if (response_pending_at == response_pending.size()) {
			response_pending.clear();
			response_pending_at = 0;
		}
		return OK;
	}
	if (blocking && p_fill) {
		// We can't use StreamPeer.get_data, since when reaching EOF we will get an
		// error without knowing how many bytes we received.
		Error err = ERR_FILE_EOF;
		int read = 0;
		int left = p_bytes;
		r_received = 0;
		while (left > 0) {
			err = connection->get_partial_data(p_buffer + r_received, left, read);
			if (err == OK) {
				r_received += read;
			} else if (err == ERR_FILE_EOF) {
				r_received += read;
				return r_received > 0 ? OK : err;
			} else {
				return err;
			}
			left -= read;
		}
		return err;
	} else {
		const Error err = connection->get_partial_data(p_buffer, p_bytes, r_received);
		return err == ERR_FILE_EOF && r_received > 0 ? OK : err;
	}
}

void HTTPClientTCP::set_read_chunk_size(int p_size) {
	ERR_FAIL_COND(p_size < READ_CHUNK_MIN || p_size > READ_CHUNK_MAX);
	read_chunk_size = p_size;
}

int HTTPClientTCP::get_read_chunk_size() const {
	return read_chunk_size;
}

void HTTPClientTCP::set_http_proxy(const String &p_host, int p_port) {
	if (p_host.is_empty() || p_port == -1) {
		http_proxy_host = "";
		http_proxy_port = -1;
	} else {
		http_proxy_host = p_host;
		http_proxy_port = p_port;
	}
}

void HTTPClientTCP::set_https_proxy(const String &p_host, int p_port) {
	if (p_host.is_empty() || p_port == -1) {
		https_proxy_host = "";
		https_proxy_port = -1;
	} else {
		https_proxy_host = p_host;
		https_proxy_port = p_port;
	}
}

HTTPClientTCP::HTTPClientTCP() {
	tcp_connection.instantiate();
	request_buffer.instantiate();
}

HTTPClient *(*HTTPClient::_create)(bool p_notify_postinitialize) = HTTPClientTCP::_create_func;

#endif // WEB_ENABLED
