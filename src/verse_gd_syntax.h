#pragma once

// GDScript's syntax, read well enough to convert it (docs/gdscript-conversion.md).
//
// A GDExtension cannot reach Godot's own GDScript parser -- it is inside the engine and nothing in
// ClassDB exposes its tree -- so the converter carries its own. It is not a validator: it accepts
// what Godot 4 accepts and builds a tree of it, and anything it does not recognise it keeps as an
// opaque node with its source text, so the converter can say *where* it gave up rather than
// refusing the file. A file that does not parse at all -- unbalanced brackets, a dedent to nowhere
// -- is the one outright refusal, and `error`/`error_line` say where.
//
// No godot-cpp dependency, like the Verse lexer beside it, so the units layer links it without
// Godot.

#include <memory>
#include <string>
#include <vector>

namespace verse_gd {

// --- Tokens ---------------------------------------------------------------------------------

enum class tok {
	ident, // names and keywords alike; the parser tells them apart
	number,
	string,
	op,
	annotation, // `@export`, text without the `@`
	node_path, // `$Path/To/Node` or `$"quoted"`, text is the path
	unique_node, // `%Name`, text is `%Name`
	newline,
	indent,
	dedent,
	eof,
};

struct token {
	tok kind = tok::eof;
	std::string text; // strings are decoded; numbers are the source spelling
	int line = 0; // 1-based
	int col = 0; // 0-based, in bytes
	char string_prefix = 0; // '&' StringName, '^' NodePath, 'r' raw, 0 plain
	bool is_float = false;
};

// A comment, kept so the converter can carry it across. `own_line` is a comment with nothing but
// whitespace before it; otherwise it trails the code on its line.
struct comment {
	int line = 0;
	std::string text; // after the `#`, with `##`'s second `#` dropped too
	bool own_line = false;
};

struct lex_result {
	std::vector<token> tokens;
	std::vector<comment> comments;
	std::string error;
	int error_line = 0;
};

lex_result lex(const std::string &p_source);

// --- The tree -------------------------------------------------------------------------------

struct func_decl;
struct stmt;
struct expr;
using expr_ptr = std::shared_ptr<expr>;
using stmt_ptr = std::shared_ptr<stmt>;
using func_ptr = std::shared_ptr<func_decl>;

enum class ek {
	number,
	string,
	boolean, // text is "true" or "false"
	null,
	self,
	ident,
	node_path, // `$A/B` or `%A`; text is the path as get_node takes it
	array, // args
	dictionary, // args as key, value, key, value...
	unary, // text is the operator ("-", "+", "~", "not"); args[0]
	binary, // text is the operator, normalised: "and", "or", "==", "in", "not in", "is", "is not"...
	ternary, // args: then, condition, else
	call, // args[0] is the callee, the rest are arguments
	attribute, // args[0].text
	subscript, // args[0][args[1]]
	cast, // args[0] as type_text
	await_, // args[0]
	lambda, // func
	super_, // `super` alone -- `super.foo()` is attribute(super_, foo)
	get_node_call, // unused by the parser; reserved for the converter's own rewrites
	opaque, // something this parser skipped: text is its source
};

struct expr {
	ek kind = ek::opaque;
	std::string text;
	std::vector<expr_ptr> args;
	std::string type_text; // `as`/`is`'s type
	char string_prefix = 0;
	bool is_float = false;
	func_ptr lambda;
	int line = 0;
	int end_line = 0;
};

enum class sk {
	expr,
	assign, // text is the operator ("=", "+=", ...); args: target, value
	var, // a local `var`/`const`
	if_, // branches
	while_,
	for_,
	match,
	return_,
	pass,
	break_,
	continue_,
	breakpoint,
};

struct match_branch {
	std::vector<expr_ptr> patterns; // `_` is an ident; a binding `var x` is an opaque "var x"
	expr_ptr guard;
	std::vector<stmt_ptr> body;
	int line = 0;
};

struct if_branch {
	expr_ptr condition; // null for the `else`
	std::vector<stmt_ptr> body;
	int line = 0;
};

struct stmt {
	sk kind = sk::pass;
	int line = 0;
	int end_line = 0;
	std::string text; // assignment operator; the local's name for `var`
	std::string type_text; // the local's declared type; `:=` is "="
	bool is_const = false;
	std::vector<expr_ptr> args; // expr: [e]; assign: [target, value]; var: [init?]; while: [cond];
	                            // for: [iterable]; match: [subject]; return: [value?]
	std::vector<stmt_ptr> body; // while, for
	std::vector<if_branch> branches; // if
	std::vector<match_branch> cases; // match
	std::string for_var; // for
	std::string for_type;
};

struct param {
	std::string name;
	std::string type_text; // "" untyped, "=" for `:=`
	expr_ptr default_value;
};

struct annotation {
	std::string name;
	std::vector<expr_ptr> args;
	int line = 0;
};

struct func_decl {
	std::string name; // "" for an anonymous lambda
	std::vector<param> params;
	std::string return_type; // "" when not written
	std::vector<stmt_ptr> body;
	bool is_static = false;
	std::vector<annotation> annotations;
	int line = 0;
	int end_line = 0;
};

struct var_decl {
	std::string name;
	std::string type_text;
	expr_ptr init;
	std::vector<annotation> annotations;
	bool is_const = false;
	bool is_static = false;
	func_ptr getter; // `get:` block, or the named method of an old `setget`
	func_ptr setter; // `set(value):` block
	std::string setget_setter; // Godot 3's `setget set_x, get_x`, by name
	std::string setget_getter;
	int line = 0;
	int end_line = 0;
};

struct signal_decl {
	std::string name;
	std::vector<param> params;
	std::vector<annotation> annotations;
	int line = 0;
};

struct enum_decl {
	std::string name; // "" for an anonymous enum
	std::vector<std::pair<std::string, expr_ptr>> values;
	int line = 0;
	int end_line = 0;
};

struct class_decl;
using class_ptr = std::shared_ptr<class_decl>;

// One member of a class, in source order, so the converter keeps the author's arrangement.
struct member {
	enum class kind { var, func, signal, enum_, inner_class } kind = kind::var;
	std::shared_ptr<var_decl> var;
	func_ptr func;
	std::shared_ptr<signal_decl> signal;
	std::shared_ptr<enum_decl> enum_;
	class_ptr inner;
	int line = 0;
};

struct class_decl {
	std::string name; // `class_name`, or an inner class's name
	std::string extends; // a class name, or a quoted path
	bool extends_is_path = false;
	std::vector<annotation> annotations; // `@tool`, `@icon(...)`, `@abstract` and the rest
	std::vector<member> members;
	int line = 0;
	int end_line = 0;
};

struct parse_result {
	class_ptr root;
	std::vector<comment> comments;
	std::string error;
	int error_line = 0;
};

parse_result parse(const std::string &p_source);

// The source text of an expression, re-spelled from the tree -- for a TODO comment, which has to
// say what was there.
std::string to_source(const expr_ptr &p_expr);

} // namespace verse_gd
