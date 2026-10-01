# Parse mail messages and exchange SMTP commands over standard streams.
extends RefCounted


class DataWriter:
	extends RefCounted
	var client: Client
	var line_start := true
	var cr := false
	var closed := false
	var writing := false

	# Keep one SMTP DATA writer attached to its transaction.
	func _init(p_client: Client):
		client = p_client

	# Send message bytes while escaping dots at line starts.
	func write(data: PackedByteArray) -> Variant, Err:
		if closed or writing or not client.conn.is_open():
			return null, Err.from("SMTP DATA writer is closed", Err.INTERRUPTED)
		writing = true
		var out := PackedByteArray()
		var _appended: bool
		for byte in data:
			if cr and byte != 10:
				writing = false
				return null, Err.from("bare CR in SMTP DATA", Err.INVALID_DATA)
			if byte == 13:
				cr = true
				_appended = out.append(byte)
				continue
			if byte == 10:
				if not cr:
					var _inserted := out.append(13)
				cr = false
				line_start = true
				_appended = out.append(byte)
				continue
			cr = false
			if line_start and byte == 46:
				_appended = out.append(46)
			line_start = false
			_appended = out.append(byte)
		var sent_v, sent_e := await client.conn.write_async(out)
		writing = false
		if sent_e != null:
			client.conn.close()
		if sent_e != null:
			return sent_v, sent_e
		return data.size()

	# End DATA and require the server's acceptance response.
	func close() -> Variant, Err:
		if closed or writing:
			return null, Err.from("SMTP DATA writer is closed", Err.INTERRUPTED)
		closed = true
		if cr:
			client.conn.close()
			client.data_active = false
			return null, Err.from("bare CR at end of SMTP DATA", Err.INVALID_DATA)
		var suffix := PackedByteArray([46, 13, 10]) if line_start else PackedByteArray([13, 10, 46, 13, 10])
		var sent_v, sent_e := await client.conn.write_async(suffix)
		if sent_e != null:
			client.conn.close()
			client.data_active = false
			return sent_v, sent_e
		var accepted_v, accepted_e := await client._expect("DATA", [250])
		client.data_active = false
		return accepted_v, accepted_e


class Client:
	extends RefCounted
	var conn: GDTCPConn
	var host: String
	var unread := PackedByteArray()
	var caps := {}
	var data_active := false
	var command_active := false

	# Retain one connection and its unread SMTP reply bytes.
	func _init(p_conn: GDTCPConn, p_host: String):
		conn = p_conn
		host = p_host

	# Read one complete CRLF-terminated SMTP reply line.
	func _line() -> Variant, Err:
		while true:
			for i in range(1, unread.size()):
				if unread[i - 1] == 13 and unread[i] == 10:
					var line, line_error := GD.mail._utf8_text(unread.slice(0, i - 1))
					unread = unread.slice(i + 1)
					if line_error != null:
						return null, line_error.note("SMTP reply")
					return line
			var received_v, received_e := await conn.read_async(4096)
			if received_e != null:
				conn.close()
				return null, received_e
			var bytes: PackedByteArray = received_v
			if bytes.is_empty():
				conn.close()
				return null, Err.from("SMTP reply ended before CRLF", Err.INVALID_DATA)
			unread.append_array(bytes)
		return null, Err.from("SMTP reply unavailable", Err.INTERRUPTED)

	# Decode one SMTP reply, including all lines with the same code.
	func _reply() -> Variant, Err:
		var code := -1
		var lines: Array[String] = []
		while true:
			var got_v, got_e := await _line()
			if got_e != null:
				return null, got_e
			var line: String = got_v
			if line.length() < 4 or not line.substr(0, 3).is_valid_int() or (line[3] != "-" and line[3] != " "):
				conn.close()
				return null, Err.from("malformed SMTP reply", Err.INVALID_DATA)
			var number := int(line.substr(0, 3))
			if code != -1 and number != code:
				conn.close()
				return null, Err.from("SMTP reply code changed within a response", Err.INVALID_DATA)
			code = number
			lines.append(line.substr(4))
			if line[3] == " ":
				return {"code": code, "text": "\n".join(lines)}
		return null, Err.from("SMTP reply unavailable", Err.INTERRUPTED)

	# Require the response code expected at one protocol stage.
	func _expect(stage: String, accepted: Array) -> Variant, Err:
		var reply_v, reply_e := await _reply()
		if reply_e != null:
			return null, reply_e
		if int(reply_v.code) in accepted:
			return reply_v, reply_e
		var info := {"code": reply_v.code, "stage": stage, "reply": reply_v.text}
		var kind := Err.UNAUTHENTICATED if stage == "AUTH" else Err.INVALID_DATA
		return null, Err.from(Err("SMTP %s failed: %d" % [stage, reply_v.code], kind, info), Err.ERROR)

	# Send one command after rejecting embedded line breaks.
	func _say(stage: String, command: String, accepted: Array) -> Variant, Err:
		if data_active or command_active or "\r" in command or "\n" in command:
			return null, Err.from("invalid SMTP command", Err.INVALID_DATA)
		command_active = true
		var sent_v, sent_e := await conn.write_async((command + "\r\n").to_utf8_buffer())
		if sent_e != null:
			command_active = false
			conn.close()
			return null, sent_e
		var reply_v, reply_e := await _expect(stage, accepted)
		command_active = false
		return reply_v, reply_e

	# Introduce this client and record the server's extensions.
	func hello(name: String = "localhost") -> Variant, Err:
		if "\r" in name or "\n" in name or name.is_empty():
			return null, Err.from("invalid SMTP hello name", Err.INVALID_DATA)
		var result_v, result_e := await _say("EHLO", "EHLO " + name, [250])
		if result_e != null:
			var fallback_v, fallback_e := await _say("HELO", "HELO " + name, [250])
			if fallback_e != null:
				return fallback_v, fallback_e
			caps.clear()
			return fallback_v, fallback_e
		caps.clear()
		for line in String(result_v.text).split("\n"):
			var words := line.split(" ", false, 1)
			if words.size() > 0:
				caps[words[0].to_upper()] = words[1] if words.size() > 1 else ""
		return result_v, result_e

	# Report an advertised extension and its parameters.
	func extension(name: String) -> Dictionary:
		return {"supported": caps.has(name.to_upper()), "value": caps.get(name.to_upper(), "")}

	# Upgrade this SMTP session and discard pre-TLS capabilities.
	func start_tls(opts: Dictionary = {}) -> Variant, Err:
		if not caps.has("STARTTLS"):
			return null, Err.from("SMTP server did not advertise STARTTLS", Err.UNSUPPORTED)
		var response_v, response_e := await _say("STARTTLS", "STARTTLS", [220])
		if response_e != null:
			return response_v, response_e
		unread.clear()
		var secured_v, secured_e := await conn.start_tls_async(host, opts)
		if secured_e != null:
			conn.close()
			return secured_v, secured_e
		conn = secured_v
		caps.clear()
		return await hello()

	# Authenticate only on TLS or a loopback connection.
	func auth(config: Dictionary) -> Variant, Err:
		if not conn.connection_state().handshake_complete and host not in ["localhost", "127.0.0.1", "::1"]:
			return null, Err.from("SMTP authentication requires TLS", Err.PERMISSION_DENIED)
		var kind: String = config.get("kind", "")
		var user: String = config.get("user", "")
		var secret: String = config.get("secret", "")
		var user_bytes := user.to_utf8_buffer()
		var secret_bytes := secret.to_utf8_buffer()
		if user.is_empty() or user_bytes.has(0) or secret_bytes.has(0):
			return null, Err.from("invalid SMTP credentials", Err.INVALID_DATA)
		if kind == "plain":
			var identity: String = config.get("identity", "")
			var token := identity.to_utf8_buffer()
			if token.has(0):
				return null, Err.from("invalid SMTP identity", Err.INVALID_DATA)
			token.append_array(PackedByteArray([0]))
			token.append_array(user_bytes)
			token.append_array(PackedByteArray([0]))
			token.append_array(secret_bytes)
			return await _say("AUTH", "AUTH PLAIN " + GD.data.base64_encode(token), [235])
		if kind == "cram_md5":
			var start_v, start_e := await _say("AUTH", "AUTH CRAM-MD5", [334])
			if start_e != null:
				return start_v, start_e
			var challenge_v, challenge_e := GD.data.base64_decode(String(start_v.text).strip_edges())
			if challenge_e != null:
				conn.close()
				return null, challenge_e
			var digest_v, digest_e := GD.mail.cram_md5(secret.to_utf8_buffer(), challenge_v)
			if digest_e != null:
				conn.close()
				return null, digest_e
			var answer := (user + " " + str(digest_v)).to_utf8_buffer()
			return await _say("AUTH", GD.data.base64_encode(answer), [235])
		return null, Err.from("unsupported SMTP authentication", Err.UNSUPPORTED)

	# Detect addresses that require an advertised UTF-8 SMTP extension.
	func _needs_utf8(value: String) -> bool:
		for c in value:
			if c.unicode_at(0) > 127:
				return true
		return false

	# Begin the envelope and use only extensions advertised by this server.
	func mail(from: String) -> Variant, Err:
		if from.contains("\r") or from.contains("\n") or from.contains("<") or from.contains(">"):
			return null, Err.from("invalid SMTP sender", Err.INVALID_DATA)
		if _needs_utf8(from) and not caps.has("SMTPUTF8"):
			return null, Err.from("SMTPUTF8 is required for this sender", Err.UNSUPPORTED)
		var command := "MAIL FROM:<" + from + ">"
		if caps.has("8BITMIME"):
			command += " BODY=8BITMIME"
		if caps.has("SMTPUTF8"):
			command += " SMTPUTF8"
		return await _say("MAIL", command, [250])

	# Add one envelope recipient.
	func rcpt(to: String) -> Variant, Err:
		if to.is_empty() or to.contains("\r") or to.contains("\n") or to.contains("<") or to.contains(">"):
			return null, Err.from("invalid SMTP recipient", Err.INVALID_DATA)
		if _needs_utf8(to) and not caps.has("SMTPUTF8"):
			return null, Err.from("SMTPUTF8 is required for this recipient", Err.UNSUPPORTED)
		return await _say("RCPT", "RCPT TO:<" + to + ">", [250, 251])

	# Enter DATA mode and return a streaming writer.
	func data() -> Variant, Err:
		var response_v, response_e := await _say("DATA", "DATA", [354])
		if response_e != null:
			return response_v, response_e
		data_active = true
		return GD.mail._wrap_writer(DataWriter.new(self))

	# Abort one transaction while retaining the connection.
	func reset() -> Variant, Err:
		return await _say("RSET", "RSET", [250])

	# Test whether the session still responds.
	func noop() -> Variant, Err:
		return await _say("NOOP", "NOOP", [250])

	# Ask the server about a mailbox without treating refusal as invalidity proof.
	func verify(address: String) -> Variant, Err:
		if address.contains("\r") or address.contains("\n"):
			return null, Err.from("invalid SMTP address", Err.INVALID_DATA)
		return await _say("VRFY", "VRFY " + address, [250, 251, 252])

	# End the SMTP session and release the transport.
	func quit() -> Variant, Err:
		var result_v, result_e := await _say("QUIT", "QUIT", [221])
		conn.close()
		return result_v, result_e

	# Release the transport immediately.
	func close() -> void:
		conn.close()

	# Report the verified connection's TLS state.
	func tls_state() -> Dictionary:
		return conn.connection_state()


