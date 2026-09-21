/**************************************************************************/
/*  singleton.h                                                          */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once
#include "cli/data/gzip_writer.h"

// Expose shared APIs and state through registered singleton instances.

#include "cli/api/box.h"
#include "cli/api/cli.h"
#include "cli/api/gen.h"
#include "cli/api/math.h"
#include "cli/api/text.h"
#include "cli/data/codec.h"
#include "cli/data/format.h"
#include "cli/data/json.h"
#include "cli/db/database.h"
#include "cli/db/pg.h"
#include "cli/db/redis.h"
#include "cli/db/sqlite.h"
#include "cli/net/serve.h"
#include "cli/net/socket.h"
#include "cli/sys/os.h"
#include "cli/sys/file_stream.h"
#include "cli/sys/perm.h"
#include "cli/sys/task.h"
#include "cli/sys/thread_limit.h"

#include "core/object/object.h"

// Group asynchronous operations under one global entry point.
class GDAsyncAPI : public Object {
	GDCLASS(GDAsyncAPI, Object);

protected:
	// Expose asynchronous APIs to script.
	static void _bind_methods();

public:
	// Set the runtime-wide managed-thread limit and return the previous value.
	int64_t set_max_threads(int64_t p_max) { return GDThreadLimit::set(p_max); }
	// Return a signal emitted after the specified delay.
	Signal sleep(double p_sec) { return Async::sleep(p_sec); }
	// Start a callable and return its completion signal.
	Signal spawn(const Callable &p_fn) { return Async::spawn(p_fn); }
	// Start supplied Callables and collect their results and supplied Signals in input order.
	Signal all(const Array &p_signals) { return Async::all(p_signals); }
	// Return the index of the first completed signal.
	Signal race(const Array &p_signals) { return Async::race(p_signals); }
	// Wait for an operation or its deadline, whichever comes first.
	Signal with_timeout(const Signal &p_signal, double p_sec) { return Async::with_timeout(p_signal, p_sec); }
	// Propagate context cancellation to an operation.
	Signal with_context(const Ref<GDAsyncContext> &p_context, const Signal &p_signal) { return Async::with_context(p_context, p_signal); }
	// Create a context that shares a cancellation reason.
	Ref<GDAsyncContext> context() { return Async::ctx(); }
};

// Provide one global entry point for the default logger.
class GDLogAPI : public Object {
	GDCLASS(GDLogAPI, Object);

	LogState log;

protected:
	// Expose logging APIs to script.
	static void _bind_methods();

public:
	// Set the logger name and minimum level.
	void setup(const String &p_name, const String &p_level);
	// Set the file receiving appended log lines.
	void set_file(const String &p_path) { log.set_file(p_path); }
	// Configure whether log lines include a timestamp.
	void set_time(bool p_on) { log.set_time(p_on); }
	// Configure terminal color output.
	void set_color(bool p_on) { log.set_color(p_on); }
	// Write a line at a level selected by name.
	Signal write(const String &p_level, const String &p_msg, const Variant &p_extra = Variant()) { return log.write(LogState::level_of(p_level), p_msg, p_extra); }
	// Write a debug-level line.
	Signal debug(const String &p_msg, const Variant &p_extra = Variant()) { return log.debug(p_msg, p_extra); }
	// Write an info-level line.
	Signal info(const String &p_msg, const Variant &p_extra = Variant()) { return log.info(p_msg, p_extra); }
	// Write a warning-level line.
	Signal warn(const String &p_msg, const Variant &p_extra = Variant()) { return log.warn(p_msg, p_extra); }
	// Write an error-level line.
	Signal error(const String &p_msg, const Variant &p_extra = Variant()) { return log.error(p_msg, p_extra); }
	// Log the contents of a failed Result.
	Signal result(const Ref<R> &p_r, const String &p_msg) { return log.result(p_r, p_msg); }
	// Format a log line without writing it.
	String format(const String &p_level, const String &p_msg, const Variant &p_extra = Variant()) const;
	// Return a signal after preceding file output completes.
	Signal flush() const;
	Signal flush_async() const { return flush(); }
};

// Group destination-name and port discovery helpers.
class GDNetAPI : public Object {
	GDCLASS(GDNetAPI, Object);

protected:
	// Expose network APIs to script.
	static void _bind_methods();

public:
	// Check whether a host port is available for listening.
	bool is_free(int64_t p_port, const String &p_host) { return Net::is_free(p_port, p_host); }
	// Let the kernel choose for zero, or search from the specified positive port.
	Ref<R> free_port(int64_t p_from, const String &p_host) { return Net::free_port(p_from, p_host); }
	// Resolve a hostname outside the event loop.
	Signal resolve(const String &p_host);
	Signal resolve_async(const String &p_host) { return resolve(p_host); }
	// Open a TCP connection while suspending only the caller during connection setup.
	Signal dial_tcp(const String &p_host, int64_t p_port, const Dictionary &p_opts) { return GDTCPDialCall::start(p_host, p_port, p_opts); }
	Signal dial_tcp_async(const String &p_host, int64_t p_port, const Dictionary &p_opts) { return dial_tcp(p_host, p_port, p_opts); }
	// Open a verified TLS connection while suspending only the caller during setup.
	Signal dial_tls(const String &p_host, int64_t p_port, const Dictionary &p_opts) { return GDTLSDialCall::start(p_host, p_port, p_opts); }
	Signal dial_tls_async(const String &p_host, int64_t p_port, const Dictionary &p_opts) { return dial_tls(p_host, p_port, p_opts); }
	// Open TCP listeners and UDP packet connections.
	Ref<R> listen_tcp(const String &p_host, int64_t p_port) { return GDTCPListener::listen(p_host, p_port); }
	Ref<R> listen_udp(const String &p_host, int64_t p_port, int64_t p_buffer) { return GDUDPPacketConn::listen(p_host, p_port, p_buffer); }
	// Dispatch port checks and discovery to I/O workers.
	Signal is_free_async(int64_t p_port, const String &p_host);
	Signal free_port_async(int64_t p_from, const String &p_host);
	// Enumerate this machine's IP addresses.
	Signal local_addresses() { return local_addresses_async(); } // Run enumeration on a worker and return either addresses or an error.
	Signal local_addresses_async();
	// Check whether text is an IP address.
	bool is_ip(const String &p_text) { return Net::is_ip(p_text); }
	// Split a host and port.
	Dictionary split_host(const String &p_text, int64_t p_default_port) { return Net::split_host(p_text, p_default_port); }
};

