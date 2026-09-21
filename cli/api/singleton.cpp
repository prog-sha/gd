/**************************************************************************/
/*  singleton.cpp                                                        */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Bind shared APIs and state, and register singleton instances declared in singleton.h.

#include "cli/api/singleton.h"
#include "cli/sys/system.h"
#include "cli/net/lookup.h"
#include "cli/net/body_source.h"

#include "cli/api/coll_call.h"
#include "cli/sys/file_job.h"
#include "cli/sys/mount.h"
#include "cli/sys/pkgsource.h"
#include "cli/sys/proc.h"
#include "cli/sys/clock.h"
#include "cli/sys/sched.h"

#include "cli/net/mw.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/version.h"
#include "modules/gdscript/gdscript_function.h"

namespace {

// Check whether a method appears in a fixed purpose-specific list.
bool method_in(const StringName &p_name, const char *const *p_names) {
	for (int i = 0; p_names[i]; i++) {
		if (p_name == p_names[i]) {
			return true;
		}
	}
	return false;
}

// Classify file API methods by purpose.
StringName file_group(const StringName &p_name) {
	static const char *const paths[] = { "parse_path", "format_path", "join", "dirname", "basename", "extname", "is_absolute", "normalize", "relative", "under", nullptr }; // Path operations.
	static const char *const formats[] = { "create_tar", "extract_tar", nullptr }; // Archive format helpers.
	static const char *const io[] = { "open", "read_text", "read_bytes", "write_text", "replace_text", "write_bytes", "append_bytes", "append_text", "exists", "remove", "size_of", "copy", "rename", "list_dir", "make_dir", "ensure_dir", "walk", "glob", "remove_all", "can_read", nullptr }; // File I/O.
	const String raw = p_name;
	const StringName name(raw.trim_suffix("_async"));
	if (method_in(name, paths)) {
		return "Paths";
	}
	if ((String(name).begins_with("read_") && !method_in(name, io)) || method_in(name, formats)) {
		return "File formats";
	}
	return method_in(name, io) ? StringName("File I/O") : StringName("Memory formats");
}

// Classify data API methods by purpose.
StringName data_group(const StringName &p_name) {
	static const char *const bytes[] = { "concat", "equals", "includes", "index_of", "last_index_of", "starts_with", "ends_with", "repeat", "fit", "split", "xor_bytes", nullptr }; // Byte-sequence operations.
	static const char *const serial[] = { "json_encode", "json_decode", "msgpack", "unmsgpack", "cbor", "uncbor", "csv", "to_csv", "csv_objects", "to_csv_objects", "ini", "to_ini", "toml", "to_toml", "yaml", "to_yaml", "jsonc", "strip_jsonc", "jsonl", "to_jsonl", "jsonl_reader", "front_matter", "has_front_matter", "to_front_matter", "xml", "to_xml", "env", "to_env", "tar", "untar", nullptr }; // Serialization.
	const String raw = p_name;
	const String plain = raw.trim_suffix("_async");
	const StringName name(plain);
	if (plain.begins_with("hex_") || plain.begins_with("base") || plain.begins_with("varint_")) {
		return "Codec";
	}
	if (method_in(name, bytes)) {
		return "Bytes";
	}
	return method_in(name, serial) ? StringName("Serialization") : StringName("Hash");
}

// Classify web API methods by purpose.
StringName web_group(const StringName &p_name) {
	static const char *const app[] = { "app", "server", nullptr }; // Application and server construction.
	static const char *const mids[] = { "jwt", "jwt_sign", "jwt_verify", "csrf", "sessions", "rate", nullptr }; // Authentication and traffic middleware.
	static const char *const rules[] = { "text_rule", "int_rule", "number_rule", "bool_rule", "list_rule", "object_rule", "optional", "one_of", "validate", "json_body", "query", "params", nullptr }; // Input validation.
	const String raw = p_name;
	const StringName name(raw.trim_suffix("_async"));
	if (method_in(name, app)) {
		return "Application";
	}
	if (method_in(name, mids)) {
		return "Middleware";
	}
	return method_in(name, rules) ? StringName("Validation") : StringName("Replies");
}

} // namespace

// Expose asynchronous coordination APIs to script.
void GDAsyncAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_max_threads", "max"), &GDAsyncAPI::set_max_threads);
	ClassDB::bind_method(D_METHOD("sleep", "sec"), &GDAsyncAPI::sleep);
	ClassDB::bind_method(D_METHOD("spawn", "fn"), &GDAsyncAPI::spawn);
	ClassDB::bind_method(D_METHOD("all", "signals"), &GDAsyncAPI::all);
	ClassDB::bind_method(D_METHOD("race", "signals"), &GDAsyncAPI::race);
	ClassDB::bind_method(D_METHOD("with_timeout", "signal", "sec"), &GDAsyncAPI::with_timeout);
	ClassDB::bind_method(D_METHOD("with_context", "context", "signal"), &GDAsyncAPI::with_context);
	ClassDB::bind_method(D_METHOD("context"), &GDAsyncAPI::context);
	ADD_AWAIT("all", "Array");
	ADD_AWAIT("race", "int");
	ADD_AWAIT("with_timeout", "int");
	ADD_AWAIT("with_context", "Variant");
}

// Set the shared logger's name and minimum level.
void GDLogAPI::setup(const String &p_name, const String &p_level) {
	log.setup(p_name, LogState::level_of(p_level));
}

// Parse a level name and format one log line.
String GDLogAPI::format(const String &p_level, const String &p_msg, const Variant &p_extra) const {
	return log.format(LogState::level_of(p_level), p_msg, p_extra);
}

// Expose logging APIs to script.
void GDLogAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "name", "level"), &GDLogAPI::setup, DEFVAL(""), DEFVAL("info"));
	ClassDB::bind_method(D_METHOD("set_file", "path"), &GDLogAPI::set_file);
	ClassDB::bind_method(D_METHOD("set_time", "on"), &GDLogAPI::set_time);
	ClassDB::bind_method(D_METHOD("set_color", "on"), &GDLogAPI::set_color);
	ClassDB::bind_method(D_METHOD("write", "level", "msg", "extra"), &GDLogAPI::write, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("debug", "msg", "extra"), &GDLogAPI::debug, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("info", "msg", "extra"), &GDLogAPI::info, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("warn", "msg", "extra"), &GDLogAPI::warn, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("error", "msg", "extra"), &GDLogAPI::error, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("result", "r", "msg"), &GDLogAPI::result);
	ClassDB::bind_method(D_METHOD("write_async", "level", "msg", "extra"), &GDLogAPI::write, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("debug_async", "msg", "extra"), &GDLogAPI::debug, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("info_async", "msg", "extra"), &GDLogAPI::info, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("warn_async", "msg", "extra"), &GDLogAPI::warn, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("error_async", "msg", "extra"), &GDLogAPI::error, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("result_async", "r", "msg"), &GDLogAPI::result);
	ADD_AWAIT("write_async", "R:Variant");
	ADD_AWAIT("debug_async", "R:Variant");
	ADD_AWAIT("info_async", "R:Variant");
	ADD_AWAIT("warn_async", "R:Variant");
	ADD_AWAIT("error_async", "R:Variant");
	ADD_AWAIT("result_async", "R:Variant");
	ClassDB::bind_method(D_METHOD("format", "level", "msg", "extra"), &GDLogAPI::format, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("flush"), &GDLogAPI::flush);
	ClassDB::bind_method(D_METHOD("flush_async"), &GDLogAPI::flush_async);
	ADD_AWAIT("flush", "R:Variant");
	ADD_AWAIT("flush_async", "R:Variant");
	ADD_AUTO_WAIT("flush");
	ADD_AWAIT("write", "R:Variant");
	ADD_AUTO_WAIT("write");
	ADD_AWAIT("debug", "R:Variant");
	ADD_AUTO_WAIT("debug");
	ADD_AWAIT("info", "R:Variant");
	ADD_AUTO_WAIT("info");
	ADD_AWAIT("warn", "R:Variant");
	ADD_AUTO_WAIT("warn");
	ADD_AWAIT("error", "R:Variant");
	ADD_AUTO_WAIT("error");
	ADD_AWAIT("result", "R:Variant");
	ADD_AUTO_WAIT("result");
}

// Insert an ordered I/O barrier and report completion of preceding log writes.
Signal GDLogAPI::flush() const {
	return LogState::flush();
}

// Expose network helpers to script.
void GDNetAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_free", "port", "host"), &GDNetAPI::is_free, DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("free_port", "from", "host"), &GDNetAPI::free_port, DEFVAL(0), DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("resolve", "host"), &GDNetAPI::resolve);
	ClassDB::bind_method(D_METHOD("resolve_async", "host"), &GDNetAPI::resolve_async);
	ClassDB::bind_method(D_METHOD("dial_tcp", "host", "port", "opts"), &GDNetAPI::dial_tcp, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("dial_tcp_async", "host", "port", "opts"), &GDNetAPI::dial_tcp_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("dial_tls", "host", "port", "opts"), &GDNetAPI::dial_tls, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("dial_tls_async", "host", "port", "opts"), &GDNetAPI::dial_tls_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("listen_tcp", "host", "port"), &GDNetAPI::listen_tcp, DEFVAL("127.0.0.1"), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("listen_udp", "host", "port", "buffer"), &GDNetAPI::listen_udp, DEFVAL("127.0.0.1"), DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("is_free_async", "port", "host"), &GDNetAPI::is_free_async, DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("free_port_async", "from", "host"), &GDNetAPI::free_port_async, DEFVAL(0), DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("local_addresses"), &GDNetAPI::local_addresses);
	ClassDB::bind_method(D_METHOD("local_addresses_async"), &GDNetAPI::local_addresses_async);
	ClassDB::bind_method(D_METHOD("is_ip", "text"), &GDNetAPI::is_ip);
	ClassDB::bind_method(D_METHOD("split_host", "text", "default_port"), &GDNetAPI::split_host, DEFVAL(80));
	ADD_RESULT("free_port", "int");
	ADD_AWAIT("resolve", "R:String");
	ADD_AWAIT("resolve_async", "R:String");
	ADD_AWAIT("dial_tcp", "R:GDTCPConn");
	ADD_AWAIT("dial_tcp_async", "R:GDTCPConn");
	ADD_AWAIT("dial_tls", "R:GDTCPConn");
	ADD_AWAIT("dial_tls_async", "R:GDTCPConn");
	ADD_AUTO_WAIT("resolve");
	ADD_AUTO_WAIT("dial_tcp");
	ADD_AUTO_WAIT("dial_tls");
	ADD_RESULT("listen_tcp", "GDTCPListener");
	ADD_RESULT("listen_udp", "GDUDPPacketConn");
	ADD_AWAIT("is_free_async", "bool");
	ADD_AWAIT("free_port_async", "R:int");
	ADD_AWAIT("local_addresses_async", "R:PackedStringArray");
	ADD_AWAIT("local_addresses", "R:PackedStringArray");
	ADD_AUTO_WAIT("local_addresses");
}

// Dispatch name resolution to an I/O worker.
Signal GDNetAPI::resolve(const String &p_host) {
	return GDLookupCall::start(p_host, true);
}

// Dispatch port availability checks to an I/O worker.
Signal GDNetAPI::is_free_async(int64_t p_port, const String &p_host) {
	return GDValueCall::start([p_port, p_host]() -> Variant { return Net::is_free(p_port, p_host); }, false);
}

// Dispatch free-port discovery to an I/O worker.
Signal GDNetAPI::free_port_async(int64_t p_from, const String &p_host) {
	return GDFileCall::start([p_from, p_host]() { return Net::free_port(p_from, p_host); });
}

// Dispatch local-address enumeration to an I/O worker.
Signal GDNetAPI::local_addresses_async() {
	return GDFileCall::start([]() { return Net::local_addresses(); });
}

// Expose the HTTP client and URL helpers to script.
void GDHTTPAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("fetch", "url", "opts"), &GDHTTPAPI::fetch, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("fetch_async", "url", "opts"), &GDHTTPAPI::fetch_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("parse_url", "raw"), &GDHTTPAPI::parse_url);
	ClassDB::bind_method(D_METHOD("parse_url_async", "raw"), &GDHTTPAPI::parse_url_async);
	ClassDB::bind_method(D_METHOD("build_url", "url"), &GDHTTPAPI::build_url);
	ClassDB::bind_method(D_METHOD("build_url_async", "url"), &GDHTTPAPI::build_url_async);
	ClassDB::bind_method(D_METHOD("request_target", "url"), &GDHTTPAPI::request_target);
	ClassDB::bind_method(D_METHOD("request_target_async", "url"), &GDHTTPAPI::request_target_async);
	ClassDB::bind_method(D_METHOD("default_port", "scheme"), &GDHTTPAPI::default_port);
	ClassDB::bind_method(D_METHOD("decode_query", "raw"), &GDHTTPAPI::decode_query);
	ClassDB::bind_method(D_METHOD("decode_query_async", "raw"), &GDHTTPAPI::decode_query_async);
	ClassDB::bind_method(D_METHOD("encode_query", "query"), &GDHTTPAPI::encode_query);
	ClassDB::bind_method(D_METHOD("encode_query_async", "query"), &GDHTTPAPI::encode_query_async);
	ClassDB::bind_method(D_METHOD("media_type", "path"), &GDHTTPAPI::media_type);
	ClassDB::bind_method(D_METHOD("media_type_for_extension", "ext"), &GDHTTPAPI::media_type_for_extension);
	ClassDB::bind_method(D_METHOD("is_textual", "kind"), &GDHTTPAPI::is_textual);
	ClassDB::bind_method(D_METHOD("extension_for_media_type", "kind"), &GDHTTPAPI::extension_for_media_type);
	ADD_AWAIT("fetch", "R:GDHTTPResponse");
	ADD_AWAIT("fetch_async", "R:GDHTTPResponse");
	ADD_AUTO_WAIT("fetch");
	ADD_RESULT("parse_url", "Dictionary");
	ADD_RESULT("decode_query", "Dictionary");
	ADD_AWAIT("parse_url_async", "R:Dictionary");
	ADD_AWAIT("build_url_async", "String");
	ADD_AWAIT("request_target_async", "String");
	ADD_AWAIT("decode_query_async", "R:Dictionary");
	ADD_AWAIT("encode_query_async", "String");
}

