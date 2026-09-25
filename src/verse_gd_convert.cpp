#include "verse_gd_convert.h"

#include "verse_bindings.h"
#include "verse_class_decl.h"
#include "verse_gd_api.gen.h"
#include "verse_gd_syntax.h"
#include "verse_keywords.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

// The whole translation is in docs/gdscript-conversion.md, rule by rule. The shape of this file:
//
//   - the mirror, asked through verse_gd_api.gen.h: how a Godot name is spelled in Verse and in
//     which shape (a value, a failable value, a test);
//   - `ty`, the converter's idea of a value's type -- enough to know `400` must be `400.0`, that an
//     `?animated_sprite2d` has to be unwrapped before `.Play()`, and which reader a variant needs;
//   - a pre-pass over each class (members, names, which methods await, which lambdas exist) and a
//     fixpoint over their effects;
//   - the emitter proper, run twice over every function body before the pass that is kept, so a
//     return type or a parameter type inferred late is known to every caller.
//
// Everything failable is *hoisted*. `$Sprite.play()` is not an expression Verse can write -- the
// lookup can fail and the cast can fail -- so the statement becomes the body of an `if` whose
// clauses bind them, which is exactly how the yardstick's hand port reads:
//
//     if (SpriteFound := GetNode["Sprite"], SpriteNode := animated_sprite2d[SpriteFound]):
//         SpriteNode.Play()
//
// Where GDScript would have raised on the missing node, the Verse does nothing. That is the one
// systematic difference, and docs/gdscript-conversion.md says so.

using namespace verse_gd;
namespace api = verse_gd_api;

int VerseGdConvertResult::todo_count() const {
	int count = 0;
	for (const VerseGdNote &note : notes) {
		count += note.todo ? 1 : 0;
	}
	return count;
}

namespace {

// --- The mirror ---------------------------------------------------------------------------------

bool member_less(const api::member &p_row, const std::pair<const char *, const char *> &p_key) {
	const int c = std::strcmp(p_row.godot_class, p_key.first);
	return c != 0 ? c < 0 : std::strcmp(p_row.godot_name, p_key.second) < 0;
}

struct row_span {
	const api::member *begin = nullptr;
	const api::member *end = nullptr;
};

row_span rows_of(const std::string &p_class, const std::string &p_name) {
	const std::pair<const char *, const char *> key(p_class.c_str(), p_name.c_str());
	const api::member *first = std::lower_bound(std::begin(api::members), std::end(api::members), key, member_less);
	const api::member *last = first;
	while (last != std::end(api::members) && p_class == last->godot_class && p_name == last->godot_name) {
		last++;
	}
	return { first, last };
}

const api::class_row *class_row_of(const std::string &p_godot) {
	const api::class_row *row = std::lower_bound(std::begin(api::classes), std::end(api::classes), p_godot,
			[](const api::class_row &r, const std::string &k) { return std::strcmp(r.godot_name, k.c_str()) < 0; });
	if (row != std::end(api::classes) && p_godot == row->godot_name) {
		return row;
	}
	return nullptr;
}

bool is_math_type(const std::string &p_godot) {
	const api::class_row *row = class_row_of(p_godot);
	return row && row->fields[0] != '\0';
}

bool is_godot_class(const std::string &p_godot) {
	const api::class_row *row = class_row_of(p_godot);
	return row && row->fields[0] == '\0';
}

std::string parent_of(const std::string &p_godot) {
	const api::class_row *row = class_row_of(p_godot);
	return row ? row->parent : "";
}

std::string verse_class_of(const std::string &p_godot) {
	const api::class_row *row = class_row_of(p_godot);
	return row ? row->verse_name : verse_binding_class_name(p_godot);
}

bool godot_inherits(const std::string &p_class, const std::string &p_base) {
	for (std::string c = p_class; !c.empty(); c = parent_of(c)) {
		if (c == p_base) {
			return true;
		}
	}
	return false;
}

// Verse's name for a mirrored class back to Godot's, for the rows whose result is a class.
const std::string &godot_of_verse(const std::string &p_verse) {
	static std::map<std::string, std::string> map;
	if (map.empty()) {
		for (const api::class_row &row : api::classes) {
			// The row whose own name maps to this Verse name, not an unemitted descendant that
			// crosses as it.
			if (verse_binding_class_name(row.godot_name) == row.verse_name || std::string(row.fields).size()) {
				map.emplace(row.verse_name, row.godot_name);
			}
		}
	}
	static const std::string none;
	auto it = map.find(p_verse);
	return it == map.end() ? none : it->second;
}

bool accepts(api::kind p_kind, std::initializer_list<api::kind> p_kinds) {
	for (api::kind k : p_kinds) {
		if (k == p_kind) {
			return true;
		}
	}
	return false;
}

// A member of a Godot class or of any of its ancestors.
const api::member *find_member(const std::string &p_class, const std::string &p_name, std::initializer_list<api::kind> p_kinds) {
	for (std::string c = p_class; !c.empty(); c = parent_of(c)) {
		const row_span span = rows_of(c, p_name);
		for (const api::member *r = span.begin; r != span.end; r++) {
			if (accepts(r->what, p_kinds)) {
				return r;
			}
		}
	}
	return nullptr;
}

const api::member *find_global(const std::string &p_name, std::initializer_list<api::kind> p_kinds) {
	const row_span span = rows_of("@GlobalScope", p_name);
	for (const api::member *r = span.begin; r != span.end; r++) {
		if (accepts(r->what, p_kinds)) {
			return r;
		}
	}
	return nullptr;
}

// Every name a class carries through its ancestry, for the rule that a member of a script class
// may not reuse one (CLAUDE.md: "A class member may not shadow an inherited mirrored one").
const std::set<std::string> &inherited_verse_names(const std::string &p_class) {
	static std::map<std::string, std::set<std::string>> cache;
	auto found = cache.find(p_class);
	if (found != cache.end()) {
		return found->second;
	}
	std::set<std::string> names = { "Handle" };
	for (std::string c = p_class; !c.empty(); c = parent_of(c)) {
		const api::member *first = std::lower_bound(std::begin(api::members), std::end(api::members),
				std::pair<const char *, const char *>(c.c_str(), ""), member_less);
		for (const api::member *r = first; r != std::end(api::members) && c == r->godot_class; r++) {
			if (accepts(r->what, { api::kind::method, api::kind::virtual_method, api::kind::property, api::kind::signal })) {
				names.insert(r->verse);
			}
		}
	}
	return cache.emplace(p_class, names).first->second;
}

const std::set<std::string> &enum_type_names() {
	static std::set<std::string> names;
	if (names.empty()) {
		for (const api::member &r : api::members) {
			if (r.what == api::kind::enum_type) {
				names.insert(r.verse);
			}
		}
	}
	return names;
}

const std::set<std::string> &module_scope_names() {
	static std::set<std::string> names;
	if (names.empty()) {
		for (const char *n : api::module_scope_names) {
			names.insert(n);
		}
		for (const char *n : verse_keywords::reserved_words) {
			names.insert(n);
		}
		// Names a script reaches without the mirror: /Verse.org/Verse's own, which the table's list
		// leaves to VERSE_STDLIB_NAMES's caller. `Self` is a keyword already.
		for (const char *n : { "Print", "Err", "Sleep", "PiFloat", "Inf", "NaN", "GetRandomFloat", "GetRandomInt",
					 "Quotient", "Mod", "Floor", "Ceil", "Round", "Int", "Float" }) {
			names.insert(n);
		}
	}
	return names;
}

// --- Params encoded in the table ----------------------------------------------------------------

struct api_param {
	std::string name;
	std::string type;
	bool optional = false; // Verse's `?Name`, passed by name
	std::string fill; // Verse requires it; Godot defaulted it to this
};

std::vector<api_param> decode_params(const char *p_encoded) {
	std::vector<api_param> out;
	std::string text = p_encoded;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find(';', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string part = text.substr(start, end - start);
		start = end + 1;
		api_param p;
		if (!part.empty() && part[0] == '?' && part.find(':') != std::string::npos && part.find(':') > 1) {
			p.optional = true;
			part = part.substr(1);
		}
		const size_t colon = part.find(':');
		p.name = part.substr(0, colon);
		std::string type = colon == std::string::npos ? "" : part.substr(colon + 1);
		const size_t eq = type.find('=');
		if (eq != std::string::npos) {
			p.fill = type.substr(eq + 1);
			type = type.substr(0, eq);
		}
		p.type = type;
		out.push_back(p);
	}
	return out;
}

// --- Types ----------------------------------------------------------------------------------------

enum class tk {
	unknown,
	void_,
	int_,
	float_,
	logic,
	string,
	math, // name: the Godot type, "Vector2"
	object, // name: the Godot class; script: a script class's Verse name
	array, // elem
	typed_array, // Godot's typed_array(t): iterated through .ToArray()
	map, // key, elem
	enum_, // name: the Verse enum
	variant,
	signal, // payload
	event, // payload
	callable,
	signal_ref,
	garray,
	dict,
	option, // elem
	rid,
	tuple, // payload
	type_value, // a type used as a value -- `Vector2`, `Node`, `State`, `Input`
	function, // one of this class's own functions, by name: payload is its Verse name
};

struct ty {
	tk k = tk::unknown;
	std::string name;
	std::string script;
	std::string payload;
	std::string tv; // type_value: "class", "math", "enum", "script", "singleton", "own_enum", "global_enum"
	std::shared_ptr<ty> elem;
	std::shared_ptr<ty> key;

	static ty of(tk p_k) {
		ty t;
		t.k = p_k;
		return t;
	}
	static ty object_of(const std::string &p_godot, const std::string &p_script = "") {
		ty t;
		t.k = tk::object;
		t.name = p_godot;
		t.script = p_script;
		return t;
	}
	static ty math_of(const std::string &p_godot) {
		ty t;
		t.k = tk::math;
		t.name = p_godot;
		return t;
	}
	static ty array_of(const ty &p_elem, tk p_kind = tk::array) {
		ty t;
		t.k = p_kind;
		t.elem = std::make_shared<ty>(p_elem);
		return t;
	}
	static ty option_of(const ty &p_elem) {
		if (p_elem.k == tk::option) {
			return p_elem;
		}
		ty t;
		t.k = tk::option;
		t.elem = std::make_shared<ty>(p_elem);
		return t;
	}
	static ty enum_of(const std::string &p_verse) {
		ty t;
		t.k = tk::enum_;
		t.name = p_verse;
		return t;
	}
	bool numeric() const { return k == tk::int_ || k == tk::float_; }
	bool known() const { return k != tk::unknown; }
	const ty &inner() const { return *elem; }
};

std::string verse_text(const ty &p_t);

std::string verse_text(const ty &p_t) {
	switch (p_t.k) {
		case tk::unknown:
		case tk::variant:
			return "variant";
		case tk::void_:
			return "void";
		case tk::int_:
			return "int";
		case tk::float_:
			return "float";
		case tk::logic:
			return "logic";
		case tk::string:
			return "string";
		case tk::math:
			return verse_class_of(p_t.name);
		case tk::object:
			return p_t.script.empty() ? verse_class_of(p_t.name.empty() ? "Object" : p_t.name) : p_t.script;
		case tk::array:
			return "[]" + verse_text(*p_t.elem);
		case tk::typed_array:
			return "typed_array(" + verse_text(*p_t.elem) + ")";
		case tk::map:
			return "[" + verse_text(*p_t.key) + "]" + verse_text(*p_t.elem);
		case tk::enum_:
			return p_t.name;
		case tk::signal:
			return "signal(" + p_t.payload + ")";
		case tk::event:
			return "event(" + p_t.payload + ")";
		case tk::callable:
			return "callable";
		case tk::signal_ref:
			return "signal_ref";
		case tk::garray:
			return "godot_array";
		case tk::dict:
			return "dictionary";
		case tk::option:
			return "?" + verse_text(*p_t.elem);
		case tk::rid:
			return "rid";
		case tk::tuple:
			return "tuple(" + p_t.payload + ")";
		case tk::type_value:
		case tk::function:
			return "variant";
	}
	return "variant";
}

bool same_type(const ty &a, const ty &b) {
	return verse_text(a) == verse_text(b);
}

// A type as the mirror spells it, back into a `ty`.
ty ty_from_verse(const std::string &p_text) {
	const std::string &t = p_text;
	if (t.empty() || t == "void") {
		return ty::of(tk::void_);
	}
	if (t == "int") {
		return ty::of(tk::int_);
	}
	if (t == "float") {
		return ty::of(tk::float_);
	}
	if (t == "logic") {
		return ty::of(tk::logic);
	}
	if (t == "string") {
		return ty::of(tk::string);
	}
	if (t == "variant") {
		return ty::of(tk::variant);
	}
	if (t == "callable") {
		return ty::of(tk::callable);
	}
	if (t == "signal_ref") {
		return ty::of(tk::signal_ref);
	}
	if (t == "godot_array") {
		return ty::of(tk::garray);
	}
	if (t == "dictionary" || t.rfind("typed_dictionary(", 0) == 0) {
		return ty::of(tk::dict);
	}
	if (t == "rid") {
		return ty::of(tk::rid);
	}
	if (t[0] == '?') {
		return ty::option_of(ty_from_verse(t.substr(1)));
	}
	if (t.rfind("[]", 0) == 0) {
		return ty::array_of(ty_from_verse(t.substr(2)));
	}
	if (t.rfind("typed_array(", 0) == 0) {
		return ty::array_of(ty_from_verse(t.substr(12, t.size() - 13)), tk::typed_array);
	}
	if (t.rfind("signal(", 0) == 0) {
		ty s = ty::of(tk::signal);
		s.payload = t.substr(7, t.size() - 8);
		return s;
	}
	if (t.rfind("tuple(", 0) == 0) {
		ty s = ty::of(tk::tuple);
		s.payload = t.substr(6, t.size() - 7);
		return s;
	}
	if (enum_type_names().count(t)) {
		return ty::enum_of(t);
	}
	const std::string &godot = godot_of_verse(t);
	if (!godot.empty()) {
		return is_math_type(godot) ? ty::math_of(godot) : ty::object_of(godot);
	}
	return ty::of(tk::unknown);
}

// Godot's variant builders and readers, by type. `Variant<GodotType>` and `.As<GodotType>[]`.
std::string variant_suffix(const ty &p_t) {
	switch (p_t.k) {
		case tk::int_:
			return "Int";
		case tk::float_:
			return "Float";
		case tk::logic:
			return "Bool";
		case tk::string:
			return "String";
		case tk::math:
			return p_t.name;
		case tk::object:
			return "Object";
		case tk::callable:
			return "Callable";
		case tk::signal_ref:
			return "Signal";
		case tk::garray:
			return "Array";
		case tk::dict:
			return "Dictionary";
		case tk::rid:
			return "Rid";
		default:
			return "";
	}
}

// The typed accessor a Godot Dictionary offers for a value of this type: `GetInt[K]`, `SetFloat(K, V)`.
// `logic` is the one lane whose accessor is not named after Godot's type.
std::string dictionary_lane(const ty &p_t) {
	if (p_t.k == tk::logic) {
		return "Logic";
	}
	if (p_t.k == tk::variant || p_t.k == tk::unknown || p_t.k == tk::option) {
		return "Variant";
	}
	const std::string suffix = variant_suffix(p_t);
	return suffix.empty() ? "Variant" : suffix;
}

bool dictionary_key(const ty &p_t) {
	return p_t.k == tk::string || p_t.k == tk::int_ || (p_t.k == tk::math && p_t.name == "Vector2i");
}

std::string default_value(const ty &p_t) {
	switch (p_t.k) {
		case tk::int_:
			return "0";
		case tk::float_:
			return "0.0";
		case tk::logic:
			return "false";
		case tk::string:
			return "\"\"";
		case tk::math:
			return verse_class_of(p_t.name) + "{}";
		case tk::array:
			return "array{}";
		case tk::map:
			return "map{}";
		case tk::option:
			return "false";
		case tk::variant:
		case tk::unknown:
			return "variant{}";
		case tk::garray:
			return "godot_array{}";
		case tk::dict:
			return "dictionary{}";
		case tk::rid:
			return "rid{}";
		default:
			return "";
	}
}

// --- Names --------------------------------------------------------------------------------------

std::string strip_leading_underscores(const std::string &p_name) {
	size_t i = 0;
	while (i < p_name.size() && p_name[i] == '_') {
		i++;
	}
	return i == p_name.size() ? p_name : p_name.substr(i);
}

bool all_caps(const std::string &p_name) {
	bool any_letter = false;
	for (char c : p_name) {
		if (std::islower((unsigned char)c)) {
			return false;
		}
		any_letter = any_letter || std::isalpha((unsigned char)c);
	}
	return any_letter;
}

std::string pascal(const std::string &p_name) {
	std::string base = strip_leading_underscores(p_name);
	if (all_caps(base)) {
		// `MAX_SPEED` is a constant, and a constant is spelled the way the mirror spells Godot's:
		// `MaxSpeed`, which is what `NOTIFICATION_READY` -> `NotificationReady` already is.
		std::string lowered;
		for (char c : base) {
			lowered += char(std::tolower((unsigned char)c));
		}
		base = lowered;
	}
	if (base.find('_') == std::string::npos && !base.empty() && std::isupper((unsigned char)base[0])) {
		return base;
	}
	return verse_binding_member_name(base);
}

std::string sanitize_identifier(const std::string &p_text) {
	std::string out;
	bool upper = true;
	for (char c : p_text) {
		if (std::isalnum((unsigned char)c)) {
			out += upper ? char(std::toupper((unsigned char)c)) : c;
			upper = false;
		} else {
			upper = true;
		}
	}
	if (out.empty() || std::isdigit((unsigned char)out[0])) {
		out = "Node" + out;
	}
	return out;
}

std::string escape_verse(const std::string &p_text) {
	std::string out;
	for (char c : p_text) {
		switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '{':
				out += "\\{";
				break;
			case '}':
				out += "\\}";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\t':
				out += "\\t";
				break;
			case '\r':
				out += "\\r";
				break;
			default:
				out += c;
		}
	}
	return out;
}

std::string trim(const std::string &p_text) {
	size_t a = 0;
	size_t b = p_text.size();
	while (a < b && std::isspace((unsigned char)p_text[a])) {
		a++;
	}
	while (b > a && std::isspace((unsigned char)p_text[b - 1])) {
		b--;
	}
	return p_text.substr(a, b - a);
}

std::string format_float(const std::string &p_source) {
	const double value = std::strtod(p_source.c_str(), nullptr);
	std::string text;
	if (p_source.find_first_of("eE") == std::string::npos) {
		text = p_source;
		if (text[0] == '.') {
			text = "0" + text;
		}
		if (text.back() == '.') {
			text += "0";
		}
		if (text.find('.') == std::string::npos) {
			text += ".0";
		}
		return text;
	}
	// An exponent is written out, shortest-first, because a digit string is the float literal
	// Verse is certain to accept.
	char buffer[64];
	for (int precision = 1; precision <= 17; precision++) {
		std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
		if (std::strtod(buffer, nullptr) == value) {
			break;
		}
	}
	text = buffer;
	while (text.size() > 2 && text.back() == '0' && text[text.size() - 2] != '.') {
		text.pop_back();
	}
	return text;
}

std::string format_int(const std::string &p_source) {
	if (p_source.size() > 2 && p_source[0] == '0' && (p_source[1] == 'x' || p_source[1] == 'X' || p_source[1] == 'b' || p_source[1] == 'B')) {
		const int base = (p_source[1] == 'x' || p_source[1] == 'X') ? 16 : 2;
		return std::to_string(std::strtoll(p_source.c_str() + 2, nullptr, base));
	}
	return p_source;
}

// --- Emitted values -------------------------------------------------------------------------------

enum class form {
	value, // an expression with a value
	fails, // `F[...]`: a value, or failure
	test, // a test with no value: a comparison, a predicate, `X?`
};

struct ev {
	std::string code;
	ty t;
	form f = form::value;
	int prec = 100;
	bool is_null = false;
	bool int_literal = false;
	bool str_literal = false;
	std::string str_inner; // the escaped text between the quotes, for merging
	bool todo = false;
	bool is_self = false;
	bool is_super = false;
	// For an attribute that names something callable rather than a value: the receiver's code and
	// the member, so the call that follows can resolve it.
	std::string member_of;
	std::string member_name;
	ty member_owner;
	bool has_owner = false;
	// What to call it if it has to be bound: `GetTree[]` binds as `Tree`, the way the yardstick
	// names it, rather than as `Value`.
	std::string hint;
	// A parameter the file gave no type, by index: using it where a type is known settles it.
	int param_index = -1;

	ev() = default;
	ev(std::string p_code, ty p_t = ty(), form p_f = form::value, int p_prec = 100) :
			code(std::move(p_code)), t(std::move(p_t)), f(p_f), prec(p_prec) {}

	static ev todo_marker() {
		ev e;
		e.todo = true;
		e.code = "false";
		e.t = ty::of(tk::unknown);
		return e;
	}
};

std::string paren(const ev &p_e, int p_min_prec) {
	return p_e.prec < p_min_prec ? "(" + p_e.code + ")" : p_e.code;
}

// --- Class model --------------------------------------------------------------------------------

enum class effect {
	transacts,
	none,
	suspends,
};

struct fn_info {
	std::string gd_name;
	std::string verse_name;
	func_ptr decl;
	bool is_virtual = false;
	const api::member *vrow = nullptr;
	bool is_static = false;
	bool is_lambda = false;
	std::vector<std::string> captures; // a lambda's enclosing locals, by GDScript name
	std::vector<std::string> param_names; // Verse
	std::vector<ty> param_types;
	std::vector<bool> param_optional;
	std::vector<bool> param_typed; // written with a type, or inferred and settled
	ty ret = ty::of(tk::unknown);
	bool ret_declared = false;
	bool ret_inferred = false;
	bool awaits = false;
	std::set<std::string> calls; // own functions called, by GDScript name
	std::set<std::string> unawaited_calls;
	effect eff = effect::transacts;
	bool spawns = false; // found during emission: it spawns a suspending call on another object
	bool split_async = false; // a virtual that awaits: its body moves to `<Name>Async`
	bool predicate_helper = false; // a `<decides>` virtual: its body moves to `<Name>Value`
	std::string helper_name;
};

struct var_info {
	std::string gd_name;
	std::string verse_name;
	std::shared_ptr<var_decl> decl;
	ty t; // the slot's type, `?x` for an object
	bool is_const = false;
	bool onready = false;
	bool exported = false;
	bool deferred_init = false; // initialiser moved into `_Ready`
	std::string getter_fn; // Verse method names, when the var had accessors
	std::string setter_fn;
};

struct sig_info {
	std::string gd_name;
	std::string verse_name;
	std::vector<ty> params;
	std::string payload;
};

struct enum_info {
	std::string gd_name;
	std::string verse_name;
	std::vector<std::pair<std::string, std::string>> values; // GDScript -> Verse
};

struct class_ctx {
	class_ptr decl;
	std::string verse_name;
	std::string base_godot = "RefCounted";
	std::string base_verse;
	std::string base_script;
	std::map<std::string, var_info> vars;
	std::map<std::string, std::shared_ptr<fn_info>> fns;
	std::vector<std::shared_ptr<fn_info>> lambdas;
	std::map<std::string, sig_info> sigs;
	std::map<std::string, enum_info> enums;
	std::map<std::string, std::string> inner_classes; // GDScript name -> Verse class
	std::set<std::string> taken; // every member name the class carries
	class_ctx *outer = nullptr;
	bool is_inner = false;
};

struct local {
	std::string verse;
	ty t;
	bool is_var = false;
	int param_index = -1;
};

struct line_out {
	std::vector<std::string> lines;
	void add(int p_indent, const std::string &p_text) {
		lines.push_back(std::string(size_t(p_indent), '\t') + p_text);
	}
	void blank() {
		if (!lines.empty() && !lines.back().empty()) {
			lines.push_back("");
		}
	}
	void append(const line_out &p_other, int p_indent) {
		for (const std::string &l : p_other.lines) {
			lines.push_back(l.empty() ? l : std::string(size_t(p_indent), '\t') + l);
		}
	}
};

// What one statement accumulated while its expressions were emitted.
struct sctx {
	std::vector<std::string> clauses;
	std::map<std::string, std::string> hoisted; // failable code -> binding, so it is looked up once
	std::vector<std::string> todos;
};

struct fctx {
	class_ctx *cls = nullptr;
	fn_info *fn = nullptr;
	std::vector<std::map<std::string, local>> scopes;
	std::set<std::string> taken;
	std::set<std::string> mutated;
	int loop_depth = 0;
	bool in_for = false;
};

// --- The converter --------------------------------------------------------------------------------

class converter {
public:
	explicit converter(const VerseGdConvertInput &p_input) :
			in(p_input) {}

	VerseGdConvertResult run();

private:
	const VerseGdConvertInput &in;
	VerseGdConvertResult res;
	parse_result parsed;
	std::vector<std::string> src_lines;
	std::vector<bool> comment_used;
	std::set<std::pair<int, std::string>> noted;
	bool final_pass = false;
	std::string verse_stem;
	// Classes the output names that only a generated binding declares.
	std::set<std::string> bindings_used;

	void uses_script_class(const VerseGdScriptClass &p_class) {
		if (p_class.is_binding && final_pass) {
			bindings_used.insert(p_class.verse_name);
		}
	}

	// ---- notes ----
	// Above zero while an expression is evaluated only for its type, so it says nothing twice.
	int quiet = 0;

	void note(int p_line, const std::string &p_message, bool p_todo = false) {
		if (!final_pass || quiet > 0) {
			return;
		}
		if (noted.insert({ p_line, p_message }).second) {
			VerseGdNote n;
			n.line = p_line;
			n.message = p_message;
			n.todo = p_todo;
			res.notes.push_back(n);
		}
	}

	std::string source_line(int p_line) const {
		return p_line >= 1 && p_line <= int(src_lines.size()) ? src_lines[size_t(p_line - 1)] : "";
	}

	// A statement that could not be translated: the reason, then the GDScript it replaces, commented.
	void emit_todo(line_out &r_out, int p_indent, int p_first, int p_last, const std::vector<std::string> &p_reasons) {
		std::string reason;
		for (const std::string &r : p_reasons) {
			if (reason.find(r) == std::string::npos) {
				reason += (reason.empty() ? "" : "; ") + r;
			}
		}
		r_out.add(p_indent, "# TODO(convert): " + reason);
		int base = -1;
		for (int l = p_first; l <= p_last; l++) {
			const std::string text = source_line(l);
			size_t lead = 0;
			while (lead < text.size() && (text[lead] == '\t' || text[lead] == ' ')) {
				lead++;
			}
			if (base < 0) {
				base = int(lead);
			}
			const size_t cut = std::min(lead, size_t(base));
			r_out.add(p_indent, "#     " + text.substr(cut));
		}
		note(p_first, reason, true);
		mark_comments_used(p_first, p_last);
	}

	// ---- comments ----
	void mark_comments_used(int p_first, int p_last) {
		for (size_t i = 0; i < parsed.comments.size(); i++) {
			if (parsed.comments[i].line >= p_first && parsed.comments[i].line <= p_last) {
				comment_used[i] = true;
			}
		}
	}

