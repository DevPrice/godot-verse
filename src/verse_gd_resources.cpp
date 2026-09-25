#include "verse_gd_convert.h"

#include "verse_gd_api.gen.h"
#include "verse_gd_syntax.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

// The other files a conversion touches: the scenes and resources that name the script, and the
// GDScripts that call into it. Both are text Godot wrote, so both are rewritten by editing the text
// in place -- never by re-serialising -- which keeps everything the conversion did not mean to
// change byte-for-byte what it was.

namespace {

// --- A text resource, read well enough to rewrite ----------------------------------------------

struct attribute {
	std::string key;
	std::string value; // unquoted
	size_t value_begin = 0; // the span of the value in the header line, quotes included
	size_t value_end = 0;
};

struct section {
	std::string tag; // "node", "ext_resource", "connection", "resource", "sub_resource", "gd_scene"...
	size_t header_line = 0;
	std::vector<attribute> attributes;
	// The property lines of the section: index of the line that starts each property, its key.
	std::vector<std::pair<size_t, std::string>> properties;
	std::string script_id; // `script = ExtResource("id")`

	const attribute *find(const std::string &p_key) const {
		for (const attribute &a : attributes) {
			if (a.key == p_key) {
				return &a;
			}
		}
		return nullptr;
	}
	std::string get(const std::string &p_key) const {
		const attribute *a = find(p_key);
		return a ? a->value : "";
	}
};

struct resource_text {
	std::vector<std::string> lines;
	std::vector<section> sections;
};

std::vector<attribute> parse_header(const std::string &p_line, std::string &r_tag) {
	std::vector<attribute> out;
	size_t i = 1;
	while (i < p_line.size() && !std::isspace((unsigned char)p_line[i]) && p_line[i] != ']') {
		r_tag += p_line[i++];
	}
	while (i < p_line.size()) {
		while (i < p_line.size() && std::isspace((unsigned char)p_line[i])) {
			i++;
		}
		if (i >= p_line.size() || p_line[i] == ']') {
			break;
		}
		attribute a;
		while (i < p_line.size() && p_line[i] != '=' && !std::isspace((unsigned char)p_line[i]) && p_line[i] != ']') {
			a.key += p_line[i++];
		}
		if (i >= p_line.size() || p_line[i] != '=') {
			continue;
		}
		i++;
		a.value_begin = i;
		if (i < p_line.size() && p_line[i] == '"') {
			i++;
			while (i < p_line.size() && p_line[i] != '"') {
				if (p_line[i] == '\\' && i + 1 < p_line.size()) {
					i++;
				}
				a.value += p_line[i++];
			}
			i++;
		} else {
			// `instance=ExtResource("2")`, `unique_name_in_owner=true`: up to the next space at
			// depth zero, brackets and quotes included.
			int depth = 0;
			bool quoted = false;
			while (i < p_line.size()) {
				const char c = p_line[i];
				if (c == '"') {
					quoted = !quoted;
				} else if (!quoted && c == '(') {
					depth++;
				} else if (!quoted && c == ')') {
					depth--;
				} else if (!quoted && depth == 0 && (std::isspace((unsigned char)c) || c == ']')) {
					break;
				}
				a.value += c;
				i++;
			}
		}
		a.value_end = i;
		out.push_back(a);
	}
	return out;
}

// `ExtResource("2_abc")`, `ExtResource( 2 )` -> `2_abc`, `2`.
std::string ext_resource_id(const std::string &p_value) {
	const size_t open = p_value.find("ExtResource(");
	if (open == std::string::npos) {
		return "";
	}
	std::string inner = p_value.substr(open + 12);
	inner = inner.substr(0, inner.find(')'));
	std::string id;
	for (char c : inner) {
		if (c != '"' && c != ' ') {
			id += c;
		}
	}
	return id;
}

int bracket_delta(const std::string &p_text) {
	int depth = 0;
	bool quoted = false;
	for (size_t i = 0; i < p_text.size(); i++) {
		const char c = p_text[i];
		if (c == '\\' && quoted) {
			i++;
			continue;
		}
		if (c == '"') {
			quoted = !quoted;
		} else if (!quoted && (c == '[' || c == '(' || c == '{')) {
			depth++;
		} else if (!quoted && (c == ']' || c == ')' || c == '}')) {
			depth--;
		}
	}
	return depth;
}

resource_text parse_resource(const std::string &p_text) {
	resource_text r;
	size_t start = 0;
	while (start <= p_text.size()) {
		size_t end = p_text.find('\n', start);
		if (end == std::string::npos) {
			end = p_text.size();
		}
		r.lines.push_back(p_text.substr(start, end - start));
		start = end + 1;
	}
	int depth = 0;
	for (size_t i = 0; i < r.lines.size(); i++) {
		std::string line = r.lines[i];
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (depth > 0) {
			depth += bracket_delta(line);
			continue;
		}
		if (!line.empty() && line[0] == '[') {
			section s;
			s.header_line = i;
			s.attributes = parse_header(line, s.tag);
			r.sections.push_back(s);
			continue;
		}
		const size_t eq = line.find(" = ");
		if (eq != std::string::npos && !r.sections.empty() && !line.empty() && !std::isspace((unsigned char)line[0])) {
			const std::string key = line.substr(0, eq);
			r.sections.back().properties.push_back({ i, key });
			if (key == "script") {
				r.sections.back().script_id = ext_resource_id(line.substr(eq + 3));
			}
			depth = bracket_delta(line.substr(eq + 3));
		}
	}
	return r;
}

std::string join_lines(const std::vector<std::string> &p_lines) {
	std::string out;
	for (size_t i = 0; i < p_lines.size(); i++) {
		out += p_lines[i];
		if (i + 1 < p_lines.size()) {
			out += '\n';
		}
	}
	return out;
}

std::string node_path(const section &p_node) {
	const std::string name = p_node.get("name");
	if (!p_node.find("parent")) {
		return ".";
	}
	const std::string parent = p_node.get("parent");
	return parent == "." ? name : parent + "/" + name;
}

std::map<std::string, std::string> ext_paths(const resource_text &p_r) {
	std::map<std::string, std::string> out;
	for (const section &s : p_r.sections) {
		if (s.tag == "ext_resource") {
			out[s.get("id")] = s.get("path");
		}
	}
	return out;
}

// The root node of a scene: its type and the script on it, following an inherited scene down.
struct root_info {
	std::string type;
	std::string script;
};

root_info scene_root(const std::string &p_path, const VerseGdResourceReader &p_read, int p_depth = 0) {
	root_info out;
	if (p_depth > 16 || !p_read) {
		return out;
	}
	const std::string text = p_read(p_path);
	if (text.empty()) {
		return out;
	}
	const resource_text r = parse_resource(text);
	const std::map<std::string, std::string> ext = ext_paths(r);
	for (const section &s : r.sections) {
		if (s.tag != "node" || s.find("parent")) {
			continue;
		}
		if (const attribute *inst = s.find("instance")) {
			auto it = ext.find(ext_resource_id(inst->value));
			if (it != ext.end()) {
				out = scene_root(it->second, p_read, p_depth + 1);
			}
		}
		if (!s.get("type").empty()) {
			out.type = s.get("type");
		}
		if (!s.script_id.empty()) {
			auto it = ext.find(s.script_id);
			if (it != ext.end()) {
				out.script = it->second;
			}
		}
		break;
	}
	return out;
}

// The script each node carries: its own, or the instanced scene's root's.
std::map<std::string, std::string> node_scripts(const resource_text &p_r, const VerseGdResourceReader &p_read) {
	std::map<std::string, std::string> out;
	const std::map<std::string, std::string> ext = ext_paths(p_r);
	for (const section &s : p_r.sections) {
		if (s.tag != "node") {
			continue;
		}
		std::string script;
		if (!s.script_id.empty()) {
			auto it = ext.find(s.script_id);
			if (it != ext.end()) {
				script = it->second;
			}
		} else if (const attribute *inst = s.find("instance")) {
			auto it = ext.find(ext_resource_id(inst->value));
			if (it != ext.end()) {
				script = scene_root(it->second, p_read).script;
			}
		}
		if (!script.empty()) {
			out[node_path(s)] = script;
		}
	}
	return out;
}

// A connection's `from`/`to` is relative to the scene root, as a node path is here.
std::string normalise(const std::string &p_path) {
	if (p_path.empty() || p_path == ".") {
		return ".";
	}
	return p_path.rfind("./", 0) == 0 ? p_path.substr(2) : p_path;
}

const VerseGdScriptMove *move_for(const std::vector<VerseGdScriptMove> &p_moves, const std::string &p_path) {
	for (const VerseGdScriptMove &m : p_moves) {
		if (m.old_path == p_path) {
			return &m;
		}
	}
	return nullptr;
}

std::string renamed(const VerseGdScriptMove &p_move, VerseGdRename::Kind p_kind, const std::string &p_name) {
	for (const VerseGdRename &r : p_move.renames) {
		if (r.kind == p_kind && r.gdscript == p_name) {
			return r.verse;
		}
	}
	return "";
}

void replace_attribute(std::string &r_line, const attribute &p_attribute, const std::string &p_value) {
	// Every attribute this rewrites is a quoted string in Godot's own output.
	r_line.replace(p_attribute.value_begin, p_attribute.value_end - p_attribute.value_begin, "\"" + p_value + "\"");
}

} // namespace