// Dispatch URL parsing to a CPU worker.
Signal GDHTTPAPI::parse_url_async(const String &p_raw) {
	return GDValueCall::start([p_raw]() -> Variant { return Url::parse(p_raw); });
}

// Dispatch URL formatting to a CPU worker.
Signal GDHTTPAPI::build_url_async(const Dictionary &p_url) {
	const Dictionary url = p_url.duplicate(true);
	return GDValueCall::start([url]() -> Variant { return Url::build(url); });
}

// Dispatch request-target construction to a CPU worker.
Signal GDHTTPAPI::request_target_async(const Dictionary &p_url) {
	const Dictionary url = p_url.duplicate(true);
	return GDValueCall::start([url]() -> Variant { return Url::request_target(url); });
}

// Dispatch query parsing to a CPU worker.
Signal GDHTTPAPI::decode_query_async(const String &p_raw) {
	return GDValueCall::start([p_raw]() -> Variant { return Url::decode_query(p_raw); });
}

// Dispatch query encoding to a CPU worker.
Signal GDHTTPAPI::encode_query_async(const Dictionary &p_query) {
	const Dictionary query = p_query.duplicate(true);
	return GDValueCall::start([query]() -> Variant { return Url::encode_query(query); });
}

// Expose file, path, and document-format APIs to script.
void GDFSAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path", "mode"), &GDFSAPI::open, DEFVAL("read"));
	ClassDB::bind_method(D_METHOD("open_async", "path", "mode"), &GDFSAPI::open_async, DEFVAL("read"));
	ClassDB::bind_method(D_METHOD("read_text", "path"), &GDFSAPI::read_text_async);
	ClassDB::bind_method(D_METHOD("read_bytes", "path", "offset", "max"), &GDFSAPI::read_bytes_async, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("write_text", "path", "body"), &GDFSAPI::write_text_async);
	ClassDB::bind_method(D_METHOD("replace_text", "path", "old", "body"), &GDFSAPI::replace_text_async);
	ClassDB::bind_method(D_METHOD("_remove_text", "path", "old"), &GDFSAPI::_remove_text);
	ClassDB::bind_method(D_METHOD("_lock", "path"), &GDFSAPI::_lock);
	ClassDB::bind_method(D_METHOD("_local", "path"), &GDFSAPI::_local);
	ClassDB::bind_method(D_METHOD("_local_stamp", "path"), &GDFSAPI::_local_stamp);
	ClassDB::bind_method(D_METHOD("write_bytes", "path", "body"), &GDFSAPI::write_bytes_async);
	ClassDB::bind_method(D_METHOD("append_bytes", "path", "body"), &GDFSAPI::append_bytes_async);
	ClassDB::bind_method(D_METHOD("append_text", "path", "body"), &GDFSAPI::append_text_async);
	ClassDB::bind_method(D_METHOD("exists", "path"), &GDFSAPI::exists_async);
	ClassDB::bind_method(D_METHOD("remove", "path"), &GDFSAPI::remove_async);
	ClassDB::bind_method(D_METHOD("size_of", "path"), &GDFSAPI::size_of_async);
	ClassDB::bind_method(D_METHOD("copy", "src", "dst"), &GDFSAPI::copy_async);
	ClassDB::bind_method(D_METHOD("rename", "src", "dst"), &GDFSAPI::rename_async);
	ClassDB::bind_method(D_METHOD("list_dir", "path"), &GDFSAPI::list_dir_async);
	ClassDB::bind_method(D_METHOD("make_dir", "path"), &GDFSAPI::make_dir_async);
	ClassDB::bind_method(D_METHOD("ensure_dir", "path"), &GDFSAPI::ensure_dir_async);
	ClassDB::bind_method(D_METHOD("walk", "path", "want_dirs", "hidden"), &GDFSAPI::walk_async, DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("glob", "path", "pattern", "hidden"), &GDFSAPI::glob_async, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("remove_all", "path"), &GDFSAPI::remove_all_async);
	ClassDB::bind_method(D_METHOD("can_read", "path"), &GDFSAPI::can_read_async);
	ClassDB::bind_method(D_METHOD("parse_path", "path"), &GDFSAPI::parse_path);
	ClassDB::bind_method(D_METHOD("format_path", "parts"), &GDFSAPI::format_path);
	ClassDB::bind_method(D_METHOD("join", "parts"), &GDFSAPI::join);
	ClassDB::bind_method(D_METHOD("dirname", "path"), &GDFSAPI::dirname);
	ClassDB::bind_method(D_METHOD("basename", "path", "suffix"), &GDFSAPI::basename, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("extname", "path"), &GDFSAPI::extname);
	ClassDB::bind_method(D_METHOD("is_absolute", "path"), &GDFSAPI::is_absolute);
	ClassDB::bind_method(D_METHOD("normalize", "path"), &GDFSAPI::normalize);
	ClassDB::bind_method(D_METHOD("relative", "from", "to"), &GDFSAPI::relative);
	ClassDB::bind_method(D_METHOD("under", "dir", "name"), &GDFSAPI::under);
	ClassDB::bind_method(D_METHOD("create_tar", "root"), &GDFSAPI::create_tar_async);
	ClassDB::bind_method(D_METHOD("extract_tar", "data", "root"), &GDFSAPI::extract_tar_async);
	ClassDB::bind_method(D_METHOD("read_csv", "path", "sep"), &GDFSAPI::read_csv_async, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("read_ini", "path"), &GDFSAPI::read_ini_async);
	ClassDB::bind_method(D_METHOD("read_toml", "path"), &GDFSAPI::read_toml_async);
	ClassDB::bind_method(D_METHOD("read_yaml", "path"), &GDFSAPI::read_yaml_async);
	ClassDB::bind_method(D_METHOD("read_jsonc", "path"), &GDFSAPI::read_jsonc_async);
	ClassDB::bind_method(D_METHOD("read_jsonl", "path"), &GDFSAPI::read_jsonl_async);
	ClassDB::bind_method(D_METHOD("read_front_matter", "path"), &GDFSAPI::read_front_matter_async);
	ClassDB::bind_method(D_METHOD("read_xml", "path"), &GDFSAPI::read_xml_async);
	ClassDB::bind_method(D_METHOD("read_env", "path"), &GDFSAPI::read_env_async, DEFVAL(".env"));
	ClassDB::bind_method(D_METHOD("read_tar", "path"), &GDFSAPI::read_tar_async);
	ClassDB::bind_method(D_METHOD("read_csv_async", "path", "sep"), &GDFSAPI::read_csv_async, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("read_ini_async", "path"), &GDFSAPI::read_ini_async);
	ClassDB::bind_method(D_METHOD("read_toml_async", "path"), &GDFSAPI::read_toml_async);
	ClassDB::bind_method(D_METHOD("read_yaml_async", "path"), &GDFSAPI::read_yaml_async);
	ClassDB::bind_method(D_METHOD("read_jsonc_async", "path"), &GDFSAPI::read_jsonc_async);
	ClassDB::bind_method(D_METHOD("read_jsonl_async", "path"), &GDFSAPI::read_jsonl_async);
	ClassDB::bind_method(D_METHOD("read_front_matter_async", "path"), &GDFSAPI::read_front_matter_async);
	ClassDB::bind_method(D_METHOD("read_xml_async", "path"), &GDFSAPI::read_xml_async);
	ClassDB::bind_method(D_METHOD("read_env_async", "path"), &GDFSAPI::read_env_async, DEFVAL(".env"));
	ClassDB::bind_method(D_METHOD("read_tar_async", "path"), &GDFSAPI::read_tar_async);
	// Run the same Os operations on worker threads through awaitable bindings.
	ClassDB::bind_method(D_METHOD("read_text_async", "path"), &GDFSAPI::read_text_async);
	ClassDB::bind_method(D_METHOD("read_bytes_async", "path", "offset", "max"), &GDFSAPI::read_bytes_async, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("write_text_async", "path", "body"), &GDFSAPI::write_text_async);
	ClassDB::bind_method(D_METHOD("write_bytes_async", "path", "body"), &GDFSAPI::write_bytes_async);
	ClassDB::bind_method(D_METHOD("append_text_async", "path", "body"), &GDFSAPI::append_text_async);
	ClassDB::bind_method(D_METHOD("append_bytes_async", "path", "body"), &GDFSAPI::append_bytes_async);
	ClassDB::bind_method(D_METHOD("copy_async", "src", "dst"), &GDFSAPI::copy_async);
	ClassDB::bind_method(D_METHOD("replace_text_async", "path", "old", "body"), &GDFSAPI::replace_text_async);
	ClassDB::bind_method(D_METHOD("exists_async", "path"), &GDFSAPI::exists_async);
	ClassDB::bind_method(D_METHOD("remove_async", "path"), &GDFSAPI::remove_async);
	ClassDB::bind_method(D_METHOD("size_of_async", "path"), &GDFSAPI::size_of_async);
	ClassDB::bind_method(D_METHOD("rename_async", "src", "dst"), &GDFSAPI::rename_async);
	ClassDB::bind_method(D_METHOD("list_dir_async", "path"), &GDFSAPI::list_dir_async);
	ClassDB::bind_method(D_METHOD("make_dir_async", "path"), &GDFSAPI::make_dir_async);
	ClassDB::bind_method(D_METHOD("ensure_dir_async", "path"), &GDFSAPI::ensure_dir_async);
	ClassDB::bind_method(D_METHOD("walk_async", "path", "want_dirs", "hidden"), &GDFSAPI::walk_async, DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("glob_async", "path", "pattern", "hidden"), &GDFSAPI::glob_async, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("remove_all_async", "path"), &GDFSAPI::remove_all_async);
	ClassDB::bind_method(D_METHOD("can_read_async", "path"), &GDFSAPI::can_read_async);
	ClassDB::bind_method(D_METHOD("create_tar_async", "root"), &GDFSAPI::create_tar_async);
	ClassDB::bind_method(D_METHOD("extract_tar_async", "data", "root"), &GDFSAPI::extract_tar_async);
	ADD_AWAIT("read_text_async", "R:String");
	ADD_AWAIT("open", "R:GDFileStream");
	ADD_AWAIT("open_async", "R:GDFileStream");
	ADD_AWAIT("read_bytes_async", "R:PackedByteArray");
	ADD_AWAIT("write_text_async", "R:Variant");
	ADD_AWAIT("write_bytes_async", "R:Variant");
	ADD_AWAIT("append_text_async", "R:Variant");
	ADD_AWAIT("append_bytes_async", "R:Variant");
	ADD_AWAIT("copy_async", "R:Variant");
	ADD_AWAIT("replace_text_async", "R:Variant");
	ADD_AWAIT("exists_async", "bool");
	ADD_AWAIT("remove_async", "R:Variant");
	ADD_AWAIT("size_of_async", "R:int");
	ADD_AWAIT("rename_async", "R:Variant");
	ADD_AWAIT("list_dir_async", "R:Array");
	ADD_AWAIT("make_dir_async", "R:Variant");
	ADD_AWAIT("ensure_dir_async", "R:Variant");
	ADD_AWAIT("walk_async", "R:Array");
	ADD_AWAIT("glob_async", "R:Array");
	ADD_AWAIT("remove_all_async", "R:Variant");
	ADD_AWAIT("can_read_async", "bool");
	ADD_AWAIT("create_tar_async", "R:PackedByteArray");
	ADD_AWAIT("extract_tar_async", "R:int");
	ADD_AWAIT("read_csv_async", "R:Array");
	ADD_AWAIT("read_ini_async", "R:Dictionary");
	ADD_AWAIT("read_toml_async", "R:Dictionary");
	ADD_AWAIT("read_yaml_async", "R:Variant");
	ADD_AWAIT("read_jsonc_async", "R:Variant");
	ADD_AWAIT("read_jsonl_async", "R:Array");
	ADD_AWAIT("read_front_matter_async", "R:Dictionary");
	ADD_AWAIT("read_xml_async", "R:Dictionary");
	ADD_AWAIT("read_env_async", "R:Dictionary");
	ADD_AWAIT("read_tar_async", "R:Array");
	ADD_AWAIT("read_text", "R:String");
	ADD_AWAIT("read_bytes", "R:PackedByteArray");
	ADD_AWAIT("write_text", "R:Variant");
	ADD_AWAIT("write_bytes", "R:Variant");
	ADD_AWAIT("append_text", "R:Variant");
	ADD_AWAIT("append_bytes", "R:Variant");
	ADD_AWAIT("copy", "R:Variant");
	ADD_AWAIT("replace_text", "R:Variant");
	ADD_AWAIT("exists", "bool");
	ADD_AWAIT("remove", "R:Variant");
	ADD_AWAIT("size_of", "R:int");
	ADD_AWAIT("rename", "R:Variant");
	ADD_AWAIT("list_dir", "R:Array");
	ADD_AWAIT("make_dir", "R:Variant");
	ADD_AWAIT("ensure_dir", "R:Variant");
	ADD_AWAIT("walk", "R:Array");
	ADD_AWAIT("glob", "R:Array");
	ADD_AWAIT("remove_all", "R:Variant");
	ADD_AWAIT("can_read", "bool");
	ADD_AWAIT("create_tar", "R:PackedByteArray");
	ADD_AWAIT("extract_tar", "R:int");
	ADD_AWAIT("read_csv", "R:Array");
	ADD_AWAIT("read_ini", "R:Dictionary");
	ADD_AWAIT("read_toml", "R:Dictionary");
	ADD_AWAIT("read_yaml", "R:Variant");
	ADD_AWAIT("read_jsonc", "R:Variant");
	ADD_AWAIT("read_jsonl", "R:Array");
	ADD_AWAIT("read_front_matter", "R:Dictionary");
	ADD_AWAIT("read_xml", "R:Dictionary");
	ADD_AWAIT("read_env", "R:Dictionary");
	ADD_AWAIT("read_tar", "R:Array");
	const char *wait_names[] = {
		"open", "read_text", "read_bytes", "write_text", "write_bytes", "append_text", "append_bytes", "copy", "replace_text",
		"exists", "remove", "size_of", "rename", "list_dir", "make_dir", "ensure_dir", "walk", "glob", "remove_all", "can_read",
		"create_tar", "extract_tar", "read_csv", "read_ini", "read_toml", "read_yaml", "read_jsonc", "read_jsonl", "read_front_matter",
		"read_xml", "read_env", "read_tar"
	};
	for (const char *name : wait_names) {
		ClassDB::set_auto_wait(get_class_static(), name);
	}
	List<MethodInfo> methods;
	ClassDB::get_method_list(get_class_static(), &methods, true);
	for (const MethodInfo &method : methods) {
		ClassDB::set_method_group(get_class_static(), method.name, file_group(method.name));
	}
}