// Group HTTP transfer, URL, and media-type helpers under one global entry point.
class GDHTTPAPI : public Object {
	GDCLASS(GDHTTPAPI, Object);

	Ref<GDHTTPTransport> transport; // Transport sharing keep-alive connections across requests.

protected:
	// Expose HTTP APIs to script.
	static void _bind_methods();

public:
	GDHTTPAPI() { transport.instantiate(); }
	// Send an asynchronous HTTP request to a URL.
	Signal fetch(const String &p_url, const Dictionary &p_opts) { return Http::fetch(p_url, p_opts, transport); }
	Signal fetch_async(const String &p_url, const Dictionary &p_opts) { return fetch(p_url, p_opts); }
	// Parse URL text into a component dictionary.
	Ref<R> parse_url(const String &p_raw) { return Url::parse(p_raw); }
	Signal parse_url_async(const String &p_raw);
	// Format URL components as text.
	String build_url(const Dictionary &p_url) { return Url::build(p_url); }
	Signal build_url_async(const Dictionary &p_url);
	// Build an HTTP request target from URL components.
	String request_target(const Dictionary &p_url) { return Url::request_target(p_url); }
	Signal request_target_async(const Dictionary &p_url);
	// Return a scheme's default port.
	int default_port(const String &p_scheme) { return Url::default_port(p_scheme); }
	// Parse query text into a dictionary.
	Ref<R> decode_query(const String &p_raw) { return Url::decode_query(p_raw); }
	Signal decode_query_async(const String &p_raw);
	// Encode a query dictionary as text.
	String encode_query(const Dictionary &p_query) { return Url::encode_query(p_query); }
	Signal encode_query_async(const Dictionary &p_query);
	// Select a media type from a filename.
	String media_type(const String &p_path) { return Media::by_path(p_path); }
	// Select a media type from an extension.
	String media_type_for_extension(const String &p_ext) { return Media::by_extension(p_ext); }
	// Check whether a media type is textual.
	bool is_textual(const String &p_kind) { return Media::is_textual(p_kind); }
	// Return the extension corresponding to a media type.
	String extension_for_media_type(const String &p_kind) { return Media::extension(p_kind); }
};

// Group file operations and safe path helpers under one global entry point.
class GDFSAPI : public Object {
	GDCLASS(GDFSAPI, Object);

protected:
	// Expose file APIs to script.
	static void _bind_methods();

public:
	// Open a regular file as a sequential read-write stream.
	Signal open(const String &p_path, const String &p_mode) { return GDFileStream::open(p_path, p_mode); }
	Signal open_async(const String &p_path, const String &p_mode) { return open(p_path, p_mode); }
	// Remove a text file only if its contents remain unchanged.
	Ref<R> _remove_text(const String &p_path, const String &p_old) { return Os::remove_text(p_path, p_old); }
	// Get the internal lock serializing multi-operation file transactions.
	Ref<R> _lock(const String &p_path) { return Os::lock_file(p_path); }
	// Mount one package checkout as a private read-only source.
	String _local(const String &p_path);
	String _local_stamp(const String &p_path);
	// Isolate regular-file operations on workers because they are not kernel-pollable.
	Signal read_text_async(const String &p_path);
	Signal read_bytes_async(const String &p_path, int64_t p_offset, int64_t p_max);
	Signal write_text_async(const String &p_path, const String &p_body);
	Signal write_bytes_async(const String &p_path, const PackedByteArray &p_body);
	Signal append_text_async(const String &p_path, const String &p_body);
	Signal append_bytes_async(const String &p_path, const PackedByteArray &p_body);
	Signal copy_async(const String &p_src, const String &p_dst);
	Signal replace_text_async(const String &p_path, const Variant &p_old, const String &p_body);
	Signal exists_async(const String &p_path);
	Signal remove_async(const String &p_path);
	Signal size_of_async(const String &p_path);
	Signal rename_async(const String &p_src, const String &p_dst);
	Signal list_dir_async(const String &p_path);
	Signal make_dir_async(const String &p_path);
	Signal ensure_dir_async(const String &p_path);
	Signal walk_async(const String &p_path, bool p_want_dirs, bool p_hidden);
	Signal glob_async(const String &p_path, const String &p_pattern, bool p_hidden);
	Signal remove_all_async(const String &p_path);
	Signal can_read_async(const String &p_path);
	Signal create_tar_async(const String &p_root);
	Signal extract_tar_async(const PackedByteArray &p_data, const String &p_root);
	// Parse a path into a component dictionary.
	Dictionary parse_path(const String &p_path) { return Path::parse(p_path); }
	// Format path components as text.
	String format_path(const Dictionary &p_parts) { return Path::format(p_parts); }
	// Join path segments with separators.
	String join(const PackedStringArray &p_parts) { return Path::join(p_parts); }
	// Return the parent directory of a path.
	String dirname(const String &p_path) { return Path::dirname(p_path); }
	// Return the final path component with the specified suffix removed.
	String basename(const String &p_path, const String &p_suffix) { return Path::basename(p_path, p_suffix); }
	// Return a path's extension.
	String extname(const String &p_path) { return Path::extname(p_path); }
	// Check whether a path is absolute.
	bool is_absolute(const String &p_path) { return Path::is_absolute(p_path); }
	// Normalize redundant separators and parent/current-directory segments.
	String normalize(const String &p_path) { return Path::normalize(p_path); }
	// Return the relative route between two paths.
	String relative(const String &p_from, const String &p_to) { return Path::relative(p_from, p_to); }
	// Resolve a name safely within a directory.
	String under(const String &p_dir, const String &p_name) { return Path::under(p_dir, p_name); }

	// Read document files asynchronously and parse them on workers.
	Signal read_csv_async(const String &p_path, const String &p_sep);
	Signal read_ini_async(const String &p_path);
	Signal read_toml_async(const String &p_path);
	Signal read_yaml_async(const String &p_path);
	Signal read_jsonc_async(const String &p_path);
	Signal read_jsonl_async(const String &p_path);
	Signal read_front_matter_async(const String &p_path);
	Signal read_xml_async(const String &p_path);
	Signal read_env_async(const String &p_path);
	Signal read_tar_async(const String &p_path);
};

