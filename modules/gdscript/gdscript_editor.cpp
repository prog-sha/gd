/**************************************************************************/
/*  gdscript_editor.cpp                                                   */
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

#include "gdscript.h"
#include "gdscript_analyzer.h"
#include "gdscript_parser.h"
#include "gdscript_tokenizer.h"
#include "gdscript_utility_functions.h"

#include "core/config/engine.h"
#include "core/core_constants.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/math/expression.h"
#include "core/object/class_db.h"
#include "core/variant/container_type_validate.h"

Vector<String> GDScriptLanguage::get_comment_delimiters() const {
	static const Vector<String> delimiters = { "#" };
	return delimiters;
}

Vector<String> GDScriptLanguage::get_doc_comment_delimiters() const {
	static const Vector<String> delimiters = { "##" };
	return delimiters;
}

Vector<String> GDScriptLanguage::get_string_delimiters() const {
	static const Vector<String> delimiters = {
		"\" \"",
		"' '",
		"\"\"\" \"\"\"",
		"''' '''",
	};
	// NOTE: StringName, NodePath and r-strings are not listed here.
	return delimiters;
}

bool GDScriptLanguage::is_using_templates() {
	return true;
}

Ref<Script> GDScriptLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<GDScript> scr;
	scr.instantiate();

	String processed_template = p_template;

	const bool type_hints = true;

	if (!type_hints) {
		processed_template = processed_template.replace(": int", "")
									 .replace(": Shader.Mode", "")
									 .replace(": VisualShader.Type", "")
									 .replace(": float", "")
									 .replace(": String", "")
									 .replace(": Array[String]", "")
									 .replace(": Node", "")
									 .replace(": CharFXTransform", "")
									 .replace(":=", "=")
									 .replace(" -> void", "")
									 .replace(" -> bool", "")
									 .replace(" -> int", "")
									 .replace(" -> PortType", "")
									 .replace(" -> String", "")
									 .replace(" -> Object", "");
	}

	processed_template = processed_template.replace("_BASE_", p_base_class_name)
								 .replace("_CLASS_SNAKE_CASE_", p_class_name.to_snake_case().validate_unicode_identifier())
								 .replace("_CLASS_", p_class_name.to_pascal_case().validate_unicode_identifier())
								 .replace("_TS_", _get_indentation());
	scr->set_source_code(processed_template);

	return scr;
}

Vector<ScriptLanguage::ScriptTemplate> GDScriptLanguage::get_built_in_templates(const StringName &p_object) {
	Vector<ScriptLanguage::ScriptTemplate> templates;
	return templates;
}

static void get_function_names_recursively(const GDScriptParser::ClassNode *p_class, const String &p_prefix, HashMap<int, String> &r_funcs) {
	for (int i = 0; i < p_class->members.size(); i++) {
		if (p_class->members[i].type == GDScriptParser::ClassNode::Member::FUNCTION) {
			const GDScriptParser::FunctionNode *function = p_class->members[i].function;
			r_funcs[function->start_line] = p_prefix.is_empty() ? String(function->identifier->name) : p_prefix + "." + String(function->identifier->name);
		} else if (p_class->members[i].type == GDScriptParser::ClassNode::Member::CLASS) {
			String new_prefix = p_class->members[i].m_class->identifier->name;
			get_function_names_recursively(p_class->members[i].m_class, p_prefix.is_empty() ? new_prefix : p_prefix + "." + new_prefix, r_funcs);
		}
	}
}

