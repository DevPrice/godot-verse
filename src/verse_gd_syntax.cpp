#include "verse_gd_syntax.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

namespace verse_gd {

namespace {

bool is_ident_start(unsigned char c) {
	return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool is_ident_char(unsigned char c) {
	return std::isalnum(c) || c == '_' || c >= 0x80;
}

void append_utf8(std::string &r_out, unsigned long p_code) {
	if (p_code < 0x80) {
		r_out += char(p_code);
	} else if (p_code < 0x800) {
		r_out += char(0xC0 | (p_code >> 6));
		r_out += char(0x80 | (p_code & 0x3F));
	} else if (p_code < 0x10000) {
		r_out += char(0xE0 | (p_code >> 12));
		r_out += char(0x80 | ((p_code >> 6) & 0x3F));
		r_out += char(0x80 | (p_code & 0x3F));
	} else {
		r_out += char(0xF0 | (p_code >> 18));
		r_out += char(0x80 | ((p_code >> 12) & 0x3F));
		r_out += char(0x80 | ((p_code >> 6) & 0x3F));
		r_out += char(0x80 | (p_code & 0x3F));
	}
}

// Multi-character operators, longest first so `**=` is not read as `**` and `=`.
const char *const OPERATORS[] = {
	"**=", "<<=", ">>=", "...",
	"**", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
	"^=", "->", ":=", "..",
	"+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^", "~", "(", ")", "[", "]", "{", "}",
	",", ":", ".", ";",
};

class lexer {
public:
	explicit lexer(const std::string &p_source) :
			src(p_source) {}

	lex_result run() {
		indents.push_back(0);
		at_line_start = true;
		while (pos < src.size() && result.error.empty()) {
			if (at_line_start && depth() == 0) {
				if (!begin_line()) {
					continue;
				}
			}
			step();
		}
		if (!result.error.empty()) {
			return result;
		}
		if (!frames.empty() || !brackets.empty()) {
			fail("a bracket is never closed");
			return result;
		}
		if (!result.tokens.empty() && result.tokens.back().kind != tok::newline) {
			push(tok::newline, "");
		}
		while (indents.size() > 1) {
			indents.pop_back();
			push(tok::dedent, "");
		}
		push(tok::eof, "");
		return result;
	}

private:
	const std::string &src;
	size_t pos = 0;
	int line = 1;
	size_t line_start = 0;
	bool at_line_start = true;
	std::vector<int> indents;
	// The brackets currently open, innermost last, and for each whether a `func` keyword has been
	// seen inside it since -- which is what says a `:` then newline opens a lambda's body rather
	// than being an error.
	std::vector<std::pair<char, bool>> brackets;
	// A multi-line lambda inside brackets. Its body is indentation-sensitive although the
	// surrounding expression is not, so the bracket stack is set aside while it runs and restored
	// when a line dedents back to where the lambda began.
	struct frame {
		std::vector<std::pair<char, bool>> brackets;
		int base_indent;
		size_t outer_depth; // the indent stack before the frame, restored silently
		size_t body_depth; // where the body's own INDENTs start, each closed with a DEDENT
	};
	std::vector<frame> frames;
	lex_result result;

	size_t depth() const { return brackets.size(); }

	void fail(const std::string &p_message) {
		if (result.error.empty()) {
			result.error = p_message;
			result.error_line = line;
		}
	}

	void push(tok p_kind, const std::string &p_text, int p_col = -1) {
		token t;
		t.kind = p_kind;
		t.text = p_text;
		t.line = line;
		t.col = p_col < 0 ? int(pos - line_start) : p_col;
		result.tokens.push_back(t);
	}

	bool prev_is_operand() const {
		if (result.tokens.empty()) {
			return false;
		}
		const token &t = result.tokens.back();
		switch (t.kind) {
			case tok::number:
			case tok::string:
			case tok::node_path:
			case tok::unique_node:
				return true;
			case tok::ident:
				// A keyword that begins an expression is not an operand: `return %Label`.
				return !(t.text == "return" || t.text == "and" || t.text == "or" || t.text == "not"
						|| t.text == "in" || t.text == "if" || t.text == "elif" || t.text == "else"
						|| t.text == "while" || t.text == "await" || t.text == "match");
			case tok::op:
				return t.text == ")" || t.text == "]" || t.text == "}";
			default:
				return false;
		}
	}

	void newline_char() {
		pos++;
		line++;
		line_start = pos;
	}

	// Measures a line's indentation and emits INDENT/DEDENT. Answers false when the line was
	// blank or a comment and has been consumed whole.
	bool begin_line() {
		int width = 0;
		size_t p = pos;
		while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) {
			width += src[p] == '\t' ? 4 : 1;
			p++;
		}
		if (p >= src.size()) {
			pos = p;
			return false;
		}
		if (src[p] == '\n' || src[p] == '\r') {
			pos = p;
			if (src[p] == '\r' && p + 1 < src.size() && src[p + 1] == '\n') {
				pos++;
			}
			newline_char();
			return false;
		}
		if (src[p] == '#') {
			pos = p;
			read_comment(true);
			return false;
		}
		pos = p;
		at_line_start = false;

		// A lambda's body ends at the first line indented no deeper than the line it began on,
		// and what follows it -- usually `)` -- belongs to the bracket it was written inside.
		if (!frames.empty() && width <= frames.back().base_indent) {
			close_frame();
			return true;
		}

		if (width > indents.back()) {
			indents.push_back(width);
			push(tok::indent, "");
		} else {
			while (width < indents.back()) {
				indents.pop_back();
				push(tok::dedent, "");
			}
			if (width != indents.back()) {
				fail("an unindent does not match any outer indentation level");
			}
		}
		return true;
	}

	// Ends the innermost lambda body and hands the line back to the bracket it was written in.
	void close_frame() {
		while (indents.size() > frames.back().body_depth) {
			indents.pop_back();
			push(tok::dedent, "");
		}
		indents.resize(frames.back().outer_depth);
		brackets = frames.back().brackets;
		frames.pop_back();
	}

	void read_comment(bool p_own_line) {
		size_t start = pos + 1;
		if (start < src.size() && src[start] == '#') {
			start++;
		}
		size_t end = start;
		while (end < src.size() && src[end] != '\n' && src[end] != '\r') {
			end++;
		}
		comment c;
		c.line = line;
		c.text = src.substr(start, end - start);
		while (!c.text.empty() && (c.text.back() == ' ' || c.text.back() == '\t')) {
			c.text.pop_back();
		}
		c.own_line = p_own_line;
		result.comments.push_back(c);
		pos = end;
	}

	void end_of_line() {
		size_t p = pos;
		if (src[p] == '\r' && p + 1 < src.size() && src[p + 1] == '\n') {
			pos++;
		}
		const bool continues = depth() > 0;
		if (!continues) {
			// A newline is significant only once something was said on the line; the lexer has
			// already swallowed the blank ones.
			if (!result.tokens.empty() && result.tokens.back().kind != tok::newline
					&& result.tokens.back().kind != tok::indent && result.tokens.back().kind != tok::dedent) {
				push(tok::newline, "");
			}
			newline_char();
			at_line_start = true;
			return;
		}
		// Inside brackets. A `:` ending the line after a `func` in this bracket opens a lambda
		// body: the rest is indentation-sensitive until it dedents back.
		if (!result.tokens.empty() && result.tokens.back().kind == tok::op && result.tokens.back().text == ":"
				&& brackets.back().second) {
			int base = 0;
			for (size_t q = line_start; q < src.size() && (src[q] == ' ' || src[q] == '\t'); q++) {
				base += src[q] == '\t' ? 4 : 1;
			}
			frame f;
			f.brackets = brackets;
			f.base_indent = base;
			f.outer_depth = indents.size();
			// The lambda's statements are indented relative to the line it began on, which is not
			// necessarily the enclosing block's level; that level is pushed silently and popped
			// silently, because the parser never saw an INDENT for it.
			if (indents.back() != base) {
				indents.push_back(base);
			}
			f.body_depth = indents.size();
			frames.push_back(f);
			brackets.clear();
			push(tok::newline, "");
			newline_char();
			at_line_start = true;
			return;
		}
		newline_char();
	}

	void step() {
		const unsigned char c = src[pos];
		if (c == ' ' || c == '\t') {
			pos++;
			return;
		}
		if (c == '\n' || c == '\r') {
			end_of_line();
			return;
		}
		if (c == '\\' && pos + 1 < src.size() && (src[pos + 1] == '\n' || src[pos + 1] == '\r')) {
			pos++;
			if (src[pos] == '\r' && pos + 1 < src.size() && src[pos + 1] == '\n') {
				pos++;
			}
			newline_char();
			return;
		}
		if (c == '#') {
			read_comment(false);
			return;
		}
		if (c == '@' && pos + 1 < src.size() && is_ident_start(src[pos + 1])) {
			const int col = int(pos - line_start);
			size_t end = pos + 1;
			while (end < src.size() && is_ident_char(src[end])) {
				end++;
			}
			push(tok::annotation, src.substr(pos + 1, end - pos - 1), col);
			pos = end;
			return;
		}
		if (c == '$') {
			read_node_path();
			return;
		}
		if (c == '%' && !prev_is_operand() && pos + 1 < src.size() && is_ident_start(src[pos + 1])) {
			const int col = int(pos - line_start);
			size_t end = pos + 1;
			while (end < src.size() && is_ident_char(src[end])) {
				end++;
			}
			push(tok::unique_node, src.substr(pos, end - pos), col);
			pos = end;
			return;
		}
		if (std::isdigit(c) || (c == '.' && pos + 1 < src.size() && std::isdigit((unsigned char)src[pos + 1])
										&& !prev_is_operand())) {
			read_number();
			return;
		}
		if ((c == 'r' || c == '&' || c == '^') && pos + 1 < src.size() && (src[pos + 1] == '"' || src[pos + 1] == '\'')) {
			const int col = int(pos - line_start);
			pos++;
			read_string(char(c), col);
			return;
		}
		if (c == '"' || c == '\'') {
			read_string(0, int(pos - line_start));
			return;
		}
		if (is_ident_start(c)) {
			const int col = int(pos - line_start);
			size_t end = pos;
			while (end < src.size() && is_ident_char(src[end])) {
				end++;
			}
			const std::string word = src.substr(pos, end - pos);
			push(tok::ident, word, col);
			pos = end;
			if (word == "func" && !brackets.empty()) {
				brackets.back().second = true;
			}
			return;
		}
		for (const char *op : OPERATORS) {
			const size_t n = std::strlen(op);
			if (src.compare(pos, n, op) == 0) {
				const int col = int(pos - line_start);
				push(tok::op, op, col);
				pos += n;
				if (n == 1 && (op[0] == '(' || op[0] == '[' || op[0] == '{')) {
					brackets.push_back({ op[0], false });
				} else if (n == 1 && (op[0] == ')' || op[0] == ']' || op[0] == '}')) {
					if (brackets.empty() && !frames.empty()) {
						// `print(x))` -- the lambda's last line closes the call it was passed to.
						result.tokens.pop_back();
						push(tok::newline, "");
						close_frame();
						push(tok::op, op, col);
					}
					if (brackets.empty()) {
						fail(std::string("`") + op + "` closes nothing");
						return;
					}
					brackets.pop_back();
				}
				return;
			}
		}
		fail(std::string("unexpected character `") + char(c) + "`");
	}

	void read_node_path() {
		const int col = int(pos - line_start);
		pos++;
		if (pos < src.size() && (src[pos] == '"' || src[pos] == '\'')) {
			read_string(0, col);
			result.tokens.back().kind = tok::node_path;
			return;
		}
		size_t end = pos;
		// `$%Unique` is also a spelling of a unique-name lookup.
		if (end < src.size() && src[end] == '%') {
			end++;
		}
		while (end < src.size() && (is_ident_char(src[end]) || src[end] == '/')) {
			end++;
		}
		push(tok::node_path, src.substr(pos, end - pos), col);
		pos = end;
	}

	void read_number() {
		const int col = int(pos - line_start);
		size_t end = pos;
		bool is_float = false;
		if (src[end] == '0' && end + 1 < src.size() && (src[end + 1] == 'x' || src[end + 1] == 'X' || src[end + 1] == 'b' || src[end + 1] == 'B')) {
			end += 2;
			while (end < src.size() && (std::isxdigit((unsigned char)src[end]) || src[end] == '_')) {
				end++;
			}
		} else {
			while (end < src.size() && (std::isdigit((unsigned char)src[end]) || src[end] == '_')) {
				end++;
			}
			if (end < src.size() && src[end] == '.' && !(end + 1 < src.size() && (src[end + 1] == '.' || is_ident_start(src[end + 1])))) {
				is_float = true;
				end++;
				while (end < src.size() && (std::isdigit((unsigned char)src[end]) || src[end] == '_')) {
					end++;
				}
			}
			if (end < src.size() && (src[end] == 'e' || src[end] == 'E')) {
				size_t e = end + 1;
				if (e < src.size() && (src[e] == '+' || src[e] == '-')) {
					e++;
				}
				if (e < src.size() && std::isdigit((unsigned char)src[e])) {
					is_float = true;
					end = e;
					while (end < src.size() && std::isdigit((unsigned char)src[end])) {
						end++;
					}
				}
			}
		}
		std::string text;
		for (size_t i = pos; i < end; i++) {
			if (src[i] != '_') {
				text += src[i];
			}
		}
		push(tok::number, text, col);
		result.tokens.back().is_float = is_float;
		pos = end;
	}

	void read_string(char p_prefix, int p_col) {
		const char quote = src[pos];
		const bool triple = src.compare(pos, 3, std::string(3, quote)) == 0;
		pos += triple ? 3 : 1;
		const int start_line = line;
		std::string value;
		while (true) {
			if (pos >= src.size()) {
				fail("a string is never closed");
				return;
			}
			const char c = src[pos];
			if (triple ? src.compare(pos, 3, std::string(3, quote)) == 0 : c == quote) {
				pos += triple ? 3 : 1;
				break;
			}
			if (c == '\n') {
				if (!triple) {
					fail("a string is never closed");
					return;
				}
				value += '\n';
				newline_char();
				continue;
			}
			if (c == '\\' && p_prefix != 'r' && pos + 1 < src.size()) {
				const char e = src[pos + 1];
				pos += 2;
				switch (e) {
					case 'n': value += '\n'; break;
					case 't': value += '\t'; break;
					case 'r': value += '\r'; break;
					case 'a': value += '\a'; break;
					case 'b': value += '\b'; break;
					case 'f': value += '\f'; break;
					case 'v': value += '\v'; break;
					case '0': value += '\0'; break;
					case 'u':
					case 'U': {
						const size_t digits = e == 'u' ? 4 : 6;
						const unsigned long code = std::strtoul(src.substr(pos, digits).c_str(), nullptr, 16);
						pos += digits;
						append_utf8(value, code);
						break;
					}
					case '\n':
						newline_char();
						break;
					default: value += e; break;
				}
				continue;
			}
			value += c;
			pos++;
		}
		token t;
		t.kind = tok::string;
		t.text = value;
		t.line = start_line;
		t.col = p_col;
		t.string_prefix = p_prefix;
		result.tokens.push_back(t);
	}
};

// --- Parser ---------------------------------------------------------------------------------

class parser {
public:
	explicit parser(std::vector<token> &&p_tokens) :
			toks(std::move(p_tokens)) {}

