#include "verse_completion.h"

#include "verse_api_lookup.h"
#include "verse_class_decl.h"
#include "verse_host_abi.h"
#include "verse_keywords.h"
#include "verse_lexer.h"
#include "verse_project_state.h"
#include "verse_runtime.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/color.hpp>

#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#endif

#include <iterator>
#include <vector>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

// Godot drops an option that is missing any one of these keys and prints an error for it, so
// every option is built here rather than assembled piecemeal by each caller.
Dictionary completion_option(const String &p_text, int64_t p_kind, int64_t p_location) {
	Dictionary option;
	option["kind"] = p_kind;
	option["display"] = p_text;
	option["insert_text"] = p_text;
	option["font_color"] = Color(1, 1, 1);
	option["icon"] = Variant();
	option["default_value"] = Variant();
	option["location"] = p_location;
	return option;
}

bool is_identifier_char(char32_t p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || (p_c >= '0' && p_c <= '9') || p_c == '_';
}

// Whether a name is a candidate for what has been typed: the typed characters appear in it in
// order, ignoring case. `Prcs` reaches `Process`.
//
// This has to be at least as permissive as Godot's own filter or it decides the answer by itself.
// CodeEdit fuzzy-matches and ranks the options it is handed, so a name trimmed here is one the
// editor would have offered and never sees -- and a prefix test trims most of what fuzzy matching
// exists to find. A subsequence admits everything a fuzzy matcher would and leaves the ranking,
// which is the half worth having, where it already lives. ASCII folding is the whole of the case
// rule because a Verse identifier is ASCII.
//
// Deliberately narrower than Godot in one direction: this reads the name, while CodeEdit reads
// the rendered display text. An option that displays a whole declaration would otherwise match on
// its parameter types, and `flt` is not what someone typing `float` is looking for.
bool matches_typed_prefix(const String &p_name, const String &p_prefix) {
	auto folded = [](char32_t p_c) { return p_c >= 'A' && p_c <= 'Z' ? p_c - 'A' + 'a' : p_c; };

	int64_t at = 0;
	for (int64_t i = 0; i < p_prefix.length(); i++) {
		while (at < p_name.length() && folded(p_name[at]) != folded(p_prefix[i])) {
			at++;
		}
		if (at >= p_name.length()) {
			return false;
		}
		at++;
	}
	return true;
}

// The same test for the class-name sets, which are the two large ones, with the subsequence rule
// withheld until there is enough typed for it to be a search.
//
// A one-character prefix admits 927 of the 1026 mirrored class names as a subsequence, and CodeEdit
// rebuilds every possible subsequence match of every option it is handed, on the editor's thread,
// on every keystroke. Nothing is gained for it: ranking by contiguity cannot make one of 927 the
// answer either, so under the threshold the set is what the author has actually begun to spell.
// Past it `Prcs` still reaches `Process`, which is the half of fuzzy matching worth having.
bool matches_typed_class_prefix(const String &p_name, const String &p_prefix) {
	constexpr int64_t SUBSEQUENCE_FROM = 3;
	return p_prefix.length() >= SUBSEQUENCE_FROM
			? matches_typed_prefix(p_name, p_prefix)
			: p_name.findn(p_prefix) == 0;
}

// Stands in for the identifier being typed while the completion buffer is analysed. A legal Verse
// identifier, so the line parses; one no project would write, so it resolves to nothing and the
// answer is about the position rather than about whatever it collided with.
const char *completion_placeholder = "VhCompletionCursor";

// The completion buffer as the host must see it: the placeholder already spliced in at
// p_placeholder_start, and the caret's line finished off so uLang keeps the file.
//
// One function because two callers have to agree byte for byte -- _complete_code builds this to
// ask, and completion_placeholder_buffer rebuilds it to decide whether an answer that has just
// landed is still the answer to what is being typed. A repair in one of them alone would make
// every late answer look like a question about some other caret and be dropped.
//
// verse_repair_completion_buffer appends only past the caret, so p_placeholder_start and every
// position the caller measured before it still mean what they meant.
String completion_buffer_repaired(const String &p_spliced, int64_t p_placeholder_start) {
	const String head = p_spliced.substr(0, p_placeholder_start);
	const int64_t line_start = head.rfind("\n") + 1;
	const std::string repaired = verse_repair_completion_buffer(p_spliced.utf8().get_data(),
			(int)head.count("\n"), (int)head.substr(line_start).utf8().length());
	return String::utf8(repaired.c_str());
}

#ifdef TOOLS_ENABLED

// The buffer _complete_code would hand the host for the caret where it is now: the identifier the
// caret is inside replaced by the placeholder, newlines normalised.
//
// Rebuilt from the editor rather than remembered, because it is the test for "still the same
// question". Two keystrokes into one identifier produce the same buffer, which is exactly right --
// the answer that just landed is the answer for both -- while a caret moved elsewhere, or a line
// edited, produces a different one and the refresh is dropped.
String completion_placeholder_buffer(CodeEdit *p_code_edit) {
	const String text = p_code_edit->get_text();
	const int64_t caret_line = p_code_edit->get_caret_line();
	const int64_t caret_column = p_code_edit->get_caret_column();

	// get_text joins the lines with "\n", so one separator per line ahead of the caret's.
	int64_t offset = 0;
	for (int64_t i = 0; i < caret_line; i++) {
		if (i >= p_code_edit->get_line_count()) {
			return String();
		}
		offset += p_code_edit->get_line(i).length() + 1;
	}
	offset += caret_column;
	if (offset < 0 || offset > text.length()) {
		return String();
	}

	int64_t prefix_start = offset;
	while (prefix_start > 0 && is_identifier_char(text[prefix_start - 1])) {
		prefix_start--;
	}
	return completion_buffer_repaired(
			verse_newline_normalized(text.substr(0, prefix_start) + String(completion_placeholder) + text.substr(offset)),
			prefix_start);
}

#endif

// Whether the token ending at p_end is a number rather than a name, which is what tells the `.` of
// `1.5` from the `.` of `Position.X`. An identifier may well end in a digit -- `node2d` does -- so
// it is the whole token that has to be digits, not just the character before the dot.
bool ends_a_number_literal(const String &p_text, int64_t p_end) {
	if (p_end < 0 || p_end >= p_text.length()) {
		return false;
	}
	int64_t start = p_end + 1;
	while (start > 0 && is_identifier_char(p_text[start - 1])) {
		start--;
	}
	for (int64_t i = start; i <= p_end; i++) {
		if (p_text[i] < '0' || p_text[i] > '9') {
			return false;
		}
	}
	return start <= p_end;
}

String word_ending_at(const String &p_text, int64_t p_end) {
	if (p_end < 0 || p_end >= p_text.length() || !is_identifier_char(p_text[p_end])) {
		return String();
	}
	int64_t start = p_end;
	while (start > 0 && is_identifier_char(p_text[start - 1])) {
		start--;
	}
	return p_text.substr(start, p_end - start + 1);
}

bool is_reserved_word(const String &p_word) {
	for (size_t i = 0; i < std::size(verse_keywords::reserved_words); i++) {
		if (p_word == verse_keywords::reserved_words[i]) {
			return true;
		}
	}
	return false;
}

// The offset of the last character of the callee of the innermost call the cursor is inside, or
// -1 when it is inside none.
//
// Scans back over the text before the cursor, closing every bracket it meets, so a nested call's
// arguments do not count as the outer call's. Verse opens a call with `(` and a failable one with
// `[`; `{` opens an archetype, which is a different construct and stops the scan rather than
// answering for it.
//
// Strings and comments are not skipped: a bracket inside either would throw the balance off. The
// cost is a wrong hint for a line with an unbalanced bracket inside a literal, which the host then
// declines to answer for anyway -- the callee that comes out is not a function.
int64_t enclosing_call_callee_end(const String &p_before) {
	int64_t depth = 0;
	for (int64_t i = p_before.length() - 1; i >= 0; i--) {
		const char32_t c = p_before[i];
		if (c == ')' || c == ']' || c == '}') {
			depth++;
		} else if (c == '(' || c == '[') {
			if (depth > 0) {
				depth--;
				continue;
			}
			// An opening bracket with nothing to close it is the call the cursor is inside. Its
			// callee is whatever identifier ends immediately before it; a bracket that opens a
			// group rather than a call has no name there and answers -1.
			int64_t end = i - 1;
			while (end >= 0 && (p_before[end] == ' ' || p_before[end] == '\t')) {
				end--;
			}
			if (end < 0 || !is_identifier_char(p_before[end])) {
				return -1;
			}
			// `if (`, `for (`, `case (`: a block macro's head is not a call, and there is no
			// signature for one -- so without this, every keystroke inside a condition spent a
			// vh_signature_at that could only ever answer VH_ERR_NOT_FOUND.
			return is_reserved_word(word_ending_at(p_before, end)) ? -1 : end;
		} else if (c == '{') {
			return -1;
		} else if (c == '\n' && depth == 0) {
			// Verse continues an argument list across lines, but a cursor on a line that opened no
			// bracket of its own is not inside a call this scan can trust.
			return -1;
		}
	}
	return -1;
}