class BodyReader:
	extends RefCounted
	var source
	var unread: PackedByteArray

	# Retain bytes already read past the header and the remaining source.
	func _init(p_source, p_unread: PackedByteArray):
		source = p_source
		unread = p_unread

	# Return body bytes incrementally without reading past the requested amount.
	func read(max_bytes: int = 32768) -> Variant, Err:
		if max_bytes < 0:
			return null, Err.from("negative mail body read length", Err.INVALID_DATA)
		if max_bytes == 0:
			return PackedByteArray()
		if not unread.is_empty():
			var take := min(max_bytes, unread.size())
			var out := unread.slice(0, take)
			unread = unread.slice(take)
			return out
		if source == null:
			return PackedByteArray()
		return await GD.async.spawn_pair(Callable(source, "read_async").bind(max_bytes))


class MultipartReader:
	extends RefCounted
	var data: PackedByteArray
	var marker: PackedByteArray
	var pos := 0
	var started := false
	var finished := false

	# Retain the byte boundary and cursor for incremental part traversal.
	func _init(p_data: PackedByteArray, p_boundary: String):
		data = p_data
		marker = ("--" + p_boundary).to_utf8_buffer()

	# Find a byte pattern without decoding a binary attachment.
	func _find(needle: PackedByteArray, from: int) -> int:
		for i in range(from, data.size() - needle.size() + 1):
			var found := true
			for j in range(needle.size()):
				if data[i + j] != needle[j]:
					found = false
					break
			if found:
				return i
		return -1

	# Return one part's headers and bytes, or null after the closing boundary.
	func next_part() -> Variant, Err:
		if finished:
			return null
		if not started:
			var first := _find(marker, 0)
			while first > 0 and (first < 2 or data[first - 2] != 13 or data[first - 1] != 10):
				first = _find(marker, first + 1)
			if first < 0:
				return null, Err.from("multipart opening boundary missing", Err.INVALID_DATA)
			pos = first
			started = true
		if pos + marker.size() + 1 < data.size() and data[pos + marker.size()] == 45 and data[pos + marker.size() + 1] == 45:
			finished = true
			return null
		var body_start := pos + marker.size()
		if body_start + 1 >= data.size() or data[body_start] != 13 or data[body_start + 1] != 10:
			return null, Err.from("invalid multipart delimiter", Err.INVALID_DATA)
		body_start += 2
		var next_marker := PackedByteArray([13, 10])
		next_marker.append_array(marker)
		var end := _find(next_marker, body_start)
		while end >= 0:
			var after := end + next_marker.size()
			if after + 1 < data.size() and ((data[after] == 13 and data[after + 1] == 10) or (data[after] == 45 and data[after + 1] == 45)):
				break
			end = _find(next_marker, end + 1)
		if end < 0:
			return null, Err.from("multipart closing boundary missing", Err.INVALID_DATA)
		var part_v, part_e := GD.mail.read_message(data.slice(body_start, end))
		if part_e != null:
			return part_v, part_e
		pos = end + 2
		return part_v, part_e


