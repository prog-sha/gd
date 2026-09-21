/**************************************************************************/
/*  http_client_tcp.h                                                     */
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

// Define HTTP/1 connection state and response parsing state.

#pragma once

#include "core/crypto/crypto.h"
#include "core/io/http_client.h"
#include "core/io/ip.h"

class StreamPeerTCP;

class HTTPClientTCP : public HTTPClient {
	GDSOFTCLASS(HTTPClientTCP, HTTPClient);

private:
	static constexpr int RESPONSE_HEADER_MAX = 10 * 1024 * 1024; // Combined response-header budget including informational responses.
	static constexpr int CHUNK_LINE_MAX = 4096; // Maximum chunk-length line size.
	static constexpr int CHUNK_EXCESS_MAX = 16 * 1024; // Cumulative excessive chunk-framing budget.
	static constexpr int CHUNK_TRAILER_MAX = 4096; // Trailer budget matching the normal parsing buffer width.
	static constexpr int RESPONSE_STATUS_MIN = 0; // Minimum accepted three-digit status code.
	static constexpr int RESPONSE_STATUS_MAX = 999; // Maximum accepted three-digit status code.
	static constexpr int CHUNK_HEX_MAX = 16; // Maximum hexadecimal digit count for a 64-bit chunk length.
	static constexpr int CHUNK_FRAME_ALLOWANCE = 16; // Initial allowance for chunk framing overhead.
	static constexpr int READ_CHUNK_MIN = 256; // Minimum public read-chunk size in bytes.
	static constexpr int READ_CHUNK_MAX = 1 << 24; // Maximum public read-chunk size in bytes.
	static constexpr int READ_CHUNK_DEFAULT = 64 * 1024; // Default public read-chunk size in bytes.

	Status status = STATUS_DISCONNECTED;
	IP::ResolverID resolving = IP::RESOLVER_INVALID_ID;
	Array ip_candidates;
	int conn_port = -1; // Server to make requests to.
	String conn_host;
	int server_port = -1; // Server to connect to (might be a proxy server).
	String server_host;
	int http_proxy_port = -1; // Proxy server for http requests.
	String http_proxy_host;
	int https_proxy_port = -1; // Proxy server for https requests.
	String https_proxy_host;
	bool blocking = false;
	bool handshaking = false;
	bool head_request = false;
	bool connect_request = false;
	Ref<TLSOptions> tls_options;

	Vector<uint8_t> response_str;
	Vector<uint8_t> response_pending; // Body bytes read ahead with response headers.
	int response_pending_at = 0; // Next unread position in the buffered body.

	bool chunked = false;
	Vector<uint8_t> chunk;
	int64_t chunk_left = 0;
	int64_t chunk_excess = 0; // Framing bytes exceeding the payload-based allowance.
	bool chunk_data_part = false; // Whether chunk body and trailing CRLF are being read.
	bool chunk_trailer_part = false;
	int chunk_trailer_bytes = 0;
	int64_t body_size = -1;
	int64_t body_left = 0;
	bool read_until_eof = false;
	bool clean_eof = false; // Clean TCP FIN terminating a body without a declared length.
	bool response_http_10 = false; // Whether HTTP/1.0 makes connection closure the default.
	int response_header_bytes = 0; // Total header bytes including informational responses.

	Ref<StreamPeerBuffer> request_buffer;
	Ref<StreamPeerTCP> tcp_connection;
	Ref<StreamPeer> connection;
	Ref<HTTPClientTCP> proxy_client; // Negotiate with proxy server.

	int response_num = 0;
	Vector<String> response_headers;
	// 64 KiB by default (favors fast download speeds at the cost of memory usage).
	int read_chunk_size = READ_CHUNK_DEFAULT;

	Error _get_http_data(uint8_t *p_buffer, int p_bytes, int &r_received, bool p_fill = true);
	// Read response headers as bytes while preserving body read-ahead.
	Error _read_response_head(bool &r_complete);
	// Parse header lines without duplicating the complete header string.
	Error _parse_response_head();

public:
	static HTTPClient *_create_func(bool p_notify_postinitialize);

	Error request(Method p_method, const String &p_url, const Vector<String> &p_headers, const uint8_t *p_body, int p_body_size) override;

	Error connect_to_host(const String &p_host, int p_port = -1, Ref<TLSOptions> p_tls_options = Ref<TLSOptions>()) override;
	void set_connection(const Ref<StreamPeer> &p_connection) override;
	Ref<StreamPeer> get_connection() const override;
	void close() override;
	Status get_status() const override;
	bool has_response() const override;
	bool is_response_chunked() const override;
	int get_response_code() const override;
	bool is_response_http_10() const { return response_http_10; }
	Error get_response_headers(List<String> *r_response) override;
	int64_t get_response_body_length() const override;
	PackedByteArray read_response_body_chunk() override;
	void set_blocking_mode(bool p_enable) override;
	bool is_blocking_mode_enabled() const override;
	void set_read_chunk_size(int p_size) override;
	int get_read_chunk_size() const override;
	Error poll() override;
	void set_http_proxy(const String &p_host, int p_port) override;
	void set_https_proxy(const String &p_host, int p_port) override;
	HTTPClientTCP();
};
