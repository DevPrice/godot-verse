#include "verse_doc_markup.h"

#include <cstring>
#include <string_view>
#include <vector>

namespace {

bool is_word_char(char p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') ||
			(p_char >= '0' && p_char <= '9') || p_char == '_';
}

bool is_blank(std::string_view p_line) {
	return p_line.find_first_not_of(" \t") == std::string_view::npos;
}

std::string_view lstripped(std::string_view p_line) {
	const size_t first = p_line.find_first_not_of(" \t");
	return first == std::string_view::npos ? std::string_view() : p_line.substr(first);
}

// Markdown's indented code block: a tab or four spaces, after a blank line.
bool is_indented(std::string_view p_line) {
	return p_line.starts_with("\t") || p_line.starts_with("    ");
}

bool is_fence(std::string_view p_line) {
	return lstripped(p_line).starts_with("```");
}

// A bullet or a numbered item, which is kept on its own line rather than joined into the
// sentence before it. `[br]` is a line break within the paragraph, which is what Godot's own
// documentation uses for a list.
bool is_list_item(std::string_view p_line) {
	const std::string_view line = lstripped(p_line);
	if (line.starts_with("- ") || line.starts_with("* ") || line.starts_with("+ ")) {
		return true;
	}
	size_t digits = 0;
	while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
		digits++;
	}
	return digits > 0 && digits + 1 < line.size() && (line[digits] == '.' || line[digits] == ')') &&
			line[digits + 1] == ' ';
}

// Every tag `_add_text_to_rt` reads, so an author who writes Godot's own spelling gets Godot's
// own rendering. `[ClassName]` is not on it: whether a bare word is a class is a ClassDB question,
// and this unit has no Godot to ask.
const char *const GODOT_DOC_TAGS[] = {
	"b", "i", "u", "s", "code", "codeblock", "codeblocks", "gdscript", "csharp", "br", "lb", "rb",
	"kbd", "url", "center", "param", "method", "constructor", "operator", "member", "signal",
	"constant", "enum", "annotation", "theme_item", "img", "color", "font", "note", "warning",
	"important", "tip",
};

bool is_godot_doc_tag(std::string_view p_name) {
	for (const char *tag : GODOT_DOC_TAGS) {
		if (p_name == tag) {
			return true;
		}
	}
	return false;
}

// The name of the tag at p_line[p_open] -- `method` for `[method Node.add_child]`, `/b` for
// `[/b]` -- if Godot would read one there, else empty. An opening tag only where the bracket does
// not follow a word character: `Floor[X]` and `Items[i]` are Verse, not `[i]`. A closing tag
// follows the word it closes.
std::string_view godot_tag_at(std::string_view p_line, size_t p_open, size_t &r_close) {
	r_close = p_line.find(']', p_open + 1);
	if (r_close == std::string_view::npos) {
		return std::string_view();
	}
	std::string_view body = p_line.substr(p_open + 1, r_close - p_open - 1);
	const size_t name_end = body.find_first_of(" =");
	std::string_view name = name_end == std::string_view::npos ? body : body.substr(0, name_end);
	const bool closing = name.starts_with("/");
	if (closing) {
		name.remove_prefix(1);
	} else if (p_open > 0 && is_word_char(p_line[p_open - 1]) && name != "lb" && name != "rb" && name != "br") {
		return std::string_view();
	}
	if (name.empty() || !is_godot_doc_tag(name)) {
		return std::string_view();
	}
	return body.substr(0, name_end == std::string_view::npos ? body.size() : name_end);
}

// The `**` or `*` that closes an emphasis opened at p_from, or npos. Markdown's rule, as much of
// it as keeps `A * B` and `2*x` out of italics: the closer follows a non-space and precedes a
// non-word.
size_t emphasis_close(std::string_view p_line, size_t p_from, std::string_view p_marker) {
	size_t at = p_line.find(p_marker, p_from);
	while (at != std::string_view::npos) {
		const size_t after = at + p_marker.size();
		const bool follows_text = at > p_from && p_line[at - 1] != ' ' && p_line[at - 1] != '\t';
		const bool precedes_gap = after >= p_line.size() || !is_word_char(p_line[after]);
		if (follows_text && precedes_gap) {
			return at;
		}
		at = p_line.find(p_marker, at + 1);
	}
	return std::string_view::npos;
}