	class_ptr parse_file() {
		auto root = std::make_shared<class_decl>();
		root->line = 1;
		skip_newlines();
		parse_class_body(*root, true);
		if (!at(tok::eof) && error.empty()) {
			fail("unexpected `" + peek().text + "`");
		}
		return root;
	}

	std::string error;
	int error_line = 0;

private:
	std::vector<token> toks;
	size_t i = 0;

	const token &peek(size_t p_ahead = 0) const {
		const size_t k = i + p_ahead;
		return toks[k < toks.size() ? k : toks.size() - 1];
	}
	bool at(tok p_kind) const { return peek().kind == p_kind; }
	bool at_op(const char *p_op) const { return peek().kind == tok::op && peek().text == p_op; }
	bool at_word(const char *p_word) const { return peek().kind == tok::ident && peek().text == p_word; }
	const token &next() {
		const token &t = peek();
		if (i < toks.size()) {
			i++;
		}
		return t;
	}
	// The line of the last token that was part of what was just parsed -- not a DEDENT, which
	// carries the line after it, or the blank lines a trailing comment sits on.
	int last_line() const {
		for (size_t k = i; k > 0; k--) {
			const token &t = toks[k - 1];
			if (t.kind != tok::dedent && t.kind != tok::indent && t.kind != tok::newline) {
				return t.line;
			}
		}
		return 1;
	}