// Group collection operations and stateful container factories.
class GDCollectionsAPI : public Object {
	GDCLASS(GDCollectionsAPI, Object);

protected:
	// Expose collection APIs to script.
	static void _bind_methods();

public:
	// Group values by selected keys.
	Dictionary group_by(const Array &p_items, const Callable &p_key) { return Coll::group_by(p_items, p_key); }
	// Transform each dictionary value.
	Dictionary map_values(const Dictionary &p_src, const Callable &p_fn) { return Coll::map_values(p_src, p_fn); }
	// Keep dictionary keys matching a predicate.
	Dictionary filter_keys(const Dictionary &p_src, const Callable &p_pred) { return Coll::filter_keys(p_src, p_pred); }
	// Partition an array by predicate result.
	Array partition(const Array &p_items, const Callable &p_pred) { return Coll::partition(p_items, p_pred); }
	// Split an array into chunks of the specified size.
	Array chunk(const Array &p_items, int p_size) { return Coll::chunk(p_items, p_size); }
	// Remove values with duplicate selected keys.
	Array unique_by(const Array &p_items, const Callable &p_key) { return Coll::unique_by(p_items, p_key); }
	// Remove duplicate values.
	Array unique(const Array &p_items) { return Coll::unique(p_items); }
	// Sort by selected values.
	Array sort_by(const Array &p_items, const Callable &p_pick) { return Coll::sort_by(p_items, p_pick); }
	// Sort by specified dictionary keys.
	Array sort_key(const Array &p_items, const String &p_key) { return Coll::sort_key(p_items, p_key); }
	// Pair elements at corresponding positions in two arrays.
	Array zip(const Array &p_a, const Array &p_b) { return Coll::zip(p_a, p_b); }
	// Sum selected numeric values.
	double sum_of(const Array &p_items, const Callable &p_pick) { return Coll::sum_of(p_items, p_pick); }
	// Return the element with the maximum selected value.
	Variant max_by(const Array &p_items, const Callable &p_pick) { return Coll::max_by(p_items, p_pick); }
	// Return the element with the minimum selected value.
	Variant min_by(const Array &p_items, const Callable &p_pick) { return Coll::min_by(p_items, p_pick); }
	// Index elements in a dictionary by selected keys.
	Dictionary index_by(const Array &p_items, const Callable &p_key) { return Coll::index_by(p_items, p_key); }
	// Merge nested dictionaries recursively.
	Dictionary deep_merge(const Dictionary &p_base, const Dictionary &p_over) { return Coll::deep_merge(p_base, p_over); }
	// Create a binary heap with selectable priorities.
	Ref<GDBinaryHeap> binary_heap(const Callable &p_pick);
	// Create a priority queue.
	Ref<GDPriorityQueue> priority_queue();
	// Create an LRU cache with entry-count and expiry settings.
	Ref<GDLRUCache> lru_cache(int p_limit, uint64_t p_ttl_ms);
	// Create an argument-keyed cache of callable results.
	Ref<GDMemoizedCallable> memo(const Callable &p_fn, int p_limit);
	// Dispatch large collection operations without callables to CPU workers.
	Signal group_by_async(const Array &p_items, const Callable &p_key);
	Signal map_values_async(const Dictionary &p_src, const Callable &p_fn);
	Signal filter_keys_async(const Dictionary &p_src, const Callable &p_pred);
	Signal partition_async(const Array &p_items, const Callable &p_pred);
	Signal unique_by_async(const Array &p_items, const Callable &p_key);
	Signal sort_by_async(const Array &p_items, const Callable &p_pick);
	Signal sum_of_async(const Array &p_items, const Callable &p_pick);
	Signal max_by_async(const Array &p_items, const Callable &p_pick);
	Signal min_by_async(const Array &p_items, const Callable &p_pick);
	Signal index_by_async(const Array &p_items, const Callable &p_key);
	Signal chunk_async(const Array &p_items, int p_size);
	Signal unique_async(const Array &p_items);
	Signal sort_key_async(const Array &p_items, const String &p_key);
	Signal zip_async(const Array &p_a, const Array &p_b);
	Signal deep_merge_async(const Dictionary &p_base, const Dictionary &p_over);
};