class MultipartStreamPart:
	extends RefCounted
	var parent
	var id: int

	# Retain the owning reader while one part body is being consumed.
	func _init(p_parent, p_id: int):
		parent = p_parent
		id = p_id

	# Read at most the requested bytes before the next MIME boundary.
	func read_async(max_bytes: int = 32768) -> Variant, Err:
		if id != parent.part_id:
			return null, Err.from("multipart part is no longer current", Err.INTERRUPTED)
		return await GD.async.spawn_pair(Callable(parent, "_read_part").bind(max_bytes))


class MultipartStreamReader:
	extends RefCounted
	var source
	var marker: PackedByteArray
	var buf := PackedByteArray()
	var started := false
	var finished := false
	var part_open := false
	var busy := false
	var eof := false
	var part_id := 0

	# Retain only the unread boundary window and current part headers.
	func _init(p_source, p_boundary: String):
		source = p_source
		marker = ("--" + p_boundary).to_utf8_buffer()

	# Find one exact byte marker in the currently buffered input.
	func _find(needle: PackedByteArray, at: int = 0) -> int:
		for i in range(at, buf.size() - needle.size() + 1):
			var same := true
			for j in range(needle.size()):
				if buf[i + j] != needle[j]:
					same = false
					break
			if same:
				return i
		return -1

	# Fetch one chunk without collecting earlier body bytes.
	func _fill() -> Variant, Err:
		if eof:
			return false
		var got_v, got_e := await GD.async.spawn_pair(Callable(source, "read_async").bind(32768))
		if got_e != null:
			return got_v, got_e
		if got_v.is_empty():
			eof = true
			return false
		buf.append_array(got_v)
		return true

	# Read and consume bytes up to the next MIME delimiter.
	func _read_part(max_bytes: int) -> Variant, Err:
		if max_bytes < 0 or busy:
			return null, Err.from("invalid multipart read", Err.INVALID_DATA)
		if max_bytes == 0:
			return PackedByteArray()
		if not part_open:
			return PackedByteArray()
		busy = true
		var delim := PackedByteArray([13, 10])
		delim.append_array(marker)
		while true:
			var at := _find(delim)
			if at >= 0 and buf.size() < at + delim.size() + 2 and not eof:
				var extended_v, extended_e := await _fill()
				if extended_e != null:
					busy = false
					return extended_v, extended_e
				continue
			if at >= 0 and buf.size() >= at + delim.size() + 2:
				var tail := at + delim.size()
				if (buf[tail] == 13 and buf[tail + 1] == 10) or (buf[tail] == 45 and buf[tail + 1] == 45):
					var take := min(max_bytes, at)
					var out := buf.slice(0, take)
					buf = buf.slice(take)
					if take == at:
						buf = buf.slice(2)
						part_open = false
					busy = false
					return out
			var safe := buf.size() - delim.size() - 2
			if safe > 0:
				var take := min(max_bytes, safe)
				var out := buf.slice(0, take)
				buf = buf.slice(take)
				busy = false
				return out
			if eof:
				busy = false
				return null, Err.from("multipart closing boundary missing", Err.INVALID_DATA)
			var filled_v, filled_e := await _fill()
			if filled_e != null:
				busy = false
				return filled_v, filled_e
		return null, Err.from("multipart input unavailable", Err.INTERRUPTED)

	# Advance to the next part and leave its body attached to this reader.
	func next_part_async() -> Variant, Err:
		if finished or busy:
			if finished:
				return null
			return null, Err.from("multipart reader is busy", Err.INVALID_DATA)
		if part_open:
			while part_open:
				var discarded_v, discarded_e := await _read_part(32768)
				if discarded_e != null:
					return discarded_v, discarded_e
		if not started:
			while true:
				var at := _find(marker)
				while at >= 0 and at > 0 and (at < 2 or buf[at - 2] != 13 or buf[at - 1] != 10):
					at = _find(marker, at + 1)
				if at >= 0 and (at == 0 or (at >= 2 and buf[at - 2] == 13 and buf[at - 1] == 10)):
					buf = buf.slice(at)
					break
				if eof:
					return null, Err.from("multipart opening boundary missing", Err.INVALID_DATA)
				if buf.size() > marker.size() + 2:
					buf = buf.slice(buf.size() - marker.size() - 2)
				var filled_v, filled_e := await _fill()
				if filled_e != null:
					return filled_v, filled_e
			started = true
		while buf.size() < marker.size() + 2:
			var filled_v, filled_e := await _fill()
			if filled_e != null or eof:
				if filled_e != null:
					return filled_v, filled_e
				return null, Err.from("truncated multipart boundary", Err.INVALID_DATA)
		if buf.slice(0, marker.size()) != marker:
			return null, Err.from("invalid multipart boundary", Err.INVALID_DATA)
		buf = buf.slice(marker.size())
		if buf[0] == 45 and buf[1] == 45:
			finished = true
			return null
		if buf[0] != 13 or buf[1] != 10:
			return null, Err.from("invalid multipart delimiter", Err.INVALID_DATA)
		buf = buf.slice(2)
		if buf.size() < 2:
			var initial_v, initial_e := await _fill()
			if initial_e != null:
				return initial_v, initial_e
		if buf.size() >= 2 and buf[0] == 13 and buf[1] == 10:
			buf = buf.slice(2)
			part_open = true
			part_id += 1
			return {"headers": {}, "body": MultipartStreamPart.new(self, part_id)}
		var sep := PackedByteArray([13, 10, 13, 10])
		var scan := 0
		while true:
			var end := _find(sep, scan)
			if end >= 0:
				var parsed_v: Variant = {"headers": {}}
				var parsed_e: Err
				if end > 0:
					var message_v, message_e := GD.mail.read_message(buf.slice(0, end + 4))
					parsed_v = message_v
					parsed_e = message_e
				if parsed_e != null:
					return parsed_v, parsed_e
				buf = buf.slice(end + 4)
				part_open = true
				part_id += 1
				return {"headers": parsed_v.headers, "body": MultipartStreamPart.new(self, part_id)}
			if eof:
				return null, Err.from("multipart headers ended early", Err.INVALID_DATA)
			scan = max(0, buf.size() - 3)
			var filled_v, filled_e := await _fill()
			if filled_e != null:
				return filled_v, filled_e
		return null, Err.from("multipart part unavailable", Err.INTERRUPTED)


class MultipartPartWriter:
	extends RefCounted
	var parent
	var headers: Dictionary
	var data := PackedByteArray()
	var closed := false

	# Retain one part's headers until its body is complete.
	func _init(p_parent, p_headers: Dictionary):
		parent = p_parent
		headers = p_headers

	# Append part bytes without interpreting their encoding.
	func write(chunk: PackedByteArray) -> Variant, Err:
		if closed:
			return null, Err.from("multipart part is closed", Err.INTERRUPTED)
		data.append_array(chunk)
		return chunk.size()

	# Commit one part to its parent writer.
	func close() -> Variant, Err:
		if closed:
			return null, Err.from("multipart part is closed", Err.INTERRUPTED)
		closed = true
		return GD.async.call_pair(Callable(parent, "_finish").bind(headers, data))