	void fail(const std::string &p_message) {
		if (error.empty()) {
			error = p_message;
			error_line = peek().line;
		}
		// Stop consuming: every loop in the parser checks `error`.
		i = toks.size() - 1;
	}

	bool accept_op(const char *p_op) {
		if (at_op(p_op)) {
			next();
			return true;
		}
		return false;
	}
	bool accept_word(const char *p_word) {
		if (at_word(p_word)) {
			next();
			return true;
		}
		return false;
	}
	void expect_op(const char *p_op) {
		if (!accept_op(p_op)) {
			fail(std::string("expected `") + p_op + "` but found `" + peek().text + "`");
		}
	}
	std::string expect_ident() {
		if (!at(tok::ident)) {
			fail("expected a name but found `" + peek().text + "`");
			return "";
		}
		return next().text;
	}
	void skip_newlines() {
		while (at(tok::newline)) {
			next();
		}
	}
	void end_statement() {
		// After a multi-line lambda the block's DEDENT already ended the line; and a one-line
		// lambda body inside a call ends at the bracket or comma that follows it.
		if (i > 0 && (toks[i - 1].kind == tok::dedent || toks[i - 1].kind == tok::newline)) {
			return;
		}
		if (at_op(")") || at_op("]") || at_op("}") || at_op(",")) {
			return;
		}
		if (accept_op(";")) {
			// `a; b` on one line: the next statement follows directly.
			return;
		}
		if (at(tok::newline)) {
			next();
			return;
		}
		if (at(tok::dedent) || at(tok::eof)) {
			return;
		}
		fail("expected the end of the line but found `" + peek().text + "`");
	}

