/**************************************************************************/
/*  compile.h                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Build a standalone executable.
//
// Append a package containing runtime .gd files to the executable.
// Locate the trailing package at startup and execute its embedded entry point.
// Use the embedded PCK layout recognized by the runtime loader.
//
// [executable][package][8-byte package length][4-byte magic]

#include "cli/sys/mount.h"
#include "cli/sys/pkgscope.h"
#include "cli/sys/perm.h"
#include "cli/sys/system.h"

#include "core/config/project_settings.h"
#include "core/extension/gdextension.h"
#include "core/extension/gdextension_library_loader.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/pck_packer.h"
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/os/shared_object.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"

class GDCompile {
	static constexpr uint32_t PACK_MAGIC = 0x43504447; // "GDPC"
	static constexpr const char *ENTRY_FILE = "res://_gd_entry";
	static constexpr const char *EXT_LIST = "res://.godot/extension_list.cfg";

	// Check whether a path should be excluded from distribution.
	static bool _skip_dir(const String &p_name) {
		return p_name == ".git" || p_name == ".godot" || p_name == "bin" || p_name == "tmp";
	}

	// Identify native libraries targeting another operating system.
	static bool _foreign_library(const String &p_name) {
#ifdef MACOS_ENABLED
		return p_name.ends_with(".dll") || p_name.ends_with(".so");
#elif defined(WINDOWS_ENABLED)
		return p_name.ends_with(".dylib") || p_name.ends_with(".so");
#else
		return p_name.ends_with(".dylib") || p_name.ends_with(".dll");
#endif
	}

	// Identify compiled executables by their trailing package marker.
	static bool _compiled(const String &p_path) {
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
		if (file.is_null() || file->get_length() < 4) {
			return false;
		}
		file->seek(file->get_length() - 4);
		return file->get_32() == PACK_MAGIC;
	}

	// Resolve relative paths against the current working directory.
	static String _absolute(const String &p_path) {
		return (p_path.is_absolute_path() ? p_path : GDSystem::cwd().path_join(p_path)).simplify_path();
	}

	// Check path containment with directory-boundary awareness.
	static bool _inside(const String &p_dir, const String &p_file) {
		const String dir = _absolute(p_dir).trim_suffix("/");
		const String file = _absolute(p_file);
		return file == dir || file.begins_with(dir + "/");
	}

	// Open the parent directory and remove a partial or old output file.
	static void _remove_file(const String &p_path) {
		const String parent = p_path.get_base_dir().is_empty() ? String(".") : p_path.get_base_dir();
		Ref<DirAccess> dir = DirAccess::open(parent);
		if (dir.is_valid()) {
			dir->remove(p_path.get_file());
		}
	}

	// Remove only an old file, never a directory, before safely recreating temporary output.
	static Error _prepare_temp(const String &p_path) {
		const String parent = p_path.get_base_dir().is_empty() ? String(".") : p_path.get_base_dir();
		Ref<DirAccess> dir = DirAccess::open(parent);
		ERR_FAIL_COND_V(dir.is_null(), ERR_CANT_OPEN);
		const String name = p_path.get_file();
		if (dir->dir_exists(name) && !dir->is_link(name)) {
			return ERR_ALREADY_EXISTS;
		}
		if ((dir->file_exists(name) || dir->is_link(name)) && dir->remove(name) != OK) {
			return ERR_CANT_CREATE;
		}
		return OK;
	}

	// Copy input in bounded chunks and detect short reads or failed writes.
	static Error _append_file(const Ref<FileAccess> &p_src, const Ref<FileAccess> &p_dst) {
		uint64_t rest = p_src->get_length();
		while (rest > 0) {
			const uint64_t size = MIN(rest, uint64_t(64 * 1024)); // Read chunk size avoids loading the whole input into memory.
			const PackedByteArray block = p_src->get_buffer(size);
			if ((uint64_t)block.size() != size) {
				return ERR_FILE_CORRUPT;
			}
			if (!p_dst->store_buffer(block)) {
				return ERR_CANT_CREATE;
			}
			rest -= size;
		}
		return OK;
	}

	// Collect runtime files while excluding secrets, generated artifacts, and foreign-platform libraries.
	static void _collect(const String &p_root, const String &p_dir, const String &p_skip, List<String> &r_files) {
		Ref<DirAccess> dir = DirAccess::open(p_dir);
		if (dir.is_null() || dir->list_dir_begin() != OK) {
			return;
		}
		String name = dir->get_next();
		while (!name.is_empty()) {
			const String full = p_dir.path_join(name);
			if (full.simplify_path() != p_skip && name != ".env" && !name.ends_with(".pck.tmp")) {
				if (dir->current_is_dir()) {
					if (!_skip_dir(name) && !name.begins_with(".") && !FileAccess::exists(full.path_join("gd.json"))) {
						_collect(p_root, full, p_skip, r_files);
					}
				} else if (!name.begins_with(".") && !_foreign_library(name) && name != "project.godot" && !_compiled(full)) {
					r_files.push_back(full);
				}
			}
			name = dir->get_next();
		}
		dir->list_dir_end();
	}

	// Map a project file to its portable package path.
	static String _resource(const String &p_root, const String &p_path) {
		const String source = _absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
		ERR_FAIL_COND_V_MSG(!_inside(p_root, source), String(), vformat("Library outside the project: %s.", p_path));
		return "res://" + source.trim_prefix(_absolute(p_root).trim_suffix("/") + "/");
	}

	// Include selected native files and manifests with portable resource paths.
	static Error _add_extension_list(const String &p_root, const Ref<PCKPacker> &p_packer, HashSet<String> &r_packed) {
		const String src = p_root.path_join(".godot/extension_list.cfg");
		if (!FileAccess::exists(src)) {
			return OK;
		}
		Error err = p_packer->add_file(EXT_LIST, src);
		ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot add the extension list.");
		Ref<FileAccess> list = FileAccess::open(src, FileAccess::READ);
		ERR_FAIL_COND_V_MSG(list.is_null(), ERR_CANT_OPEN, "Cannot read the extension list.");
		const auto has_feature = [](const String &p_feature) { return OS::get_singleton()->has_feature(p_feature); };
		const String feature = OS::get_singleton()->get_name().to_lower();
		while (!list->eof_reached()) {
			const String rel = list->get_line().strip_edges().trim_prefix("res://").trim_prefix("./").simplify_path();
			if (rel.is_empty() || rel.begins_with("#")) {
				continue;
			}
			const String manifest = _absolute(p_root).path_join(rel);
			const String resource = _resource(p_root, manifest);
			ERR_FAIL_COND_V(resource.is_empty(), ERR_INVALID_DATA);
			if (r_packed.has(resource)) {
				continue;
			}
			Ref<ConfigFile> config;
			config.instantiate();
			ERR_FAIL_COND_V_MSG(config->load(manifest) != OK, ERR_CANT_OPEN, vformat("Cannot read %s.", manifest));
			const String library = GDExtensionLibraryLoader::find_extension_library(manifest, config, has_feature);
			ERR_FAIL_COND_V_MSG(library.is_empty(), ERR_FILE_NOT_FOUND, vformat("No library for this machine in %s.", manifest));
			List<String> needed;
			needed.push_back(library);
			Dictionary dependencies;
			for (const SharedObject &dep : GDExtensionLibraryLoader::find_extension_dependencies(manifest, config, has_feature)) {
				const String res = _resource(p_root, dep.path);
				ERR_FAIL_COND_V(res.is_empty(), ERR_INVALID_DATA);
				dependencies[res] = dep.target;
				needed.push_back(dep.path);
			}
			for (const String &path : needed) {
				const String res = _resource(p_root, path);
				ERR_FAIL_COND_V(res.is_empty(), ERR_INVALID_DATA);
				if (r_packed.has(res)) {
					continue;
				}
				const String source = ProjectSettings::get_singleton()->globalize_path(path);
				ERR_FAIL_COND_V_MSG(!FileAccess::exists(source), ERR_FILE_NOT_FOUND, vformat("Missing library %s.", path));
				err = p_packer->add_file(res, source);
				ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Cannot add %s.", path));
				r_packed.insert(res);
			}
			// Retain metadata while recording the loader's selected files for this executable.
			if (config->has_section("libraries")) {
				config->erase_section("libraries");
			}
			if (config->has_section("dependencies")) {
				config->erase_section("dependencies");
			}
			config->set_value("libraries", feature, _resource(p_root, library));
			config->set_value("dependencies", feature, dependencies);
			err = p_packer->add_file_from_buffer(resource, config->encode_to_text().to_utf8_buffer());
			ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Cannot add %s.", manifest));
			r_packed.insert(resource);
		}
		return OK;
	}

	// Add every regular file below a native directory under a resource prefix.
	// Packages are taken whole: registry files are exactly what was published, and a local
	// package is read from its copy under pkg/, which already leaves out dotfiles and the token.
	static Error _add_tree(const String &p_native, const String &p_res, const Ref<PCKPacker> &p_packer, HashSet<String> &r_packed) {
		Ref<DirAccess> dir = DirAccess::open(p_native);
		ERR_FAIL_COND_V_MSG(dir.is_null(), ERR_CANT_OPEN, vformat("Cannot read %s.", p_native));
		ERR_FAIL_COND_V(dir->list_dir_begin() != OK, ERR_CANT_OPEN);
		String name = dir->get_next();
		while (!name.is_empty()) {
			const String native = p_native.path_join(name);
			const String res = p_res.path_join(name);
			if (name == "." || name == "..") {
				name = dir->get_next();
				continue;
			}
			if (dir->current_is_dir()) {
				const Error err = _add_tree(native, res, p_packer, r_packed);
				if (err != OK) {
					return err;
				}
			} else if (!_foreign_library(name) && !r_packed.has(res)) {
				const Error err = p_packer->add_file(res, native);
				ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Cannot add %s.", native));
				r_packed.insert(res);
			}
			name = dir->get_next();
		}
		dir->list_dir_end();
		return OK;
	}

	// Resolve every package gd.json names and every package those import, keyed by the directory
	// it takes under pkg/. Static reachability from the entry cannot see load() at run time or
	// an inherited base, so the whole graph goes in; gd.lock already fixes it exactly.
	// Run this before any trusted region so pkg:// still resolves through the jail.
	static Error _find_packages(const String &p_entry, HashMap<String, String> &r_roots) {
		Ref<Script> entry = ResourceLoader::load(p_entry);
		ERR_FAIL_COND_V_MSG(entry.is_null(), ERR_CANT_OPEN, vformat("Cannot load %s.", p_entry));
		List<String> keys;
		Mount::pkg_keys(&keys);
		for (const String &key : keys) {
			// A project alias and its canonical id share one directory; take it once.
			const String id = PkgScope::is_id(key) ? key : PkgScope::resolve(String(), key);
			const String dir = id.is_empty() ? key : PkgScope::dir_of(id);
			if (r_roots.has(dir)) {
				continue;
			}
			String why;
			const String native = Mount::resolve("pkg://" + key, false, why);
			ERR_FAIL_COND_V_MSG(native.is_empty(), ERR_FILE_NOT_FOUND, why);
			r_roots[dir] = native;
		}
		return OK;
	}

	// Embed resolved packages under pkg/, where the executable resolves pkg:// from the embedded gd.lock.
	static Error _add_packages(const HashMap<String, String> &p_roots, const Ref<PCKPacker> &p_packer, HashSet<String> &r_packed) {
		for (const KeyValue<String, String> &e : p_roots) {
			const Error err = _add_tree(e.value, String("res://pkg").path_join(e.key), p_packer, r_packed);
			if (err != OK) {
				return err;
			}
		}
		return OK;
	}

	// Extract resource directories while preserving native-loader relative dependency layout.
	static Error _extract_tree(const String &p_src, const String &p_dst) {
		Ref<DirAccess> dir = DirAccess::open(p_src);
		ERR_FAIL_COND_V(dir.is_null(), ERR_CANT_OPEN);
		ERR_FAIL_COND_V(DirAccess::make_dir_recursive_absolute(p_dst) != OK, ERR_CANT_CREATE);
		ERR_FAIL_COND_V(dir->list_dir_begin() != OK, ERR_CANT_OPEN);
		String name = dir->get_next();
		while (!name.is_empty()) {
			const String src = p_src.path_join(name);
			const String dst = p_dst.path_join(name);
			Error err = OK;
			if (dir->current_is_dir()) {
				err = _extract_tree(src, dst);
			} else {
				const String sum = FileAccess::get_sha256(src);
				if (!FileAccess::exists(dst) || FileAccess::get_sha256(dst) != sum) {
					Ref<FileAccess> in = FileAccess::open(src, FileAccess::READ);
					ERR_FAIL_COND_V(in.is_null(), ERR_CANT_OPEN);
					const String tmp = dst + ".tmp";
					Ref<FileAccess> out = FileAccess::open(tmp, FileAccess::WRITE);
					ERR_FAIL_COND_V(out.is_null(), ERR_CANT_CREATE);
					out->store_buffer(in->get_buffer(in->get_length()));
					out->close();
					if (FileAccess::get_sha256(tmp) != sum) {
						return ERR_FILE_CORRUPT;
					}
					err = DirAccess::rename_absolute(tmp, dst);
					FileAccess::set_unix_permissions(dst, 0755);
				}
			}
			if (err != OK) {
				dir->list_dir_end();
				return err;
			}
			name = dir->get_next();
		}
		dir->list_dir_end();
		return OK;
	}

public:
	// Build a standalone executable from a .gd entry point.
	static int run(const String &p_entry, const String &p_output) {
		if (p_entry.is_empty()) {
			print_line("usage: gd compile [--output <name>] <script.gd>");
			return EXIT_FAILURE;
		}
		if (!FileAccess::exists(p_entry)) {
			ERR_FAIL_V_MSG(EXIT_FAILURE, vformat("No such script: %s", p_entry));
		}

		// Use the working directory, where gd.json and res:// live, as the root.
		// Use the entry directory when it has its own manifest or lies outside the working directory.
		String root = ".";
		const String entry = p_entry.simplify_path();
		const String entry_dir = entry.get_base_dir().is_empty() ? String(".") : entry.get_base_dir();
		if (FileAccess::exists(entry_dir.path_join("gd.json")) || !_inside(root, entry)) {
			root = entry_dir;
		}
		const String entry_rel = "res://" + entry.trim_prefix(root).trim_prefix("/");
		// Load the entry as this run sees it, so pkg:// resolves through the same aliases.
		const String entry_src = entry.begins_with("res://") || entry.is_absolute_path() ? entry : "res://" + entry;
		HashMap<String, String> packages; // Directory under pkg/ to its native source for every package gd.json reaches.
		ERR_FAIL_COND_V_MSG(_find_packages(entry_src, packages) != OK, EXIT_FAILURE, "Cannot resolve the packages the script uses.");
		const Perm::Trusted trust; // Read the executable and write the output selected by -o.

		String out = p_output;
		if (out.is_empty()) {
			out = p_entry.get_file().get_basename();
		}
#ifdef WINDOWS_ENABLED
		// Give native Windows executables the platform filename expected by launchers and users.
		if (out.get_extension().nocasecmp_to("exe") != 0) {
			out += ".exe";
		}
#endif

		// Build a package containing runtime files beneath the selected root.
		const String out_dir = out.get_base_dir().is_empty() ? String(".") : out.get_base_dir();
		const String pck_path = out_dir.path_join("." + out.get_file() + ".gd-pack.tmp");
		ERR_FAIL_COND_V_MSG(_prepare_temp(pck_path) != OK, EXIT_FAILURE, "Cannot prepare the temporary pack.");
		Ref<PCKPacker> packer;
		packer.instantiate();
		Error err = packer->pck_start(pck_path);
		ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, "Cannot start the pack.");

		List<String> files;
		HashSet<String> packed; // Resource paths added while walking the tree.
		err = _add_extension_list(root, packer, packed);
		ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, "Cannot add the native extensions.");
		_collect(root, root, out.simplify_path(), files);
		for (const String &file : files) {
			// Map ./a/b.gd to res://a/b.gd.
			const String rel = file.trim_prefix(root).trim_prefix("/");
			if (packed.has("res://" + rel)) {
				continue;
			}
			packed.insert("res://" + rel);
			// Exclude publishing tokens and package only configuration needed at runtime.
			if (rel == "gd.json") {
				Ref<FileAccess> cfg = FileAccess::open(file, FileAccess::READ);
				ERR_FAIL_COND_V_MSG(cfg.is_null(), EXIT_FAILURE, "Cannot read gd.json.");
				JSON json;
				if (json.parse(cfg->get_as_text()) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
					packer.unref();
					_remove_file(pck_path);
					ERR_PRINT("gd.json must be a valid JSON object.");
					return EXIT_FAILURE;
				}
				Dictionary doc = json.get_data();
				doc.erase("token");
				err = packer->add_file_from_buffer("res://gd.json", (JSON::stringify(doc, "  ") + "\n").to_utf8_buffer());
			} else {
				err = packer->add_file("res://" + rel, file);
			}
			ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, vformat("Cannot add %s.", file));
		}

		// Include the minimal project configuration required by the resource loader.
		// Resource paths cannot be opened without this configuration.
		const String project_ini = vformat(
				"config_version=5\n\n[application]\n\nconfig/name=\"%s\"\n",
				out.get_file());
		err = packer->add_file_from_buffer("res://project.godot", project_ini.to_utf8_buffer());
		ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, "Cannot write the project file.");

		err = _add_packages(packages, packer, packed);
		ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, "Cannot add the packages the script uses.");

		// Record the embedded entry-point path.
		err = packer->add_file_from_buffer(ENTRY_FILE, entry_rel.to_utf8_buffer());
		ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, "Cannot record the entry point.");

		err = packer->flush();
		ERR_FAIL_COND_V_MSG(err != OK, EXIT_FAILURE, "Cannot write the pack.");
		packer.unref(); // Close the temporary pack before reading and deleting it.

		// Concatenate the executable and package incrementally.
		Ref<FileAccess> self = FileAccess::open(OS::get_singleton()->get_executable_path(), FileAccess::READ);
		ERR_FAIL_COND_V_MSG(self.is_null(), EXIT_FAILURE, "Cannot read gd itself.");
		Ref<FileAccess> pck = FileAccess::open(pck_path, FileAccess::READ);
		ERR_FAIL_COND_V_MSG(pck.is_null(), EXIT_FAILURE, "Cannot read the pack.");
		const uint64_t body_size = self->get_length();
		const uint64_t pack_size = pck->get_length();

		// Write incomplete output to a hidden temporary file and replace the destination only on success.
		const String tmp_path = out_dir.path_join("." + out.get_file() + ".gd-compile.tmp");
		ERR_FAIL_COND_V_MSG(_prepare_temp(tmp_path) != OK, EXIT_FAILURE, "Cannot prepare the temporary output.");
		Ref<FileAccess> w = FileAccess::open(tmp_path, FileAccess::WRITE);
		ERR_FAIL_COND_V_MSG(w.is_null(), EXIT_FAILURE, vformat("Cannot write %s.", tmp_path));
		const bool wrote = _append_file(self, w) == OK && _append_file(pck, w) == OK && w->store_64(pack_size) && w->store_32(PACK_MAGIC);
		w->close();
		self->close();
		pck->close();
		_remove_file(pck_path);
		if (!wrote) {
			_remove_file(tmp_path);
			ERR_PRINT(vformat("Cannot finish %s.", out));
			return EXIT_FAILURE;
		}
		const Error mode_err = FileAccess::set_unix_permissions(tmp_path, 0755);
		if (mode_err != OK && mode_err != ERR_UNAVAILABLE) {
			_remove_file(tmp_path);
			ERR_PRINT(vformat("Cannot make %s executable.", out));
			return EXIT_FAILURE;
		}

#ifdef MACOS_ENABLED
		// Appending data invalidates the executable signature checked by macOS.
		// Sign the finished executable again so the operating system can launch it.
		{
			List<String> sign_args;
			sign_args.push_back("--force");
			sign_args.push_back("--sign");
			sign_args.push_back("-"); // Use an ad-hoc signature.
			sign_args.push_back(tmp_path);
			String sign_out;
			int sign_code = 0;
			const Error sign_err = OS::get_singleton()->execute("/usr/bin/codesign", sign_args, &sign_out, &sign_code, true);
			// The trailing pack is outside strict Mach-O layout, but ad-hoc signature replacement still completes.
			const String sign_text = sign_out.strip_edges();
			const bool appended_pack = sign_code == 1 && sign_text.contains(tmp_path + ": replacing existing signature") &&
					sign_text.ends_with(tmp_path + ": main executable failed strict validation");
			if (sign_err != OK || (sign_code != 0 && !appended_pack)) {
				_remove_file(tmp_path);
				ERR_PRINT(vformat("Cannot sign %s: %s", out, sign_text));
				return EXIT_FAILURE;
			}
		}
#endif
		if (DirAccess::rename_absolute(tmp_path, out) != OK) {
			_remove_file(tmp_path);
			ERR_PRINT(vformat("Cannot replace %s.", out));
			return EXIT_FAILURE;
		}

		print_line(vformat("compiled %s -> %s (%d files, %.1f MB)", p_entry, out, files.size(), (body_size + pack_size) / 1048576.0));
		return EXIT_SUCCESS;
	}

	// Check whether the executable contains a trailing package.
	// Inspect its marker directly because argument parsing precedes package mounting.
	static bool has_embedded() {
		const Perm::Trusted trust; // Read this executable.
		static int cached = -1; // Cache the result to avoid reopening on every check.
		if (cached >= 0) {
			return cached == 1;
		}
		cached = 0;
		Ref<FileAccess> f = FileAccess::open(OS::get_singleton()->get_executable_path(), FileAccess::READ);
		if (f.is_valid() && f->get_length() > 12) {
			f->seek(f->get_length() - 4);
			if (f->get_32() == PACK_MAGIC) {
				cached = 1;
			}
		}
		return cached == 1;
	}

	// Return the entry point of an appended package.
	static String embedded_entry() {
		const Perm::Trusted trust; // Read this executable.
		if (!FileAccess::exists(ENTRY_FILE)) {
			return String();
		}
		Ref<FileAccess> f = FileAccess::open(ENTRY_FILE, FileAccess::READ);
		if (f.is_null()) {
			return String();
		}
		return f->get_as_text().strip_edges();
	}

	// Extract embedded GDExtensions to real files and pass them to the native extension loader.
	static Error prepare_extensions() {
		const Perm::Trusted trust; // Treat embedded extensions as part of the executable.
		if (!has_embedded() || !FileAccess::exists(EXT_LIST)) {
			return OK;
		}
		Ref<FileAccess> list = FileAccess::open(EXT_LIST, FileAccess::READ);
		ERR_FAIL_COND_V(list.is_null(), ERR_CANT_OPEN);
		const String hash = FileAccess::get_sha256(OS::get_singleton()->get_executable_path()).left(24);
		const String root = GDSystem::user_dir().path_join("gd-extensions").path_join(hash);
		ERR_FAIL_COND_V(DirAccess::make_dir_recursive_absolute(root) != OK, ERR_CANT_CREATE);
		PackedStringArray manifests;
		PackedStringArray extracted;
		HashMap<String, Ref<ConfigFile>> configs; // Save manifests after all directory copies finish.
		while (!list->eof_reached()) {
			String path = list->get_line().strip_edges();
			if (path.is_empty() || path.begins_with("#")) {
				continue;
			}
			path = path.trim_prefix("res://").trim_prefix("./").simplify_path();
			if (path.begins_with("../") || path.is_absolute_path() || !path.ends_with(".gdextension")) {
				return ERR_INVALID_DATA;
			}
			const String base = path.get_base_dir();
			if (!extracted.has(base)) {
				const Error err = _extract_tree("res://" + base, root.path_join(base));
				if (err != OK) {
					return err;
				}
				extracted.push_back(base);
			}
			Ref<ConfigFile> config;
			config.instantiate();
			ERR_FAIL_COND_V(config->load("res://" + path) != OK, ERR_CANT_OPEN);
			const auto has_feature = [](const String &p_feature) { return OS::get_singleton()->has_feature(p_feature); };
			const String library = GDExtensionLibraryLoader::find_extension_library("res://" + path, config, has_feature);
			ERR_FAIL_COND_V(library.is_empty(), ERR_FILE_NOT_FOUND);
			List<String> needed;
			needed.push_back(library);
			Dictionary dependencies;
			for (const SharedObject &dep : GDExtensionLibraryLoader::find_extension_dependencies("res://" + path, config, has_feature)) {
				needed.push_back(dep.path);
				dependencies[root.path_join(dep.path.trim_prefix("res://"))] = dep.target;
			}
			// Materialize selected dependencies outside the manifest directory as well.
			for (const String &src : needed) {
				ERR_FAIL_COND_V(!src.begins_with("res://") || !_inside(root, root.path_join(src.trim_prefix("res://"))), ERR_INVALID_DATA);
				const String dir = src.get_base_dir().trim_prefix("res://");
				if (!extracted.has(dir)) {
					const Error err = _extract_tree("res://" + dir, root.path_join(dir));
					ERR_FAIL_COND_V(err != OK, err);
					extracted.push_back(dir);
				}
			}
			// Native loaders need real paths; resource URLs cannot address extracted files.
			if (config->has_section("libraries")) {
				config->erase_section("libraries");
			}
			if (config->has_section("dependencies")) {
				config->erase_section("dependencies");
			}
			const String feature = OS::get_singleton()->get_name().to_lower();
			config->set_value("libraries", feature, root.path_join(library.trim_prefix("res://")));
			config->set_value("dependencies", feature, dependencies);
			const String manifest = root.path_join(path);
			configs[manifest] = config;
			manifests.push_back(manifest);
		}
		// Commit rewritten manifests only after later copies can no longer overwrite them.
		for (const KeyValue<String, Ref<ConfigFile>> &entry : configs) {
			ERR_FAIL_COND_V(entry.value->save(entry.key + ".tmp") != OK, ERR_CANT_CREATE);
			ERR_FAIL_COND_V(DirAccess::rename_absolute(entry.key + ".tmp", entry.key) != OK, ERR_CANT_CREATE);
		}
		const String out_path = root.path_join("extension_list.cfg");
		Ref<FileAccess> out = FileAccess::open(out_path + ".tmp", FileAccess::WRITE);
		ERR_FAIL_COND_V(out.is_null(), ERR_CANT_CREATE);
		for (const String &path : manifests) {
			out->store_line(path);
		}
		out->close();
		ERR_FAIL_COND_V(DirAccess::rename_absolute(out_path + ".tmp", out_path) != OK, ERR_CANT_CREATE);
		GDExtension::set_extension_list_config_file(out_path);
		return OK;
	}
};