// Group binary encoding and byte-sequence helpers under one global entry point.
class GDCodecAPI : public Object {
	GDCLASS(GDCodecAPI, Object);

protected:
	// Expose encoding APIs to script.
	static void _bind_methods();

public:
	Ref<R> gzip_writer(const Ref<RefCounted> &p_writer, int64_t p_level) { return GDGzipWriter::create(p_writer, p_level); } // Create a compressor writing to a byte destination.
	// Read and write in-memory document formats.
	Ref<R> csv(const String &p_src, const String &p_sep) { return Csv::parse(p_src, p_sep); }
	String to_csv(const Array &p_rows, const String &p_sep) { return Csv::stringify(p_rows, p_sep); }
	Ref<R> csv_objects(const String &p_src, const String &p_sep) { return Csv::parse_objects(p_src, p_sep); }
	String to_csv_objects(const Array &p_items, const String &p_sep) { return Csv::stringify_objects(p_items, p_sep); }
	Ref<R> ini(const String &p_src) { return Ini::parse(p_src); }
	String to_ini(const Dictionary &p_data) { return Ini::stringify(p_data); }
	Ref<R> toml(const String &p_src) { return Toml::parse(p_src); }
	Ref<R> to_toml(const Dictionary &p_data, const String &p_prefix) { return Toml::stringify(p_data, p_prefix); }
	Ref<R> yaml(const String &p_src) { return Yaml::parse(p_src); }
	Ref<R> to_yaml(const Variant &p_data, int p_depth) { return Yaml::stringify(p_data, p_depth); }
	Ref<R> jsonc(const String &p_src) { return Jsonc::parse(p_src); }
	String strip_jsonc(const String &p_src) { return Jsonc::strip(p_src); }
	Ref<R> jsonl(const String &p_src) { return Jsonl::parse(p_src); }
	Ref<R> to_jsonl(const Array &p_items) { return Jsonl::stringify(p_items); }
	Ref<GDJSONLReader> jsonl_reader();
	Ref<R> front_matter(const String &p_src) { return Front::parse(p_src); }
	bool has_front_matter(const String &p_src) { return Front::has(p_src); }
	Ref<R> to_front_matter(const Dictionary &p_attrs, const String &p_body, const String &p_kind) { return Front::stringify(p_attrs, p_body, p_kind); }
	Ref<R> xml(const String &p_src) { return Xml::parse(p_src); }
	Ref<R> to_xml(const Dictionary &p_data, int p_indent) { return Xml::stringify(p_data, p_indent); }
	Ref<R> env(const String &p_src) { return Dotenv::parse(p_src); }
	Ref<R> to_env(const Dictionary &p_data) { return Dotenv::stringify(p_data); }
	Ref<R> tar(const Array &p_entries) { return Tar::pack(p_entries); }
	Ref<R> untar(const PackedByteArray &p_data) { return Tar::unpack(p_data); }
	// Encode a value as strict JSON UTF-8 bytes.
	Ref<R> json_encode(const Variant &p_value, const Dictionary &p_opts) { return JsonData::encode(p_value, p_opts); }
	// Decode bytes as strict JSON.
	Ref<R> json_decode(const PackedByteArray &p_data) { return JsonData::decode(p_data); }
	// Encode bytes as hexadecimal text.
	String hex_encode(const PackedByteArray &p_data) { return Encoding::hex_encode(p_data); }
	// Decode hexadecimal text into bytes.
	Ref<R> hex_decode(const String &p_text) { return Encoding::hex_decode(p_text); }
	// Encode bytes as Base64 text.
	String base64_encode(const PackedByteArray &p_data) { return Encoding::base64_encode(p_data); }
	// Decode Base64 text into bytes.
	Ref<R> base64_decode(const String &p_text, bool p_raw = false) { return Encoding::base64_decode(p_text, p_raw); }
	// Encode bytes as URL-safe Base64 text.
	String base64url_encode(const PackedByteArray &p_data) { return Encoding::base64url_encode(p_data); }
	// Decode URL-safe Base64 text into bytes.
	Ref<R> base64url_decode(const String &p_text, bool p_raw = true) { return Encoding::base64url_decode(p_text, p_raw); }
	// Encode bytes as Base32 text.
	String base32_encode(const PackedByteArray &p_data) { return Encoding::base32_encode(p_data); }
	// Decode Base32 text into bytes.
	Ref<R> base32_decode(const String &p_text, bool p_raw = false) { return Encoding::base32_decode(p_text, p_raw); }
	// Encode an integer as variable-length bytes.
	PackedByteArray varint_encode(int64_t p_n) { return Encoding::varint_encode(p_n); }
	// Read an integer from variable-length bytes.
	Ref<R> varint_decode(const PackedByteArray &p_data, int p_at) { return Encoding::varint_decode(p_data, p_at); }
	// Concatenate byte sequences.
	PackedByteArray concat(const Array &p_parts) { return Bytes::concat(p_parts); }
	// Check whether two byte sequences are equal.
	bool equals(const PackedByteArray &p_a, const PackedByteArray &p_b) { return Bytes::equals(p_a, p_b); }
	// Check whether bytes contain a subsequence.
	bool includes(const PackedByteArray &p_hay, const PackedByteArray &p_needle) { return Bytes::includes(p_hay, p_needle); }
	// Return the first byte-subsequence position.
	int index_of(const PackedByteArray &p_hay, const PackedByteArray &p_needle, int p_from) { return Bytes::index_of(p_hay, p_needle, p_from); }
	// Return the last byte-subsequence position.
	int last_index_of(const PackedByteArray &p_hay, const PackedByteArray &p_needle) { return Bytes::last_index_of(p_hay, p_needle); }
	// Check whether bytes begin with a prefix.
	bool starts_with(const PackedByteArray &p_hay, const PackedByteArray &p_prefix) { return Bytes::starts_with(p_hay, p_prefix); }
	// Check whether bytes end with a suffix.
	bool ends_with(const PackedByteArray &p_hay, const PackedByteArray &p_suffix) { return Bytes::ends_with(p_hay, p_suffix); }
	// Repeat a byte sequence the specified number of times.
	PackedByteArray repeat(const PackedByteArray &p_src, int p_times) { return Bytes::repeat(p_src, p_times); }
	// Resize a byte sequence to the specified length.
	PackedByteArray fit(const PackedByteArray &p_src, int p_size) { return Bytes::fit(p_src, p_size); }
	// Split bytes on a delimiter sequence.
	Array split(const PackedByteArray &p_src, const PackedByteArray &p_sep) { return Bytes::split(p_src, p_sep); }
	// Encode a value as MessagePack.
	Ref<R> msgpack(const Variant &p_value) { return Msgpack::encode(p_value); }
	// Decode MessagePack into a value.
	Ref<R> unmsgpack(const PackedByteArray &p_data) { return Msgpack::decode(p_data); }
	// Encode a value as CBOR.
	Ref<R> cbor(const Variant &p_value) { return Cbor::encode(p_value); }
	// Decode CBOR into a value.
	Ref<R> uncbor(const PackedByteArray &p_data) { return Cbor::decode(p_data); }
	// Compute a SHA-224 digest.
	PackedByteArray sha224(const PackedByteArray &p_msg) { return Hash::sha224(p_msg); }
	// Compute a SHA-256 digest.
	PackedByteArray sha256(const PackedByteArray &p_msg) { return Hash::sha256(p_msg); }
	// Compute a SHA-384 digest.
	PackedByteArray sha384(const PackedByteArray &p_msg) { return Hash::sha384(p_msg); }
	// Compute a SHA-512 digest.
	PackedByteArray sha512(const PackedByteArray &p_msg) { return Hash::sha512(p_msg); }
	// Compute a SHA3-224 digest.
	PackedByteArray sha3_224(const PackedByteArray &p_msg) { return Hash::sha3_224(p_msg); }
	// Compute a SHA3-256 digest.
	PackedByteArray sha3_256(const PackedByteArray &p_msg) { return Hash::sha3_256(p_msg); }
	// Compute a SHA3-384 digest.
	PackedByteArray sha3_384(const PackedByteArray &p_msg) { return Hash::sha3_384(p_msg); }
	// Compute a SHA3-512 digest.
	PackedByteArray sha3_512(const PackedByteArray &p_msg) { return Hash::sha3_512(p_msg); }
	// Compute a SHA-1 digest.
	PackedByteArray sha1(const PackedByteArray &p_msg) { return Hash::sha1(p_msg); }
	// Compute HMAC with the selected hash.
	Ref<R> hmac(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_msg) { return Hash::hmac(p_hash, p_key, p_msg); }
	// Compute HMAC-SHA-256.
	PackedByteArray hmac_sha256(const PackedByteArray &p_key, const PackedByteArray &p_msg) { return Hash::hmac_sha256(p_key, p_msg); }
	// Combine two byte sequences with exclusive OR.
	PackedByteArray xor_bytes(const PackedByteArray &p_a, const PackedByteArray &p_b) { return Hash::xor_bytes(p_a, p_b); }
	// Compare two byte sequences in constant time.
	bool equal_ct(const PackedByteArray &p_a, const PackedByteArray &p_b) { return Hash::equal_ct(p_a, p_b); }
	// Derive a key with PBKDF2-HMAC-SHA-256.
	Ref<R> pbkdf2_sha256(const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds) { return Hash::pbkdf2_sha256(p_pass, p_salt, p_rounds); }
	// Derive a PBKDF2 key with the selected hash and output length.
	Ref<R> pbkdf2(const String &p_hash, const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds, int64_t p_size) { return Hash::pbkdf2(p_hash, p_pass, p_salt, p_rounds, p_size); }
	// Derive an HKDF key from a secret, salt, and context information.
	Ref<R> hkdf(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt, const PackedByteArray &p_info, int64_t p_size) { return Hash::hkdf(p_hash, p_secret, p_salt, p_info, p_size); }
	// Extract an HKDF pseudorandom key from a secret and salt.
	Ref<R> hkdf_extract(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt) { return Hash::hkdf_extract(p_hash, p_secret, p_salt); }
	// Expand an HKDF key from a pseudorandom key and context information.
	Ref<R> hkdf_expand(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_info, int64_t p_size) { return Hash::hkdf_expand(p_hash, p_key, p_info, p_size); }
	// Run key derivation on CPU workers.
	Signal pbkdf2_sha256_async(const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds);
	Signal json_encode_async(const Variant &p_value, const Dictionary &p_opts);
	Signal json_decode_async(const PackedByteArray &p_data);
	Signal msgpack_async(const Variant &p_value);
	Signal unmsgpack_async(const PackedByteArray &p_data);
	Signal cbor_async(const Variant &p_value);
	Signal uncbor_async(const PackedByteArray &p_data);
	Signal sha224_async(const PackedByteArray &p_msg);
	Signal sha256_async(const PackedByteArray &p_msg);
	Signal sha384_async(const PackedByteArray &p_msg);
	Signal sha512_async(const PackedByteArray &p_msg);
	Signal sha3_224_async(const PackedByteArray &p_msg);
	Signal sha3_256_async(const PackedByteArray &p_msg);
	Signal sha3_384_async(const PackedByteArray &p_msg);
	Signal sha3_512_async(const PackedByteArray &p_msg);
	Signal sha1_async(const PackedByteArray &p_msg);
	Signal hmac_async(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_msg);
	Signal hmac_sha256_async(const PackedByteArray &p_key, const PackedByteArray &p_msg);
	Signal pbkdf2_async(const String &p_hash, const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds, int64_t p_size);
	Signal hkdf_async(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt, const PackedByteArray &p_info, int64_t p_size);
	Signal hkdf_extract_async(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt);
	Signal hkdf_expand_async(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_info, int64_t p_size);
	Signal hex_encode_async(const PackedByteArray &p_data);
	Signal hex_decode_async(const String &p_text);
	Signal base64_encode_async(const PackedByteArray &p_data);
	Signal base64_decode_async(const String &p_text, bool p_raw = false);
	Signal base64url_encode_async(const PackedByteArray &p_data);
	Signal base64url_decode_async(const String &p_text, bool p_raw = true);
	Signal base32_encode_async(const PackedByteArray &p_data);
	Signal base32_decode_async(const String &p_text, bool p_raw = false);
	Signal concat_async(const Array &p_parts);
	Signal equals_async(const PackedByteArray &p_a, const PackedByteArray &p_b);
	Signal includes_async(const PackedByteArray &p_hay, const PackedByteArray &p_needle);
	Signal index_of_async(const PackedByteArray &p_hay, const PackedByteArray &p_needle, int p_from);
	Signal last_index_of_async(const PackedByteArray &p_hay, const PackedByteArray &p_needle);
	Signal starts_with_async(const PackedByteArray &p_hay, const PackedByteArray &p_prefix);
	Signal ends_with_async(const PackedByteArray &p_hay, const PackedByteArray &p_suffix);
	Signal repeat_async(const PackedByteArray &p_src, int p_times);
	Signal fit_async(const PackedByteArray &p_src, int p_size);
	Signal split_async(const PackedByteArray &p_src, const PackedByteArray &p_sep);
	Signal xor_bytes_async(const PackedByteArray &p_a, const PackedByteArray &p_b);
	Signal equal_ct_async(const PackedByteArray &p_a, const PackedByteArray &p_b);
	// Dispatch in-memory format conversion to CPU workers.
	Signal csv_async(const String &p_src, const String &p_sep);
	Signal to_csv_async(const Array &p_rows, const String &p_sep);
	Signal csv_objects_async(const String &p_src, const String &p_sep);
	Signal to_csv_objects_async(const Array &p_items, const String &p_sep);
	Signal ini_async(const String &p_src);
	Signal to_ini_async(const Dictionary &p_data);
	Signal toml_async(const String &p_src);
	Signal to_toml_async(const Dictionary &p_data, const String &p_prefix);
	Signal yaml_async(const String &p_src);
	Signal to_yaml_async(const Variant &p_data, int p_depth);
	Signal jsonc_async(const String &p_src);
	Signal strip_jsonc_async(const String &p_src);
	Signal jsonl_async(const String &p_src);
	Signal to_jsonl_async(const Array &p_items);
	Signal front_matter_async(const String &p_src);
	Signal to_front_matter_async(const Dictionary &p_attrs, const String &p_body, const String &p_kind);
	Signal xml_async(const String &p_src);
	Signal to_xml_async(const Dictionary &p_data, int p_indent);
	Signal env_async(const String &p_src);
	Signal to_env_async(const Dictionary &p_data);
	Signal tar_async(const Array &p_entries);
	Signal untar_async(const PackedByteArray &p_data);
};