	// A type as written: `int`, `Array[int]`, `Dictionary[String, Node]`, `Foo.Bar`.
	std::string parse_type() {
		std::string t = expect_ident();
		while (accept_op(".")) {
			t += "." + expect_ident();
		}
		if (accept_op("[")) {
			t += "[" + parse_type();
			while (accept_op(",")) {
				t += "," + parse_type();
			}
			expect_op("]");
			t += "]";
		}
		return t;
	}

	std::vector<annotation> parse_annotations() {
		std::vector<annotation> out;
		while (at(tok::annotation) && error.empty()) {
			annotation a;
			a.line = peek().line;
			a.name = next().text;
			if (at_op("(")) {
				next();
				while (!at_op(")") && error.empty()) {
					a.args.push_back(parse_expr());
					if (!accept_op(",")) {
						break;
					}
				}
				expect_op(")");
			}
			out.push_back(a);
			// An annotation may sit on a line of its own above what it annotates.
			skip_newlines();
		}
		return out;
	}

	void parse_class_body(class_decl &r_class, bool p_top_level) {
		while (error.empty() && !at(tok::eof) && !at(tok::dedent)) {
			skip_newlines();
			if (at(tok::eof) || at(tok::dedent)) {
				break;
			}
			std::vector<annotation> annotations = parse_annotations();
			if (at(tok::eof) || at(tok::dedent)) {
				// Class-level annotations with nothing after them: `@tool` alone in a file.
				r_class.annotations.insert(r_class.annotations.end(), annotations.begin(), annotations.end());
				break;
			}
			const int line = peek().line;
			if (accept_word("extends")) {
				r_class.annotations.insert(r_class.annotations.end(), annotations.begin(), annotations.end());
				if (at(tok::string)) {
					r_class.extends = next().text;
					r_class.extends_is_path = true;
				} else {
					r_class.extends = parse_type();
				}
				end_statement();
				continue;
			}
			if (accept_word("class_name")) {
				r_class.annotations.insert(r_class.annotations.end(), annotations.begin(), annotations.end());
				r_class.name = expect_ident();
				if (accept_word("extends")) {
					if (at(tok::string)) {
						r_class.extends = next().text;
						r_class.extends_is_path = true;
					} else {
						r_class.extends = parse_type();
					}
				}
				end_statement();
				continue;
			}
			// A class annotation (`@tool`, `@icon`) is followed by the class's own statements, and
			// sorting it from a member annotation needs the name.
			std::vector<annotation> member_annotations;
			for (const annotation &a : annotations) {
				if (a.name == "tool" || a.name == "icon" || a.name == "abstract" || a.name == "static_unload") {
					r_class.annotations.push_back(a);
				} else {
					member_annotations.push_back(a);
				}
			}
			member m;
			m.line = line;
			bool is_static = accept_word("static");
			if (at_word("func")) {
				m.kind = member::kind::func;
				m.func = parse_func(true);
				m.func->is_static = is_static;
				m.func->annotations = member_annotations;
			} else if (at_word("var") || at_word("const")) {
				m.kind = member::kind::var;
				m.var = parse_member_var();
				m.var->is_static = is_static;
				m.var->annotations = member_annotations;
			} else if (accept_word("signal")) {
				m.kind = member::kind::signal;
				m.signal = std::make_shared<signal_decl>();
				m.signal->line = line;
				m.signal->name = expect_ident();
				m.signal->annotations = member_annotations;
				if (accept_op("(")) {
					m.signal->params = parse_params();
				}
				end_statement();
			} else if (accept_word("enum")) {
				m.kind = member::kind::enum_;
				m.enum_ = parse_enum(line);
			} else if (accept_word("class")) {
				m.kind = member::kind::inner_class;
				m.inner = std::make_shared<class_decl>();
				m.inner->line = line;
				m.inner->name = expect_ident();
				if (accept_word("extends")) {
					if (at(tok::string)) {
						m.inner->extends = next().text;
						m.inner->extends_is_path = true;
					} else {
						m.inner->extends = parse_type();
					}
				}
				expect_op(":");
				if (at(tok::newline)) {
					next();
				}
				if (!at(tok::indent)) {
					fail("expected an indented class body");
					break;
				}
				next();
				parse_class_body(*m.inner, false);
				m.inner->end_line = last_line();
				if (!accept_dedent()) {
					break;
				}
			} else if (accept_word("pass")) {
				end_statement();
				continue;
			} else {
				fail("expected a member declaration but found `" + peek().text + "`");
				break;
			}
			r_class.members.push_back(m);
		}
		(void)p_top_level;
	}