class MultipartWriter:
	extends RefCounted
	var boundary: String
	var body := PackedByteArray()
	var active := false
	var closed := false

	# Choose one safe boundary for every part in this body.
	func _init(p_boundary: String):
		boundary = p_boundary

	# Return the Content-Type value needed by the enclosing message.
	func content_type() -> String:
		return "multipart/mixed; boundary=\"" + boundary + "\""

	# Reject malformed field names and line breaks before writing a part.
	static func valid_headers(headers: Dictionary) -> bool:
		for key in headers:
			var name := str(key)
			if name.is_empty() or str(headers[key]).contains("\r") or str(headers[key]).contains("\n"):
				return false
			for c in name:
				if c.unicode_at(0) < 33 or c.unicode_at(0) > 126 or c == ":":
					return false
		return true

	# Begin one part with its own MIME header fields.
	func create_part(headers: Dictionary) -> Variant, Err:
		if active or closed:
			return null, Err.from("multipart writer already has an open part", Err.INVALID_DATA)
		if not valid_headers(headers):
			return null, Err.from("invalid MIME part header", Err.INVALID_DATA)
		active = true
		return MultipartPartWriter.new(self, headers)

	# Add one complete part in wire order.
	func _finish(headers: Dictionary, data: PackedByteArray) -> Variant, Err:
		body.append_array(("--" + boundary + "\r\n").to_utf8_buffer())
		for key in headers:
			body.append_array((str(key) + ": " + str(headers[key]) + "\r\n").to_utf8_buffer())
		body.append_array(PackedByteArray([13, 10]))
		body.append_array(data)
		body.append_array(PackedByteArray([13, 10]))
		active = false
		return null

	# Add the terminal boundary and return the assembled body.
	func close() -> Variant, Err:
		if active or closed:
			return null, Err.from("multipart writer has an open part or is closed", Err.INVALID_DATA)
		closed = true
		body.append_array(("--" + boundary + "--\r\n").to_utf8_buffer())
		return body


class MultipartStreamPartWriter:
	extends RefCounted
	var parent
	var closed := false
	var busy := false

	# Keep the active part attached to its destination stream.
	func _init(p_parent):
		parent = p_parent

	# Forward attachment bytes only after the destination accepts them.
	func write_async(data: PackedByteArray) -> Variant, Err:
		if closed or busy or parent.closed:
			return null, Err.from("multipart part is closed or busy", Err.INTERRUPTED)
		busy = true
		var sent_v, sent_e := await GD.async.spawn_pair(Callable(parent.sink, "write_async").bind(data))
		busy = false
		if sent_e != null:
			parent.closed = true
		return sent_v, sent_e

	# End this part so the next boundary can be written.
	func close_async() -> Variant, Err:
		if closed or busy:
			return null, Err.from("multipart part is closed or busy", Err.INTERRUPTED)
		closed = true
		var sent_v, sent_e := await GD.async.spawn_pair(Callable(parent.sink, "write_async").bind(PackedByteArray([13, 10])))
		parent.active = false
		if sent_e != null:
			parent.closed = true
		return sent_v, sent_e


class MultipartStreamWriter:
	extends RefCounted
	var sink
	var boundary: String
	var active := false
	var closed := false

	# Retain a backpressured destination and one boundary spelling.
	func _init(p_sink, p_boundary: String):
		sink = p_sink
		boundary = p_boundary

	# Return the Content-Type value needed by the enclosing message.
	func content_type() -> String:
		return "multipart/mixed; boundary=\"" + boundary + "\""

	# Begin one part after writing its boundary and header block.
	func create_part_async(headers: Dictionary) -> Variant, Err:
		if active or closed:
			return null, Err.from("multipart writer already has an open part", Err.INVALID_DATA)
		if not MultipartWriter.valid_headers(headers):
			return null, Err.from("invalid MIME part header", Err.INVALID_DATA)
		var head := "--" + boundary + "\r\n"
		for key in headers:
			head += str(key) + ": " + str(headers[key]) + "\r\n"
		head += "\r\n"
		active = true
		var sent_v, sent_e := await GD.async.spawn_pair(Callable(sink, "write_async").bind(head.to_utf8_buffer()))
		if sent_e != null:
			closed = true
			return sent_v, sent_e
		return MultipartStreamPartWriter.new(self)

	# Write the terminal boundary after all parts have ended.
	func close_async() -> Variant, Err:
		if active or closed:
			return null, Err.from("multipart writer has an open part or is closed", Err.INVALID_DATA)
		closed = true
		return await GD.async.spawn_pair(Callable(sink, "write_async").bind(("--" + boundary + "--\r\n").to_utf8_buffer()))


class QuotedPrintableReader:
	extends RefCounted
	var source
	var raw := PackedByteArray()
	var at := 0
	var eof := false
	var busy := false

	# Retain undecoded escapes across source read boundaries.
	func _init(p_source):
		source = p_source

	# Decode at most one requested block without collecting the whole source.
	func read_async(max_bytes: int = 32768) -> Variant, Err:
		if max_bytes < 0 or busy:
			return null, Err.from("invalid quoted-printable read", Err.INVALID_DATA)
		if max_bytes == 0:
			return PackedByteArray()
		busy = true
		var out := PackedByteArray()
		var hex := "0123456789ABCDEF"
		var _appended: bool
		while out.size() < max_bytes:
			if at >= raw.size() or (raw[at] == 61 and raw.size() - at < 3):
				if eof:
					if at < raw.size():
						busy = false
						return out, Err.from("truncated quoted-printable escape", Err.INVALID_DATA).with_partial(out)
					break
				raw = raw.slice(at)
				at = 0
				var received_v, received_e := await GD.async.spawn_pair(Callable(source, "read_async").bind(32768))
				if received_e != null:
					busy = false
					return received_v, received_e
				if received_v.is_empty():
					eof = true
				else:
					raw.append_array(received_v)
				continue
			if raw[at] != 61:
				_appended = out.append(raw[at])
				at += 1
				continue
			if raw[at + 1] == 13 and raw[at + 2] == 10:
				at += 3
				continue
			var hi := hex.find(String.chr(raw[at + 1]).to_upper())
			var lo := hex.find(String.chr(raw[at + 2]).to_upper())
			if hi < 0 or lo < 0:
				busy = false
				return out, Err.from("invalid quoted-printable escape", Err.INVALID_DATA).with_partial(out)
			_appended = out.append(hi * 16 + lo)
			at += 3
		busy = false
		return out