// The innermost bracket before p_from that nothing has closed, or -1, with the character that
// opened it in r_opener.
//
// Crosses newlines, unlike enclosing_call_callee_end: a class header's parentheses and an
// archetype's braces both wrap, and the positions below are about which construct encloses the
// cursor rather than about one line. Strings and comments are not skipped for the same reason and
// at the same cost as that scan -- and completion declines inside both before reaching here.
int64_t enclosing_open_bracket(const String &p_before, int64_t p_from, char32_t &r_opener) {
	int64_t depth = 0;
	for (int64_t i = p_from - 1; i >= 0; i--) {
		const char32_t c = p_before[i];
		if (c == ')' || c == ']' || c == '}') {
			depth++;
		} else if (c == '(' || c == '[' || c == '{') {
			if (depth > 0) {
				depth--;
				continue;
			}
			r_opener = c;
			return i;
		}
	}
	return -1;
}

// Whether the identifier being typed stands where a type is expected.
//
// A `:` immediately before it is the whole test, because nothing else in Verse puts an identifier
// directly after one on the same line: a block header's `:` ends its line, and the `:` of `:=`
// cannot be adjacent to the name it binds. The run skipped over is the punctuation a type spelling
// puts between the colon and the name -- `:?node`, `:[]string`.
bool completing_a_type(const String &p_before, int64_t p_prefix_start) {
	int64_t at = p_prefix_start;
	while (at > 0) {
		const char32_t c = p_before[at - 1];
		if (c == '?' || c == '[' || c == ']' || c == ' ' || c == '\t') {
			at--;
			continue;
		}
		return c == ':';
	}
	return false;
}

// Whether the cursor stands in the parentheses of a `class(...)`, `struct(...)` or
// `interface(...)` header, where only a class or an interface may be named.
bool completing_a_supertype(const String &p_before, int64_t p_prefix_start) {
	char32_t opener = 0;
	const int64_t bracket = enclosing_open_bracket(p_before, p_prefix_start, opener);
	if (bracket < 0 || opener != '(') {
		return false;
	}
	int64_t end = bracket - 1;
	while (end >= 0 && (p_before[end] == ' ' || p_before[end] == '\t')) {
		end--;
	}
	// `class<unique>(...)` puts the specifiers between the keyword and its parentheses.
	if (end >= 0 && p_before[end] == '>') {
		while (end >= 0 && p_before[end] != '<') {
			end--;
		}
		end--;
		while (end >= 0 && (p_before[end] == ' ' || p_before[end] == '\t')) {
			end--;
		}
	}
	const String word = word_ending_at(p_before, end);
	return word == "class" || word == "struct" || word == "interface";
}

// The last byte of the class named before the `{` the cursor is inside, or -1 when the cursor is
// not naming one of its fields.
//
// Three things disqualify a cursor that is inside the braces. A `{` with no name in front of it is
// a block rather than an archetype. A field that has already been given its `:=` puts the cursor in
// the *value* instead, and the commas separate one field from the next, so the scan back stops at
// one. And a reserved word in front of the brace is one of the constructs that borrows the same
// syntax without taking field names -- `array{...}`, `map{...}`, `enum{...}`, `spawn{...}` -- where
// a field list is not merely the wrong answer but an empty popup, since the position declines to
// append anything else.
int64_t archetype_class_end(const String &p_before, int64_t p_prefix_start) {
	char32_t opener = 0;
	const int64_t bracket = enclosing_open_bracket(p_before, p_prefix_start, opener);
	if (bracket < 0 || opener != '{') {
		return -1;
	}
	for (int64_t i = p_prefix_start - 1; i > bracket; i--) {
		const char32_t c = p_before[i];
		if (c == ',') {
			break;
		}
		if (c == '=') {
			return -1;
		}
	}
	const String name = word_ending_at(p_before, bracket - 1);
	return !name.is_empty() && !is_reserved_word(name) ? bracket - 1 : -1;
}

// Which argument the cursor sits in: the commas between the call's opening bracket and the cursor,
// counted at bracket depth zero so a nested call's own commas do not advance the outer one.
int64_t argument_index_in_call(const String &p_before, int64_t p_callee_end) {
	int64_t depth = 0;
	int64_t index = 0;
	for (int64_t i = p_callee_end + 1; i < p_before.length(); i++) {
		const char32_t c = p_before[i];
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			depth--;
		} else if (c == ',' && depth == 1) {
			index++;
		}
	}
	return index;
}

// Whether the `?` immediately before p_prefix_start opens a *named argument* rather than being one
// of the two other things a `?` spells in Verse.
//
// The three are told apart by what stands before the `?`. A named argument begins an argument, so
// the nearest non-blank character is the call's own bracket or the comma ahead of it; the postfix
// unwrap of `if (Target?)` always follows an expression; and the `?node2d` of an option *type*
// follows a `:`. Only the first has a fixed set of names that may follow it.
bool opens_a_named_argument(const String &p_before, int64_t p_prefix_start) {
	int64_t i = p_prefix_start - 2;
	while (i >= 0 && (p_before[i] == ' ' || p_before[i] == '\t')) {
		i--;
	}
	return i >= 0 && (p_before[i] == '(' || p_before[i] == '[' || p_before[i] == ',');
}

// Which bracket the call the cursor is inside was opened with. vh_signature_desc reports a
// function's parameters but not its effects, so the hint takes the author's own answer: a call
// already written with `[` is the fallible one, and spelling its hint with parentheses contradicts
// the line it sits above.
bool call_opened_with_bracket(const String &p_before, int64_t p_callee_end) {
	for (int64_t i = p_callee_end + 1; i < p_before.length(); i++) {
		if (p_before[i] != ' ' && p_before[i] != '\t') {
			return p_before[i] == '[';
		}
	}
	return false;
}

// The signature as Verse spells it, with the argument the cursor is in wrapped in the markers
// Godot highlights between. Verse's own order -- name, parameters, then `:type` -- rather than
// GDScript's leading return type, because that is how the declaration reads in the file.
String call_hint_for(const Dictionary &p_signature, int64_t p_argument, bool p_fallible) {
	const String name = p_signature["name"];
	const String result_type = p_signature["result"];
	const TypedArray<Dictionary> params = p_signature["params"];

	String hint = name + (p_fallible ? String("[") : String("("));
	for (int64_t i = 0; i < params.size(); i++) {
		if (i > 0) {
			hint += ", ";
		}
		if (i == p_argument) {
			hint += String::chr(0xFFFF);
		}
		const Dictionary param = params[i];
		// A named parameter keeps its `?`, because that is half of how it is written: the call
		// passes it `?ExactMatch := true` and cannot pass it positionally at all, so a hint
		// spelling it `ExactMatch:logic` describes an argument list the compiler would refuse.
		hint += ((bool)param["is_named"] ? String("?") : String()) + String(param["name"]) + String(":") + String(param["type"]);
		if (i == p_argument) {
			hint += String::chr(0xFFFF);
		}
	}
	hint += p_fallible ? "]" : ")";
	if (!result_type.is_empty()) {
		hint += String(":") + result_type;
	}
	return hint;
}

// Godot's kind for a definition, so the completion box draws the right icon beside it.
int64_t completion_kind_for(int64_t p_lookup_kind) {
	switch ((vh_lookup_kind)p_lookup_kind) {
		case VH_LOOKUP_FUNCTION:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION;
		case VH_LOOKUP_CLASS:
		case VH_LOOKUP_TYPE_ALIAS:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS;
		case VH_LOOKUP_ENUM:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_ENUM;
		case VH_LOOKUP_MODULE:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_FILE_PATH;
		case VH_LOOKUP_UNKNOWN:
		case VH_LOOKUP_DATA:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER;
	}
	return ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER;
}

// Whether a call to a function has to be written with brackets rather than parentheses: Verse
// spells a `<decides>` call `GetChild[0]`, and completing it with `(` is a compile error at the
// moment it lands.
//
// Read off the effect specifiers alone rather than searched for anywhere in the signature, because
// a parameter or the result type can be a fallible function *type* -- the infallible
// `(Pred:(:int)<decides>->logic)<transacts>:void` spells `<decides>` too. SpellSignature puts the
// specifiers between the parameter list's closing parenthesis and the `:` before the result type,
// and emits no default values, so counting parentheses finds that one exactly.
bool is_fallible_call(const String &p_signature) {
	int64_t depth = 0;
	for (int64_t i = 0; i < p_signature.length(); i++) {
		const char32_t c = p_signature[i];
		if (c == '(') {
			depth++;
		} else if (c == ')' && --depth == 0) {
			const int64_t result_type = p_signature.find(":", i);
			return p_signature.substr(i, result_type < 0 ? -1 : result_type - i).find("<decides>") >= 0;
		}
	}
	return false;
}