	bool accept_dedent() {
		if (at(tok::dedent)) {
			next();
			return true;
		}
		if (at(tok::eof)) {
			return true;
		}
		fail("expected the end of the block but found `" + peek().text + "`");
		return false;
	}

	std::shared_ptr<enum_decl> parse_enum(int p_line) {
		auto e = std::make_shared<enum_decl>();
		e->line = p_line;
		if (at(tok::ident)) {
			e->name = next().text;
		}
		expect_op("{");
		while (!at_op("}") && error.empty()) {
			std::string name = expect_ident();
			expr_ptr value;
			if (accept_op("=")) {
				value = parse_expr();
			}
			e->values.push_back({ name, value });
			if (!accept_op(",")) {
				break;
			}
		}
		expect_op("}");
		e->end_line = last_line();
		end_statement();
		return e;
	}

	std::vector<param> parse_params() {
		std::vector<param> out;
		while (!at_op(")") && error.empty()) {
			param p;
			p.name = expect_ident();
			if (accept_op(":=")) {
				p.type_text = "=";
				p.default_value = parse_expr();
			} else {
				if (accept_op(":")) {
					p.type_text = parse_type();
				}
				if (accept_op("=")) {
					p.default_value = parse_expr();
				}
			}
			out.push_back(p);
			if (!accept_op(",")) {
				break;
			}
		}
		expect_op(")");
		return out;
	}

	func_ptr parse_func(bool p_named) {
		auto f = std::make_shared<func_decl>();
		f->line = peek().line;
		next(); // func
		if (p_named || at(tok::ident)) {
			if (at(tok::ident)) {
				f->name = next().text;
			} else if (p_named) {
				fail("expected a function name");
			}
		}
		expect_op("(");
		f->params = parse_params();
		if (accept_op("->")) {
			f->return_type = parse_type();
		}
		expect_op(":");
		f->body = parse_block();
		f->end_line = last_line();
		return f;
	}

	std::shared_ptr<var_decl> parse_member_var() {
		auto v = std::make_shared<var_decl>();
		v->line = peek().line;
		v->is_const = next().text == "const";
		v->name = expect_ident();
		bool block_accessors = false;
		if (accept_op(":=")) {
			v->type_text = "=";
			v->init = parse_expr();
		} else {
			if (accept_op(":")) {
				// `var x: int:` opens the get/set block with no initialiser; `var x:` alone opens it
				// untyped, which Godot also accepts.
				if (at(tok::newline)) {
					block_accessors = true;
				} else {
					v->type_text = parse_type();
				}
			}
			if (!block_accessors && accept_op("=")) {
				v->init = parse_expr();
			}
		}
		if (!block_accessors && accept_op(":")) {
			block_accessors = true;
		}
		if (block_accessors) {
			parse_accessor_block(*v);
		} else if (accept_word("setget")) {
			if (at(tok::ident)) {
				v->setget_setter = next().text;
			}
			if (accept_op(",")) {
				v->setget_getter = expect_ident();
			}
			end_statement();
		} else {
			end_statement();
		}
		v->end_line = last_line();
		return v;
	}

	// `get:` / `get = name`, `set(value):` / `set = name`, inside the indented block after a var.
	void parse_accessor_block(var_decl &r_var) {
		if (!at(tok::newline)) {
			fail("expected the accessor block on the next line");
			return;
		}
		next();
		if (!at(tok::indent)) {
			fail("expected an indented accessor block");
			return;
		}
		next();
		while (error.empty() && !at(tok::dedent) && !at(tok::eof)) {
			skip_newlines();
			if (at(tok::dedent)) {
				break;
			}
			const std::string which = expect_ident();
			if (which != "get" && which != "set") {
				fail("expected `get` or `set` in the accessor block");
				return;
			}
			auto f = std::make_shared<func_decl>();
			f->line = last_line();
			if (accept_op("=")) {
				const std::string target = expect_ident();
				if (which == "get") {
					r_var.setget_getter = target;
				} else {
					r_var.setget_setter = target;
				}
				end_statement();
				continue;
			}
			if (which == "set") {
				expect_op("(");
				f->params = parse_params();
			}
			expect_op(":");
			f->body = parse_block();
			f->end_line = last_line();
			(which == "get" ? r_var.getter : r_var.setter) = f;
		}
		accept_dedent();
	}

	std::vector<stmt_ptr> parse_block() {
		std::vector<stmt_ptr> body;
		if (!at(tok::newline)) {
			// A one-line body: `if x: return`. Several may be joined with `;`.
			while (error.empty()) {
				body.push_back(parse_statement());
				// Only a `;` keeps the one-line body going; end_statement consumed it, or the
				// newline that ended the body.
				if (!(toks[i - 1].kind == tok::op && toks[i - 1].text == ";") || at(tok::newline)) {
					break;
				}
			}
			if (at(tok::newline)) {
				next();
			}
			return body;
		}
		next();
		skip_newlines();
		if (!at(tok::indent)) {
			fail("expected an indented block");
			return body;
		}
		next();
		while (error.empty() && !at(tok::dedent) && !at(tok::eof)) {
			skip_newlines();
			if (at(tok::dedent) || at(tok::eof)) {
				break;
			}
			body.push_back(parse_statement());
		}
		accept_dedent();
		return body;
	}