// Mount one local package source only while the package command is active.
String GDFSAPI::_local(const String &p_path) {
	return Mount::local(p_path);
}

// Fingerprint a checkout through the caller's existing read permissions.
String GDFSAPI::_local_stamp(const String &p_path) {
	return PkgSource::stamp(p_path);
}

// --- Awaitable file operations ---
// Execute the synchronous Os operations on worker threads.
// Keep blocking regular-file system calls off the main thread so the event loop can progress.

// Read a text file through an awaitable operation.
Signal GDFSAPI::read_text_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::read_text(p_path); });
}

// Read up to the requested byte count from a file offset asynchronously.
Signal GDFSAPI::read_bytes_async(const String &p_path, int64_t p_offset, int64_t p_max) {
	return GDFileCall::start([p_path, p_offset, p_max]() { return Os::read_bytes(p_path, p_offset, p_max); });
}

// Write a complete text file asynchronously.
Signal GDFSAPI::write_text_async(const String &p_path, const String &p_body) {
	return GDFileCall::start([p_path, p_body]() { return Os::write_text(p_path, p_body); });
}

// Write a complete binary file asynchronously.
Signal GDFSAPI::write_bytes_async(const String &p_path, const PackedByteArray &p_body) {
	return GDFileCall::start([p_path, p_body]() { return Os::write_bytes(p_path, p_body); });
}

// Append text to a file asynchronously.
Signal GDFSAPI::append_text_async(const String &p_path, const String &p_body) {
	return GDFileCall::start([p_path, p_body]() { return Os::append_text(p_path, p_body); });
}

// Append bytes to a file asynchronously.
Signal GDFSAPI::append_bytes_async(const String &p_path, const PackedByteArray &p_body) {
	return GDFileCall::start([p_path, p_body]() { return Os::append_bytes(p_path, p_body); });
}

// Copy a file to another path asynchronously.
Signal GDFSAPI::copy_async(const String &p_src, const String &p_dst) {
	return GDFileCall::start([p_src, p_dst]() { return Os::copy(p_src, p_dst); });
}

// Replace a file asynchronously only if its contents remain unchanged.
Signal GDFSAPI::replace_text_async(const String &p_path, const Variant &p_old, const String &p_body) {
	const Variant old = p_old.duplicate(true);
	return GDFileCall::start([p_path, old, p_body]() { return Os::replace_text(p_path, old, p_body); });
}

// Check path existence asynchronously.
Signal GDFSAPI::exists_async(const String &p_path) {
	return GDValueCall::start([p_path]() -> Variant { return Os::exists(p_path); }, false);
}

// Remove a file or empty directory asynchronously.
Signal GDFSAPI::remove_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::remove(p_path); });
}

// Get a file's byte count asynchronously.
Signal GDFSAPI::size_of_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::size_of(p_path); });
}

// Rename a file or directory asynchronously.
Signal GDFSAPI::rename_async(const String &p_src, const String &p_dst) {
	return GDFileCall::start([p_src, p_dst]() { return Os::rename(p_src, p_dst); });
}

// List immediate directory entries asynchronously.
Signal GDFSAPI::list_dir_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::list_dir(p_path); });
}

// Create one directory asynchronously.
Signal GDFSAPI::make_dir_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::make_dir(p_path); });
}

// Create a directory and missing parents asynchronously.
Signal GDFSAPI::ensure_dir_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::ensure_dir(p_path); });
}

// Enumerate a directory tree asynchronously.
Signal GDFSAPI::walk_async(const String &p_path, bool p_want_dirs, bool p_hidden) {
	return GDFileCall::start([p_path, p_want_dirs, p_hidden]() { return Os::walk(p_path, p_want_dirs, p_hidden); });
}

// Find paths matching a pattern asynchronously.
Signal GDFSAPI::glob_async(const String &p_path, const String &p_pattern, bool p_hidden) {
	return GDFileCall::start([p_path, p_pattern, p_hidden]() { return Os::glob(p_path, p_pattern, p_hidden); });
}

// Remove a directory tree and its contents asynchronously.
Signal GDFSAPI::remove_all_async(const String &p_path) {
	return GDFileCall::start([p_path]() { return Os::remove_all(p_path); });
}

// Check path readability asynchronously.
Signal GDFSAPI::can_read_async(const String &p_path) {
	return GDValueCall::start([p_path]() -> Variant { return Os::can_read(p_path); }, false);
}

// Archive a directory tree as tar asynchronously.
Signal GDFSAPI::create_tar_async(const String &p_root) {
	return GDFileCall::start([p_root]() { return Tar::pack_dir(p_root); });
}

// Extract a tar archive into a directory asynchronously.
Signal GDFSAPI::extract_tar_async(const PackedByteArray &p_data, const String &p_root) {
	return GDFileCall::start([p_data, p_root]() { return Tar::unpack_to(p_data, p_root); });
}

// Dispatch CSV parsing to a CPU worker.
Signal GDCodecAPI::csv_async(const String &p_src, const String &p_sep) {
	return GDValueCall::start([p_src, p_sep]() -> Variant { return Csv::parse(p_src, p_sep); });
}

// Dispatch CSV encoding to a CPU worker.
Signal GDCodecAPI::to_csv_async(const Array &p_rows, const String &p_sep) {
	const Array rows = p_rows.duplicate(true);
	return GDValueCall::start([rows, p_sep]() -> Variant { return Csv::stringify(rows, p_sep); });
}

// Dispatch object CSV parsing to a CPU worker.
Signal GDCodecAPI::csv_objects_async(const String &p_src, const String &p_sep) {
	return GDValueCall::start([p_src, p_sep]() -> Variant { return Csv::parse_objects(p_src, p_sep); });
}

// Dispatch object CSV encoding to a CPU worker.
Signal GDCodecAPI::to_csv_objects_async(const Array &p_items, const String &p_sep) {
	const Array items = p_items.duplicate(true);
	return GDValueCall::start([items, p_sep]() -> Variant { return Csv::stringify_objects(items, p_sep); });
}

// Dispatch INI parsing to a CPU worker.
Signal GDCodecAPI::ini_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Ini::parse(p_src); });
}

// Dispatch INI encoding to a CPU worker.
Signal GDCodecAPI::to_ini_async(const Dictionary &p_data) {
	const Dictionary data = p_data.duplicate(true);
	return GDValueCall::start([data]() -> Variant { return Ini::stringify(data); });
}

// Dispatch TOML parsing to a CPU worker.
Signal GDCodecAPI::toml_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Toml::parse(p_src); });
}

// Dispatch TOML encoding to a CPU worker.
Signal GDCodecAPI::to_toml_async(const Dictionary &p_data, const String &p_prefix) {
	const Dictionary data = p_data.duplicate(true);
	return GDValueCall::start([data, p_prefix]() -> Variant { return Toml::stringify(data, p_prefix); });
}

// Dispatch YAML parsing to a CPU worker.
Signal GDCodecAPI::yaml_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Yaml::parse(p_src); });
}

// Dispatch YAML encoding to a CPU worker.
Signal GDCodecAPI::to_yaml_async(const Variant &p_data, int p_depth) {
	const Variant data = p_data.duplicate(true);
	return GDValueCall::start([data, p_depth]() -> Variant { return Yaml::stringify(data, p_depth); });
}

// Dispatch commented JSON parsing to a CPU worker.
Signal GDCodecAPI::jsonc_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Jsonc::parse(p_src); });
}

// Dispatch JSON comment stripping to a CPU worker.
Signal GDCodecAPI::strip_jsonc_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Jsonc::strip(p_src); });
}

// Dispatch JSON Lines parsing to a CPU worker.
Signal GDCodecAPI::jsonl_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Jsonl::parse(p_src); });
}

// Dispatch JSON Lines encoding to a CPU worker.
Signal GDCodecAPI::to_jsonl_async(const Array &p_items) {
	const Array items = p_items.duplicate(true);
	return GDValueCall::start([items]() -> Variant { return Jsonl::stringify(items); });
}

// Dispatch front-matter parsing to a CPU worker.
Signal GDCodecAPI::front_matter_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Front::parse(p_src); });
}

// Dispatch front-matter encoding to a CPU worker.
Signal GDCodecAPI::to_front_matter_async(const Dictionary &p_attrs, const String &p_body, const String &p_kind) {
	const Dictionary attrs = p_attrs.duplicate(true);
	return GDValueCall::start([attrs, p_body, p_kind]() -> Variant { return Front::stringify(attrs, p_body, p_kind); });
}

// Dispatch XML parsing to a CPU worker.
Signal GDCodecAPI::xml_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Xml::parse(p_src); });
}

// Dispatch XML encoding to a CPU worker.
Signal GDCodecAPI::to_xml_async(const Dictionary &p_data, int p_indent) {
	const Dictionary data = p_data.duplicate(true);
	return GDValueCall::start([data, p_indent]() -> Variant { return Xml::stringify(data, p_indent); });
}

// Dispatch dotenv parsing to a CPU worker.
Signal GDCodecAPI::env_async(const String &p_src) {
	return GDValueCall::start([p_src]() -> Variant { return Dotenv::parse(p_src); });
}

// Dispatch dotenv encoding to a CPU worker.
Signal GDCodecAPI::to_env_async(const Dictionary &p_data) {
	const Dictionary data = p_data.duplicate(true);
	return GDValueCall::start([data]() -> Variant { return Dotenv::stringify(data); });
}

// Dispatch tar encoding to a CPU worker.
Signal GDCodecAPI::tar_async(const Array &p_entries) {
	const Array entries = p_entries.duplicate(true);
	return GDValueCall::start([entries]() -> Variant { return Tar::pack(entries); });
}

// Dispatch tar decoding to a CPU worker.
Signal GDCodecAPI::untar_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Tar::unpack(p_data); });
}

namespace {

// Dispatch text-file loading and format parsing to separate worker queues.
Signal read_format_async(const String &p_path, std::function<Ref<R>(const String &)> p_parse) {
	return GDFormatCall::start([p_path]() { return Os::read_text(p_path); }, [p_parse](const Ref<R> &p_text) { return p_parse(p_text->get_v()); });
}

} // namespace

// Create a sequential JSON Lines reader.
Ref<GDJSONLReader> GDCodecAPI::jsonl_reader() {
	Ref<GDJSONLReader> out;
	out.instantiate();
	return out;
}

// Read a CSV file on a worker and parse its rows.
Signal GDFSAPI::read_csv_async(const String &p_path, const String &p_sep) {
	return read_format_async(p_path, [p_sep](const String &p_text) { return Csv::parse(p_text, p_sep); });
}

// Read an INI file on a worker and parse its dictionary.
Signal GDFSAPI::read_ini_async(const String &p_path) {
	return read_format_async(p_path, &Ini::parse);
}

// Read a TOML file on a worker and parse its dictionary.
Signal GDFSAPI::read_toml_async(const String &p_path) {
	return read_format_async(p_path, &Toml::parse);
}

// Read a YAML file on a worker and parse its value.
Signal GDFSAPI::read_yaml_async(const String &p_path) {
	return read_format_async(p_path, &Yaml::parse);
}

// Read a commented JSON file on a worker and parse its value.
Signal GDFSAPI::read_jsonc_async(const String &p_path) {
	return read_format_async(p_path, &Jsonc::parse);
}

// Read a JSON Lines file on a worker and parse its values.
Signal GDFSAPI::read_jsonl_async(const String &p_path) {
	return read_format_async(p_path, &Jsonl::parse);
}

// Read a front-matter file on a worker and parse its attributes and body.
Signal GDFSAPI::read_front_matter_async(const String &p_path) {
	return read_format_async(p_path, &Front::parse);
}

// Read an XML file on a worker and parse its dictionary.
Signal GDFSAPI::read_xml_async(const String &p_path) {
	return read_format_async(p_path, &Xml::parse);
}

// Read a dotenv file on a worker and parse its environment dictionary.
Signal GDFSAPI::read_env_async(const String &p_path) {
	return read_format_async(p_path, &Dotenv::parse);
}

// Read a tar file on a worker and decode its entries.
Signal GDFSAPI::read_tar_async(const String &p_path) {
	return GDFormatCall::start([p_path]() { return Os::read_bytes(p_path); }, [](const Ref<R> &p_data) { return Tar::unpack(p_data->get_v()); });
}

// Create a binary heap with a selectable priority function.
Ref<GDBinaryHeap> GDCollectionsAPI::binary_heap(const Callable &p_pick) {
	Ref<GDBinaryHeap> out;
	out.instantiate();
	out->set_pick(p_pick);
	return out;
}

// Create a priority queue.
Ref<GDPriorityQueue> GDCollectionsAPI::priority_queue() {
	Ref<GDPriorityQueue> out;
	out.instantiate();
	return out;
}

// Create an LRU cache with entry-count and expiry settings.
Ref<GDLRUCache> GDCollectionsAPI::lru_cache(int p_limit, uint64_t p_ttl_ms) {
	Ref<GDLRUCache> out;
	out.instantiate();
	out->setup(p_limit, p_ttl_ms);
	return out;
}

// Create an argument-keyed cache of callable results.
Ref<GDMemoizedCallable> GDCollectionsAPI::memo(const Callable &p_fn, int p_limit) {
	Ref<GDMemoizedCallable> out;
	out.instantiate();
	out->setup(p_fn, p_limit);
	return out;
}

// Distribute key-based grouping across scheduler turns.
Signal GDCollectionsAPI::group_by_async(const Array &p_items, const Callable &p_key) {
	return GDCollectionCall::start_array(GDCollectionCall::GROUP, p_items, p_key);
}