// Turns one vh_complete_item into an option.
//
// A call is completed with its brackets, and where the caret lands afterwards depends on whether
// there is anything to type between them: a function with parameters inserts only the opening one,
// so CodeEdit's brace completion closes it and leaves the caret inside, while one without inserts
// the pair and leaves the caret past it. GDScript spells it exactly this way, ellipsis and all.
Dictionary completion_option_for(const Dictionary &p_item) {
	const String name = p_item["name"];
	const int64_t kind = p_item["kind"];
	const int64_t param_count = p_item["param_count"];
	const bool is_function = kind == VH_LOOKUP_FUNCTION;

	// `location` is the only lever Godot offers over the order options appear in, and the host now
	// says how far each item is from what was asked about -- in the same hops Godot's own
	// LOCATION_PARENT_MASK counts, so a class's own members sort above its parent's above Object's.
	// Before this the whole mirror tied at LOCATION_OTHER and `Position`, `Name` and `Connect` came
	// back in one undifferentiated two-thousand-item list.
	//
	// A -1 is not a distance: it is a name reached through a `using`, which is how all 1026
	// mirrored classes and the Verse standard library come into scope at once. Those keep the older
	// distinction, the only one that could be drawn without the host -- the generated API below
	// anything a script or Verse itself declared.
	const String owner = p_item["owner"];
	const int64_t distance = p_item.get("owner_distance", -1);
	int64_t location = verse_godot_class_for(owner) == nullptr
			? ScriptLanguageExtension::LOCATION_LOCAL
			: ScriptLanguageExtension::LOCATION_OTHER;
	if (distance == 0) {
		location = ScriptLanguageExtension::LOCATION_LOCAL;
	} else if (distance > 0) {
		// Godot reads the low byte as the hop count, so a hierarchy deeper than that saturates
		// rather than wrapping into LOCATION_OTHER_USER_CODE.
		location = ScriptLanguageExtension::LOCATION_PARENT_MASK | (distance > 255 ? 255 : distance);
	}

	Dictionary option = completion_option(name, completion_kind_for(kind), location);
	if (is_function) {
		const bool takes_arguments = param_count > 0;
		const bool fallible = is_fallible_call(p_item["signature"]);
		const String open = fallible ? String("[") : String("(");
		const String close = fallible ? String("]") : String(")");
		option["insert_text"] = name + (takes_arguments ? open : open + close);
		option["display"] = name + open + (takes_arguments ? String::utf8("…") : String()) + close;
	}
	return option;
}

// One named parameter as an option for the `?` the author has just typed.
//
// The `?` is left where it is, exactly as an attribute's `@` is: Godot matches and replaces the run
// of identifier characters past the symbol, so both the text it filters on and the text it
// overwrites are the name alone. The `:=` comes with the name because there is nothing else a
// named argument can be followed by -- a bare `?ExactMatch` is an option type, not an argument.
Dictionary named_argument_option_for(const Dictionary &p_param) {
	const String name = p_param["name"];
	Dictionary option = completion_option(name, ScriptLanguageExtension::CODE_COMPLETION_KIND_VARIABLE,
			ScriptLanguageExtension::LOCATION_LOCAL);
	option["insert_text"] = name + String(" := ");
	option["display"] = name + String(":") + String(p_param["type"]);
	return option;
}

// Every class a receiver of type p_verse_class reaches a member of, nearest first.
//
// Only a mirrored hierarchy is walked past its first link. A script class's own base is written in
// its file and in no table the host answers for, so following it would mean reading another file on
// the keystroke that opened the popup -- which costs more than a partial answer is worth when the
// full one is a frame or two behind it.
PackedStringArray member_bearing_chain(const String &p_verse_class) {
	PackedStringArray chain;
	if (p_verse_class.is_empty()) {
		return chain;
	}
	chain.push_back(p_verse_class);
	const char *godot_name = godot_classdb_class_for(p_verse_class);
	if (godot_name == nullptr) {
		return chain;
	}
	for (String name = ClassDB::get_parent_class(String(godot_name)); !name.is_empty();
			name = ClassDB::get_parent_class(name)) {
		if (const char *verse_name = mirrored_class(name)) {
			chain.push_back(String(verse_name));
		}
	}
	return chain;
}

// The class a member's declared type names, or empty. A `var` reads as `^node2d` and an `option`
// as `?node2d`; anything with a bracket in it is a container or a function type, which names no one
// class and is not worth a guess.
String class_named_by_type(const String &p_type) {
	String name = p_type.strip_edges();
	while (!name.is_empty() && (name[0] == '^' || name[0] == '?')) {
		name = name.substr(1).strip_edges();
	}
	if (name.is_empty() || name.find("(") >= 0 || name.find("[") >= 0 || name.find(":") >= 0) {
		return String();
	}
	return name;
}

// One inherited method as the declaration that would override it, which is what GDScript
// completes inside a class body -- the whole `func _ready() -> void:` rather than the name.
//
// The Verse spelling of the same thing is the base's own signature with `<override>` after the
// name, which is why the signature is asked of the compiler rather than rebuilt from the type:
// an override must match what it overrides, parameter names included, and the function type
// drops those. The trailing ` =` is where the body goes; the editor's auto-indent takes the
// caret there on the next Enter, so nothing here inserts a newline of its own.
//
// LOCATION_LOCAL unconditionally, mirrored Godot class or not: at a position where a member is
// being declared, an override is what was asked for and belongs above the rest of the scope.
Dictionary override_option_for(const Dictionary &p_item) {
	const String declaration = String(p_item["name"]) + String("<override>") + String(p_item["signature"]) + String(" =");
	return completion_option(declaration, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, ScriptLanguageExtension::LOCATION_LOCAL);
}

// Whether an option is an inherited method worth offering as a declaration -- one the compiler
// would take the override of *and* something would dispatch to.
//
// `is_overridable` answers only the first half, and on its own it offers the whole mirrored Godot
// API: every one of those 9597 methods is a class member the compiler would accept an override of,
// and overriding `GetName` compiles and changes nothing, because the body forwards through the
// handle either way. The second half is `is_virtual` on the method table, which is generated from
// the same `is_virtual` in extension_api.json that decided how to emit the member in the first
// place -- so the two cannot disagree.
//
// This used to exclude the mirror wholesale, and that was right until it wasn't: when the guard
// was written Godot's virtuals were hand-written on the native root, so "not a mirrored class" and
// "a virtual" named the same set. Phase 4 generated all 1413 of them onto the classes that declare
// them -- `_Ready` onto `node`, `_Draw` onto `canvas_item` -- and the guard silently began
// rejecting every override this exists to offer. Ask the table what the member *is* rather than
// where it lives (by-hand-findings.md B1).
//
// A name the table does not carry at all is a class the author wrote, which is worth offering, or
// a member of the native root that is not `_Notification`, which is not.
//
// A method the class already declares comes back owned by that class -- the host lets a subclass'
// copy win over the superclass' and drops the duplicate -- so comparing the owner is what stops an
// override that is already written from being offered again.
bool completes_as_override(const Dictionary &p_item, const String &p_enclosing_class) {
	const String owner = p_item["owner"];
	// The owner of a script class's member is module-qualified -- `left/widget` -- and the class
	// this is declaring in was read off the buffer, where only the bare name is written.
	if (!(bool)p_item["is_overridable"] || String(p_item["signature"]).is_empty() || owner.get_file() == p_enclosing_class) {
		return false;
	}
	if (const verse_api::method_mapping *mirrored = godot_method_for(owner, p_item["name"])) {
		return mirrored->is_virtual;
	}
	return verse_godot_class_for(owner) == nullptr && owner != String("vh_object");
}

// A line's indentation width, or -1 for one carrying no code -- blank, or a comment, which sits
// at whatever column it was written at and so says nothing about the block it is in.
int64_t code_line_indent(const String &p_line) {
	int64_t i = 0;
	while (i < p_line.length() && (p_line[i] == ' ' || p_line[i] == '\t')) {
		i++;
	}
	if (i >= p_line.length() || p_line[i] == '#') {
		return -1;
	}
	return i;
}

// The name a line of code begins with, or empty for a line that begins with anything else.
String leading_identifier(const String &p_line) {
	int64_t start = 0;
	while (start < p_line.length() && (p_line[start] == ' ' || p_line[start] == '\t')) {
		start++;
	}
	int64_t end = start;
	while (end < p_line.length() && is_identifier_char(p_line[end])) {
		end++;
	}
	return p_line.substr(start, end - start);
}

