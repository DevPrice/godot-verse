// Standalone driver for the top-level class scanner behind Godot's global-class registry. No
// Godot and no godot-cpp dependency, like the lexer test beside it.
#include "verse_class_decl.h"

#include <cstdio>
#include <string>

namespace {

bool Step(const char* Name, bool Result)
{
	printf("[verse_class_decl_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

bool TestPlainClass()
{
	const VerseClassDecl Decl = verse_scan_class_decl("using { /Godot.org/Godot }\n\nmover := class(node2d):\n\tReady<override>():void =\n\t\tPrint(\"hi\")\n");
	return Step("a plain class reports its name and base", Decl.name == "mover" && Decl.base == "node2d")
		&& Step("a plain class is not global", !Decl.is_global);
}

bool TestGlobalClass()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\nmover := class(node2d):\n");
	return Step("@global_class marks the class global", Decl.is_global && Decl.name == "mover");
}

bool TestAttributeAboveComment()
{
	// A comment between the attribute and the definition is ordinary Verse; the attribute still
	// applies to the class below it.
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\n# why this class exists\nmover := class(node2d):\n");
	return Step("a comment between attribute and class does not break the binding", Decl.is_global);
}

bool TestOtherAttributesIgnored()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@editable\n@global_class\nmover := class(node2d):\n");
	return Step("an unrelated attribute beside it is ignored", Decl.is_global && Decl.name == "mover");
}

bool TestAttributeInBlockComment()
{
	const VerseClassDecl Decl = verse_scan_class_decl("<#\n@global_class\n#>\nmover := class(node2d):\n");
	return Step("@global_class inside a block comment does not count", !Decl.is_global && Decl.name == "mover");
}

bool TestClassInBlockComment()
{
	// The commented-out class must not be the one reported, or the file registers under a name
	// that is not in it.
	const VerseClassDecl Decl = verse_scan_class_decl("<#\nold_mover := class(node3d):\n#>\nmover := class(node2d):\n");
	return Step("a class inside a block comment is skipped", Decl.name == "mover" && Decl.base == "node2d");
}

bool TestNestedBlockComment()
{
	const VerseClassDecl Decl = verse_scan_class_decl("<#\n<# inner #>\n@global_class\n#>\nmover := class(node2d):\n");
	return Step("a nested block comment closes at the right depth", !Decl.is_global && Decl.name == "mover");
}

bool TestLineComment()
{
	const VerseClassDecl Decl = verse_scan_class_decl("# @global_class\nmover := class(node2d):\n");
	return Step("@global_class in a line comment does not count", !Decl.is_global);
}

bool TestStringLiteralNotAClass()
{
	// The `:=` is real but the right side is a string, so there is no class on this line.
	const VerseClassDecl Decl = verse_scan_class_decl("banner := \"x := class(node2d):\"\nmover := class(node2d):\n");
	return Step("a string that looks like a class is not one", Decl.name == "mover" && Decl.base == "node2d");
}

bool TestIndentedClassSkipped()
{
	const VerseClassDecl Decl = verse_scan_class_decl("outer := class(node2d):\n\tinner := class(object):\n");
	return Step("only the column-zero class is reported", Decl.name == "outer" && Decl.base == "node2d");
}

bool TestNoBase()
{
	const VerseClassDecl Decl = verse_scan_class_decl("thing := class:\n\tX<public>:int = 1\n");
	return Step("a class with no supers reports an empty base", Decl.name == "thing" && Decl.base.empty());
}

bool TestBraceBody()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\nthing := class(object) {}\n");
	return Step("a brace-bodied class is recognised", Decl.is_global && Decl.name == "thing" && Decl.base == "object");
}

bool TestAbstract()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\nbase_thing := class<abstract>(node2d):\n");
	return Step("<abstract> is reported", Decl.is_abstract && Decl.base == "node2d" && Decl.is_global);
}

bool TestNotAbstract()
{
	const VerseClassDecl Decl = verse_scan_class_decl("thing := class<final>(node2d):\n");
	return Step("an unrelated specifier does not read as abstract", !Decl.is_abstract && Decl.base == "node2d");
}