	void flush_comments(line_out &r_out, int p_indent, int p_before_line) {
		for (size_t i = 0; i < parsed.comments.size(); i++) {
			const comment &c = parsed.comments[i];
			if (comment_used[i] || !c.own_line || c.line >= p_before_line) {
				continue;
			}
			comment_used[i] = true;
			r_out.add(p_indent, "#" + (c.text.empty() || c.text[0] == ' ' ? c.text : " " + c.text));
		}
	}

	std::string trailing_comment(int p_first, int p_last) {
		std::string out;
		for (size_t i = 0; i < parsed.comments.size(); i++) {
			const comment &c = parsed.comments[i];
			if (comment_used[i] || c.own_line || c.line < p_first || c.line > p_last) {
				continue;
			}
			comment_used[i] = true;
			out += " #" + (c.text.empty() || c.text[0] == ' ' ? c.text : " " + c.text);
		}
		return out;
	}

	// ---- type annotations ----
	ty ty_from_gd(const std::string &p_type, class_ctx &p_cls, int p_line) {
		const std::string &t = p_type;
		if (t.empty() || t == "=") {
			return ty::of(tk::unknown);
		}
		if (t == "int") {
			return ty::of(tk::int_);
		}
		if (t == "float") {
			return ty::of(tk::float_);
		}
		if (t == "bool") {
			return ty::of(tk::logic);
		}
		if (t == "String" || t == "StringName" || t == "NodePath") {
			return ty::of(tk::string);
		}
		if (t == "void") {
			return ty::of(tk::void_);
		}
		if (t == "Variant") {
			return ty::of(tk::variant);
		}
		if (t == "Callable") {
			return ty::of(tk::callable);
		}
		if (t == "Signal") {
			return ty::of(tk::signal_ref);
		}
		if (t == "RID") {
			return ty::of(tk::rid);
		}
		if (t == "Array") {
			return ty::array_of(ty::of(tk::variant));
		}
		if (t.rfind("Array[", 0) == 0) {
			return ty::array_of(ty_from_gd(t.substr(6, t.size() - 7), p_cls, p_line));
		}
		if (t == "Dictionary") {
			return ty::of(tk::dict);
		}
		if (t.rfind("Dictionary[", 0) == 0) {
			const std::string inside = t.substr(11, t.size() - 12);
			const size_t comma = inside.find(',');
			ty m;
			m.k = tk::map;
			m.key = std::make_shared<ty>(ty_from_gd(inside.substr(0, comma), p_cls, p_line));
			m.elem = std::make_shared<ty>(ty_from_gd(inside.substr(comma + 1), p_cls, p_line));
			return m;
		}
		static const std::map<std::string, std::string> packed = {
			{ "PackedByteArray", "int" }, { "PackedInt32Array", "int" }, { "PackedInt64Array", "int" },
			{ "PackedFloat32Array", "float" }, { "PackedFloat64Array", "float" }, { "PackedStringArray", "String" },
			{ "PackedVector2Array", "Vector2" }, { "PackedVector3Array", "Vector3" }, { "PackedColorArray", "Color" },
			{ "PackedVector4Array", "Vector4" },
		};
		auto p = packed.find(t);
		if (p != packed.end()) {
			return ty::array_of(ty_from_gd(p->second, p_cls, p_line));
		}
		if (is_math_type(t)) {
			return ty::math_of(t);
		}
		for (class_ctx *c = &p_cls; c; c = c->outer) {
			auto e = c->enums.find(t);
			if (e != c->enums.end()) {
				return ty::enum_of(e->second.verse_name);
			}
			auto inner = c->inner_classes.find(t);
			if (inner != c->inner_classes.end()) {
				return ty::object_of("RefCounted", inner->second);
			}
		}
		const size_t dot = t.find('.');
		if (dot != std::string::npos) {
			const api::member *row = find_member(t.substr(0, dot), t.substr(dot + 1), { api::kind::enum_type });
			if (row) {
				return ty::enum_of(row->verse);
			}
		}
		if (const api::member *row = find_global(t, { api::kind::enum_type })) {
			return ty::enum_of(row->verse);
		}
		if (is_godot_class(t)) {
			return ty::object_of(t);
		}
		if (t == res.gd_class_name) {
			return ty::object_of(root_ctx->base_godot, root_ctx->verse_name);
		}
		auto script = in.script_classes.find(t);
		if (script != in.script_classes.end()) {
			uses_script_class(script->second);
			return ty::object_of(script->second.godot_base, script->second.verse_name);
		}
		note(p_line, "`" + t + "` is not a type the converter knows; it became `variant`", true);
		return ty::of(tk::variant);
	}

	class_ctx *root_ctx = nullptr;
	std::vector<std::unique_ptr<class_ctx>> classes;

	// ---- the class pre-pass ----
	void prepare_class(class_ctx &r_cls);
	void analyse_function(class_ctx &r_cls, fn_info &r_fn);
	void scan_expr(class_ctx &r_cls, fn_info &r_fn, const expr_ptr &p_e, bool p_awaited, const std::set<std::string> &p_enclosing);
	void scan_stmts(class_ctx &r_cls, fn_info &r_fn, const std::vector<stmt_ptr> &p_body, const std::set<std::string> &p_enclosing);
	void solve_effects(class_ctx &r_cls);
	std::string claim_member_name(class_ctx &r_cls, const std::string &p_wanted, int p_line, const std::string &p_gd);

	// ---- emission ----
	void emit_class(class_ctx &r_cls, line_out &r_out);
	void emit_function(class_ctx &r_cls, fn_info &r_fn, line_out &r_out);
	void emit_ready_prologue(class_ctx &r_cls, fctx &r_f, line_out &r_out);
	void emit_member_var(class_ctx &r_cls, var_info &r_var, line_out &r_out);
	void type_vars(class_ctx &r_cls);
	void build_class(class_ptr p_decl, const std::string &p_verse_name, class_ctx *p_outer);
	std::string export_attributes(const var_info &p_var, ty &r_type, int p_line);
	void emit_block(fctx &r_f, const std::vector<stmt_ptr> &p_body, int p_indent, line_out &r_out);
	void emit_stmt(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out);
	void emit_stmt_inner(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out);
	void emit_guarded(line_out &r_out, int p_indent, const sctx &p_s, const std::string &p_code, const std::string &p_trailing);
	std::string last_guard;
	std::string interpolate(fctx &r_f, const ev &p_e, sctx &r_s);
	void emit_if(fctx &r_f, const stmt_ptr &p_s, size_t p_branch, int p_indent, line_out &r_out, bool p_else_if);
	void emit_for(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out);
	void emit_match(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out);
	bool emit_assign(fctx &r_f, const expr_ptr &p_target, const ev &p_value, sctx &r_s, std::string &r_code, int p_line);

	ev ex(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_expect = nullptr);
	ev ex_ident(fctx &r_f, const expr_ptr &p_e, sctx &r_s);
	ev ex_attribute(fctx &r_f, const expr_ptr &p_e, sctx &r_s);
	ev ex_call(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_expect);
	ev ex_binary(fctx &r_f, const expr_ptr &p_e, sctx &r_s);
	ev ex_node_path(fctx &r_f, const std::string &p_path, const std::string &p_receiver, sctx &r_s, int p_line);
	ev ex_await(fctx &r_f, const expr_ptr &p_e, sctx &r_s);
	ev ex_array(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_expect);
	ev ex_string_format(fctx &r_f, const ev &p_format, const expr_ptr &p_args, sctx &r_s, int p_line);
	ev call_godot(fctx &r_f, const std::string &p_receiver, const api::member *p_row, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line);
	ev call_own(fctx &r_f, const std::string &p_receiver, fn_info &p_fn, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line, bool p_awaited);
	ev call_dynamic(fctx &r_f, const std::string &p_receiver, const std::string &p_method, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line);
	ev call_builtin(fctx &r_f, const std::string &p_name, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line, const ty *p_expect, bool &r_handled);
	ev member_of_value(fctx &r_f, const ev &p_receiver, const std::string &p_name, sctx &r_s, int p_line);
	ev load_resource(fctx &r_f, const expr_ptr &p_path, sctx &r_s, int p_line);
	ev function_reference(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_payload);

	std::string value_of(fctx &r_f, const ev &p_e, sctx &r_s, const std::string &p_hint);
	std::string test_of(const ev &p_e);
	std::string coerce(fctx &r_f, const ev &p_e, const ty &p_target, sctx &r_s, int p_line);
	std::string hoist(fctx &r_f, sctx &r_s, const std::string &p_code, const std::string &p_hint);
	std::string unwrap(fctx &r_f, const ev &p_e, sctx &r_s, const std::string &p_hint, ty &r_inner);
	std::string fresh(fctx &r_f, const std::string &p_base, const char *p_clash_suffix = "Value");
	std::string fold(const sctx &p_s, const std::string &p_value, const std::string &p_default);

	const local *find_local(fctx &r_f, const std::string &p_name) {
		for (auto it = r_f.scopes.rbegin(); it != r_f.scopes.rend(); ++it) {
			auto found = it->find(p_name);
			if (found != it->end()) {
				return &found->second;
			}
		}
		return nullptr;
	}

	fn_info *own_fn(class_ctx &r_cls, const std::string &p_name) {
		auto it = r_cls.fns.find(p_name);
		return it == r_cls.fns.end() ? nullptr : it->second.get();
	}

	ty self_type(const class_ctx &p_cls) const { return ty::object_of(p_cls.base_godot, p_cls.verse_name); }

	bool is_own_script(const ty &p_t, const class_ctx &p_cls) const {
		return p_t.k == tk::object && p_t.script == p_cls.verse_name;
	}

	std::string payload_of(const std::vector<ty> &p_params) {
		if (p_params.empty()) {
			return "tuple()";
		}
		if (p_params.size() == 1) {
			return verse_text(p_params[0]);
		}
		std::string out = "tuple(";
		for (size_t i = 0; i < p_params.size(); i++) {
			out += (i ? ", " : "") + verse_text(p_params[i]);
		}
		return out + ")";
	}

	std::vector<ty> payload_types(const std::string &p_payload) {
		std::vector<ty> out;
		if (p_payload == "tuple()" || p_payload.empty()) {
			return out;
		}
		if (p_payload.rfind("tuple(", 0) == 0) {
			std::string inner = p_payload.substr(6, p_payload.size() - 7);
			int depth = 0;
			std::string cur;
			for (char c : inner) {
				if (c == '(' || c == '[') {
					depth++;
				} else if (c == ')' || c == ']') {
					depth--;
				}
				if (c == ',' && depth == 0) {
					out.push_back(ty_from_verse(trim(cur)));
					cur.clear();
					continue;
				}
				cur += c;
			}
			if (!trim(cur).empty()) {
				out.push_back(ty_from_verse(trim(cur)));
			}
			return out;
		}
		out.push_back(ty_from_verse(p_payload));
		return out;
	}

	bool is_static_initializer(const expr_ptr &p_e);

	const VerseGdScriptClass *script_info(const std::string &p_verse_name) const {
		for (const auto &kv : in.script_classes) {
			if (kv.second.verse_name == p_verse_name) {
				return &kv.second;
			}
		}
		return nullptr;
	}
	ev call_script(fctx &r_f, const std::string &p_receiver, const VerseGdMethodInfo &p_method, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line, bool p_awaited,
			const std::string &p_method_owner, const std::string &p_method_name);
	void observe(const std::string &p_class, const std::string &p_method, size_t p_index, const ev &p_arg) {
		if (!final_pass) {
			return;
		}
		ty t = p_arg.t;
		if (p_arg.f == form::test) {
			t = ty::of(tk::logic);
		}
		if (p_arg.is_null || !t.known() || t.k == tk::variant || t.k == tk::type_value || t.k == tk::function || t.k == tk::void_) {
			t = ty::of(tk::variant);
		}
		res.observed_arguments[p_class][p_method][int(p_index)].insert(verse_text(t));
	}
};

// ================================================================================================
// Names

std::string converter::claim_member_name(class_ctx &r_cls, const std::string &p_wanted, int p_line, const std::string &p_gd) {
	std::string name = p_wanted;
	const std::set<std::string> &inherited = inherited_verse_names(r_cls.base_godot);
	const bool clash = inherited.count(name) || module_scope_names().count(name);
	if (clash) {
		name += "Value";
		note(p_line, "`" + p_gd + "` became `" + name + "`: `" + p_wanted
						+ "` is already a name Verse or the Godot mirror defines, and Verse refuses a member that shadows one");
	}
	std::string unique = name;
	for (int n = 2; r_cls.taken.count(unique); n++) {
		unique = name + std::to_string(n);
	}
	if (unique != name) {
		note(p_line, "`" + p_gd + "` became `" + unique + "`: another member already converts to `" + name + "`");
	}
	r_cls.taken.insert(unique);
	return unique;
}

std::string converter::fresh(fctx &r_f, const std::string &p_base, const char *p_clash_suffix) {
	std::string base = p_base.empty() ? "Value" : p_base;
	if (!std::isupper((unsigned char)base[0])) {
		base[0] = char(std::toupper((unsigned char)base[0]));
	}
	const std::set<std::string> &inherited = inherited_verse_names(r_f.cls->base_godot);
	auto clashes = [&](const std::string &n) {
		return r_f.taken.count(n) || r_f.cls->taken.count(n) || inherited.count(n) || module_scope_names().count(n);
	};
	std::string name = base;
	if (clashes(name) && (r_f.cls->taken.count(name) || inherited.count(name) || module_scope_names().count(name))) {
		// Taken by a member or by the mirror rather than by another local: a suffix that says so.
		base += p_clash_suffix;
		name = base;
	}
	for (int n = 2; clashes(name); n++) {
		name = base + std::to_string(n);
	}
	r_f.taken.insert(name);
	return name;
}

// ================================================================================================
// The class pre-pass

bool converter::is_static_initializer(const expr_ptr &p_e) {
	if (!p_e) {
		return true;
	}
	switch (p_e->kind) {
		case ek::number:
		case ek::string:
		case ek::boolean:
		case ek::null:
			return true;
		case ek::unary:
		case ek::binary:
		case ek::array:
		case ek::dictionary:
			for (const expr_ptr &a : p_e->args) {
				if (!is_static_initializer(a)) {
					return false;
				}
			}
			return p_e->kind != ek::binary || p_e->text == "+" || p_e->text == "-" || p_e->text == "*";
		case ek::call: {
			// A math constructor, or `SomeClass.new()`: an archetype, which a data member's default
			// may be.
			const expr_ptr &callee = p_e->args[0];
			if (callee->kind == ek::ident && is_math_type(callee->text)) {
				for (size_t i = 1; i < p_e->args.size(); i++) {
					if (!is_static_initializer(p_e->args[i])) {
						return false;
					}
				}
				return true;
			}
			if (callee->kind == ek::attribute && callee->text == "new" && p_e->args.size() == 1
					&& callee->args[0]->kind == ek::ident && is_godot_class(callee->args[0]->text)) {
				return true;
			}
			return false;
		}
		case ek::attribute:
			// `Vector2.ZERO`, `Node.PROCESS_MODE_ALWAYS`, `State.IDLE`.
			if (p_e->args[0]->kind != ek::ident) {
				return false;
			}
			if (is_math_type(p_e->args[0]->text) || is_godot_class(p_e->args[0]->text)) {
				return true;
			}
			for (const auto &c : classes) {
				if (c->enums.count(p_e->args[0]->text)) {
					return true;
				}
			}
			return false;
		case ek::ident: {
			if (p_e->text == "PI" || p_e->text == "TAU" || p_e->text == "INF" || p_e->text == "NAN"
					|| find_global(p_e->text, { api::kind::enum_value }) != nullptr) {
				return true;
			}
			// Another member's constant: inlined, because a default cannot read a member.
			for (const auto &c : classes) {
				auto v = c->vars.find(p_e->text);
				if (v != c->vars.end() && v->second.is_const && v->second.decl->init && v->second.decl->init.get() != p_e.get()) {
					return is_static_initializer(v->second.decl->init);
				}
				for (const auto &kv : c->enums) {
					if (kv.first.rfind("<anonymous", 0) == 0) {
						for (const auto &val : kv.second.values) {
							if (val.first == p_e->text) {
								return true;
							}
						}
					}
				}
			}
			return false;
		}
		default:
			return false;
	}
}

void converter::scan_expr(class_ctx &r_cls, fn_info &r_fn, const expr_ptr &p_e, bool p_awaited, const std::set<std::string> &p_enclosing) {
	if (!p_e) {
		return;
	}
	if (p_e->kind == ek::await_) {
		r_fn.awaits = true;
		for (const expr_ptr &a : p_e->args) {
			scan_expr(r_cls, r_fn, a, true, p_enclosing);
		}
		return;
	}
	if (p_e->kind == ek::lambda && p_e->lambda) {
		// Hoisted to a method of its own, named after the function it was written in. Verse has
		// no anonymous functions and no nested ones yet.
		//
		// TODO(nested-functions): when Verse accepts a nested function -- and later a lambda --
		// emit it in place instead, where its captures come for free.
		auto lf = std::make_shared<fn_info>();
		lf->is_lambda = true;
		lf->decl = p_e->lambda;
		const std::string owner = r_fn.is_lambda ? r_fn.verse_name : pascal(r_fn.gd_name);
		int n = 1;
		for (const auto &other : r_cls.lambdas) {
			n += other->verse_name.rfind(owner + "Lambda", 0) == 0 ? 1 : 0;
		}
		lf->gd_name = "<lambda:" + std::to_string(p_e->line) + ":" + std::to_string(r_cls.lambdas.size()) + ">";
		lf->verse_name = claim_member_name(r_cls, owner + "Lambda" + std::to_string(n), p_e->line, "lambda");
		// What it captures: the enclosing function's names that its body reads and does not
		// declare itself.
		std::set<std::string> own;
		for (const param &p : p_e->lambda->params) {
			own.insert(p.name);
		}
		std::function<void(const expr_ptr &)> reads;
		std::set<std::string> read_names;
		reads = [&](const expr_ptr &e) {
			if (!e) {
				return;
			}
			if (e->kind == ek::ident) {
				read_names.insert(e->text);
			}
			for (const expr_ptr &a : e->args) {
				reads(a);
			}
		};
		std::function<void(const std::vector<stmt_ptr> &)> walk = [&](const std::vector<stmt_ptr> &body) {
			for (const stmt_ptr &s : body) {
				if (s->kind == sk::var) {
					own.insert(s->text);
				}
				if (s->kind == sk::for_) {
					own.insert(s->for_var);
				}
				for (const expr_ptr &a : s->args) {
					reads(a);
				}
				for (const if_branch &b : s->branches) {
					reads(b.condition);
					walk(b.body);
				}
				for (const match_branch &b : s->cases) {
					walk(b.body);
				}
				walk(s->body);
			}
		};
		walk(p_e->lambda->body);
		for (const std::string &name : read_names) {
			if (p_enclosing.count(name) && !own.count(name)) {
				lf->captures.push_back(name);
			}
		}
		p_e->text = lf->verse_name; // the emitter finds it by this
		r_cls.lambdas.push_back(lf);
		r_cls.fns[lf->gd_name] = lf;
		return;
	}
	if (p_e->kind == ek::call && !p_e->args.empty()) {
		const expr_ptr &callee = p_e->args[0];
		std::string name;
		if (callee->kind == ek::ident) {
			name = callee->text;
		} else if (callee->kind == ek::attribute && callee->args[0]->kind == ek::self) {
			name = callee->text;
		}
		if (!name.empty() && r_cls.fns.count(name)) {
			r_fn.calls.insert(name);
			if (!p_awaited) {
				r_fn.unawaited_calls.insert(name);
			}
		}
	}
	for (const expr_ptr &a : p_e->args) {
		scan_expr(r_cls, r_fn, a, false, p_enclosing);
	}
}

void converter::scan_stmts(class_ctx &r_cls, fn_info &r_fn, const std::vector<stmt_ptr> &p_body, const std::set<std::string> &p_enclosing) {
	for (const stmt_ptr &s : p_body) {
		for (const expr_ptr &a : s->args) {
			scan_expr(r_cls, r_fn, a, false, p_enclosing);
		}
		for (const if_branch &b : s->branches) {
			scan_expr(r_cls, r_fn, b.condition, false, p_enclosing);
			scan_stmts(r_cls, r_fn, b.body, p_enclosing);
		}
		for (const match_branch &b : s->cases) {
			for (const expr_ptr &p : b.patterns) {
				scan_expr(r_cls, r_fn, p, false, p_enclosing);
			}
			scan_expr(r_cls, r_fn, b.guard, false, p_enclosing);
			scan_stmts(r_cls, r_fn, b.body, p_enclosing);
		}
		scan_stmts(r_cls, r_fn, s->body, p_enclosing);
	}
}

void converter::analyse_function(class_ctx &r_cls, fn_info &r_fn) {
	std::set<std::string> enclosing;
	for (const param &p : r_fn.decl->params) {
		enclosing.insert(p.name);
	}
	for (const std::string &c : r_fn.captures) {
		enclosing.insert(c);
	}
	std::function<void(const std::vector<stmt_ptr> &)> locals = [&](const std::vector<stmt_ptr> &body) {
		for (const stmt_ptr &s : body) {
			if (s->kind == sk::var) {
				enclosing.insert(s->text);
			}
			if (s->kind == sk::for_) {
				enclosing.insert(s->for_var);
			}
			for (const if_branch &b : s->branches) {
				locals(b.body);
			}
			for (const match_branch &b : s->cases) {
				locals(b.body);
			}
			locals(s->body);
		}
	};
	locals(r_fn.decl->body);
	scan_stmts(r_cls, r_fn, r_fn.decl->body, enclosing);
}

void converter::solve_effects(class_ctx &r_cls) {
	// Verse checks effects, and GDScript has none to read, so they are derived:
	//
	//   - a method that awaits is `<suspends>`;
	//   - a method that calls one of those without awaiting it `spawn`s it, and `spawn` may only
	//     be written in an unnarrowed body, so it has no specifier;
	//   - so does anything that calls a method with no specifier, because narrowing is contagious
	//     downward (CLAUDE.md, "Transactions, effects and raising");
	//   - everything else is `<transacts>`, the yardstick's own choice for a helper, which is what
	//     lets it be called from inside an `if`.
	//
	// A virtual keeps Godot's declaration, which has no specifier. One that awaits cannot be
	// `<suspends>` -- that is a different function (glitch 3532) -- so its body moves to a helper
	// the override spawns.
	std::vector<fn_info *> all;
	for (auto &kv : r_cls.fns) {
		all.push_back(kv.second.get());
	}
	for (fn_info *f : all) {
		if (f->is_virtual) {
			f->eff = effect::none;
			f->split_async = f->awaits;
		} else if (f->awaits) {
			f->eff = effect::suspends;
		} else {
			f->eff = effect::transacts;
		}
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (fn_info *f : all) {
			if (f->eff != effect::transacts) {
				continue;
			}
			if (f->spawns) {
				f->eff = effect::none;
				changed = true;
				continue;
			}
			for (const std::string &callee : f->calls) {
				fn_info *g = own_fn(r_cls, callee);
				if (!g) {
					continue;
				}
				const bool spawns = g->eff == effect::suspends && f->unawaited_calls.count(callee);
				// A virtual that awaits is spawned through its helper, and its own declaration has
				// no specifier either way.
				if (spawns || g->eff == effect::none || g->split_async) {
					f->eff = effect::none;
					changed = true;
					break;
				}
			}
		}
	}
}

