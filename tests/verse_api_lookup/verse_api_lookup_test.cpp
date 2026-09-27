// Standalone test for the godot-cpp-free half of src/verse_api_lookup.cpp's class-name lookup
// (docs/architecture-review.md item 3 step 5): verse_api_godot_name_for and
// verse_api_verse_name_for, which verse_godot_class_for and mirrored_class wrap. No Godot, no
// godot-cpp -- verse_api::classes is data, so the round trip below sweeps every row rather than a
// sample, which is cheap because the table is generated.
//
// verse_godot_class_name, godot_classdb_class_for and the rest of verse_api_lookup.cpp are not
// covered here: they call into ClassDB or otherwise need a running Godot to answer anything, and
// splitting them out is not a thin wrapper the way these two are -- see the report for detail.
#include "verse_api_lookup_core.h"

#include <cstdio>
#include <iterator>
#include <string>

namespace {

int g_cases = 0;
int g_failures = 0;

void step(const std::string &p_case, bool p_ok, const std::string &p_detail = std::string()) {
	g_cases++;
	if (p_ok) {
		printf("[verse_api_lookup_test] %s: ok\n", p_case.c_str());
	} else {
		g_failures++;
		printf("[verse_api_lookup_test] %s: FAIL%s%s\n", p_case.c_str(),
				p_detail.empty() ? "" : " -- ", p_detail.c_str());
	}
}

} // namespace

int main() {
	// Every row of verse_api::classes round-trips both ways. One aggregate case rather than one per
	// row, the way verse_bindings_test's tallies are: there are over a thousand rows and a mismatch
	// prints its own line regardless.
	int mismatches = 0;
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		const std::string godot_name = verse_api::classes[i].godot_name;
		const std::string verse_name = verse_api::classes[i].verse_name;
		const char *forward = verse_api_godot_name_for(verse_name);
		const char *back = verse_api_verse_name_for(godot_name);
		const bool ok = forward != nullptr && godot_name == forward && back != nullptr && verse_name == back;
		if (!ok) {
			mismatches++;
			printf("[verse_api_lookup_test] round_trip %s/%s: FAIL (forward=%s, back=%s)\n",
					godot_name.c_str(), verse_name.c_str(),
					forward != nullptr ? forward : "<null>", back != nullptr ? back : "<null>");
		}
	}
	step("round_trip (" + std::to_string(std::size(verse_api::classes)) + " rows)", mismatches == 0);

	// The sixteen math types and RID (B28): verse_api::classes carries them beside the mirrored
	// classes and ClassDB has heard of none of them, which is exactly why this half of the lookup --
	// unlike godot_classdb_class_for -- needs no ClassDB to answer for them at all.
	static const char *const math_and_rid[] = {
		"AABB", "Basis", "Color", "Plane", "Projection", "Quaternion", "RID", "Rect2", "Rect2i",
		"Transform2D", "Transform3D", "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i"
	};
	for (const char *godot_name : math_and_rid) {
		const char *verse_name = verse_api_verse_name_for(godot_name);
		step(std::string("math_or_rid ") + godot_name, verse_name != nullptr);
		if (verse_name != nullptr) {
			const char *back = verse_api_godot_name_for(verse_name);
			step(std::string("math_or_rid_reverse ") + verse_name,
					back != nullptr && std::string(back) == godot_name,
					back != nullptr ? back : "<null>");
		}
	}

	// Names in neither direction, and the empty string, which is not a row either.
	step("unknown_godot_name", verse_api_verse_name_for("NoSuchGodotClass") == nullptr);
	step("unknown_verse_name", verse_api_godot_name_for("no_such_verse_class") == nullptr);
	step("empty_name", verse_api_godot_name_for("") == nullptr && verse_api_verse_name_for("") == nullptr);

	printf("[verse_api_lookup_test] %d cases, %s\n", g_cases,
			g_failures == 0 && g_cases > 0 ? "all checks passed" : "FAILURES");
	return g_failures == 0 && g_cases > 0 ? 0 : 1;
}