// The class whose members are declared at p_line/p_indent, or empty when the cursor is somewhere
// a name is used rather than declared. Both answers come from one scan because an override option
// needs them together: whether to offer declarations at all, and which class' own methods are
// already written and so must not be offered again.
//
// r_declared is the second: every name the class body already begins a line with at the cursor's
// own indentation, which is where its members are. The host answers the same question off its
// snapshot, and correctly -- but the snapshot describes the last analysed text, and the method the
// author just finished typing is exactly what is not in it.
//
// The cursor's own line is left out. It carries whatever has been typed of the name being
// completed, and a prefix that happens to spell a whole inherited name would otherwise withdraw
// the one candidate the author is reaching for.
//
// Read from the text, because Godot asks for completion on every keystroke and an analysis of a
// half-written declaration would not report the class it belongs to anyway. The test is the one
// the indentation already encodes: every line of code between the class and the cursor is
// indented at least as far as the cursor, since a line indented less would be the header of the
// block the cursor is really inside.
//
// Only the class named after the file is found, so a member being added to one of the file's
// *other* top-level classes completes as an ordinary name. That is the conservative direction,
// and the file's own class is the one a script is actually written in.
String member_declaration_class(const String &p_source, const String &p_file_stem, int64_t p_line, int64_t p_indent, PackedStringArray &r_declared) {
	const VerseClassDecl decl = verse_scan_class_decl(p_source.utf8().get_data(), p_file_stem.utf8().get_data());
	if (decl.line < 0 || p_line <= decl.line) {
		return String();
	}

	const PackedStringArray lines = p_source.split("\n");
	if (decl.line >= lines.size() || p_line >= lines.size() || p_indent <= code_line_indent(lines[decl.line])) {
		return String();
	}

	for (int64_t i = p_line - 1; i > decl.line; i--) {
		const int64_t indent = code_line_indent(lines[i]);
		if (indent >= 0 && indent < p_indent) {
			return String();
		}
	}

	// Forward as well as back: a member declared below the cursor is as written as one above it.
	for (int64_t i = decl.line + 1; i < lines.size(); i++) {
		const int64_t indent = code_line_indent(lines[i]);
		if (indent >= 0 && indent < p_indent) {
			break;
		}
		if (indent != p_indent || i == p_line) {
			continue;
		}
		const String name = leading_identifier(lines[i]);
		if (!name.is_empty()) {
			r_declared.push_back(name);
		}
	}
	return String(decl.name.c_str());
}

// The offset of the quote that opened the string literal the cursor is inside, or -1.
//
// Only ever asked after the lexer has said the cursor is in one, so this is a backward scan for the
// nearest unescaped quote rather than a second opinion about where strings begin. A Verse string
// does not span lines, so the scan stops at one.
int64_t enclosing_string_start(const String &p_before) {
	for (int64_t i = p_before.length() - 1; i >= 0; i--) {
		const char32_t c = p_before[i];
		if (c == '\n') {
			return -1;
		}
		if (c != '"') {
			continue;
		}
		int64_t backslashes = 0;
		while (i - 1 - backslashes >= 0 && p_before[i - 1 - backslashes] == '\\') {
			backslashes++;
		}
		if (backslashes % 2 == 0) {
			return i;
		}
	}
	return -1;
}

// What a string literal is naming, when it is naming something the editor can enumerate.
enum class string_argument {
	none,
	node_path,
	resource_path,
	input_action,
	signal_name,
};

// The callee whose argument it is, which is the same key GDScript answers this question by --
// Object::get_argument_options, a virtual no GDExtension can reach. The callee is a word in the
// buffer, though, so the table is written out here instead of asked for.
//
// Names are the mirror's, which is Godot's own PascalCased: `get_node` is `GetNode`. An argument of
// -1 means every string argument of that call is one of these, which is what `GetVector`'s four
// action names need.
struct string_argument_rule {
	const char *callee;
	string_argument kind;
	int argument;
};

constexpr string_argument_rule string_argument_rules[] = {
	{ "GetNode", string_argument::node_path, 0 },
	{ "HasNode", string_argument::node_path, 0 },
	{ "FindChild", string_argument::node_path, 0 },
	{ "FindChildren", string_argument::node_path, 0 },
	{ "GetNodeAndResource", string_argument::node_path, 0 },

	{ "Load", string_argument::resource_path, 0 },
	{ "Exists", string_argument::resource_path, 0 },
	{ "LoadThreadedRequest", string_argument::resource_path, 0 },
	{ "ChangeSceneToFile", string_argument::resource_path, 0 },
	{ "Save", string_argument::resource_path, 1 },

	{ "IsActionPressed", string_argument::input_action, 0 },
	{ "IsActionJustPressed", string_argument::input_action, 0 },
	{ "IsActionJustReleased", string_argument::input_action, 0 },
	{ "IsActionReleased", string_argument::input_action, 0 },
	{ "IsAction", string_argument::input_action, 0 },
	{ "GetActionStrength", string_argument::input_action, 0 },
	{ "GetActionRawStrength", string_argument::input_action, 0 },
	{ "ActionPress", string_argument::input_action, 0 },
	{ "ActionRelease", string_argument::input_action, 0 },
	{ "HasAction", string_argument::input_action, 0 },
	{ "EraseAction", string_argument::input_action, 0 },
	{ "GetAxis", string_argument::input_action, -1 },
	{ "GetVector", string_argument::input_action, -1 },

	{ "Connect", string_argument::signal_name, 0 },
	{ "Disconnect", string_argument::signal_name, 0 },
	{ "IsConnected", string_argument::signal_name, 0 },
	{ "HasSignal", string_argument::signal_name, 0 },
	{ "GetSignalConnectionList", string_argument::signal_name, 0 },
	{ "MakeSignal", string_argument::signal_name, 1 },
};

string_argument string_argument_kind(const String &p_callee, int64_t p_argument) {
	for (size_t i = 0; i < std::size(string_argument_rules); i++) {
		const string_argument_rule &rule = string_argument_rules[i];
		if (p_callee == rule.callee && (rule.argument < 0 || rule.argument == p_argument)) {
			return rule.kind;
		}
	}
	return string_argument::none;
}

// Every descendant of p_base, spelled the way a NodePath argument to GetNode wants it.
void collect_node_paths(Node *p_base, Node *p_from, Array &r_options) {
	for (int64_t i = 0; i < p_from->get_child_count(); i++) {
		Node *child = p_from->get_child(i);
		if (child == nullptr) {
			continue;
		}
		r_options.push_back(completion_option(String(p_base->get_path_to(child)),
				ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
				ScriptLanguageExtension::LOCATION_LOCAL));
		collect_node_paths(p_base, child, r_options);
	}
}

