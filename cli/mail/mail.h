/**************************************************************************/
/*  mail.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Expose mail parsing and SMTP transport through the standard API.

#include "cli/sys/std.h"
#include "core/object/script_language.h"

class GDMailAPI;

// Retain parsed headers and a byte or streaming body behind one typed result.
class GDMailMessage : public RefCounted {
	GDCLASS(GDMailMessage, RefCounted);
	Dictionary headers; // Lowercase field names mapped to every value in wire order.
	Variant body; // Original bytes or a reader over the remaining source.
protected:
	static void _bind_methods();
public:
	// Store every parsed header value in wire order.
	void set_headers(const Dictionary &p_headers) { headers = p_headers; }
	// Return the parsed header values.
	Dictionary get_headers() const { return headers; }
	// Store the original body bytes or a streaming reader.
	void set_body(const Variant &p_body) { body = p_body; }
	// Return the body representation selected during parsing.
	Variant get_body() const { return body; }
};

// Expose a DATA transaction as a typed asynchronous byte writer.
class GDSMTPDataWriter : public RefCounted {
	GDCLASS(GDSMTPDataWriter, RefCounted);
	Ref<RefCounted> impl; // Script transaction retained until its final response.
protected:
	static void _bind_methods();
public:
	void set_impl(const Ref<RefCounted> &p_impl) { impl = p_impl; }
	Signal write(const PackedByteArray &p_data);
	Signal write_async(const PackedByteArray &p_data) { return write(p_data); }
	Signal close();
	Signal close_async() { return close(); }
};

// Expose SMTP session operations with a stable native type.
class GDSMTPClient : public RefCounted {
	GDCLASS(GDSMTPClient, RefCounted);
	Ref<RefCounted> impl; // Session retained across pending operations.
	Signal run(const String &p_name, const Array &p_args = Array());
protected:
	static void _bind_methods();
public:
	void set_impl(const Ref<RefCounted> &p_impl) { impl = p_impl; }
	Signal hello(const String &p_name = "localhost");
	Signal start_tls(const Dictionary &p_opts = Dictionary());
	Signal auth(const Dictionary &p_config);
	Signal mail(const String &p_from);
	Signal rcpt(const String &p_to);
	Signal data();
	Signal reset();
	Signal noop();
	Signal verify(const String &p_address);
	Signal quit();
	void close();
	Dictionary extension(const String &p_name);
	Dictionary tls_state();
};

// Expose MIME formatting and multipart traversal under the mail API.
class GDMailMimeAPI : public Object {
	GDCLASS(GDMailMimeAPI, Object);
	GDMailAPI *owner = nullptr; // Parent retaining the embedded implementation.

protected:
	static void _bind_methods();

public:
	void set_owner(GDMailAPI *p_owner) { owner = p_owner; }
	VariantPair parse_media_type(const String &p_text);
	VariantPair format_media_type(const String &p_type, const Dictionary &p_params);
	VariantPair encode_word(const String &p_text, const String &p_mode = "b");
	VariantPair decode_word(const String &p_text);
	VariantPair decode_header(const String &p_text);
	VariantPair quoted_printable_encode(const PackedByteArray &p_data);
	VariantPair quoted_printable_decode(const PackedByteArray &p_data);
	VariantPair quoted_printable_reader(const Variant &p_source);
	VariantPair quoted_printable_writer(const Variant &p_sink);
	VariantPair multipart_reader(const PackedByteArray &p_data, const String &p_boundary);
	VariantPair multipart_writer(const String &p_boundary = "");
	VariantPair multipart_reader_stream(const Variant &p_source, const String &p_boundary);
	VariantPair multipart_writer_to(const Variant &p_sink, const String &p_boundary = "");
};

class GDMailAPI : public Object {
	GDCLASS(GDMailAPI, Object);

	Ref<RefCounted> impl; // Embedded mail implementation retained across calls.
	GDMailMimeAPI *mime = nullptr; // MIME child API owned with this entry point.
	Ref<RefCounted> worker(); // Load the standard implementation after the script runtime starts.
	friend class GDMailMimeAPI;

protected:
	static void _bind_methods();

public:
	GDMailAPI();
	~GDMailAPI();
	GDMailMimeAPI *get_mime() const { return mime; }
	// Release embedded script state while the script language is still running.
	void shutdown() { impl.unref(); }
	VariantPair parse_address(const String &p_text);
	VariantPair parse_address_list(const String &p_text);
	VariantPair parse_date(const String &p_text);
	VariantPair read_message(const PackedByteArray &p_data);
	VariantPair utf8_text(const PackedByteArray &p_data);
	Signal read_message_async(const Variant &p_source);
	Dictionary plain_auth(const String &p_user, const String &p_password, const String &p_identity = "");
	Dictionary cram_md5_auth(const String &p_user, const String &p_secret);
	VariantPair cram_md5(const PackedByteArray &p_key, const PackedByteArray &p_challenge);
	Ref<GDSMTPClient> wrap_client(const Ref<RefCounted> &p_impl);
	Ref<GDSMTPDataWriter> wrap_writer(const Ref<RefCounted> &p_impl);
	Ref<GDMailMessage> wrap_message(const Dictionary &p_headers, const Variant &p_body);
	Signal dial_smtp(const String &p_host, int64_t p_port, const Dictionary &p_opts = Dictionary());
	Signal dial_smtp_async(const String &p_host, int64_t p_port, const Dictionary &p_opts = Dictionary()) { return dial_smtp(p_host, p_port, p_opts); }
	Signal send_mail(const String &p_host, int64_t p_port, const String &p_from, const PackedStringArray &p_to, const PackedByteArray &p_message, const Dictionary &p_opts = Dictionary());
	Signal send_mail_async(const String &p_host, int64_t p_port, const String &p_from, const PackedStringArray &p_to, const PackedByteArray &p_message, const Dictionary &p_opts = Dictionary()) { return send_mail(p_host, p_port, p_from, p_to, p_message, p_opts); }
};