void converter::prepare_class(class_ctx &r_cls) {
	class_decl &d = *r_cls.decl;

	// The base: a Godot class, another script class, or (a path) whatever that script's class is.
	std::string extends = d.extends;
	if (d.extends_is_path) {
		for (const auto &kv : in.script_classes) {
			if (kv.second.path == extends) {
				extends = kv.first;
				break;
			}
		}
	}
	if (extends.empty()) {
		extends = "RefCounted";
	}
	if (is_godot_class(extends)) {
		r_cls.base_godot = extends;
		r_cls.base_verse = verse_class_of(extends);
	} else if (in.script_classes.count(extends)) {
		const VerseGdScriptClass &s = in.script_classes.at(extends);
		if (s.is_binding) {
			note(d.line, "`" + d.extends + "` is still GDScript, and a Verse class cannot extend a script class's binding: convert `"
							+ d.extends + "` first",
					true);
		}
		r_cls.base_godot = s.godot_base.empty() ? "Node" : s.godot_base;
		r_cls.base_verse = s.verse_name;
		r_cls.base_script = s.verse_name;
	} else if (r_cls.outer && r_cls.outer->inner_classes.count(extends)) {
		r_cls.base_godot = "RefCounted";
		r_cls.base_verse = r_cls.outer->inner_classes[extends];
	} else {
		note(d.line, "`extends " + d.extends + "` names nothing the converter can find; the class extends `node` instead", true);
		r_cls.base_godot = "Node";
		r_cls.base_verse = "node";
	}
	if (!r_cls.base_script.empty()) {
		note(d.line, "`" + r_cls.verse_name + "` extends the script class `" + r_cls.base_script
						+ "`, which has to be Verse too: a Verse class cannot extend a GDScript one");
	}

	// Inner classes first, so their names resolve in the outer class's members.
	for (member &m : d.members) {
		if (m.kind == member::kind::inner_class) {
			r_cls.inner_classes[m.inner->name] = r_cls.verse_name + "_" + verse_snake_case(m.inner->name);
		}
	}
	for (member &m : d.members) {
		if (m.kind == member::kind::enum_) {
			enum_info e;
			e.gd_name = m.enum_->name;
			const std::string stem = m.enum_->name.empty() ? "values" : verse_snake_case(m.enum_->name);
			e.verse_name = r_cls.verse_name + "_" + stem;
			bool explicit_values = false;
			for (const auto &v : m.enum_->values) {
				e.values.push_back({ v.first, pascal(v.first) });
				explicit_values = explicit_values || v.second;
			}
			if (explicit_values) {
				note(m.enum_->line, "`" + (m.enum_->name.empty() ? std::string("enum") : m.enum_->name)
								+ "` sets its values explicitly, and a Verse enum has no values: anything that relies on the numbers "
								  "(arithmetic, a value stored in a scene, `int(...)`) needs another look",
						true);
			}
			r_cls.enums[m.enum_->name.empty() ? "<anonymous" + std::to_string(m.enum_->line) + ">" : m.enum_->name] = e;
		}
	}
	for (member &m : d.members) {
		if (m.kind == member::kind::signal) {
			sig_info s;
			s.gd_name = m.signal->name;
			s.verse_name = claim_member_name(r_cls, pascal(m.signal->name), m.signal->line, m.signal->name);
			for (const param &p : m.signal->params) {
				ty t = ty_from_gd(p.type_text, r_cls, m.signal->line);
				if (!t.known()) {
					t = ty::of(tk::variant);
				}
				if (t.k == tk::object) {
					// A Godot signal can carry null for any object, so its payload is an option.
					t = ty::option_of(t);
				}
				s.params.push_back(t);
			}
			s.payload = payload_of(s.params);
			r_cls.sigs[s.gd_name] = s;
			if (!r_cls.is_inner) {
				res.renames.push_back({ VerseGdRename::Kind::Signal, s.gd_name, s.verse_name });
			}
		}
	}
	// Functions before vars' types are settled, so a var's accessor can name them.
	for (member &m : d.members) {
		if (m.kind != member::kind::func) {
			continue;
		}
		auto f = std::make_shared<fn_info>();
		f->gd_name = m.func->name;
		f->decl = m.func;
		f->is_static = m.func->is_static;
		const api::member *vrow = m.func->name.size() && m.func->name[0] == '_'
				? find_member(r_cls.base_godot, m.func->name, { api::kind::virtual_method })
				: nullptr;
		if (vrow && !f->is_static) {
			f->is_virtual = true;
			f->vrow = vrow;
			f->verse_name = vrow->verse;
			r_cls.taken.insert(f->verse_name);
		} else if (f->is_static) {
			f->verse_name = pascal(m.func->name);
			// A static is a module-level function, so the class's name goes in front of it: a module
			// name and a class member of the same name are ambiguous whatever their signatures.
			f->verse_name = verse_pascal_case(r_cls.verse_name) + f->verse_name;
		} else {
			f->verse_name = claim_member_name(r_cls, pascal(m.func->name), m.func->line, m.func->name);
			if (!r_cls.is_inner) {
				res.renames.push_back({ VerseGdRename::Kind::Method, f->gd_name, f->verse_name });
			}
		}
		r_cls.fns[f->gd_name] = f;
	}
	for (member &m : d.members) {
		if (m.kind != member::kind::var) {
			continue;
		}
		var_decl &vd = *m.var;
		var_info v;
		v.gd_name = vd.name;
		v.decl = m.var;
		v.is_const = vd.is_const;
		for (const annotation &a : vd.annotations) {
			if (a.name == "onready") {
				v.onready = true;
			}
			if (a.name.rfind("export", 0) == 0) {
				v.exported = true;
			}
		}
		const bool preload = vd.init && vd.init->kind == ek::call && vd.init->args[0]->kind == ek::ident
				&& (vd.init->args[0]->text == "preload" || vd.init->args[0]->text == "load");
		if (vd.is_const && !preload) {
			v.verse_name = claim_member_name(r_cls, pascal(vd.name), vd.line, vd.name);
		} else {
			v.verse_name = claim_member_name(r_cls, pascal(vd.name), vd.line, vd.name);
		}
		if (preload) {
			v.is_const = false;
			v.deferred_init = true;
		}
		if (vd.is_static) {
			note(vd.line, "`static var " + vd.name + "` became an ordinary member: Verse has no static data, so each instance has its own", true);
		}
		if (!r_cls.is_inner && !v.is_const) {
			res.renames.push_back({ VerseGdRename::Kind::Property, v.gd_name, v.verse_name });
		}
		r_cls.vars[v.gd_name] = v;
	}
	// Accessor bodies become methods, named for what they do.
	for (auto &kv : r_cls.vars) {
		var_info &v = kv.second;
		const std::string stem = v.verse_name;
		if (v.decl->setter) {
			auto f = std::make_shared<fn_info>();
			f->gd_name = "<set:" + v.gd_name + ">";
			f->decl = v.decl->setter;
			f->verse_name = claim_member_name(r_cls, "Set" + stem, v.decl->line, v.gd_name + "'s setter");
			r_cls.fns[f->gd_name] = f;
			v.setter_fn = f->verse_name;
		} else if (!v.decl->setget_setter.empty() && r_cls.fns.count(v.decl->setget_setter)) {
			v.setter_fn = r_cls.fns[v.decl->setget_setter]->verse_name;
		}
		if (v.decl->getter) {
			auto f = std::make_shared<fn_info>();
			f->gd_name = "<get:" + v.gd_name + ">";
			f->decl = v.decl->getter;
			f->verse_name = claim_member_name(r_cls, "Get" + stem, v.decl->line, v.gd_name + "'s getter");
			r_cls.fns[f->gd_name] = f;
			v.getter_fn = f->verse_name;
		} else if (!v.decl->setget_getter.empty() && r_cls.fns.count(v.decl->setget_getter)) {
			v.getter_fn = r_cls.fns[v.decl->setget_getter]->verse_name;
		}
		if (!v.setter_fn.empty() || !v.getter_fn.empty()) {
			note(v.decl->line, "`" + v.gd_name + "`'s accessors became `" + (v.setter_fn.empty() ? v.getter_fn : v.setter_fn)
							+ (v.setter_fn.empty() || v.getter_fn.empty() ? "" : "`/`" + v.getter_fn)
							+ "`: this file calls them, but a value Godot writes -- the inspector, a scene, an animation -- goes straight to `"
							+ v.verse_name + "` and skips them",
					true);
		}
	}

	// Signatures, which need every name above.
	for (auto &kv : r_cls.fns) {
		fn_info &f = *kv.second;
		std::vector<api_param> vparams;
		if (f.is_virtual) {
			vparams = decode_params(f.vrow->params);
		}
		for (size_t i = 0; i < f.decl->params.size(); i++) {
			const param &p = f.decl->params[i];
			f.param_names.push_back(pascal(p.name));
			ty t = ty::of(tk::unknown);
			bool typed = false;
			if (f.is_virtual && i < vparams.size()) {
				t = ty_from_verse(vparams[i].type);
				typed = true;
			} else if (!p.type_text.empty() && p.type_text != "=") {
				t = ty_from_gd(p.type_text, r_cls, f.decl->line);
				typed = true;
				if (t.k == tk::object) {
					t = ty::option_of(t);
				}
			} else if (!r_cls.is_inner && in.param_hints.count(f.gd_name) && in.param_hints.at(f.gd_name).count(int(i))) {
				t = ty_from_verse(in.param_hints.at(f.gd_name).at(int(i)));
				typed = t.known();
				if (typed) {
					note(f.decl->line, "`" + p.name + "` is a `" + verse_text(t) + "` because that is what every other converted file passes it");
				}
			} else if (p.default_value) {
				switch (p.default_value->kind) {
					case ek::number:
						t = ty::of(p.default_value->is_float ? tk::float_ : tk::int_);
						typed = true;
						break;
					case ek::string:
						t = ty::of(tk::string);
						typed = true;
						break;
					case ek::boolean:
						t = ty::of(tk::logic);
						typed = true;
						break;
					default:
						break;
				}
			}
			f.param_types.push_back(t);
			f.param_typed.push_back(typed);
			f.param_optional.push_back(p.default_value != nullptr);
		}
		for (const std::string &c : f.captures) {
			f.param_names.push_back(pascal(c));
			f.param_types.push_back(ty::of(tk::unknown));
			f.param_typed.push_back(false);
			f.param_optional.push_back(false);
		}
		if (f.is_virtual) {
			f.ret = ty_from_verse(f.vrow->result);
			f.ret_declared = true;
			if (f.vrow->form == api::shape::predicate) {
				f.ret = ty::of(tk::logic);
				f.predicate_helper = true;
			}
		} else if (!f.decl->return_type.empty()) {
			f.ret = ty_from_gd(f.decl->return_type, r_cls, f.decl->line);
			if (f.ret.k == tk::object) {
				f.ret = ty::option_of(f.ret);
			}
			f.ret_declared = true;
		} else if (f.gd_name.rfind("<set:", 0) == 0) {
			f.ret = ty::of(tk::void_);
			f.ret_declared = true;
		}
	}
	// A setter's parameter is the var's type; a getter answers it.
	for (auto &kv : r_cls.vars) {
		var_info &v = kv.second;
		v.t = ty_from_gd(v.decl->type_text, r_cls, v.decl->line);

		if (v.t.k == tk::object) {
			v.t = ty::option_of(v.t);
		}
	}
	for (member &m : d.members) {
		if (m.kind == member::kind::func) {
			analyse_function(r_cls, *r_cls.fns[m.func->name]);
		}
	}
	for (auto &kv : r_cls.vars) {
		if (kv.second.decl->setter) {
			analyse_function(r_cls, *r_cls.fns["<set:" + kv.first + ">"]);
		}
		if (kv.second.decl->getter) {
			analyse_function(r_cls, *r_cls.fns["<get:" + kv.first + ">"]);
		}
	}
	// Lambdas found while scanning are functions too, and may hold lambdas of their own.
	for (size_t i = 0; i < r_cls.lambdas.size(); i++) {
		fn_info &lf = *r_cls.lambdas[i];
		for (size_t p = 0; p < lf.decl->params.size(); p++) {
			const param &pp = lf.decl->params[p];
			lf.param_names.push_back(pascal(pp.name));
			ty t = pp.type_text.empty() || pp.type_text == "=" ? ty::of(tk::unknown) : ty_from_gd(pp.type_text, r_cls, lf.decl->line);
			lf.param_types.push_back(t);
			lf.param_typed.push_back(t.known());
			lf.param_optional.push_back(pp.default_value != nullptr);
		}
		for (const std::string &c : lf.captures) {
			lf.param_names.push_back(pascal(c));
			lf.param_types.push_back(ty::of(tk::unknown));
			lf.param_typed.push_back(false);
			lf.param_optional.push_back(false);
		}
		if (!lf.decl->return_type.empty()) {
			lf.ret = ty_from_gd(lf.decl->return_type, r_cls, lf.decl->line);
			lf.ret_declared = true;
		}
		analyse_function(r_cls, lf);
	}
	solve_effects(r_cls);
	for (auto &kv : r_cls.fns) {
		fn_info &f = *kv.second;
		if (f.split_async) {
			f.helper_name = claim_member_name(r_cls, strip_leading_underscores(f.verse_name) + "Async", f.decl->line, f.gd_name);
		} else if (f.predicate_helper) {
			f.helper_name = claim_member_name(r_cls, strip_leading_underscores(f.verse_name) + "Value", f.decl->line, f.gd_name);
		}
	}
}

// ================================================================================================
// Expressions

std::string converter::hoist(fctx &r_f, sctx &r_s, const std::string &p_code, const std::string &p_hint) {
	auto found = r_s.hoisted.find(p_code);
	if (found != r_s.hoisted.end()) {
		return found->second;
	}
	const std::string name = fresh(r_f, p_hint);
	r_s.clauses.push_back(name + " := " + p_code);
	r_s.hoisted[p_code] = name;
	return name;
}

// A failable value, bound in the statement's clauses; a test, made a `logic`; anything else as is.
std::string converter::value_of(fctx &r_f, const ev &p_e, sctx &r_s, const std::string &p_hint) {
	if (p_e.f == form::fails) {
		return hoist(r_f, r_s, p_e.code, p_hint == "Value" && !p_e.hint.empty() ? p_e.hint : p_hint);
	}
	if (p_e.f == form::test) {
		return "logic{" + p_e.code + "}";
	}
	return p_e.code;
}

// What goes in an `if`'s clause list: a test as it is, a `logic` queried, a number or a string
// compared the way GDScript's truthiness does.
std::string converter::test_of(const ev &p_e) {
	if (p_e.f != form::value) {
		return p_e.code;
	}
	switch (p_e.t.k) {
		case tk::logic:
		case tk::option:
			return paren(p_e, 90) + "?";
		case tk::int_:
			return paren(p_e, 50) + " <> 0";
		case tk::float_:
			return paren(p_e, 50) + " <> 0.0";
		case tk::string:
		case tk::array:
			return paren(p_e, 95) + ".Length > 0";
		case tk::object:
			// A plain (non-optional) object is never null.
			return "true?";
		default:
			return paren(p_e, 90) + "?";
	}
}

// The value inside an option, bound in the clauses.
std::string converter::unwrap(fctx &r_f, const ev &p_e, sctx &r_s, const std::string &p_hint, ty &r_inner) {
	if (p_e.t.k != tk::option) {
		r_inner = p_e.t;
		return value_of(r_f, p_e, r_s, p_hint);
	}
	r_inner = p_e.t.inner();
	const std::string code = value_of(r_f, p_e, r_s, p_hint);
	// `Mob?` binds as `MobValue`: the name says what it holds.
	bool simple = !code.empty() && std::isupper((unsigned char)code[0]);
	for (char c : code) {
		simple = simple && (std::isalnum((unsigned char)c) || c == '_');
	}
	return hoist(r_f, r_s, code + "?", p_hint == "Value" && simple ? code + "Value" : p_hint);
}

std::string converter::fold(const sctx &p_s, const std::string &p_value, const std::string &p_default) {
	if (p_s.clauses.empty()) {
		return p_value;
	}
	std::string clauses;
	for (size_t i = 0; i < p_s.clauses.size(); i++) {
		clauses += (i ? ", " : "") + p_s.clauses[i];
	}
	return "if (" + clauses + ") then " + p_value + " else " + p_default;
}

std::string converter::coerce(fctx &r_f, const ev &p_e, const ty &p_target, sctx &r_s, int p_line) {
	// An untyped parameter used where a type is wanted takes that type -- `position = pos` makes
	// `pos` a `vector2` -- on the next pass, and the first use wins.
	if (p_e.param_index >= 0 && r_f.fn && size_t(p_e.param_index) < r_f.fn->param_typed.size()
			&& !r_f.fn->param_typed[size_t(p_e.param_index)] && p_target.known() && p_target.k != tk::variant) {
		r_f.fn->param_types[size_t(p_e.param_index)] = p_target.k == tk::object ? ty::option_of(p_target) : p_target;
		r_f.fn->param_typed[size_t(p_e.param_index)] = true;
	}
	const ty &from = p_e.t;
	const ty &to = p_target;
	if (to.k == tk::unknown) {
		return value_of(r_f, p_e, r_s, "Value");
	}
	if (to.k == tk::logic) {
		if (p_e.f == form::test) {
			return "logic{" + p_e.code + "}";
		}
		if (p_e.f == form::fails) {
			return "logic{" + p_e.code + "}";
		}
		if (from.k == tk::logic) {
			return p_e.code;
		}
		if (from.known()) {
			return "logic{" + test_of(p_e) + "}";
		}
		return p_e.code;
	}
	if (to.k == tk::option) {
		if (p_e.is_null) {
			return "false";
		}
		if (from.k == tk::option) {
			return value_of(r_f, p_e, r_s, "Value");
		}
		if (p_e.f == form::fails) {
			return "option{" + p_e.code + "}";
		}
		ev inner = p_e;
		return "option{" + coerce(r_f, inner, to.inner(), r_s, p_line) + "}";
	}
	if (to.k == tk::variant) {
		if (from.k == tk::variant) {
			return value_of(r_f, p_e, r_s, "Value");
		}
		if (p_e.is_null) {
			return "variant{}";
		}
		if (from.k == tk::option && from.inner().k == tk::object) {
			return "VariantMaybeObject(" + value_of(r_f, p_e, r_s, "Value") + ")";
		}
		if (from.k == tk::enum_) {
			return "VariantInt(ToInt(" + value_of(r_f, p_e, r_s, "Value") + "))";
		}
		const std::string suffix = variant_suffix(from.k == tk::option ? from.inner() : from);
		if (!suffix.empty()) {
			std::string code = value_of(r_f, p_e, r_s, "Value");
			if (from.k == tk::option) {
				ty inner;
				code = unwrap(r_f, p_e, r_s, "Value", inner);
			}
			return "Variant" + suffix + "(" + code + ")";
		}
		if (from.known()) {
			return "MakeVariant[" + value_of(r_f, p_e, r_s, "Value") + "]";
		}
		return value_of(r_f, p_e, r_s, "Value");
	}
	if (from.k == tk::variant && to.k != tk::variant) {
		// A variant read into a typed slot: the reader for that lane, which can decline.
		const std::string suffix = variant_suffix(to.k == tk::option ? to.inner() : to);
		const std::string code = value_of(r_f, p_e, r_s, "Value");
		if (suffix.empty()) {
			r_s.todos.push_back("a `variant` has no reader for `" + verse_text(to) + "`");
			return code;
		}
		std::string read = paren(ev{ code }, 95) + ".As" + suffix + "[]";
		if (to.k == tk::object || (to.k == tk::option && to.inner().k == tk::object)) {
			const ty &cls = to.k == tk::option ? to.inner() : to;
			read = verse_text(cls) + "[" + read + "]";
		}
		return hoist(r_f, r_s, read, "Read");
	}
	if (to.k == tk::float_ && from.k == tk::int_) {
		if (p_e.int_literal) {
			return p_e.code + ".0";
		}
		return paren(p_e, 70) + " * 1.0";
	}
	if (to.k == tk::int_ && from.k == tk::enum_) {
		return "ToInt(" + value_of(r_f, p_e, r_s, "Value") + ")";
	}
	if (to.k == tk::object && (from.k == tk::object || from.k == tk::option)) {
		ty inner;
		const std::string code = unwrap(r_f, p_e, r_s, "Value", inner);
		const bool is_ancestor = inner.k == tk::object && !to.script.empty() ? inner.script == to.script
																		 : (inner.k == tk::object && inner.script.empty() && godot_inherits(inner.name, to.name.empty() ? "Object" : to.name))
						|| (inner.k == tk::object && !inner.script.empty() && to.script.empty() && godot_inherits(inner.name, to.name.empty() ? "Object" : to.name));
		if (is_ancestor || to.name == "Object") {
			return code;
		}
		return hoist(r_f, r_s, verse_text(to) + "[" + code + "]", sanitize_identifier(verse_binding_member_name(verse_text(to))));
	}
	if (from.k == tk::option && to.k != tk::option) {
		ty inner;
		const std::string code = unwrap(r_f, p_e, r_s, "Value", inner);
		ev unwrapped;
		unwrapped.code = code;
		unwrapped.t = inner;
		return coerce(r_f, unwrapped, to, r_s, p_line);
	}
	if (to.k == tk::array && p_e.code == "array{}") {
		return p_e.code;
	}
	return value_of(r_f, p_e, r_s, "Value");
}

// One `{...}` of an interpolated string, or the text itself for a literal. Verse's interpolation
// calls ToString, which exists for numbers, strings and (the mirror's) objects and nothing else, so
// the rest are spelled out: a math value field by field the way Godot prints it, a `logic` as the
// word.
std::string converter::interpolate(fctx &r_f, const ev &p_e, sctx &r_s) {
	if (p_e.str_literal) {
		return p_e.str_inner;
	}
	if (p_e.f == form::test || p_e.t.k == tk::logic) {
		const std::string test = p_e.f == form::test ? p_e.code : paren(p_e, 90) + "?";
		return "{if (" + test + ") then \"true\" else \"false\"}";
	}
	ty inner;
	const std::string code = p_e.t.k == tk::option ? unwrap(r_f, p_e, r_s, "Value", inner) : value_of(r_f, p_e, r_s, "Value");
	if (p_e.t.k != tk::option) {
		inner = p_e.t;
	}
	switch (inner.k) {
		case tk::int_:
		case tk::float_:
		case tk::string:
		case tk::object:
			return "{" + code + "}";
		case tk::enum_:
			if (enum_type_names().count(inner.name)) {
				return "{ToInt(" + code + ")}";
			}
			break;
		case tk::math: {
			const api::class_row *row = class_row_of(inner.name);
			std::vector<api_param> fields = decode_params(row->fields);
			std::string out = "(";
			for (size_t i = 0; i < fields.size(); i++) {
				if (fields[i].type != "float" && fields[i].type != "int") {
					r_s.todos.push_back("a `" + inner.name + "` in a string: its fields are not numbers, so it has no one-line spelling here");
					return "";
				}
				out += (i ? ", {" : "{") + paren(ev{ code }, 95) + "." + fields[i].name + "}";
			}
			return out + ")";
		}
		default:
			break;
	}
	r_s.todos.push_back("a `" + verse_text(inner) + "` in a string: Verse has no `ToString` for it; read it as its own type first");
	return "";
}

ev converter::ex_node_path(fctx &r_f, const std::string &p_path, const std::string &p_receiver, sctx &r_s, int p_line) {
	std::string type;
	if (p_receiver.empty()) {
		auto found = in.node_types.find(p_path);
		if (found != in.node_types.end()) {
			type = found->second;
		}
	}
	std::string last = p_path;
	const size_t slash = last.find_last_of('/');
	if (slash != std::string::npos) {
		last = last.substr(slash + 1);
	}
	const std::string stem = sanitize_identifier(last);
	const std::string lookup = (p_receiver.empty() ? "" : p_receiver + ".") + "GetNode[\"" + escape_verse(p_path) + "\"]";
	ev e;
	if (type.empty() || type == "Node") {
		e.code = hoist(r_f, r_s, lookup, stem + "Node");
		e.t = ty::object_of("Node");
		return e;
	}
	ty target;
	if (is_godot_class(type)) {
		target = ty::object_of(type);
	} else if (type == res.gd_class_name) {
		target = self_type(*root_ctx);
	} else if (in.script_classes.count(type)) {
		uses_script_class(in.script_classes.at(type));
		target = ty::object_of(in.script_classes.at(type).godot_base, in.script_classes.at(type).verse_name);
	} else {
		note(p_line, "`$" + p_path + "` is a `" + type + "`, which the converter does not know; it is looked up as a `node`");
		e.code = hoist(r_f, r_s, lookup, stem + "Node");
		e.t = ty::object_of("Node");
		return e;
	}
	const std::string found = hoist(r_f, r_s, lookup, stem + "Found");
	e.code = hoist(r_f, r_s, verse_text(target) + "[" + found + "]", stem + "Node");
	e.t = target;
	return e;
}

ev converter::ex_ident(fctx &r_f, const expr_ptr &p_e, sctx &r_s) {
	const std::string &name = p_e->text;
	class_ctx &cls = *r_f.cls;
	ev e;
	if (const local *l = find_local(r_f, name)) {
		e.code = l->verse;
		e.t = l->t;
		e.param_index = l->param_index;
		if (l->param_index >= 0 && r_f.fn && size_t(l->param_index) < r_f.fn->param_types.size()) {
			// A settled parameter reads as its settled type on the next pass.
			e.t = r_f.fn->param_types[size_t(l->param_index)].known() ? r_f.fn->param_types[size_t(l->param_index)] : ty::of(tk::variant);
		}
		return e;
	}
	for (class_ctx *c = &cls; c; c = c->outer) {
		auto v = c->vars.find(name);
		if (v != c->vars.end() && c == &cls && !r_f.fn && v->second.is_const && v->second.decl->init) {
			// A member's default is written before there is a `Self` to read a constant from,
			// so the constant's own value is written instead.
			return ex(r_f, v->second.decl->init, r_s);
		}
		if (v != c->vars.end() && c == &cls) {
			if (!v->second.getter_fn.empty() && !(r_f.fn && (r_f.fn->gd_name == "<get:" + name + ">" || r_f.fn->gd_name == "<set:" + name + ">"))) {
				e.code = v->second.getter_fn + "()";
			} else {
				e.code = v->second.verse_name;
			}
			e.t = v->second.t;
			if (!e.t.known() && v->second.decl->init) {
				// An untyped member whose type came from its initialiser: that was settled when the
				// member was emitted.
				e.t = ty::of(tk::variant);
			}
			return e;
		}
		auto s = c->sigs.find(name);
		if (s != c->sigs.end() && c == &cls) {
			e.code = s->second.verse_name;
			e.t = ty::of(tk::event);
			e.t.payload = s->second.payload;
			return e;
		}
		auto en = c->enums.find(name);
		if (en != c->enums.end()) {
			e.t = ty::of(tk::type_value);
			e.t.tv = "own_enum";
			e.t.name = en->second.verse_name;
			e.code = en->second.verse_name;
			return e;
		}
		for (const auto &kv : c->enums) {
			if (kv.first.rfind("<anonymous", 0) == 0) {
				for (const auto &val : kv.second.values) {
					if (val.first == name) {
						e.code = kv.second.verse_name + "." + val.second;
						e.t = ty::enum_of(kv.second.verse_name);
						return e;
					}
				}
			}
		}
		auto inner = c->inner_classes.find(name);
		if (inner != c->inner_classes.end()) {
			e.t = ty::of(tk::type_value);
			e.t.tv = "script";
			e.t.script = inner->second;
			e.t.name = "RefCounted";
			e.code = inner->second;
			return e;
		}
	}
	if (fn_info *f = own_fn(cls, name)) {
		e.code = f->verse_name;
		e.t = ty::of(tk::function);
		e.t.payload = name;
		return e;
	}
	// A member of the Godot base, reached without `self.`.
	if (const api::member *row = find_member(cls.base_godot, name, { api::kind::property, api::kind::accessor_property, api::kind::signal })) {
		ev self;
		self.code = "Self";
		self.t = self_type(cls);
		self.is_self = true;
		return member_of_value(r_f, self, name, r_s, p_e->line);
		(void)row;
	}
	if (const api::member *row = find_member(cls.base_godot, name, { api::kind::constant, api::kind::enum_value })) {
		e.code = row->verse;
		e.t = ty_from_verse(row->result);
		return e;
	}
	if (name == "PI") {
		e.code = "MathPi";
		e.t = ty::of(tk::float_);
		return e;
	}
	if (name == "TAU") {
		e.code = "MathTau";
		e.t = ty::of(tk::float_);
		return e;
	}
	if (name == "INF") {
		e.code = "Inf";
		e.t = ty::of(tk::float_);
		return e;
	}
	if (name == "NAN") {
		e.code = "NaN";
		e.t = ty::of(tk::float_);
		return e;
	}
	if (const api::member *row = find_global(name, { api::kind::enum_value, api::kind::constant })) {
		e.code = row->verse;
		e.t = ty_from_verse(row->result);
		return e;
	}
	if (const api::member *row = find_global(name, { api::kind::enum_type })) {
		e.t = ty::of(tk::type_value);
		e.t.tv = "global_enum";
		e.t.name = row->verse;
		e.code = row->verse;
		return e;
	}
	if (const api::class_row *row = class_row_of(name)) {
		e.t = ty::of(tk::type_value);
		e.t.name = name;
		if (row->fields[0]) {
			e.t.tv = "math";
			e.code = name + "Statics";
		} else if (row->singleton_accessor[0]) {
			e.t.tv = "singleton";
			if (row->singleton_decides) {
				e.code = hoist(r_f, r_s, std::string(row->singleton_accessor) + "[]", name);
			} else {
				e.code = std::string(row->singleton_accessor) + "()";
			}
		} else {
			e.t.tv = "class";
			e.code = row->verse_name;
		}
		return e;
	}
	if (name == res.gd_class_name && !name.empty()) {
		e.t = ty::of(tk::type_value);
		e.t.tv = "script";
		e.t.script = root_ctx->verse_name;
		e.t.name = root_ctx->base_godot;
		e.code = root_ctx->verse_name;
		return e;
	}
	auto script = in.script_classes.find(name);
	if (script != in.script_classes.end()) {
		uses_script_class(script->second);
		e.t = ty::of(tk::type_value);
		e.t.tv = "script";
		e.t.script = script->second.verse_name;
		e.t.name = script->second.godot_base;
		e.code = script->second.verse_name;
		return e;
	}
	r_s.todos.push_back("`" + name + "` names nothing the converter can find");
	return ev::todo_marker();
}

