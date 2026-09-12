// Standalone driver for the source-path-to-module rule. No Godot and no godot-cpp, like the lexer
// and class-declaration tests beside it: every rule about which directory is a module is decidable
// from two lists of paths.
#include "verse_module_map.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool Step(const char* Name, bool Result)
{
	printf("[verse_module_map_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

std::string ModuleOf(const VerseModuleMap& Map, const std::string& Source)
{
	const auto Found = Map.module_by_source.find(Source);
	return Found == Map.module_by_source.end() ? std::string("<missing>") : Found->second;
}

bool TestNoMarkers()
{
	// The upgrade case, and the one that matters most: no project on disk has a .vmodule, so
	// nothing changes meaning when this lands.
	const VerseModuleMap Map = verse_build_module_map(
		{ "scripts/player.verse", "scripts/enemy/boss.verse", "main.verse" }, {});
	return Step("with no markers every file is in the root module",
				ModuleOf(Map, "scripts/player.verse").empty()
					&& ModuleOf(Map, "scripts/enemy/boss.verse").empty()
					&& ModuleOf(Map, "main.verse").empty())
		&& Step("and there are no modules at all", Map.directories_by_module.empty());
}

bool TestMarkerNamesTheModule()
{
	// my-stuff is not a Verse identifier and does not have to be: the marker names the module.
	const VerseModuleMap Map = verse_build_module_map(
		{ "my-stuff/player.verse" }, { "my-stuff/gameplay.vmodule" });
	return Step("the marker's stem names the module, not the directory",
				ModuleOf(Map, "my-stuff/player.verse") == "gameplay")
		&& Step("and a directory name that is no identifier is not a complaint", Map.diagnostics.empty());
}

bool TestUnmarkedDirectoriesAreOrganisational()
{
	const VerseModuleMap Map = verse_build_module_map(
		{ "gameplay/player.verse", "gameplay/enemies/boss.verse", "gameplay/enemies/minions/rat.verse" },
		{ "gameplay/gameplay.vmodule" });
	return Step("an unmarked subdirectory joins its nearest marked ancestor",
				ModuleOf(Map, "gameplay/enemies/boss.verse") == "gameplay")
		&& Step("however deep it is",
				ModuleOf(Map, "gameplay/enemies/minions/rat.verse") == "gameplay");
}

bool TestNearestAncestorWins()
{
	const VerseModuleMap Map = verse_build_module_map(
		{ "gameplay/player.verse", "gameplay/ai/brain.verse" },
		{ "gameplay/gameplay.vmodule", "gameplay/ai/ai.vmodule" });
	return Step("a nested marker takes the files below it",
				ModuleOf(Map, "gameplay/ai/brain.verse") == "gameplay/ai")
		&& Step("and leaves the ones above it alone",
				ModuleOf(Map, "gameplay/player.verse") == "gameplay");
}

bool TestNestedModulePathIsQualified()
{
	// The nested module's path carries its parent's, which is what makes `using` write
	// /user@localhost/gameplay/ai rather than /user@localhost/ai.
	const VerseModuleMap Map = verse_build_module_map(
		{ "a/b/c/deep.verse" },
		{ "a/outer.vmodule", "a/b/c/inner.vmodule" });
	return Step("a nested module's path is prefixed by the module it sits in",
				ModuleOf(Map, "a/b/c/deep.verse") == "outer/inner");
}

bool TestRootFileStaysAtRoot()
{
	const VerseModuleMap Map = verse_build_module_map(
		{ "main.verse", "gameplay/player.verse" }, { "gameplay/gameplay.vmodule" });
	return Step("a file beside the project root is still in the root module",
				ModuleOf(Map, "main.verse").empty());
}

bool TestTwoMarkersMergeIntoOneModule()
{
	// FindOrAddSubmodule is find-*or*-add, so one name in two places is one module spread across
	// two directories rather than a collision. Both directories have to be recoverable, because
	// the duplicate-definition diagnostic names them.
	const VerseModuleMap Map = verse_build_module_map(
		{ "a/one.verse", "b/two.verse" },
		{ "a/gameplay.vmodule", "b/gameplay.vmodule" });
	const auto Found = Map.directories_by_module.find("gameplay");
	return Step("two markers with the same name are one module",
				ModuleOf(Map, "a/one.verse") == "gameplay" && ModuleOf(Map, "b/two.verse") == "gameplay")
		&& Step("and the module knows both directories it came from",
				Found != Map.directories_by_module.end() && Found->second.size() == 2
					&& Found->second[0] == "a" && Found->second[1] == "b");
}

bool TestInvalidMarkerName()
{
	const VerseModuleMap Map = verse_build_module_map(
		{ "stuff/player.verse" }, { "stuff/my-stuff.vmodule" });
	const bool Reported = Map.diagnostics.size() == 1
		&& Map.diagnostics[0].marker_path == "stuff/my-stuff.vmodule"
		&& Map.diagnostics[0].message.find("my-stuff") != std::string::npos
		&& Map.diagnostics[0].message.find(".vmodule") != std::string::npos;
	return Step("a marker whose stem is not an identifier is reported by name", Reported)
		&& Step("and the message says what a legal one looks like",
				!Map.diagnostics.empty()
					&& Map.diagnostics[0].message.find("gameplay.vmodule") != std::string::npos)
		&& Step("while its scripts fall back to the module above rather than disappearing",
				ModuleOf(Map, "stuff/player.verse").empty());
}

bool TestDigitLeadingMarkerName()
{
	const VerseModuleMap Map = verse_build_module_map({ "2d/a.verse" }, { "2d/2d.vmodule" });
	return Step("a module name may not start with a digit",
				Map.diagnostics.size() == 1 && ModuleOf(Map, "2d/a.verse").empty());
}

bool TestValidNames()
{
	return Step("an underscore may lead", verse_is_valid_module_name("_private"))
		&& Step("digits may follow", verse_is_valid_module_name("ui2"))
		&& Step("an empty name is not one", !verse_is_valid_module_name(""))
		&& Step("nor is one with a dash", !verse_is_valid_module_name("my-stuff"))
		&& Step("nor one with a space", !verse_is_valid_module_name("my stuff"))
		&& Step("nor one with a dot", !verse_is_valid_module_name("my.stuff"));
}

bool TestResPrefixTolerated()
{
	// The caller is Godot, which spells paths res://-relative; the map should not care either way.
	const VerseModuleMap Map = verse_build_module_map(
		{ "res://gameplay/player.verse" }, { "res://gameplay/gameplay.vmodule" });
	return Step("a res:// prefix is stripped on both sides",
				ModuleOf(Map, "res://gameplay/player.verse") == "gameplay");
}

bool TestOrderIndependence()
{
	const VerseModuleMap Forward = verse_build_module_map(
		{ "a/b/x.verse" }, { "a/outer.vmodule", "a/b/inner.vmodule" });
	const VerseModuleMap Backward = verse_build_module_map(
		{ "a/b/x.verse" }, { "a/b/inner.vmodule", "a/outer.vmodule" });
	return Step("the answer does not depend on the order the markers arrive in",
				ModuleOf(Forward, "a/b/x.verse") == ModuleOf(Backward, "a/b/x.verse")
					&& ModuleOf(Forward, "a/b/x.verse") == "outer/inner");
}

} // namespace

int main()
{
	const bool Ok = TestNoMarkers()
		&& TestMarkerNamesTheModule()
		&& TestUnmarkedDirectoriesAreOrganisational()
		&& TestNearestAncestorWins()
		&& TestNestedModulePathIsQualified()
		&& TestRootFileStaysAtRoot()
		&& TestTwoMarkersMergeIntoOneModule()
		&& TestInvalidMarkerName()
		&& TestDigitLeadingMarkerName()
		&& TestValidNames()
		&& TestResPrefixTolerated()
		&& TestOrderIndependence();

	printf("[verse_module_map_test] %s\n", Ok ? "all checks passed" : "FAILURES");
	return Ok ? 0 : 1;
}