// Distribute dictionary value mapping across scheduler turns.
Signal GDCollectionsAPI::map_values_async(const Dictionary &p_src, const Callable &p_fn) {
	return GDCollectionCall::start_dict(GDCollectionCall::MAP_VALUES, p_src, p_fn);
}

// Distribute dictionary key filtering across scheduler turns.
Signal GDCollectionsAPI::filter_keys_async(const Dictionary &p_src, const Callable &p_pred) {
	return GDCollectionCall::start_dict(GDCollectionCall::FILTER_KEYS, p_src, p_pred);
}

// Distribute array partitioning across scheduler turns.
Signal GDCollectionsAPI::partition_async(const Array &p_items, const Callable &p_pred) {
	return GDCollectionCall::start_array(GDCollectionCall::PARTITION, p_items, p_pred);
}

// Distribute key-based deduplication across scheduler turns.
Signal GDCollectionsAPI::unique_by_async(const Array &p_items, const Callable &p_key) {
	return GDCollectionCall::start_array(GDCollectionCall::UNIQUE_BY, p_items, p_key);
}

// Distribute key extraction across scheduler turns, then sort on a CPU worker.
Signal GDCollectionsAPI::sort_by_async(const Array &p_items, const Callable &p_pick) {
	return GDCollectionCall::start_array(GDCollectionCall::SORT_BY, p_items, p_pick);
}

// Distribute numeric summation across scheduler turns.
Signal GDCollectionsAPI::sum_of_async(const Array &p_items, const Callable &p_pick) {
	return GDCollectionCall::start_array(GDCollectionCall::SUM_OF, p_items, p_pick);
}

// Distribute maximum selection across scheduler turns.
Signal GDCollectionsAPI::max_by_async(const Array &p_items, const Callable &p_pick) {
	return GDCollectionCall::start_array(GDCollectionCall::MAX_BY, p_items, p_pick);
}

// Distribute minimum selection across scheduler turns.
Signal GDCollectionsAPI::min_by_async(const Array &p_items, const Callable &p_pick) {
	return GDCollectionCall::start_array(GDCollectionCall::MIN_BY, p_items, p_pick);
}

// Distribute key-index construction across scheduler turns.
Signal GDCollectionsAPI::index_by_async(const Array &p_items, const Callable &p_key) {
	return GDCollectionCall::start_array(GDCollectionCall::INDEX_BY, p_items, p_key);
}

// Dispatch array chunking to a CPU worker.
Signal GDCollectionsAPI::chunk_async(const Array &p_items, int p_size) {
	const Array items = p_items.duplicate(true);
	return GDValueCall::start([items, p_size]() -> Variant { return Coll::chunk(items, p_size); });
}

// Dispatch deduplication to a CPU worker.
Signal GDCollectionsAPI::unique_async(const Array &p_items) {
	const Array items = p_items.duplicate(true);
	return GDValueCall::start([items]() -> Variant { return Coll::unique(items); });
}

// Dispatch dictionary-key sorting to a CPU worker.
Signal GDCollectionsAPI::sort_key_async(const Array &p_items, const String &p_key) {
	const Array items = p_items.duplicate(true);
	return GDValueCall::start([items, p_key]() -> Variant { return Coll::sort_key(items, p_key); });
}

// Dispatch array zipping to a CPU worker.
Signal GDCollectionsAPI::zip_async(const Array &p_a, const Array &p_b) {
	const Array a = p_a.duplicate(true);
	const Array b = p_b.duplicate(true);
	return GDValueCall::start([a, b]() -> Variant { return Coll::zip(a, b); });
}

// Dispatch nested dictionary merging to a CPU worker.
Signal GDCollectionsAPI::deep_merge_async(const Dictionary &p_base, const Dictionary &p_over) {
	const Dictionary base = p_base.duplicate(true);
	const Dictionary over = p_over.duplicate(true);
	return GDValueCall::start([base, over]() -> Variant { return Coll::deep_merge(base, over); });
}

// Expose collection operations and container factories to script.
void GDCollectionsAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("group_by", "items", "key"), &GDCollectionsAPI::group_by);
	ClassDB::bind_method(D_METHOD("map_values", "src", "fn"), &GDCollectionsAPI::map_values);
	ClassDB::bind_method(D_METHOD("filter_keys", "src", "pred"), &GDCollectionsAPI::filter_keys);
	ClassDB::bind_method(D_METHOD("partition", "items", "pred"), &GDCollectionsAPI::partition);
	ClassDB::bind_method(D_METHOD("chunk", "items", "size"), &GDCollectionsAPI::chunk);
	ClassDB::bind_method(D_METHOD("unique_by", "items", "key"), &GDCollectionsAPI::unique_by);
	ClassDB::bind_method(D_METHOD("unique", "items"), &GDCollectionsAPI::unique);
	ClassDB::bind_method(D_METHOD("sort_by", "items", "pick"), &GDCollectionsAPI::sort_by);
	ClassDB::bind_method(D_METHOD("sort_key", "items", "key"), &GDCollectionsAPI::sort_key);
	ClassDB::bind_method(D_METHOD("zip", "a", "b"), &GDCollectionsAPI::zip);
	ClassDB::bind_method(D_METHOD("sum_of", "items", "pick"), &GDCollectionsAPI::sum_of);
	ClassDB::bind_method(D_METHOD("max_by", "items", "pick"), &GDCollectionsAPI::max_by);
	ClassDB::bind_method(D_METHOD("min_by", "items", "pick"), &GDCollectionsAPI::min_by);
	ClassDB::bind_method(D_METHOD("index_by", "items", "key"), &GDCollectionsAPI::index_by);
	ClassDB::bind_method(D_METHOD("deep_merge", "base", "over"), &GDCollectionsAPI::deep_merge);
	ClassDB::bind_method(D_METHOD("binary_heap", "pick"), &GDCollectionsAPI::binary_heap, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("priority_queue"), &GDCollectionsAPI::priority_queue);
	ClassDB::bind_method(D_METHOD("lru_cache", "limit", "ttl_ms"), &GDCollectionsAPI::lru_cache, DEFVAL(128), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("memo", "fn", "limit"), &GDCollectionsAPI::memo, DEFVAL(128));
	ClassDB::bind_method(D_METHOD("group_by_async", "items", "key"), &GDCollectionsAPI::group_by_async);
	ClassDB::bind_method(D_METHOD("map_values_async", "src", "fn"), &GDCollectionsAPI::map_values_async);
	ClassDB::bind_method(D_METHOD("filter_keys_async", "src", "pred"), &GDCollectionsAPI::filter_keys_async);
	ClassDB::bind_method(D_METHOD("partition_async", "items", "pred"), &GDCollectionsAPI::partition_async);
	ClassDB::bind_method(D_METHOD("unique_by_async", "items", "key"), &GDCollectionsAPI::unique_by_async);
	ClassDB::bind_method(D_METHOD("sort_by_async", "items", "pick"), &GDCollectionsAPI::sort_by_async);
	ClassDB::bind_method(D_METHOD("sum_of_async", "items", "pick"), &GDCollectionsAPI::sum_of_async);
	ClassDB::bind_method(D_METHOD("max_by_async", "items", "pick"), &GDCollectionsAPI::max_by_async);
	ClassDB::bind_method(D_METHOD("min_by_async", "items", "pick"), &GDCollectionsAPI::min_by_async);
	ClassDB::bind_method(D_METHOD("index_by_async", "items", "key"), &GDCollectionsAPI::index_by_async);
	ClassDB::bind_method(D_METHOD("chunk_async", "items", "size"), &GDCollectionsAPI::chunk_async);
	ClassDB::bind_method(D_METHOD("unique_async", "items"), &GDCollectionsAPI::unique_async);
	ClassDB::bind_method(D_METHOD("sort_key_async", "items", "key"), &GDCollectionsAPI::sort_key_async);
	ClassDB::bind_method(D_METHOD("zip_async", "a", "b"), &GDCollectionsAPI::zip_async);
	ClassDB::bind_method(D_METHOD("deep_merge_async", "base", "over"), &GDCollectionsAPI::deep_merge_async);
	ADD_AWAIT("group_by_async", "Dictionary");
	ADD_AWAIT("map_values_async", "Dictionary");
	ADD_AWAIT("filter_keys_async", "Dictionary");
	ADD_AWAIT("partition_async", "Array");
	ADD_AWAIT("unique_by_async", "Array");
	ADD_AWAIT("sort_by_async", "Array");
	ADD_AWAIT("sum_of_async", "float");
	ADD_AWAIT("max_by_async", "Variant");
	ADD_AWAIT("min_by_async", "Variant");
	ADD_AWAIT("index_by_async", "Dictionary");
	ADD_AWAIT("chunk_async", "Array");
	ADD_AWAIT("unique_async", "Array");
	ADD_AWAIT("sort_key_async", "Array");
	ADD_AWAIT("zip_async", "Array");
	ADD_AWAIT("deep_merge_async", "Dictionary");
}