bool GDScriptLanguage::validate(const String &p_script, const String &p_path, List<String> *r_functions, List<ScriptLanguage::ScriptError> *r_errors, List<ScriptLanguage::Warning> *r_warnings, HashSet<int> *r_safe_lines) const {
	GDScriptParser parser;
	GDScriptAnalyzer analyzer(&parser);

	Error err = parser.parse(p_script, p_path, false);
	if (err == OK) {
		err = analyzer.analyze();
	}
#ifdef DEBUG_ENABLED
	if (r_warnings) {
		for (const GDScriptWarning &E : parser.get_warnings()) {
			const GDScriptWarning &warn = E;
			ScriptLanguage::Warning w;
			w.start_line = warn.start_line;
			w.end_line = warn.end_line;
			w.code = (int)warn.code;
			w.string_code = GDScriptWarning::get_name_from_code(warn.code);
			w.message = warn.get_message();
			r_warnings->push_back(w);
		}
	}
#endif
	if (err) {
		if (r_errors) {
			for (const GDScriptParser::ParserError &pe : parser.get_errors()) {
				ScriptLanguage::ScriptError e;
				e.path = p_path;
				e.line = pe.start_line;
				e.column = pe.start_column;
				e.message = pe.message;
				r_errors->push_back(e);
			}

			for (KeyValue<String, Ref<GDScriptParserRef>> E : parser.get_depended_parsers()) {
				GDScriptParser *depended_parser = E.value->get_parser();
				for (const GDScriptParser::ParserError &pe : depended_parser->get_errors()) {
					ScriptLanguage::ScriptError e;
					e.path = E.key;
					e.line = pe.start_line;
					e.column = pe.start_column;
					e.message = pe.message;
					r_errors->push_back(e);
				}
			}
		}
		return false;
	} else if (r_functions) {
		const GDScriptParser::ClassNode *cl = parser.get_tree();
		HashMap<int, String> funcs;

		get_function_names_recursively(cl, "", funcs);

		for (const KeyValue<int, String> &E : funcs) {
			r_functions->push_back(E.value + ":" + itos(E.key));
		}
	}

#ifdef DEBUG_ENABLED
	if (r_safe_lines) {
		const HashSet<int> &unsafe_lines = parser.get_unsafe_lines();
		for (int i = 1; i <= parser.get_last_line_number(); i++) {
			if (!unsafe_lines.has(i)) {
				r_safe_lines->insert(i);
			}
		}
	}
#endif

	return true;
}

bool GDScriptLanguage::supports_builtin_mode() const {
	return true;
}

bool GDScriptLanguage::supports_documentation() const {
	return true;
}

int GDScriptLanguage::find_function(const String &p_function, const String &p_code) const {
	GDScriptTokenizerText tokenizer;
	tokenizer.set_source_code(p_code);
	int indent = 0;
	GDScriptTokenizer::Token current = tokenizer.scan();
	while (current.type != GDScriptTokenizer::Token::TK_EOF && current.type != GDScriptTokenizer::Token::ERROR) {
		if (current.type == GDScriptTokenizer::Token::INDENT) {
			indent++;
		} else if (current.type == GDScriptTokenizer::Token::DEDENT) {
			indent--;
		}
		if (indent == 0 && current.type == GDScriptTokenizer::Token::FUNC) {
			current = tokenizer.scan();
			if (current.is_identifier()) {
				String identifier = current.get_identifier();
				if (identifier == p_function) {
					return current.start_line;
				}
			}
		}
		current = tokenizer.scan();
	}
	return -1;
}

/* DEBUGGER FUNCTIONS */

thread_local int GDScriptLanguage::_debug_parse_err_line = -1;
thread_local String GDScriptLanguage::_debug_parse_err_file;
thread_local String GDScriptLanguage::_debug_error;

bool GDScriptLanguage::debug_break_parse(const String &p_file, int p_line, const String &p_error) {
	// break because of parse error

	if (EngineDebugger::is_active() && Thread::get_caller_id() == Thread::get_main_id()) {
		_debug_parse_err_line = p_line;
		_debug_parse_err_file = p_file;
		_debug_error = p_error;
		EngineDebugger::get_script_debugger()->debug(this, false, true);
		// Because this is thread local, clear the memory afterwards.
		_debug_parse_err_file = String();
		_debug_error = String();
		return true;
	} else {
		return false;
	}
}

bool GDScriptLanguage::debug_break(const String &p_error, bool p_allow_continue) {
	if (EngineDebugger::is_active()) {
		_debug_parse_err_line = -1;
		_debug_parse_err_file = "";
		_debug_error = p_error;
		bool is_error_breakpoint = p_error != "Breakpoint";
		EngineDebugger::get_script_debugger()->debug(this, p_allow_continue, is_error_breakpoint);
		// Because this is thread local, clear the memory afterwards.
		_debug_parse_err_file = String();
		_debug_error = String();
		return true;
	} else {
		return false;
	}
}

String GDScriptLanguage::debug_get_error() const {
	return _debug_error;
}

int GDScriptLanguage::debug_get_stack_level_count() const {
	if (_debug_parse_err_line >= 0) {
		return 1;
	}

	return _call_stack_size;
}

int GDScriptLanguage::debug_get_stack_level_line(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return _debug_parse_err_line;
	}

	ERR_FAIL_INDEX_V(p_level, (int)_call_stack_size, -1);

	return *(_get_stack_level(p_level)->line);
}