void append_escaped(std::string &r_out, char p_char) {
	if (p_char == '[') {
		r_out += "[lb]";
	} else if (p_char == ']') {
		r_out += "[rb]";
	} else {
		r_out += p_char;
	}
}

// One line of prose to BBCode. r_opens_codeblock is set when the line carries a `[codeblock]`
// of the author's own that this line does not close, so the caller copies the lines after it
// verbatim until one closes it.
std::string convert_inline(std::string_view p_line, bool &r_opens_codeblock) {
	std::string out;
	r_opens_codeblock = false;

	for (size_t i = 0; i < p_line.size();) {
		const char c = p_line[i];

		if (c == '`') {
			const size_t close = p_line.find('`', i + 1);
			if (close != std::string_view::npos) {
				out += "[code]";
				out.append(p_line.substr(i + 1, close - i - 1));
				out += "[/code]";
				i = close + 1;
				continue;
			}
		}

		if (c == '[') {
			size_t close = 0;
			const std::string_view tag = godot_tag_at(p_line, i, close);
			if (!tag.empty()) {
				out.append(p_line.substr(i, close - i + 1));
				i = close + 1;
				// Godot reads the inside of these raw, up to the closer, so escaping inside them
				// would print the escape.
				if (tag == "code" || tag == "kbd") {
					const std::string closer = "[/" + std::string(tag) + "]";
					const size_t end = p_line.find(closer, i);
					if (end != std::string_view::npos) {
						out.append(p_line.substr(i, end + closer.size() - i));
						i = end + closer.size();
					}
				} else if (tag == "codeblock") {
					const size_t end = p_line.find("[/codeblock]", i);
					if (end == std::string_view::npos) {
						out.append(p_line.substr(i));
						r_opens_codeblock = true;
						return out;
					}
					out.append(p_line.substr(i, end + std::strlen("[/codeblock]") - i));
					i = end + std::strlen("[/codeblock]");
				}
				continue;
			}
		}

		if (c == '*' && (i == 0 || !is_word_char(p_line[i - 1]))) {
			const bool strong = i + 1 < p_line.size() && p_line[i + 1] == '*';
			const std::string_view marker = strong ? "**" : "*";
			const size_t inner = i + marker.size();
			if (inner < p_line.size() && p_line[inner] != ' ' && p_line[inner] != '\t' && p_line[inner] != '*') {
				const size_t close = emphasis_close(p_line, inner, marker);
				if (close != std::string_view::npos) {
					bool ignored = false;
					out += strong ? "[b]" : "[i]";
					out += convert_inline(p_line.substr(inner, close - inner), ignored);
					out += strong ? "[/b]" : "[/i]";
					i = close + marker.size();
					continue;
				}
			}
		}

		append_escaped(out, c);
		i++;
	}
	return out;
}

// A code block, dedented by what its lines share, so the first line is not the only one Godot's
// strip_edges leaves flush.
void append_codeblock(std::string &r_out, std::vector<std::string> p_lines, const std::string &p_lang) {
	while (!p_lines.empty() && is_blank(p_lines.back())) {
		p_lines.pop_back();
	}

	size_t common = std::string::npos;
	const std::string *reference = nullptr;
	for (const std::string &line : p_lines) {
		if (is_blank(line)) {
			continue;
		}
		const size_t indent = line.find_first_not_of(" \t");
		if (reference == nullptr) {
			reference = &line;
			common = indent;
			continue;
		}
		size_t shared = 0;
		while (shared < common && shared < indent && line[shared] == (*reference)[shared]) {
			shared++;
		}
		common = shared;
	}
	if (common == std::string::npos) {
		common = 0;
	}

	if (!r_out.empty()) {
		r_out += "\n";
	}
	r_out += "[codeblock lang=" + p_lang + "]\n";
	for (size_t i = 0; i < p_lines.size(); i++) {
		if (i > 0) {
			r_out += "\n";
		}
		if (!is_blank(p_lines[i])) {
			r_out += p_lines[i].substr(common);
		}
	}
	r_out += "\n[/codeblock]";
}

