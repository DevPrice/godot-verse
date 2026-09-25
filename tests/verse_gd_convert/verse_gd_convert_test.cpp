// Standalone driver for the GDScript-to-Verse converter (docs/gdscript-conversion.md). No Godot,
// no godot-cpp and no UE: the converter is pure, and everything it knows about the mirror comes from
// src/verse_gd_api.gen.h.
//
// Two kinds of case. A *golden* case converts a fixture under fixtures/ and compares the whole
// output -- the Verse and the notes -- with the `.expected` files beside it, because what the
// converter writes is read by a person and every line of it is the behaviour. `--update` rewrites
// the expectations; read the diff before committing one. The other cases are the rules stated
// once each: names, determinism, refusals, and the scene and caller rewrites.
//
// Run from the repository root, or pass the fixtures directory as the first argument.
#include "verse_gd_convert.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#endif

namespace {

int g_failures = 0;
bool g_update = false;
std::string g_dir = "tests/verse_gd_convert/fixtures";

bool Step(const std::string &Name, bool Result) {
	printf("[verse_gd_convert_test] %s: %s\n", Name.c_str(), Result ? "ok" : "FAIL");
	if (!Result) {
		g_failures++;
	}
	return Result;
}

bool Expect(const std::string &Name, const std::string &Got, const std::string &Want) {
	const bool Ok = Got == Want;
	if (!Ok) {
		printf("    want: %s\n    got:  %s\n", Want.c_str(), Got.c_str());
	}
	return Step(Name, Ok);
}

std::string Slurp(const std::string &Path) {
	std::ifstream File(Path, std::ios::binary);
	std::stringstream Out;
	Out << File.rdbuf();
	return Out.str();
}

bool Exists(const std::string &Path) {
	std::ifstream File(Path);
	return File.good();
}

void Spit(const std::string &Path, const std::string &Text) {
	std::ofstream File(Path, std::ios::binary);
	File << Text;
}

std::vector<std::string> ListFixtures() {
	std::vector<std::string> Names;
#if defined(_WIN32)
	_finddata_t Data;
	intptr_t Handle = _findfirst((g_dir + "/*.gd").c_str(), &Data);
	if (Handle != -1) {
		do {
			Names.push_back(Data.name);
		} while (_findnext(Handle, &Data) == 0);
		_findclose(Handle);
	}
#else
	if (DIR *Dir = opendir(g_dir.c_str())) {
		while (dirent *Entry = readdir(Dir)) {
			const std::string Name = Entry->d_name;
			if (Name.size() > 3 && Name.compare(Name.size() - 3, 3, ".gd") == 0) {
				Names.push_back(Name);
			}
		}
		closedir(Dir);
	}
#endif
	std::sort(Names.begin(), Names.end());
	for (std::string &Name : Names) {
		Name = Name.substr(0, Name.size() - 3);
	}
	return Names;
}

std::vector<std::string> Lines(const std::string &Text) {
	std::vector<std::string> Out;
	std::istringstream In(Text);
	std::string Line;
	while (std::getline(In, Line)) {
		if (!Line.empty() && Line.back() == '\r') {
			Line.pop_back();
		}
		if (!Line.empty()) {
			Out.push_back(Line);
		}
	}
	return Out;
}

// A fixture is `name.gd`, and optionally `name.nodes` (`$path=Type` per line) and `name.classes`
// (`ClassName=verse_name|GodotBase|res://path` per line), which stand in for what the editor reads
// out of the project's scenes and global class list.
VerseGdConvertInput LoadInput(const std::string &Name) {
	VerseGdConvertInput In;
	In.source = Slurp(g_dir + "/" + Name + ".gd");
	In.file_stem = Name;
	for (const std::string &Line : Lines(Slurp(g_dir + "/" + Name + ".nodes"))) {
		const size_t Eq = Line.find('=');
		In.node_types[Line.substr(0, Eq)] = Line.substr(Eq + 1);
	}
	for (const std::string &Line : Lines(Slurp(g_dir + "/" + Name + ".classes"))) {
		const size_t Eq = Line.find('=');
		std::string Rest = Line.substr(Eq + 1);
		VerseGdScriptClass Class;
		Class.verse_name = Rest.substr(0, Rest.find('|'));
		Rest = Rest.substr(Rest.find('|') + 1);
		Class.godot_base = Rest.substr(0, Rest.find('|'));
		Class.path = Rest.substr(Rest.find('|') + 1);
		In.script_classes[Line.substr(0, Eq)] = Class;
	}
	return In;
}

std::string RenderNotes(const VerseGdConvertResult &Result) {
	std::string Out;
	if (!Result.ok) {
		return "error " + std::to_string(Result.error_line) + ": " + Result.error + "\n";
	}
	Out += "stem " + Result.verse_stem + (Result.global_name.empty() ? "" : ", registered as " + Result.global_name) + "\n";
	for (const VerseGdNote &Note : Result.notes) {
		Out += std::to_string(Note.line) + (Note.todo ? " TODO " : " note ") + Note.message + "\n";
	}
	for (const VerseGdRename &Rename : Result.renames) {
		const char *Kind = Rename.kind == VerseGdRename::Kind::Property ? "property" : Rename.kind == VerseGdRename::Kind::Method ? "method" : "signal";
		Out += std::string("rename ") + Kind + " " + Rename.gdscript + " -> " + Rename.verse + "\n";
	}
	return Out;
}

void Golden(const std::string &Label, const std::string &Stem, const VerseGdConvertResult &Result) {
	const std::string VersePath = g_dir + "/" + Stem + ".verse.expected";
	const std::string NotesPath = g_dir + "/" + Stem + ".notes.expected";
	const std::string Notes = RenderNotes(Result);
	if (g_update) {
		Spit(VersePath, Result.verse);
		Spit(NotesPath, Notes);
		Step(Label + " (expectation rewritten)", true);
		return;
	}
	if (!Exists(VersePath)) {
		Step(Label + ": no " + VersePath + " -- run with --update and read what it writes", false);
		return;
	}
	const bool VerseOk = Result.verse == Slurp(VersePath);
	const bool NotesOk = Notes == Slurp(NotesPath);
	if (!VerseOk) {
		printf("    %s differs from %s; run with --update to see the diff\n", Label.c_str(), VersePath.c_str());
	}
	if (!NotesOk) {
		printf("    the notes differ from %s:\n%s", NotesPath.c_str(), Notes.c_str());
	}
	Step(Label, VerseOk && NotesOk);
}

void GoldenFixtures() {
	for (const std::string &Name : ListFixtures()) {
		Golden("golden " + Name + ".gd", Name, verse_gd_convert(LoadInput(Name)));
	}
}

// Dodge the Creeps' four scripts as one selection: `main` learns that `hud.show_game_over()`
// suspends, and `hud.update_score` learns from `main` that its parameter is an int.
void GoldenBatch() {
	std::vector<std::string> Names = { "hud", "main", "mob", "player" };
	std::vector<VerseGdConvertInput> Inputs;
	for (const std::string &Name : Names) {
		Inputs.push_back(LoadInput(Name));
	}
	const std::vector<VerseGdConvertResult> Results = verse_gd_convert_batch(Inputs);
	for (size_t i = 0; i < Names.size(); i++) {
		Golden("golden batch " + Names[i] + ".gd", "batch/" + Names[i], Results[i]);
	}
	// Selection order is not an input.
	std::vector<VerseGdConvertInput> Reversed(Inputs.rbegin(), Inputs.rend());
	const std::vector<VerseGdConvertResult> Again = verse_gd_convert_batch(Reversed);
	bool Same = true;
	for (size_t i = 0; i < Names.size(); i++) {
		Same = Same && Again[Names.size() - 1 - i].verse == Results[i].verse;
	}
	Step("a batch does not depend on the order it was selected in", Same);
}

void Names() {
	Expect("a snake_case var", verse_gd_member_name("max_speed"), "MaxSpeed");
	Expect("a signal handler loses its underscore", verse_gd_member_name("_on_body_entered"), "OnBodyEntered");
	Expect("a SCREAMING constant", verse_gd_member_name("MAX_HEALTH"), "MaxHealth");
	Expect("a name already Pascal", verse_gd_member_name("Speed"), "Speed");
	Expect("a stem from class_name", verse_gd_stem("player", "PlayerController"), "player_controller");
	Expect("a stem from the file", verse_gd_stem("enemy_spawner", ""), "enemy_spawner");
	Expect("a PascalCase file stem", verse_gd_stem("EnemySpawner", ""), "enemy_spawner");
	Expect("a stem with a dash", verse_gd_stem("my-script", ""), "my_script");
	Expect("a stem that starts with a digit", verse_gd_stem("2d_player", ""), "script_2d_player");
	Expect("class_name, read without parsing", verse_gd_scan_class_name("@tool\nextends Node\nclass_name Foo\n"), "Foo");
	Expect("class_name and extends on one line", verse_gd_scan_extends("class_name Foo extends Node2D\n"), "Node2D");
	Expect("an extends path", verse_gd_scan_extends("extends \"res://base.gd\"\n"), "res://base.gd");
	Expect("a common class", verse_gd_common_class("Button", "Label"), "Control");
}

void Refusals() {
	VerseGdConvertInput In;
	In.file_stem = "broken";
	In.source = "extends Node\nfunc _ready():\n\tif true:\n\t\tpass\n  print(1)\n";
	const VerseGdConvertResult Result = verse_gd_convert(In);
	Step("a file that does not parse is refused", !Result.ok && Result.error_line == 5 && Result.verse.empty());

	In.source = "extends Node\nfunc f():\n\treturn (1 + 2\n";
	Step("an unclosed bracket is refused", !verse_gd_convert(In).ok);
}

void Determinism() {
	const VerseGdConvertInput In = LoadInput("features");
	const std::string First = verse_gd_convert(In).verse;
	bool Same = true;
	for (int i = 0; i < 3; i++) {
		Same = Same && verse_gd_convert(In).verse == First;
	}
	Step("the same input converts to the same bytes", Same && !First.empty());
}

void Scenes() {
	const std::map<std::string, std::string> Files = {
		{ "res://player.tscn",
				"[gd_scene load_steps=2 format=3 uid=\"uid://p\"]\n"
				"\n"
				"[ext_resource type=\"Script\" uid=\"uid://ps\" path=\"res://player.gd\" id=\"1_ab\"]\n"
				"\n"
				"[node name=\"Player\" type=\"Area2D\"]\n"
				"script = ExtResource(\"1_ab\")\n"
				"speed = 450\n"
				"\n"
				"[node name=\"Sprite\" type=\"AnimatedSprite2D\" parent=\".\"]\n"
				"\n"
				"[node name=\"Hitbox\" type=\"CollisionShape2D\" parent=\".\" unique_name_in_owner=true]\n"
				"\n"
				"[connection signal=\"body_entered\" from=\".\" to=\".\" method=\"_on_body_entered\"]\n" },
		{ "res://main.tscn",
				"[gd_scene load_steps=3 format=3]\n"
				"\n"
				"[ext_resource type=\"Script\" path=\"res://main.gd\" id=\"1\"]\n"
				"[ext_resource type=\"PackedScene\" path=\"res://player.tscn\" id=\"2\"]\n"
				"\n"
				"[node name=\"Main\" type=\"Node\"]\n"
				"script = ExtResource(\"1\")\n"
				"\n"
				"[node name=\"Player\" parent=\".\" instance=ExtResource(\"2\")]\n"
				"speed = 500\n"
				"points = [1, 2,\n"
				"speed = 3]\n"
				"\n"
				"[connection signal=\"hit\" from=\"Player\" to=\".\" method=\"game_over\"]\n" },
		{ "res://player.gd", "extends Area2D\nclass_name Player\n" },
		{ "res://stats.tres",
				"[gd_resource type=\"Resource\" script_class=\"Stats\" load_steps=2 format=3]\n"
				"\n"
				"[ext_resource type=\"Script\" path=\"res://stats.gd\" id=\"1\"]\n"
				"\n"
				"[resource]\n"
				"script = ExtResource(\"1\")\n"
				"max_hp = 10\n" },
	};
	const VerseGdResourceReader Read = [&](const std::string &Path) {
		auto It = Files.find(Path);
		return It == Files.end() ? std::string() : It->second;
	};

	VerseGdScriptMove Player;
	Player.old_path = "res://player.gd";
	Player.new_path = "res://player.verse";
	Player.old_class_name = "Player";
	Player.new_class_name = "Player";
	Player.renames = {
		{ VerseGdRename::Kind::Property, "speed", "Speed" },
		{ VerseGdRename::Kind::Signal, "hit", "Hit" },
		{ VerseGdRename::Kind::Method, "_on_body_entered", "OnBodyEntered" },
	};

	Expect("the scene that holds the script: its path, its values, its own connection",
			verse_gd_rewrite_resource(Files.at("res://player.tscn"), { Player }, Read),
			"[gd_scene load_steps=2 format=3 uid=\"uid://p\"]\n"
			"\n"
			"[ext_resource type=\"Script\" uid=\"uid://ps\" path=\"res://player.verse\" id=\"1_ab\"]\n"
			"\n"
			"[node name=\"Player\" type=\"Area2D\"]\n"
			"script = ExtResource(\"1_ab\")\n"
			"Speed = 450\n"
			"\n"
			"[node name=\"Sprite\" type=\"AnimatedSprite2D\" parent=\".\"]\n"
			"\n"
			"[node name=\"Hitbox\" type=\"CollisionShape2D\" parent=\".\" unique_name_in_owner=true]\n"
			"\n"
			"[connection signal=\"body_entered\" from=\".\" to=\".\" method=\"OnBodyEntered\"]\n");

	// The instancing scene never names player.gd, and still stores its values and connects its
	// signal -- through the instanced scene's root. A continuation line of a multi-line value is
	// not a property, whatever it looks like.
	Expect("a scene that instances the scene that holds the script",
			verse_gd_rewrite_resource(Files.at("res://main.tscn"), { Player }, Read),
			"[gd_scene load_steps=3 format=3]\n"
			"\n"
			"[ext_resource type=\"Script\" path=\"res://main.gd\" id=\"1\"]\n"
			"[ext_resource type=\"PackedScene\" path=\"res://player.tscn\" id=\"2\"]\n"
			"\n"
			"[node name=\"Main\" type=\"Node\"]\n"
			"script = ExtResource(\"1\")\n"
			"\n"
			"[node name=\"Player\" parent=\".\" instance=ExtResource(\"2\")]\n"
			"Speed = 500\n"
			"points = [1, 2,\n"
			"speed = 3]\n"
			"\n"
			"[connection signal=\"Hit\" from=\"Player\" to=\".\" method=\"game_over\"]\n");

	VerseGdScriptMove Main;
	Main.old_path = "res://main.gd";
	Main.new_path = "res://main.verse";
	Main.renames = { { VerseGdRename::Kind::Method, "game_over", "GameOver" } };
	const std::string Both = verse_gd_rewrite_resource(Files.at("res://main.tscn"), { Player, Main }, Read);
	Step("a batch rewrites both ends of one connection",
			Both.find("[connection signal=\"Hit\" from=\"Player\" to=\".\" method=\"GameOver\"]") != std::string::npos
					&& Both.find("path=\"res://main.verse\"") != std::string::npos);

	Step("a resource that names nothing converted is not touched",
			verse_gd_rewrite_resource(Files.at("res://stats.tres"), { Player }, Read) == Files.at("res://stats.tres"));

	VerseGdScriptMove Stats;
	Stats.old_path = "res://stats.gd";
	Stats.new_path = "res://stats.verse";
	Stats.old_class_name = "Stats";
	Stats.new_class_name = "StatsBlock";
	Stats.renames = { { VerseGdRename::Kind::Property, "max_hp", "MaxHp" } };
	const std::string Resource = verse_gd_rewrite_resource(Files.at("res://stats.tres"), { Stats }, Read);
	Step("a .tres: its script, its values and the class name in its header",
			Resource.find("script_class=\"StatsBlock\"") != std::string::npos && Resource.find("path=\"res://stats.verse\"") != std::string::npos
					&& Resource.find("\nMaxHp = 10") != std::string::npos);

	const std::map<std::string, std::string> Types = verse_gd_scene_node_types(Files.at("res://player.tscn"), "res://player.gd", Read);
	std::string Rendered;
	for (const auto &KV : Types) {
		Rendered += KV.first + "=" + KV.second + ";";
	}
	Expect("what `$` sees, from the scene", Rendered, "%Hitbox=CollisionShape2D;Hitbox=CollisionShape2D;Sprite=AnimatedSprite2D;");

	const std::map<std::string, std::string> MainTypes = verse_gd_scene_node_types(Files.at("res://main.tscn"), "res://main.gd", Read);
	Expect("an instanced child is its script's class", MainTypes.count("Player") ? MainTypes.at("Player") : "", "Player");

	std::vector<std::string> Deps = verse_gd_resource_dependencies(Files.at("res://main.tscn"));
	Expect("dependencies", Deps.size() == 2 ? Deps[0] + "," + Deps[1] : "", "res://main.gd,res://player.tscn");
}

void Callers() {
	VerseGdScriptMove Player;
	Player.old_path = "res://player.gd";
	Player.new_path = "res://player.verse";
	Player.old_class_name = "Player";
	Player.new_class_name = "Player";
	Player.renames = {
		{ VerseGdRename::Kind::Property, "speed", "Speed" },
		{ VerseGdRename::Kind::Method, "start", "Start" },
		{ VerseGdRename::Kind::Signal, "hit", "Hit" },
	};
	const std::string Source =
			"extends Node\n"
			"const PlayerScript = preload(\"res://player.gd\")\n"
			"var hero: Player\n"
			"var other\n"
			"func _ready():\n"
			"\thero.start(Vector2.ZERO)\n"
			"\thero.hit.connect(_on_hit)\n"
			"\t$Player.speed = 5\n"
			"\tother.start()\n"
			"\t(other as Player).speed += 1\n"
			"\tif hero.has_method(\"start\"):\n"
			"\t\tpass\n"
			"\t$Timer.start()\n";
	const VerseGdCallerRewrite Result = verse_gd_rewrite_callers(Source, { Player }, { { "Player", "Player" }, { "Timer", "Timer" } });
	Expect("typed receivers are rewritten, untyped ones are not",
			Result.text,
			"extends Node\n"
			"const PlayerScript = preload(\"res://player.verse\")\n"
			"var hero: Player\n"
			"var other\n"
			"func _ready():\n"
			"\thero.Start(Vector2.ZERO)\n"
			"\thero.Hit.connect(_on_hit)\n"
			"\t$Player.Speed = 5\n"
			"\tother.start()\n"
			"\t(other as Player).Speed += 1\n"
			"\tif hero.has_method(\"start\"):\n"
			"\t\tpass\n"
			"\t$Timer.start()\n");
	std::string Unresolved;
	for (const VerseGdCallerSite &Site : Result.sites) {
		if (!Site.rewritten) {
			Unresolved += std::to_string(Site.line) + ";";
		}
	}
	// `$Timer.start()` is a Timer's own `start`: the scene says so, and it is not reported.
	Expect("what is left for the author is reported, and only that", Unresolved, "9;11;");
}

} // namespace

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--update") == 0) {
			g_update = true;
		} else {
			g_dir = argv[i];
		}
	}
	Names();
	Refusals();
	Determinism();
	Scenes();
	Callers();
	GoldenFixtures();
	GoldenBatch();
	if (g_failures) {
		printf("[verse_gd_convert_test] %d failure(s)\n", g_failures);
		return 1;
	}
	printf("[verse_gd_convert_test] all passed\n");
	return 0;
}