String GDScriptLanguage::debug_get_stack_level_function(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return "";
	}

	ERR_FAIL_INDEX_V(p_level, (int)_call_stack_size, "");
	GDScriptFunction *func = _get_stack_level(p_level)->function;
	return func ? func->get_name().operator String() : "";
}

String GDScriptLanguage::debug_get_stack_level_source(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return _debug_parse_err_file;
	}

	ERR_FAIL_INDEX_V(p_level, (int)_call_stack_size, "");
	return _get_stack_level(p_level)->function->get_source();
}

void GDScriptLanguage::debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	if (_debug_parse_err_line >= 0) {
		return;
	}

	ERR_FAIL_INDEX(p_level, (int)_call_stack_size);

	CallLevel *cl = _get_stack_level(p_level);
	GDScriptFunction *f = cl->function;

	List<Pair<StringName, int>> locals;

	f->debug_get_stack_member_state(*cl->line, &locals);
	for (const Pair<StringName, int> &E : locals) {
		p_locals->push_back(E.first);

		if (f->constant_map.has(E.first)) {
			p_values->push_back(f->constant_map[E.first]);
		} else {
			p_values->push_back(cl->stack[E.second]);
		}
	}
}

void GDScriptLanguage::debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	if (_debug_parse_err_line >= 0) {
		return;
	}

	ERR_FAIL_INDEX(p_level, (int)_call_stack_size);

	CallLevel *cl = _get_stack_level(p_level);
	GDScriptInstance *instance = cl->instance;

	if (!instance) {
		return;
	}

	Ref<GDScript> scr = instance->get_script();
	ERR_FAIL_COND(scr.is_null());

	const HashMap<StringName, GDScript::MemberInfo> &mi = scr->debug_get_member_indices();

	for (const KeyValue<StringName, GDScript::MemberInfo> &E : mi) {
		p_members->push_back(E.key);
		p_values->push_back(instance->debug_get_member_by_index(E.value.index));
	}
}

ScriptInstance *GDScriptLanguage::debug_get_stack_level_instance(int p_level) {
	if (_debug_parse_err_line >= 0) {
		return nullptr;
	}

	ERR_FAIL_INDEX_V(p_level, (int)_call_stack_size, nullptr);

	return _get_stack_level(p_level)->instance;
}

void GDScriptLanguage::debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	const HashMap<StringName, int> &name_idx = GDScriptLanguage::get_singleton()->get_global_map();
	const Variant *gl_array = GDScriptLanguage::get_singleton()->get_global_array();

	List<Pair<String, Variant>> cinfo;
	get_public_constants(&cinfo);

	for (const KeyValue<StringName, int> &E : name_idx) {
		if (GDScriptAnalyzer::class_exists(E.key) || Engine::get_singleton()->has_singleton(E.key)) {
			continue;
		}

		bool is_script_constant = false;
		for (List<Pair<String, Variant>>::Element *CE = cinfo.front(); CE; CE = CE->next()) {
			if (CE->get().first == E.key) {
				is_script_constant = true;
				break;
			}
		}
		if (is_script_constant) {
			continue;
		}

		const Variant &var = gl_array[E.value];
		bool freed = false;
		const Object *obj = var.get_validated_object_with_check(freed);
		if (obj && !freed) {
			if (Object::cast_to<GDScriptNativeClass>(obj)) {
				continue;
			}
		}

		bool skip = false;
		for (int i = 0; i < CoreConstants::get_global_constant_count(); i++) {
			if (E.key == CoreConstants::get_global_constant_name(i)) {
				skip = true;
				break;
			}
		}
		if (skip) {
			continue;
		}

		p_globals->push_back(E.key);
		p_values->push_back(var);
	}
}

String GDScriptLanguage::debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems, int p_max_depth) {
	List<String> names;
	List<Variant> values;
	debug_get_stack_level_locals(p_level, &names, &values, p_max_subitems, p_max_depth);

	Vector<String> name_vector;
	for (const String &name : names) {
		name_vector.push_back(name);
	}

	Array value_array;
	for (const Variant &value : values) {
		value_array.push_back(value);
	}

	Expression expression;
	if (expression.parse(p_expression, name_vector) == OK) {
		ScriptInstance *instance = debug_get_stack_level_instance(p_level);
		if (instance) {
			Variant return_val = expression.execute(value_array, instance->get_owner());
			return return_val.get_construct_string();
		}
	}

	return String();
}

void GDScriptLanguage::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("gd");
}