// Expose binary, encoding, and hashing APIs to script.
void GDCodecAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("gzip_writer", "writer", "level"), &GDCodecAPI::gzip_writer, DEFVAL(-1));
	ADD_RESULT("gzip_writer", "GDGzipWriter");
	ClassDB::bind_method(D_METHOD("csv", "src", "sep"), &GDCodecAPI::csv, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("to_csv", "rows", "sep"), &GDCodecAPI::to_csv, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("csv_objects", "src", "sep"), &GDCodecAPI::csv_objects, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("to_csv_objects", "items", "sep"), &GDCodecAPI::to_csv_objects, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("ini", "src"), &GDCodecAPI::ini);
	ClassDB::bind_method(D_METHOD("to_ini", "data"), &GDCodecAPI::to_ini);
	ClassDB::bind_method(D_METHOD("toml", "src"), &GDCodecAPI::toml);
	ClassDB::bind_method(D_METHOD("to_toml", "data", "prefix"), &GDCodecAPI::to_toml, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("yaml", "src"), &GDCodecAPI::yaml);
	ClassDB::bind_method(D_METHOD("to_yaml", "data", "depth"), &GDCodecAPI::to_yaml, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("jsonc", "src"), &GDCodecAPI::jsonc);
	ClassDB::bind_method(D_METHOD("strip_jsonc", "src"), &GDCodecAPI::strip_jsonc);
	ClassDB::bind_method(D_METHOD("jsonl", "src"), &GDCodecAPI::jsonl);
	ClassDB::bind_method(D_METHOD("to_jsonl", "items"), &GDCodecAPI::to_jsonl);
	ClassDB::bind_method(D_METHOD("jsonl_reader"), &GDCodecAPI::jsonl_reader);
	ClassDB::bind_method(D_METHOD("front_matter", "src"), &GDCodecAPI::front_matter);
	ClassDB::bind_method(D_METHOD("has_front_matter", "src"), &GDCodecAPI::has_front_matter);
	ClassDB::bind_method(D_METHOD("to_front_matter", "attrs", "body", "kind"), &GDCodecAPI::to_front_matter, DEFVAL("yaml"));
	ClassDB::bind_method(D_METHOD("xml", "src"), &GDCodecAPI::xml);
	ClassDB::bind_method(D_METHOD("to_xml", "data", "indent"), &GDCodecAPI::to_xml, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("env", "src"), &GDCodecAPI::env);
	ClassDB::bind_method(D_METHOD("to_env", "data"), &GDCodecAPI::to_env);
	ClassDB::bind_method(D_METHOD("tar", "entries"), &GDCodecAPI::tar);
	ClassDB::bind_method(D_METHOD("untar", "data"), &GDCodecAPI::untar);
	ClassDB::bind_method(D_METHOD("json_encode", "value", "opts"), &GDCodecAPI::json_encode, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("json_decode", "data"), &GDCodecAPI::json_decode);
	ClassDB::bind_method(D_METHOD("hex_encode", "data"), &GDCodecAPI::hex_encode);
	ClassDB::bind_method(D_METHOD("hex_decode", "text"), &GDCodecAPI::hex_decode);
	ClassDB::bind_method(D_METHOD("base64_encode", "data"), &GDCodecAPI::base64_encode);
	ClassDB::bind_method(D_METHOD("base64_decode", "text", "raw"), &GDCodecAPI::base64_decode, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("base64url_encode", "data"), &GDCodecAPI::base64url_encode);
	ClassDB::bind_method(D_METHOD("base64url_decode", "text", "raw"), &GDCodecAPI::base64url_decode, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("base32_encode", "data"), &GDCodecAPI::base32_encode);
	ClassDB::bind_method(D_METHOD("base32_decode", "text", "raw"), &GDCodecAPI::base32_decode, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("varint_encode", "n"), &GDCodecAPI::varint_encode);
	ClassDB::bind_method(D_METHOD("varint_decode", "data", "at"), &GDCodecAPI::varint_decode, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("concat", "parts"), &GDCodecAPI::concat);
	ClassDB::bind_method(D_METHOD("equals", "a", "b"), &GDCodecAPI::equals);
	ClassDB::bind_method(D_METHOD("includes", "hay", "needle"), &GDCodecAPI::includes);
	ClassDB::bind_method(D_METHOD("index_of", "hay", "needle", "from"), &GDCodecAPI::index_of, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("last_index_of", "hay", "needle"), &GDCodecAPI::last_index_of);
	ClassDB::bind_method(D_METHOD("starts_with", "hay", "prefix"), &GDCodecAPI::starts_with);
	ClassDB::bind_method(D_METHOD("ends_with", "hay", "suffix"), &GDCodecAPI::ends_with);
	ClassDB::bind_method(D_METHOD("repeat", "src", "times"), &GDCodecAPI::repeat);
	ClassDB::bind_method(D_METHOD("fit", "src", "size"), &GDCodecAPI::fit);
	ClassDB::bind_method(D_METHOD("split", "src", "sep"), &GDCodecAPI::split);
	ClassDB::bind_method(D_METHOD("msgpack", "value"), &GDCodecAPI::msgpack);
	ClassDB::bind_method(D_METHOD("unmsgpack", "data"), &GDCodecAPI::unmsgpack);
	ClassDB::bind_method(D_METHOD("cbor", "value"), &GDCodecAPI::cbor);
	ClassDB::bind_method(D_METHOD("uncbor", "data"), &GDCodecAPI::uncbor);
	ClassDB::bind_method(D_METHOD("sha224", "msg"), &GDCodecAPI::sha224);
	ClassDB::bind_method(D_METHOD("sha256", "msg"), &GDCodecAPI::sha256);
	ClassDB::bind_method(D_METHOD("sha384", "msg"), &GDCodecAPI::sha384);
	ClassDB::bind_method(D_METHOD("sha512", "msg"), &GDCodecAPI::sha512);
	ClassDB::bind_method(D_METHOD("sha3_224", "msg"), &GDCodecAPI::sha3_224);
	ClassDB::bind_method(D_METHOD("sha3_256", "msg"), &GDCodecAPI::sha3_256);
	ClassDB::bind_method(D_METHOD("sha3_384", "msg"), &GDCodecAPI::sha3_384);
	ClassDB::bind_method(D_METHOD("sha3_512", "msg"), &GDCodecAPI::sha3_512);
	ClassDB::bind_method(D_METHOD("sha1", "msg"), &GDCodecAPI::sha1);
	ClassDB::bind_method(D_METHOD("hmac", "hash", "key", "msg"), &GDCodecAPI::hmac);
	ClassDB::bind_method(D_METHOD("hmac_sha256", "key", "msg"), &GDCodecAPI::hmac_sha256);
	ClassDB::bind_method(D_METHOD("xor_bytes", "a", "b"), &GDCodecAPI::xor_bytes);
	ClassDB::bind_method(D_METHOD("equal_ct", "a", "b"), &GDCodecAPI::equal_ct);
	ClassDB::bind_method(D_METHOD("pbkdf2_sha256", "password", "salt", "rounds"), &GDCodecAPI::pbkdf2_sha256);
	ClassDB::bind_method(D_METHOD("pbkdf2", "hash", "password", "salt", "rounds", "size"), &GDCodecAPI::pbkdf2);
	ClassDB::bind_method(D_METHOD("hkdf", "hash", "secret", "salt", "info", "size"), &GDCodecAPI::hkdf);
	ClassDB::bind_method(D_METHOD("hkdf_extract", "hash", "secret", "salt"), &GDCodecAPI::hkdf_extract);
	ClassDB::bind_method(D_METHOD("hkdf_expand", "hash", "key", "info", "size"), &GDCodecAPI::hkdf_expand);
	ClassDB::bind_method(D_METHOD("pbkdf2_sha256_async", "password", "salt", "rounds"), &GDCodecAPI::pbkdf2_sha256_async);
	ClassDB::bind_method(D_METHOD("pbkdf2_async", "hash", "password", "salt", "rounds", "size"), &GDCodecAPI::pbkdf2_async);
	ClassDB::bind_method(D_METHOD("hkdf_async", "hash", "secret", "salt", "info", "size"), &GDCodecAPI::hkdf_async);
	ClassDB::bind_method(D_METHOD("hkdf_extract_async", "hash", "secret", "salt"), &GDCodecAPI::hkdf_extract_async);
	ClassDB::bind_method(D_METHOD("hkdf_expand_async", "hash", "key", "info", "size"), &GDCodecAPI::hkdf_expand_async);
	ClassDB::bind_method(D_METHOD("json_encode_async", "value", "opts"), &GDCodecAPI::json_encode_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("json_decode_async", "data"), &GDCodecAPI::json_decode_async);
	ClassDB::bind_method(D_METHOD("msgpack_async", "value"), &GDCodecAPI::msgpack_async);
	ClassDB::bind_method(D_METHOD("unmsgpack_async", "data"), &GDCodecAPI::unmsgpack_async);
	ClassDB::bind_method(D_METHOD("cbor_async", "value"), &GDCodecAPI::cbor_async);
	ClassDB::bind_method(D_METHOD("uncbor_async", "data"), &GDCodecAPI::uncbor_async);
	ClassDB::bind_method(D_METHOD("sha224_async", "msg"), &GDCodecAPI::sha224_async);
	ClassDB::bind_method(D_METHOD("sha256_async", "msg"), &GDCodecAPI::sha256_async);
	ClassDB::bind_method(D_METHOD("sha384_async", "msg"), &GDCodecAPI::sha384_async);
	ClassDB::bind_method(D_METHOD("sha512_async", "msg"), &GDCodecAPI::sha512_async);
	ClassDB::bind_method(D_METHOD("sha3_224_async", "msg"), &GDCodecAPI::sha3_224_async);
	ClassDB::bind_method(D_METHOD("sha3_256_async", "msg"), &GDCodecAPI::sha3_256_async);
	ClassDB::bind_method(D_METHOD("sha3_384_async", "msg"), &GDCodecAPI::sha3_384_async);
	ClassDB::bind_method(D_METHOD("sha3_512_async", "msg"), &GDCodecAPI::sha3_512_async);
	ClassDB::bind_method(D_METHOD("sha1_async", "msg"), &GDCodecAPI::sha1_async);
	ClassDB::bind_method(D_METHOD("hmac_async", "hash", "key", "msg"), &GDCodecAPI::hmac_async);
	ClassDB::bind_method(D_METHOD("hmac_sha256_async", "key", "msg"), &GDCodecAPI::hmac_sha256_async);
	ClassDB::bind_method(D_METHOD("hex_encode_async", "data"), &GDCodecAPI::hex_encode_async);
	ClassDB::bind_method(D_METHOD("hex_decode_async", "text"), &GDCodecAPI::hex_decode_async);
	ClassDB::bind_method(D_METHOD("base64_encode_async", "data"), &GDCodecAPI::base64_encode_async);
	ClassDB::bind_method(D_METHOD("base64_decode_async", "text", "raw"), &GDCodecAPI::base64_decode_async, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("base64url_encode_async", "data"), &GDCodecAPI::base64url_encode_async);
	ClassDB::bind_method(D_METHOD("base64url_decode_async", "text", "raw"), &GDCodecAPI::base64url_decode_async, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("base32_encode_async", "data"), &GDCodecAPI::base32_encode_async);
	ClassDB::bind_method(D_METHOD("base32_decode_async", "text", "raw"), &GDCodecAPI::base32_decode_async, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("concat_async", "parts"), &GDCodecAPI::concat_async);
	ClassDB::bind_method(D_METHOD("equals_async", "a", "b"), &GDCodecAPI::equals_async);
	ClassDB::bind_method(D_METHOD("includes_async", "hay", "needle"), &GDCodecAPI::includes_async);
	ClassDB::bind_method(D_METHOD("index_of_async", "hay", "needle", "from"), &GDCodecAPI::index_of_async, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("last_index_of_async", "hay", "needle"), &GDCodecAPI::last_index_of_async);
	ClassDB::bind_method(D_METHOD("starts_with_async", "hay", "prefix"), &GDCodecAPI::starts_with_async);
	ClassDB::bind_method(D_METHOD("ends_with_async", "hay", "suffix"), &GDCodecAPI::ends_with_async);
	ClassDB::bind_method(D_METHOD("repeat_async", "src", "times"), &GDCodecAPI::repeat_async);
	ClassDB::bind_method(D_METHOD("fit_async", "src", "size"), &GDCodecAPI::fit_async);
	ClassDB::bind_method(D_METHOD("split_async", "src", "sep"), &GDCodecAPI::split_async);
	ClassDB::bind_method(D_METHOD("xor_bytes_async", "a", "b"), &GDCodecAPI::xor_bytes_async);
	ClassDB::bind_method(D_METHOD("equal_ct_async", "a", "b"), &GDCodecAPI::equal_ct_async);
	ClassDB::bind_method(D_METHOD("csv_async", "src", "sep"), &GDCodecAPI::csv_async, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("to_csv_async", "rows", "sep"), &GDCodecAPI::to_csv_async, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("csv_objects_async", "src", "sep"), &GDCodecAPI::csv_objects_async, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("to_csv_objects_async", "items", "sep"), &GDCodecAPI::to_csv_objects_async, DEFVAL(","));
	ClassDB::bind_method(D_METHOD("ini_async", "src"), &GDCodecAPI::ini_async);
	ClassDB::bind_method(D_METHOD("to_ini_async", "data"), &GDCodecAPI::to_ini_async);
	ClassDB::bind_method(D_METHOD("toml_async", "src"), &GDCodecAPI::toml_async);
	ClassDB::bind_method(D_METHOD("to_toml_async", "data", "prefix"), &GDCodecAPI::to_toml_async, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("yaml_async", "src"), &GDCodecAPI::yaml_async);
	ClassDB::bind_method(D_METHOD("to_yaml_async", "data", "depth"), &GDCodecAPI::to_yaml_async, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("jsonc_async", "src"), &GDCodecAPI::jsonc_async);
	ClassDB::bind_method(D_METHOD("strip_jsonc_async", "src"), &GDCodecAPI::strip_jsonc_async);
	ClassDB::bind_method(D_METHOD("jsonl_async", "src"), &GDCodecAPI::jsonl_async);
	ClassDB::bind_method(D_METHOD("to_jsonl_async", "items"), &GDCodecAPI::to_jsonl_async);
	ClassDB::bind_method(D_METHOD("front_matter_async", "src"), &GDCodecAPI::front_matter_async);
	ClassDB::bind_method(D_METHOD("to_front_matter_async", "attrs", "body", "kind"), &GDCodecAPI::to_front_matter_async, DEFVAL("yaml"));
	ClassDB::bind_method(D_METHOD("xml_async", "src"), &GDCodecAPI::xml_async);
	ClassDB::bind_method(D_METHOD("to_xml_async", "data", "indent"), &GDCodecAPI::to_xml_async, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("env_async", "src"), &GDCodecAPI::env_async);
	ClassDB::bind_method(D_METHOD("to_env_async", "data"), &GDCodecAPI::to_env_async);
	ClassDB::bind_method(D_METHOD("tar_async", "entries"), &GDCodecAPI::tar_async);
	ClassDB::bind_method(D_METHOD("untar_async", "data"), &GDCodecAPI::untar_async);
	ADD_RESULT("json_encode", "PackedByteArray");
	ADD_RESULT("json_decode", "Variant");
	ADD_RESULT("hex_decode", "PackedByteArray");
	ADD_RESULT("base64_decode", "PackedByteArray");
	ADD_RESULT("base64url_decode", "PackedByteArray");
	ADD_RESULT("base32_decode", "PackedByteArray");
	ADD_RESULT("varint_decode", "Dictionary");
	ADD_RESULT("to_toml", "String");
	ADD_RESULT("to_yaml", "String");
	ADD_RESULT("to_front_matter", "String");
	ADD_RESULT("to_xml", "String");
	ADD_RESULT("env", "Dictionary");
	ADD_RESULT("to_env", "String");
	ADD_RESULT("tar", "PackedByteArray");
	ADD_RESULT("msgpack", "PackedByteArray");
	ADD_RESULT("cbor", "PackedByteArray");
	ADD_RESULT("pbkdf2_sha256", "PackedByteArray");
	ADD_RESULT("unmsgpack", "Variant");
	ADD_RESULT("uncbor", "Variant");
	ADD_RESULT("hmac", "PackedByteArray");
	ADD_RESULT("pbkdf2", "PackedByteArray");
	ADD_RESULT("hkdf", "PackedByteArray");
	ADD_RESULT("hkdf_extract", "PackedByteArray");
	ADD_RESULT("hkdf_expand", "PackedByteArray");
	ADD_RESULT("csv", "Array");
	ADD_RESULT("csv_objects", "Array");
	ADD_RESULT("ini", "Dictionary");
	ADD_RESULT("toml", "Dictionary");
	ADD_RESULT("yaml", "Variant");
	ADD_RESULT("jsonc", "Variant");
	ADD_RESULT("jsonl", "Array");
	ADD_RESULT("to_jsonl", "String");
	ADD_RESULT("front_matter", "Dictionary");
	ADD_RESULT("xml", "Dictionary");
	ADD_RESULT("untar", "Array");
	ADD_AWAIT("pbkdf2_sha256_async", "R:PackedByteArray");
	ADD_AWAIT("pbkdf2_async", "R:PackedByteArray");
	ADD_AWAIT("hkdf_async", "R:PackedByteArray");
	ADD_AWAIT("hkdf_extract_async", "R:PackedByteArray");
	ADD_AWAIT("hkdf_expand_async", "R:PackedByteArray");
	ADD_AWAIT("json_encode_async", "R:PackedByteArray");
	ADD_AWAIT("json_decode_async", "R:Variant");
	ADD_AWAIT("msgpack_async", "R:PackedByteArray");
	ADD_AWAIT("unmsgpack_async", "R:Variant");
	ADD_AWAIT("cbor_async", "R:PackedByteArray");
	ADD_AWAIT("uncbor_async", "R:Variant");
	ADD_AWAIT("sha224_async", "PackedByteArray");
	ADD_AWAIT("sha256_async", "PackedByteArray");
	ADD_AWAIT("sha384_async", "PackedByteArray");
	ADD_AWAIT("sha512_async", "PackedByteArray");
	ADD_AWAIT("sha3_224_async", "PackedByteArray");
	ADD_AWAIT("sha3_256_async", "PackedByteArray");
	ADD_AWAIT("sha3_384_async", "PackedByteArray");
	ADD_AWAIT("sha3_512_async", "PackedByteArray");
	ADD_AWAIT("sha1_async", "PackedByteArray");
	ADD_AWAIT("hmac_async", "R:PackedByteArray");
	ADD_AWAIT("hmac_sha256_async", "PackedByteArray");
	ADD_AWAIT("hex_encode_async", "String");
	ADD_AWAIT("hex_decode_async", "R:PackedByteArray");
	ADD_AWAIT("base64_encode_async", "String");
	ADD_AWAIT("base64_decode_async", "R:PackedByteArray");
	ADD_AWAIT("base64url_encode_async", "String");
	ADD_AWAIT("base64url_decode_async", "R:PackedByteArray");
	ADD_AWAIT("base32_encode_async", "String");
	ADD_AWAIT("base32_decode_async", "R:PackedByteArray");
	ADD_AWAIT("concat_async", "PackedByteArray");
	ADD_AWAIT("equals_async", "bool");
	ADD_AWAIT("includes_async", "bool");
	ADD_AWAIT("index_of_async", "int");
	ADD_AWAIT("last_index_of_async", "int");
	ADD_AWAIT("starts_with_async", "bool");
	ADD_AWAIT("ends_with_async", "bool");
	ADD_AWAIT("repeat_async", "PackedByteArray");
	ADD_AWAIT("fit_async", "PackedByteArray");
	ADD_AWAIT("split_async", "Array");
	ADD_AWAIT("xor_bytes_async", "PackedByteArray");
	ADD_AWAIT("equal_ct_async", "bool");
	ADD_AWAIT("csv_async", "R:Array");
	ADD_AWAIT("to_csv_async", "String");
	ADD_AWAIT("csv_objects_async", "R:Array");
	ADD_AWAIT("to_csv_objects_async", "String");
	ADD_AWAIT("ini_async", "R:Dictionary");
	ADD_AWAIT("to_ini_async", "String");
	ADD_AWAIT("toml_async", "R:Dictionary");
	ADD_AWAIT("to_toml_async", "R:String");
	ADD_AWAIT("yaml_async", "R:Variant");
	ADD_AWAIT("to_yaml_async", "R:String");
	ADD_AWAIT("jsonc_async", "R:Variant");
	ADD_AWAIT("strip_jsonc_async", "String");
	ADD_AWAIT("jsonl_async", "R:Array");
	ADD_AWAIT("to_jsonl_async", "R:String");
	ADD_AWAIT("front_matter_async", "R:Dictionary");
	ADD_AWAIT("to_front_matter_async", "R:String");
	ADD_AWAIT("xml_async", "R:Dictionary");
	ADD_AWAIT("to_xml_async", "R:String");
	ADD_AWAIT("env_async", "R:Dictionary");
	ADD_AWAIT("to_env_async", "R:String");
	ADD_AWAIT("tar_async", "R:PackedByteArray");
	ADD_AWAIT("untar_async", "R:Array");
	List<MethodInfo> methods;
	ClassDB::get_method_list(get_class_static(), &methods, true);
	for (const MethodInfo &method : methods) {
		ClassDB::set_method_group(get_class_static(), method.name, data_group(method.name));
	}
}