bool TestMultipleSupers()
{
	const VerseClassDecl Decl = verse_scan_class_decl("thing := class(node2d, some_interface):\n");
	return Step("the first super is the base", Decl.base == "node2d");
}

bool TestModuleShapedFile()
{
	const VerseClassDecl Decl = verse_scan_class_decl("using { /Godot.org/Godot }\n\nReady<public>():void =\n\tPrint(\"hi\")\n");
	return Step("a file with no class reports no name", Decl.name.empty() && !Decl.is_global);
}

bool TestAttributeDoesNotLeakPastNonClass()
{
	// The attribute belongs to the alias, so the class below must not inherit it.
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\nmy_alias := node2d\nmover := class(node2d):\n");
	return Step("an attribute on a non-class definition does not leak", !Decl.is_global && Decl.name == "mover");
}

bool TestCrlf()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\r\nmover := class(node2d):\r\n");
	return Step("CRLF source scans the same", Decl.is_global && Decl.name == "mover" && Decl.base == "node2d");
}

bool TestSpacingVariants()
{
	const VerseClassDecl Decl = verse_scan_class_decl("mover:=class( node2d ):\n");
	return Step("tight and loose spacing both parse", Decl.name == "mover" && Decl.base == "node2d");
}

bool TestEmptySource()
{
	const VerseClassDecl Decl = verse_scan_class_decl("");
	return Step("empty source reports no class", Decl.name.empty() && !Decl.is_global);
}

bool TestPascalCase()
{
	return Step("a single word capitalises", verse_pascal_case("mover") == "Mover")
		&& Step("underscores join words", verse_pascal_case("player_controller") == "PlayerController")
		&& Step("three words join", verse_pascal_case("very_long_class_name") == "VeryLongClassName")
		&& Step("a trailing digit is kept", verse_pascal_case("enemy2") == "Enemy2")
		&& Step("a 2d suffix takes Godot's spelling", verse_pascal_case("enemy2d") == "Enemy2D")
		&& Step("a 3d suffix takes Godot's spelling", verse_pascal_case("boss3d") == "Boss3D")
		&& Step("the suffix rule applies per word", verse_pascal_case("enemy2d_spawner") == "Enemy2DSpawner")
		&& Step("a suffix already capital is left alone", verse_pascal_case("enemy2D") == "Enemy2D")
		&& Step("a d not behind a digit is untouched", verse_pascal_case("add") == "Add")
		&& Step("an ordinary word ending in d is untouched", verse_pascal_case("hud") == "Hud")
		&& Step("a trailing letter that is not d is untouched", verse_pascal_case("vector2i") == "Vector2i")
		&& Step("an already-capital word is left alone", verse_pascal_case("Mover") == "Mover")
		&& Step("a leading underscore does not produce an empty word", verse_pascal_case("_private") == "Private")
		&& Step("a doubled underscore does not produce an empty word", verse_pascal_case("a__b") == "AB")
		&& Step("a word starting with a digit is left alone", verse_pascal_case("2fast") == "2fast")
		&& Step("an empty name stays empty", verse_pascal_case("").empty());
}

bool TestSnakeCase()
{
	return Step("a single word lowercases", verse_snake_case("Mover") == "mover")
		&& Step("word boundaries join with an underscore", verse_snake_case("MyTestMethod") == "my_test_method")
		&& Step("a trailing digit is kept", verse_snake_case("Enemy2") == "enemy2")
		&& Step("a run of capitals is one word", verse_snake_case("GetHP") == "get_hp")
		&& Step("a run of capitals breaks before the last if lowercase follows", verse_snake_case("GetHPValue") == "get_hp_value")
		&& Step("already snake_case is left alone", verse_snake_case("already_snake") == "already_snake")
		&& Step("an empty name stays empty", verse_snake_case("").empty())
		&& Step("round-trips through verse_pascal_case for ordinary words", verse_pascal_case(verse_snake_case("MyTestMethod")) == "MyTestMethod");
}

