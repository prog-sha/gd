/**************************************************************************/
/*  mail.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Bind the standard mail API to its embedded, coroutine-aware implementation.

#include "cli/mail/mail.h"
#include "cli/data/utf8.h"
#include "cli/sys/task.h"
#include "core/object/class_db.h"
#include "core/crypto/crypto_core.h"
#include "modules/gdscript/gdscript.h"
#include "cli/mail/mail.gen.h"
#include <cstring>

// Call an embedded mail function and retain both language-level results.
template <typename... A>
static VariantPair mail_call(const Ref<RefCounted> &p_obj, const StringName &p_name, const A &...p_args) {
	if (!p_obj.is_valid()) return { Variant(), Err::make("mail implementation unavailable", Err::INVALID_DATA) };
	Variant values[] = { Variant(p_args)... };
	const Variant *args[sizeof...(A)];
	for (size_t i = 0; i < sizeof...(A); i++) args[i] = &values[i];
	Variant result;
	Variant error_value;
	Callable::CallError error;
	error.result_error = &error_value;
	Callable(p_obj.ptr(), p_name).callp(args, sizeof...(A), result, error);
	return error.error == Callable::CallError::CALL_OK && !error.runtime_failed ? VariantPair{ result, error_value } : VariantPair{ Variant(), Err::make("mail implementation call failed", Err::INVALID_DATA) };
}

// Publish parsed mail fields while retaining their source body representation.
void GDMailMessage::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_headers"), &GDMailMessage::get_headers);
	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &GDMailMessage::set_headers);
	ClassDB::bind_method(D_METHOD("get_body"), &GDMailMessage::get_body);
	ClassDB::bind_method(D_METHOD("set_body", "body"), &GDMailMessage::set_body);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "headers"), "set_headers", "get_headers");
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "body"), "set_body", "get_body");
}

// Forward one DATA operation while retaining its script transaction.
Signal GDSMTPDataWriter::write(const PackedByteArray &p_data) { return Async::spawn_pair(Callable(impl.ptr(), "write").bind(p_data)); }

// Request the final SMTP DATA response.
Signal GDSMTPDataWriter::close() { return Async::spawn_pair(Callable(impl.ptr(), "close")); }

// Publish the typed DATA writer and its wait results.
void GDSMTPDataWriter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("write", "data"), &GDSMTPDataWriter::write);
	ClassDB::bind_method(D_METHOD("write_async", "data"), &GDSMTPDataWriter::write_async);
	ClassDB::bind_method(D_METHOD("close"), &GDSMTPDataWriter::close);
	ClassDB::bind_method(D_METHOD("close_async"), &GDSMTPDataWriter::close_async);
	ADD_AWAIT("write", "Pair:int"); ADD_AWAIT("write_async", "Pair:int"); ADD_AUTO_WAIT("write");
	ADD_AWAIT("close", "Pair:Variant"); ADD_AWAIT("close_async", "Pair:Variant"); ADD_AUTO_WAIT("close");
}

// Schedule one SMTP session operation through its retained implementation.
Signal GDSMTPClient::run(const String &p_name, const Array &p_args) { return Async::spawn_pair(Callable(impl.ptr(), p_name).bindv(p_args)); }

// Introduce the session with EHLO or HELO.
Signal GDSMTPClient::hello(const String &p_name) { return run("hello", { p_name }); }

// Upgrade the existing SMTP connection to verified TLS.
Signal GDSMTPClient::start_tls(const Dictionary &p_opts) { return run("start_tls", { p_opts }); }

// Authenticate after transport policy is checked.
Signal GDSMTPClient::auth(const Dictionary &p_config) { return run("auth", { p_config }); }

// Start one SMTP envelope.
Signal GDSMTPClient::mail(const String &p_from) { return run("mail", { p_from }); }

// Add one recipient to the envelope.
Signal GDSMTPClient::rcpt(const String &p_to) { return run("rcpt", { p_to }); }

// Enter DATA mode and return a typed writer.
Signal GDSMTPClient::data() { return run("data"); }

// Reset the active envelope.
Signal GDSMTPClient::reset() { return run("reset"); }

// Check whether the SMTP peer responds.
Signal GDSMTPClient::noop() { return run("noop"); }

// Ask the peer about one address.
Signal GDSMTPClient::verify(const String &p_address) { return run("verify", { p_address }); }

// End the SMTP session and close its transport.
Signal GDSMTPClient::quit() { return run("quit"); }

// Release the SMTP transport immediately.
void GDSMTPClient::close() { impl->call("close"); }

// Read one advertised SMTP extension.
Dictionary GDSMTPClient::extension(const String &p_name) { return impl->call("extension", p_name); }

// Read the connection's TLS negotiation state.
Dictionary GDSMTPClient::tls_state() { return impl->call("tls_state"); }

// Publish session methods and typed asynchronous results.
void GDSMTPClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("hello", "name"), &GDSMTPClient::hello, DEFVAL("localhost"));
	ClassDB::bind_method(D_METHOD("hello_async", "name"), &GDSMTPClient::hello, DEFVAL("localhost"));
	ClassDB::bind_method(D_METHOD("start_tls", "opts"), &GDSMTPClient::start_tls, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("start_tls_async", "opts"), &GDSMTPClient::start_tls, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("auth", "config"), &GDSMTPClient::auth);
	ClassDB::bind_method(D_METHOD("auth_async", "config"), &GDSMTPClient::auth);
	ClassDB::bind_method(D_METHOD("mail", "from"), &GDSMTPClient::mail);
	ClassDB::bind_method(D_METHOD("mail_async", "from"), &GDSMTPClient::mail);
	ClassDB::bind_method(D_METHOD("rcpt", "to"), &GDSMTPClient::rcpt);
	ClassDB::bind_method(D_METHOD("rcpt_async", "to"), &GDSMTPClient::rcpt);
	ClassDB::bind_method(D_METHOD("data"), &GDSMTPClient::data);
	ClassDB::bind_method(D_METHOD("data_async"), &GDSMTPClient::data);
	ClassDB::bind_method(D_METHOD("reset"), &GDSMTPClient::reset);
	ClassDB::bind_method(D_METHOD("reset_async"), &GDSMTPClient::reset);
	ClassDB::bind_method(D_METHOD("noop"), &GDSMTPClient::noop);
	ClassDB::bind_method(D_METHOD("noop_async"), &GDSMTPClient::noop);
	ClassDB::bind_method(D_METHOD("verify", "address"), &GDSMTPClient::verify);
	ClassDB::bind_method(D_METHOD("verify_async", "address"), &GDSMTPClient::verify);
	ClassDB::bind_method(D_METHOD("quit"), &GDSMTPClient::quit);
	ClassDB::bind_method(D_METHOD("quit_async"), &GDSMTPClient::quit);
	ClassDB::bind_method(D_METHOD("close"), &GDSMTPClient::close);
	ClassDB::bind_method(D_METHOD("extension", "name"), &GDSMTPClient::extension);
	ClassDB::bind_method(D_METHOD("tls_state"), &GDSMTPClient::tls_state);
	ADD_AWAIT("hello", "Pair:Dictionary"); ADD_AWAIT("hello_async", "Pair:Dictionary"); ADD_AUTO_WAIT("hello");
	ADD_AWAIT("start_tls", "Pair:Dictionary"); ADD_AWAIT("start_tls_async", "Pair:Dictionary"); ADD_AUTO_WAIT("start_tls");
	ADD_AWAIT("auth", "Pair:Dictionary"); ADD_AWAIT("auth_async", "Pair:Dictionary"); ADD_AUTO_WAIT("auth");
	ADD_AWAIT("mail", "Pair:Dictionary"); ADD_AWAIT("mail_async", "Pair:Dictionary"); ADD_AUTO_WAIT("mail");
	ADD_AWAIT("rcpt", "Pair:Dictionary"); ADD_AWAIT("rcpt_async", "Pair:Dictionary"); ADD_AUTO_WAIT("rcpt");
	ADD_AWAIT("data", "Pair:GDSMTPDataWriter"); ADD_AWAIT("data_async", "Pair:GDSMTPDataWriter"); ADD_AUTO_WAIT("data");
	ADD_AWAIT("reset", "Pair:Dictionary"); ADD_AWAIT("reset_async", "Pair:Dictionary"); ADD_AUTO_WAIT("reset");
	ADD_AWAIT("noop", "Pair:Dictionary"); ADD_AWAIT("noop_async", "Pair:Dictionary"); ADD_AUTO_WAIT("noop");
	ADD_AWAIT("verify", "Pair:Dictionary"); ADD_AWAIT("verify_async", "Pair:Dictionary"); ADD_AUTO_WAIT("verify");
	ADD_AWAIT("quit", "Pair:Dictionary"); ADD_AWAIT("quit_async", "Pair:Dictionary"); ADD_AUTO_WAIT("quit");
}

// Allocate the MIME child under the same standard mail entry point.
GDMailAPI::GDMailAPI() { mime = memnew(GDMailMimeAPI); mime->set_owner(this); }

// Release the MIME child before the parent entry point.
GDMailAPI::~GDMailAPI() { memdelete(mime); }

// Load the mail implementation only when a mail operation is first used.
Ref<RefCounted> GDMailAPI::worker() {
	if (impl.is_valid()) return impl;
	const String source = String::utf8((const char *)gd_mail_script, sizeof(gd_mail_script));
	Ref<GDScript> script;
	script.instantiate();
	script->set_path("res://__gd_mail.gd");
	script->set_source_code(source);
	if (script->reload() != OK || !script->is_valid()) return Ref<RefCounted>();
	impl.instantiate();
	impl->set_script(script);
	return impl;
}

// Parse one display name and mailbox.
VariantPair GDMailAPI::parse_address(const String &p_text) {
	const Ref<RefCounted> obj = worker();
	return mail_call(obj, SNAME("parse_address"), p_text);
}

// Parse comma-separated mailboxes without discarding display names.
VariantPair GDMailAPI::parse_address_list(const String &p_text) {
	const Ref<RefCounted> obj = worker();
	return mail_call(obj, SNAME("parse_address_list"), p_text);
}

// Parse a mail date while retaining its numeric timezone offset.
VariantPair GDMailAPI::parse_date(const String &p_text) {
	const Ref<RefCounted> obj = worker();
	return mail_call(obj, SNAME("parse_date"), p_text);
}

// Split a mail message into repeated headers and its untouched body bytes.
VariantPair GDMailAPI::read_message(const PackedByteArray &p_data) {
	const Ref<RefCounted> obj = worker();
	return mail_call(obj, SNAME("read_message"), p_data);
}

// Decode protocol text without silently replacing malformed wire bytes.
VariantPair GDMailAPI::utf8_text(const PackedByteArray &p_data) {
	return ::utf8_text(p_data.ptr(), p_data.size());
}

// Parse headers from bytes or a readable stream without collecting the whole body.
Signal GDMailAPI::read_message_async(const Variant &p_source) {
	const Ref<RefCounted> obj = worker();
	return obj.is_valid() ? Async::spawn_pair(Callable(obj.ptr(), "read_message_async").bind(p_source)) : Async::ready_pair({ Variant(), Err::make("mail implementation unavailable", Err::INVALID_DATA) });
}

// Package PLAIN credentials for a verified SMTP session.
Dictionary GDMailAPI::plain_auth(const String &p_user, const String &p_password, const String &p_identity) {
	Dictionary out;
	out["kind"] = "plain";
	out["user"] = p_user;
	out["secret"] = p_password;
	out["identity"] = p_identity;
	return out;
}

// Package CRAM-MD5 credentials for a verified SMTP session.
Dictionary GDMailAPI::cram_md5_auth(const String &p_user, const String &p_secret) {
	Dictionary out;
	out["kind"] = "cram_md5";
	out["user"] = p_user;
	out["secret"] = p_secret;
	return out;
}

// Compute the HMAC-MD5 response required by the CRAM-MD5 SMTP mechanism.
VariantPair GDMailAPI::cram_md5(const PackedByteArray &p_key, const PackedByteArray &p_challenge) {
	uint8_t key[64] = {}; // MD5's protocol-defined HMAC block width.
	if (p_key.size() > 64) {
		if (CryptoCore::md5(p_key.ptr(), p_key.size(), key) != OK) return { Variant(), Err::make("cannot hash SMTP key", Err::INVALID_DATA) };
	} else if (!p_key.is_empty()) {
		memcpy(key, p_key.ptr(), p_key.size());
	}
	uint8_t inner_pad[64], outer_pad[64], inner[16], digest[16]; // HMAC pads and intermediate digests.
	for (int i = 0; i < 64; i++) { inner_pad[i] = key[i] ^ 0x36; outer_pad[i] = key[i] ^ 0x5c; }
	CryptoCore::MD5Context ctx;
	if (ctx.start() != OK || ctx.update(inner_pad, 64) != OK || ctx.update(p_challenge.ptr(), p_challenge.size()) != OK || ctx.finish(inner) != OK ||
			ctx.start() != OK || ctx.update(outer_pad, 64) != OK || ctx.update(inner, 16) != OK || ctx.finish(digest) != OK) {
		return { Variant(), Err::make("cannot compute SMTP challenge response", Err::INVALID_DATA) };
	}
	static const char hex[] = "0123456789abcdef"; // Wire spelling of the hexadecimal HMAC result.
	String out;
	for (uint8_t byte : digest) { out += String::chr(hex[byte >> 4]); out += String::chr(hex[byte & 15]); }
	return { out, Variant() };
}

// Give a completed SMTP dial a stable public session type.
Ref<GDSMTPClient> GDMailAPI::wrap_client(const Ref<RefCounted> &p_impl) {
	Ref<GDSMTPClient> client;
	client.instantiate();
	client->set_impl(p_impl);
	return client;
}

// Give a DATA transaction a stable public writer type.
Ref<GDSMTPDataWriter> GDMailAPI::wrap_writer(const Ref<RefCounted> &p_impl) {
	Ref<GDSMTPDataWriter> writer;
	writer.instantiate();
	writer->set_impl(p_impl);
	return writer;
}

// Store one parsed header map and its unread body in the public message type.
Ref<GDMailMessage> GDMailAPI::wrap_message(const Dictionary &p_headers, const Variant &p_body) {
	Ref<GDMailMessage> message;
	message.instantiate();
	message->set_headers(p_headers);
	message->set_body(p_body);
	return message;
}

// Open a reusable SMTP session without blocking the caller.
Signal GDMailAPI::dial_smtp(const String &p_host, int64_t p_port, const Dictionary &p_opts) {
	const Ref<RefCounted> obj = worker();
	return obj.is_valid() ? Async::spawn_pair(Callable(obj.ptr(), "dial_smtp").bind(p_host, p_port, p_opts)) : Async::ready_pair({ Variant(), Err::make("mail implementation unavailable", Err::INVALID_DATA) });
}

// Send one complete message through a reusable SMTP session.
Signal GDMailAPI::send_mail(const String &p_host, int64_t p_port, const String &p_from, const PackedStringArray &p_to, const PackedByteArray &p_message, const Dictionary &p_opts) {
	const Ref<RefCounted> obj = worker();
	return obj.is_valid() ? Async::spawn_pair(Callable(obj.ptr(), "send_mail").bind(p_host, p_port, p_from, p_to, p_message, p_opts)) : Async::ready_pair({ Variant(), Err::make("mail implementation unavailable", Err::INVALID_DATA) });
}

// Publish mail helpers with the same result and wait metadata as other standard APIs.
void GDMailAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_mime"), &GDMailAPI::get_mime);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mime", PROPERTY_HINT_NONE, "GDMailMimeAPI"), "", "get_mime");
	ClassDB::bind_method(D_METHOD("parse_address", "text"), &GDMailAPI::parse_address);
	ClassDB::bind_method(D_METHOD("parse_address_list", "text"), &GDMailAPI::parse_address_list);
	ClassDB::bind_method(D_METHOD("parse_date", "text"), &GDMailAPI::parse_date);
	ClassDB::bind_method(D_METHOD("read_message", "data"), &GDMailAPI::read_message);
	ClassDB::bind_method(D_METHOD("_utf8_text", "data"), &GDMailAPI::utf8_text);
	ClassDB::bind_method(D_METHOD("read_message_async", "source"), &GDMailAPI::read_message_async);
	ClassDB::bind_method(D_METHOD("plain_auth", "user", "password", "identity"), &GDMailAPI::plain_auth, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("cram_md5_auth", "user", "secret"), &GDMailAPI::cram_md5_auth);
	ClassDB::bind_method(D_METHOD("cram_md5", "key", "challenge"), &GDMailAPI::cram_md5);
	ClassDB::bind_method(D_METHOD("_wrap_client", "impl"), &GDMailAPI::wrap_client);
	ClassDB::bind_method(D_METHOD("_wrap_writer", "impl"), &GDMailAPI::wrap_writer);
	ClassDB::bind_method(D_METHOD("_wrap_message", "headers", "body"), &GDMailAPI::wrap_message);
	ClassDB::bind_method(D_METHOD("dial_smtp", "host", "port", "opts"), &GDMailAPI::dial_smtp, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("dial_smtp_async", "host", "port", "opts"), &GDMailAPI::dial_smtp_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("send_mail", "host", "port", "from", "to", "message", "opts"), &GDMailAPI::send_mail, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("send_mail_async", "host", "port", "from", "to", "message", "opts"), &GDMailAPI::send_mail_async, DEFVAL(Dictionary()));
	ADD_PAIR_RESULT("parse_address", "Dictionary");
	ADD_PAIR_RESULT("parse_address_list", "Array");
	ADD_PAIR_RESULT("parse_date", "Dictionary");
	ADD_PAIR_RESULT("read_message", "GDMailMessage");
	ADD_PAIR_RESULT("_utf8_text", "String");
	ADD_AWAIT("read_message_async", "Pair:GDMailMessage");
	ADD_PAIR_RESULT("cram_md5", "String");
	ADD_AWAIT("dial_smtp", "Pair:GDSMTPClient");
	ADD_AWAIT("dial_smtp_async", "Pair:GDSMTPClient");
	ADD_AWAIT("send_mail", "Pair:Variant");
	ADD_AWAIT("send_mail_async", "Pair:Variant");
	ADD_AUTO_WAIT("dial_smtp");
	ADD_AUTO_WAIT("send_mail");
}

// Parse a MIME media type and its parameters.
VariantPair GDMailMimeAPI::parse_media_type(const String &p_text) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_parse_media_type"), p_text);
}

// Format a MIME media type after checking parameter syntax.
VariantPair GDMailMimeAPI::format_media_type(const String &p_type, const Dictionary &p_params) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_format_media_type"), p_type, p_params);
}

// Encode one unstructured header word.
VariantPair GDMailMimeAPI::encode_word(const String &p_text, const String &p_mode) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_encode_word"), p_text, p_mode);
}

// Decode one encoded header word.
VariantPair GDMailMimeAPI::decode_word(const String &p_text) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_decode_word"), p_text);
}

// Decode an unstructured header containing adjacent encoded words.
VariantPair GDMailMimeAPI::decode_header(const String &p_text) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_decode_header"), p_text);
}

// Encode body bytes with quoted-printable transfer syntax.
VariantPair GDMailMimeAPI::quoted_printable_encode(const PackedByteArray &p_data) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_qp_encode"), p_data);
}

// Decode quoted-printable body bytes.
VariantPair GDMailMimeAPI::quoted_printable_decode(const PackedByteArray &p_data) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_qp_decode"), p_data);
}

// Read quoted-printable escapes from a stream only as the caller requests bytes.
VariantPair GDMailMimeAPI::quoted_printable_reader(const Variant &p_source) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_qp_reader"), p_source);
}

// Write quoted-printable bytes through an asynchronous sink.
VariantPair GDMailMimeAPI::quoted_printable_writer(const Variant &p_sink) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_qp_writer"), p_sink);
}

// Traverse MIME parts from a boundary-delimited byte sequence.
VariantPair GDMailMimeAPI::multipart_reader(const PackedByteArray &p_data, const String &p_boundary) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_multipart_reader"), p_data, p_boundary);
}

// Build a multipart body with an optional caller-selected boundary.
VariantPair GDMailMimeAPI::multipart_writer(const String &p_boundary) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_multipart_writer"), p_boundary);
}

// Traverse MIME parts without holding the complete source body.
VariantPair GDMailMimeAPI::multipart_reader_stream(const Variant &p_source, const String &p_boundary) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_multipart_reader_stream"), p_source, p_boundary);
}

// Write MIME parts directly to an asynchronous byte sink.
VariantPair GDMailMimeAPI::multipart_writer_to(const Variant &p_sink, const String &p_boundary) {
	const Ref<RefCounted> obj = owner->worker();
	return mail_call(obj, SNAME("mime_multipart_writer_to"), p_sink, p_boundary);
}

// Publish MIME helpers as a discoverable child API.
void GDMailMimeAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("parse_media_type", "text"), &GDMailMimeAPI::parse_media_type);
	ClassDB::bind_method(D_METHOD("format_media_type", "type", "params"), &GDMailMimeAPI::format_media_type);
	ClassDB::bind_method(D_METHOD("encode_word", "text", "mode"), &GDMailMimeAPI::encode_word, DEFVAL("b"));
	ClassDB::bind_method(D_METHOD("decode_word", "text"), &GDMailMimeAPI::decode_word);
	ClassDB::bind_method(D_METHOD("decode_header", "text"), &GDMailMimeAPI::decode_header);
	ClassDB::bind_method(D_METHOD("quoted_printable_encode", "data"), &GDMailMimeAPI::quoted_printable_encode);
	ClassDB::bind_method(D_METHOD("quoted_printable_decode", "data"), &GDMailMimeAPI::quoted_printable_decode);
	ClassDB::bind_method(D_METHOD("quoted_printable_reader", "source"), &GDMailMimeAPI::quoted_printable_reader);
	ClassDB::bind_method(D_METHOD("quoted_printable_writer", "sink"), &GDMailMimeAPI::quoted_printable_writer);
	ClassDB::bind_method(D_METHOD("multipart_reader", "data", "boundary"), &GDMailMimeAPI::multipart_reader);
	ClassDB::bind_method(D_METHOD("multipart_writer", "boundary"), &GDMailMimeAPI::multipart_writer, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("multipart_reader_stream", "source", "boundary"), &GDMailMimeAPI::multipart_reader_stream);
	ClassDB::bind_method(D_METHOD("multipart_writer_to", "sink", "boundary"), &GDMailMimeAPI::multipart_writer_to, DEFVAL(""));
	ADD_PAIR_RESULT("parse_media_type", "Dictionary");
	ADD_PAIR_RESULT("format_media_type", "String");
	ADD_PAIR_RESULT("encode_word", "String");
	ADD_PAIR_RESULT("decode_word", "String");
	ADD_PAIR_RESULT("decode_header", "String");
	ADD_PAIR_RESULT("quoted_printable_encode", "PackedByteArray");
	ADD_PAIR_RESULT("quoted_printable_decode", "PackedByteArray");
	ADD_PAIR_RESULT("quoted_printable_reader", "Variant");
	ADD_PAIR_RESULT("quoted_printable_writer", "Variant");
	ADD_PAIR_RESULT("multipart_reader", "Variant");
	ADD_PAIR_RESULT("multipart_writer", "Variant");
	ADD_PAIR_RESULT("multipart_reader_stream", "Variant");
	ADD_PAIR_RESULT("multipart_writer_to", "Variant");
}