class QuotedPrintableWriter:
	extends RefCounted
	var sink
	var line := 0
	var cr := false
	var whitespace := PackedByteArray()
	var out := PackedByteArray()
	var busy := false
	var closed := false

	# Retain line and whitespace state between destination writes.
	func _init(p_sink):
		sink = p_sink

	# Append one printable token while folding long encoded lines.
	func _token(token: PackedByteArray) -> void:
		if line + token.size() > 75:
			out.append_array(PackedByteArray([61, 13, 10]))
			line = 0
		out.append_array(token)
		line += token.size()

	# Encode one byte when wire syntax requires an escape.
	func _byte(byte: int, force: bool = false) -> void:
		if force or byte == 61 or byte < 32 or byte > 126:
			var hex := "0123456789ABCDEF"
			_token(("=" + hex[byte >> 4] + hex[byte & 15]).to_utf8_buffer())
		else:
			_token(PackedByteArray([byte]))

	# Flush spaces and tabs after deciding whether a line ends here.
	func _spaces(trailing: bool) -> void:
		for byte in whitespace:
			_byte(byte, trailing)
		whitespace.clear()

	# Accept one source byte while preserving CRLF across calls.
	func _accept(byte: int) -> void:
		if cr:
			cr = false
			if byte == 10:
				_spaces(true)
				out.append_array(PackedByteArray([13, 10]))
				line = 0
				return
			_spaces(false)
			_byte(13)
		if byte == 13:
			cr = true
		elif byte == 32 or byte == 9:
			var _appended := whitespace.append(byte)
		else:
			_spaces(false)
			_byte(byte)

	# Write encoded bytes only after the sink accepts this chunk.
	func write_async(data: PackedByteArray) -> Variant, Err:
		if closed or busy:
			return null, Err.from("quoted-printable writer is closed or busy", Err.INTERRUPTED)
		busy = true
		for byte in data:
			_accept(byte)
		var sent_v, sent_e := await GD.async.spawn_pair(Callable(sink, "write_async").bind(out))
		out.clear()
		busy = false
		if sent_e != null:
			closed = true
		if sent_e != null:
			return sent_v, sent_e
		return data.size()

	# Flush a final CR or trailing whitespace without closing the sink.
	func close_async() -> Variant, Err:
		if closed or busy:
			return null, Err.from("quoted-printable writer is closed or busy", Err.INTERRUPTED)
		closed = true
		if cr:
			_byte(13)
		_spaces(true)
		var sent_v, sent_e := await GD.async.spawn_pair(Callable(sink, "write_async").bind(out))
		out.clear()
		if sent_e != null:
			return sent_v, sent_e
		return null


# Connect to an SMTP submission server with the requested TLS policy.
func dial_smtp(host: String, port: int, opts: Dictionary = {}) -> Variant, Err:
	var mode: String = opts.get("tls", "starttls")
	if host.is_empty() or port < 1 or port > 65535 or mode not in ["starttls", "implicit", "plain"]:
		return null, Err.from("invalid SMTP host, port, or TLS mode", Err.INVALID_DATA)
	var net_opts := {"timeout": opts.get("timeout", 0.0)}
	for key in ["ca_file", "cert_file", "key_file", "server_name"]:
		if opts.has(key):
			net_opts[key] = opts[key]
	var opened_v: Variant
	var opened_e: Err
	if mode == "implicit":
		var tls_v, tls_e := await GD.net.dial_tls_async(host, port, net_opts)
		opened_v = tls_v
		opened_e = tls_e
	else:
		var tcp_v, tcp_e := await GD.net.dial_tcp_async(host, port, {"timeout": net_opts.timeout})
		opened_v = tcp_v
		opened_e = tcp_e
	if opened_e != null:
		return null, opened_e
	var client := Client.new(opened_v, host)
	var deadline_e: Err = client.conn.set_deadline(float(opts.get("timeout", 0.0)))
	if deadline_e != null:
		client.close()
		return null, deadline_e
	var greeting_v, greeting_e := await client._expect("greeting", [220])
	if greeting_e != null:
		client.close()
		return null, greeting_e
	var introduced_v, introduced_e := await client.hello(str(opts.get("hello", "localhost")))
	if introduced_e != null:
		client.close()
		return null, introduced_e
	if mode == "starttls":
		var secured_v, secured_e := await client.start_tls(net_opts)
		if secured_e != null:
			client.close()
			return null, secured_e
	return GD.mail._wrap_client(client)


# Deliver one RFC 5322 message to every requested envelope recipient.
func send_mail(host: String, port: int, from: String, to: PackedStringArray, message: PackedByteArray, opts: Dictionary = {}) -> Variant, Err:
	if to.is_empty():
		return null, Err.from("SMTP needs at least one recipient", Err.INVALID_DATA)
	var opened_v, opened_e := await dial_smtp(host, port, opts)
	if opened_e != null:
		return opened_v, opened_e
	var client: GDSMTPClient = opened_v
	var need_utf8: bool = not client.extension("SMTPUTF8").supported
	var need_8bit: bool = not client.extension("8BITMIME").supported
	if need_utf8 or need_8bit:
		var in_headers := true
		for i in range(message.size()):
			if message[i] > 127 and ((in_headers and need_utf8) or (not in_headers and need_8bit)):
				client.close()
				return null, Err.from("SMTP server cannot accept this message encoding", Err.UNSUPPORTED)
			if (i >= 3 and message[i - 3] == 13 and message[i - 2] == 10 and message[i - 1] == 13 and message[i] == 10) or (i >= 1 and message[i - 1] == 10 and message[i] == 10):
				in_headers = false
	var auth_config: Dictionary = opts.get("auth", {})
	if not auth_config.is_empty():
		var authenticated_v, authenticated_e := await client.auth(auth_config)
		if authenticated_e != null:
			client.close()
			return null, authenticated_e
	var sender_v, sender_e := await client.mail(from)
	if sender_e != null:
		client.close()
		return null, sender_e
	var accepted := PackedStringArray()
	var _appended: bool
	for recipient in to:
		var result_v, result_e := await client.rcpt(recipient)
		if result_e != null:
			client.close()
			return accepted, Err.from(result_e, Err.ERROR).with_partial(accepted)
		_appended = accepted.append(recipient)
	var started_v, started_e := await client.data()
	if started_e != null:
		client.close()
		return accepted, started_e.with_partial(accepted)
	var writer: GDSMTPDataWriter = started_v
	for at in range(0, message.size(), 32768):
		var wrote_v, wrote_e := await writer.write(message.slice(at, min(at + 32768, message.size())))
		if wrote_e != null:
			client.close()
			return accepted, Err.from(wrote_e, Err.ERROR).with_partial(accepted)
	var completed_v, completed_e := await writer.close()
	if completed_e != null:
		client.close()
		return accepted, completed_e.with_partial(accepted)
	var finished_v, finished_e := await client.quit()
	if finished_e != null:
		return accepted, finished_e.with_partial(accepted)
	return accepted


# Split an address list only at commas outside quoted names and angle brackets.
func parse_address_list(text: String) -> Variant, Err:
	var items: Array = []
	var start := 0
	var quoted := false
	var escaped := false
	var angle := 0
	var comments := 0
	for i in range(text.length()):
		var c := text[i]
		if escaped:
			escaped = false
		elif (quoted or comments > 0) and c == "\\":
			escaped = true
		elif comments == 0 and c == "\"":
			quoted = not quoted
		elif not quoted and c == "(":
			comments += 1
		elif not quoted and c == ")":
			comments -= 1
		elif not quoted and comments == 0 and c == "<":
			angle += 1
		elif not quoted and comments == 0 and c == ">":
			angle -= 1
		elif not quoted and comments == 0 and angle == 0 and c == ",":
			var parsed_v, parsed_e := parse_address(text.substr(start, i - start))
			if parsed_e != null:
				return items, parsed_e.with_partial(items)
			items.append(parsed_v)
			start = i + 1
	if quoted or angle != 0 or comments != 0:
		return null, Err.from("unfinished mail address", Err.INVALID_DATA)
	var final_v, final_e := parse_address(text.substr(start))
	if final_e != null:
		return items, final_e.with_partial(items)
	items.append(final_v)
	return items


