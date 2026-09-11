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

} // namespace

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

	printf("[verse_class_decl_test] %s\n", Ok ? "ALL PASS" : "FAILURES");
	return Ok ? 0 : 1;
}