std::string verse_gd_common_class(const std::string &p_a, const std::string &p_b) {
	if (p_a == p_b) {
		return p_a;
	}
	auto parent = [](const std::string &p_class) -> std::string {
		const verse_gd_api::class_row *row = std::lower_bound(std::begin(verse_gd_api::classes), std::end(verse_gd_api::classes), p_class,
				[](const verse_gd_api::class_row &r, const std::string &k) { return std::strcmp(r.godot_name, k.c_str()) < 0; });
		if (row != std::end(verse_gd_api::classes) && p_class == row->godot_name) {
			return row->parent;
		}
		return "";
	};
	std::set<std::string> ancestors;
	for (std::string c = p_a; !c.empty(); c = parent(c)) {
		ancestors.insert(c);
	}
	for (std::string c = p_b; !c.empty(); c = parent(c)) {
		if (ancestors.count(c)) {
			return c;
		}
	}
	return "Node";
}

std::map<std::string, std::string> verse_gd_scene_node_types(const std::string &p_scene_text, const std::string &p_script_path,
		const VerseGdResourceReader &p_read) {
	std::map<std::string, std::string> out;
	const resource_text r = parse_resource(p_scene_text);
	const std::map<std::string, std::string> ext = ext_paths(r);
	const std::map<std::string, std::string> scripts = node_scripts(r, p_read);

	// Each node's type as `$` would see it: a script's `class_name` over the node's own class.
	std::map<std::string, std::string> types;
	std::set<std::string> unique;
	for (const section &s : r.sections) {
		if (s.tag != "node") {
			continue;
		}
		const std::string path = node_path(s);
		std::string type = s.get("type");
		if (const attribute *inst = s.find("instance")) {
			auto it = ext.find(ext_resource_id(inst->value));
			if (it != ext.end() && type.empty()) {
				type = scene_root(it->second, p_read).type;
			}
		}
		auto script = scripts.find(path);
		if (script != scripts.end() && p_read) {
			const std::string class_name = verse_gd_scan_class_name(p_read(script->second));
			if (!class_name.empty()) {
				type = class_name;
			}
		}
		types[path] = type.empty() ? "Node" : type;
		if (s.get("unique_name_in_owner") == "true") {
			unique.insert(path);
		}
	}
	for (const auto &owner : scripts) {
		if (owner.second != p_script_path) {
			continue;
		}
		const std::string prefix = owner.first == "." ? "" : owner.first + "/";
		for (const auto &node : types) {
			if (node.first == owner.first || node.first == ".") {
				continue;
			}
			if (!prefix.empty() && node.first.rfind(prefix, 0) != 0) {
				continue;
			}
			const std::string relative = node.first.substr(prefix.size());
			auto merge = [&](const std::string &key) {
				auto existing = out.find(key);
				out[key] = existing == out.end() ? node.second : verse_gd_common_class(existing->second, node.second);
			};
			merge(relative);
			if (unique.count(node.first)) {
				const size_t slash = node.first.find_last_of('/');
				merge("%" + (slash == std::string::npos ? node.first : node.first.substr(slash + 1)));
			}
		}
	}
	return out;
}