	stmt_ptr parse_statement() {
		auto s = std::make_shared<stmt>();
		s->line = peek().line;
		// A statement annotation (`@warning_ignore(...)`) says nothing a conversion keeps.
		while (at(tok::annotation)) {
			parse_annotations();
		}
		s->line = peek().line;
		if (accept_word("pass")) {
			s->kind = sk::pass;
			end_statement();
		} else if (accept_word("break")) {
			s->kind = sk::break_;
			end_statement();
		} else if (accept_word("continue")) {
			s->kind = sk::continue_;
			end_statement();
		} else if (accept_word("breakpoint")) {
			s->kind = sk::breakpoint;
			end_statement();
		} else if (accept_word("return")) {
			s->kind = sk::return_;
			if (!at(tok::newline) && !at(tok::dedent) && !at(tok::eof) && !at_op(";") && !at_op(")")) {
				s->args.push_back(parse_expr());
			}
			end_statement();
		} else if (at_word("var") || at_word("const")) {
			s->kind = sk::var;
			s->is_const = next().text == "const";
			s->text = expect_ident();
			if (accept_op(":=")) {
				s->type_text = "=";
				s->args.push_back(parse_expr());
			} else {
				if (accept_op(":")) {
					s->type_text = parse_type();
				}
				if (accept_op("=")) {
					s->args.push_back(parse_expr());
				}
			}
			end_statement();
		} else if (accept_word("if")) {
			s->kind = sk::if_;
			if_branch b;
			b.line = s->line;
			b.condition = parse_expr();
			expect_op(":");
			b.body = parse_block();
			s->branches.push_back(b);
			while (error.empty()) {
				skip_newlines_if_followed_by_else();
				if (at_word("elif")) {
					if_branch e;
					e.line = next().line;
					e.condition = parse_expr();
					expect_op(":");
					e.body = parse_block();
					s->branches.push_back(e);
				} else if (at_word("else")) {
					if_branch e;
					e.line = next().line;
					expect_op(":");
					e.body = parse_block();
					s->branches.push_back(e);
					break;
				} else {
					break;
				}
			}
		} else if (accept_word("while")) {
			s->kind = sk::while_;
			s->args.push_back(parse_expr());
			expect_op(":");
			s->body = parse_block();
		} else if (accept_word("for")) {
			s->kind = sk::for_;
			s->for_var = expect_ident();
			if (accept_op(":")) {
				s->for_type = parse_type();
			}
			if (!accept_word("in")) {
				fail("expected `in` in a for loop");
			}
			s->args.push_back(parse_expr());
			expect_op(":");
			s->body = parse_block();
		} else if (accept_word("match")) {
			s->kind = sk::match;
			s->args.push_back(parse_expr());
			expect_op(":");
			parse_match_body(*s);
		} else {
			expr_ptr e = parse_expr();
			static const std::set<std::string> assign_ops = { "=", "+=", "-=", "*=", "/=", "%=", "**=", "&=", "|=", "^=", "<<=", ">>=" };
			if (peek().kind == tok::op && assign_ops.count(peek().text)) {
				s->kind = sk::assign;
				s->text = next().text;
				s->args.push_back(e);
				s->args.push_back(parse_expr());
			} else {
				s->kind = sk::expr;
				s->args.push_back(e);
			}
			end_statement();
		}
		s->end_line = last_line();
		return s;
	}

	// `if`'s `elif`/`else` follow the dedent that ended the previous branch.
	void skip_newlines_if_followed_by_else() {
		size_t k = i;
		while (k < toks.size() && toks[k].kind == tok::newline) {
			k++;
		}
		if (k < toks.size() && toks[k].kind == tok::ident && (toks[k].text == "elif" || toks[k].text == "else")) {
			i = k;
		}
	}

	void parse_match_body(stmt &r_stmt) {
		if (!at(tok::newline)) {
			fail("expected the match branches on the next line");
			return;
		}
		next();
		if (!at(tok::indent)) {
			fail("expected indented match branches");
			return;
		}
		next();
		while (error.empty() && !at(tok::dedent) && !at(tok::eof)) {
			skip_newlines();
			if (at(tok::dedent)) {
				break;
			}
			match_branch b;
			b.line = peek().line;
			while (error.empty()) {
				if (at_word("var")) {
					next();
					auto binding = std::make_shared<expr>();
					binding->kind = ek::opaque;
					binding->text = "var " + expect_ident();
					binding->line = last_line();
					b.patterns.push_back(binding);
				} else {
					b.patterns.push_back(parse_expr());
				}
				if (!accept_op(",")) {
					break;
				}
			}
			if (accept_word("when")) {
				b.guard = parse_expr();
			}
			expect_op(":");
			b.body = parse_block();
			r_stmt.cases.push_back(b);
		}
		accept_dedent();
	}

	// --- Expressions, lowest precedence first (GDScript's own table) ---

	expr_ptr make(ek p_kind, const std::string &p_text, int p_line) {
		auto e = std::make_shared<expr>();
		e->kind = p_kind;
		e->text = p_text;
		e->line = p_line;
		e->end_line = p_line;
		return e;
	}
	expr_ptr make_binary(const std::string &p_op, expr_ptr p_left, expr_ptr p_right) {
		auto e = make(ek::binary, p_op, p_left ? p_left->line : last_line());
		e->args = { p_left, p_right };
		e->end_line = last_line();
		return e;
	}

	expr_ptr parse_expr() { return parse_as(); }

	expr_ptr parse_as() {
		expr_ptr e = parse_ternary();
		while (at_word("as") && error.empty()) {
			next();
			auto c = make(ek::cast, "as", e->line);
			c->args = { e };
			c->type_text = parse_type();
			c->end_line = last_line();
			e = c;
		}
		return e;
	}