// Group time-ordered IDs and UUIDs under one global entry point.
class GDIDAPI : public Object {
	GDCLASS(GDIDAPI, Object);
	Ulid ulids;

protected:
	// Expose ID APIs to script.
	static void _bind_methods();

public:
	// Create a ULID for the specified time.
	String ulid(int64_t p_ms) { return ulids.make(p_ms); }
	// Check whether text is a ULID.
	bool is_ulid(const String &p_text) { return Ulid::is_valid(p_text); }
	// Read the timestamp from a ULID.
	Ref<R> ulid_time(const String &p_text) { return Ulid::time_of(p_text); }
	// Create a random version-4 UUID.
	String uuid() { return Uuid::v4(); }
	// Create a name-based version-5 UUID.
	Ref<R> uuid_v5(const String &p_space, const String &p_name) { return Uuid::v5(p_space, p_name); }
	// Dispatch UUID generation for long names to a CPU worker.
	Signal uuid_v5_async(const String &p_space, const String &p_name);
	// Check whether text is a UUID.
	bool is_uuid(const String &p_text) { return Uuid::is_valid(p_text); }
	// Convert a UUID to 16 bytes.
	Ref<R> uuid_bytes(const String &p_text) { return Uuid::to_bytes(p_text); }
	// Return a UUID's version.
	int uuid_version(const String &p_text) { return Uuid::version_of(p_text); }
	// Return the nil UUID.
	String nil_uuid() { return Uuid::nil_id(); }
	// Return the DNS UUID namespace.
	String dns_namespace() { return Uuid::ns_dns(); }
	// Return the URL UUID namespace.
	String url_namespace() { return Uuid::ns_url(); }
	// Return the OID UUID namespace.
	String oid_namespace() { return Uuid::ns_oid(); }
};