# Remove balanced RFC address comments outside quoted strings.
func _without_comments(text: String) -> Variant, Err:
	var out := ""
	var quoted := false
	var escaped := false
	var depth := 0
	for c in text:
		if escaped:
			if depth == 0:
				out += c
			escaped = false
		elif c == "\\" and (quoted or depth > 0):
			if depth == 0:
				out += c
			escaped = true
		elif c == "\"" and depth == 0:
			quoted = not quoted
			out += c
		elif c == "(" and not quoted:
			depth += 1
		elif c == ")" and not quoted:
			depth -= 1
			if depth < 0:
				return null, Err.from("unbalanced mail comment", Err.INVALID_DATA)
		elif depth == 0:
			out += c
	if depth != 0 or quoted:
		return null, Err.from("unfinished mail comment or quote", Err.INVALID_DATA)
	return out


# Parse a mailbox and optional display name.
func parse_address(text: String) -> Variant, Err:
	var cleaned_v, cleaned_e := _without_comments(text)
	if cleaned_e != null:
		return cleaned_v, cleaned_e
	var raw: String = cleaned_v.strip_edges()
	var name := ""
	var address := raw
	var left := raw.find("<")
	if left >= 0:
		var right := raw.rfind(">")
		if right <= left or not raw.substr(right + 1).strip_edges().is_empty():
			return null, Err.from("invalid mail address brackets", Err.INVALID_DATA)
		name = raw.substr(0, left).strip_edges()
		address = raw.substr(left + 1, right - left - 1).strip_edges()
		if name.begins_with("\"") and name.ends_with("\"") and name.length() >= 2:
			name = name.substr(1, name.length() - 2).replace("\\\"", "\"").replace("\\\\", "\\")
		elif name.contains("=?"):
			var decoded_v, decoded_e := mime_decode_header(name)
			if decoded_e != null:
				return decoded_v, decoded_e
			name = decoded_v
		else:
			name = " ".join(name.replace("\t", " ").split(" ", false))
	if address.is_empty() or address.contains("\r") or address.contains("\n"):
		return null, Err.from("invalid mail address", Err.INVALID_DATA)
	var at := -1
	var quoted := false
	var escaped := false
	for i in range(address.length()):
		var c := address[i]
		if escaped:
			escaped = false
		elif quoted and c == "\\":
			escaped = true
		elif c == "\"":
			quoted = not quoted
		elif c == "@" and not quoted:
			if at >= 0:
				return null, Err.from("invalid mail address", Err.INVALID_DATA)
			at = i
	if quoted or at <= 0 or at == address.length() - 1:
		return null, Err.from("invalid mail address", Err.INVALID_DATA)
	var local := address.substr(0, at)
	var domain := address.substr(at + 1)
	if local.begins_with("\""):
		if not local.ends_with("\"") or local.length() < 3:
			return null, Err.from("invalid quoted mail local part", Err.INVALID_DATA)
		var contents := local.substr(1, local.length() - 2).replace("\\\"", "\"").replace("\\\\", "\\")
		if contents.contains("\r") or contents.contains("\n"):
			return null, Err.from("invalid quoted mail local part", Err.INVALID_DATA)
	else:
		if local.begins_with(".") or local.ends_with(".") or local.contains(".."):
			return null, Err.from("invalid mail local part", Err.INVALID_DATA)
		for c in local:
			if c.unicode_at(0) <= 32 or c.unicode_at(0) == 127 or "()<>[]:;@\\,\"".contains(c):
				return null, Err.from("invalid mail local part", Err.INVALID_DATA)
	if domain.begins_with("["):
		if not domain.ends_with("]") or domain.length() < 3:
			return null, Err.from("invalid mail domain literal", Err.INVALID_DATA)
	else:
		if domain.begins_with(".") or domain.ends_with(".") or domain.contains(".."):
			return null, Err.from("invalid mail domain", Err.INVALID_DATA)
		for c in domain:
			if c.unicode_at(0) <= 32 or c.unicode_at(0) == 127 or "()<>[]:;@\\,\"".contains(c):
				return null, Err.from("invalid mail domain", Err.INVALID_DATA)
	return {"name": name, "address": address}


# Parse an RFC 5322 numeric-zone date into UTC seconds and its original offset.
func parse_date(text: String) -> Variant, Err:
	var cleaned_v, cleaned_e := _without_comments(text)
	if cleaned_e != null:
		return cleaned_v, cleaned_e
	var value := String(cleaned_v).strip_edges()
	var comma := value.find(",")
	if comma >= 0:
		value = value.substr(comma + 1).strip_edges()
	var fields := value.split(" ", false)
	if fields.size() != 5:
		return null, Err.from("invalid mail date", Err.INVALID_DATA)
	var months := ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
	var month := months.find(fields[1]) + 1
	var time := fields[3].split(":")
	var zone: String = fields[4]
	var numeric_zone := zone.length() == 5 and zone[0] in ["+", "-"] and zone.substr(1).is_valid_int()
	var named_zone := zone.length() >= 2 and zone.is_valid_identifier() and zone == zone.to_upper()
	if month == 0 or not fields[0].is_valid_int() or not fields[2].is_valid_int() or time.size() < 2 or time.size() > 3 or (not numeric_zone and not named_zone):
		return null, Err.from("invalid mail date", Err.INVALID_DATA)
	for field in time:
		if not field.is_valid_int():
			return null, Err.from("invalid mail time", Err.INVALID_DATA)
	var day := int(fields[0])
	var year := int(fields[2])
	if fields[2].length() == 2:
		year += 2000 if year < 69 else 1900
	var hour := int(time[0])
	var minute := int(time[1])
	var second := int(time[2]) if time.size() == 3 else 0
	var offset := int(zone.substr(1, 2)) * 3600 + int(zone.substr(3, 2)) * 60 if numeric_zone else 0
	if day < 1 or day > GD.time.days_in_month(year, month) or hour < 0 or hour > 23 or minute < 0 or minute > 59 or second < 0 or second > 60 or (numeric_zone and (int(zone.substr(1, 2)) > 24 or int(zone.substr(3, 2)) > 59)):
		return null, Err.from("invalid mail date", Err.INVALID_DATA)
	if numeric_zone and zone[0] == "-":
		offset = -offset
	var timestamp := GD.time.from_parts({"year": year, "month": month, "day": day, "hour": hour, "minute": minute, "second": second}) - offset
	return {"unix": timestamp, "offset": offset, "zone": zone}