	expr_ptr parse_ternary() {
		expr_ptr e = parse_or();
		if (at_word("if") && error.empty()) {
			next();
			expr_ptr condition = parse_or();
			if (!accept_word("else")) {
				fail("expected `else` in a conditional expression");
				return e;
			}
			expr_ptr otherwise = parse_ternary();
			auto t = make(ek::ternary, "if", e->line);
			t->args = { e, condition, otherwise };
			t->end_line = last_line();
			return t;
		}
		return e;
	}

	expr_ptr parse_or() {
		expr_ptr e = parse_and();
		while ((at_word("or") || at_op("||")) && error.empty()) {
			next();
			e = make_binary("or", e, parse_and());
		}
		return e;
	}

	expr_ptr parse_and() {
		expr_ptr e = parse_not();
		while ((at_word("and") || at_op("&&")) && error.empty()) {
			next();
			e = make_binary("and", e, parse_not());
		}
		return e;
	}

	expr_ptr parse_not() {
		if ((at_word("not") || at_op("!")) && error.empty()) {
			const int line = next().line;
			auto u = make(ek::unary, "not", line);
			u->args = { parse_not() };
			u->end_line = last_line();
			return u;
		}
		return parse_in();
	}

	expr_ptr parse_in() {
		expr_ptr e = parse_comparison();
		while (error.empty()) {
			if (at_word("in")) {
				next();
				e = make_binary("in", e, parse_comparison());
			} else if (at_word("not") && peek(1).kind == tok::ident && peek(1).text == "in") {
				next();
				next();
				e = make_binary("not in", e, parse_comparison());
			} else {
				break;
			}
		}
		return e;
	}

	expr_ptr parse_comparison() {
		expr_ptr e = parse_bit_or();
		while (error.empty() && peek().kind == tok::op
				&& (peek().text == "==" || peek().text == "!=" || peek().text == "<" || peek().text == ">"
						|| peek().text == "<=" || peek().text == ">=")) {
			const std::string op = next().text;
			e = make_binary(op, e, parse_bit_or());
		}
		return e;
	}

	expr_ptr parse_bit_or() {
		expr_ptr e = parse_bit_xor();
		while (at_op("|") && error.empty()) {
			next();
			e = make_binary("|", e, parse_bit_xor());
		}
		return e;
	}
	expr_ptr parse_bit_xor() {
		expr_ptr e = parse_bit_and();
		while (at_op("^") && error.empty()) {
			next();
			e = make_binary("^", e, parse_bit_and());
		}
		return e;
	}
	expr_ptr parse_bit_and() {
		expr_ptr e = parse_shift();
		while (at_op("&") && error.empty()) {
			next();
			e = make_binary("&", e, parse_shift());
		}
		return e;
	}
	expr_ptr parse_shift() {
		expr_ptr e = parse_additive();
		while ((at_op("<<") || at_op(">>")) && error.empty()) {
			const std::string op = next().text;
			e = make_binary(op, e, parse_additive());
		}
		return e;
	}
	expr_ptr parse_additive() {
		expr_ptr e = parse_multiplicative();
		while ((at_op("+") || at_op("-")) && error.empty()) {
			const std::string op = next().text;
			e = make_binary(op, e, parse_multiplicative());
		}
		return e;
	}
	expr_ptr parse_multiplicative() {
		expr_ptr e = parse_unary();
		while ((at_op("*") || at_op("/") || at_op("%")) && error.empty()) {
			const std::string op = next().text;
			e = make_binary(op, e, parse_unary());
		}
		return e;
	}
	expr_ptr parse_unary() {
		if ((at_op("-") || at_op("+") || at_op("~")) && error.empty()) {
			const token &t = next();
			auto u = make(ek::unary, t.text, t.line);
			u->args = { parse_unary() };
			u->end_line = last_line();
			return u;
		}
		return parse_power();
	}
	expr_ptr parse_power() {
		expr_ptr e = parse_is();
		if (at_op("**") && error.empty()) {
			next();
			// Right-associative, and binds tighter than a unary minus on its right.
			e = make_binary("**", e, parse_unary());
		}
		return e;
	}
	expr_ptr parse_is() {
		expr_ptr e = parse_postfix();
		while (at_word("is") && error.empty()) {
			next();
			const bool negated = accept_word("not");
			auto c = make(ek::binary, negated ? "is not" : "is", e->line);
			c->args = { e };
			c->type_text = parse_type();
			c->end_line = last_line();
			e = c;
		}
		return e;
	}

	expr_ptr parse_postfix() {
		if (at_word("await")) {
			const int line = next().line;
			auto a = make(ek::await_, "await", line);
			a->args = { parse_postfix() };
			a->end_line = last_line();
			return a;
		}
		expr_ptr e = parse_primary();
		while (error.empty()) {
			if (at_op(".")) {
				next();
				auto a = make(ek::attribute, expect_ident(), e->line);
				a->args = { e };
				a->end_line = last_line();
				e = a;
			} else if (at_op("(")) {
				next();
				auto c = make(ek::call, "", e->line);
				c->args.push_back(e);
				while (!at_op(")") && error.empty()) {
					c->args.push_back(parse_expr());
					if (!accept_op(",")) {
						break;
					}
				}
				expect_op(")");
				c->end_line = last_line();
				e = c;
			} else if (at_op("[")) {
				next();
				auto s = make(ek::subscript, "", e->line);
				s->args = { e, parse_expr() };
				expect_op("]");
				s->end_line = last_line();
				e = s;
			} else {
				break;
			}
		}
		return e;
	}