bool TestDeclarationLine()
{
	// The row is what locates the comment block above the class, which is the class' own
	// documentation -- so it has to count the attributes and comments the scan walks past.
	const VerseClassDecl Decl = verse_scan_class_decl("using { /Godot.org/Godot }\n\n# what this is for\n@global_class\nmover := class(node2d):\n");
	return Step("the declaration reports the row it is on", Decl.line == 4)
		&& Step("a file with no class reports no row", verse_scan_class_decl("using { /Godot.org/Godot }\n").line == -1);
}

bool TestFileStemPicksTheClass()
{
	// Phase 2's derived_entity.verse already declared an interface, two structs, an enum and a
	// parametric class beside its own, so the scanner was already being asked more than it
	// promised. Modules make it matter: a file may declare any number of top-level names and only
	// the one named after the file is the script.
	const char* Source =
		"using { /Godot.org/Godot }\n"
		"\n"
		"helper := class(object):\n"
		"    X<public>:int = 1\n"
		"\n"
		"player := class(node2d):\n"
		"    Speed<public>:float = 1.0\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("the class named after the file is the one reported",
				Decl.name == "player" && Decl.base == "node2d")
		&& Step("and the first one in the file is not, when it is not that one",
				verse_scan_class_decl(Source).name == "helper");
}

bool TestFileStemWithNoMatch()
{
	const VerseClassDecl Decl = verse_scan_class_decl("helper := class(object):\n", "player");
	return Step("a file declaring no class of its own name reports none", Decl.name.empty() && Decl.line == -1);
}

bool TestAttributeBindsToTheNamedClass()
{
	// The attribute above a class that is *not* the file's must not carry over to the one that is.
	const char* Source =
		"@global_class\n"
		"helper := class(object):\n"
		"\n"
		"player := class(node2d):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("an attribute above another class does not reach this one",
				Decl.name == "player" && !Decl.is_global);
}

bool TestAttributeOnTheNamedClassStillBinds()
{
	const char* Source =
		"helper := class(object):\n"
		"\n"
		"@global_class\n"
		"player := class(node2d):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("and its own attribute still does", Decl.name == "player" && Decl.is_global);
}

bool TestToolAttribute()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@tool\nmover := class(node2d):\n");
	return Step("@tool marks the class a tool script", Decl.is_tool && Decl.name == "mover")
		&& Step("and an unmarked class is not one",
				!verse_scan_class_decl("mover := class(node2d):\n").is_tool);
}

bool TestToolAndGlobalTogether()
{
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\n@tool\nmover := class(node2d):\n");
	return Step("both attributes bind to the same class", Decl.is_tool && Decl.is_global);
}

// R-EXP-8's `@icon`, which is read here for the reason `@tool` is: Godot asks get_class_icon_path of
// a script it has merely scanned, from the filesystem thread, before anything has been built.

bool TestIconAttribute()
{
	const VerseClassDecl Decl =
		verse_scan_class_decl("@icon(\"res://art/player.svg\")\nplayer := class(node2d):\n");
	return Step("@icon carries its path", Decl.icon_path == "res://art/player.svg")
		&& Step("and a class without one carries none",
				verse_scan_class_decl("player := class(node2d):\n").icon_path.empty());
}

bool TestIconBindsToItsOwnClass()
{
	// The same rule every other attribute follows: it binds to the class below it, and a class that
	// is not the file's takes its own attributes with it.
	const char* Source =
		"@icon(\"res://art/helper.svg\")\n"
		"helper := class(node2d):\n"
		"\n"
		"@icon(\"res://art/player.svg\")\n"
		"player := class(node2d):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("an @icon on another class does not leak onto the file's",
			Decl.icon_path == "res://art/player.svg");
}

bool TestIconWithoutAPathIsEmpty()
{
	// An attribute's argument is evaluated by the compiler, and this is a text scan -- so a path
	// that is not a plain literal is one this cannot honestly read. Empty rather than a guess, and
	// Godot draws the base class's icon, which is what no `@icon` means too.
	return Step("a bare @icon carries nothing",
				verse_scan_class_decl("@icon\nplayer := class(node2d):\n").icon_path.empty())
		&& Step("and so does one whose path is not a literal",
				verse_scan_class_decl("@icon(IconPath)\nplayer := class(node2d):\n").icon_path.empty())
		&& Step("while a commented-out one is not seen at all",
				verse_scan_class_decl("# @icon(\"res://a.svg\")\nplayer := class(node2d):\n")
						.icon_path.empty());
}