// Expose text formatting through one global entry point.
class GDTextAPI : public Object {
	GDCLASS(GDTextAPI, Object);

protected:
	// Expose text APIs to script.
	static void _bind_methods();

public:
	// Return the edit distance between two strings.
	int distance(const String &p_a, const String &p_b) { return Text::distance(p_a, p_b); }
	// Return the nearest candidate string.
	String closest(const String &p_word, const PackedStringArray &p_options) { return Text::closest(p_word, p_options); }
	// Shorten text to fit a width and append an ellipsis.
	String ellipsis(const String &p_text, int p_width) { return Text::ellipsis(p_text, p_width); }
	// Format a byte count with readable units.
	String size_of(int64_t p_bytes) { return Text::size_of(p_bytes); }
	// Format milliseconds as a readable duration.
	String duration(double p_ms) { return Text::duration(p_ms); }
	// Convert text to snake_case.
	String snake(const String &p_text) { return Text::snake(p_text); }
	// Convert text to camelCase.
	String camel(const String &p_text) { return Text::camel(p_text); }
	// Convert text to Title Case.
	String title(const String &p_text) { return Text::title(p_text); }
	// Format rows as an aligned text table.
	String table(const Array &p_rows, int p_gap) { return Text::table(p_rows, p_gap); }
	// Dispatch input-dependent text processing to CPU workers.
	Signal distance_async(const String &p_a, const String &p_b);
	Signal closest_async(const String &p_word, const PackedStringArray &p_options);
	Signal ellipsis_async(const String &p_text, int p_width);
	Signal snake_async(const String &p_text);
	Signal camel_async(const String &p_text);
	Signal title_async(const String &p_text);
	Signal table_async(const Array &p_rows, int p_gap);
};

// Expose HTML entities, tags, and templates through one global entry point.
class GDHTMLAPI : public Object {
	GDCLASS(GDHTMLAPI, Object);

protected:
	// Expose HTML APIs to script.
	static void _bind_methods();

public:
	// Escape HTML text.
	String escape(const String &p_text) { return Html::escape(p_text); }
	// Decode HTML entities into text.
	String unescape(const String &p_text) { return Html::unescape(p_text); }
	// Escape an HTML attribute value safely.
	String attr(const String &p_value) { return Html::attr(p_value); }
	// Build an HTML tag from its name, body, and attributes.
	String tag(const String &p_name, const String &p_body, const Dictionary &p_attrs) { return Html::tag(p_name, p_body, p_attrs); }
	// Fill template placeholders with dictionary values.
	Ref<R> fill(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials) { return Html::fill(p_tpl, p_data, p_partials); }
	// Analyze template HTML contexts into a shareable renderer.
	Ref<R> template_of(const String &p_tpl, const Dictionary &p_partials) { return Html::template_of(p_tpl, p_partials); }
	// Dispatch HTML processing to CPU workers.
	Signal escape_async(const String &p_text);
	Signal unescape_async(const String &p_text);
	Signal attr_async(const String &p_value);
	Signal tag_async(const String &p_name, const String &p_body, const Dictionary &p_attrs);
	Signal fill_async(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials);
};

// Expose semantic-version parsing and comparison through one global entry point.
class GDSemanticVersionAPI : public Object {
	GDCLASS(GDSemanticVersionAPI, Object);

protected:
	// Expose semantic-version APIs to script.
	static void _bind_methods();

public:
	// Parse version text into a component dictionary.
	Ref<R> parse(const String &p_raw) { return Semver::parse(p_raw); }
	// Require a complete semantic version suitable for packages.
	bool is_canonical(const String &p_raw) { return Semver::is_canonical(p_raw); }
	// Compare two versions.
	int compare(const Dictionary &p_a, const Dictionary &p_b) { return Semver::compare(p_a, p_b); }
	// Check whether a version satisfies a range.
	bool satisfies(const Dictionary &p_v, const String &p_range) { return Semver::satisfies(p_v, p_range); }
	// Check whether a version is not a prerelease.
	bool is_stable(const Dictionary &p_v) { return Semver::is_stable(p_v); }
	// Format a version dictionary as text.
	String text(const Dictionary &p_v) { return Semver::text(p_v); }
	// Select the newest version within a range.
	Ref<R> best(const PackedStringArray &p_list, const String &p_range) { return Semver::best(p_list, p_range); }
	// Dispatch selection from large version lists to a CPU worker.
	Signal best_async(const PackedStringArray &p_list, const String &p_range);
};

