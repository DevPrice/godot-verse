// Standalone driver for include/verse_diagnostics.h. No Godot, no godot-cpp and no UE: the registry
// and the rejection sentences are what both src/ and vm/ print, so they are tested once, here,
// against every enumerator the ABI defines. The registry's own consistency -- unique IDs, unique
// sentences, every ID emitted and documented -- is tests/verse_diagnostics/test_verse_diagnostics.py.
#include "verse_diagnostics.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

bool Step(const std::string &Name, bool Result, const std::string &Got = std::string()) {
	printf("[verse_diagnostics_test] %s: %s\n", Name.c_str(), Result ? "ok" : "FAIL");
	if (!Result) {
		g_failures++;
		if (!Got.empty()) {
			printf("    got: %s\n", Got.c_str());
		}
	}
	return Result;
}

bool StartsWith(const std::string &Text, const std::string &Prefix) {
	return Text.compare(0, Prefix.size(), Prefix) == 0;
}

bool Contains(const std::string &Text, const std::string &Part) {
	return Text.find(Part) != std::string::npos;
}

// A `{name}` left in the output is a placeholder the caller did not fill.
bool HasUnfilledPlaceholder(const std::string &Text) {
	for (size_t at = Text.find('{'); at != std::string::npos; at = Text.find('{', at + 1)) {
		size_t end = at + 1;
		while (end < Text.size() && ((Text[end] >= 'a' && Text[end] <= 'z') || Text[end] == '_')) {
			end++;
		}
		if (end > at + 1 && end < Text.size() && Text[end] == '}') {
			return true;
		}
	}
	return false;
}

// Carries its ID first, names the member it is about and fills every placeholder.
void ExpectSentence(const std::string &Name, const std::string &Got, const char *Id, const std::string &Names) {
	Step(Name, StartsWith(Got, std::string(Id) + ": ") && Contains(Got, Names) && !HasUnfilledPlaceholder(Got), Got);
}

} // namespace