// Godot collects one global class per script *path*, so `@global_class` on any class but the file's
// own is a request with nowhere to go. Each is reported so `_validate` can say so at the attribute
// instead of ignoring it silently. docs/property-export.md §"A second class in one file".

bool TestInertGlobalClassRecorded()
{
	const char* Source =
		"@global_class\n"
		"settings := class(resource):\n"
		"\n"
		"@global_class\n"
		"stowaway := class(resource):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "settings");
	return Step("the file's own @global_class still registers", Decl.name == "settings" && Decl.is_global)
		&& Step("a second one is reported as inert", Decl.inert_global_classes.size() == 1)
		&& Step("named, so the warning can name it", Decl.inert_global_classes[0].name == "stowaway")
		&& Step("and located at the attribute, not at the class",
				Decl.inert_global_classes[0].attribute_line == 3);
}

bool TestInertGlobalClassAboveTheScript()
{
	// The half that already worked: the attribute above another class never reached the file's.
	// What is new is that it is now reported rather than only withheld.
	const char* Source =
		"@global_class\n"
		"helper := class(object):\n"
		"\n"
		"player := class(node2d):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("an inert attribute above the file's class is reported too",
				Decl.inert_global_classes.size() == 1
					&& Decl.inert_global_classes[0].name == "helper"
					&& Decl.inert_global_classes[0].attribute_line == 0)
		&& Step("and the file's class is still not global", !Decl.is_global);
}

bool TestPlainSecondClassIsNotReported()
{
	// A second class is ordinary in Verse and in GDScript alike. Only the attribute is a request
	// that cannot be served, so only the attribute is reported.
	const char* Source =
		"player := class(node2d):\n"
		"\n"
		"helper := class(object):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("a second class with no attribute says nothing", Decl.inert_global_classes.empty());
}

bool TestSeveralInertGlobalClasses()
{
	// Below the file's own class, which is the half the scan could not see when it stopped at the
	// first match.
	const char* Source =
		"player := class(node2d):\n"
		"\n"
		"@global_class\n"
		"one := class(resource):\n"
		"\n"
		"@tool\n"
		"@global_class\n"
		"two := class(resource):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("every inert attribute is reported, in declaration order",
				Decl.inert_global_classes.size() == 2
					&& Decl.inert_global_classes[0].name == "one"
					&& Decl.inert_global_classes[1].name == "two")
		&& Step("each at its own attribute's line, past an unrelated attribute",
				Decl.inert_global_classes[0].attribute_line == 2
					&& Decl.inert_global_classes[1].attribute_line == 6);
}

bool TestInertGlobalClassInCommentIgnored()
{
	// The same lexer guarantee the file's own attribute has: a commented-out attribute is text.
	const char* Source =
		"player := class(node2d):\n"
		"\n"
		"# @global_class\n"
		"helper := class(object):\n";
	const VerseClassDecl Decl = verse_scan_class_decl(Source, "player");
	return Step("a commented-out @global_class is not reported", Decl.inert_global_classes.empty());
}

bool TestFileWithOnlyAnInertGlobalClass()
{
	// No class named after the file, so no script -- and the attribute is why the author thinks
	// there is one. The report has to survive `name` being empty.
	const VerseClassDecl Decl = verse_scan_class_decl("@global_class\nhelper := class(object):\n", "player");
	return Step("a file with no script of its own still reports the inert attribute",
				Decl.name.empty() && Decl.inert_global_classes.size() == 1
					&& Decl.inert_global_classes[0].name == "helper");
}

} // namespace

// The word a type's own name hovers with, which the ABI has nothing to answer: a type has no
// type to spell, and the lookup kind cannot separate a struct from a class.
bool TestTypeKeyword()
{
	const std::string Source =
			"using { /Godot.org/Godot }\n\n"
			"helper := class(node2d):\n\tAmount<public>:int = 1\n\n"
			"reading := struct:\n\tValue<public>:float = 0.0\n\n"
			"tempo := enum{Slow, Fast}\n\n"
			"named := interface:\n\tName<public>()<transacts>:string\n";
	return Step("a class reports class", verse_scan_type_keyword(Source, "helper") == "class")
		&& Step("a struct reports struct", verse_scan_type_keyword(Source, "reading") == "struct")
		&& Step("an enum reports enum", verse_scan_type_keyword(Source, "tempo") == "enum")
		&& Step("an interface reports interface", verse_scan_type_keyword(Source, "named") == "interface");
}