// Expose UTC date-time conversion through one global entry point.
class GDDateTimeAPI : public Object {
	GDCLASS(GDDateTimeAPI, Object);

protected:
	// Expose date-time APIs to script.
	static void _bind_methods();

public:
	// Return the current Unix timestamp.
	int64_t now() { return Datetime::now(); }
	// Split a Unix timestamp into date-time components.
	Dictionary to_parts(int64_t p_unix) { return Datetime::to_parts(p_unix); }
	// Convert date-time components into a Unix timestamp.
	int64_t from_parts(const Dictionary &p_parts) { return Datetime::from_parts(p_parts); }
	// Format a Unix timestamp with the specified pattern.
	String format(int64_t p_unix, const String &p_pattern) { return Datetime::format(p_unix, p_pattern); }
	// Format a Unix timestamp as ISO 8601 text.
	String to_iso(int64_t p_unix) { return Datetime::to_iso(p_unix); }
	// Format a Unix timestamp as an HTTP date.
	String to_http(int64_t p_unix) { return Datetime::to_http(p_unix); }
	// Parse ISO 8601 text into a Unix timestamp.
	Ref<R> parse_iso(const String &p_text) { return Datetime::parse_iso(p_text); }
	// Add an amount in the specified unit to a Unix timestamp.
	int64_t add(int64_t p_unix, int64_t p_amount, const String &p_unit) { return Datetime::add(p_unix, p_amount, p_unit); }
	// Return the difference between timestamps in the specified unit.
	int64_t diff(int64_t p_a, int64_t p_b, const String &p_unit) { return Datetime::diff(p_a, p_b, p_unit); }
	// Return the start of the same UTC day.
	int64_t start_of_day(int64_t p_unix) { return Datetime::start_of_day(p_unix); }
	// Return the weekday number for a Unix timestamp.
	int weekday(int64_t p_unix) { return Datetime::weekday(p_unix); }
	// Check whether a year is a leap year.
	bool is_leap(int p_year) { return Datetime::is_leap(p_year); }
	// Return the number of days in a month.
	int days_in_month(int p_year, int p_month) { return Datetime::days_in_month(p_year, p_month); }
	// Format a timestamp relative to a reference time.
	String ago(int64_t p_unix, int64_t p_base) { return Datetime::ago(p_unix, p_base); }
};

// Group command-line arguments and environment helpers.
class GDCLIAPI : public Object {
	GDCLASS(GDCLIAPI, Object);

protected:
	// Expose command-line APIs to script.
	static void _bind_methods();

public:
	// Return the running executable's version.
	String version() const;
	// Return the upstream documentation base used to resolve version-independent links.
	String docs_url() const;
	// Create a command-line flag parser.
	Ref<GDCLIFlags> flags() const;
	// Return a permitted environment value or its default.
	Variant env(const String &p_name, const Variant &p_fallback) const;
	// Return a required environment value or Err.
	Ref<R> require_env(const String &p_name) const;
	// Return the current working directory.
	String cwd() const;
	// Return the operating-system name only when permitted.
	String platform() const;
	// Return the CPU architecture name only when permitted.
	String arch() const;
	// Check whether standard input is an interactive terminal.
	bool stdin_tty() const { return Text::stdin_tty(); }
	// Check whether standard output is an interactive terminal.
	bool stdout_tty() const { return Text::stdout_tty(); }
	// Check whether standard error is an interactive terminal.
	bool stderr_tty() const { return Text::stderr_tty(); }
	// Decorate terminal text using an ANSI code.
	String paint(const String &p_text, int p_code) const { return Text::paint(p_text, p_code, Text::stdout_tty()); }
	// Decorate terminal text in red.
	String red(const String &p_text) const { return Text::paint(p_text, 31, Text::stdout_tty()); }
	// Decorate terminal text in green.
	String green(const String &p_text) const { return Text::paint(p_text, 32, Text::stdout_tty()); }
	// Decorate terminal text in yellow.
	String yellow(const String &p_text) const { return Text::paint(p_text, 33, Text::stdout_tty()); }
	// Decorate terminal text in blue.
	String blue(const String &p_text) const { return Text::paint(p_text, 34, Text::stdout_tty()); }
	// Decorate terminal text in gray.
	String gray(const String &p_text) const { return Text::paint(p_text, 90, Text::stdout_tty()); }
	// Decorate terminal text in bold.
	String bold(const String &p_text) const { return Text::paint(p_text, 1, Text::stdout_tty()); }
	// Start a child process and return an awaitable completion signal.
	Signal run(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts);
	Signal run_async(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts) { return run(p_path, p_args, p_opts); }
};

// Create test assertion collectors.
class GDTestAPI : public Object {
	GDCLASS(GDTestAPI, Object);

protected:
	// Expose test APIs to script.
	static void _bind_methods();

public:
	// Create a lightweight assertion collector.
	Ref<GDTestCheck> check() const;
};

class GDWebAPI;

// Provide the sole global entry point for standard APIs.
class GDAPI : public Object {
	GDCLASS(GDAPI, Object);

	GDAsyncAPI *async = nullptr;
	GDLogAPI *log = nullptr;
	GDNetAPI *net = nullptr;
	GDHTTPAPI *http = nullptr;
	GDWebAPI *web = nullptr;
	GDFSAPI *file = nullptr;
	GDCollectionsAPI *collection = nullptr;
	GDCodecAPI *data = nullptr;
	GDIDAPI *id = nullptr;
	GDTextAPI *text = nullptr;
	GDHTMLAPI *html = nullptr;
	GDMathAPI *math = nullptr;
	GDSemanticVersionAPI *version = nullptr;
	GDDateTimeAPI *time = nullptr;
	GDCLIAPI *cli = nullptr;
	GDTestAPI *test = nullptr;
	GDDatabaseAPI *database = nullptr;

protected:
	// Expose child APIs as GD properties.
	static void _bind_methods();

public:
	// Create child API instances.
	GDAPI();
	// Destroy child API instances.
	~GDAPI();
	// Return the asynchronous API.
	GDAsyncAPI *get_async() const { return async; }
	// Return the logging API.
	GDLogAPI *get_log() const { return log; }
	// Return the network API.
	GDNetAPI *get_net() const { return net; }
	// Return the HTTP client API.
	GDHTTPAPI *get_http() const { return http; }
	// Return the web application API.
	GDWebAPI *get_web() const { return web; }
	// Return the file API.
	GDFSAPI *get_file() const { return file; }
	// Return the collection API.
	GDCollectionsAPI *get_collection() const { return collection; }
	// Return the encoding API.
	GDCodecAPI *get_data() const { return data; }
	// Return the ID API.
	GDIDAPI *get_id() const { return id; }
	// Return the text API.
	GDTextAPI *get_text() const { return text; }
	// Return the HTML API.
	GDHTMLAPI *get_html() const { return html; }
	// Return the mathematics API.
	GDMathAPI *get_math() const { return math; }
	// Return the semantic-version API.
	GDSemanticVersionAPI *get_version() const { return version; }
	// Return the date-time API.
	GDDateTimeAPI *get_time() const { return time; }
	// Return the command-line API.
	GDCLIAPI *get_cli() const { return cli; }
	// Return the test API.
	GDTestAPI *get_test() const { return test; }
	// Return the shared database API.
	GDDatabaseAPI *get_database() const { return database; }
};