// Dispatch key derivation to a CPU worker.
Signal GDCodecAPI::pbkdf2_sha256_async(const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds) {
	return GDValueCall::start([p_pass, p_salt, p_rounds]() -> Variant { return Hash::pbkdf2_sha256(p_pass, p_salt, p_rounds); });
}

// Dispatch PBKDF2 with the selected hash to a CPU worker.
Signal GDCodecAPI::pbkdf2_async(const String &p_hash, const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds, int64_t p_size) {
	return GDValueCall::start([p_hash, p_pass, p_salt, p_rounds, p_size]() -> Variant { return Hash::pbkdf2(p_hash, p_pass, p_salt, p_rounds, p_size); });
}

// Dispatch HKDF key derivation to a CPU worker.
Signal GDCodecAPI::hkdf_async(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt, const PackedByteArray &p_info, int64_t p_size) {
	return GDValueCall::start([p_hash, p_secret, p_salt, p_info, p_size]() -> Variant { return Hash::hkdf(p_hash, p_secret, p_salt, p_info, p_size); });
}

// Dispatch HKDF extraction to a CPU worker.
Signal GDCodecAPI::hkdf_extract_async(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt) {
	return GDValueCall::start([p_hash, p_secret, p_salt]() -> Variant { return Hash::hkdf_extract(p_hash, p_secret, p_salt); });
}

// Dispatch HKDF expansion to a CPU worker.
Signal GDCodecAPI::hkdf_expand_async(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_info, int64_t p_size) {
	return GDValueCall::start([p_hash, p_key, p_info, p_size]() -> Variant { return Hash::hkdf_expand(p_hash, p_key, p_info, p_size); });
}

// Dispatch JSON encoding to a CPU worker.
Signal GDCodecAPI::json_encode_async(const Variant &p_value, const Dictionary &p_opts) {
	const Variant value = p_value;
	const Dictionary opts = p_opts.duplicate();
	return GDValueCall::start([value, opts]() -> Variant { return JsonData::encode(value, opts); });
}

// Dispatch JSON decoding to a CPU worker.
Signal GDCodecAPI::json_decode_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return JsonData::decode(p_data); });
}

// Dispatch MessagePack encoding to a CPU worker.
Signal GDCodecAPI::msgpack_async(const Variant &p_value) {
	const Variant value = p_value.duplicate(true);
	return GDValueCall::start([value]() -> Variant { return Msgpack::encode(value); });
}

// Dispatch MessagePack decoding to a CPU worker.
Signal GDCodecAPI::unmsgpack_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Msgpack::decode(p_data); });
}

// Dispatch CBOR encoding to a CPU worker.
Signal GDCodecAPI::cbor_async(const Variant &p_value) {
	const Variant value = p_value.duplicate(true);
	return GDValueCall::start([value]() -> Variant { return Cbor::encode(value); });
}

// Dispatch CBOR decoding to a CPU worker.
Signal GDCodecAPI::uncbor_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Cbor::decode(p_data); });
}

// Dispatch SHA-224 hashing to a CPU worker.
Signal GDCodecAPI::sha224_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha224(p_msg); });
}

// Dispatch SHA-256 hashing to a CPU worker.
Signal GDCodecAPI::sha256_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha256(p_msg); });
}

// Dispatch SHA-384 hashing to a CPU worker.
Signal GDCodecAPI::sha384_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha384(p_msg); });
}

// Dispatch SHA-512 hashing to a CPU worker.
Signal GDCodecAPI::sha512_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha512(p_msg); });
}

// Dispatch SHA3-224 hashing to a CPU worker.
Signal GDCodecAPI::sha3_224_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha3_224(p_msg); });
}

// Dispatch SHA3-256 hashing to a CPU worker.
Signal GDCodecAPI::sha3_256_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha3_256(p_msg); });
}

// Dispatch SHA3-384 hashing to a CPU worker.
Signal GDCodecAPI::sha3_384_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha3_384(p_msg); });
}

// Dispatch SHA3-512 hashing to a CPU worker.
Signal GDCodecAPI::sha3_512_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha3_512(p_msg); });
}

// Dispatch SHA-1 hashing to a CPU worker.
Signal GDCodecAPI::sha1_async(const PackedByteArray &p_msg) {
	return GDValueCall::start([p_msg]() -> Variant { return Hash::sha1(p_msg); });
}

// Dispatch HMAC with the selected hash to a CPU worker.
Signal GDCodecAPI::hmac_async(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_msg) {
	return GDValueCall::start([p_hash, p_key, p_msg]() -> Variant { return Hash::hmac(p_hash, p_key, p_msg); });
}

// Dispatch HMAC-SHA-256 to a CPU worker.
Signal GDCodecAPI::hmac_sha256_async(const PackedByteArray &p_key, const PackedByteArray &p_msg) {
	return GDValueCall::start([p_key, p_msg]() -> Variant { return Hash::hmac_sha256(p_key, p_msg); });
}

// Dispatch hexadecimal encoding to a CPU worker.
Signal GDCodecAPI::hex_encode_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Encoding::hex_encode(p_data); });
}

// Dispatch hexadecimal decoding to a CPU worker.
Signal GDCodecAPI::hex_decode_async(const String &p_text) {
	return GDValueCall::start([p_text]() -> Variant { return Encoding::hex_decode(p_text); });
}

// Dispatch Base64 encoding to a CPU worker.
Signal GDCodecAPI::base64_encode_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Encoding::base64_encode(p_data); });
}

// Dispatch Base64 decoding to a CPU worker.
Signal GDCodecAPI::base64_decode_async(const String &p_text, bool p_raw) {
	return GDValueCall::start([p_text, p_raw]() -> Variant { return Encoding::base64_decode(p_text, p_raw); });
}

// Dispatch URL-safe Base64 encoding to a CPU worker.
Signal GDCodecAPI::base64url_encode_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Encoding::base64url_encode(p_data); });
}

// Dispatch URL-safe Base64 decoding to a CPU worker.
Signal GDCodecAPI::base64url_decode_async(const String &p_text, bool p_raw) {
	return GDValueCall::start([p_text, p_raw]() -> Variant { return Encoding::base64url_decode(p_text, p_raw); });
}

// Dispatch Base32 encoding to a CPU worker.
Signal GDCodecAPI::base32_encode_async(const PackedByteArray &p_data) {
	return GDValueCall::start([p_data]() -> Variant { return Encoding::base32_encode(p_data); });
}

// Dispatch Base32 decoding to a CPU worker.
Signal GDCodecAPI::base32_decode_async(const String &p_text, bool p_raw) {
	return GDValueCall::start([p_text, p_raw]() -> Variant { return Encoding::base32_decode(p_text, p_raw); });
}

// Dispatch byte-sequence concatenation to a CPU worker.
Signal GDCodecAPI::concat_async(const Array &p_parts) {
	const Array parts = p_parts.duplicate(true);
	return GDValueCall::start([parts]() -> Variant { return Bytes::concat(parts); });
}

// Dispatch byte-sequence equality checks to a CPU worker.
Signal GDCodecAPI::equals_async(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	return GDValueCall::start([p_a, p_b]() -> Variant { return Bytes::equals(p_a, p_b); });
}

// Dispatch byte-subsequence searches to a CPU worker.
Signal GDCodecAPI::includes_async(const PackedByteArray &p_hay, const PackedByteArray &p_needle) {
	return GDValueCall::start([p_hay, p_needle]() -> Variant { return Bytes::includes(p_hay, p_needle); });
}

// Dispatch first byte-subsequence searches to a CPU worker.
Signal GDCodecAPI::index_of_async(const PackedByteArray &p_hay, const PackedByteArray &p_needle, int p_from) {
	return GDValueCall::start([p_hay, p_needle, p_from]() -> Variant { return Bytes::index_of(p_hay, p_needle, p_from); });
}

// Dispatch last byte-subsequence searches to a CPU worker.
Signal GDCodecAPI::last_index_of_async(const PackedByteArray &p_hay, const PackedByteArray &p_needle) {
	return GDValueCall::start([p_hay, p_needle]() -> Variant { return Bytes::last_index_of(p_hay, p_needle); });
}

// Dispatch byte-prefix comparison to a CPU worker.
Signal GDCodecAPI::starts_with_async(const PackedByteArray &p_hay, const PackedByteArray &p_prefix) {
	return GDValueCall::start([p_hay, p_prefix]() -> Variant { return Bytes::starts_with(p_hay, p_prefix); });
}

// Dispatch byte-suffix comparison to a CPU worker.
Signal GDCodecAPI::ends_with_async(const PackedByteArray &p_hay, const PackedByteArray &p_suffix) {
	return GDValueCall::start([p_hay, p_suffix]() -> Variant { return Bytes::ends_with(p_hay, p_suffix); });
}

// Dispatch byte repetition to a CPU worker.
Signal GDCodecAPI::repeat_async(const PackedByteArray &p_src, int p_times) {
	return GDValueCall::start([p_src, p_times]() -> Variant { return Bytes::repeat(p_src, p_times); });
}

// Dispatch byte-sequence resizing to a CPU worker.
Signal GDCodecAPI::fit_async(const PackedByteArray &p_src, int p_size) {
	return GDValueCall::start([p_src, p_size]() -> Variant { return Bytes::fit(p_src, p_size); });
}

// Dispatch byte-sequence splitting to a CPU worker.
Signal GDCodecAPI::split_async(const PackedByteArray &p_src, const PackedByteArray &p_sep) {
	return GDValueCall::start([p_src, p_sep]() -> Variant { return Bytes::split(p_src, p_sep); });
}

// Dispatch bytewise exclusive OR to a CPU worker.
Signal GDCodecAPI::xor_bytes_async(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	return GDValueCall::start([p_a, p_b]() -> Variant { return Hash::xor_bytes(p_a, p_b); });
}

// Dispatch constant-time comparison to a CPU worker.
Signal GDCodecAPI::equal_ct_async(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	return GDValueCall::start([p_a, p_b]() -> Variant { return Hash::equal_ct(p_a, p_b); });
}

// Expose UUID and ULID APIs to script.
void GDIDAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("ulid", "ms"), &GDIDAPI::ulid, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("is_ulid", "text"), &GDIDAPI::is_ulid);
	ClassDB::bind_method(D_METHOD("ulid_time", "text"), &GDIDAPI::ulid_time);
	ClassDB::bind_method(D_METHOD("uuid"), &GDIDAPI::uuid);
	ClassDB::bind_method(D_METHOD("uuid_v5", "space", "name"), &GDIDAPI::uuid_v5);
	ClassDB::bind_method(D_METHOD("uuid_v5_async", "space", "name"), &GDIDAPI::uuid_v5_async);
	ClassDB::bind_method(D_METHOD("is_uuid", "text"), &GDIDAPI::is_uuid);
	ClassDB::bind_method(D_METHOD("uuid_bytes", "text"), &GDIDAPI::uuid_bytes);
	ClassDB::bind_method(D_METHOD("uuid_version", "text"), &GDIDAPI::uuid_version);
	ClassDB::bind_method(D_METHOD("nil_uuid"), &GDIDAPI::nil_uuid);
	ClassDB::bind_method(D_METHOD("dns_namespace"), &GDIDAPI::dns_namespace);
	ClassDB::bind_method(D_METHOD("url_namespace"), &GDIDAPI::url_namespace);
	ClassDB::bind_method(D_METHOD("oid_namespace"), &GDIDAPI::oid_namespace);
	ADD_RESULT("ulid_time", "int");
	ADD_RESULT("uuid_v5", "String");
	ADD_RESULT("uuid_bytes", "PackedByteArray");
	ADD_AWAIT("uuid_v5_async", "R:String");
}

// Dispatch name-based UUID generation to a CPU worker.
Signal GDIDAPI::uuid_v5_async(const String &p_space, const String &p_name) {
	return GDValueCall::start([p_space, p_name]() -> Variant { return Uuid::v5(p_space, p_name); });
}