ev converter::member_of_value(fctx &r_f, const ev &p_receiver, const std::string &p_name, sctx &r_s, int p_line) {
	class_ctx &cls = *r_f.cls;
	ev e;
	ty owner = p_receiver.t;
	std::string recv = p_receiver.code;

	if (owner.k == tk::type_value) {
		if (owner.tv == "own_enum") {
			for (class_ctx *c = &cls; c; c = c->outer) {
				for (const auto &kv : c->enums) {
					if (kv.second.verse_name != owner.name) {
						continue;
					}
					for (const auto &val : kv.second.values) {
						if (val.first == p_name) {
							e.code = owner.name + "." + val.second;
							e.t = ty::enum_of(owner.name);
							return e;
						}
					}
				}
			}
		}
		if (owner.tv == "class" || owner.tv == "math" || owner.tv == "singleton") {
			if (const api::member *row = find_member(owner.name, p_name, { api::kind::constant, api::kind::enum_value })) {
				e.code = row->verse;
				e.t = ty_from_verse(row->result);
				return e;
			}
			if (const api::member *row = find_member(owner.name, p_name, { api::kind::enum_type })) {
				e.t = ty::of(tk::type_value);
				e.t.tv = "global_enum";
				e.t.name = row->verse;
				e.code = row->verse;
				return e;
			}
			if (owner.tv == "singleton") {
				ev instance = p_receiver;
				instance.t = ty::object_of(owner.name);
				return member_of_value(r_f, instance, p_name, r_s, p_line);
			}
		}
		if (owner.tv == "global_enum") {
			// `Key.KEY_SPACE` -- the value is registered under its own name at @GlobalScope.
			if (const api::member *row = find_global(p_name, { api::kind::enum_value })) {
				e.code = row->verse;
				e.t = ty_from_verse(row->result);
				return e;
			}
		}
		if (owner.tv == "script" && owner.script == cls.verse_name) {
			auto v = cls.vars.find(p_name);
			if (v != cls.vars.end() && v->second.is_const) {
				r_s.todos.push_back("`" + p_name + "` is a class constant read through the class; Verse has no constant on a type");
				return ev::todo_marker();
			}
		}
		e.member_of = recv;
		e.member_name = p_name;
		e.member_owner = owner;
		e.has_owner = true;
		e.code = recv + "." + p_name;
		e.t = ty::of(tk::unknown);
		return e;
	}

	// Anything that has to be unwrapped first.
	if (owner.k == tk::option) {
		ty inner;
		recv = unwrap(r_f, p_receiver, r_s, sanitize_identifier(p_receiver.code.substr(0, p_receiver.code.find_first_of("[(."))) + "Value", inner);
		owner = inner;
	} else if (p_receiver.f == form::fails) {
		recv = value_of(r_f, p_receiver, r_s, "Value");
	}

	switch (owner.k) {
		case tk::math: {
			if (const api::member *row = find_member(owner.name, p_name, { api::kind::field })) {
				e.code = paren(ev{ recv }, 95) + "." + row->verse;
				if (recv.find(' ') != std::string::npos && recv.front() != '(') {
					e.code = "(" + recv + ")." + row->verse;
				}
				e.t = ty_from_verse(row->result);
				return e;
			}
			break;
		}
		case tk::object: {
			const bool own = owner.script == cls.verse_name;
			if (own) {
				auto v = cls.vars.find(p_name);
				if (v != cls.vars.end()) {
					e.code = p_receiver.is_self ? v->second.verse_name : recv + "." + v->second.verse_name;
					e.t = v->second.t;
					return e;
				}
				auto s = cls.sigs.find(p_name);
				if (s != cls.sigs.end()) {
					e.code = p_receiver.is_self ? s->second.verse_name : recv + "." + s->second.verse_name;
					e.t = ty::of(tk::event);
					e.t.payload = s->second.payload;
					return e;
				}
			}
			if (const api::member *row = find_member(owner.name.empty() ? "Object" : owner.name, p_name,
						{ api::kind::property, api::kind::accessor_property, api::kind::signal })) {
				const std::string prefix = p_receiver.is_self ? "" : recv + ".";
				if (row->what == api::kind::property) {
					e.code = prefix + row->verse;
					e.t = ty_from_verse(row->result);
					return e;
				}
				if (row->what == api::kind::signal) {
					e.code = prefix + row->verse + "()";
					e.t = ty::of(tk::signal);
					e.t.payload = row->result;
					return e;
				}
				// A property the mirror left as its get/set pair: read through the getter.
				const api::member *getter = find_member(owner.name, row->verse, { api::kind::method });
				if (getter) {
					e.code = prefix + getter->verse + (getter->form == api::shape::decides ? "[]" : "()");
					e.t = ty_from_verse(getter->result);
					e.f = getter->form == api::shape::decides ? form::fails : form::value;
					if (getter->form == api::shape::predicate) {
						e.code = prefix + getter->verse + "[]";
						e.f = form::test;
						e.t = ty::of(tk::logic);
					}
					return e;
				}
			}
			if (!owner.script.empty() && !own) {
				if (const VerseGdScriptClass *info = script_info(owner.script)) {
					auto prop = info->properties.find(p_name);
					if (prop != info->properties.end()) {
						e.code = recv + "." + prop->second.first;
						e.t = ty_from_verse(prop->second.second);
						return e;
					}
					auto sig = info->signals.find(p_name);
					if (sig != info->signals.end()) {
						e.code = recv + "." + sig->second.first;
						e.t = ty::of(tk::event);
						e.t.payload = sig->second.second;
						return e;
					}
				}
				// Another script class: its members are PascalCase whether it is Verse already or
				// reached through its generated binding (R-INT-7).
				e.code = recv + "." + pascal(p_name);
				e.t = ty::of(tk::unknown);
				note(p_line, "`" + p_name + "` on `" + owner.script + "` is assumed to be spelled `" + pascal(p_name)
								+ "`, as a converted script or its generated binding spells it");
				return e;
			}
			// Nothing the mirror knows: Godot's own dynamic read, which is what GDScript did.
			e.code = paren(ev{ recv }, 95) + ".Get(\"" + escape_verse(p_name) + "\")";
			if (p_receiver.is_self) {
				e.code = "Get(\"" + escape_verse(p_name) + "\")";
			}
			e.t = ty::of(tk::variant);
			note(p_line, "`" + p_name + "` is not a member of `" + (owner.name.empty() ? std::string("Object") : owner.name)
							+ "` the mirror knows, so it is read with `Get`, which answers a `variant`");
			return e;
		}
		case tk::array:
		case tk::string:
			break;
		default:
			break;
	}
	e.member_of = recv;
	e.member_name = p_name;
	e.member_owner = owner;
	e.has_owner = true;
	e.code = recv + "." + pascal(p_name);
	e.t = ty::of(tk::unknown);
	if (!owner.known() || owner.k == tk::variant) {
		r_s.todos.push_back("`." + p_name + "` is read from a value whose type is not known here; give it a type in the GDScript, or read it with `Get`");
		return ev::todo_marker();
	}
	return e;
}

ev converter::ex_attribute(fctx &r_f, const expr_ptr &p_e, sctx &r_s) {
	const expr_ptr &base = p_e->args[0];
	if (base->kind == ek::self) {
		ev self;
		self.code = "Self";
		self.t = self_type(*r_f.cls);
		self.is_self = true;
		// A method named through `self.` is resolved by the call.
		if (own_fn(*r_f.cls, p_e->text)) {
			ev fe;
			fe.code = own_fn(*r_f.cls, p_e->text)->verse_name;
			fe.t = ty::of(tk::function);
			fe.t.payload = p_e->text;
			return fe;
		}
		return member_of_value(r_f, self, p_e->text, r_s, p_e->line);
	}
	ev receiver = ex(r_f, base, r_s);
	if (receiver.todo) {
		return receiver;
	}
	return member_of_value(r_f, receiver, p_e->text, r_s, p_e->line);
}

ev converter::ex_await(fctx &r_f, const expr_ptr &p_e, sctx &r_s) {
	const expr_ptr &inner = p_e->args[0];
	if (inner->kind == ek::call) {
		const expr_ptr &callee = inner->args[0];
		std::string name;
		if (callee->kind == ek::ident) {
			name = callee->text;
		} else if (callee->kind == ek::attribute && callee->args[0]->kind == ek::self) {
			name = callee->text;
		}
		if (fn_info *f = own_fn(*r_f.cls, name)) {
			std::vector<expr_ptr> args(inner->args.begin() + 1, inner->args.end());
			return call_own(r_f, "", *f, args, r_s, p_e->line, true);
		}
		if (callee->kind == ek::attribute && callee->args[0]->kind != ek::self) {
			ev recv = ex(r_f, callee->args[0], r_s);
			if (recv.todo) {
				return recv;
			}
			ty owner = recv.t;
			std::string code;
			if (owner.k == tk::option) {
				code = unwrap(r_f, recv, r_s, "Value", owner);
			} else {
				code = value_of(r_f, recv, r_s, "Value");
			}
			if (owner.k == tk::object && !owner.script.empty()) {
				if (const VerseGdScriptClass *info = script_info(owner.script)) {
					auto m = info->methods.find(callee->text);
					if (m != info->methods.end()) {
						std::vector<expr_ptr> args(inner->args.begin() + 1, inner->args.end());
						return call_script(r_f, code, m->second, args, r_s, p_e->line, true, owner.script, callee->text);
					}
				}
			}
		}
	}
	ev sig = ex(r_f, inner, r_s);
	if (sig.todo) {
		return sig;
	}
	ev e;
	if (sig.t.k == tk::signal || sig.t.k == tk::event) {
		e.code = paren(sig, 95) + ".Await()";
		std::vector<ty> payload = payload_types(sig.t.payload);
		e.t = payload.empty() ? ty::of(tk::void_) : payload.size() == 1 ? payload[0] : ty::of(tk::tuple);
		if (e.t.k == tk::tuple) {
			e.t.payload = sig.t.payload;
		}
		return e;
	}
	if (sig.t.k == tk::signal_ref) {
		e.code = paren(sig, 95) + ".Await()";
		e.t = ty::of(tk::garray);
		return e;
	}
	r_s.todos.push_back("`await` of something that is not a signal or one of this script's functions");
	return ev::todo_marker();
}

ev converter::ex_array(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_expect) {
	ty elem = p_expect && (p_expect->k == tk::array) ? p_expect->inner() : ty::of(tk::unknown);
	std::vector<ev> items;
	for (const expr_ptr &a : p_e->args) {
		items.push_back(ex(r_f, a, r_s, elem.known() ? &elem : nullptr));
		if (items.back().todo) {
			return items.back();
		}
	}
	if (!elem.known()) {
		// Homogeneous, or ints and floats together (float): anything else is variants.
		bool all_same = !items.empty();
		bool numeric = !items.empty();
		bool any_float = false;
		for (const ev &item : items) {
			all_same = all_same && same_type(item.t, items[0].t) && item.t.known();
			numeric = numeric && item.t.numeric();
			any_float = any_float || item.t.k == tk::float_;
		}
		if (numeric && any_float) {
			elem = ty::of(tk::float_);
		} else if (all_same) {
			elem = items[0].t;
		} else {
			elem = ty::of(tk::variant);
		}
	}
	std::string code = "array{";
	for (size_t i = 0; i < items.size(); i++) {
		code += (i ? ", " : "") + coerce(r_f, items[i], elem, r_s, p_e->line);
	}
	code += "}";
	ev e;
	e.code = code;
	e.t = ty::array_of(elem);
	return e;
}

ev converter::ex_string_format(fctx &r_f, const ev &p_format, const expr_ptr &p_args, sctx &r_s, int p_line) {
	std::vector<expr_ptr> args;
	if (p_args->kind == ek::array) {
		args = p_args->args;
	} else {
		args.push_back(p_args);
	}
	std::string out;
	size_t next_arg = 0;
	const std::string &f = p_format.str_inner;
	for (size_t i = 0; i < f.size(); i++) {
		if (f[i] != '%') {
			out += f[i];
			continue;
		}
		if (i + 1 < f.size() && f[i + 1] == '%') {
			out += '%';
			i++;
			continue;
		}
		size_t j = i + 1;
		while (j < f.size() && (std::isdigit((unsigned char)f[j]) || f[j] == '.' || f[j] == '-' || f[j] == '+' || f[j] == ' ' || f[j] == '*')) {
			j++;
		}
		if (j >= f.size() || next_arg >= args.size()) {
			r_s.todos.push_back("a `%` format the converter cannot read");
			return ev::todo_marker();
		}
		const char conv = f[j];
		const std::string flags = f.substr(i + 1, j - i - 1);
		if (!flags.empty() || !(conv == 's' || conv == 'd' || conv == 'i' || conv == 'f' || conv == 'v')) {
			r_s.todos.push_back("the format `%" + flags + conv + "` has padding or precision, which Verse interpolation does not");
			return ev::todo_marker();
		}
		ev a = ex(r_f, args[next_arg++], r_s);
		if (a.todo) {
			return a;
		}
		out += interpolate(r_f, a, r_s);
		i = j;
	}
	(void)p_line;
	ev e;
	e.code = "\"" + out + "\"";
	e.t = ty::of(tk::string);
	e.str_literal = true;
	e.str_inner = out;
	return e;
}

ev converter::load_resource(fctx &r_f, const expr_ptr &p_path, sctx &r_s, int p_line) {
	if (!p_path || p_path->kind != ek::string) {
		r_s.todos.push_back("`preload`/`load` of a path that is not a literal");
		return ev::todo_marker();
	}
	const std::string &path = p_path->text;
	std::string ext = path.substr(path.find_last_of('.') + 1);
	for (char &c : ext) {
		c = char(std::tolower((unsigned char)c));
	}
	std::string cls = "Resource";
	if (ext == "tscn" || ext == "scn") {
		cls = "PackedScene";
	} else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg" || ext == "bmp" || ext == "tga") {
		cls = "Texture2D";
	} else if (ext == "wav" || ext == "ogg" || ext == "mp3") {
		cls = "AudioStream";
	} else if (ext == "gd" || ext == "verse") {
		cls = "Script";
	} else if (ext == "gdshader") {
		cls = "Shader";
	} else if (ext == "ttf" || ext == "otf" || ext == "woff" || ext == "woff2") {
		cls = "FontFile";
	}
	const api::class_row *loader = class_row_of("ResourceLoader");
	const std::string accessor = loader && loader->singleton_accessor[0] ? loader->singleton_accessor : "GetResourceLoader";
	const std::string stem = sanitize_identifier(path.substr(path.find_last_of('/') + 1, path.find_last_of('.') - path.find_last_of('/') - 1));
	const std::string loaded = hoist(r_f, r_s,
			accessor + "().Load[\"" + escape_verse(path) + "\", \"\", resource_loader_cache_mode.Reuse]", stem + "Loaded");
	ev e;
	if (cls == "Resource") {
		e.code = loaded;
	} else {
		e.code = hoist(r_f, r_s, verse_class_of(cls) + "[" + loaded + "]", stem + "Resource");
	}
	e.t = ty::object_of(cls);
	(void)p_line;
	return e;
}

ev converter::function_reference(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_payload) {
	// What a `connect` hands over: one of this script's methods, by name or through `self.`, a
	// `Callable(self, "name")`, or a lambda, which was hoisted to a method in the pre-pass.
	fn_info *target = nullptr;
	if (p_e->kind == ek::ident || (p_e->kind == ek::attribute && p_e->args[0]->kind == ek::self)) {
		target = own_fn(*r_f.cls, p_e->text);
	} else if (p_e->kind == ek::lambda) {
		for (auto &lf : r_f.cls->lambdas) {
			if (lf->verse_name == p_e->text) {
				target = lf.get();
			}
		}
	} else if (p_e->kind == ek::call && p_e->args[0]->kind == ek::ident && p_e->args[0]->text == "Callable" && p_e->args.size() == 3
			&& p_e->args[1]->kind == ek::self && p_e->args[2]->kind == ek::string) {
		target = own_fn(*r_f.cls, p_e->args[2]->text);
	}
	if (!target) {
		r_s.todos.push_back("a callable that is not one of this script's own functions (" + to_source(p_e) + ")");
		return ev::todo_marker();
	}
	if (!target->captures.empty()) {
		std::string names;
		for (const std::string &c : target->captures) {
			names += (names.empty() ? "`" : ", `") + c + "`";
		}
		r_s.todos.push_back("the lambda reads " + names + " from the function it was written in, and a hoisted method cannot capture them");
		return ev::todo_marker();
	}
	// A handler's untyped parameters take the signal's payload types.
	if (p_payload) {
		std::vector<ty> types = payload_types(p_payload->payload);
		for (size_t i = 0; i < target->param_types.size() && i < types.size(); i++) {
			if (!target->param_typed[i]) {
				target->param_types[i] = types[i];
				target->param_typed[i] = true;
			}
		}
	}
	if (target->eff == effect::suspends) {
		r_s.todos.push_back("`" + target->gd_name + "` awaits, and a signal handler cannot suspend; move the waiting into a function it spawns");
		return ev::todo_marker();
	}
	ev e;
	e.code = target->verse_name;
	e.t = ty::of(tk::function);
	return e;
}

ev converter::call_own(fctx &r_f, const std::string &p_receiver, fn_info &p_fn, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line, bool p_awaited) {
	std::string args;
	const size_t declared = p_fn.decl->params.size();
	if (p_args.size() > declared) {
		r_s.todos.push_back("`" + p_fn.gd_name + "` is called with more arguments than it declares");
		return ev::todo_marker();
	}
	for (size_t i = 0; i < p_args.size(); i++) {
		ev a = ex(r_f, p_args[i], r_s, p_fn.param_types[i].known() ? &p_fn.param_types[i] : nullptr);
		if (a.todo) {
			return a;
		}
		// A call site settles an untyped parameter's type when nothing else has.
		if (!p_fn.param_typed[i] && a.t.known() && a.t.k != tk::type_value && a.t.k != tk::function && !a.is_null) {
			p_fn.param_types[i] = a.t.k == tk::option ? a.t : (a.t.k == tk::object ? ty::option_of(a.t) : a.t);
			p_fn.param_typed[i] = true;
		}
		const std::string value = coerce(r_f, a, p_fn.param_types[i], r_s, p_line);
		args += (args.empty() ? "" : ", ") + (p_fn.param_optional[i] ? "?" + p_fn.param_names[i] + " := " + value : value);
	}
	ev e;
	const std::string name = p_fn.split_async ? p_fn.helper_name : p_fn.verse_name;
	e.code = (p_receiver.empty() ? "" : p_receiver + ".") + name + "(" + args + ")";
	e.t = p_fn.ret.known() ? p_fn.ret : ty::of(tk::unknown);
	if (p_fn.is_static) {
		e.code = name + "(" + args + ")";
	}
	if ((p_fn.eff == effect::suspends || p_fn.split_async) && !p_awaited) {
		// Called without `await`: GDScript runs it until its first wait and carries on, which is
		// what `spawn` does.
		e.code = "spawn{" + e.code + "}";
		e.t = ty::of(tk::void_);
		if (r_f.fn) {
			r_f.fn->spawns = true;
		}
	}
	return e;
}

ev converter::call_script(fctx &r_f, const std::string &p_receiver, const VerseGdMethodInfo &p_method, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line, bool p_awaited,
		const std::string &p_method_owner, const std::string &p_method_name) {
	if (p_args.size() > p_method.param_types.size()) {
		r_s.todos.push_back("`" + p_method.verse_name + "` is called with more arguments than it declares");
		return ev::todo_marker();
	}
	std::string args;
	for (size_t i = 0; i < p_args.size(); i++) {
		const ty want = ty_from_verse(p_method.param_types[i]);
		ev a = ex(r_f, p_args[i], r_s, want.known() && want.k != tk::variant ? &want : nullptr);
		if (a.todo) {
			return a;
		}
		observe(p_method_owner, p_method_name, i, a);
		const std::string value = coerce(r_f, a, want, r_s, p_line);
		const bool named = i < p_method.param_optional.size() && p_method.param_optional[i];
		args += (args.empty() ? "" : ", ") + (named ? "?" + p_method.param_names[i] + " := " + value : value);
	}
	ev e;
	e.code = p_receiver + "." + p_method.verse_name + "(" + args + ")";
	e.t = ty_from_verse(p_method.result_type);
	if (p_method.suspends && !p_awaited) {
		e.code = "spawn{" + e.code + "}";
		e.t = ty::of(tk::void_);
		if (r_f.fn) {
			r_f.fn->spawns = true;
		}
	}
	return e;
}

ev converter::call_dynamic(fctx &r_f, const std::string &p_receiver, const std::string &p_method, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line) {
	std::string args = "\"" + escape_verse(p_method) + "\"";
	std::vector<std::string> values;
	for (const expr_ptr &a : p_args) {
		ev v = ex(r_f, a, r_s);
		if (v.todo) {
			return v;
		}
		values.push_back(coerce(r_f, v, ty::of(tk::variant), r_s, p_line));
	}
	if (values.size() > 4) {
		std::string list;
		for (size_t i = 0; i < values.size(); i++) {
			list += (i ? ", " : "") + values[i];
		}
		args += ", array{" + list + "}";
	} else {
		for (const std::string &v : values) {
			args += ", " + v;
		}
	}
	ev e;
	e.code = (p_receiver.empty() ? "" : paren(ev{ p_receiver }, 95) + ".") + "Call(" + args + ")";
	e.t = ty::of(tk::variant);
	note(p_line, "`" + p_method + "` is not a method the mirror knows on this receiver, so it is called with `Call`, which answers a `variant`");
	return e;
}

ev converter::call_godot(fctx &r_f, const std::string &p_receiver, const api::member *p_row, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line) {
	std::vector<api_param> params = decode_params(p_row->params);
	const bool vararg = std::string(p_row->extra) == "vararg";
	std::string args;
	std::vector<std::string> extra;
	for (size_t i = 0; i < p_args.size(); i++) {
		if (i >= params.size()) {
			if (!vararg) {
				r_s.todos.push_back("`" + std::string(p_row->godot_name) + "` is called with more arguments than Godot declares");
				return ev::todo_marker();
			}
			ev v = ex(r_f, p_args[i], r_s);
			if (v.todo) {
				return v;
			}
			extra.push_back(coerce(r_f, v, ty::of(tk::variant), r_s, p_line));
			continue;
		}
		const ty target = ty_from_verse(params[i].type);
		ev v = ex(r_f, p_args[i], r_s, &target);
		if (v.todo) {
			return v;
		}
		const std::string value = coerce(r_f, v, target, r_s, p_line);
		args += (args.empty() ? "" : ", ") + (params[i].optional ? "?" + params[i].name + " := " + value : value);
	}
	for (size_t i = p_args.size(); i < params.size(); i++) {
		if (params[i].optional) {
			continue;
		}
		if (!params[i].fill.empty()) {
			args += (args.empty() ? "" : ", ") + params[i].fill;
			continue;
		}
		r_s.todos.push_back("`" + std::string(p_row->godot_name) + "` needs its `" + params[i].name
				+ "` argument, whose Godot default has no Verse spelling");
		return ev::todo_marker();
	}
	if (!extra.empty()) {
		if (extra.size() > 4) {
			std::string list;
			for (size_t i = 0; i < extra.size(); i++) {
				list += (i ? ", " : "") + extra[i];
			}
			args += (args.empty() ? "" : ", ") + std::string("array{") + list + "}";
		} else {
			for (const std::string &x : extra) {
				args += (args.empty() ? "" : ", ") + x;
			}
		}
	}
	ev e;
	const std::string prefix = p_receiver.empty() ? "" : p_receiver + ".";
	const bool brackets = p_row->form == api::shape::decides || p_row->form == api::shape::predicate;
	e.code = prefix + p_row->verse + (brackets ? "[" : "(") + args + (brackets ? "]" : ")");
	{
		std::string hint = p_row->verse;
		const size_t dot = hint.find_last_of('.');
		if (dot != std::string::npos) {
			hint = hint.substr(dot + 1);
		}
		if (hint.rfind("Get", 0) == 0 && hint.size() > 3) {
			hint = hint.substr(3);
		}
		e.hint = hint;
	}
	switch (p_row->form) {
		case api::shape::value:
			e.t = ty_from_verse(p_row->result);
			break;
		case api::shape::decides:
			e.f = form::fails;
			e.t = std::string(p_row->result) == "*" ? ty::of(tk::unknown) : ty_from_verse(p_row->result);
			break;
		case api::shape::predicate:
			e.f = form::test;
			e.t = ty::of(tk::logic);
			break;
		case api::shape::none:
			e.t = ty::of(tk::void_);
			break;
	}
	return e;
}