// Run caller-provided JWT revocation checks on the main thread after CPU work completes.
class GDWebJwtCall : public RefCounted {
	GDCLASS(GDWebJwtCall, RefCounted);

	Ref<GDWebJwtCall> self_hold; // Keep this operation alive until completion is delivered.
	Variant check; // Optional revocation check; reject non-Callable values.
	bool has_check = false; // Distinguish an omitted check from an invalid value.

	// Apply main-thread revocation checks to worker verification results.
	void verified(const Variant &p_result);

protected:
	static void _bind_methods();

public:
	// Dispatch cryptography and parsing to workers, then call the final Callable on the main thread.
	static Signal start(const String &p_token, const Variant &p_key, const Dictionary &p_opts);
};

// Create web applications and standard middleware through one entry point.
class GDWebAPI : public Object {
	GDCLASS(GDWebAPI, Object);

protected:
	// Expose web APIs to script.
	static void _bind_methods();

public:
	// Create a routed web application.
	Ref<GDWebApp> app() const;
	// Create a raw HTTP server.
	Ref<GDWebServer> server() const;
	// Create a plain-text response.
	Dictionary text(const String &p_body, int64_t p_status) const { return Http::text(p_body, p_status); }
	// Create an HTML response.
	Dictionary html(const String &p_body, int64_t p_status) const { return Http::html(p_body, p_status); }
	// Encode locally and yield only unfinished traversal after the current time slice.
	Variant json(const Variant &p_data, int64_t p_status) const;
	// Create a binary response.
	Dictionary bytes(const PackedByteArray &p_body, const String &p_type, int64_t p_status) const { return Http::bytes_out(p_body, p_type, p_status); }
	// Create a lazy response from a producer writing bytes into its response Writer.
	Dictionary stream(const Callable &p_next, int64_t p_length, const String &p_type, int64_t p_status) const;
	// Create a redirect response.
	Dictionary redirect(const String &p_to, int64_t p_status, bool p_away) const { return Http::redirect(p_to, p_status, p_away); }
	// Add basic security headers to a response.
	Dictionary guard(const Dictionary &p_reply) const { return Http::guard(p_reply); }
	// Create a 404 response.
	Dictionary not_found(const String &p_msg) const { return Http::not_found(p_msg); }
	// Replace a response header.
	Dictionary header(const Dictionary &p_reply, const String &p_name, const Variant &p_value) const { return Http::head(p_reply, p_name, p_value); }
	// Append another response header with the same name.
	Dictionary add_header(const Dictionary &p_reply, const String &p_name, const Variant &p_value) const { return Http::add_head(p_reply, p_name, p_value); }
	// Render an HTML response from a template file, yielding internal waits to the event loop.
	Signal view(const String &p_path, const Dictionary &p_data, int64_t p_status, const Callable &p_renderer) const;
	// Load a template on a worker and apply the optional renderer before returning a result.
	Signal view_async(const String &p_path, const Dictionary &p_data, int64_t p_status, const Callable &p_renderer) const;
	// Dispatch large JSON response encoding to a CPU worker.
	Signal json_async(const Variant &p_data, int64_t p_status) const;
	// Return the HTTP status corresponding to an Err.
	int error_status(const Ref<Err> &p_err) const { return Http::status_of(p_err); }
	// Sign claims into a JWT.
	Ref<R> jwt_sign(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts) const { return GDWebApp::jwt_sign(p_claims, p_key, p_opts); }
	// Verify a JWT and return its claims.
	Ref<R> jwt_verify(const String &p_token, const Variant &p_key, const Dictionary &p_opts) const { return GDWebApp::jwt_verify(p_token, p_key, p_opts); }
	Signal jwt_sign_async(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts) const;
	Signal jwt_verify_async(const String &p_token, const Variant &p_key, const Dictionary &p_opts) const;
	// Create middleware that verifies bearer JWTs.
	Ref<GDWebMiddleware> jwt(const Variant &p_key, const Dictionary &p_opts) const { return GDWebApp::jwt(p_key, p_opts); }
	// Create CSRF middleware that permits only same-origin writes.
	Ref<GDWebMiddleware> csrf(const Dictionary &p_opts) const { return GDWebApp::csrf(p_opts); }
	// Create a cookie-session store and middleware.
	Ref<GDWebSessionStore> sessions(const Dictionary &p_opts) const;
	// Create a per-key fixed-window rate limiter.
	Ref<GDWebMiddleware> rate(const Dictionary &p_opts) const;
	// Create a text-length validation rule.
	Dictionary text_rule(const Dictionary &p_opts) const;
	// Create an integer-range validation rule.
	Dictionary int_rule(const Dictionary &p_opts) const;
	// Create a numeric-range validation rule.
	Dictionary number_rule(const Dictionary &p_opts) const;
	// Create a boolean validation rule.
	Dictionary bool_rule() const { return GDWebApp::rule_boolean(); }
	// Create an array item and length validation rule.
	Dictionary list_rule(const Dictionary &p_item, const Dictionary &p_opts) const;
	// Create an object-field validation rule.
	Dictionary object_rule(const Dictionary &p_fields, const Dictionary &p_opts) const;
	// Add a default value for missing input to a rule.
	Dictionary optional(const Dictionary &p_rule, const Variant &p_fallback) const { return GDWebApp::rule_optional(p_rule, p_fallback); }
	// Create a rule from an allowed-value list.
	Dictionary one_of(const Array &p_values) const { return GDWebApp::rule_one_of(p_values); }
	// Validate a value with a rule and return the safe value.
	Ref<R> validate(const Variant &p_value, const Dictionary &p_rule) const { return GDWebApp::validate(p_value, p_rule); }
	Signal validate_async(const Variant &p_value, const Dictionary &p_rule) const;
	// Create JSON-body validation middleware.
	Ref<GDWebMiddleware> json_body(const Dictionary &p_rule, const String &p_name) const { return GDWebApp::valid_json(p_rule, p_name); }
	// Create query validation middleware.
	Ref<GDWebMiddleware> query(const Dictionary &p_rule, const String &p_name) const { return GDWebApp::valid_query(p_rule, p_name); }
	// Create route-parameter validation middleware.
	Ref<GDWebMiddleware> params(const Dictionary &p_rule, const String &p_name) const { return GDWebApp::valid_params(p_rule, p_name); }
};

// Register singleton instances with the runtime.
void register_cli_singletons();
// Destroy registered singleton instances.
void unregister_cli_singletons();