// Expose text formatting APIs to script.
void GDTextAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("closest", "word", "options"), &GDTextAPI::closest);
	ClassDB::bind_method(D_METHOD("ellipsis", "text", "width"), &GDTextAPI::ellipsis);
	ClassDB::bind_method(D_METHOD("size_of", "bytes"), &GDTextAPI::size_of);
	ClassDB::bind_method(D_METHOD("duration", "ms"), &GDTextAPI::duration);
	ClassDB::bind_method(D_METHOD("distance", "a", "b"), &GDTextAPI::distance);
	ClassDB::bind_method(D_METHOD("snake", "text"), &GDTextAPI::snake);
	ClassDB::bind_method(D_METHOD("camel", "text"), &GDTextAPI::camel);
	ClassDB::bind_method(D_METHOD("title", "text"), &GDTextAPI::title);
	ClassDB::bind_method(D_METHOD("table", "rows", "gap"), &GDTextAPI::table, DEFVAL(2));
	ClassDB::bind_method(D_METHOD("distance_async", "a", "b"), &GDTextAPI::distance_async);
	ClassDB::bind_method(D_METHOD("closest_async", "word", "options"), &GDTextAPI::closest_async);
	ClassDB::bind_method(D_METHOD("ellipsis_async", "text", "width"), &GDTextAPI::ellipsis_async);
	ClassDB::bind_method(D_METHOD("snake_async", "text"), &GDTextAPI::snake_async);
	ClassDB::bind_method(D_METHOD("camel_async", "text"), &GDTextAPI::camel_async);
	ClassDB::bind_method(D_METHOD("title_async", "text"), &GDTextAPI::title_async);
	ClassDB::bind_method(D_METHOD("table_async", "rows", "gap"), &GDTextAPI::table_async, DEFVAL(2));
	ADD_AWAIT("distance_async", "int");
	ADD_AWAIT("closest_async", "String");
	ADD_AWAIT("ellipsis_async", "String");
	ADD_AWAIT("snake_async", "String");
	ADD_AWAIT("camel_async", "String");
	ADD_AWAIT("title_async", "String");
	ADD_AWAIT("table_async", "String");
}

// Dispatch edit-distance calculation to a CPU worker.
Signal GDTextAPI::distance_async(const String &p_a, const String &p_b) {
	return GDValueCall::start([p_a, p_b]() -> Variant { return Text::distance(p_a, p_b); });
}

// Dispatch nearest-candidate selection to a CPU worker.
Signal GDTextAPI::closest_async(const String &p_word, const PackedStringArray &p_options) {
	return GDValueCall::start([p_word, p_options]() -> Variant { return Text::closest(p_word, p_options); });
}

// Dispatch text shortening to a CPU worker.
Signal GDTextAPI::ellipsis_async(const String &p_text, int p_width) {
	return GDValueCall::start([p_text, p_width]() -> Variant { return Text::ellipsis(p_text, p_width); });
}

// Dispatch snake-case conversion to a CPU worker.
Signal GDTextAPI::snake_async(const String &p_text) {
	return GDValueCall::start([p_text]() -> Variant { return Text::snake(p_text); });
}

// Dispatch camel-case conversion to a CPU worker.
Signal GDTextAPI::camel_async(const String &p_text) {
	return GDValueCall::start([p_text]() -> Variant { return Text::camel(p_text); });
}

// Dispatch title-case conversion to a CPU worker.
Signal GDTextAPI::title_async(const String &p_text) {
	return GDValueCall::start([p_text]() -> Variant { return Text::title(p_text); });
}

// Dispatch text-table formatting to a CPU worker.
Signal GDTextAPI::table_async(const Array &p_rows, int p_gap) {
	const Array rows = p_rows.duplicate(true);
	return GDValueCall::start([rows, p_gap]() -> Variant { return Text::table(rows, p_gap); });
}

// Expose HTML entities, tags, and templates to script.
void GDHTMLAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("fill", "tpl", "data", "partials"), &GDHTMLAPI::fill, DEFVAL(Dictionary()));
	ADD_RESULT("fill", "String");
	ClassDB::bind_method(D_METHOD("template", "tpl", "partials"), &GDHTMLAPI::template_of, DEFVAL(Dictionary()));
	ADD_RESULT("template", "GDHTMLTemplate");
	ClassDB::bind_method(D_METHOD("attr", "value"), &GDHTMLAPI::attr);
	ClassDB::bind_method(D_METHOD("tag", "name", "body", "attrs"), &GDHTMLAPI::tag, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("escape", "text"), &GDHTMLAPI::escape);
	ClassDB::bind_method(D_METHOD("unescape", "text"), &GDHTMLAPI::unescape);
	ClassDB::bind_method(D_METHOD("escape_async", "text"), &GDHTMLAPI::escape_async);
	ClassDB::bind_method(D_METHOD("unescape_async", "text"), &GDHTMLAPI::unescape_async);
	ClassDB::bind_method(D_METHOD("attr_async", "value"), &GDHTMLAPI::attr_async);
	ClassDB::bind_method(D_METHOD("tag_async", "name", "body", "attrs"), &GDHTMLAPI::tag_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("fill_async", "tpl", "data", "partials"), &GDHTMLAPI::fill_async, DEFVAL(Dictionary()));
	ADD_AWAIT("escape_async", "String");
	ADD_AWAIT("unescape_async", "String");
	ADD_AWAIT("attr_async", "String");
	ADD_AWAIT("tag_async", "String");
	ADD_AWAIT("fill_async", "R:String");
}

// Dispatch HTML text escaping to a CPU worker.
Signal GDHTMLAPI::escape_async(const String &p_text) {
	return GDValueCall::start([p_text]() -> Variant { return Html::escape(p_text); });
}

// Dispatch HTML entity decoding to a CPU worker.
Signal GDHTMLAPI::unescape_async(const String &p_text) {
	return GDValueCall::start([p_text]() -> Variant { return Html::unescape(p_text); });
}

// Dispatch HTML attribute escaping to a CPU worker.
Signal GDHTMLAPI::attr_async(const String &p_value) {
	return GDValueCall::start([p_value]() -> Variant { return Html::attr(p_value); });
}

// Dispatch HTML tag construction to a CPU worker.
Signal GDHTMLAPI::tag_async(const String &p_name, const String &p_body, const Dictionary &p_attrs) {
	const Dictionary attrs = p_attrs;
	return GDValueCall::start([p_name, p_body, attrs]() -> Variant { return Html::tag(p_name, p_body, attrs); });
}

// Dispatch HTML template rendering to a CPU worker.
Signal GDHTMLAPI::fill_async(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials) {
	const Dictionary data = p_data;
	const Dictionary partials = p_partials;
	return GDValueCall::start([p_tpl, data, partials]() -> Variant { return Html::fill(p_tpl, data, partials); });
}

// Expose semantic-version APIs to script.
void GDSemanticVersionAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("parse", "raw"), &GDSemanticVersionAPI::parse);
	ClassDB::bind_method(D_METHOD("is_canonical", "raw"), &GDSemanticVersionAPI::is_canonical);
	ClassDB::bind_method(D_METHOD("compare", "a", "b"), &GDSemanticVersionAPI::compare);
	ClassDB::bind_method(D_METHOD("is_stable", "v"), &GDSemanticVersionAPI::is_stable);
	ClassDB::bind_method(D_METHOD("text", "v"), &GDSemanticVersionAPI::text);
	ClassDB::bind_method(D_METHOD("best", "list", "range"), &GDSemanticVersionAPI::best, DEFVAL("*"));
	ClassDB::bind_method(D_METHOD("best_async", "list", "range"), &GDSemanticVersionAPI::best_async, DEFVAL("*"));
	ClassDB::bind_method(D_METHOD("satisfies", "v", "range"), &GDSemanticVersionAPI::satisfies);
	ADD_RESULT("parse", "Dictionary");
	ADD_RESULT("best", "Dictionary");
	ADD_AWAIT("best_async", "R:Dictionary");
}

// Dispatch version-list selection to a CPU worker.
Signal GDSemanticVersionAPI::best_async(const PackedStringArray &p_list, const String &p_range) {
	return GDValueCall::start([p_list, p_range]() -> Variant { return Semver::best(p_list, p_range); });
}

// Expose date-time conversion and arithmetic to script.
void GDDateTimeAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("now"), &GDDateTimeAPI::now);
	ClassDB::bind_method(D_METHOD("to_parts", "unix"), &GDDateTimeAPI::to_parts);
	ClassDB::bind_method(D_METHOD("from_parts", "parts"), &GDDateTimeAPI::from_parts);
	ClassDB::bind_method(D_METHOD("to_iso", "unix"), &GDDateTimeAPI::to_iso);
	ClassDB::bind_method(D_METHOD("to_http", "unix"), &GDDateTimeAPI::to_http);
	ClassDB::bind_method(D_METHOD("parse_iso", "text"), &GDDateTimeAPI::parse_iso);
	ClassDB::bind_method(D_METHOD("add", "unix", "amount", "unit"), &GDDateTimeAPI::add, DEFVAL("second"));
	ClassDB::bind_method(D_METHOD("diff", "a", "b", "unit"), &GDDateTimeAPI::diff, DEFVAL("second"));
	ClassDB::bind_method(D_METHOD("start_of_day", "unix"), &GDDateTimeAPI::start_of_day);
	ClassDB::bind_method(D_METHOD("weekday", "unix"), &GDDateTimeAPI::weekday);
	ClassDB::bind_method(D_METHOD("is_leap", "year"), &GDDateTimeAPI::is_leap);
	ClassDB::bind_method(D_METHOD("days_in_month", "year", "month"), &GDDateTimeAPI::days_in_month);
	ClassDB::bind_method(D_METHOD("ago", "unix", "base"), &GDDateTimeAPI::ago, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("format", "unix", "pattern"), &GDDateTimeAPI::format);
	ADD_RESULT("parse_iso", "int");
}

// Return the running executable's version.
String GDCLIAPI::version() const {
	return GD_CLI_VERSION;
}

// Return the upstream documentation base used to resolve version-independent documentation links.
String GDCLIAPI::docs_url() const {
	return GODOT_VERSION_DOCS_URL;
}

// Create a command-line flag parser.
Ref<GDCLIFlags> GDCLIAPI::flags() const {
	Ref<GDCLIFlags> out;
	out.instantiate();
	return out;
}

// Read a permitted environment value, or return the default when absent.
Variant GDCLIAPI::env(const String &p_name, const Variant &p_fallback) const {
	if (!Perm::check(Perm::ENV, p_name) || !GDSystem::has_env(p_name)) {
		return p_fallback;
	}
	return GDSystem::env(p_name);
}

// Read a required environment value, or return Err when absent.
Ref<R> GDCLIAPI::require_env(const String &p_name) const {
	if (!Perm::check(Perm::ENV, p_name)) {
		return R::err(vformat("environment variable %s is not allowed", p_name), Err::PERMISSION_DENIED);
	}
	if (!GDSystem::has_env(p_name)) {
		return R::err(vformat("environment variable %s is missing", p_name), Err::NOT_FOUND);
	}
	return R::ok(GDSystem::env(p_name));
}

// Return the current working directory.
String GDCLIAPI::cwd() const {
	return GDSystem::cwd();
}
// Return the operating-system name only when permitted.
String GDCLIAPI::platform() const {
	return Perm::check(Perm::SYS, "platform") ? GDSystem::platform() : String();
}
// Return the CPU architecture name only when permitted.
String GDCLIAPI::arch() const {
	return Perm::check(Perm::SYS, "arch") ? Engine::get_singleton()->get_architecture_name() : String();
}

// Expose command-line and environment APIs to script.
void GDCLIAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("version"), &GDCLIAPI::version);
	ClassDB::bind_method(D_METHOD("docs_url"), &GDCLIAPI::docs_url);
	ClassDB::bind_method(D_METHOD("flags"), &GDCLIAPI::flags);
	ClassDB::bind_method(D_METHOD("env", "name", "fallback"), &GDCLIAPI::env, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("require_env", "name"), &GDCLIAPI::require_env);
	ClassDB::bind_method(D_METHOD("cwd"), &GDCLIAPI::cwd);
	ClassDB::bind_method(D_METHOD("platform"), &GDCLIAPI::platform);
	ClassDB::bind_method(D_METHOD("arch"), &GDCLIAPI::arch);
	ClassDB::bind_method(D_METHOD("stdin_tty"), &GDCLIAPI::stdin_tty);
	ClassDB::bind_method(D_METHOD("stdout_tty"), &GDCLIAPI::stdout_tty);
	ClassDB::bind_method(D_METHOD("stderr_tty"), &GDCLIAPI::stderr_tty);
	ClassDB::bind_method(D_METHOD("paint", "text", "code"), &GDCLIAPI::paint);
	ClassDB::bind_method(D_METHOD("red", "text"), &GDCLIAPI::red);
	ClassDB::bind_method(D_METHOD("green", "text"), &GDCLIAPI::green);
	ClassDB::bind_method(D_METHOD("yellow", "text"), &GDCLIAPI::yellow);
	ClassDB::bind_method(D_METHOD("blue", "text"), &GDCLIAPI::blue);
	ClassDB::bind_method(D_METHOD("gray", "text"), &GDCLIAPI::gray);
	ClassDB::bind_method(D_METHOD("bold", "text"), &GDCLIAPI::bold);
	ClassDB::bind_method(D_METHOD("run", "path", "args", "opts"), &GDCLIAPI::run, DEFVAL(PackedStringArray()), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("run_async", "path", "args", "opts"), &GDCLIAPI::run_async, DEFVAL(PackedStringArray()), DEFVAL(Dictionary()));
	ADD_RESULT("require_env", "String");
	ADD_AWAIT("run", "R:Dictionary");
	ADD_AWAIT("run_async", "R:Dictionary");
	ADD_AUTO_WAIT("run");
}

// Start a child process and return an awaitable completion signal.
Signal GDCLIAPI::run(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts) {
	return Proc::run(p_path, p_args, p_opts);
}

// Create a lightweight assertion collector.
Ref<GDTestCheck> GDTestAPI::check() const {
	Ref<GDTestCheck> out;
	out.instantiate();
	return out;
}

// Expose test helpers to script.
void GDTestAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("check"), &GDTestAPI::check);
}

