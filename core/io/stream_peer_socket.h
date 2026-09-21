/**************************************************************************/
/*  stream_peer_socket.h                                                  */
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

#pragma once

#include "core/io/net_socket.h"
#include "core/io/stream_peer.h"

#ifndef DISABLE_DEPRECATED
namespace compat::StreamPeerTCP {
enum class Status;
} //namespace compat::StreamPeerTCP
#endif

class StreamPeerSocket : public StreamPeer {
	GDCLASS(StreamPeerSocket, StreamPeer);

public:
	enum Status {
		STATUS_NONE,
		STATUS_CONNECTING,
		STATUS_CONNECTED,
		STATUS_ERROR,
	};

protected:
#ifndef DISABLE_DEPRECATED
	compat::StreamPeerTCP::Status _get_status_compat_107954() const;
	static void _bind_compatibility_methods();
#endif

	Ref<NetSocket> _sock;
	uint64_t timeout = 0;
	Status status = STATUS_NONE;
	NetSocket::Address peer_address;
	bool keep_write_on_eof = false; // Preserve the write half after receive EOF for opted-in plaintext connections.
	bool read_eof = false; // Whether recv has confirmed the read-side end.

	Error write(const uint8_t *p_data, int p_bytes, int &r_sent, bool p_block);
	Error read(uint8_t *p_buffer, int p_bytes, int &r_received, bool p_block);

	static void _bind_methods();

public:
	virtual void accept_socket(Ref<NetSocket> p_sock, const NetSocket::Address &p_addr) = 0;

	void disconnect_from_host();

	int get_available_bytes() const override;
	Status get_status() const;
	// Configure internal half-close retention without exposing it to scripts.
	void set_keep_write_on_eof(bool p_enabled) { keep_write_on_eof = p_enabled; }
	// Return whether only the receive half has ended.
	bool is_read_eof() const { return read_eof; }

	// Poll socket updating its state.
	Error poll();

	// Wait or check for writable, readable.
	Error wait(NetSocket::PollType p_type, int p_timeout = 0);
	// Enable read readiness notifications only while required.
	void set_read_wait_enabled(bool p_enabled);
	// Enable write readiness notifications only while required.
	void set_write_wait_enabled(bool p_enabled);
	// Pass the internal kernel-readiness handler to the underlying socket.
	void set_wait_callback(const Callable &p_callback);

	// Read/Write from StreamPeer
	Error put_data(const uint8_t *p_data, int p_bytes) override;
	Error put_partial_data(const uint8_t *p_data, int p_bytes, int &r_sent) override;
	Error get_data(uint8_t *p_buffer, int p_bytes) override;
	Error get_partial_data(uint8_t *p_buffer, int p_bytes, int &r_received) override;

	StreamPeerSocket();
	virtual ~StreamPeerSocket();
};

VARIANT_ENUM_CAST(StreamPeerSocket::Status);