std::string fence_language(std::string_view p_fence) {
	std::string_view lang = lstripped(p_fence).substr(3);
	lang = lstripped(lang);
	size_t end = 0;
	while (end < lang.size() && (is_word_char(lang[end]) || lang[end] == '+' || lang[end] == '-' || lang[end] == '#')) {
		end++;
	}
	return end == 0 ? std::string("verse") : std::string(lang.substr(0, end));
}

} // namespace

std::string verse_doc_to_bbcode(const std::string &p_doc) {
	std::vector<std::string_view> lines;
	{
		std::string_view rest = p_doc;
		while (!rest.empty()) {
			const size_t newline = rest.find('\n');
			std::string_view line = rest.substr(0, newline);
			if (line.ends_with("\r")) {
				line.remove_suffix(1);
			}
			lines.push_back(line);
			if (newline == std::string_view::npos) {
				break;
			}
			rest.remove_prefix(newline + 1);
		}
	}

	enum State { NORMAL, FENCE, INDENTED, BBCODE_BLOCK };
	State state = NORMAL;
	std::string out;
	std::vector<std::string> block;
	std::string block_lang;
	bool paragraph_break = false;
	bool after_block = false;

	for (std::string_view line : lines) {
		if (state == FENCE) {
			if (is_fence(line)) {
				append_codeblock(out, block, block_lang);
				block.clear();
				state = NORMAL;
				after_block = true;
				paragraph_break = false;
			} else {
				block.emplace_back(line);
			}
			continue;
		}

		if (state == BBCODE_BLOCK) {
			out += "\n";
			out.append(line);
			if (line.find("[/codeblock]") != std::string_view::npos) {
				state = NORMAL;
				after_block = true;
				paragraph_break = false;
			}
			continue;
		}

		if (state == INDENTED) {
			if (is_blank(line) || is_indented(line)) {
				block.emplace_back(line);
				continue;
			}
			append_codeblock(out, block, "verse");
			block.clear();
			state = NORMAL;
			after_block = true;
			paragraph_break = false;
		}

		if (is_fence(line)) {
			block_lang = fence_language(line);
			state = FENCE;
			continue;
		}
		if (is_indented(line) && (out.empty() || paragraph_break || after_block)) {
			block.emplace_back(line);
			state = INDENTED;
			continue;
		}
		if (is_blank(line)) {
			paragraph_break = !out.empty();
			continue;
		}

		bool opens_codeblock = false;
		const std::string converted = convert_inline(lstripped(line), opens_codeblock);
		if (!out.empty()) {
			if (paragraph_break || after_block || lstripped(line).starts_with("[codeblock")) {
				out += "\n";
			} else if (is_list_item(line)) {
				out += "[br]";
			} else {
				out += " ";
			}
		}
		out += converted;
		paragraph_break = false;
		after_block = false;
		if (opens_codeblock) {
			state = BBCODE_BLOCK;
		}
	}

	if (state == FENCE || state == INDENTED) {
		append_codeblock(out, block, state == FENCE ? block_lang : std::string("verse"));
	}
	return out;
}

std::string verse_doc_brief(const std::string &p_bbcode) {
	if (p_bbcode.starts_with("[codeblock")) {
		return std::string();
	}
	size_t from = 0;
	while (true) {
		const size_t newline = p_bbcode.find('\n', from);
		if (newline == std::string::npos) {
			return p_bbcode;
		}
		const size_t open = p_bbcode.rfind("[codeblock", newline);
		if (open != std::string::npos && open >= from) {
			const size_t close = p_bbcode.find("[/codeblock]", open);
			if (close == std::string::npos) {
				return p_bbcode;
			}
			from = close + std::strlen("[/codeblock]");
			continue;
		}
		return p_bbcode.substr(0, newline);
	}
}
