/**************************************************************************/
/*  pkgscope.h                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Decide which package a script belongs to and which packages it may name.
//
// gd.json names packages by alias. gd.lock pins every registry package as
// @scope/name@version and records, per package, the alias each of its own
// imports resolved to. A script inside a package resolves pkg://alias/ through
// its own package's table, so two packages can hold different versions of one
// package under the same alias without seeing each other.
//
// Every registry package has one canonical path, pkg://@scope/name@version/.
// Aliases expand to it before a script is loaded, so one version is one script
// identity however it is reached. Local and url packages keep their alias path.
//
// Native extensions register classes process-wide; the analyzer asks can_see
// so a script may only name classes of extensions its own package imports.
//
// Implementation is in pkgscope.cpp.

#pragma once

#include "core/os/mutex.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"

class PkgScope {
	static HashMap<String, HashMap<String, String>> tables; // Owner id ("" for the project) to alias to canonical id.
	static HashMap<String, String> dirs; // res://pkg/<dir> to the canonical id copied there.
	static HashMap<String, String> class_owners; // Extension class to the canonical id of its package, "" outside packages.
	static bool classes_scanned; // Whether class_owners reflects the loaded extensions.
	static Mutex class_mutex; // Guards the lazy class scan, since scripts may be analyzed off the main thread.

	static void _scan_classes();

public:
	// Preserve the lexical caller across nested calls and coroutine resumes without allocating.
	class Frame {
		const String *previous; // Calling script path, restored on every return.

	public:
		explicit Frame(const String *p_path);
		~Frame();
	};
	// Record that alias inside p_owner ("" for the project) names the package p_id.
	static void set_alias(const String &p_owner, const String &p_alias, const String &p_id);
	// Record that res://pkg/<p_dir> holds the package p_id.
	static void set_dir(const String &p_dir, const String &p_id);
	// Forget everything, for tests.
	static void clear();

	// Report whether a pkg:// key is a canonical @scope/name@version id rather than an alias.
	static bool is_id(const String &p_key);
	// Return the alias or canonical id that leads a pkg:// path, and the rest after it.
	static String key_of(const String &p_path, String *r_rest = nullptr);
	// Return the directory name under pkg/ that holds p_id: its project alias when one exists, else the id.
	static String dir_of(const String &p_id);
	// Return the canonical id of the package holding a script, or "" for project scripts.
	static String owner_of(const String &p_script_path);
	// Return the id every alias in p_owner's table resolves to, or empty when none does.
	static String resolve(const String &p_owner, const String &p_alias);

	// Rewrite pkg://alias/rest to its canonical form as seen from the script p_from.
	// Return other paths unchanged, and empty with a reason when the script's package lacks the alias.
	static String expand(const String &p_path, const String &p_from, String &r_why);
	// Expand using the active lexical caller, or the project outside script calls.
	static String canonical(const String &p_path);
	// Give a copy under pkg/ the identity of its pkg:// path, so one script is one resource by either spelling.
	static String of_copy(const String &p_path);

	// Report whether the script p_from may name the native class; explain a refusal in r_why.
	static bool can_see(const StringName &p_class, const String &p_from, String &r_why);

	// Resolve an @import spec written in p_from to one script path, and the identifier it binds.
	// p_quoted says the spec was a string literal, which alone may name explicit paths.
	// Return false with r_why when nothing or more than one thing matches.
	static bool locate(const String &p_spec, bool p_quoted, const String &p_as, const String &p_from, String &r_path, String &r_name, String &r_why);
	// After extensions load, refuse a package extension that registered a class its manifest's
	// [classes] does not declare: the registry reserved names from that declaration.
	static bool verify_declared(String &r_why);
};