// Every file the editor's filesystem knows about, which is where a res:// path argument points.
// EditorFileSystem rather than a DirAccess walk for the reason GDScript uses it: the editor has
// already scanned the project, and a completion is not the place to scan it again.
void collect_resource_paths(Array &r_options) {
#ifdef TOOLS_ENABLED
	EditorInterface *editor = EditorInterface::get_singleton();
	EditorFileSystem *filesystem = editor != nullptr ? editor->get_resource_filesystem() : nullptr;
	if (filesystem == nullptr) {
		return;
	}
	std::vector<EditorFileSystemDirectory *> pending;
	pending.push_back(filesystem->get_filesystem());
	while (!pending.empty()) {
		EditorFileSystemDirectory *directory = pending.back();
		pending.pop_back();
		if (directory == nullptr) {
			continue;
		}
		for (int32_t i = 0; i < directory->get_file_count(); i++) {
			r_options.push_back(completion_option(directory->get_file_path(i),
					ScriptLanguageExtension::CODE_COMPLETION_KIND_FILE_PATH,
					ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
		}
		for (int32_t i = 0; i < directory->get_subdir_count(); i++) {
			pending.push_back(directory->get_subdir(i));
		}
	}
#endif
}

// The project's input actions, which live in the settings under `input/` and nowhere else -- there
// is no InputMap to ask in the editor, because the editor does not load the project's own map.
void collect_input_actions(Array &r_options) {
	const TypedArray<Dictionary> settings = ProjectSettings::get_singleton()->get_property_list();
	for (int64_t i = 0; i < settings.size(); i++) {
		const String name = Dictionary(settings[i])["name"];
		if (!name.begins_with("input/")) {
			continue;
		}
		r_options.push_back(completion_option(name.substr(6),
				ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
	}
}

// The byte an archetype receiver's class name ends at -- the `main_script` of `main_script{}.`
// -- or p_receiver_end itself when the receiver is not an archetype, or -1 for one this cannot
// read.
//
// A `}` where the receiver ends closes an archetype, and the class is the word in front of the
// brace that opened it. A quote anywhere in the span is where this declines rather than
// guesses: a brace inside a string literal counts as one to a scan like this, and a wrong
// member list is worse than a late one.
int64_t archetype_receiver_end(const String &p_source, int64_t p_receiver_end) {
	if (p_receiver_end < 0 || p_receiver_end >= p_source.length() || p_source[p_receiver_end] != '}') {
		return p_receiver_end;
	}
	int64_t depth = 0;
	for (int64_t scan = p_receiver_end; scan >= 0; scan--) {
		const char32_t at = p_source[scan];
		if (at == '"') {
			return -1;
		}
		if (at == '}') {
			depth++;
		} else if (at == '{') {
			depth--;
			if (depth == 0) {
				return scan - 1;
			}
		}
	}
	return -1;
}

} // namespace

bool completing_in_comment(const String &p_code, int64_t p_marker) {
	const String before = p_code.substr(0, p_marker);
	const int64_t line_start = before.rfind("\n") + 1;
	const String source = before + p_code.substr(p_marker + 1);
	return verse_position_in_comment(source.utf8().get_data(),
			(int)before.count("\n"), (int)before.substr(line_start).utf8().length());
}

bool completing_in_string(const String &p_code, int64_t p_marker) {
	const String before = p_code.substr(0, p_marker);
	const int64_t line_start = before.rfind("\n") + 1;
	const String source = before + p_code.substr(p_marker + 1);
	return verse_position_in_string(source.utf8().get_data(),
			(int)before.count("\n"), (int)before.substr(line_start).utf8().length());
}

// The classes whose members a `.` at p_receiver_end reaches, nearest first, decided from the
// buffer and the snapshot alone -- which is the whole of what exists while the analysis that would
// answer properly is still running.
//
// Four receivers are knowable without one. `Self` is the class this file declares, and the class
// header names its base. A bare name the class declares carries a declared type, which the snapshot
// spells. A mirrored class written outright is its own answer. And an archetype names its class in
// front of its own brace. Everything else -- a call's result, a local, a dotted chain -- needs the
// types this deliberately does not build, and answers nothing rather than guessing: a wrong member
// list is worse than a late one, because the author acts on it.
PackedStringArray VerseCompletion::receiver_classes_from_text(const String &p_source, const String &p_path, int64_t p_receiver_end) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return PackedStringArray();
	}

	const int64_t class_end = archetype_receiver_end(p_source, p_receiver_end);
	const bool is_archetype = class_end != p_receiver_end;
	const String word = class_end < 0 ? String() : word_ending_at(p_source, class_end);
	if (word.is_empty()) {
		return PackedStringArray();
	}
	const int64_t word_start = class_end - word.length() + 1;
	if (word_start > 0 && p_source[word_start - 1] == '.') {
		return PackedStringArray();
	}

	// The word in front of an archetype's brace is a type by construction, so it is taken as one
	// without the table test the bare-name path below needs -- which is what reaches a class the
	// project declares and a generated binding alike, neither of which is in that table.
	if (is_archetype) {
		return member_bearing_chain(word);
	}

	const String own_class = language.qualified_class_name(p_path);
	if (word == String("Self")) {
		PackedStringArray chain;
		chain.push_back(own_class);
		// The base out of the buffer rather than out of the snapshot: a class header the author
		// has just changed is exactly the case this branch exists for, and verse_scan_class_decl
		// reads the text in front of them.
		const VerseClassDecl decl = verse_scan_class_decl(
				verse_newline_normalized(p_source).utf8().get_data(),
				p_path.get_file().get_basename().utf8().get_data());
		if (!decl.name.empty()) {
			if (const char *verse_base = mirrored_class(language.base_types_for(decl).instance_base)) {
				chain.append_array(member_bearing_chain(String(verse_base)));
			}
		}
		return chain;
	}

	const TypedArray<Dictionary> members = runtime->class_members(own_class);
	for (int64_t i = 0; i < members.size(); i++) {
		const Dictionary item = members[i];
		if (String(item["name"]) == word) {
			return member_bearing_chain(class_named_by_type(item["type"]));
		}
	}

	return verse_godot_class_for(word) != nullptr ? member_bearing_chain(word) : PackedStringArray();
}

// The last analysis only describes a class when the whole project parsed, so a class it says
// nothing about is answered with what it said the last time it did. `vh_has_class` is not that
// test: around an analysis that did not parse it can still answer yes for a class it describes no
// member of. A class with members of its own and no candidates is answered fresh -- its
// base changed, and the old list would offer overrides of a base it no longer extends. Before any
// analysis has landed the host says so rather than answering empty, and r_not_ready passes that on.
TypedArray<Dictionary> VerseCompletion::override_candidates(const String &p_class_name, bool *r_not_ready) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return TypedArray<Dictionary>();
	}
	bool not_ready = false;
	const TypedArray<Dictionary> candidates = runtime->class_override_candidates(p_class_name, &not_ready);
	if (not_ready && r_not_ready != nullptr) {
		*r_not_ready = true;
	}
	if (!not_ready && (!candidates.is_empty() || !runtime->class_members(p_class_name).is_empty())) {
		last_good_override_candidates[p_class_name] = candidates;
		return candidates;
	}
	const TypedArray<Dictionary> *last_good = last_good_override_candidates.getptr(p_class_name);
	return last_good != nullptr ? *last_good : candidates;
}

// What a string literal at the cursor can be completed to, or nothing.
//
// Godot re-quotes every option handed back while the caret is inside a string (CodeEdit's
// _filter_code_completion_candidates), so the names here are bare and the editor puts the quotes
// back. Forced, because the popup is worth opening on the quote itself: none of these four is a
// name the author can be expected to have typed a prefix of.
void VerseCompletion::complete_in_string(const String &p_code, const String &p_path, int64_t p_marker, Object *p_owner, Dictionary &r_result) const {
	const String before = p_code.substr(0, p_marker);
	const int64_t quote = enclosing_string_start(before);
	if (quote < 0) {
		return;
	}

	// The call scan starts at the quote rather than at the cursor: a bracket inside the literal is
	// text, and letting it count would make `GetNode("(")` look like a call that opened one.
	const String head = before.substr(0, quote);
	const int64_t callee_end = enclosing_call_callee_end(head);
	if (callee_end < 0) {
		return;
	}
	const String callee = word_ending_at(head, callee_end);
	const string_argument kind = string_argument_kind(callee, argument_index_in_call(head, callee_end));

	Array options;
	switch (kind) {
		case string_argument::node_path: {
			// The node this script is attached to in the edited scene, which Godot resolves before
			// asking and hands over here. Without one -- a script open with no scene around it --
			// there is no tree to name paths against.
			if (Node *base = Object::cast_to<Node>(p_owner)) {
				collect_node_paths(base, base, options);
			}
		} break;
		case string_argument::resource_path:
			collect_resource_paths(options);
			break;
		case string_argument::input_action:
			collect_input_actions(options);
			break;
		case string_argument::signal_name:
			collect_signal_names(p_code, p_path, callee_end - callee.length() - 1, options);
			break;
		case string_argument::none:
			return;
	}

	if (options.is_empty()) {
		return;
	}
	r_result["options"] = options;
	r_result["force"] = true;
}

// The signals the receiver of a `Connect`-like call can be asked for.
//
// Two sources, because a scripted node has two kinds: the Verse-spelled ones its own class declares
// (`Hit`, registered with Godot under that name) and Godot's own snake_case ones, which the nearest
// mirrored ancestor answers for with inheritance included. The chain is the same one member
// completion resolves a receiver by, so `Self.Connect("` and `Enemy.Connect("` both work.
void VerseCompletion::collect_signal_names(const String &p_source, const String &p_path, int64_t p_receiver_end, Array &r_options) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	PackedStringArray chain = receiver_classes_from_text(p_source, p_path, p_receiver_end);
	if (chain.is_empty()) {
		chain.push_back(language.qualified_class_name(p_path));
	}

	for (int64_t i = 0; i < chain.size(); i++) {
		if (const char *godot_class = godot_classdb_class_for(chain[i])) {
			// With inheritance, so the first mirrored class in the chain is the last one to ask.
			const TypedArray<Dictionary> signals = ClassDB::class_get_signal_list(String(godot_class), false);
			for (int64_t s = 0; s < signals.size(); s++) {
				r_options.push_back(completion_option(Dictionary(signals[s])["name"],
						ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL,
						ScriptLanguageExtension::LOCATION_OTHER));
			}
			return;
		}

		const Vector<VerseSignalInfo> declared = runtime->class_signals(chain[i]);
		for (int64_t s = 0; s < declared.size(); s++) {
			r_options.push_back(completion_option(String(declared[s].name),
					ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL,
					ScriptLanguageExtension::LOCATION_LOCAL));
		}
	}
}