# Parse headers while preserving duplicate field values and untouched body bytes.
func read_message(data: PackedByteArray) -> Variant, Err:
	var end := -1
	var width := 0
	if data.size() >= 2 and data[0] == 13 and data[1] == 10:
		return GD.mail._wrap_message({}, data.slice(2))
	if not data.is_empty() and data[0] == 10:
		return GD.mail._wrap_message({}, data.slice(1))
	for i in range(data.size() - 1):
		if i + 3 < data.size() and data[i] == 13 and data[i + 1] == 10 and data[i + 2] == 13 and data[i + 3] == 10:
			end = i
			width = 4
			break
		if data[i] == 10 and data[i + 1] == 10:
			end = i
			width = 2
			break
	if end < 0:
		return null, Err.from("mail headers have no blank terminator", Err.INVALID_DATA)
	var head, head_error := GD.mail._utf8_text(data.slice(0, end))
	if head_error != null:
		return null, head_error.note("mail headers")
	head = head.replace("\r\n", "\n")
	var headers := {}
	var previous := ""
	for line in head.split("\n"):
		if line.contains("\r"):
			return null, Err.from("invalid mail header line", Err.INVALID_DATA)
		if line.begins_with(" ") or line.begins_with("\t"):
			if previous.is_empty():
				return null, Err.from("mail header continuation has no field", Err.INVALID_DATA)
			var values: Array = headers[previous]
			values[values.size() - 1] += " " + line.strip_edges()
			continue
		var colon := line.find(":")
		if colon <= 0:
			return null, Err.from("invalid mail header", Err.INVALID_DATA)
		previous = line.substr(0, colon).to_lower()
		for c in previous:
			if c.unicode_at(0) < 33 or c.unicode_at(0) > 126 or c == ":":
				return null, Err.from("invalid mail header name", Err.INVALID_DATA)
		if not headers.has(previous):
			headers[previous] = []
		headers[previous].append(line.substr(colon + 1).strip_edges())
	return GD.mail._wrap_message(headers, data.slice(end + width))


# Read only the header section and leave the body attached to its input stream.
func read_message_async(source) -> Variant, Err:
	if source is PackedByteArray:
		var parsed_v, parsed_e := read_message(source)
		if parsed_e != null:
			return parsed_v, parsed_e
		parsed_v.body = BodyReader.new(null, parsed_v.body)
		return parsed_v, parsed_e
	if source == null or not source.has_method("read_async"):
		return null, Err.from("mail source must support read_async", Err.INVALID_DATA)
	var bytes := PackedByteArray()
	var scan := 0
	while true:
		if (bytes.size() >= 2 and bytes[0] == 13 and bytes[1] == 10) or (not bytes.is_empty() and bytes[0] == 10):
			var empty_v, empty_e := read_message(bytes)
			empty_v.body = BodyReader.new(source, empty_v.body)
			return empty_v, empty_e
		for i in range(scan, bytes.size() - 1):
			if (i + 3 < bytes.size() and bytes[i] == 13 and bytes[i + 1] == 10 and bytes[i + 2] == 13 and bytes[i + 3] == 10) or (bytes[i] == 10 and bytes[i + 1] == 10):
				var parsed_v, parsed_e := read_message(bytes)
				if parsed_e != null:
					return parsed_v, parsed_e
				parsed_v.body = BodyReader.new(source, parsed_v.body)
				return parsed_v, parsed_e
		var received_v, received_e := await GD.async.spawn_pair(Callable(source, "read_async").bind(4096))
		if received_e != null:
			return null, received_e
		if received_v.is_empty():
			return null, Err.from("mail headers ended before blank line", Err.INVALID_DATA)
		scan = max(0, bytes.size() - 3)
		bytes.append_array(received_v)
	return null, Err.from("mail header unavailable", Err.INTERRUPTED)


# Check an RFC media-type token before accepting or emitting it.
func _mime_token(value: String) -> bool:
	if value.is_empty():
		return false
	for c in value:
		var n := c.unicode_at(0)
		if n <= 32 or n >= 127 or ("()<>@,;:" + String.chr(34) + String.chr(92) + "/[]?=").contains(c):
			return false
	return true


# Parse a MIME type and quoted parameters without splitting inside quotes.
func mime_parse_media_type(text: String) -> Variant, Err:
	var fields := []
	var quoted := false
	var escaped := false
	var start := 0
	for i in range(text.length()):
		var c := text[i]
		if escaped:
			escaped = false
		elif quoted and c == "\\":
			escaped = true
		elif c == "\"":
			quoted = not quoted
		elif c == ";" and not quoted:
			fields.append(text.substr(start, i - start).strip_edges())
			start = i + 1
	if quoted:
		return null, Err.from("unterminated MIME parameter", Err.INVALID_DATA)
	fields.append(text.substr(start).strip_edges())
	var media: String = fields[0].to_lower()
	var slash := media.find("/")
	if slash <= 0 or not _mime_token(media.substr(0, slash)) or not _mime_token(media.substr(slash + 1)):
		return null, Err.from("invalid MIME media type", Err.INVALID_DATA)
	var params := {}
	for i in range(1, fields.size()):
		var equal: int = fields[i].find("=")
		if equal <= 0:
			return null, Err.from("invalid MIME parameter", Err.INVALID_DATA)
		var key: String = fields[i].substr(0, equal).strip_edges().to_lower()
		var value: String = fields[i].substr(equal + 1).strip_edges()
		if value.begins_with("\""):
			if not value.ends_with("\"") or value.length() < 2:
				return null, Err.from("invalid quoted MIME parameter", Err.INVALID_DATA)
			value = value.substr(1, value.length() - 2).replace("\\\"", "\"").replace("\\\\", "\\")
		elif not _mime_token(value):
			return null, Err.from("invalid MIME parameter value", Err.INVALID_DATA)
		if not _mime_token(key) or params.has(key):
			return null, Err.from("duplicate or empty MIME parameter", Err.INVALID_DATA)
		params[key] = value
	return {"type": media, "params": params}


# Format a MIME type with quoted values when token spelling is insufficient.
func mime_format_media_type(media: String, params: Dictionary) -> Variant, Err:
	var checked_v, checked_e := mime_parse_media_type(media)
	if checked_e != null or not checked_v.params.is_empty():
		return null, Err.from("invalid MIME media type", Err.INVALID_DATA)
	var out := media.to_lower()
	var keys := params.keys()
	keys.sort()
	for raw_key in keys:
		var key := str(raw_key).to_lower()
		var value := str(params[raw_key])
		if not _mime_token(key) or value.contains("\r") or value.contains("\n"):
			return null, Err.from("invalid MIME parameter", Err.INVALID_DATA)
		out += "; " + key + "=\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
	return out


# Encode a UTF-8 header word with the requested RFC 2047 transfer form.
func mime_encode_word(text: String, mode: String = "b") -> Variant, Err:
	var form := mode.to_lower()
	if form not in ["b", "q"]:
		return null, Err.from("unknown encoded-word mode", Err.INVALID_DATA)
	var hex := "0123456789ABCDEF"
	var words: Array[String] = []
	var chunk := PackedByteArray()
	var encoded := ""
	for c in text:
		var bytes := c.to_utf8_buffer()
		var q := ""
		for byte in bytes:
			if byte == 32:
				q += "_"
			elif byte >= 33 and byte <= 126 and byte not in [61, 63, 95]:
				q += String.chr(byte)
			else:
				q += "=" + hex[byte >> 4] + hex[byte & 15]
		var too_long := (4 * ((chunk.size() + bytes.size() + 2) / 3) > 63) if form == "b" else encoded.length() + q.length() > 63
		if too_long and not chunk.is_empty():
			words.append("=?utf-8?B?" + GD.data.base64_encode(chunk) + "?=" if form == "b" else "=?utf-8?Q?" + encoded + "?=")
			chunk.clear()
			encoded = ""
		chunk.append_array(bytes)
		encoded += q
	if not chunk.is_empty():
		words.append("=?utf-8?B?" + GD.data.base64_encode(chunk) + "?=" if form == "b" else "=?utf-8?Q?" + encoded + "?=")
	return " ".join(words)