ev converter::call_builtin(fctx &r_f, const std::string &p_name, const std::vector<expr_ptr> &p_args, sctx &r_s, int p_line, const ty *p_expect, bool &r_handled) {
	r_handled = true;
	ev e;
	auto arg = [&](size_t i, const ty *expect = nullptr) { return ex(r_f, p_args[i], r_s, expect); };

	if (p_name == "print" || p_name == "print_debug" || p_name == "prints" || p_name == "printt" || p_name == "printerr"
			|| p_name == "push_error" || p_name == "push_warning" || p_name == "print_rich" || p_name == "printraw") {
		const std::string separator = p_name == "prints" ? " " : p_name == "printt" ? "\\t" : "";
		std::string text;
		bool single_string = p_args.size() == 1;
		std::string single;
		for (size_t i = 0; i < p_args.size(); i++) {
			ev a = arg(i);
			if (a.todo) {
				return a;
			}
			if (i) {
				text += separator;
			}
			text += interpolate(r_f, a, r_s);
			if (single_string) {
				single = a.t.k == tk::string ? value_of(r_f, a, r_s, "Value") : "";
			}
		}
		const std::string fn = p_name == "printerr" ? "PrintErr"
				: p_name == "push_error"            ? "PushError"
				: p_name == "push_warning"          ? "PushWarning"
				: p_name == "print_rich"            ? "PrintRich"
				: p_name == "printraw"              ? "PrintRaw"
													: "Print";
		e.code = fn + "(" + (single_string && !single.empty() ? single : "\"" + text + "\"") + ")";
		e.t = ty::of(tk::void_);
		return e;
	}
	if (p_name == "str") {
		std::string text;
		for (size_t i = 0; i < p_args.size(); i++) {
			ev a = arg(i);
			if (a.todo) {
				return a;
			}
			text += interpolate(r_f, a, r_s);
		}
		e.code = "\"" + text + "\"";
		e.t = ty::of(tk::string);
		e.str_literal = true;
		e.str_inner = text;
		return e;
	}
	if (p_name == "typeof" && p_args.size() == 1) {
		// A variant knows its own lane, and `TYPE_INT` is `variant_type.TypeInt` in the table.
		ev a = arg(0);
		if (a.todo) {
			return a;
		}
		e.code = "VariantKind(" + coerce(r_f, a, ty::of(tk::variant), r_s, p_line) + ")";
		e.t = ty::enum_of("variant_type");
		return e;
	}
	if (p_name == "len" && p_args.size() == 1) {
		ev a = arg(0);
		if (a.todo) {
			return a;
		}
		ty inner;
		const std::string v = unwrap(r_f, a, r_s, "Value", inner);
		e.code = paren(ev{ v }, 95) + (inner.k == tk::typed_array ? ".ToArray().Length" : ".Length");
		e.t = ty::of(tk::int_);
		return e;
	}
	if ((p_name == "preload" || p_name == "load") && p_args.size() == 1) {
		return load_resource(r_f, p_args[0], r_s, p_line);
	}
	if (p_name == "is_instance_valid" && p_args.size() == 1) {
		ev a = arg(0);
		if (a.todo) {
			return a;
		}
		if (a.t.k == tk::option) {
			e.code = paren(a, 90) + "?";
			e.f = form::test;
			e.t = ty::of(tk::logic);
			return e;
		}
		e.code = "IsInstanceValid[" + value_of(r_f, a, r_s, "Value") + "]";
		e.f = form::test;
		e.t = ty::of(tk::logic);
		return e;
	}
	if ((p_name == "int" || p_name == "float" || p_name == "bool" || p_name == "String") && p_args.size() == 1) {
		ev a = arg(0);
		if (a.todo) {
			return a;
		}
		if (p_name == "float") {
			ty f = ty::of(tk::float_);
			e.code = coerce(r_f, a, f, r_s, p_line);
			e.t = f;
			e.prec = 70;
			return e;
		}
		if (p_name == "String") {
			e.str_inner = interpolate(r_f, a, r_s);
			e.code = "\"" + e.str_inner + "\"";
			e.str_literal = true;
			e.t = ty::of(tk::string);
			return e;
		}
		if (p_name == "bool") {
			e.code = coerce(r_f, a, ty::of(tk::logic), r_s, p_line);
			e.t = ty::of(tk::logic);
			return e;
		}
		if (a.t.k == tk::int_) {
			return a;
		}
		if (a.t.k == tk::enum_) {
			e.code = "ToInt(" + value_of(r_f, a, r_s, "Value") + ")";
			e.t = ty::of(tk::int_);
			return e;
		}
		if (a.t.k == tk::float_) {
			// GDScript truncates toward zero; Verse's Floor floors. Both agree for the values
			// that are not negative, which is the common case, and the note says so.
			note(p_line, "`int(...)` became `Floor[...]`, which rounds a negative number down where GDScript truncates toward zero");
			e.code = "Floor[" + value_of(r_f, a, r_s, "Value") + "]";
			e.f = form::fails;
			e.t = ty::of(tk::int_);
			return e;
		}
		r_s.todos.push_back("`int(...)` of a value the converter cannot type");
		return ev::todo_marker();
	}
	if (p_name == "range") {
		r_s.todos.push_back("`range(...)` outside a `for` loop, which Verse would need as an array");
		return ev::todo_marker();
	}
	// `StringName("x")`, `PackedStringArray([...])`, `Array(xs)`: in Verse each is already the
	// type it converts to -- a string, an array -- so the conversion is the value itself.
	{
		static const std::map<std::string, std::string> conversions = {
			{ "StringName", "String" }, { "NodePath", "String" },
			{ "Array", "" }, { "PackedStringArray", "String" }, { "PackedInt32Array", "int" }, { "PackedInt64Array", "int" },
			{ "PackedFloat32Array", "float" }, { "PackedFloat64Array", "float" }, { "PackedVector2Array", "Vector2" },
			{ "PackedVector3Array", "Vector3" }, { "PackedColorArray", "Color" }, { "PackedByteArray", "int" },
		};
		auto conversion = conversions.find(p_name);
		if (conversion != conversions.end() && p_args.size() <= 1) {
			const bool is_array = p_name == "Array" || p_name.rfind("Packed", 0) == 0;
			ty target = !is_array ? ty::of(tk::string)
					: conversion->second.empty() ? (p_expect && p_expect->k == tk::array ? *p_expect : ty::array_of(ty::of(tk::variant)))
												   : ty::array_of(ty_from_gd(conversion->second, *r_f.cls, p_line));
			if (p_args.empty()) {
				e.code = is_array ? "array{}" : "\"\"";
				e.t = target;
				return e;
			}
			ev a = arg(0, &target);
			if (a.todo) {
				return a;
			}
			if (a.t.k == tk::typed_array && is_array) {
				e.code = paren(a, 95) + ".ToArray()";
				e.t = ty::array_of(a.t.inner());
				return e;
			}
			if ((is_array && (a.t.k == tk::array || a.t.k == tk::unknown)) || (!is_array && a.t.k == tk::string)) {
				return a;
			}
			r_s.todos.push_back("`" + p_name + "(...)` of a `" + verse_text(a.t) + "`");
			return ev::todo_marker();
		}
	}
	if (p_name == "Callable") {
		r_s.todos.push_back("`Callable(...)` outside a `connect`");
		return ev::todo_marker();
	}
	if (p_name == "get_node" || p_name == "get_node_or_null") {
		if (p_args.size() == 1 && p_args[0]->kind == ek::string) {
			return ex_node_path(r_f, p_args[0]->text, "", r_s, p_line);
		}
	}
	if (p_name == "emit_signal" && !p_args.empty() && p_args[0]->kind == ek::string) {
		auto s = r_f.cls->sigs.find(p_args[0]->text);
		if (s != r_f.cls->sigs.end()) {
			std::vector<std::string> values;
			for (size_t i = 1; i < p_args.size(); i++) {
				const ty &want = i - 1 < s->second.params.size() ? s->second.params[i - 1] : ty::of(tk::variant);
				ev a = arg(i, &want);
				if (a.todo) {
					return a;
				}
				values.push_back(coerce(r_f, a, want, r_s, p_line));
			}
			std::string payload = values.empty() ? "()" : values.size() == 1 ? values[0] : "(";
			if (values.size() > 1) {
				for (size_t i = 0; i < values.size(); i++) {
					payload += (i ? ", " : "") + values[i];
				}
				payload += ")";
			}
			e.code = s->second.verse_name + ".Emit(" + payload + ")";
			e.t = ty::of(tk::void_);
			return e;
		}
	}
	// A math constructor: `Vector2(x, y)`, `Color(r, g, b)`.
	if (is_math_type(p_name)) {
		const api::class_row *row = class_row_of(p_name);
		std::vector<api_param> fields = decode_params(row->fields);
		if (p_args.empty()) {
			e.code = std::string(row->verse_name) + "{}";
			e.t = ty::math_of(p_name);
			return e;
		}
		if (p_name == "Color" && p_args.size() == 3) {
			fields.resize(3);
		}
		if (p_args.size() != fields.size()) {
			r_s.todos.push_back("`" + p_name + "(...)` with " + std::to_string(p_args.size()) + " arguments, which is not its field-by-field constructor");
			return ev::todo_marker();
		}
		std::string code = std::string(row->verse_name) + "{";
		for (size_t i = 0; i < fields.size(); i++) {
			const ty ft = ty_from_verse(fields[i].type);
			ev a = arg(i, &ft);
			if (a.todo) {
				return a;
			}
			code += (i ? ", " : "") + fields[i].name + " := " + coerce(r_f, a, ft, r_s, p_line);
		}
		e.code = code + "}";
		e.t = ty::math_of(p_name);
		return e;
	}
	// @GlobalScope: a dispatched utility, or the Verse function Godot's is spelled as.
	if (const api::member *row = find_global(p_name, { api::kind::utility })) {
		std::vector<api_param> params = decode_params(row->params);
		std::vector<ev> values;
		bool any_float = false;
		for (size_t i = 0; i < p_args.size(); i++) {
			ty want = i < params.size() ? ty_from_verse(params[i].type) : ty::of(tk::unknown);
			ev a = arg(i, want.known() ? &want : p_expect);
			if (a.todo) {
				return a;
			}
			any_float = any_float || a.t.k == tk::float_;
			values.push_back(a);
		}
		std::string result = row->result;
		ty generic = ty::of(tk::unknown);
		if (result == "*") {
			// abs, clamp, min, max, lerp: the arguments' own type, float if any is.
			generic = any_float ? ty::of(tk::float_) : values.empty() ? ty::of(tk::unknown) : values[0].t;
			if (std::string(p_name).find("lerp") != std::string::npos || p_name == "lerpf") {
				generic = values.empty() ? ty::of(tk::float_) : (values[0].t.k == tk::int_ ? ty::of(tk::float_) : values[0].t);
			}
			if (generic.k == tk::option) {
				generic = generic.inner();
			}
		}
		std::string args;
		for (size_t i = 0; i < values.size(); i++) {
			ty want = i < params.size() ? ty_from_verse(params[i].type) : generic;
			if (generic.k == tk::float_ && values[i].t.k == tk::int_) {
				want = generic;
			}
			args += (i ? ", " : "") + coerce(r_f, values[i], want, r_s, p_line);
		}
		const bool brackets = row->form == api::shape::decides || row->form == api::shape::predicate;
		e.code = std::string(row->verse) + (brackets ? "[" : "(") + args + (brackets ? "]" : ")");
		e.t = result == "*" ? generic : ty_from_verse(result);
		e.f = row->form == api::shape::decides ? form::fails : row->form == api::shape::predicate ? form::test : form::value;
		if (row->form == api::shape::none) {
			e.t = ty::of(tk::void_);
		}
		return e;
	}
	r_handled = false;
	return e;
}

ev converter::ex_call(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_expect) {
	const expr_ptr &callee = p_e->args[0];
	const std::vector<expr_ptr> args(p_e->args.begin() + 1, p_e->args.end());
	class_ctx &cls = *r_f.cls;
	const int line = p_e->line;

	if (callee->kind == ek::ident) {
		const std::string &name = callee->text;
		if (!find_local(r_f, name)) {
			if (fn_info *f = own_fn(cls, name)) {
				return call_own(r_f, "", *f, args, r_s, line, false);
			}
			bool handled = false;
			ev b = call_builtin(r_f, name, args, r_s, line, p_expect, handled);
			if (handled) {
				return b;
			}
			if (const api::member *row = find_member(cls.base_godot, name, { api::kind::method })) {
				return call_godot(r_f, "", row, args, r_s, line);
			}
			if (fn_info *f = own_fn(cls, name)) {
				return call_own(r_f, "", *f, args, r_s, line, false);
			}
		}
		ev fe = ex(r_f, callee, r_s);
		if (fe.todo) {
			return fe;
		}
		r_s.todos.push_back("`" + name + "(...)` calls something that is not a function the converter knows");
		return ev::todo_marker();
	}

	if (callee->kind == ek::attribute) {
		const std::string &method = callee->text;
		const expr_ptr &base = callee->args[0];

		if (base->kind == ek::super_) {
			// `super.method(...)`: the parent's version, which Verse spells `(super:)`.
			std::string verse_name = pascal(method);
			ev e;
			if (const api::member *row = find_member(cls.base_godot, method, { api::kind::virtual_method, api::kind::method })) {
				if (row->what == api::kind::virtual_method && cls.base_script.empty()) {
					// A Godot virtual's inherited body is empty, so calling it does nothing.
					e.code = "";
					e.t = ty::of(tk::void_);
					return e;
				}
				verse_name = row->verse;
			}
			std::string a;
			for (const expr_ptr &x : args) {
				ev v = ex(r_f, x, r_s);
				if (v.todo) {
					return v;
				}
				a += (a.empty() ? "" : ", ") + value_of(r_f, v, r_s, "Value");
			}
			e.code = "(super:)" + verse_name + "(" + a + ")";
			e.t = ty::of(tk::unknown);
			return e;
		}
		if (base->kind == ek::self) {
			if (fn_info *f = own_fn(cls, method)) {
				return call_own(r_f, "", *f, args, r_s, line, false);
			}
			if (const api::member *row = find_member(cls.base_godot, method, { api::kind::method })) {
				return call_godot(r_f, "", row, args, r_s, line);
			}
			bool handled = false;
			ev b = call_builtin(r_f, method, args, r_s, line, p_expect, handled);
			if (handled) {
				return b;
			}
			return call_dynamic(r_f, "", method, args, r_s, line);
		}

		ev recv = ex(r_f, base, r_s);
		if (recv.todo) {
			return recv;
		}
		// `.new()` on a class: an archetype, which on a mirrored class mints the Godot object too.
		if (recv.t.k == tk::type_value && method == "new") {
			if (!args.empty()) {
				r_s.todos.push_back("`new(...)` with arguments: a Verse archetype takes none, and `_init` is not converted");
				return ev::todo_marker();
			}
			ev e;
			if (recv.t.tv == "class") {
				e.code = verse_class_of(recv.t.name) + "{}";
				e.t = ty::object_of(recv.t.name);
				return e;
			}
			if (recv.t.tv == "script") {
				e.code = recv.t.script + "{}";
				e.t = ty::object_of(recv.t.name, recv.t.script);
				return e;
			}
		}
		if (recv.t.k == tk::type_value && (recv.t.tv == "class" || recv.t.tv == "math")) {
			if (const api::member *row = find_member(recv.t.name, method, { api::kind::static_method })) {
				ev e = call_godot(r_f, "", row, args, r_s, line);
				return e;
			}
		}
		if (recv.t.k == tk::type_value && recv.t.tv == "singleton") {
			if (const api::member *row = find_member(recv.t.name, method, { api::kind::method })) {
				return call_godot(r_f, recv.code, row, args, r_s, line);
			}
		}
		if (recv.t.k == tk::type_value && recv.t.tv == "script" && recv.t.script == cls.verse_name) {
			if (fn_info *f = own_fn(cls, method)) {
				return call_own(r_f, "", *f, args, r_s, line, false);
			}
		}

		// Signals: this script's own events, and Godot's.
		if (recv.t.k == tk::event || recv.t.k == tk::signal) {
			ev e;
			if (method == "connect" && !args.empty()) {
				ev handler = function_reference(r_f, args[0], r_s, &recv.t);
				if (handler.todo) {
					return handler;
				}
				if (args.size() > 1) {
					r_s.todos.push_back("`connect` with flags");
					return ev::todo_marker();
				}
				e.code = paren(recv, 95) + ".Subscribe(" + handler.code + ")";
				e.t = ty::of(tk::void_);
				return e;
			}
			if (method == "emit" && recv.t.k == tk::event) {
				std::vector<ty> types = payload_types(recv.t.payload);
				std::vector<std::string> values;
				for (size_t i = 0; i < args.size(); i++) {
					const ty want = i < types.size() ? types[i] : ty::of(tk::variant);
					ev a = ex(r_f, args[i], r_s, &want);
					if (a.todo) {
						return a;
					}
					values.push_back(coerce(r_f, a, want, r_s, line));
				}
				std::string payload = values.empty() ? "()" : values.size() == 1 ? values[0] : "(";
				if (values.size() > 1) {
					for (size_t i = 0; i < values.size(); i++) {
						payload += (i ? ", " : "") + values[i];
					}
					payload += ")";
				}
				e.code = paren(recv, 95) + ".Emit(" + payload + ")";
				e.t = ty::of(tk::void_);
				return e;
			}
			r_s.todos.push_back("`." + method + "(...)` on a signal: only `connect` and `emit` are converted");
			return ev::todo_marker();
		}

		ty owner = recv.t;
		std::string code = recv.code;
		if (owner.k == tk::option) {
			ty inner;
			code = unwrap(r_f, recv, r_s, sanitize_identifier(recv.code.substr(0, recv.code.find_first_of("[(."))) + "Value", inner);
			owner = inner;
		} else if (recv.f == form::fails) {
			code = value_of(r_f, recv, r_s, "Value");
		} else if (recv.f == form::test) {
			code = value_of(r_f, recv, r_s, "Value");
		}

		switch (owner.k) {
			case tk::object: {
				if (owner.script == cls.verse_name) {
					if (fn_info *f = own_fn(cls, method)) {
						return call_own(r_f, code, *f, args, r_s, line, false);
					}
				}
				if (method == "get_node" || method == "get_node_or_null") {
					if (args.size() == 1 && args[0]->kind == ek::string) {
						return ex_node_path(r_f, args[0]->text, code, r_s, line);
					}
				}
				if (const api::member *row = find_member(owner.name.empty() ? "Object" : owner.name, method, { api::kind::method })) {
					return call_godot(r_f, code, row, args, r_s, line);
				}
				if (!owner.script.empty()) {
					if (const VerseGdScriptClass *info = script_info(owner.script)) {
						auto m = info->methods.find(method);
						if (m != info->methods.end()) {
							return call_script(r_f, code, m->second, args, r_s, line, false, owner.script, method);
						}
					}
					std::string a;
					for (size_t i = 0; i < args.size(); i++) {
						ev v = ex(r_f, args[i], r_s);
						if (v.todo) {
							return v;
						}
						observe(owner.script, method, i, v);
						a += (a.empty() ? "" : ", ") + value_of(r_f, v, r_s, "Value");
					}
					ev e;
					e.code = code + "." + pascal(method) + "(" + a + ")";
					e.t = ty::of(tk::unknown);
					note(line, "`" + method + "` on `" + owner.script + "` is assumed to be spelled `" + pascal(method)
									+ "`, as a converted script or its generated binding spells it");
					return e;
				}
				return call_dynamic(r_f, code, method, args, r_s, line);
			}
			case tk::math: {
				if (const api::member *row = find_member(owner.name, method, { api::kind::method })) {
					return call_godot(r_f, paren(ev{ code, owner, form::value, code.find(' ') != std::string::npos ? 0 : 100 }, 95), row, args, r_s, line);
				}
				r_s.todos.push_back("`" + owner.name + "." + method + "` has no Verse counterpart in GodotMath yet");
				return ev::todo_marker();
			}
			case tk::dict: {
				// Godot's own Dictionary, through the mirror's typed accessors. A lookup that finds
				// nothing *fails*, which is what `has` asks and what `get`'s default answers.
				ev e;
				if ((method == "size") && args.empty()) {
					e.code = paren(ev{ code }, 95) + ".Length()";
					e.t = ty::of(tk::int_);
					return e;
				}
				if (method == "is_empty" && args.empty()) {
					e.code = paren(ev{ code }, 95) + ".Length() = 0";
					e.f = form::test;
					e.t = ty::of(tk::logic);
					e.prec = 40;
					return e;
				}
				if ((method == "has" || method == "get") && !args.empty() && args.size() <= 2) {
					ev k = ex(r_f, args[0], r_s);
					if (k.todo) {
						return k;
					}
					if (!dictionary_key(k.t)) {
						r_s.todos.push_back("a Dictionary key of type `" + verse_text(k.t) + "`: the mirror reads one keyed by a string, an int or a vector2i");
						return ev::todo_marker();
					}
					const ty want = p_expect ? *p_expect : ty::of(tk::variant);
					const std::string lane = method == "has" ? "Variant" : dictionary_lane(want);
					e.code = paren(ev{ code }, 95) + ".Get" + lane + "[" + value_of(r_f, k, r_s, "Key") + "]";
					e.hint = "Entry";
					if (method == "has") {
						e.f = form::test;
						e.t = ty::of(tk::logic);
						return e;
					}
					e.t = lane == "Variant" ? ty::of(tk::variant) : want;
					e.f = form::fails;
					if (args.size() == 2) {
						ev fallback = ex(r_f, args[1], r_s, &e.t);
						if (fallback.todo) {
							return fallback;
						}
						const std::string bound = fresh(r_f, "Entry");
						e.code = "if (" + bound + " := " + e.code + ") then " + bound + " else " + coerce(r_f, fallback, e.t, r_s, line);
						e.f = form::value;
						e.prec = 5;
					}
					return e;
				}
				r_s.todos.push_back("`Dictionary." + method + "` has no counterpart among the mirror's Dictionary accessors");
				return ev::todo_marker();
			}
			case tk::string: {
				ev e;
				if ((method == "length" || method == "size") && args.empty()) {
					e.code = paren(ev{ code }, 95) + ".Length";
					e.t = ty::of(tk::int_);
					return e;
				}
				if (method == "is_empty" && args.empty()) {
					e.code = paren(ev{ code }, 95) + ".Length = 0";
					e.f = form::test;
					e.t = ty::of(tk::logic);
					e.prec = 40;
					return e;
				}
				r_s.todos.push_back("`String." + method + "` has no Verse counterpart the converter knows");
				return ev::todo_marker();
			}
			case tk::array:
			case tk::typed_array: {
				ev e;
				const std::string arr = owner.k == tk::typed_array ? paren(ev{ code }, 95) + ".ToArray()" : code;
				if (method == "size" && args.empty()) {
					e.code = paren(ev{ arr }, 95) + ".Length";
					e.t = ty::of(tk::int_);
					return e;
				}
				if (method == "is_empty" && args.empty()) {
					e.code = paren(ev{ arr }, 95) + ".Length = 0";
					e.f = form::test;
					e.t = ty::of(tk::logic);
					e.prec = 40;
					return e;
				}
				if ((method == "has" || method == "find") && args.size() == 1 && owner.k == tk::array) {
					ev a = ex(r_f, args[0], r_s, &owner.inner());
					if (a.todo) {
						return a;
					}
					e.code = paren(ev{ arr }, 95) + ".Find[" + coerce(r_f, a, owner.inner(), r_s, line) + "]";
					e.f = method == "has" ? form::test : form::fails;
					e.t = method == "has" ? ty::of(tk::logic) : ty::of(tk::int_);
					return e;
				}
				if (method == "pick_random" && args.empty()) {
					const std::string list = arr;
					e.code = paren(ev{ list }, 95) + "[GodotStatics.RandiRange(0, " + paren(ev{ list }, 95) + ".Length - 1)]";
					e.f = form::fails;
					e.t = owner.inner();
					return e;
				}
				if ((method == "front" || method == "back") && args.empty()) {
					e.code = paren(ev{ arr }, 95) + "[" + (method == "front" ? std::string("0") : paren(ev{ arr }, 95) + ".Length - 1") + "]";
					e.f = form::fails;
					e.t = owner.inner();
					return e;
				}
				r_s.todos.push_back("`Array." + method + "` changes or reads the array in a way the converter does not translate");
				return ev::todo_marker();
			}
			default:
				break;
		}
		if (!owner.known() || owner.k == tk::variant) {
			r_s.todos.push_back("`." + method + "(...)` is called on a value whose type is not known here; give it a type in the GDScript");
			return ev::todo_marker();
		}
		r_s.todos.push_back("`." + method + "(...)` on a `" + verse_text(owner) + "`, which the converter does not translate");
		return ev::todo_marker();
	}

	if (callee->kind == ek::super_) {
		ev e;
		if (r_f.fn && r_f.fn->is_virtual && r_f.cls->base_script.empty()) {
			e.code = "";
			e.t = ty::of(tk::void_);
			return e;
		}
		std::string a;
		for (const expr_ptr &x : args) {
			ev v = ex(r_f, x, r_s);
			if (v.todo) {
				return v;
			}
			a += (a.empty() ? "" : ", ") + value_of(r_f, v, r_s, "Value");
		}
		e.code = "(super:)" + (r_f.fn ? r_f.fn->verse_name : std::string("_Init")) + "(" + a + ")";
		e.t = ty::of(tk::unknown);
		return e;
	}
	r_s.todos.push_back("a call through an expression (" + to_source(callee) + ")");
	return ev::todo_marker();
}