// Completion, answered by the compiler wherever it can be.
//
// Godot marks the cursor by splicing U+FFFF into the buffer, and everything here is derived from
// where that landed: what precedes it -- a `.`, so this completes members of whatever is to the
// left, an `@`, so it completes the attributes in scope, or neither, so it completes names -- and
// what has been typed of the identifier so far.
//
// The buffer handed to the host has that half-typed identifier replaced by a fixed one that
// nothing defines. Replacing rather than deleting keeps the line parsing as the identifier it
// was going to be -- `Position.` and `set X = ` are both syntax errors, and a parse error can
// take the enclosing function's AST with it, while an unknown identifier costs one diagnostic
// nobody sees. And the substitution is what makes every keystroke of one identifier the same
// question, and so a cache hit: the host is already holding that buffer, so the answer is the warm
// half-millisecond rather than a ~750 ms analysis behind every character.
//
// The class-name and keyword sets are still offered alongside the compiler's answer for a bare
// identifier. They cover what a scope walk cannot -- a class the author has not brought into
// view, and the keywords, which are not definitions at all.
//
// One position answers differently: a bare identifier on a line of its own inside a class body is
// a member being declared, and an inherited method offered there completes to the whole
// declaration that overrides it rather than to a call. Everything else in scope is still offered,
// so a misread of the position costs nothing beyond an option that was already going to be there.
Dictionary VerseCompletion::complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = (int64_t)OK;
	result["force"] = false;
	result["call_hint"] = String();

	const int64_t marker = p_code.find(String::chr(0xFFFF));
	if (marker < 0) {
		return result;
	}

	// A comment is prose, and every set below is names. Godot raises the popup on its own as soon
	// as one of them matches what is being typed, so answering here puts the Godot API over the
	// middle of a sentence.
	if (completing_in_comment(p_code, marker)) {
		return result;
	}

	// A string is not names either, and for a while answered nothing for that reason. What it holds
	// is decided by the call it is an argument to, and four of those name something the editor can
	// enumerate -- which is the whole of what GDScript completes inside a string too.
	if (completing_in_string(p_code, marker)) {
		complete_in_string(p_code, p_path, marker, p_owner, result);
		return result;
	}

	// What has been typed, which every set below is trimmed against before it is handed over.
	// Godot filters again and does the ranking, but only after building an option for each of a
	// thousand class names, on every keystroke.
	const String before = p_code.substr(0, marker);
	int64_t prefix_start = before.length();
	while (prefix_start > 0 && is_identifier_char(before[prefix_start - 1])) {
		prefix_start--;
	}
	const String prefix = before.substr(prefix_start);

	// The receiver is whatever ends immediately before the dot. Only a dot: `?` and `^` are
	// postfix operators the host unwraps on its own, and a space between the two is not something
	// Verse writes.
	const int64_t receiver_end = prefix_start - 2;
	const bool completing_members = prefix_start > 0
			&& before[prefix_start - 1] == '.'
			&& !ends_a_number_literal(before, receiver_end);

	// An `@` means an attribute and nothing else, so the whole scope is the wrong answer there:
	// `@e` is reaching for `editable`, not for every name in the project that contains an e.
	//
	// The names offered are bare, the `@` left where it is. Godot's own filter walks back over
	// identifier characters and stops at the symbol, so the text it is matching against and the
	// text it replaces on insert are both the part past it -- GDScript strips the `@` off its
	// annotations for exactly this reason.
	const bool completing_attribute = !completing_members && prefix_start > 0 && before[prefix_start - 1] == '@';

	// A `<` is the other half of the same idea and a different set of names: Verse refuses an
	// attribute in the wrong position, so `public` is spellable only as `<public>` and `editable`
	// only as `@editable` (SemanticAnalyzer's ErrSemantic_InvalidAttributeScope). The host answers
	// the two as separate modes rather than one list asked for twice.
	//
	// Unlike `@` this is not forced, and a bare `<` with nothing typed falls through to the empty
	// prefix refusal below. `A<B` is a comparison Verse writes without spaces, and popping a
	// specifier list over one is noise; a specifier is always reached for with letters after it.
	const bool completing_specifier = !completing_members && !completing_attribute
			&& prefix_start > 0 && before[prefix_start - 1] == '<';

	// A `?` at the head of an argument is Verse's named-argument spelling -- `?ExactMatch := true`
	// -- and the only names that can stand there are the callee's own named parameters. The scope
	// is as wrong an answer here as it is after an `@`, and wrong in a worse way: every name in it
	// is refused at that position rather than merely unlikely.
	const bool completing_named_argument = !completing_members && !completing_attribute && !completing_specifier
			&& prefix_start > 0 && before[prefix_start - 1] == '?'
			&& opens_a_named_argument(before, prefix_start);

	const int64_t line_start = before.rfind("\n") + 1;
	const String ahead_of_prefix = before.substr(line_start, prefix_start - line_start);

	// The four positions that bound the answer by what Verse will accept there rather than by what
	// is in scope. Each is decided from the buffer alone, and each is a construct with no second
	// reading -- which is the whole reason to narrow on them and not on, say, an argument, where
	// any expression is legal and a narrowed list would hide the right name.
	//
	// Tested in this order: a `.`, an `@`, a `<` and a `?` above have already claimed the cursor,
	// and a `set` target and a type position cannot both be true of one caret.
	const bool bounded = !completing_members && !completing_attribute && !completing_specifier
			&& !completing_named_argument;
	const bool completing_assignable = bounded && ahead_of_prefix.strip_edges() == String("set");
	const bool completing_type = bounded && !completing_assignable && completing_a_type(before, prefix_start);
	const bool completing_supertype = bounded && !completing_assignable && !completing_type
			&& completing_a_supertype(before, prefix_start);
	const int64_t archetype_end = bounded && !completing_assignable && !completing_type && !completing_supertype
			? archetype_class_end(before, prefix_start)
			: -1;
	const bool completing_field = archetype_end >= 0;

	// Whether the position's own answer is the whole of it. The class-name and keyword sets
	// appended at the end are names written on their own, so they belong to a bare identifier and
	// to a type position -- a mirrored class is a type -- and nowhere else.
	const bool answers_alone = completing_members || completing_attribute || completing_specifier
			|| completing_assignable || completing_field;
	const bool appends_keywords = !answers_alone && !completing_supertype;

	// A bare identifier, which is the only position the snapshot fallback and the override
	// declarations below are about: the others each have a narrower question to ask.
	const bool completing_a_bare_name = bounded && !completing_assignable && !completing_type
			&& !completing_supertype && !completing_field;

	// The class this is adding a member to, when that is what the cursor is doing: nothing but
	// indentation ahead of the prefix on its line, and that line belonging to the class body. An
	// inherited method offered there is being declared rather than called, and completes to the
	// whole declaration.
	PackedStringArray already_declared;
	const String declaring_in_class = completing_a_bare_name && !ahead_of_prefix.is_empty() && ahead_of_prefix.strip_edges().is_empty()
			? member_declaration_class(verse_newline_normalized(p_code), p_path.get_file().get_basename(),
					  before.count("\n"), ahead_of_prefix.length(), already_declared)
			: String();

	Array options;

	// Every name the host's answer already offered, so the sets appended below skip a row they
	// would otherwise duplicate. The overlap is not partial: a scope the file's `using` has
	// brought /Godot.org/Godot into carries all 1026 mirrored class names, and 28 of the 156
	// reserved words come back as the types they are (`int`, `float`, `logic`, ...), so without
	// this a prefix of `node` drew 178 duplicate rows and `spr` 169. Godot deduplicates nothing.
	//
	// Keyed by the item's bare `name` rather than by the option text: an override completes to a
	// whole declaration, and the appended sets are bare spellings.
	HashSet<String> host_offered_names;

	VerseRuntime *runtime = get_runtime();
	const bool host_can_answer = language.is_built() && runtime != nullptr && runtime->is_host_loaded();

	// The buffer as the compiler should see it: marker gone, and the identifier being typed
	// standing in for whatever it will become. The same text for every prefix of one identifier,
	// and one no script can collide with -- a name Verse code could define would make the
	// substitution resolve to it. Shared by the options below and the argument hint, which is what
	// lets the host answer both off one analysis.
	//
	// Repaired, because a half-written line is a *parse* error and uLang keeps no partial snippet:
	// without it a caret anywhere inside `if (...)` answered nothing at all.
	const String source = completion_buffer_repaired(
			verse_newline_normalized(before.substr(0, prefix_start) + String(completion_placeholder) + p_code.substr(marker + 1)),
			prefix_start);

	// The zero-based row and utf8 byte column of a character offset into that buffer, which is how
	// the compiler counts and is not how Godot counts.
	auto position_of = [&source](int64_t p_offset, int64_t &r_line, int64_t &r_column) {
		const String up_to = source.substr(0, p_offset);
		r_line = up_to.count("\n");
		const int64_t line_start = up_to.rfind("\n") + 1;
		r_column = up_to.substr(line_start).utf8().length();
	};

	// Whether signature_cache describes the call this buffer's cursor is inside, which the named
	// argument options below need as well as the hint does: the names that may stand past a `?`
	// are the callee's parameters, and the cache is where they already are.
	bool signature_is_current = false;

	// The argument hint, which is what Godot draws above the caret while a call is open. Asked
	// before the options because it is the answer for a cursor with nothing typed at all -- the
	// moment right after the `(` -- which is exactly where the options below decline.
	if (host_can_answer) {
		const int64_t callee_end = enclosing_call_callee_end(before);
		if (callee_end >= 0) {
			int64_t line = 0;
			int64_t column = 0;
			position_of(callee_end, line, column);

			bool have_signature = signature_cache_source == source
					&& signature_cache_line == (int32_t)line
					&& signature_cache_column == (int32_t)column
					&& signature_cache_epoch == language.analysis_epoch_value();
			if (!have_signature) {
				const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
				bool not_ready = false;
				const Dictionary answer = runtime->signature_at(globalized, source, (int32_t)line, (int32_t)column, &not_ready);
				if (not_ready) {
					// The host has never analysed this buffer, and since ABI v7 it will not do it
					// here. Queue it and draw no hint; the analysis lands in a later _frame, which
					// asks the editor to complete again and arrives back here with an answer.
					language.request_check(p_path, source, VerseProjectState::CheckKind::COMPLETION);
				} else {
					signature_cache = answer;
					signature_cache_source = source;
					signature_cache_line = (int32_t)line;
					signature_cache_column = (int32_t)column;
					signature_cache_epoch = language.analysis_epoch_value();
					have_signature = true;
				}
			}

			signature_is_current = have_signature;

			// Only ever the hint for *this* buffer. The cache keys are left alone on a refusal, so
			// without the guard the previous call's hint would be drawn over the new one.
			if (have_signature && !signature_cache.is_empty()) {
				result["call_hint"] = call_hint_for(signature_cache, argument_index_in_call(before, callee_end), call_opened_with_bracket(before, callee_end));
			}
		}
	}

	// Without a dot, a bare cursor would offer every name in scope as one undifferentiated list;
	// with one, the member set is bounded by the receiver's type and is exactly what was asked
	// for. An `@` bounds it just as tightly, and is worth showing unprompted for the same reason:
	// the attributes in scope are a short list and nothing else can follow it. So are an
	// archetype's fields and a `set` target, which are shorter still.
	if (!completing_members && !completing_attribute && !completing_assignable && !completing_field
			&& !completing_named_argument && prefix.is_empty()) {
		return result;
	}
	result["force"] = completing_attribute || completing_named_argument;

	// A named argument is the one position whose answer comes off the signature rather than off
	// vh_complete_symbol: what may be written past the `?` is a parameter of the call the cursor is
	// inside, which no scope at the cursor knows anything about. The hint above has already
	// resolved it, so this costs nothing and is answerable on exactly the keystrokes the hint is.
	//
	// A parameter already passed by name is offered again. Godot's own completion does no better,
	// and repeating one is a compile error the author reads at the line they are writing.
	if (completing_named_argument) {
		if (signature_is_current && !signature_cache.is_empty()) {
			const TypedArray<Dictionary> params = signature_cache["params"];
			for (int64_t i = 0; i < params.size(); i++) {
				const Dictionary param = params[i];
				if ((bool)param["is_named"] && matches_typed_prefix(param["name"], prefix)) {
					options.push_back(named_argument_option_for(param));
				}
			}
		}
		result["options"] = options;
		return result;
	}

	if (host_can_answer && (!completing_members || receiver_end >= 0)) {
		// Two of the modes are asked about a receiver's last byte -- the expression before a `.`,
		// and the class named before an archetype's `{` -- and the rest about where the name would
		// be written, which is where the prefix started. All of them sit before the substitution,
		// so none moves when the placeholder is a different length than what was typed.
		int64_t position = prefix_start;
		int32_t mode = VH_COMPLETE_SCOPE;
		if (completing_members) {
			position = receiver_end;
			mode = VH_COMPLETE_MEMBERS;
		} else if (completing_field) {
			position = archetype_end;
			mode = VH_COMPLETE_ARCHETYPE_FIELDS;
		} else if (completing_attribute) {
			mode = VH_COMPLETE_ATTRIBUTES;
		} else if (completing_specifier) {
			mode = VH_COMPLETE_SPECIFIERS;
		} else if (completing_assignable) {
			mode = VH_COMPLETE_ASSIGNABLE;
		} else if (completing_supertype) {
			mode = VH_COMPLETE_SUPERTYPES;
		} else if (completing_type) {
			mode = VH_COMPLETE_TYPES;
		}

		int64_t line = 0;
		int64_t column = 0;
		position_of(position, line, column);

		bool have_options = completion_cache_source == source && completion_cache_line == (int32_t)line
				&& completion_cache_column == (int32_t)column && completion_cache_mode == mode
				&& completion_cache_epoch == language.analysis_epoch_value();
		if (!have_options) {
			const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
			bool not_ready = false;
			const TypedArray<Dictionary> answer =
					runtime->complete_symbol(globalized, source, (int32_t)line, (int32_t)column, mode, &not_ready);
			if (not_ready) {
				// Queue the completion buffer and answer now with whatever is free. This is the
				// whole of the change ABI v7 bought: the analysis still costs ~1.3 s, but it is the
				// host's thread that spends it rather than the keystroke.
				language.request_check(p_path, source, VerseProjectState::CheckKind::COMPLETION);
			} else {
				completion_cache_options = answer;
				completion_cache_source = source;
				completion_cache_line = (int32_t)line;
				completion_cache_column = (int32_t)column;
				completion_cache_mode = mode;
				completion_cache_epoch = language.analysis_epoch_value();
				have_options = true;
			}
		}

		if (have_options) {
			for (int64_t i = 0; i < completion_cache_options.size(); i++) {
				const Dictionary item = completion_cache_options[i];
				const String name = item["name"];
				if (!matches_typed_prefix(name, prefix)) {
					continue;
				}
				host_offered_names.insert(name);
				if (!declaring_in_class.is_empty() && completes_as_override(item, declaring_in_class)) {
					options.push_back(override_option_for(item));
				} else {
					options.push_back(completion_option_for(item));
				}
			}
		} else if (completing_members) {
			// The partial answer after a `.`, which without it is an empty popup that opens only
			// once the analysis lands -- late enough that the author has typed past it. What the
			// receiver's class and its bases declare, off the same snapshot and described by the
			// same host code the refined answer will use, so the rows offered here are the rows
			// that replace them.
			//
			// Nearest first, and a name a nearer class already answered is not offered twice: a
			// redeclared member belongs to the class that redeclared it, which is what
			// vh_complete_symbol would say too.
			const PackedStringArray chain = receiver_classes_from_text(source, p_path, receiver_end);
			HashSet<String> offered;
			for (int64_t c = 0; c < chain.size(); c++) {
				const TypedArray<Dictionary> members = runtime->class_members(chain[c]);
				for (int64_t i = 0; i < members.size(); i++) {
					Dictionary item = Dictionary(members[i]).duplicate();
					const String name = item["name"];
					if (!matches_typed_prefix(name, prefix) || offered.has(name)) {
						continue;
					}
					// class_members answers for one class, so every item it hands back calls itself
					// distance zero; here the step along the chain is the distance, and ranking is
					// the whole reason the chain is walked nearest-first.
					item["owner_distance"] = c;
					offered.insert(name);
					options.push_back(completion_option_for(item));
				}
			}
		} else if (completing_a_bare_name) {
			// The partial answer for a bare identifier: what the enclosing class declares, which
			// the analysis snapshot already holds and so costs nothing. LOCATION_LOCAL puts it
			// above the class names and keywords appended below -- Godot ranks by `location` alone
			// when nothing has been typed, and uses it as the fourth tie-break once something has.
			//
			// Members only, not the scope walk: everything else a scope admits lives in the AST,
			// which is exactly what no analysis of this buffer has built yet.
			bool members_not_ready = false;
			const TypedArray<Dictionary> members = runtime->class_members(language.qualified_class_name(p_path), &members_not_ready);
			if (members_not_ready) {
				language.request_check(p_path, source, VerseProjectState::CheckKind::COMPLETION);
			}
			for (int64_t i = 0; i < members.size(); i++) {
				const Dictionary item = members[i];
				if (!matches_typed_prefix(item["name"], prefix)) {
					continue;
				}
				host_offered_names.insert(item["name"]);
				if (!declaring_in_class.is_empty() && completes_as_override(item, declaring_in_class)) {
					options.push_back(override_option_for(item));
				} else {
					options.push_back(completion_option_for(item));
				}
			}

		}

		// The other half of what a member declaration is reaching for: what the class inherits and
		// could override. Off the snapshot, described by the same host code the refined answer
		// uses, so an item here formats identically to the one the scope walk offers and the full
		// list replaces the partial one without anything moving (B13).
		//
		// After the host's answer as well as instead of it, because the scope walk has a class to
		// walk only when the buffer parses. One syntax error anywhere leaves it none, and the
		// author's buffer holds one for most of the time they are typing. A name the walk did
		// offer is skipped, so a buffer that parses gets exactly the list it always did.
		//
		// Nothing but overrides: an inherited name that is not one is an ordinary call, and the
		// thousands of them belong to the scope walk.
		if (!declaring_in_class.is_empty()) {
			// No analysis has landed at all on the first keystroke of a session, and an empty list
			// then would be the whole popup (B13). Queued as a completion so that landing re-asks.
			bool candidates_not_ready = false;
			const TypedArray<Dictionary> candidates = override_candidates(language.qualified_class_name(p_path), &candidates_not_ready);
			if (candidates_not_ready) {
				language.request_check(p_path, source, VerseProjectState::CheckKind::COMPLETION);
			}
			for (int64_t i = 0; i < candidates.size(); i++) {
				const Dictionary item = candidates[i];
				const String name = item["name"];
				if (!matches_typed_prefix(name, prefix) || already_declared.has(name) || host_offered_names.has(name)) {
					continue;
				}
				if (completes_as_override(item, declaring_in_class)) {
					host_offered_names.insert(name);
					options.push_back(override_option_for(item));
				}
			}
		}
	}

	// A dot, an `@` and a `<` have all answered everything they are going to; the sets below are
	// names that could be written on their own, which is none of the three.
	if (answers_alone) {
		result["options"] = options;
		return result;
	}

	const PackedStringArray &mirrored = VerseScriptLanguage::mirrored_class_names();
	for (int64_t i = 0; i < mirrored.size(); i++) {
		if (matches_typed_class_prefix(mirrored[i], prefix) && !host_offered_names.has(mirrored[i])) {
			options.push_back(completion_option(mirrored[i], ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, ScriptLanguageExtension::LOCATION_OTHER));
		}
	}

	// The bare name, which is what an author types: `left/widget` is how the bridge and the host
	// spell the class, and no Verse source ever does. Two modules' `widget`s are one option.
	const PackedStringArray &class_names = language.script_class_names();
	for (int64_t i = 0; i < class_names.size(); i++) {
		const String leaf = class_names[i].get_file();
		if (matches_typed_class_prefix(leaf, prefix) && !host_offered_names.has(leaf)) {
			host_offered_names.insert(leaf);
			options.push_back(completion_option(leaf, ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
		}
	}

	// Not in a class header: a keyword is never a superclass, and `class(` is the one appended set
	// whose whole point is that only two kinds of name belong there.
	for (size_t i = 0; appends_keywords && i < std::size(verse_keywords::reserved_words); i++) {
		const String word = verse_keywords::reserved_words[i];
		if (matches_typed_prefix(word, prefix) && !host_offered_names.has(word)) {
			options.push_back(completion_option(word, ScriptLanguageExtension::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguageExtension::LOCATION_OTHER));
		}
	}

	result["options"] = options;
	return result;
}

// Every completion the editor could ask for at the positions it is handed.
//
// Two things the editor does have to be reproduced or the rows describe a popup nobody can raise.
// The marker is spliced at the caret, which is what CodeEdit's get_text_with_cursor_char does. And
// each position is asked twice: _complete_code answers immediately from whatever the last analysis
// left and queues the buffer this caret actually needs, and only the second ask sees that buffer.
// Both answers are reported, because the difference between them is a real thing an author sees --
// the popup that opens at once and refines in place.
//
// What this does *not* cover is whether the popup opens at all. CodeEdit decides that from its own
// completion-prefix table (scene/gui/code_edit.cpp), which no answer from here can reach.
TypedArray<Dictionary> VerseCompletion::probe(const String &p_path, const PackedInt32Array &p_positions) {
	TypedArray<Dictionary> rows;

	language.ensure_project_built();
	language.flush_pending_check();

	const String file = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return rows;
	}
	const String source = verse_newline_normalized(file);
	const CharString source_utf8 = source.utf8();
	const std::string all(source_utf8.get_data(), (size_t)source_utf8.length());

	std::vector<size_t> line_starts;
	line_starts.push_back(0);
	for (size_t i = 0; i < all.size(); i++) {
		if (all[i] == '\n') {
			line_starts.push_back(i + 1);
		}
	}

	auto describe = [](const Dictionary &p_answer, Dictionary &r_row, const String &p_prefix) {
		const Array options = p_answer.get("options", Array());
		Array names;
		for (int64_t i = 0; i < options.size(); i++) {
			const Dictionary option = options[i];
			Dictionary row;
			row["display"] = option.get("display", String());
			row["insert_text"] = option.get("insert_text", String());
			row["kind"] = option.get("kind", (int64_t)-1);
			row["location"] = option.get("location", (int64_t)-1);
			names.push_back(row);
		}
		r_row[p_prefix + String("count")] = (int64_t)options.size();
		r_row[p_prefix + String("options")] = names;
		r_row[p_prefix + String("force")] = p_answer.get("force", false);
		r_row[p_prefix + String("call_hint")] = p_answer.get("call_hint", String());
	};

	for (int64_t i = 0; i + 1 < p_positions.size(); i += 2) {
		const int64_t line = p_positions[i];
		const int64_t column = p_positions[i + 1];
		if (line < 0 || (size_t)line >= line_starts.size() || column < 0) {
			continue;
		}
		const size_t at = line_starts[(size_t)line] + (size_t)column;
		if (at > all.size()) {
			continue;
		}

		const std::string buffer = all.substr(0, at) + "\xEF\xBF\xBF" + all.substr(at);
		const String code = String::utf8(buffer.data(), (int64_t)buffer.length());

		Dictionary row;
		row["line"] = line;
		row["column"] = column;
		// The character the caret sits behind, which is what decides both the position's own
		// question here and whether CodeEdit would have raised the popup at all.
		row["trigger"] = at > 0 ? String::utf8(all.data() + at - 1, 1) : String();

		const Dictionary first = language._complete_code(code, p_path, nullptr);
		describe(first, row, "first_");

		// Only when the first ask actually queued something. A caret the host can already describe
		// answers once and the two halves of the row are the same list, which is itself worth
		// reporting: it says the author saw the right names without waiting.
		//
		// The completion slot alone: _complete_code queues nothing else, and the ordinary one may
		// still hold the buffer the build queued if some earlier caret's flush took this one.
		const bool queued = language.completion_check_pending();
		if (queued) {
			language.flush_pending_check();
			const Dictionary second = language._complete_code(code, p_path, nullptr);
			describe(second, row, "");
		} else {
			describe(first, row, "");
		}
		row["refined"] = queued;

		rows.push_back(row);
	}

	return rows;
}