std::vector<std::string> verse_gd_resource_dependencies(const std::string &p_text) {
	std::vector<std::string> out;
	for (const auto &kv : ext_paths(parse_resource(p_text))) {
		out.push_back(kv.second);
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string verse_gd_rewrite_resource(const std::string &p_text, const std::vector<VerseGdScriptMove> &p_moves,
		const VerseGdResourceReader &p_read) {
	resource_text r = parse_resource(p_text);
	const std::map<std::string, std::string> ext = ext_paths(r);
	const std::map<std::string, std::string> scripts = node_scripts(r, p_read);
	bool changed = false;

	auto script_of_section = [&](const section &s) -> std::string {
		if (s.tag == "node") {
			auto it = scripts.find(node_path(s));
			return it == scripts.end() ? "" : it->second;
		}
		if (!s.script_id.empty()) {
			auto it = ext.find(s.script_id);
			return it == ext.end() ? "" : it->second;
		}
		return "";
	};

	for (section &s : r.sections) {
		std::string &header = r.lines[s.header_line];
		if (s.tag == "ext_resource") {
			const attribute *path = s.find("path");
			if (path) {
				if (const VerseGdScriptMove *m = move_for(p_moves, path->value)) {
					replace_attribute(header, *path, m->new_path);
					changed = true;
				}
			}
			continue;
		}
		if (s.tag == "connection") {
			// Right to left, so the earlier attributes' offsets stay good.
			std::vector<std::pair<const attribute *, std::string>> edits;
			auto from = scripts.find(normalise(s.get("from")));
			auto to = scripts.find(normalise(s.get("to")));
			if (from != scripts.end()) {
				if (const VerseGdScriptMove *m = move_for(p_moves, from->second)) {
					const std::string sig = renamed(*m, VerseGdRename::Kind::Signal, s.get("signal"));
					if (!sig.empty()) {
						edits.push_back({ s.find("signal"), sig });
					}
				}
			}
			if (to != scripts.end()) {
				if (const VerseGdScriptMove *m = move_for(p_moves, to->second)) {
					const std::string method = renamed(*m, VerseGdRename::Kind::Method, s.get("method"));
					if (!method.empty()) {
						edits.push_back({ s.find("method"), method });
					}
				}
			}
			std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return a.first->value_begin > b.first->value_begin; });
			for (const auto &e : edits) {
				replace_attribute(header, *e.first, e.second);
				changed = true;
			}
			continue;
		}
		const std::string script = script_of_section(s);
		const VerseGdScriptMove *m = script.empty() ? nullptr : move_for(p_moves, script);
		if (!m) {
			continue;
		}
		for (const auto &property : s.properties) {
			const std::string verse = renamed(*m, VerseGdRename::Kind::Property, property.second);
			if (!verse.empty()) {
				std::string &line = r.lines[property.first];
				line.replace(0, property.second.size(), verse);
				changed = true;
			}
		}
	}
	// A `.tres` names its script's global class in its header, and Godot reads that before the
	// script to decide what the resource is.
	if (!r.sections.empty() && r.sections[0].tag == "gd_resource") {
		const section &head = r.sections[0];
		const attribute *script_class = head.find("script_class");
		if (script_class) {
			for (const section &s : r.sections) {
				if (s.tag != "resource") {
					continue;
				}
				const VerseGdScriptMove *m = move_for(p_moves, script_of_section(s));
				if (m && m->old_class_name == script_class->value && !m->new_class_name.empty() && m->new_class_name != m->old_class_name) {
					replace_attribute(r.lines[head.header_line], *script_class, m->new_class_name);
					changed = true;
				}
			}
		}
	}
	return changed ? join_lines(r.lines) : p_text;
}