ev converter::ex_binary(fctx &r_f, const expr_ptr &p_e, sctx &r_s) {
	const std::string &op = p_e->text;
	ev e;
	e.t = ty::of(tk::logic);

	if (op == "is" || op == "is not") {
		ev a = ex(r_f, p_e->args[0], r_s);
		if (a.todo) {
			return a;
		}
		ty target = ty_from_gd(p_e->type_text, *r_f.cls, p_e->line);
		if (target.k != tk::object) {
			r_s.todos.push_back("`is " + p_e->type_text + "` tests for something that is not a class");
			return ev::todo_marker();
		}
		ty inner;
		const std::string v = unwrap(r_f, a, r_s, "Value", inner);
		e.code = verse_text(target) + "[" + v + "]";
		if (op == "is not") {
			e.code = "not " + e.code;
			e.prec = 30;
		}
		e.f = form::test;
		return e;
	}

	if (op == "and" || op == "or") {
		ev a = ex(r_f, p_e->args[0], r_s);
		ev b = ex(r_f, p_e->args[1], r_s);
		if (a.todo) {
			return a;
		}
		if (b.todo) {
			return b;
		}
		const int prec = op == "and" ? 20 : 10;
		e.code = paren(ev{ test_of(a), a.t, form::test, a.f == form::value ? 80 : a.prec }, prec + 1) + " " + op + " "
				+ paren(ev{ test_of(b), b.t, form::test, b.f == form::value ? 80 : b.prec }, prec + 1);
		e.f = form::test;
		e.prec = prec;
		return e;
	}

	if (op == "in" || op == "not in") {
		ev needle = ex(r_f, p_e->args[0], r_s);
		ev hay = ex(r_f, p_e->args[1], r_s);
		if (needle.todo) {
			return needle;
		}
		if (hay.todo) {
			return hay;
		}
		ty inner;
		const std::string list = unwrap(r_f, hay, r_s, "Values", inner);
		if (inner.k == tk::array) {
			e.code = paren(ev{ list }, 95) + ".Find[" + coerce(r_f, needle, inner.inner(), r_s, p_e->line) + "]";
		} else if (inner.k == tk::map) {
			e.code = paren(ev{ list }, 95) + "[" + coerce(r_f, needle, *inner.key, r_s, p_e->line) + "]";
		} else {
			r_s.todos.push_back("`in` on a `" + verse_text(inner) + "`");
			return ev::todo_marker();
		}
		if (op == "not in") {
			e.code = "not " + e.code;
			e.prec = 30;
		}
		e.f = form::test;
		return e;
	}

	ev a = ex(r_f, p_e->args[0], r_s);
	if (a.todo) {
		return a;
	}
	const ty *hint = a.t.known() && a.t.k != tk::option ? &a.t : nullptr;
	ev b = ex(r_f, p_e->args[1], r_s, hint);
	if (b.todo) {
		return b;
	}
	if (!hint && b.t.known() && p_e->args[0]->kind == ek::number) {
		a = ex(r_f, p_e->args[0], r_s, &b.t);
	}

	if (op == "==" || op == "!=") {
		// Against null: whether an option holds anything.
		if (a.is_null || b.is_null) {
			const ev &v = a.is_null ? b : a;
			if (v.t.k == tk::option || v.f == form::fails) {
				const std::string code = v.f == form::fails ? v.code : paren(v, 90) + "?";
				e.code = op == "==" ? "not " + code : code;
				e.prec = op == "==" ? 30 : 90;
				e.f = form::test;
				return e;
			}
			if (v.t.k == tk::object) {
				e.code = op == "==" ? "false?" : "true?";
				e.f = form::test;
				return e;
			}
			r_s.todos.push_back("a comparison with `null` of a value that cannot be null in Verse");
			return ev::todo_marker();
		}
	}

	// Numbers meet as floats when either is one.
	ty result = a.t;
	std::string lhs;
	std::string rhs;
	if (a.t.numeric() && b.t.numeric() && a.t.k != b.t.k) {
		const ty f = ty::of(tk::float_);
		lhs = coerce(r_f, a, f, r_s, p_e->line);
		rhs = coerce(r_f, b, f, r_s, p_e->line);
		result = f;
		a.prec = 70;
		b.prec = 70;
	} else if (a.t.k == tk::math && b.t.k == tk::int_) {
		lhs = value_of(r_f, a, r_s, "Value");
		rhs = coerce(r_f, b, ty::of(tk::float_), r_s, p_e->line);
	} else if (b.t.k == tk::math && a.t.k == tk::int_) {
		lhs = coerce(r_f, a, ty::of(tk::float_), r_s, p_e->line);
		rhs = value_of(r_f, b, r_s, "Value");
		result = b.t;
	} else {
		ty ia;
		ty ib;
		lhs = a.t.k == tk::option ? unwrap(r_f, a, r_s, "Value", ia) : value_of(r_f, a, r_s, "Value");
		rhs = b.t.k == tk::option ? unwrap(r_f, b, r_s, "Value", ib) : value_of(r_f, b, r_s, "Value");
		if (a.t.k == tk::option) {
			result = ia;
		}
		if (b.t.k == tk::math && a.t.k != tk::math) {
			result = b.t;
		}
	}
	if (a.f == form::test) {
		a.prec = 100;
	}
	if (b.f == form::test) {
		b.prec = 100;
	}
	auto prec_of = [](const std::string &code, const ev &original) {
		if (code == original.code) {
			return original.prec;
		}
		return code.size() > 6 && code.compare(code.size() - 6, 6, " * 1.0") == 0 ? 70 : 100;
	};
	const ev la{ lhs, a.t, form::value, prec_of(lhs, a) };
	const ev rb{ rhs, b.t, form::value, prec_of(rhs, b) };

	static const std::map<std::string, std::string> comparisons = {
		{ "==", "=" }, { "!=", "<>" }, { "<", "<" }, { ">", ">" }, { "<=", "<=" }, { ">=", ">=" },
	};
	auto cmp = comparisons.find(op);
	if (cmp != comparisons.end()) {
		e.code = paren(la, 51) + " " + cmp->second + " " + paren(rb, 51);
		e.f = form::test;
		e.prec = 40;
		return e;
	}

	if (op == "+" && (result.k == tk::string || a.t.k == tk::string)) {
		if (a.str_literal && b.str_literal) {
			e.code = "\"" + a.str_inner + b.str_inner + "\"";
			e.str_literal = true;
			e.str_inner = a.str_inner + b.str_inner;
			e.t = ty::of(tk::string);
			return e;
		}
		e.code = paren(la, 60) + " + " + paren(rb, 61);
		e.t = ty::of(tk::string);
		e.prec = 60;
		return e;
	}
	if (op == "+" || op == "-") {
		e.code = paren(la, 60) + " " + op + " " + paren(rb, 61);
		e.t = result;
		e.prec = 60;
		return e;
	}
	if (op == "*") {
		e.code = paren(la, 70) + " * " + paren(rb, 71);
		e.t = result;
		e.prec = 70;
		return e;
	}
	if (op == "/") {
		if (a.t.k == tk::int_ && b.t.k == tk::int_) {
			// GDScript truncates toward zero; Verse's Quotient floors, so GodotMath's
			// TruncatedQuotient is the one that agrees. It fails on a zero divisor.
			e.code = "TruncatedQuotient[" + lhs + ", " + rhs + "]";
			e.hint = "Quotient";
			e.f = form::fails;
			e.t = ty::of(tk::int_);
			return e;
		}
		e.code = paren(la, 70) + " / " + paren(rb, 71);
		e.t = result;
		e.prec = 70;
		return e;
	}
	if (op == "%") {
		if (a.str_literal) {
			return ex_string_format(r_f, a, p_e->args[1], r_s, p_e->line);
		}
		if (a.t.k == tk::int_ && b.t.k == tk::int_) {
			note(p_e->line, "`%` became `Mod[...]`, which is never negative where GDScript's `%` takes the sign of the left side");
			e.code = "Mod[" + lhs + ", " + rhs + "]";
			e.hint = "Remainder";
			e.f = form::fails;
			e.t = ty::of(tk::int_);
			return e;
		}
		e.code = "FMod(" + lhs + ", " + rhs + ")";
		e.t = ty::of(tk::float_);
		return e;
	}
	if (op == "**") {
		e.code = "Pow(" + coerce(r_f, a, ty::of(tk::float_), r_s, p_e->line) + ", " + coerce(r_f, b, ty::of(tk::float_), r_s, p_e->line) + ")";
		e.t = ty::of(tk::float_);
		return e;
	}
	static const std::map<std::string, std::string> bitwise = {
		{ "&", "BitAnd" }, { "|", "BitOr" }, { "^", "BitXor" }, { "<<", "BitLshift" }, { ">>", "BitRshift" },
	};
	auto bit = bitwise.find(op);
	if (bit != bitwise.end()) {
		auto as_int = [&](const ev &v, const std::string &code) {
			return v.t.k == tk::enum_ ? "ToInt(" + code + ")" : code;
		};
		e.code = bit->second + "(" + as_int(a, lhs) + ", " + as_int(b, rhs) + ")";
		e.t = ty::of(tk::int_);
		return e;
	}
	r_s.todos.push_back("the operator `" + op + "`");
	return ev::todo_marker();
}

ev converter::ex(fctx &r_f, const expr_ptr &p_e, sctx &r_s, const ty *p_expect) {
	ev e;
	if (!p_e) {
		return ev::todo_marker();
	}
	auto element_hint = [](const std::string &p_list) {
		bool simple = !p_list.empty() && std::isupper((unsigned char)p_list[0]);
		for (char c : p_list) {
			simple = simple && std::isalnum((unsigned char)c);
		}
		if (!simple) {
			return std::string("Item");
		}
		// `Scores[Key]` binds as `Score`, `Names[I]` as `Name`.
		return p_list.size() > 1 && p_list.back() == 's' ? p_list.substr(0, p_list.size() - 1) : p_list + "Item";
	};
	switch (p_e->kind) {
		case ek::number:
			if (p_e->is_float || (p_expect && p_expect->k == tk::float_)) {
				e.code = format_float(p_e->is_float ? p_e->text : format_int(p_e->text));
				e.t = ty::of(tk::float_);
			} else {
				e.code = format_int(p_e->text);
				e.t = ty::of(tk::int_);
				e.int_literal = true;
			}
			return e;
		case ek::string:
			e.str_inner = escape_verse(p_e->text);
			e.code = "\"" + e.str_inner + "\"";
			e.t = ty::of(tk::string);
			e.str_literal = true;
			return e;
		case ek::boolean:
			e.code = p_e->text;
			e.t = ty::of(tk::logic);
			return e;
		case ek::null:
			e.code = "false";
			e.is_null = true;
			e.t = p_expect ? *p_expect : ty::option_of(ty::of(tk::variant));
			return e;
		case ek::self:
			e.code = "Self";
			e.t = self_type(*r_f.cls);
			e.is_self = true;
			return e;
		case ek::ident:
			return ex_ident(r_f, p_e, r_s);
		case ek::node_path:
			return ex_node_path(r_f, p_e->text, "", r_s, p_e->line);
		case ek::array:
			return ex_array(r_f, p_e, r_s, p_expect);
		case ek::dictionary: {
			if (p_e->args.empty() && p_expect && p_expect->k == tk::map) {
				e.code = "map{}";
				e.t = *p_expect;
				return e;
			}
			ty key = p_expect && p_expect->k == tk::map ? *p_expect->key : ty::of(tk::unknown);
			ty val = p_expect && p_expect->k == tk::map ? p_expect->inner() : ty::of(tk::unknown);
			std::vector<std::pair<ev, ev>> items;
			for (size_t i = 0; i + 1 < p_e->args.size(); i += 2) {
				ev k = ex(r_f, p_e->args[i], r_s, key.known() ? &key : nullptr);
				ev v = ex(r_f, p_e->args[i + 1], r_s, val.known() ? &val : nullptr);
				if (k.todo) {
					return k;
				}
				if (v.todo) {
					return v;
				}
				items.push_back({ k, v });
			}
			if (!key.known() && !items.empty()) {
				key = items[0].first.t;
				val = items[0].second.t;
				for (const auto &kv : items) {
					if (!same_type(kv.first.t, key)) {
						r_s.todos.push_back("a dictionary whose keys are of different types");
						return ev::todo_marker();
					}
					if (!same_type(kv.second.t, val)) {
						val = kv.second.t.numeric() && val.numeric() ? ty::of(tk::float_) : ty::of(tk::variant);
					}
				}
			}
			if (!key.known()) {
				// `{}` with nothing to say what it will hold is what GDScript made it: Godot's own
				// Dictionary, which the mirror reads and writes through typed accessors.
				e.code = "dictionary{}";
				e.t = ty::of(tk::dict);
				return e;
			}
			std::string code = "map{";
			for (size_t i = 0; i < items.size(); i++) {
				code += (i ? ", " : "") + coerce(r_f, items[i].first, key, r_s, p_e->line) + " => " + coerce(r_f, items[i].second, val, r_s, p_e->line);
			}
			e.code = code + "}";
			e.t.k = tk::map;
			e.t.key = std::make_shared<ty>(key);
			e.t.elem = std::make_shared<ty>(val);
			return e;
		}
		case ek::unary: {
			ev a = ex(r_f, p_e->args[0], r_s, p_e->text == "not" ? nullptr : p_expect);
			if (a.todo) {
				return a;
			}
			if (p_e->text == "not") {
				e.code = "not " + paren(ev{ test_of(a), a.t, form::test, a.f == form::value ? 90 : a.prec }, 30);
				e.f = form::test;
				e.t = ty::of(tk::logic);
				e.prec = 30;
				return e;
			}
			if (p_e->text == "-") {
				const std::string v = value_of(r_f, a, r_s, "Value");
				e.code = "-" + paren(ev{ v, a.t, form::value, a.prec }, 90);
				e.t = a.t;
				e.prec = 90;
				e.int_literal = a.int_literal;
				return e;
			}
			if (p_e->text == "+") {
				return a;
			}
			if (p_e->text == "~") {
				e.code = "BitNot(" + value_of(r_f, a, r_s, "Value") + ")";
				e.t = ty::of(tk::int_);
				return e;
			}
			break;
		}
		case ek::binary:
			return ex_binary(r_f, p_e, r_s);
		case ek::ternary: {
			// `a if c else b`, with anything the condition hoists kept inside the `if`.
			sctx inner;
			inner.hoisted = r_s.hoisted;
			ev c = ex(r_f, p_e->args[1], inner);
			ev a = ex(r_f, p_e->args[0], r_s, p_expect);
			ev b = ex(r_f, p_e->args[2], r_s, p_expect && p_expect->known() ? p_expect : &a.t);
			r_s.todos.insert(r_s.todos.end(), inner.todos.begin(), inner.todos.end());
			if (c.todo || a.todo || b.todo) {
				return ev::todo_marker();
			}
			ty t = a.t;
			if (a.t.numeric() && b.t.numeric() && a.t.k != b.t.k) {
				t = ty::of(tk::float_);
			}
			if (a.is_null) {
				t = b.t.k == tk::option ? b.t : ty::option_of(b.t);
			} else if (b.is_null) {
				t = a.t.k == tk::option ? a.t : ty::option_of(a.t);
			}
			std::string clauses;
			for (const std::string &cl : inner.clauses) {
				clauses += cl + ", ";
			}
			e.code = "if (" + clauses + test_of(c) + ") then " + coerce(r_f, a, t, r_s, p_e->line) + " else " + coerce(r_f, b, t, r_s, p_e->line);
			e.t = t;
			e.prec = 5;
			return e;
		}
		case ek::call:
			return ex_call(r_f, p_e, r_s, p_expect);
		case ek::attribute:
			return ex_attribute(r_f, p_e, r_s);
		case ek::subscript: {
			ev a = ex(r_f, p_e->args[0], r_s);
			if (a.todo) {
				return a;
			}
			ty inner;
			const std::string list = unwrap(r_f, a, r_s, "Values", inner);
			if (inner.k == tk::array || inner.k == tk::typed_array || inner.k == tk::string) {
				ty want = ty::of(tk::int_);
				ev i = ex(r_f, p_e->args[1], r_s, &want);
				if (i.todo) {
					return i;
				}
				std::string index = coerce(r_f, i, want, r_s, p_e->line);
				if (i.code.size() > 1 && i.code[0] == '-' && i.int_literal) {
					index = paren(ev{ list }, 95) + ".Length " + i.code.replace(0, 1, "- ");
				}
				e.code = paren(ev{ inner.k == tk::typed_array ? list + ".ToArray()" : list }, 95) + "[" + index + "]";
				e.hint = element_hint(list);
				e.f = form::fails;
				e.t = inner.k == tk::string ? ty::of(tk::string) : inner.inner();
				if (inner.k == tk::string) {
					r_s.todos.push_back("indexing a string answers a `char` in Verse, not a one-character string");
					return ev::todo_marker();
				}
				return e;
			}
			if (inner.k == tk::dict) {
				ev k = ex(r_f, p_e->args[1], r_s);
				if (k.todo) {
					return k;
				}
				if (!dictionary_key(k.t)) {
					r_s.todos.push_back("a Dictionary key of type `" + verse_text(k.t) + "`: the mirror reads one keyed by a string, an int or a vector2i");
					return ev::todo_marker();
				}
				const ty want = p_expect ? *p_expect : ty::of(tk::variant);
				const std::string lane = dictionary_lane(want);
				e.code = paren(ev{ list }, 95) + ".Get" + lane + "[" + value_of(r_f, k, r_s, "Key") + "]";
				e.hint = "Entry";
				e.f = form::fails;
				e.t = lane == "Variant" ? ty::of(tk::variant) : want;
				return e;
			}
			if (inner.k == tk::map) {
				ev k = ex(r_f, p_e->args[1], r_s, inner.key.get());
				if (k.todo) {
					return k;
				}
				e.code = list + "[" + coerce(r_f, k, *inner.key, r_s, p_e->line) + "]";
				e.hint = element_hint(list);
				e.f = form::fails;
				e.t = inner.inner();
				return e;
			}
			r_s.todos.push_back("indexing a `" + verse_text(inner) + "`");
			return ev::todo_marker();
		}
		case ek::cast: {
			ev a = ex(r_f, p_e->args[0], r_s);
			if (a.todo) {
				return a;
			}
			ty target = ty_from_gd(p_e->type_text, *r_f.cls, p_e->line);
			if (target.k == tk::object) {
				ty inner;
				const std::string v = a.t.k == tk::option ? unwrap(r_f, a, r_s, "Value", inner) : value_of(r_f, a, r_s, "Value");
				if (a.t.k != tk::option) {
					inner = a.t;
				}
				if (inner.k == tk::object && target.script.empty() && inner.script.empty() && godot_inherits(inner.name, target.name)) {
					// `$UI/Label as Label` when the scene already said it is a Label.
					ev same;
					same.code = v;
					same.t = inner;
					return same;
				}
				// `$Sprite as AnimatedSprite2D` looked up the node already; cast what it found.
				e.code = verse_text(target) + "[" + v + "]";
				e.f = form::fails;
				e.t = target;
				return e;
			}
			if (target.numeric() || target.k == tk::string || target.k == tk::logic) {
				ev c;
				c.code = coerce(r_f, a, target, r_s, p_e->line);
				c.t = target;
				return c;
			}
			r_s.todos.push_back("`as " + p_e->type_text + "`");
			return ev::todo_marker();
		}
		case ek::await_:
			return ex_await(r_f, p_e, r_s);
		case ek::lambda: {
			ev f = function_reference(r_f, p_e, r_s, nullptr);
			return f;
		}
		case ek::super_:
		case ek::opaque:
		case ek::get_node_call:
			break;
	}
	r_s.todos.push_back("`" + to_source(p_e) + "`");
	return ev::todo_marker();
}

// ================================================================================================
// Statements

std::string join_clauses(const std::vector<std::string> &p_clauses) {
	std::string out;
	for (size_t i = 0; i < p_clauses.size(); i++) {
		out += (i ? ", " : "") + p_clauses[i];
	}
	return out;
}

// Writes `p_code` as a statement, inside an `if` over the clauses its expressions hoisted.
void converter::emit_guarded(line_out &r_out, int p_indent, const sctx &p_s, const std::string &p_code, const std::string &p_trailing) {
	if (p_s.clauses.empty()) {
		r_out.add(p_indent, p_code + p_trailing);
		return;
	}
	last_guard = join_clauses(p_s.clauses);
	r_out.add(p_indent, "if (" + last_guard + "):");
	r_out.add(p_indent + 1, p_code + p_trailing);
}

bool converter::emit_assign(fctx &r_f, const expr_ptr &p_target, const ev &p_value, sctx &r_s, std::string &r_code, int p_line) {
	class_ctx &cls = *r_f.cls;
	const expr_ptr &t = p_target;
	if (t->kind == ek::ident || (t->kind == ek::attribute && t->args[0]->kind == ek::self)) {
		const std::string &name = t->text;
		if (t->kind == ek::ident) {
			if (const local *l = find_local(r_f, name)) {
				r_code = "set " + l->verse + " = " + coerce(r_f, p_value, l->t, r_s, p_line);
				return true;
			}
		}
		auto v = cls.vars.find(name);
		if (v != cls.vars.end()) {
			const bool inside_accessor = r_f.fn && (r_f.fn->gd_name == "<set:" + name + ">" || r_f.fn->gd_name == "<get:" + name + ">");
			if (!v->second.setter_fn.empty() && !inside_accessor) {
				r_code = v->second.setter_fn + "(" + coerce(r_f, p_value, v->second.t, r_s, p_line) + ")";
				return true;
			}
			r_code = "set " + v->second.verse_name + " = " + coerce(r_f, p_value, v->second.t, r_s, p_line);
			return true;
		}
		if (const api::member *row = find_member(cls.base_godot, name, { api::kind::property, api::kind::accessor_property })) {
			if (row->what == api::kind::property) {
				r_code = "set " + std::string(row->verse) + " = " + coerce(r_f, p_value, ty_from_verse(row->result), r_s, p_line);
				return true;
			}
			if (row->extra[0]) {
				const api::member *setter = find_member(cls.base_godot, row->extra, { api::kind::method });
				if (setter) {
					std::vector<api_param> params = decode_params(setter->params);
					const ty want = params.empty() ? ty::of(tk::unknown) : ty_from_verse(params[0].type);
					r_code = std::string(setter->verse) + "(" + coerce(r_f, p_value, want, r_s, p_line) + ")";
					return true;
				}
			}
		}
		r_s.todos.push_back("an assignment to `" + name + "`, which the converter cannot find");
		return false;
	}
	if (t->kind == ek::attribute) {
		ev base = ex(r_f, t->args[0], r_s);
		if (base.todo) {
			return false;
		}
		// A field of a math value: rebuild the whole value and assign that, because a Verse struct's
		// fields cannot be written one at a time.
		if (base.t.k == tk::math) {
			const api::class_row *row = class_row_of(base.t.name);
			std::vector<api_param> fields = decode_params(row->fields);
			const api::member *field = find_member(base.t.name, t->text, { api::kind::field });
			if (!field) {
				r_s.todos.push_back("`." + t->text + "` is not a field of `" + base.t.name + "`");
				return false;
			}
			const std::string current = value_of(r_f, base, r_s, "Value");
			std::string rebuilt = std::string(row->verse_name) + "{";
			for (size_t i = 0; i < fields.size(); i++) {
				rebuilt += (i ? ", " : "") + fields[i].name + " := ";
				if (fields[i].name == field->verse) {
					rebuilt += coerce(r_f, p_value, ty_from_verse(fields[i].type), r_s, p_line);
				} else {
					rebuilt += paren(ev{ current }, 95) + "." + fields[i].name;
				}
			}
			rebuilt += "}";
			ev whole;
			whole.code = rebuilt;
			whole.t = base.t;
			return emit_assign(r_f, t->args[0], whole, r_s, r_code, p_line);
		}
		ty owner = base.t;
		std::string recv = base.code;
		if (owner.k == tk::option) {
			ty inner;
			recv = unwrap(r_f, base, r_s, sanitize_identifier(base.code.substr(0, base.code.find_first_of("[(."))) + "Value", inner);
			owner = inner;
		} else if (base.f == form::fails) {
			recv = value_of(r_f, base, r_s, "Value");
		}
		if (owner.k == tk::object) {
			if (owner.script == cls.verse_name) {
				auto v = cls.vars.find(t->text);
				if (v != cls.vars.end()) {
					r_code = "set " + recv + "." + v->second.verse_name + " = " + coerce(r_f, p_value, v->second.t, r_s, p_line);
					return true;
				}
			}
			if (const api::member *row = find_member(owner.name.empty() ? "Object" : owner.name, t->text, { api::kind::property, api::kind::accessor_property })) {
				if (row->what == api::kind::property) {
					r_code = "set " + recv + "." + row->verse + " = " + coerce(r_f, p_value, ty_from_verse(row->result), r_s, p_line);
					return true;
				}
				if (row->extra[0]) {
					if (const api::member *setter = find_member(owner.name, row->extra, { api::kind::method })) {
						std::vector<api_param> params = decode_params(setter->params);
						const ty want = params.empty() ? ty::of(tk::unknown) : ty_from_verse(params[0].type);
						r_code = recv + "." + setter->verse + "(" + coerce(r_f, p_value, want, r_s, p_line) + ")";
						return true;
					}
				}
			}
			if (!owner.script.empty()) {
				r_code = "set " + recv + "." + pascal(t->text) + " = " + value_of(r_f, p_value, r_s, "Value");
				note(p_line, "`" + t->text + "` on `" + owner.script + "` is assumed to be spelled `" + pascal(t->text) + "`");
				return true;
			}
			// Godot's own dynamic write, which is what GDScript did.
			r_code = recv + ".Set(\"" + escape_verse(t->text) + "\", " + coerce(r_f, p_value, ty::of(tk::variant), r_s, p_line) + ")";
			note(p_line, "`" + t->text + "` is not a member the mirror knows, so it is written with `Set`");
			return true;
		}
		r_s.todos.push_back("an assignment to `." + t->text + "` of a value whose type is not known here");
		return false;
	}
	if (t->kind == ek::subscript) {
		ev base = ex(r_f, t->args[0], r_s);
		if (base.todo) {
			return false;
		}
		if (base.t.k == tk::dict) {
			ev k = ex(r_f, t->args[1], r_s);
			if (k.todo) {
				return false;
			}
			if (!dictionary_key(k.t)) {
				r_s.todos.push_back("a Dictionary key of type `" + verse_text(k.t) + "`");
				return false;
			}
			const std::string lane = dictionary_lane(p_value.f == form::test ? ty::of(tk::logic) : p_value.t);
			const ty lane_type = lane == "Variant" ? ty::of(tk::variant) : (p_value.f == form::test ? ty::of(tk::logic) : p_value.t);
			r_code = value_of(r_f, base, r_s, "Values") + ".Set" + lane + "(" + value_of(r_f, k, r_s, "Key") + ", " + coerce(r_f, p_value, lane_type, r_s, p_line) + ")";
			return true;
		}
		if (base.t.k == tk::array || base.t.k == tk::map) {
			const ty key = base.t.k == tk::array ? ty::of(tk::int_) : *base.t.key;
			ev k = ex(r_f, t->args[1], r_s, &key);
			if (k.todo) {
				return false;
			}
			// Indexed writes can fail -- past the end of an array -- so the write is the test.
			r_code = "if (set " + base.code + "[" + coerce(r_f, k, key, r_s, p_line) + "] = " + coerce(r_f, p_value, base.t.inner(), r_s, p_line) + ") {}";
			return true;
		}
		r_s.todos.push_back("an indexed write into a `" + verse_text(base.t) + "`");
		return false;
	}
	r_s.todos.push_back("an assignment to `" + to_source(t) + "`");
	return false;
}

void converter::emit_block(fctx &r_f, const std::vector<stmt_ptr> &p_body, int p_indent, line_out &r_out) {
	r_f.scopes.emplace_back();
	const size_t before = r_out.lines.size();
	int last_line = 0;
	std::string previous_guard;
	size_t previous_end = 0;
	for (const stmt_ptr &s : p_body) {
		if (last_line && s->line > last_line + 1) {
			// One blank line where the author left at least one, and only when no comment sits there.
			bool comment_between = false;
			for (const comment &c : parsed.comments) {
				comment_between = comment_between || (c.line > last_line && c.line < s->line);
			}
			if (!comment_between) {
				r_out.blank();
			}
		}
		flush_comments(r_out, p_indent, s->line);
		const size_t start = r_out.lines.size();
		emit_stmt(r_f, s, p_indent, r_out);
		// Consecutive statements guarded by the same lookups share one `if`: four lines about
		// `$AnimatedSprite2D` read as one block, the way the yardstick's hand port does.
		if (!last_guard.empty() && last_guard == previous_guard && start == previous_end && r_out.lines.size() > start) {
			r_out.lines.erase(r_out.lines.begin() + long(start));
		}
		previous_guard = last_guard;
		previous_end = r_out.lines.size();
		last_line = s->end_line;
	}
	bool any_code = false;
	for (size_t i = before; i < r_out.lines.size(); i++) {
		const std::string line = trim(r_out.lines[i]);
		any_code = any_code || (!line.empty() && line[0] != '#');
	}
	if (!any_code) {
		// A body with nothing left in it -- `pass`, a `super._ready()` that does nothing, or only
		// TODOs -- still has to be a body.
		r_out.add(p_indent, "{}");
	}
	r_f.scopes.pop_back();
}

// Narrowing: inside `if x:`, `if x != null:` and `if x is Foo:`, a use of `x` means the bound value,
// which is how the yardstick reads (`if (Animation := Sprite?): Animation.Play()`).
struct narrowing {
	std::string gd_name;
	local binding;
};