// Asks the editor for completion again now that the host describes the buffer the last answer
// declined on.
//
// `request_code_completion(true)` is the same call Ctrl+Space makes. Forced, so it skips both the
// prefix test and CodeTextEditor's 0.3 s debounce and emits `code_completion_requested` straight
// away, which lands back in _complete_code -- where the host now answers and the cache fills. An
// open popup is refreshed in place rather than closed and reopened: CodeEdit only declines a
// re-request while one is open when every option showing is a path or a signal, which no answer
// here ever is, and the selected index survives unless the head of the list actually changed.
void VerseCompletion::refresh_if_current() const {
#ifdef TOOLS_ENABLED
	if (language.completion_refresh_request().path.is_empty()) {
		return;
	}

	EditorInterface *editor_interface = verse_editor_interface();
	ScriptEditor *script_editor = editor_interface != nullptr ? editor_interface->get_script_editor() : nullptr;
	if (script_editor == nullptr) {
		return;
	}

	// Only the file the analysis was for. The author may have switched tabs while it ran, and
	// asking some other script to complete would open a popup nobody asked for.
	const Ref<Script> script = script_editor->get_current_script();
	if (script.is_null() || script->get_path() != language.completion_refresh_request().path) {
		return;
	}

	ScriptEditorBase *current = script_editor->get_current_editor();
	CodeEdit *code_edit = current != nullptr ? Object::cast_to<CodeEdit>(current->get_base_editor()) : nullptr;
	if (code_edit == nullptr) {
		return;
	}

	// The caret has to still be inside the identifier the question was about. Anywhere else and
	// the answer that just landed is not the answer to what is being typed now.
	if (completion_placeholder_buffer(code_edit) != language.completion_refresh_request().source) {
		return;
	}

	code_edit->request_code_completion(true);
#endif
}
