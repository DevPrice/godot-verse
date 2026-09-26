// Standalone differential test for the naming rules `src/verse_bindings.cpp` ports from
// `tools/gen_verse_api.py` (docs/architecture-review.md item 1 step 6). No Godot, no godot-cpp and
// no JSON parser: `naming_vectors.txt` is the same flat, tab-separated text `gen_verse_api.py`
// writes from `extension_api.json`, and this reads it and runs every case through the matching C++
// function, so a rule the two sides once only agreed on by inspection (`docs/generated-bindings.md`
// §10.9, "the differential test §4 promised is not built") now agrees by a test.
//
// One line per case; a `#` line and a blank line are both comments. The rule name is the first
// tab-separated field:
//
//   class_name          GodotName                  Expected
//   member_name         GodotName                  Expected
//   constant_name       GodotName                  Expected
//   enum_name           Owner        GodotEnum      Expected
//   enumerator_names    Owner.Enum   Name1,Name2    Expected1,Expected2
//   property_is_member  VerseType                   true|false
//   predicate           GodotName    Sibling        true|false
//
// `enumerator_names`'s two list fields are comma-joined; `predicate`'s sibling field is empty for
// no sibling. Every other field is a single token, because a Godot identifier is `[A-Za-z0-9_]`.
//
// Run from the repository root, or pass the vectors file as the first argument.
#include "verse_bindings.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

std::string g_path = "tests/verse_bindings/naming_vectors.txt";
int g_failures = 0;
int g_cases = 0;

std::vector<std::string> split(const std::string &p_text, char p_sep) {
	std::vector<std::string> fields;
	std::string cur;
	for (const char c : p_text) {
		if (c == p_sep) {
			fields.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	fields.push_back(cur);
	return fields;
}

std::string join(const std::vector<std::string> &p_items) {
	std::string out;
	for (size_t i = 0; i < p_items.size(); i++) {
		if (i > 0) {
			out += ",";
		}
		out += p_items[i];
	}
	return out;
}

/// One rule's running tally. A mismatch prints its own line immediately -- there are tens of
/// thousands of cases, so the summary is one line per rule rather than one per case, the way
/// `verse_gd_convert_test`'s golden diff is one line per fixture rather than per token.
struct RuleTally {
	const char *name;
	int total = 0;
	int failed = 0;
};

void report(const RuleTally &p_tally) {
	printf("[verse_bindings_test] %s (%d/%d rows): %s\n", p_tally.name, p_tally.total - p_tally.failed,
			p_tally.total, p_tally.failed == 0 ? "ok" : "FAIL");
}

void step(RuleTally &r_tally, const std::string &p_input, const std::string &p_got, const std::string &p_want) {
	r_tally.total++;
	g_cases++;
	if (p_got == p_want) {
		return;
	}
	r_tally.failed++;
	g_failures++;
	printf("[verse_bindings_test] %s %s: FAIL\n    want: %s\n    got:  %s\n", r_tally.name, p_input.c_str(),
			p_want.c_str(), p_got.c_str());
}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1) {
		g_path = argv[1];
	}

	std::ifstream file(g_path);
	if (!file.is_open()) {
		printf("[verse_bindings_test] could not open %s\n", g_path.c_str());
		return 1;
	}

	RuleTally class_name_tally{ "class_name" };
	RuleTally member_name_tally{ "member_name" };
	RuleTally constant_name_tally{ "constant_name" };
	RuleTally enum_name_tally{ "enum_name" };
	RuleTally enumerator_names_tally{ "enumerator_names" };
	RuleTally property_is_member_tally{ "property_is_member" };
	RuleTally predicate_tally{ "predicate" };

	int malformed = 0;
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const std::vector<std::string> f = split(line, '\t');
		const std::string &rule = f[0];

		if (rule == "class_name" && f.size() == 3) {
			step(class_name_tally, f[1], verse_binding_class_name(f[1]), f[2]);
		} else if (rule == "member_name" && f.size() == 3) {
			step(member_name_tally, f[1], verse_binding_member_name(f[1]), f[2]);
		} else if (rule == "constant_name" && f.size() == 3) {
			step(constant_name_tally, f[1], verse_binding_constant_name(f[1]), f[2]);
		} else if (rule == "enum_name" && f.size() == 4) {
			step(enum_name_tally, f[1] + "." + f[2], verse_binding_enum_name(f[1], f[2]), f[3]);
		} else if (rule == "enumerator_names" && f.size() == 4) {
			const std::string got = join(verse_binding_enumerator_names(split(f[2], ',')));
			step(enumerator_names_tally, f[1], got, f[3]);
		} else if (rule == "property_is_member" && f.size() == 3) {
			const std::string got = verse_binding_property_is_member(f[1]) ? "true" : "false";
			step(property_is_member_tally, f[1], got, f[2]);
		} else if (rule == "predicate" && f.size() == 4) {
			std::set<std::string> siblings;
			if (!f[2].empty()) {
				siblings.insert(f[2]);
			}
			const std::string got = verse_binding_is_predicate(f[1], siblings) ? "true" : "false";
			step(predicate_tally, f[2].empty() ? f[1] : f[1] + "+" + f[2], got, f[3]);
		} else {
			printf("[verse_bindings_test] unrecognized vector line: %s\n", line.c_str());
			malformed++;
		}
	}

	for (const RuleTally *tally : { &class_name_tally, &member_name_tally, &constant_name_tally,
				 &enum_name_tally, &enumerator_names_tally, &property_is_member_tally, &predicate_tally }) {
		report(*tally);
	}

	const bool ok = g_failures == 0 && malformed == 0 && g_cases > 0;
	printf("[verse_bindings_test] %d cases, %s\n", g_cases, ok ? "all checks passed" : "FAILURES");
	return ok ? 0 : 1;
}