void converter::emit_if(fctx &r_f, const stmt_ptr &p_s, size_t p_branch, int p_indent, line_out &r_out, bool p_else_if) {
	const if_branch &b = p_s->branches[p_branch];
	sctx s;
	std::vector<narrowing> narrowed;

	// `a and b and c` is a clause list, which is what lets each clause bind.
	std::vector<expr_ptr> parts;
	std::function<void(const expr_ptr &)> split = [&](const expr_ptr &e) {
		if (e->kind == ek::binary && e->text == "and") {
			split(e->args[0]);
			split(e->args[1]);
		} else {
			parts.push_back(e);
		}
	};
	split(b.condition);

	r_f.scopes.emplace_back();
	for (const expr_ptr &part : parts) {
		// The shapes that narrow a plain name.
		expr_ptr subject;
		std::string cast_type;
		if (part->kind == ek::ident) {
			subject = part;
		} else if (part->kind == ek::binary && part->text == "!=" && part->args[1]->kind == ek::null && part->args[0]->kind == ek::ident) {
			subject = part->args[0];
		} else if (part->kind == ek::call && part->args[0]->kind == ek::ident && part->args[0]->text == "is_instance_valid" && part->args.size() == 2
				&& part->args[1]->kind == ek::ident) {
			subject = part->args[1];
		} else if (part->kind == ek::binary && part->text == "is" && part->args[0]->kind == ek::ident) {
			subject = part->args[0];
			cast_type = part->type_text;
		}
		if (subject) {
			ev v = ex(r_f, subject, s);
			if (!v.todo && (v.t.k == tk::option || !cast_type.empty())) {
				ty inner = v.t.k == tk::option ? v.t.inner() : v.t;
				std::string code = v.t.k == tk::option ? paren(v, 90) + "?" : v.code;
				if (!cast_type.empty()) {
					ty target = ty_from_gd(cast_type, *r_f.cls, b.line);
					if (target.k != tk::object) {
						s.todos.push_back("`is " + cast_type + "`");
						break;
					}
					if (v.t.k == tk::option) {
						code = hoist(r_f, s, code, pascal(subject->text) + "Value");
					}
					code = verse_text(target) + "[" + code + "]";
					inner = target;
				}
				const std::string name = fresh(r_f, pascal(subject->text) + (cast_type.empty() ? "Value" : sanitize_identifier(verse_binding_member_name(verse_text(inner)))));
				s.clauses.push_back(name + " := " + code);
				narrowed.push_back({ subject->text, { name, inner, false } });
				r_f.scopes.back()[subject->text] = { name, inner, false };
				continue;
			}
		}
		ev c = ex(r_f, part, s);
		if (c.todo) {
			break;
		}
		s.clauses.push_back(test_of(c));
	}

	const std::string keyword = p_else_if ? "else if" : "if";
	if (!s.todos.empty()) {
		r_f.scopes.pop_back();
		emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
		return;
	}
	r_out.add(p_indent, keyword + " (" + join_clauses(s.clauses) + "):" + trailing_comment(b.line, b.line));
	emit_block(r_f, b.body, p_indent + 1, r_out);
	r_f.scopes.pop_back();

	if (p_branch + 1 < p_s->branches.size()) {
		const if_branch &next = p_s->branches[p_branch + 1];
		if (next.condition) {
			emit_if(r_f, p_s, p_branch + 1, p_indent, r_out, true);
		} else {
			flush_comments(r_out, p_indent, next.line);
			r_out.add(p_indent, "else:" + trailing_comment(next.line, next.line));
			emit_block(r_f, next.body, p_indent + 1, r_out);
		}
	}
}

void converter::emit_for(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out) {
	// Verse's `for` has no `break` and no `continue`; either one is a TODO for the whole loop.
	std::function<bool(const std::vector<stmt_ptr> &)> escapes = [&](const std::vector<stmt_ptr> &body) {
		for (const stmt_ptr &s : body) {
			if (s->kind == sk::break_ || s->kind == sk::continue_) {
				return true;
			}
			if (s->kind == sk::while_ || s->kind == sk::for_) {
				continue;
			}
			for (const if_branch &b : s->branches) {
				if (escapes(b.body)) {
					return true;
				}
			}
			for (const match_branch &b : s->cases) {
				if (escapes(b.body)) {
					return true;
				}
			}
		}
		return false;
	};
	if (escapes(p_s->body)) {
		emit_todo(r_out, p_indent, p_s->line, p_s->end_line, { "Verse's `for` has no `break` or `continue`; rewrite the loop with `loop:` and `break`" });
		return;
	}
	sctx s;
	const expr_ptr &it = p_s->args[0];
	const std::string var = fresh(r_f, pascal(p_s->for_var));
	ty var_type = ty::of(tk::int_);
	std::string header;

	auto range_header = [&](const expr_ptr &from, const expr_ptr &to) -> bool {
		const ty int_t = ty::of(tk::int_);
		std::string lo = "0";
		if (from) {
			ev f = ex(r_f, from, s, &int_t);
			if (f.todo) {
				return false;
			}
			lo = coerce(r_f, f, int_t, s, p_s->line);
		}
		ev t = ex(r_f, to, s, &int_t);
		if (t.todo) {
			return false;
		}
		if (t.t.k != tk::int_ && t.t.known()) {
			s.todos.push_back("a `range` over something that is not an int");
			return false;
		}
		const std::string hi = t.int_literal ? std::to_string(std::atoll(t.code.c_str()) - 1) : paren(t, 61) + " - 1";
		header = "for (" + var + " := " + lo + ".." + hi + "):";
		return true;
	};

	bool ok = true;
	if (it->kind == ek::call && it->args[0]->kind == ek::ident && it->args[0]->text == "range") {
		if (it->args.size() == 2) {
			ok = range_header(nullptr, it->args[1]);
		} else if (it->args.size() == 3) {
			ok = range_header(it->args[1], it->args[2]);
		} else {
			s.todos.push_back("`range` with a step; Verse's `a..b` counts by one");
			ok = false;
		}
	} else {
		ev c = ex(r_f, it, s);
		if (c.todo) {
			ok = false;
		} else if (c.t.k == tk::int_) {
			header = "for (" + var + " := 0.." + (c.int_literal ? std::to_string(std::atoll(c.code.c_str()) - 1) : paren(c, 61) + " - 1") + "):";
		} else {
			ty inner;
			const std::string list = unwrap(r_f, c, s, "Values", inner);
			if (inner.k == tk::array) {
				header = "for (" + var + " : " + list + "):";
				var_type = inner.inner();
			} else if (inner.k == tk::typed_array) {
				header = "for (" + var + " : " + paren(ev{ list }, 95) + ".ToArray()):";
				var_type = inner.inner();
			} else if (inner.k == tk::map) {
				header = "for (" + var + " -> " + fresh(r_f, var + "Value") + " : " + list + "):";
				var_type = *inner.key;
			} else {
				s.todos.push_back("a `for` over a `" + verse_text(inner) + "`");
				ok = false;
			}
		}
	}
	if (!ok || !s.todos.empty()) {
		emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
		return;
	}
	int indent = p_indent;
	if (!s.clauses.empty()) {
		r_out.add(p_indent, "if (" + join_clauses(s.clauses) + "):");
		indent++;
	}
	r_out.add(indent, header + trailing_comment(p_s->line, p_s->line));
	r_f.scopes.emplace_back();
	r_f.scopes.back()[p_s->for_var] = { var, var_type, false };
	r_f.loop_depth++;
	emit_block(r_f, p_s->body, indent + 1, r_out);
	r_f.loop_depth--;
	r_f.scopes.pop_back();
}

void converter::emit_match(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out) {
	// An `if` chain over `=`: exactly what a literal match is, and it keeps Verse's `case` for
	// the shapes that fit it without having to prove which do.
	sctx s;
	ev subject = ex(r_f, p_s->args[0], s);
	if (subject.todo) {
		emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
		return;
	}
	std::string value = value_of(r_f, subject, s, "Subject");
	int indent = p_indent;
	if (!s.clauses.empty()) {
		r_out.add(indent, "if (" + join_clauses(s.clauses) + "):");
		indent++;
	}
	if (p_s->args[0]->kind != ek::ident && p_s->args[0]->kind != ek::attribute) {
		const std::string name = fresh(r_f, "Subject");
		r_out.add(indent, name + " := " + value);
		value = name;
	}
	ty subject_type = subject.t.k == tk::option ? subject.t.inner() : subject.t;
	bool first = true;
	for (const match_branch &b : p_s->cases) {
		sctx cs;
		std::vector<std::string> alternatives;
		bool is_default = false;
		for (const expr_ptr &p : b.patterns) {
			if (p->kind == ek::ident && p->text == "_") {
				is_default = true;
				continue;
			}
			if (p->kind == ek::opaque || p->kind == ek::array || p->kind == ek::dictionary) {
				cs.todos.push_back("a binding, array or dictionary pattern");
				continue;
			}
			ev pv = ex(r_f, p, cs, subject_type.known() ? &subject_type : nullptr);
			if (pv.todo) {
				continue;
			}
			alternatives.push_back(paren(ev{ value }, 51) + " = " + paren(ev{ coerce(r_f, pv, subject_type, cs, b.line), pv.t, form::value, pv.prec }, 51));
		}
		std::string guard;
		if (b.guard) {
			ev g = ex(r_f, b.guard, cs);
			if (!g.todo) {
				guard = test_of(g);
			}
		}
		if (!cs.todos.empty()) {
			emit_todo(r_out, indent, b.line, b.body.empty() ? b.line : b.body.back()->end_line, cs.todos);
			continue;
		}
		std::string test;
		if (alternatives.size() == 1) {
			test = alternatives[0];
		} else {
			for (size_t i = 0; i < alternatives.size(); i++) {
				test += (i ? " or " : "") + std::string("(") + alternatives[i] + ")";
			}
		}
		std::vector<std::string> clauses = cs.clauses;
		if (!test.empty() && !is_default) {
			clauses.push_back(test);
		}
		if (!guard.empty()) {
			clauses.push_back(guard);
		}
		bool empty = true;
		for (const stmt_ptr &st : b.body) {
			empty = empty && st->kind == sk::pass;
		}
		if (clauses.empty() && empty && !first) {
			break;
		}
		flush_comments(r_out, indent, b.line);
		if (clauses.empty()) {
			r_out.add(indent, first ? "if (true?):" : "else:");
		} else {
			r_out.add(indent, std::string(first ? "if" : "else if") + " (" + join_clauses(clauses) + "):");
		}
		emit_block(r_f, b.body, indent + 1, r_out);
		first = false;
		if (clauses.empty()) {
			break;
		}
	}
}

// A binding an `if` makes is gone after it, so the next statement may use the name again -- which
// is what makes two statements about `$Sprite` bind it identically, and so merge (emit_block).
void converter::emit_stmt(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out) {
	const std::set<std::string> saved = r_f.taken;
	last_guard.clear();
	emit_stmt_inner(r_f, p_s, p_indent, r_out);
	std::set<std::string> keep = saved;
	for (const auto &kv : r_f.scopes.back()) {
		keep.insert(kv.second.verse);
	}
	r_f.taken = keep;
}

void converter::emit_stmt_inner(fctx &r_f, const stmt_ptr &p_s, int p_indent, line_out &r_out) {
	sctx s;
	switch (p_s->kind) {
		case sk::pass:
			return;
		case sk::breakpoint:
			note(p_s->line, "`breakpoint` was dropped; set one in the Verse file instead");
			return;
		case sk::break_:
			if (r_f.loop_depth == 0) {
				emit_todo(r_out, p_indent, p_s->line, p_s->line, { "`break` outside a loop the converter wrote" });
				return;
			}
			r_out.add(p_indent, "break" + trailing_comment(p_s->line, p_s->line));
			return;
		case sk::continue_:
			emit_todo(r_out, p_indent, p_s->line, p_s->line, { "Verse has no `continue`; wrap the rest of the loop body in an `if` instead" });
			return;
		case sk::return_: {
			if (p_s->args.empty()) {
				r_out.add(p_indent, "return" + trailing_comment(p_s->line, p_s->end_line));
				return;
			}
			fn_info &fn = *r_f.fn;
			const ty *want = fn.ret.known() && fn.ret.k != tk::void_ ? &fn.ret : nullptr;
			ev v = ex(r_f, p_s->args[0], s, want);
			if (v.todo || !s.todos.empty()) {
				emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
				// `Err` has every type, so the function still says what it answers; reaching it is a
				// runtime error naming the line, rather than a Verse file that does not compile.
				if (want) {
					r_out.add(p_indent, "return Err(\"TODO(convert): line " + std::to_string(p_s->line) + " of the GDScript was not converted\")");
				}
				return;
			}
			if (!fn.ret_declared && !fn.ret_inferred && v.t.known() && v.t.k != tk::void_ && !v.is_null) {
				fn.ret = v.f == form::fails ? (v.t.k == tk::object ? ty::option_of(v.t) : v.t) : v.t;
				if (v.f == form::test) {
					fn.ret = ty::of(tk::logic);
				}
				fn.ret_inferred = true;
			}
			const ty &target = fn.ret.known() ? fn.ret : v.t;
			sctx inner;
			inner.hoisted = {};
			std::string value;
			if (target.k == tk::option && v.f == form::fails) {
				value = "option{" + v.code + "}";
			} else {
				value = coerce(r_f, v, target, s, p_s->line);
			}
			if (!s.clauses.empty()) {
				const std::string fallback = default_value(target);
				if (fallback.empty()) {
					emit_todo(r_out, p_indent, p_s->line, p_s->end_line, { "a return value that can be absent, of a type with no default" });
					return;
				}
				value = fold(s, value, fallback);
			}
			r_out.add(p_indent, "return " + value + trailing_comment(p_s->line, p_s->end_line));
			return;
		}
		case sk::var: {
			const std::string &name = p_s->text;
			const bool mutated = r_f.mutated.count(name) > 0;
			ty declared = ty_from_gd(p_s->type_text, *r_f.cls, p_s->line);
			if (declared.k == tk::object) {
				declared = ty::option_of(declared);
			}
			// A local that shares a member's name is `MobLocal`, so a binding of it can be
			// `MobLocalValue` rather than a second `Value`.
			const std::string verse = fresh(r_f, pascal(name), "Local");
			ev v;
			bool has_init = !p_s->args.empty();
			if (has_init) {
				v = ex(r_f, p_s->args[0], s, declared.known() ? &declared : nullptr);
				if (v.todo || !s.todos.empty()) {
					emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
					r_f.scopes.back()[name] = { verse, declared.known() ? declared : ty::of(tk::variant), mutated };
					return;
				}
			}
			ty t = declared;
			if (!t.known()) {
				if (!has_init) {
					t = ty::option_of(ty::of(tk::variant));
				} else if (v.is_null) {
					t = ty::option_of(ty::of(tk::variant));
				} else if (v.f == form::fails && v.t.k == tk::object) {
					t = ty::option_of(v.t);
				} else if (v.f == form::test) {
					t = ty::of(tk::logic);
				} else {
					t = v.t;
				}
				if (t.k == tk::void_ || t.k == tk::type_value || t.k == tk::function) {
					emit_todo(r_out, p_indent, p_s->line, p_s->end_line, { "a local initialised with something that is not a value" });
					return;
				}
			}
			std::string value;
			if (!has_init) {
				value = default_value(t);
				if (value.empty()) {
					emit_todo(r_out, p_indent, p_s->line, p_s->end_line, { "a local of a type with no default value" });
					return;
				}
			} else if (t.k == tk::option && v.f == form::fails) {
				value = "option{" + v.code + "}";
			} else {
				value = coerce(r_f, v, t, s, p_s->line);
			}
			if (!s.clauses.empty()) {
				std::string fallback = default_value(t);
				if (fallback.empty() && t.k == tk::object) {
					// A value that may be absent in a slot that cannot be: make the slot an option.
					t = ty::option_of(t);
					value = "option{" + value + "}";
					fallback = "false";
				}
				if (fallback.empty()) {
					emit_todo(r_out, p_indent, p_s->line, p_s->end_line, { "a value that can be absent, of a type with no default" });
					return;
				}
				value = fold(s, value, fallback);
			}
			const std::string tail = trailing_comment(p_s->line, p_s->end_line);
			if (mutated) {
				r_out.add(p_indent, "var " + verse + ":" + verse_text(t) + " = " + value + tail);
			} else if (declared.known() || (t.k == tk::option && !has_init)) {
				r_out.add(p_indent, verse + ":" + verse_text(t) + " = " + value + tail);
			} else {
				r_out.add(p_indent, verse + " := " + value + tail);
			}
			r_f.scopes.back()[name] = { verse, t, mutated };
			return;
		}
		case sk::assign: {
			const std::string &op = p_s->text;
			ev value;
			const expr_ptr &target = p_s->args[0];
			if (op == "=") {
				// The target's type is the value's hint, so `x = 5` into a float writes `5.0`.
				sctx probe;
				probe.hoisted = {};
				fctx copy = r_f;
				quiet++;
				ev t = ex(copy, target, probe);
				quiet--;
				value = ex(r_f, p_s->args[1], s, t.t.known() && !t.todo ? &t.t : nullptr);
			} else {
				// `x += 1` is `x = x + 1`, spelled through the same arithmetic.
				auto combined = std::make_shared<expr>();
				combined->kind = ek::binary;
				combined->text = op.substr(0, op.size() - 1);
				combined->args = { target, p_s->args[1] };
				combined->line = p_s->line;
				value = ex(r_f, combined, s);
			}
			if (value.todo || !s.todos.empty()) {
				emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
				return;
			}
			std::string code;
			if (!emit_assign(r_f, target, value, s, code, p_s->line) || !s.todos.empty()) {
				emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
				return;
			}
			// `set X = X + 1` reads better as `set X += 1`, which Verse has for numbers.
			if ((op == "+=" || op == "-=") && code.rfind("set ", 0) == 0) {
				const size_t eq = code.find(" = ");
				const std::string lhs = code.substr(4, eq - 4);
				const std::string prefix = lhs + " " + op.substr(0, 1) + " ";
				const std::string rhs = code.substr(eq + 3);
				if (rhs.rfind(prefix, 0) == 0 && rhs.find(" + ", prefix.size()) == std::string::npos && rhs.find(" - ", prefix.size()) == std::string::npos) {
					code = "set " + lhs + " " + op + " " + rhs.substr(prefix.size());
				}
			}
			emit_guarded(r_out, p_indent, s, code, trailing_comment(p_s->line, p_s->end_line));
			return;
		}
		case sk::expr: {
			const expr_ptr &e = p_s->args[0];
			// `array.append(x)`: Verse arrays are values, so the append is an assignment.
			if (e->kind == ek::call && e->args[0]->kind == ek::attribute
					&& (e->args[0]->text == "append" || e->args[0]->text == "push_back" || e->args[0]->text == "clear" || e->args[0]->text == "append_array")) {
				const expr_ptr &list = e->args[0]->args[0];
				fctx copy = r_f;
				sctx probe;
				quiet++;
				ev l = ex(copy, list, probe);
				quiet--;
				if (!l.todo && l.t.k == tk::array) {
					ev whole;
					whole.t = l.t;
					if (e->args[0]->text == "clear") {
						whole.code = "array{}";
					} else {
						if (e->args.size() != 2) {
							emit_todo(r_out, p_indent, p_s->line, p_s->end_line, { "`append` with other than one argument" });
							return;
						}
						const ty want = e->args[0]->text == "append_array" ? l.t : l.t.inner();
						ev item = ex(r_f, e->args[1], s, &want);
						if (item.todo) {
							emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
							return;
						}
						const std::string current = value_of(r_f, ex(r_f, list, s), s, "Values");
						const std::string added = coerce(r_f, item, want, s, p_s->line);
						whole.code = current + " + " + (e->args[0]->text == "append_array" ? added : "array{" + added + "}");
					}
					std::string code;
					if (emit_assign(r_f, list, whole, s, code, p_s->line) && s.todos.empty()) {
						const size_t eq = code.find(" = ");
						if (code.rfind("set ", 0) == 0 && eq != std::string::npos && code.compare(eq + 3, code.size() - eq - 3 - 0, code, 4, eq - 4) != 0) {
							const std::string lhs = code.substr(4, eq - 4);
							if (code.compare(eq + 3, lhs.size() + 3, lhs + " + ") == 0) {
								code = "set " + lhs + " += " + code.substr(eq + 3 + lhs.size() + 3);
							}
						}
						emit_guarded(r_out, p_indent, s, code, trailing_comment(p_s->line, p_s->end_line));
						return;
					}
					emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
					return;
				}
			}
			ev v = ex(r_f, e, s);
			if (v.todo || !s.todos.empty()) {
				emit_todo(r_out, p_indent, p_s->line, p_s->end_line, s.todos);
				return;
			}
			if (v.code.empty()) {
				return;
			}
			std::string code = v.code;
			if (v.f == form::fails || v.f == form::test) {
				// A failable call made for its effect: the call is the test, and the body is empty.
				s.clauses.push_back(v.code);
				r_out.add(p_indent, "if (" + join_clauses(s.clauses) + ") {}" + trailing_comment(p_s->line, p_s->end_line));
				return;
			}
			emit_guarded(r_out, p_indent, s, code, trailing_comment(p_s->line, p_s->end_line));
			return;
		}
		case sk::if_:
			emit_if(r_f, p_s, 0, p_indent, r_out, false);
			return;
		case sk::while_: {
			// `while c:` is a `loop` whose body runs only while the clauses hold, and breaks
			// otherwise -- the one spelling that keeps the clauses' bindings in scope for the body.
			sctx c;
			ev cond = ex(r_f, p_s->args[0], c);
			if (cond.todo || !c.todos.empty()) {
				emit_todo(r_out, p_indent, p_s->line, p_s->end_line, c.todos);
				return;
			}
			c.clauses.push_back(test_of(cond));
			r_out.add(p_indent, "loop:" + trailing_comment(p_s->line, p_s->line));
			r_out.add(p_indent + 1, "if (" + join_clauses(c.clauses) + "):");
			r_f.loop_depth++;
			emit_block(r_f, p_s->body, p_indent + 2, r_out);
			r_f.loop_depth--;
			r_out.add(p_indent + 1, "else:");
			r_out.add(p_indent + 2, "break");
			return;
		}
		case sk::for_:
			emit_for(r_f, p_s, p_indent, r_out);
			return;
		case sk::match:
			emit_match(r_f, p_s, p_indent, r_out);
			return;
	}
}

// ================================================================================================
// Functions and classes

void collect_mutated(const std::vector<stmt_ptr> &p_body, std::set<std::string> &r_out) {
	for (const stmt_ptr &s : p_body) {
		if (s->kind == sk::assign) {
			// The root name of the target: `x`, `x.y`, `x[i]` all write `x`'s value when `x` is a
			// struct or an array, and assigning a field of an object writes nothing of `x`'s -- a
			// harmless over-approximation that only costs a `var`.
			expr_ptr t = s->args[0];
			while (t && (t->kind == ek::attribute || t->kind == ek::subscript)) {
				t = t->args[0];
			}
			if (t && t->kind == ek::ident) {
				r_out.insert(t->text);
			}
		}
		if (s->kind == sk::expr && s->args[0]->kind == ek::call && s->args[0]->args[0]->kind == ek::attribute) {
			const std::string &m = s->args[0]->args[0]->text;
			if (m == "append" || m == "push_back" || m == "clear" || m == "append_array") {
				expr_ptr t = s->args[0]->args[0]->args[0];
				if (t->kind == ek::ident) {
					r_out.insert(t->text);
				}
			}
		}
		for (const if_branch &b : s->branches) {
			collect_mutated(b.body, r_out);
		}
		for (const match_branch &b : s->cases) {
			collect_mutated(b.body, r_out);
		}
		collect_mutated(s->body, r_out);
	}
}

std::string effect_text(effect p_e) {
	switch (p_e) {
		case effect::transacts:
			return "<transacts>";
		case effect::suspends:
			return "<suspends>";
		case effect::none:
			return "";
	}
	return "";
}

void converter::emit_function(class_ctx &r_cls, fn_info &r_fn, line_out &r_out) {
	fctx f;
	f.cls = &r_cls;
	f.fn = &r_fn;
	f.scopes.emplace_back();
	collect_mutated(r_fn.decl->body, f.mutated);

	// Parameters are immutable in Verse; one the body assigns is copied into a `var` first.
	std::vector<std::string> decls;
	std::vector<std::string> copies;
	for (size_t i = 0; i < r_fn.param_names.size(); i++) {
		const bool is_capture = i >= r_fn.decl->params.size();
		const std::string gd = is_capture ? r_fn.captures[i - r_fn.decl->params.size()] : r_fn.decl->params[i].name;
		std::string name = r_fn.param_names[i];
		f.taken.insert(name);
		ty t = r_fn.param_types[i];
		if (!t.known()) {
			t = ty::of(tk::variant);
			note(r_fn.decl->line, "`" + gd + "` has no type and nothing in the file says what it is, so it is a `variant`");
		}
		std::string decl = name + ":" + verse_text(t);
		if (r_fn.param_optional[i] && !is_capture) {
			sctx s;
			ev d = ex(f, r_fn.decl->params[i].default_value, s, &t);
			const std::string dv = d.todo ? default_value(t) : coerce(f, d, t, s, r_fn.decl->line);
			decl = "?" + decl + " = " + (dv.empty() ? "false" : dv);
		}
		decls.push_back(decl);
		const int index = r_fn.param_typed[i] ? -1 : int(i);
		if (f.mutated.count(gd)) {
			const std::string copy = fresh(f, name + "Value");
			copies.push_back("var " + copy + ":" + verse_text(t) + " = " + name);
			f.scopes.back()[gd] = { copy, t, true, index };
		} else {
			f.scopes.back()[gd] = { name, t, false, index };
		}
	}
	std::string params;
	for (size_t i = 0; i < decls.size(); i++) {
		params += (i ? ", " : "") + decls[i];
	}

	line_out body;
	for (const std::string &c : copies) {
		body.add(0, c);
	}
	if (r_fn.is_virtual && r_fn.gd_name == "_ready") {
		emit_ready_prologue(r_cls, f, body);
	}
	emit_block(f, r_fn.decl->body, 0, body);
	// The empty-body `{}` is only needed when nothing else in the body is code -- a `_Ready` whose
	// own body was `pass` still has its prologue.
	if (!body.lines.empty() && body.lines.back() == "{}") {
		int code_lines = 0;
		for (const std::string &l : body.lines) {
			const std::string t = trim(l);
			code_lines += !t.empty() && t[0] != '#' ? 1 : 0;
		}
		if (code_lines > 1) {
			body.lines.pop_back();
		}
	}

	ty ret = r_fn.ret.known() ? r_fn.ret : ty::of(tk::void_);
	const std::string ret_text = ret.k == tk::void_ ? "void" : verse_text(ret);
	const std::string trailing = trailing_comment(r_fn.decl->line, r_fn.decl->line);

	if (r_fn.is_virtual && (r_fn.split_async || r_fn.predicate_helper)) {
		std::string call_args;
		for (size_t i = 0; i < r_fn.param_names.size(); i++) {
			call_args += (i ? ", " : "") + r_fn.param_names[i];
		}
		if (r_fn.split_async) {
			r_out.add(1, "# Godot calls `" + r_fn.verse_name + "` and cannot wait for it, so the waiting happens in `" + r_fn.helper_name + "`.");
			r_out.add(1, r_fn.verse_name + "<override>(" + params + "):void =" + trailing);
			r_out.add(2, "spawn{" + r_fn.helper_name + "(" + call_args + ")}");
			r_out.blank();
			r_out.add(1, r_fn.helper_name + "(" + params + ")<suspends>:" + ret_text + " =");
		} else {
			r_out.add(1, r_fn.verse_name + "<override>(" + params + ")<decides>:void =" + trailing);
			r_out.add(2, r_fn.helper_name + "(" + call_args + ")?");
			r_out.blank();
			r_out.add(1, r_fn.helper_name + "(" + params + ")" + effect_text(r_fn.eff == effect::none ? effect::transacts : r_fn.eff) + ":logic =");
		}
		r_out.append(body, 2);
		return;
	}

	std::string head;
	if (r_fn.is_static) {
		head = r_fn.verse_name + "(" + params + ")" + effect_text(r_fn.eff) + ":" + ret_text + " =";
		r_out.add(0, head + trailing);
		r_out.append(body, 1);
		return;
	}
	if (r_fn.is_virtual) {
		head = r_fn.verse_name + "<override>(" + params + "):" + ret_text + " =";
	} else {
		head = r_fn.verse_name + "(" + params + ")" + effect_text(r_fn.eff) + ":" + ret_text + " =";
	}
	r_out.add(1, head + trailing);
	r_out.append(body, 2);
}