// Create one instance of each standard child API.
GDAPI::GDAPI() {
	async = memnew(GDAsyncAPI);
	log = memnew(GDLogAPI);
	net = memnew(GDNetAPI);
	http = memnew(GDHTTPAPI);
	web = memnew(GDWebAPI);
	file = memnew(GDFSAPI);
	collection = memnew(GDCollectionsAPI);
	data = memnew(GDCodecAPI);
	id = memnew(GDIDAPI);
	text = memnew(GDTextAPI);
	html = memnew(GDHTMLAPI);
	math = memnew(GDMathAPI);
	version = memnew(GDSemanticVersionAPI);
	time = memnew(GDDateTimeAPI);
	cli = memnew(GDCLIAPI);
	test = memnew(GDTestAPI);
	database = memnew(GDDatabaseAPI);
}

// Destroy child APIs in reverse construction order.
GDAPI::~GDAPI() {
	memdelete(database);
	memdelete(test);
	memdelete(cli);
	memdelete(time);
	memdelete(version);
	memdelete(math);
	memdelete(html);
	memdelete(text);
	memdelete(id);
	memdelete(data);
	memdelete(collection);
	memdelete(file);
	memdelete(web);
	memdelete(http);
	memdelete(net);
	memdelete(log);
	memdelete(async);
}

// Expose child APIs as GD properties.
void GDAPI::_bind_methods() {
#define GD_CHILD(m_name, m_type) \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &GDAPI::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, #m_name, PROPERTY_HINT_NONE, #m_type), "", "get_" #m_name)
	GD_CHILD(async, GDAsyncAPI);
	GD_CHILD(log, GDLogAPI);
	GD_CHILD(net, GDNetAPI);
	GD_CHILD(http, GDHTTPAPI);
	GD_CHILD(web, GDWebAPI);
	GD_CHILD(file, GDFSAPI);
	GD_CHILD(collection, GDCollectionsAPI);
	GD_CHILD(data, GDCodecAPI);
	GD_CHILD(id, GDIDAPI);
	GD_CHILD(text, GDTextAPI);
	GD_CHILD(html, GDHTMLAPI);
	GD_CHILD(math, GDMathAPI);
	GD_CHILD(version, GDSemanticVersionAPI);
	GD_CHILD(time, GDDateTimeAPI);
	GD_CHILD(cli, GDCLIAPI);
	GD_CHILD(test, GDTestAPI);
	GD_CHILD(database, GDDatabaseAPI);
#undef GD_CHILD
}

// Expose the JWT verification completion signal.
void GDWebJwtCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Run the optional callable on the main thread after cryptographic verification.
Signal GDWebJwtCall::start(const String &p_token, const Variant &p_key, const Dictionary &p_opts) {
	Ref<GDWebJwtCall> call;
	call.instantiate();
	call->self_hold = call;
	Dictionary opts = p_opts.duplicate();
	call->has_check = opts.has("check");
	call->check = opts.get("check", Variant());
	opts.erase("check");
	const Variant key = p_key;
	const Signal signal(call.ptr(), "finished");
	GDValueCall::start([p_token, key, opts]() -> Variant { return GDWebApp::jwt_verify(p_token, key, opts); }).connect(
			callable_mp(call.ptr(), &GDWebJwtCall::verified), Object::CONNECT_ONE_SHOT);
	return signal;
}

// Apply revocation checks to the worker result and return the final outcome.
void GDWebJwtCall::verified(const Variant &p_result) {
	Ref<GDWebJwtCall> keep(this);
	Ref<R> out = p_result;
	if (out.is_null()) {
		out = R::err("JWT worker returned no result", Err::INVALID_DATA);
	} else if (out->get_ok() && has_check) {
		if (check.get_type() != Variant::CALLABLE || !Callable(check).is_valid()) {
			out = R::err("JWT check must be a valid Callable", Err::INVALID_DATA);
		} else {
			const Callable fn = check;
			const Variant arg = out->get_v();
			const Variant *args[] = { &arg };
			Variant accepted;
			Callable::CallError err;
			fn.callp(args, 1, accepted, err);
			if (err.error != Callable::CallError::CALL_OK || accepted.get_type() != Variant::BOOL || !(bool)accepted) {
				out = R::err("JWT was revoked", Err::UNAUTHENTICATED);
			}
		}
	}
	check = Variant();
	emit_signal("finished", out);
	self_hold.unref();
}

// Create a routed web application.
Ref<GDWebApp> GDWebAPI::app() const {
	Ref<GDWebApp> out;
	out.instantiate();
	return out;
}

// Create an incremental response without buffering the producer's complete output.
Dictionary GDWebAPI::stream(const Callable &p_next, int64_t p_length, const String &p_type, int64_t p_status) const {
	return GDWebWriter::reply(p_next, p_length, p_type, p_status);
}

// Create a raw HTTP server.
Ref<GDWebServer> GDWebAPI::server() const {
	Ref<GDWebServer> out;
	out.instantiate();
	return out;
}

// Move template loading and rendering off the serve main loop.
Signal GDWebAPI::view(const String &p_path, const Dictionary &p_data, int64_t p_status, const Callable &p_renderer) const {
	return GDWebViewCall::start(p_path, p_data, p_status, p_renderer);
}

// Complete short JSON locally and move only unfinished traversal off the main thread.
Variant GDWebAPI::json(const Variant &p_data, int64_t p_status) const {
	uint64_t until = GDScriptFunction::time_slice_deadline();
	if (!until && Thread::is_main_thread()) until = GDClock::usec() + GD_SCHED_SLICE_USEC;
	return Http::json_out(p_data, p_status, until);
}

// Create a cookie-session store and middleware.
Ref<GDWebSessionStore> GDWebAPI::sessions(const Dictionary &p_opts) const {
	return GDWebApp::sessions(p_opts.get("total", 1024), p_opts.get("per_user", 3), p_opts.get("idle", 1800), p_opts.get("life", 43200), p_opts.get("cookie", "sid"), p_opts.get("keep", "user"));
}

// Create a per-key fixed-window rate limiter.
Ref<GDWebMiddleware> GDWebAPI::rate(const Dictionary &p_opts) const {
	return GDWebRateLimit::make(p_opts);
}

// Create a text-length validation rule.
Dictionary GDWebAPI::text_rule(const Dictionary &p_opts) const {
	return GDWebApp::rule_text(p_opts.get("min", 0), p_opts.get("max", 4096));
}

// Create an integer-range validation rule.
Dictionary GDWebAPI::int_rule(const Dictionary &p_opts) const {
	return GDWebApp::rule_integer(p_opts.get("min", INT64_MIN), p_opts.get("max", INT64_MAX));
}

// Create a numeric-range validation rule.
Dictionary GDWebAPI::number_rule(const Dictionary &p_opts) const {
	return GDWebApp::rule_number(p_opts.get("min", -1e308), p_opts.get("max", 1e308));
}

// Create an array item and length validation rule.
Dictionary GDWebAPI::list_rule(const Dictionary &p_item, const Dictionary &p_opts) const {
	return GDWebApp::rule_list(p_item, p_opts.get("min", 0), p_opts.get("max", 1024));
}

// Create an object-field validation rule.
Dictionary GDWebAPI::object_rule(const Dictionary &p_fields, const Dictionary &p_opts) const {
	return GDWebApp::rule_object(p_fields, p_opts.get("extra", false));
}

// Load a template on a worker and apply the optional renderer before returning a result.
Signal GDWebAPI::view_async(const String &p_path, const Dictionary &p_data, int64_t p_status, const Callable &p_renderer) const {
	return GDWebViewCall::start(p_path, p_data, p_status, p_renderer);
}

// Create large JSON responses on a CPU worker.
Signal GDWebAPI::json_async(const Variant &p_data, int64_t p_status) const {
	const Variant data = p_data;
	return GDValueCall::start([data, p_status]() -> Variant { return Http::json_out(data, p_status); });
}

// Sign JWT claims on a CPU worker.
Signal GDWebAPI::jwt_sign_async(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts) const {
	const Dictionary claims = p_claims;
	const Variant key = p_key;
	const Dictionary opts = p_opts;
	return GDValueCall::start([claims, key, opts]() -> Variant { return GDWebApp::jwt_sign(claims, key, opts); });
}

// Verify a JWT on a CPU worker.
Signal GDWebAPI::jwt_verify_async(const String &p_token, const Variant &p_key, const Dictionary &p_opts) const {
	return GDWebJwtCall::start(p_token, p_key, p_opts);
}

// Dispatch nested input validation to a CPU worker.
Signal GDWebAPI::validate_async(const Variant &p_value, const Dictionary &p_rule) const {
	const Variant value = p_value;
	const Dictionary rule = p_rule;
	return GDValueCall::start([value, rule]() -> Variant { return GDWebApp::validate(value, rule); });
}

// Expose web application, response, and middleware APIs to script.
void GDWebAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("app"), &GDWebAPI::app);
	ClassDB::bind_method(D_METHOD("server"), &GDWebAPI::server);
	ClassDB::bind_method(D_METHOD("text", "body", "status"), &GDWebAPI::text, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("html", "body", "status"), &GDWebAPI::html, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("json", "data", "status"), &GDWebAPI::json, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("bytes", "body", "type", "status"), &GDWebAPI::bytes, DEFVAL("application/octet-stream"), DEFVAL(200));
	ClassDB::bind_method(D_METHOD("stream", "producer", "length", "type", "status"), &GDWebAPI::stream, DEFVAL(-1), DEFVAL("application/octet-stream"), DEFVAL(200));
	ClassDB::bind_method(D_METHOD("redirect", "to", "status", "away"), &GDWebAPI::redirect, DEFVAL(302), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("guard", "reply"), &GDWebAPI::guard);
	ClassDB::bind_method(D_METHOD("not_found", "msg"), &GDWebAPI::not_found, DEFVAL("Not Found"));
	ClassDB::bind_method(D_METHOD("header", "reply", "name", "value"), &GDWebAPI::header);
	ClassDB::bind_method(D_METHOD("add_header", "reply", "name", "value"), &GDWebAPI::add_header);
	ClassDB::bind_method(D_METHOD("view", "path", "data", "status", "renderer"), &GDWebAPI::view, DEFVAL(Dictionary()), DEFVAL(200), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("view_async", "path", "data", "status", "renderer"), &GDWebAPI::view_async, DEFVAL(Dictionary()), DEFVAL(200), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("json_async", "data", "status"), &GDWebAPI::json_async, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("error_status", "error"), &GDWebAPI::error_status);
	ClassDB::bind_method(D_METHOD("jwt", "key", "opts"), &GDWebAPI::jwt, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("jwt_sign", "claims", "key", "opts"), &GDWebAPI::jwt_sign, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("jwt_verify", "token", "key", "opts"), &GDWebAPI::jwt_verify, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("jwt_sign_async", "claims", "key", "opts"), &GDWebAPI::jwt_sign_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("jwt_verify_async", "token", "key", "opts"), &GDWebAPI::jwt_verify_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("csrf", "opts"), &GDWebAPI::csrf, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("sessions", "opts"), &GDWebAPI::sessions, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("rate", "opts"), &GDWebAPI::rate, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("text_rule", "opts"), &GDWebAPI::text_rule, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("int_rule", "opts"), &GDWebAPI::int_rule, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("number_rule", "opts"), &GDWebAPI::number_rule, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("bool_rule"), &GDWebAPI::bool_rule);
	ClassDB::bind_method(D_METHOD("list_rule", "item", "opts"), &GDWebAPI::list_rule, DEFVAL(Dictionary()), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("object_rule", "fields", "opts"), &GDWebAPI::object_rule, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("optional", "rule", "fallback"), &GDWebAPI::optional, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("one_of", "values"), &GDWebAPI::one_of);
	ClassDB::bind_method(D_METHOD("validate", "value", "rule"), &GDWebAPI::validate);
	ClassDB::bind_method(D_METHOD("validate_async", "value", "rule"), &GDWebAPI::validate_async);
	ClassDB::bind_method(D_METHOD("json_body", "rule", "name"), &GDWebAPI::json_body, DEFVAL("body"));
	ClassDB::bind_method(D_METHOD("query", "rule", "name"), &GDWebAPI::query, DEFVAL("query"));
	ClassDB::bind_method(D_METHOD("params", "rule", "name"), &GDWebAPI::params, DEFVAL("params"));
	ADD_RESULT("jwt_sign", "String");
	ADD_AWAIT("view", "R:Dictionary");
	ADD_AWAIT("json", "R:Dictionary");
	ADD_AWAIT("view_async", "R:Dictionary");
	ADD_AWAIT("json_async", "R:Dictionary");
	ADD_AWAIT("jwt_sign_async", "R:String");
	ADD_AWAIT("jwt_verify_async", "R:Dictionary");
	ADD_AWAIT("validate_async", "R:Variant");
	ADD_RESULT("jwt_verify", "Dictionary");
	ADD_RESULT("validate", "Variant");
	ADD_AUTO_WAIT("view");
	ADD_AUTO_WAIT("json");
	List<MethodInfo> methods;
	ClassDB::get_method_list(get_class_static(), &methods, true);
	for (const MethodInfo &method : methods) {
		ClassDB::set_method_group(get_class_static(), method.name, web_group(method.name));
	}
}

namespace {

struct Entry {
	const char *name;
	Object *object;
	void (*free)(Object *);
};

LocalVector<Entry> entries;

// Create registered instances and associate public names with internal types.
template <typename T>
// Release an Object safely.
void free_object(Object *p_object) {
	memdelete(static_cast<T *>(p_object));
}

template <typename T>
// Register a named singleton instance.
void add(const char *p_name) {
	T *object = memnew(T);
	entries.push_back({ p_name, object, &free_object<T> });
	Engine::get_singleton()->add_singleton(Engine::Singleton(p_name, object, T::get_class_static()));
}

} // namespace

// Register the standard singleton.
void register_cli_singletons() {
	add<GDAPI>("GD");
}

// Unregister the standard singleton.
void unregister_cli_singletons() {
	for (int i = entries.size() - 1; i >= 0; i--) {
		Engine::get_singleton()->remove_singleton(entries[i].name);
		entries[i].free(entries[i].object);
	}
	entries.clear();
}