# Decode one RFC 2047 word in UTF-8, ASCII, or ISO-8859-1.
func mime_decode_word(text: String) -> Variant, Err:
	if text.length() > 75:
		return null, Err.from("encoded-word exceeds RFC length", Err.INVALID_DATA)
	if not text.begins_with("=?") or not text.ends_with("?="):
		return null, Err.from("invalid encoded-word boundary", Err.INVALID_DATA)
	var fields := text.substr(2, text.length() - 4).split("?", true, 2)
	if fields.size() != 3:
		return null, Err.from("invalid encoded-word fields", Err.INVALID_DATA)
	var charset := fields[0].to_lower()
	var bytes := PackedByteArray()
	if fields[1].to_lower() == "b":
		var decoded_v, decoded_e := GD.data.base64_decode(fields[2])
		if decoded_e != null:
			return "", decoded_e
		bytes = decoded_v
	elif fields[1].to_lower() == "q":
		var raw := fields[2].replace("_", " ").to_utf8_buffer()
		var decoded_v, decoded_e := mime_qp_decode(raw)
		if decoded_e != null:
			return "", decoded_e
		bytes = decoded_v
	else:
		return null, Err.from("invalid encoded-word mode", Err.INVALID_DATA)
	if charset == "utf-8" or charset == "us-ascii":
		var result := bytes.get_string_from_utf8()
		if result.to_utf8_buffer() != bytes:
			return null, Err.from("invalid encoded-word text", Err.INVALID_DATA)
		return result
	if charset == "iso-8859-1":
		var result := ""
		for byte in bytes:
			result += String.chr(byte)
		return result
	return null, Err.from("unsupported encoded-word charset", Err.UNSUPPORTED)


# Decode adjacent encoded words in an unstructured header value.
func mime_decode_header(text: String) -> Variant, Err:
	var out := ""
	var previous_encoded := false
	for piece in text.replace("\t", " ").split(" ", false):
		var encoded := piece.begins_with("=?")
		var value := piece
		if encoded:
			var decoded_v, decoded_e := mime_decode_word(piece)
			if decoded_e != null:
				return decoded_v, decoded_e
			value = decoded_v
		if not out.is_empty() and not (previous_encoded and encoded):
			out += " "
		out += value
		previous_encoded = encoded
	return out


# Encode bytes using printable ASCII and soft line breaks.
func mime_qp_encode(data: PackedByteArray) -> Variant, Err:
	var out := PackedByteArray()
	var line := 0
	var hex := "0123456789ABCDEF"
	for i in range(data.size()):
		var byte: int = data[i]
		if byte == 13 and i + 1 < data.size() and data[i + 1] == 10:
			out.append_array(PackedByteArray([13, 10]))
			line = 0
			continue
		if byte == 10 and i > 0 and data[i - 1] == 13:
			continue
		var escape := byte == 61 or byte < 32 or byte > 126 or ((byte == 32 or byte == 9) and (i + 1 == data.size() or data[i + 1] == 13))
		var token := ("=" + hex[byte >> 4] + hex[byte & 15]).to_utf8_buffer() if escape else PackedByteArray([byte])
		if line + token.size() > 75:
			out.append_array(PackedByteArray([61, 13, 10]))
			line = 0
		out.append_array(token)
		line += token.size()
	return out


# Decode hexadecimal escapes and soft breaks from quoted-printable bytes.
func mime_qp_decode(data: PackedByteArray) -> Variant, Err:
	var out := PackedByteArray()
	var _added: bool
	var i := 0
	var hex := "0123456789ABCDEF"
	while i < data.size():
		if data[i] != 61:
			_added = out.append(data[i])
			i += 1
			continue
		if i + 2 < data.size() and data[i + 1] == 13 and data[i + 2] == 10:
			i += 3
			continue
		if i + 2 >= data.size():
			return out, Err.from("truncated quoted-printable escape", Err.INVALID_DATA).with_partial(out)
		var high := hex.find(String.chr(data[i + 1]).to_upper())
		var low := hex.find(String.chr(data[i + 2]).to_upper())
		if high < 0 or low < 0:
			return out, Err.from("invalid quoted-printable escape", Err.INVALID_DATA).with_partial(out)
		_added = out.append(high * 16 + low)
		i += 3
	return out


# Decode quoted-printable bytes from a readable source on demand.
func mime_qp_reader(source) -> Variant, Err:
	if source == null or not source.has_method("read_async"):
		return null, Err.from("quoted-printable source must support read_async", Err.INVALID_DATA)
	return QuotedPrintableReader.new(source)


# Encode quoted-printable bytes directly into a destination sink.
func mime_qp_writer(sink) -> Variant, Err:
	if sink == null or not sink.has_method("write_async"):
		return null, Err.from("quoted-printable sink must support write_async", Err.INVALID_DATA)
	return QuotedPrintableWriter.new(sink)


# Create a multipart reader over an existing MIME body.
func mime_multipart_reader(data: PackedByteArray, boundary: String) -> Variant, Err:
	if boundary.is_empty() or boundary.contains("\r") or boundary.contains("\n") or boundary.length() > 70:
		return null, Err.from("invalid MIME boundary", Err.INVALID_DATA)
	return MultipartReader.new(data, boundary)


# Open a multipart reader that retains only the current boundary window.
func mime_multipart_reader_stream(source, boundary: String) -> Variant, Err:
	if source == null or not source.has_method("read_async"):
		return null, Err.from("multipart source must support read_async", Err.INVALID_DATA)
	if boundary.is_empty() or boundary.contains("\r") or boundary.contains("\n") or boundary.length() > 70:
		return null, Err.from("invalid MIME boundary", Err.INVALID_DATA)
	return MultipartStreamReader.new(source, boundary)


# Accept only boundary bytes that can be represented in a MIME parameter.
func _mime_valid_boundary(boundary: String) -> bool:
	if boundary.is_empty() or boundary.length() > 70 or boundary.ends_with(" "):
		return false
	for c in boundary:
		var n := c.unicode_at(0)
		if not ((n >= 48 and n <= 57) or (n >= 65 and n <= 90) or (n >= 97 and n <= 122) or ("'()+_,-./:=? ").contains(c)):
			return false
	return true


# Create a multipart writer with a random or caller-selected boundary.
func mime_multipart_writer(boundary: String = "") -> Variant, Err:
	if boundary.is_empty():
		boundary = GD.id.uuid().replace("-", "")
	if not _mime_valid_boundary(boundary):
		return null, Err.from("invalid MIME boundary", Err.INVALID_DATA)
	return MultipartWriter.new(boundary)


# Open a multipart writer that forwards each part to a byte sink.
func mime_multipart_writer_to(sink, boundary: String = "") -> Variant, Err:
	if sink == null or not sink.has_method("write_async"):
		return null, Err.from("multipart sink must support write_async", Err.INVALID_DATA)
	if boundary.is_empty():
		boundary = GD.id.uuid().replace("-", "")
	if not _mime_valid_boundary(boundary):
		return null, Err.from("invalid MIME boundary", Err.INVALID_DATA)
	return MultipartStreamWriter.new(sink, boundary)