int main() {
	Step("a row's ID is the enumerator's own name", verse_diag_row_of(verse_diag::VG1001).id == "VG1001");
	Step("the rows are in enumerator order",
			verse_diag_row_of(verse_diag::VG4004).id == "VG4004" && verse_diag_row_of(verse_diag::VG2006).id == "VG2006");

	const std::string filled = verse_diag_text(verse_diag::VG1002, { { "name", "Speed" } });
	Step("a placeholder is filled by name and the ID leads", StartsWith(filled, "VG1002: Speed is an option"), filled);
	const std::string unfilled = verse_diag_text(verse_diag::VG1002);
	Step("a placeholder with no argument stays visible", Contains(unfilled, "{name}"), unfilled);
	const std::string twice = verse_diag_text(verse_diag::VG1001, { { "name", "Target" }, { "class", "node2d" } });
	Step("a placeholder named twice is filled both times",
			Contains(twice, "is a node2d,") && Contains(twice, "`?node2d`") && !HasUnfilledPlaceholder(twice), twice);
	const std::string braced = verse_diag_text(verse_diag::VG1001, { { "name", "{class}" }, { "class", "x" } });
	Step("an argument is not itself scanned for placeholders", StartsWith(braced, "VG1001: {class} is a x,"), braced);

	struct ExportCase {
		vh_export_reject reject;
		const char *id;
	};
	const ExportCase exports[] = {
		{ VH_EXPORT_OBJECT_NOT_OPTIONAL, "VG1001" },
		{ VH_EXPORT_OPTION_NOT_OBJECT, "VG1002" },
		{ VH_EXPORT_HINT_WRONG_TYPE, "VG1003" },
		{ VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL, "VG1004" },
		{ VH_EXPORT_BINDING_CLASS_UNSUPPORTED, "VG1005" },
		{ VH_EXPORT_UNSUPPORTED_TYPE, "VG1007" },
		{ VH_EXPORT_OK, "VG1007" },
	};
	for (const ExportCase &c : exports) {
		ExpectSentence(std::string("export reject ") + std::to_string((int)c.reject) + " reads as " + c.id,
				verse_export_rejection(c.reject, VH_EXPORT_HINT_FLAGS, "Target", "mob", "Node2D"), c.id, "Target");
	}
	ExpectSentence("a binding class with no native base reads as VG1006",
			verse_export_rejection(VH_EXPORT_BINDING_CLASS_UNSUPPORTED, VH_EXPORT_HINT_NONE, "Target", "mob", ""), "VG1006", "mob");
	Step("a binding class with a native base names it",
			Contains(verse_export_rejection(VH_EXPORT_BINDING_CLASS_UNSUPPORTED, VH_EXPORT_HINT_NONE, "Target", "mob", "Node2D"),
					"`Node2D` instead"));
	Step("a flags hint wants an int",
			Contains(verse_export_rejection(VH_EXPORT_HINT_WRONG_TYPE, VH_EXPORT_HINT_FLAGS, "Mask", "", ""),
					"carries `@export_flags`, which describes an `int`"));
	Step("a file hint wants a string",
			Contains(verse_export_rejection(VH_EXPORT_HINT_WRONG_TYPE, VH_EXPORT_HINT_FILE, "Path", "", ""),
					"carries `@export_file`, which describes a `string`"));

	struct CodeCase {
		int32_t code;
		const char *id;
	};
	const CodeCase signals[] = {
		{ VH_SIGNAL_IS_VAR, "VG2001" },
		{ VH_SIGNAL_NOT_PUBLIC, "VG2002" },
		{ VH_SIGNAL_NO_GODOT_OWNER, "VG2003" },
		{ VH_SIGNAL_PAYLOAD_NESTED_STRUCT, "VG2004" },
		{ VH_SIGNAL_PAYLOAD_UNSUPPORTED, "VG2005" },
		{ VH_SIGNAL_NEEDS_ATTRIBUTE, "VG2006" },
		{ VH_SIGNAL_OK, "VG2007" },
	};
	for (const CodeCase &c : signals) {
		ExpectSentence(std::string("signal reject ") + std::to_string(c.code) + " reads as " + c.id,
				verse_signal_rejection("Scored", c.code, "Inner"), c.id, "Scored");
	}
	Step("a nested payload names its field",
			Contains(verse_signal_rejection("Scored", VH_SIGNAL_PAYLOAD_NESTED_STRUCT, "Inner"), "field `Inner`"));

	const CodeCase rpcs[] = {
		{ VH_RPC_UNKNOWN_ARGUMENT, "VG3001" },
		{ VH_RPC_DUPLICATE_CATEGORY, "VG3002" },
		{ VH_RPC_BAD_ARGUMENT_TYPE, "VG3003" },
		{ VH_RPC_OK, "VG3003" },
	};
	for (const CodeCase &c : rpcs) {
		ExpectSentence(std::string("rpc reject ") + std::to_string(c.code) + " reads as " + c.id,
				verse_rpc_rejection("Fire", c.code, "any_peers"), c.id, "Fire");
	}

	const CodeCase calls[] = {
		{ VH_CALL_DEAD_OBJECT, "VG4001" },
		{ VH_CALL_BAD_VALUE, "VG4002" },
		{ VH_CALL_BAD_ARITY, "VG4003" },
		{ VH_CALL_NO_SUCH_MEMBER, "VG4004" },
		{ VH_CALL_OK, "VG4004" },
	};
	for (const CodeCase &c : calls) {
		ExpectSentence(std::string("call status ") + std::to_string(c.code) + " reads as " + c.id,
				verse_call_failure("Called", "get_position", 7, c.code), c.id, "Called `get_position` on Godot object 7");
	}

	printf("[verse_diagnostics_test] %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
	return g_failures == 0 ? 0 : 1;
}