void GDScriptLanguage::get_public_functions(List<MethodInfo> *p_functions) const {
	List<StringName> functions;
	GDScriptUtilityFunctions::get_function_list(&functions);

	for (const StringName &E : functions) {
		p_functions->push_back(GDScriptUtilityFunctions::get_function_info(E));
	}

	// Not really "functions", but show in documentation.
	{
		MethodInfo mi;
		mi.name = "preload";
		mi.arguments.push_back(PropertyInfo(Variant::STRING, "path"));
		mi.return_val = PropertyInfo(Variant::OBJECT, "", PROPERTY_HINT_RESOURCE_TYPE, Resource::get_class_static());
		p_functions->push_back(mi);
	}
	{
		MethodInfo mi;
		mi.name = "assert";
		mi.return_val.type = Variant::NIL;
		mi.arguments.push_back(PropertyInfo(Variant::BOOL, "condition"));
		mi.arguments.push_back(PropertyInfo(Variant::STRING, "message"));
		mi.default_arguments.push_back(String());
		p_functions->push_back(mi);
	}
}

void GDScriptLanguage::get_public_constants(List<Pair<String, Variant>> *p_constants) const {
	Pair<String, Variant> pi;
	pi.first = "PI";
	pi.second = Math::PI;
	p_constants->push_back(pi);

	Pair<String, Variant> tau;
	tau.first = "TAU";
	tau.second = Math::TAU;
	p_constants->push_back(tau);

	Pair<String, Variant> infinity;
	infinity.first = "INF";
	infinity.second = Math::INF;
	p_constants->push_back(infinity);

	Pair<String, Variant> nan;
	nan.first = "NAN";
	nan.second = Math::NaN;
	p_constants->push_back(nan);
}

void GDScriptLanguage::get_public_annotations(List<MethodInfo> *p_annotations) const {
	GDScriptParser parser;
	List<MethodInfo> annotations;
	parser.get_annotation_list(&annotations);

	for (const MethodInfo &E : annotations) {
		p_annotations->push_back(E);
	}
}

String GDScriptLanguage::make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const {
	const bool type_hints = true;

	String result = "func " + p_name + "(";
	if (p_args.size()) {
		for (int i = 0; i < p_args.size(); i++) {
			if (i > 0) {
				result += ", ";
			}

			const String name_unstripped = p_args[i].get_slicec(':', 0);
			result += name_unstripped.strip_edges();

			if (type_hints) {
				const String type_stripped = p_args[i].substr(name_unstripped.length() + 1).strip_edges();
				if (!type_stripped.is_empty()) {
					result += ": " + type_stripped;
				}
			}
		}
	}
	result += String(")") + (type_hints ? " -> void" : "") + ":\n" +
			_get_indentation() + "pass # Replace with function body.\n";

	return result;
}

//////// COMPLETION //////////

Error GDScriptLanguage::complete_code(const String &p_code, const String &p_path, Object *p_owner, List<ScriptLanguage::CodeCompletionOption> *r_options, bool &r_forced, String &r_call_hint) {
	return OK;
}

//////// END COMPLETION //////////

String GDScriptLanguage::_get_indentation() const {
	return "\t";
}

void GDScriptLanguage::auto_indent_code(String &p_code, int p_from_line, int p_to_line) const {
	String indent = _get_indentation();

	Vector<String> lines = p_code.split("\n");
	List<int> indent_stack;

	for (int i = 0; i < lines.size(); i++) {
		String l = lines[i];
		int tc = 0;
		for (int j = 0; j < l.length(); j++) {
			if (l[j] == ' ' || l[j] == '\t') {
				tc++;
			} else {
				break;
			}
		}

		String st = l.substr(tc).strip_edges();
		if (st.is_empty() || st.begins_with("#")) {
			continue; //ignore!
		}

		int ilevel = 0;
		if (indent_stack.size()) {
			ilevel = indent_stack.back()->get();
		}

		if (tc > ilevel) {
			indent_stack.push_back(tc);
		} else if (tc < ilevel) {
			while (indent_stack.size() && indent_stack.back()->get() > tc) {
				indent_stack.pop_back();
			}

			if (indent_stack.size() && indent_stack.back()->get() != tc) {
				indent_stack.push_back(tc); // this is not right but gets the job done
			}
		}

		if (i >= p_from_line) {
			l = indent.repeat(indent_stack.size()) + st;
		} else if (i > p_to_line) {
			break;
		}

		lines.write[i] = l;
	}

	p_code = "";
	for (int i = 0; i < lines.size(); i++) {
		if (i > 0) {
			p_code += "\n";
		}
		p_code += lines[i];
	}
}