	expr_ptr parse_primary() {
		const token &t = peek();
		const int line = t.line;
		switch (t.kind) {
			case tok::number: {
				next();
				auto e = make(ek::number, t.text, line);
				e->is_float = t.is_float;
				return e;
			}
			case tok::string: {
				next();
				auto e = make(ek::string, t.text, line);
				e->string_prefix = t.string_prefix;
				return e;
			}
			case tok::node_path:
			case tok::unique_node: {
				next();
				return make(ek::node_path, t.text, line);
			}
			case tok::ident: {
				if (t.text == "true" || t.text == "false") {
					next();
					return make(ek::boolean, t.text, line);
				}
				if (t.text == "null") {
					next();
					return make(ek::null, "null", line);
				}
				if (t.text == "self") {
					next();
					return make(ek::self, "self", line);
				}
				if (t.text == "super") {
					next();
					return make(ek::super_, "super", line);
				}
				if (t.text == "func") {
					auto e = make(ek::lambda, "func", line);
					e->lambda = parse_func(false);
					e->end_line = last_line();
					return e;
				}
				next();
				return make(ek::ident, t.text, line);
			}
			case tok::op: {
				if (t.text == "(") {
					next();
					expr_ptr e = parse_expr();
					expect_op(")");
					return e;
				}
				if (t.text == "[") {
					next();
					auto e = make(ek::array, "", line);
					skip_layout();
					while (!at_op("]") && error.empty()) {
						e->args.push_back(parse_expr());
						skip_layout();
						if (!accept_op(",")) {
							break;
						}
						skip_layout();
					}
					expect_op("]");
					e->end_line = last_line();
					return e;
				}
				if (t.text == "{") {
					next();
					auto e = make(ek::dictionary, "", line);
					while (!at_op("}") && error.empty()) {
						// Lua style, `{a = 1}`, keys a bare name as a string.
						if (at(tok::ident) && peek(1).kind == tok::op && peek(1).text == "=") {
							auto key = make(ek::string, next().text, line);
							next();
							e->args.push_back(key);
						} else {
							e->args.push_back(parse_expr());
							expect_op(":");
						}
						e->args.push_back(parse_expr());
						if (!accept_op(",")) {
							break;
						}
					}
					expect_op("}");
					e->end_line = last_line();
					return e;
				}
				break;
			}
			default:
				break;
		}
		fail("expected an expression but found `" + (t.kind == tok::newline ? std::string("end of line") : t.text) + "`");
		return make(ek::opaque, "", line);
	}

	// Inside brackets a multi-line lambda's frame leaves newline/indent tokens behind it.
	void skip_layout() {
		while (at(tok::newline) || at(tok::indent) || at(tok::dedent)) {
			next();
		}
	}
};

std::string quote(const std::string &p_text) {
	std::string out = "\"";
	for (char c : p_text) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\t') {
			out += "\\t";
		} else {
			out += c;
		}
	}
	return out + "\"";
}

} // namespace

lex_result lex(const std::string &p_source) {
	return lexer(p_source).run();
}

parse_result parse(const std::string &p_source) {
	parse_result result;
	lex_result lexed = lex(p_source);
	result.comments = lexed.comments;
	if (!lexed.error.empty()) {
		result.error = lexed.error;
		result.error_line = lexed.error_line;
		return result;
	}
	parser p(std::move(lexed.tokens));
	result.root = p.parse_file();
	result.error = p.error;
	result.error_line = p.error_line;
	return result;
}

std::string to_source(const expr_ptr &p_expr) {
	if (!p_expr) {
		return "";
	}
	const expr &e = *p_expr;
	auto arg = [&](size_t k) { return k < e.args.size() ? to_source(e.args[k]) : std::string(); };
	switch (e.kind) {
		case ek::number:
		case ek::ident:
		case ek::opaque:
			return e.text;
		case ek::string:
			return (e.string_prefix && e.string_prefix != 'r' ? std::string(1, e.string_prefix) : std::string()) + quote(e.text);
		case ek::boolean:
			return e.text;
		case ek::null:
			return "null";
		case ek::self:
			return "self";
		case ek::super_:
			return "super";
		case ek::node_path:
			return e.text[0] == '%' ? e.text : "$" + e.text;
		case ek::array: {
			std::string out = "[";
			for (size_t k = 0; k < e.args.size(); k++) {
				out += (k ? ", " : "") + arg(k);
			}
			return out + "]";
		}
		case ek::dictionary: {
			std::string out = "{";
			for (size_t k = 0; k + 1 < e.args.size(); k += 2) {
				out += (k ? ", " : "") + arg(k) + ": " + arg(k + 1);
			}
			return out + "}";
		}
		case ek::unary:
			return e.text == "not" ? "not " + arg(0) : e.text + arg(0);
		case ek::binary:
			if (e.text == "is" || e.text == "is not") {
				return arg(0) + " " + e.text + " " + e.type_text;
			}
			return arg(0) + " " + e.text + " " + arg(1);
		case ek::ternary:
			return arg(0) + " if " + arg(1) + " else " + arg(2);
		case ek::call: {
			std::string out = arg(0) + "(";
			for (size_t k = 1; k < e.args.size(); k++) {
				out += (k > 1 ? ", " : "") + arg(k);
			}
			return out + ")";
		}
		case ek::attribute:
			return arg(0) + "." + e.text;
		case ek::subscript:
			return arg(0) + "[" + arg(1) + "]";
		case ek::cast:
			return arg(0) + " as " + e.type_text;
		case ek::await_:
			return "await " + arg(0);
		case ek::lambda: {
			std::string out = "func(";
			if (e.lambda) {
				for (size_t k = 0; k < e.lambda->params.size(); k++) {
					out += (k ? ", " : "") + e.lambda->params[k].name;
				}
			}
			return out + "): ...";
		}
		case ek::get_node_call:
			return e.text;
	}
	return e.text;
}

} // namespace verse_gd