// Where a member's initialiser runs when a Verse data member's default cannot hold it: an
// `@onready` var, a `preload`, or anything that calls a function. GDScript runs `@onready` ones
// just before `_ready`, which is the start of `_Ready` here; the others it ran earlier, at
// construction, which has no Verse hook, so they move too and say so.
void converter::emit_ready_prologue(class_ctx &r_cls, fctx &r_f, line_out &r_out) {
	for (const member &m : r_cls.decl->members) {
		if (m.kind != member::kind::var) {
			continue;
		}
		var_info &v = r_cls.vars[m.var->name];
		if (!(v.onready || v.deferred_init) || !v.decl->init) {
			continue;
		}
		sctx s;
		const ty want = v.t.k == tk::option ? v.t.inner() : v.t;
		ev value = ex(r_f, v.decl->init, s, want.known() ? &want : nullptr);
		if (value.todo || !s.todos.empty()) {
			emit_todo(r_out, 0, v.decl->line, v.decl->end_line, s.todos);
			continue;
		}
		std::string code;
		if (v.t.k == tk::option && value.f == form::fails) {
			code = "option{" + value.code + "}";
		} else {
			code = coerce(r_f, value, v.t, s, v.decl->line);
		}
		emit_guarded(r_out, 0, s, "set " + v.verse_name + " = " + code, "");
	}
}

std::string converter::export_attributes(const var_info &p_var, ty &r_type, int p_line) {
	std::string out;
	auto string_args = [&](const annotation &a, const std::string &p_separator) {
		std::string joined;
		for (const expr_ptr &arg : a.args) {
			if (arg->kind != ek::string) {
				return std::string();
			}
			joined += (joined.empty() ? "" : p_separator) + arg->text;
		}
		return joined;
	};
	bool exported = false;
	for (const annotation &a : p_var.decl->annotations) {
		const std::string &n = a.name;
		if (n == "export" || n == "export_storage") {
			exported = true;
			if (n == "export_storage") {
				note(a.line, "`@export_storage` became `@export`: Verse has no storage-only export, so the member is in the inspector too", true);
			}
		} else if (n == "export_range") {
			exported = true;
			// A bounded type is how Verse says what `@export_range` said (docs/property-export.md).
			if (a.args.size() >= 2 && a.args[0]->kind == ek::number && a.args[1]->kind == ek::number && r_type.numeric()) {
				auto literal = [&](const expr_ptr &e) {
					return r_type.k == tk::float_ ? format_float(e->is_float ? e->text : format_int(e->text)) : format_int(e->text);
				};
				bool integral_bounds = r_type.k != tk::int_ || (!a.args[0]->is_float && !a.args[1]->is_float);
				if (integral_bounds) {
					const std::string t = verse_text(r_type);
					r_type.payload = "type{_X:" + t + " where " + literal(a.args[0]) + " <= _X, _X <= " + literal(a.args[1]) + "}";
				}
				if (a.args.size() > 2) {
					note(a.line, "`@export_range`'s step and hints (`or_greater`, `suffix:` ...) have no Verse spelling and were dropped");
				}
			} else {
				note(a.line, "`@export_range` became a plain `@export`: its bounds are not number literals on a number", true);
			}
		} else if (n == "export_file" || n == "export_global_file") {
			exported = true;
			const std::string filters = string_args(a, ",");
			out += "\t@export_file(\"" + escape_verse(filters.empty() ? "*" : filters) + "\")\n";
		} else if (n == "export_dir" || n == "export_global_dir") {
			exported = true;
			out += "\t@export_dir\n";
		} else if (n == "export_multiline") {
			exported = true;
			out += "\t@export_multiline\n";
		} else if (n == "export_flags") {
			exported = true;
			out += "\t@export_flags(\"" + escape_verse(string_args(a, ",")) + "\")\n";
		} else if (n == "export_node_path") {
			exported = true;
			out += "\t@export_node_path(\"" + escape_verse(string_args(a, ",")) + "\")\n";
		} else if (n == "export_category" || n == "export_group" || n == "export_subgroup") {
			if (!a.args.empty() && a.args[0]->kind == ek::string) {
				out += "\t@" + n + "(\"" + escape_verse(a.args[0]->text) + "\")\n";
				if (a.args.size() > 1) {
					note(a.line, "`@" + n + "`'s prefix argument has no Verse spelling and was dropped");
				}
			}
		} else if (n.rfind("export", 0) == 0) {
			exported = true;
			note(a.line, "`@" + n + "` became a plain `@export`: the inspector hint it gives has no Verse attribute yet", true);
		}
	}
	(void)p_line;
	if (exported) {
		out += "\t@export\n";
	}
	return out;
}

void converter::type_vars(class_ctx &r_cls) {
	fctx f;
	f.cls = &r_cls;
	f.scopes.emplace_back();
	// `var screen_size` with no type and no initialiser: the first thing a method assigns to it,
	// in source order, is its type -- `screen_size = get_viewport_rect().size` makes a `vector2`
	// rather than the `?variant` nothing else would say.
	for (auto &kv : r_cls.vars) {
		var_info &v = kv.second;
		if (v.decl->init || !v.decl->type_text.empty()) {
			continue;
		}
		bool settled = false;
		for (const member &m : r_cls.decl->members) {
			if (settled || m.kind != member::kind::func) {
				continue;
			}
			fn_info &fn = *r_cls.fns[m.func->name];
			fctx ff;
			ff.cls = &r_cls;
			ff.fn = &fn;
			ff.scopes.emplace_back();
			bool shadowed = false;
			for (size_t i = 0; i < fn.param_names.size() && i < fn.decl->params.size(); i++) {
				ff.scopes.back()[fn.decl->params[i].name] = { fn.param_names[i], fn.param_types[i].known() ? fn.param_types[i] : ty::of(tk::variant), false };
				shadowed = shadowed || fn.decl->params[i].name == v.gd_name;
			}
			std::function<void(const std::vector<stmt_ptr> &)> walk = [&](const std::vector<stmt_ptr> &body) {
				for (const stmt_ptr &st : body) {
					if (settled) {
						return;
					}
					if (st->kind == sk::var && st->text == v.gd_name) {
						shadowed = true;
					}
					if (!shadowed && st->kind == sk::assign && st->text == "=" && st->args[0]->kind == ek::ident && st->args[0]->text == v.gd_name) {
						sctx scratch;
						const bool quiet = final_pass;
						final_pass = false;
						ev value = ex(ff, st->args[1], scratch);
						final_pass = quiet;
						if (!value.todo && !value.is_null && value.t.known() && value.t.k != tk::void_ && value.t.k != tk::type_value
								&& value.t.k != tk::function && value.t.k != tk::variant) {
							v.t = value.t.k == tk::object || value.f == form::fails ? ty::option_of(value.t) : value.t;
							if (value.f == form::test) {
								v.t = ty::of(tk::logic);
							}
							settled = true;
							return;
						}
					}
					for (const if_branch &b : st->branches) {
						walk(b.body);
					}
					for (const match_branch &b : st->cases) {
						walk(b.body);
					}
					walk(st->body);
				}
			};
			walk(fn.decl->body);
		}
	}
	for (auto &kv : r_cls.vars) {
		var_info &v = kv.second;
		if (v.t.known() || !v.decl->init) {
			continue;
		}
		sctx s;
		const bool quiet = final_pass;
		final_pass = false;
		ev value = ex(f, v.decl->init, s);
		final_pass = quiet;
		if (value.todo) {
			v.t = ty::option_of(ty::of(tk::variant));
			continue;
		}
		if (value.is_null) {
			v.t = ty::option_of(ty::of(tk::variant));
		} else if (value.f == form::test) {
			v.t = ty::of(tk::logic);
		} else if (value.t.k == tk::object) {
			// A node looked up, a resource loaded: absent until `_Ready` fills it in, and an object
			// slot has no value for "nothing" but `false`.
			const bool archetype = value.code.size() > 2 && value.code.compare(value.code.size() - 2, 2, "{}") == 0;
			v.t = archetype && s.clauses.empty() && !v.onready ? value.t : ty::option_of(value.t);
		} else {
			v.t = value.t;
		}
		if (!v.t.known() || v.t.k == tk::void_ || v.t.k == tk::type_value || v.t.k == tk::function) {
			v.t = ty::option_of(ty::of(tk::variant));
		}
	}
	for (auto &kv : r_cls.vars) {
		var_info &v = kv.second;
		if (v.t.known()) {
			auto setter = r_cls.fns.find("<set:" + v.gd_name + ">");
			if (setter != r_cls.fns.end() && !setter->second->param_types.empty() && !setter->second->param_typed[0]) {
				setter->second->param_types[0] = v.t;
				setter->second->param_typed[0] = true;
			}
			auto getter = r_cls.fns.find("<get:" + v.gd_name + ">");
			if (getter != r_cls.fns.end() && !getter->second->ret_declared) {
				getter->second->ret = v.t;
				getter->second->ret_declared = true;
			}
		}
		if (!v.t.known()) {
			// `var x` with nothing to go on: an optional variant, which holds what GDScript's could
			// and starts as the null GDScript's did.
			v.t = ty::option_of(ty::of(tk::variant));
		}
		if (v.decl->init && !v.onready && !v.deferred_init && !is_static_initializer(v.decl->init)) {
			v.deferred_init = true;
			note(v.decl->line, "`" + v.gd_name + "`'s initialiser calls something, which a Verse member's default cannot; it runs at the start of `_Ready` instead");
		}
	}
}

void converter::emit_member_var(class_ctx &r_cls, var_info &r_var, line_out &r_out) {
	ty t = r_var.t;
	const std::string attributes = export_attributes(r_var, t, r_var.decl->line);
	std::string value;
	if (r_var.decl->init && !r_var.onready && !r_var.deferred_init) {
		fctx f;
		f.cls = &r_cls;
		f.scopes.emplace_back();
		sctx s;
		const ty want = t;
		ev v = ex(f, r_var.decl->init, s, want.known() ? &want : nullptr);
		if (!v.todo && s.todos.empty() && s.clauses.empty()) {
			value = coerce(f, v, t, s, r_var.decl->line);
		}
		if (!s.todos.empty() || v.todo) {
			emit_todo(r_out, 1, r_var.decl->line, r_var.decl->end_line, s.todos);
		}
	}
	if (value.empty() && t.k == tk::enum_) {
		for (class_ctx *c = &r_cls; c && value.empty(); c = c->outer) {
			for (const auto &kv : c->enums) {
				if (kv.second.verse_name == t.name && !kv.second.values.empty()) {
					value = t.name + "." + kv.second.values[0].second;
				}
			}
		}
	}
	if (value.empty()) {
		value = default_value(t);
		if (value.empty()) {
			t = ty::option_of(t);
			value = "false";
		}
	}
	std::string type_text = t.payload.rfind("type{", 0) == 0 && t.numeric() ? t.payload : verse_text(t);
	if (!attributes.empty()) {
		std::string a = attributes;
		size_t start = 0;
		while (start < a.size()) {
			size_t end = a.find('\n', start);
			r_out.add(0, a.substr(start, end - start));
			start = end + 1;
		}
	}
	const std::string tail = trailing_comment(r_var.decl->line, r_var.decl->line);
	if (r_var.is_const) {
		r_out.add(1, r_var.verse_name + ":" + type_text + " = " + value + tail);
	} else {
		r_out.add(1, "var " + r_var.verse_name + ":" + type_text + " = " + value + tail);
	}
}

void converter::build_class(class_ptr p_decl, const std::string &p_verse_name, class_ctx *p_outer) {
	auto c = std::make_unique<class_ctx>();
	c->decl = p_decl;
	c->verse_name = p_verse_name;
	c->outer = p_outer;
	c->is_inner = p_outer != nullptr;
	class_ctx *raw = c.get();
	classes.push_back(std::move(c));
	if (!p_outer) {
		root_ctx = raw;
	}
	prepare_class(*raw);
	for (member &m : p_decl->members) {
		if (m.kind == member::kind::inner_class) {
			build_class(m.inner, raw->inner_classes[m.inner->name], raw);
		}
	}
}

void converter::emit_class(class_ctx &r_cls, line_out &r_out) {
	class_decl &d = *r_cls.decl;
	if (!r_cls.is_inner) {
		if (!d.name.empty()) {
			r_out.add(0, "@global_class");
		}
	} else {
		r_out.add(0, "# `" + d.name + "`, an inner class of `" + r_cls.outer->decl->name + "`: a class of its own here. Only the class named after "
							"the file can go on a node, so this one is made with `" + r_cls.verse_name + "{}` and never attached.");
		note(d.line, "the inner class `" + d.name + "` became the top-level class `" + r_cls.verse_name + "`");
	}
	for (const annotation &a : d.annotations) {
		if (a.name == "tool") {
			r_out.add(0, "@tool");
		} else if (a.name == "icon" && !a.args.empty() && a.args[0]->kind == ek::string) {
			r_out.add(0, "@icon(\"" + escape_verse(a.args[0]->text) + "\")");
		} else if (a.name == "abstract") {
			note(a.line, "`@abstract` was dropped: Verse's `<abstract>` refuses the archetype the bridge builds a node's script from", true);
		}
	}
	r_out.add(0, r_cls.verse_name + " := class(" + r_cls.base_verse + "):");

	bool has_ready = r_cls.fns.count("_ready") > 0;
	bool needs_prologue = false;
	for (auto &kv : r_cls.vars) {
		needs_prologue = needs_prologue || kv.second.onready || kv.second.deferred_init;
	}

	int last_line = 0;
	bool last_was_func = false;
	bool first = true;
	bool emitted_synth_ready = false;
	auto spacer = [&](int line, bool is_func) {
		if (first) {
			r_out.blank();
			first = false;
			return;
		}
		if (is_func || last_was_func || (last_line && line > last_line + 1)) {
			r_out.blank();
		}
	};
	for (member &m : d.members) {
		switch (m.kind) {
			case member::kind::signal: {
				spacer(m.line, false);
				flush_comments(r_out, 1, m.line);
				const sig_info &s = r_cls.sigs[m.signal->name];
				r_out.add(1, "@export_signal");
				r_out.add(1, s.verse_name + ":event(" + (s.payload == "tuple()" ? "" : s.payload) + ") = event(" + (s.payload == "tuple()" ? "" : s.payload) + "){}"
								+ trailing_comment(m.line, m.line));
				last_line = m.line;
				last_was_func = false;
				break;
			}
			case member::kind::var: {
				spacer(m.line, false);
				flush_comments(r_out, 1, m.line);
				emit_member_var(r_cls, r_cls.vars[m.var->name], r_out);
				last_line = m.var->end_line;
				last_was_func = false;
				if (m.var->getter || m.var->setter) {
					// Their bodies are emitted with the functions below.
					mark_comments_used(m.var->line + 1, m.var->end_line);
				}
				break;
			}
			case member::kind::func: {
				fn_info &f = *r_cls.fns[m.func->name];
				if (f.is_static) {
					break;
				}
				if (!has_ready && needs_prologue && !emitted_synth_ready) {
					spacer(m.line, true);
					r_out.add(1, "_Ready<override>():void =");
					line_out prologue;
					fctx ff;
					ff.cls = &r_cls;
					ff.scopes.emplace_back();
					emit_ready_prologue(r_cls, ff, prologue);
					r_out.append(prologue, 2);
					emitted_synth_ready = true;
				}
				spacer(m.line, true);
				flush_comments(r_out, 1, m.line);
				emit_function(r_cls, f, r_out);
				last_line = m.func->end_line;
				last_was_func = true;
				break;
			}
			case member::kind::enum_:
			case member::kind::inner_class:
				mark_comments_used(m.line, m.kind == member::kind::enum_ ? m.enum_->end_line : m.inner->end_line);
				break;
		}
	}
	if (!has_ready && needs_prologue && !emitted_synth_ready) {
		spacer(0, true);
		r_out.add(1, "_Ready<override>():void =");
		line_out prologue;
		fctx ff;
		ff.cls = &r_cls;
		ff.scopes.emplace_back();
		emit_ready_prologue(r_cls, ff, prologue);
		r_out.append(prologue, 2);
	}
	// Accessor bodies and hoisted lambdas, after everything the author wrote.
	for (auto &kv : r_cls.vars) {
		for (const char *which : { "<set:", "<get:" }) {
			auto f = r_cls.fns.find(which + kv.first + ">");
			if (f != r_cls.fns.end()) {
				r_out.blank();
				emit_function(r_cls, *f->second, r_out);
			}
		}
	}
	for (auto &lf : r_cls.lambdas) {
		r_out.blank();
		r_out.add(1, "# Was a lambda on line " + std::to_string(lf->decl->line) + ". Verse has no anonymous or nested functions yet.");
		emit_function(r_cls, *lf, r_out);
	}
	if (first) {
		// A class with nothing in it still needs a body.
		r_out.lines.back() = r_cls.verse_name + " := class(" + r_cls.base_verse + ") {}";
	}
}

VerseGdConvertResult converter::run() {
	parsed = parse(in.source);
	if (!parsed.error.empty()) {
		res.ok = false;
		res.error = parsed.error;
		res.error_line = parsed.error_line;
		return res;
	}
	res.ok = true;
	{
		size_t start = 0;
		while (start <= in.source.size()) {
			size_t end = in.source.find('\n', start);
			if (end == std::string::npos) {
				end = in.source.size();
			}
			std::string l = in.source.substr(start, end - start);
			if (!l.empty() && l.back() == '\r') {
				l.pop_back();
			}
			src_lines.push_back(l);
			start = end + 1;
		}
	}
	comment_used.assign(parsed.comments.size(), false);

	class_decl &root = *parsed.root;
	res.gd_class_name = root.name;
	verse_stem = verse_gd_stem(in.file_stem, root.name);
	res.verse_stem = verse_stem;
	if (!root.name.empty()) {
		res.global_name = verse_pascal_case(verse_stem);
		if (res.global_name != root.name) {
			note(root.line, "Godot will register this class as `" + res.global_name + "` rather than `" + root.name
							+ "`: a Verse global class takes its name from its file");
		}
	}

	// The pre-pass speaks through notes, so it runs as the final pass would.
	final_pass = true;
	build_class(parsed.root, verse_stem, nullptr);
	for (auto &c : classes) {
		type_vars(*c);
	}

	// Twice without keeping anything, so a return type inferred from a `return` and a parameter
	// type settled by a call site are known before the pass that is written out.
	final_pass = false;
	for (int pass = 0; pass < 2; pass++) {
		for (auto &c : classes) {
			line_out scratch;
			emit_class(*c, scratch);
			for (const member &m : c->decl->members) {
				if (m.kind == member::kind::func && m.func->is_static) {
					emit_function(*c, *c->fns[m.func->name], scratch);
				}
			}
		}
		comment_used.assign(parsed.comments.size(), false);
		for (auto &c : classes) {
			solve_effects(*c);
		}
	}
	final_pass = true;

	// What another file's conversion needs to know about this class.
	res.self.verse_name = root_ctx->verse_name;
	res.self.godot_base = root_ctx->base_godot;
	for (const auto &kv : root_ctx->fns) {
		const fn_info &f = *kv.second;
		if (f.is_lambda || f.is_static || kv.first.empty() || kv.first[0] == '<') {
			continue;
		}
		VerseGdMethodInfo m;
		m.verse_name = f.split_async ? f.helper_name : f.verse_name;
		for (size_t i = 0; i < f.decl->params.size(); i++) {
			m.param_names.push_back(f.param_names[i]);
			m.param_types.push_back(verse_text(f.param_types[i].known() ? f.param_types[i] : ty::of(tk::variant)));
			m.param_optional.push_back(f.param_optional[i]);
		}
		m.result_type = f.ret.known() ? (f.ret.k == tk::void_ ? "void" : verse_text(f.ret)) : "void";
		m.suspends = f.eff == effect::suspends || f.split_async;
		res.self.methods[kv.first] = m;
	}
	for (const auto &kv : root_ctx->vars) {
		if (!kv.second.is_const) {
			res.self.properties[kv.first] = { kv.second.verse_name, verse_text(kv.second.t) };
		}
	}
	for (const auto &kv : root_ctx->sigs) {
		res.self.signals[kv.first] = { kv.second.verse_name, kv.second.payload };
	}

	line_out out;
	out.add(0, "using { /Godot.org/Godot }");
	if (!bindings_used.empty()) {
		// Every GDScript class this file names that stays GDScript, reached through its binding.
		out.add(0, "using { /Godot.org/Bindings }");
		std::string names;
		for (const std::string &n : bindings_used) {
			names += (names.empty() ? "`" : ", `") + n + "`";
		}
		note(0, names + (bindings_used.size() == 1 ? " is" : " are") + " still GDScript, reached through the generated bindings");
	}
	out.add(0, "");
	int first_line = 1 << 30;
	for (const member &m : root.members) {
		first_line = std::min(first_line, m.line);
	}
	for (const annotation &a : root.annotations) {
		first_line = std::min(first_line, a.line);
	}
	// Anything above `extends` is the file's own header; say it before anything else.
	int extends_line = 0;
	for (size_t i = 0; i < src_lines.size(); i++) {
		const std::string t = trim(src_lines[i]);
		if (t.rfind("extends", 0) == 0 || t.rfind("class_name", 0) == 0) {
			extends_line = int(i) + 1;
			break;
		}
	}
	flush_comments(out, 0, extends_line ? extends_line : first_line);
	if (out.lines.size() > 2) {
		out.add(0, "");
	}
	mark_comments_used(extends_line, extends_line);

	for (auto &c : classes) {
		for (const member &m : c->decl->members) {
			if (m.kind != member::kind::enum_) {
				continue;
			}
			const std::string key = m.enum_->name.empty() ? "<anonymous" + std::to_string(m.enum_->line) + ">" : m.enum_->name;
			const enum_info &e = c->enums[key];
			flush_comments(out, 0, m.line);
			out.add(0, e.verse_name + " := enum:");
			for (const auto &v : e.values) {
				out.add(1, v.second);
			}
			out.add(0, "");
		}
	}
	for (size_t i = 0; i < classes.size(); i++) {
		if (i) {
			out.blank();
		}
		emit_class(*classes[i], out);
	}
	for (auto &c : classes) {
		for (const member &m : c->decl->members) {
			if (m.kind == member::kind::func && m.func->is_static) {
				out.blank();
				flush_comments(out, 0, m.line);
				emit_function(*c, *c->fns[m.func->name], out);
			}
		}
	}
	// A comment nothing claimed -- at the end of the file, or after the last member.
	flush_comments(out, 0, 1 << 30);
	while (!out.lines.empty() && out.lines.back().empty()) {
		out.lines.pop_back();
	}
	for (const std::string &l : out.lines) {
		res.verse += l + "\n";
	}
	std::stable_sort(res.notes.begin(), res.notes.end(), [](const VerseGdNote &a, const VerseGdNote &b) { return a.line < b.line; });
	return res;
}

} // namespace

VerseGdConvertResult verse_gd_convert(const VerseGdConvertInput &p_input) {
	return converter(p_input).run();
}

std::vector<VerseGdConvertResult> verse_gd_convert_batch(const std::vector<VerseGdConvertInput> &p_inputs) {
	std::vector<VerseGdConvertInput> inputs = p_inputs;
	std::vector<VerseGdConvertResult> results;
	for (int round = 0; round < 3; round++) {
		results.clear();
		for (const VerseGdConvertInput &input : inputs) {
			results.push_back(verse_gd_convert(input));
		}
		// What every file passes to each class's untyped parameters, merged: a hint only where
		// every caller agrees, and never `variant`, which says nothing.
		std::map<std::string, std::map<std::string, std::map<int, std::set<std::string>>>> seen;
		for (const VerseGdConvertResult &r : results) {
			for (const auto &cls : r.observed_arguments) {
				for (const auto &method : cls.second) {
					for (const auto &arg : method.second) {
						seen[cls.first][method.first][arg.first].insert(arg.second.begin(), arg.second.end());
					}
				}
			}
		}
		for (size_t i = 0; i < inputs.size(); i++) {
			if (!results[i].ok) {
				continue;
			}
			inputs[i].param_hints.clear();
			for (const auto &method : seen[results[i].verse_stem]) {
				for (const auto &arg : method.second) {
					if (arg.second.size() == 1 && *arg.second.begin() != "variant") {
						inputs[i].param_hints[method.first][arg.first] = *arg.second.begin();
					}
				}
			}
			for (auto &kv : inputs[i].script_classes) {
				for (size_t j = 0; j < results.size(); j++) {
					if (j != i && results[j].ok && kv.second.verse_name == results[j].verse_stem) {
						const std::string path = kv.second.path;
						kv.second = results[j].self;
						kv.second.path = path;
					}
				}
			}
		}
	}
	return results;
}

std::string verse_gd_member_name(const std::string &p_gdscript_name) {
	return pascal(p_gdscript_name);
}

std::string verse_gd_stem(const std::string &p_file_stem, const std::string &p_class_name) {
	std::string stem;
	if (!p_class_name.empty()) {
		stem = verse_binding_class_name(p_class_name);
	} else {
		for (char c : p_file_stem) {
			stem += std::isalnum((unsigned char)c) ? char(std::tolower((unsigned char)c)) : '_';
		}
		if (p_file_stem.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos && p_file_stem.find('_') == std::string::npos
				&& p_file_stem.find('-') == std::string::npos && p_file_stem.find(' ') == std::string::npos) {
			stem = verse_binding_class_name(p_file_stem);
		}
	}
	if (stem.empty() || std::isdigit((unsigned char)stem[0])) {
		stem = "script_" + stem;
	}
	return stem;
}

static std::string scan_top_level(const std::string &p_source, const char *p_keyword) {
	const lex_result lexed = lex(p_source);
	int depth = 0;
	for (size_t i = 0; i + 1 < lexed.tokens.size(); i++) {
		const token &t = lexed.tokens[i];
		if (t.kind == tok::indent) {
			depth++;
		} else if (t.kind == tok::dedent) {
			depth--;
		} else if (depth == 0 && t.kind == tok::ident && t.text == p_keyword && (i == 0 || lexed.tokens[i - 1].kind == tok::newline)) {
			const token &n = lexed.tokens[i + 1];
			if (n.kind == tok::ident || n.kind == tok::string) {
				return n.text;
			}
		} else if (depth == 0 && std::string(p_keyword) == "extends" && t.kind == tok::ident && t.text == "class_name") {
			// `class_name X extends Y` on one line.
			if (i + 3 < lexed.tokens.size() && lexed.tokens[i + 2].kind == tok::ident && lexed.tokens[i + 2].text == "extends") {
				return lexed.tokens[i + 3].text;
			}
		}
	}
	return "";
}

std::string verse_gd_scan_class_name(const std::string &p_source) {
	return scan_top_level(p_source, "class_name");
}

std::string verse_gd_scan_extends(const std::string &p_source) {
	return scan_top_level(p_source, "extends");
}