// --- Callers ------------------------------------------------------------------------------------

VerseGdCallerRewrite verse_gd_rewrite_callers(const std::string &p_source, const std::vector<VerseGdScriptMove> &p_moves,
		const std::map<std::string, std::string> &p_node_types) {
	using namespace verse_gd;
	VerseGdCallerRewrite out;
	out.text = p_source;
	const lex_result lexed = lex(p_source);
	if (!lexed.error.empty()) {
		return out;
	}
	const std::vector<token> &t = lexed.tokens;

	std::vector<std::string> lines;
	{
		size_t start = 0;
		while (start <= p_source.size()) {
			size_t end = p_source.find('\n', start);
			if (end == std::string::npos) {
				end = p_source.size();
			}
			lines.push_back(p_source.substr(start, end - start));
			start = end + 1;
		}
	}

	auto move_for_class = [&](const std::string &p_class) -> const VerseGdScriptMove * {
		for (const VerseGdScriptMove &m : p_moves) {
			if (!m.old_class_name.empty() && m.old_class_name == p_class) {
				return &m;
			}
		}
		return nullptr;
	};

	// Names declared with a converted class's type, anywhere in the file: `var p: Player`, a
	// parameter `p: Player`, `var p := x as Player`. A name declared with two different types is
	// not trusted.
	std::map<std::string, const VerseGdScriptMove *> typed;
	std::set<std::string> ambiguous;
	auto declare = [&](const std::string &p_name, const VerseGdScriptMove *p_move) {
		auto found = typed.find(p_name);
		if (found != typed.end() && found->second != p_move) {
			ambiguous.insert(p_name);
		}
		typed[p_name] = p_move;
	};
	for (size_t i = 0; i + 2 < t.size(); i++) {
		if (t[i].kind == tok::ident && t[i + 1].kind == tok::op && t[i + 1].text == ":" && t[i + 2].kind == tok::ident) {
			declare(t[i].text, move_for_class(t[i + 2].text));
		}
		if (t[i].kind == tok::ident && (t[i].text == "var" || t[i].text == "const") && t[i + 1].kind == tok::ident) {
			// `var p := $Player as Player` / `var p = Player.new()`
			for (size_t k = i + 2; k + 1 < t.size() && t[k].kind != tok::newline; k++) {
				if (t[k].kind == tok::ident && t[k].text == "as" && t[k + 1].kind == tok::ident && move_for_class(t[k + 1].text)) {
					declare(t[i + 1].text, move_for_class(t[k + 1].text));
				}
				if (t[k].kind == tok::ident && move_for_class(t[k].text) && k + 2 < t.size() && t[k + 1].text == "." && t[k + 2].text == "new") {
					declare(t[i + 1].text, move_for_class(t[k].text));
				}
			}
		}
	}

	std::set<std::string> renamed_names;
	for (const VerseGdScriptMove &m : p_moves) {
		for (const VerseGdRename &r : m.renames) {
			renamed_names.insert(r.gdscript);
		}
	}

	struct edit {
		int line;
		size_t col;
		size_t length;
		std::string text;
	};
	std::vector<edit> edits;
	std::map<int, VerseGdCallerSite> sites;
	auto site = [&](int p_line, bool p_rewritten, const std::string &p_reason) {
		VerseGdCallerSite &s = sites[p_line];
		s.line = p_line;
		std::string text = p_line >= 1 && p_line <= int(lines.size()) ? lines[size_t(p_line - 1)] : "";
		size_t a = 0;
		while (a < text.size() && std::isspace((unsigned char)text[a])) {
			a++;
		}
		s.source = text.substr(a);
		while (!s.source.empty() && (s.source.back() == '\r' || s.source.back() == ' ')) {
			s.source.pop_back();
		}
		// A line with one site rewritten and another not is reported as needing a look.
		if (!p_rewritten) {
			s.rewritten = false;
			if (s.reason.empty()) {
				s.reason = p_reason;
			}
		} else if (s.reason.empty()) {
			s.rewritten = true;
		}
	};

	for (size_t i = 0; i < t.size(); i++) {
		const token &k = t[i];
		const bool after_dot = i > 0 && t[i - 1].kind == tok::op && t[i - 1].text == ".";
		// A converted class's name, as a type or a value.
		if (k.kind == tok::ident && !after_dot) {
			if (const VerseGdScriptMove *m = move_for_class(k.text)) {
				if (!m->new_class_name.empty() && m->new_class_name != k.text) {
					edits.push_back({ k.line, size_t(k.col), k.text.size(), m->new_class_name });
					site(k.line, true, "");
				}
			}
		}
		// The old path in a string: `preload("res://player.gd")`.
		if (k.kind == tok::string) {
			if (const VerseGdScriptMove *m = move_for(p_moves, k.text)) {
				const std::string &line = lines[size_t(k.line - 1)];
				const size_t at = line.find(k.text, size_t(k.col));
				if (at != std::string::npos) {
					edits.push_back({ k.line, at, k.text.size(), m->new_path });
					site(k.line, true, "");
				}
				continue;
			}
			if (renamed_names.count(k.text)) {
				site(k.line, false, "`\"" + k.text + "\"` names a member by string -- `call`, `has_method`, `connect` or `set` -- and the converter cannot tell whose");
			}
		}
		if (!(after_dot && k.kind == tok::ident && renamed_names.count(k.text))) {
			continue;
		}
		// `receiver.member`: which receiver.
		const VerseGdScriptMove *owner = nullptr;
		bool known = false;
		if (i >= 2) {
			const token &r = t[i - 2];
			if (r.kind == tok::ident && typed.count(r.text) && !ambiguous.count(r.text) && !(i >= 3 && t[i - 3].text == ".")) {
				owner = typed[r.text];
				known = true;
			} else if (r.kind == tok::node_path || r.kind == tok::unique_node) {
				auto type = p_node_types.find(r.text);
				if (type != p_node_types.end()) {
					owner = move_for_class(type->second);
					known = true;
				}
			} else if (r.kind == tok::ident && move_for_class(r.text)) {
				// `Player.some_static()`
				owner = move_for_class(r.text);
				known = true;
			} else if (r.kind == tok::op && r.text == ")" && i >= 4 && t[i - 3].kind == tok::ident && move_for_class(t[i - 3].text)
					&& t[i - 4].kind == tok::ident && t[i - 4].text == "as") {
				// `($x as Player).start()`
				owner = move_for_class(t[i - 3].text);
				known = true;
			}
		}
		if (owner) {
			std::string verse;
			for (const VerseGdRename &r : owner->renames) {
				if (r.gdscript == k.text) {
					verse = r.verse;
				}
			}
			if (!verse.empty()) {
				edits.push_back({ k.line, size_t(k.col), k.text.size(), verse });
				site(k.line, true, "");
				continue;
			}
		}
		if (!known) {
			site(k.line, false, "`." + k.text + "` is reached through a value whose type this file does not say; if it is the converted script, the member is now `"
							+ verse_gd_member_name(k.text) + "`");
		}
	}

	std::sort(edits.begin(), edits.end(), [](const edit &a, const edit &b) { return a.line != b.line ? a.line > b.line : a.col > b.col; });
	for (const edit &e : edits) {
		std::string &line = lines[size_t(e.line - 1)];
		if (e.col + e.length <= line.size()) {
			line.replace(e.col, e.length, e.text);
			out.changed = true;
		}
	}
	if (out.changed) {
		out.text.clear();
		for (size_t i = 0; i < lines.size(); i++) {
			out.text += lines[i];
			if (i + 1 < lines.size()) {
				out.text += '\n';
			}
		}
	}
	for (auto &kv : sites) {
		out.sites.push_back(kv.second);
	}
	return out;
}