bool TestTypeKeywordPastWhatPrecedesTheAssignment()
{
	// The two things that sit between the name and the `:=`: an access specifier, which a
	// module written for @statics carries, and a parametric class's own parameters.
	return Step("an access specifier before the := is skipped",
			verse_scan_type_keyword("Statics<public> := module:\n\tX<public>:int = 1\n", "Statics") == "module")
		&& Step("a parametric class's parameters are skipped",
			verse_scan_type_keyword("box(t:type) := class:\n\tValue<public>:t\n", "box") == "class");
}

bool TestTypeKeywordRefusals()
{
	return Step("a member of a class is not a top-level type",
			verse_scan_type_keyword("mover := class(node2d):\n\tinner := class:\n", "inner").empty())
		&& Step("a function is not a type",
			verse_scan_type_keyword("Unbox<public>(B:box):int = 0\n", "Unbox").empty())
		&& Step("a name the source does not declare is empty",
			verse_scan_type_keyword("mover := class(node2d):\n", "widget").empty())
		&& Step("a class inside a block comment is not one",
			verse_scan_type_keyword("<#\nhelper := class:\n#>\n", "helper").empty());
}

int main()
{
	bool Ok = true;
	Ok = TestPlainClass() && Ok;
	Ok = TestGlobalClass() && Ok;
	Ok = TestAttributeAboveComment() && Ok;
	Ok = TestOtherAttributesIgnored() && Ok;
	Ok = TestAttributeInBlockComment() && Ok;
	Ok = TestClassInBlockComment() && Ok;
	Ok = TestNestedBlockComment() && Ok;
	Ok = TestLineComment() && Ok;
	Ok = TestStringLiteralNotAClass() && Ok;
	Ok = TestIndentedClassSkipped() && Ok;
	Ok = TestNoBase() && Ok;
	Ok = TestBraceBody() && Ok;
	Ok = TestAbstract() && Ok;
	Ok = TestNotAbstract() && Ok;
	Ok = TestMultipleSupers() && Ok;
	Ok = TestModuleShapedFile() && Ok;
	Ok = TestAttributeDoesNotLeakPastNonClass() && Ok;
	Ok = TestCrlf() && Ok;
	Ok = TestDeclarationLine() && Ok;
	Ok = TestSpacingVariants() && Ok;
	Ok = TestEmptySource() && Ok;
	Ok = TestPascalCase() && Ok;
	Ok = TestSnakeCase() && Ok;
	Ok = TestFileStemPicksTheClass() && Ok;
	Ok = TestFileStemWithNoMatch() && Ok;
	Ok = TestAttributeBindsToTheNamedClass() && Ok;
	Ok = TestAttributeOnTheNamedClassStillBinds() && Ok;
	Ok = TestToolAttribute() && Ok;
	Ok = TestToolAndGlobalTogether() && Ok;
	Ok = TestIconAttribute() && Ok;
	Ok = TestIconBindsToItsOwnClass() && Ok;
	Ok = TestIconWithoutAPathIsEmpty() && Ok;
	Ok = TestInertGlobalClassRecorded() && Ok;
	Ok = TestInertGlobalClassAboveTheScript() && Ok;
	Ok = TestPlainSecondClassIsNotReported() && Ok;
	Ok = TestSeveralInertGlobalClasses() && Ok;
	Ok = TestInertGlobalClassInCommentIgnored() && Ok;
	Ok = TestFileWithOnlyAnInertGlobalClass() && Ok;
	Ok = TestTypeKeyword() && Ok;
	Ok = TestTypeKeywordPastWhatPrecedesTheAssignment() && Ok;
	Ok = TestTypeKeywordRefusals() && Ok;

	printf("[verse_class_decl_test] %s\n", Ok ? "ALL PASS" : "FAILURES");
	return Ok ? 0 : 1;
}
