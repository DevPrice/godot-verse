# Converting GDScript to Verse

**Status:** 2026-09-25 · **built; the converter is tested, the editor half has not been run.**
R-TOOL-13. Right-click a `.gd` in the FileSystem dock or on the script editor's script list and
choose **Convert to Verse**. The file is replaced by a Verse equivalent, the scenes and resources
that use it are updated to match, and the whole change is one Edit > Undo.

Everything that decides *what the Verse says* is `src/verse_gd_*.cpp`. It is pure, needs neither
Godot nor Unreal, and is asserted in the units layer (`tests/verse_gd_convert`). Everything that
touches the editor is `src/verse_convert_menu.cpp`. No test here can run that half, so it is listed
under [What is not verified](#what-is-not-verified).

## The requirements, as settled

These came from an interview with the project owner before anything was written, and each one
decided code:

| question | answer | where it lives |
| --- | --- | --- |
| What happens to GDScript that cannot be translated faithfully? | **Emit, and mark only what truly cannot be translated.** The statement becomes `# TODO(convert): <why>` over the GDScript it replaced, the file is still written, and the note is listed. `var x` is *not* such a case (it is `var X:?variant = false`), and neither is a lambda (it is a hoisted method) | `converter::emit_todo` |
| What does "replaces the file" mean? | **Write the `.verse`, repoint every scene and resource, delete the `.gd`, as one undoable action.** Undo was asked for "if possible", with a confirmation before migrating as the fallback; undo turned out to be possible, so there is no confirmation | `VerseConvertMenu::apply_files` |
| How far does type inference go? | **Local inference, falling back to `variant`**: literals, constructors, typed calls, the scenes that say what `$Child` is, the first assignment to an untyped member, the use of an untyped parameter | `ty_from_gd`, `type_vars`, `coerce` |
| Where does the converter live? | **A godot-cpp-free C++ unit in `src/`** with its own test binary | `src/verse_gd_*.cpp` |
| Names? | **PascalCase members, and rewrite the scenes to match.** `speed` → `Speed`, `_on_body_entered` → `OnBodyEntered`; Godot's virtuals keep Godot's spelling (`_ready` → `_Ready`) | `pascal`, `verse_gd_rewrite_resource` |
| Other GDScripts that call a renamed member? | **Ask after converting.** Where the receiver's type is known, offer to rewrite; list everything else for the author and touch none of it | `verse_gd_rewrite_callers`, `prepare_caller_updates` |
| Lambdas? | **Hoist to a method for now**, with a TODO in the implementation to emit a nested function (and later a lambda) once Verse accepts one | `scan_expr`, marked `TODO(nested-functions)` |
| What can be selected? | **One `.gd` or many**; many are one batch and one undo | `verse_gd_convert_batch` |

## What deterministic means here

The output is a function of the inputs and nothing else: the file's text, its stem, the node types
the scenes give `$` paths, and the project's other script classes. Nothing is keyed by pointer or
clock, and a batch is sorted before it is converted. The units layer asserts that converting the
same input four times gives the same bytes, and that a batch converts the same way in either
selection order.

## Reading the output

The shape follows `dodge-the-creeps/`, the hand port of Godot's own first game, because that is
what an author reading converted code will compare it to.

**Anything failable is hoisted into an `if`.** `$Sprite.play()` is not an expression Verse can
write: the lookup can fail and the cast can fail. So the statement becomes the body of an `if` whose
clauses bind them, and consecutive statements over the same lookups share one:

    if (AnimatedSprite2DFound := GetNode["AnimatedSprite2D"], AnimatedSprite2DNode := animated_sprite2d[AnimatedSprite2DFound]):
        AnimatedSprite2DNode.SetAnimation("right")
        set AnimatedSprite2DNode.FlipV = false

**This is the one systematic difference in behaviour.** Where GDScript raises on a missing node,
the Verse does nothing. Where a failable value is needed as a *value*, as in a `var` initialiser or
a `return`, it is an `if ... then ... else <default>` expression, and the default is the type's zero.

**The rules, construct by construct:**

| GDScript | Verse |
| --- | --- |
| `extends Area2D`, `class_name Player` | `player := class(area2d):` under `@global_class`. The file is named after the class, snake case, because only the class named after its file can go on a node. A `class_name` that does not survive the round trip (`HUD` registers as `Hud`) is noted, and the callers dialog rewrites the type name |
| `@tool`, `@icon("…")` | `@tool`, `@icon("…")` |
| `signal hit`, `signal scored(points: int, bonus)` | `@export_signal` over `Hit:event() = event(){}` and `Scored:event(tuple(int, variant))`. An object payload is an option, because a signal can carry null |
| `hit.emit()`, `emit_signal("hit", x)` | `Hit.Emit(())`, `Hit.Emit(X)`, `Scored.Emit((A, B))` |
| `sig.connect(handler)`, `$Timer.timeout.connect(f)` | `Sig.Subscribe(Handler)`, `Timer.Timeout().Subscribe(F)`. An untyped handler's parameters take the signal's payload types |
| `await $Timer.timeout`, `await get_tree().create_timer(1).timeout` | `.Timeout().Await()` in a `<suspends>` method |
| a method that awaits | `<suspends>`. Called without `await` it is `spawn{...}`, and the caller loses its specifier, because `spawn` needs an unnarrowed body. A **virtual** that awaits keeps Godot's declaration and spawns a `<Name>Async` helper holding the body |
| any other method | `<transacts>`, the yardstick's own choice for a helper, unless it calls something unnarrowed. Worked out as a fixpoint (`solve_effects`) |
| `func _process(delta)` | `_Process<override>(Delta:float):void`; the parameter types come from the mirror |
| a bool virtual (`_has_point`, `_can_drop_data`) | the `<decides>` override calls a `<Name>Value():logic` helper holding the body, so `return x` stays `return` |
| `@export var speed = 400` | `@export` over `var Speed:int = 400`; `@export_range(0, 10)` is a bounded type, `type{_X:int where 0 <= _X, _X <= 10}` |
| `@export_file`, `_dir`, `_multiline`, `_flags`, `_node_path`, `_group`, `_subgroup`, `_category` | the bridge's attribute of the same name, one string argument |
| any other `@export_*` hint | `@export` and a TODO: the hint has no Verse attribute |
| `@onready var s = $Sprite` | `var S:?sprite2d = false`, filled at the top of `_Ready` (made if absent) |
| a member initialiser that calls something, `preload(...)` | filled at the top of `_Ready`, noted: a Verse member's default cannot call |
| a `const` | an immutable member; read from a default, its value is inlined |
| `var x` (untyped, no initialiser) | the type of the first assignment to it in the file, else `var X:?variant = false` |
| an object-typed member | an option: `var Target:?node2d = false` |
| `enum State { IDLE, RUN }` | `player_state := enum:` at module scope; explicit values are a TODO, since a Verse enum has none |
| setters and getters | `SetX`/`GetX` methods this file calls instead of writing the member. A TODO says that Godot's own writes (inspector, scene, animation) skip them |
| `static func` | a module-level function prefixed with the class: `PlayerDouble` |
| an inner `class Helper` | a top-level class `player_helper`, noted: it cannot go on a node |
| a lambda | a method `<Function>Lambda<n>`. One that reads the enclosing function's locals is a TODO, because a hoisted method cannot capture them |
| `x = v`, `x += v` | `set X = V`, `set X += V` |
| `position.x += 1` | `set Position = vector2{X := Position.X + 1.0, Y := Position.Y}`: a struct's field cannot be written alone |
| `arr.append(x)` | `set Arr += array{X}` |
| `var d := {}`, `d[k] = v`, `d.get(k, 0)`, `d.has(k)`, `d.size()` | Godot's own Dictionary, through the mirror's typed accessors: `dictionary{}`, `D.SetInt(K, V)`, `if (Entry := D.GetInt[K]) then Entry else 0`, `D.GetVariant[K]` as a test (a lookup of an absent key *fails*), `D.Length()`. A literal with keys is a Verse map instead |
| `typeof(x) == TYPE_INT` | `VariantKind(X) = variant_type.TypeInt` |
| `StringName("x")`, `PackedStringArray([...])`, `Array(xs)` | the value itself: in Verse it is already a `string`, an array |
| `a / b` on ints, `a % b` | `TruncatedQuotient[A, B]` (it truncates as GDScript does), `Mod[A, B]` (noted: it floors) |
| `int + float` | the int promoted, `Speed * 1.0`; a literal becomes `400.0` |
| `if a and b:` | `if (A, B):`, a clause list, so each clause can bind |
| `if x:`, `if x != null:`, `if x is Foo:` | narrowing: `if (XValue := X?)`, and `x` means the bound value in the clauses after it and in the body |
| `a if c else b` | `if (C) then A else B` |
| `match` | an `if`/`else if` chain over `=`; binding, array and dictionary patterns are TODOs |
| `while c:` | `loop:` over `if (c): body else: break` |
| `for i in range(n)`, `for x in arr`, `for k in dict` | `for (I := 0..N - 1)`, `for (X : Arr)`, `for (K -> V : Dict)`. A `for` containing `break` or `continue` is a TODO |
| `continue` | a TODO: Verse has none |
| a statement that is a TODO | the comment, and nothing else -- except a `return`, which also becomes `return Err("…")` so the function still has its type, and a body left with nothing but comments, which gets `{}` |
| `str(x)`, `"%d" % x`, `print(a, b)` | interpolation, `"{X}"`. A math value is spelled field by field, `({V.X}, {V.Y})`, and a `logic` as the word. A `variant` is a TODO because nothing gives it a `ToString`, and so is a padded or precise `%` format |
| `Input.is_action_pressed(...)` | `GetInputSingleton().IsActionPressed[...]`, a test. Every spelling comes from the mirror's table (below) |
| a member the mirror does not know, on an object | `X.Get("name")`, `X.Set("name", …)`, `X.Call("name", …)`, which is Godot's own dynamic access and what GDScript did. Noted, not marked |
| another script class's members | their conversion's own spelling when converted in the same batch, PascalCase otherwise (what a generated binding spells). A class that stays GDScript adds `using { /Godot.org/Bindings }` |

**Where spellings come from.** The converter cannot ask the host how the mirror spells anything, and
the spelling is not derivable from Godot's name: `get_node_or_null` is `GetNode[...]`, a predicate is
`[...]` and a test, a string property is `SetText(...)` because a `string` cannot be a `var`
property, and `add_child` needs two arguments Godot defaults and Verse does not. All of that is
decided in `tools/gen_verse_api.py`, so the pass that writes the mirror also writes
`src/verse_gd_api.gen.h`: 23,000 rows, one per method, property, signal, constant, static, utility
and enum value, each with its shape, result and parameters. Rows are recorded where each member
is emitted, so the table cannot disagree with the mirror.

## The files around the script

**Scenes and resources** (`verse_gd_rewrite_resource`). Every `.tscn` and `.tres` in the project is
checked. It is edited as text, line by line, never re-serialised, so nothing the conversion did not
mean to change moves:

- the `[ext_resource]` path;
- each stored exported value's key, in every section whose script is the converted one. That
  includes a node that **instances** a scene whose root carries it, which is where an override like
  `speed = 500` lives;
- a `[connection]`'s `signal=` where the emitting node carries it, and `method=` where the receiving
  node does;
- a `.tres` header's `script_class=`.

A binary `.scn`/`.res` cannot be read. The editor counts them and says so.

**The UID moves with the script.** `player.gd.uid` becomes `player.verse.uid` with the same text,
and `ResourceUID` is pointed at the new path, so every `uid://` reference resolves to the Verse file.

**Other GDScripts** (`verse_gd_rewrite_callers`). After converting, every other `.gd` is checked for
references to what changed. A reference is rewritten only where the receiver's type is *written
down*: a variable, parameter or cast of the converted `class_name`, `$Path` where the caller's own
scenes say it is that class, the class name itself, or `preload("res://player.gd")`. Everything else,
such as an untyped receiver or a member named in a string (`call("start")`, `has_method`,
`connect("hit", …)`), is listed and left alone. A dialog offers the rewrites as a second undoable
action. The listing also goes to the Output panel, so it survives the dialog being dismissed.

**A batch knows about itself.** Converting several files together runs the conversion three times:
each round, every file is converted knowing what the others said about themselves (member
spellings and types, which methods suspend) and what they pass to each other's untyped parameters.
That is how `main` learns to `spawn{Hud.ShowGameOver()}` and how `hud.update_score(score)` learns
its parameter is the `int` `main` passes it. The goldens under `tests/verse_gd_convert/fixtures/batch`
are Dodge the Creeps' four scripts converted that way.

## What is not verified

- **The editor half has not been run.** No Godot was available when it was written, and nothing
  automated can drive a context menu. It was compiled against godot-cpp's 4.7 bindings with g++, not
  linked -- the extension does not link on Linux, which is out of scope (`verse_runtime.cpp` is
  Windows-only) -- and not built with MSVC. The by-hand check: convert `dodge-the-creeps`' GDScript
  originals (`tests/verse_gd_convert/fixtures/`) in a copy of the project, play it, undo, play again.
- **The Verse the converter writes has not been compiled.** Its spellings follow the mirror and the
  repository's own fixtures. Five do not appear in any fixture here and are Verse's documented
  forms, so `tests/verse_probe` should be asked about them before anything leans on them: `loop:`
  with `break`, `(super:)Method()`, an empty block written `{}` as a statement, `if (set A[I] = V) {}`
  for an indexed write, and `BitLshift`/`BitRshift` for shifts.
- **Interpolating a `float`** prints Verse's formatting, which is not Godot's (`1.0` where Godot
  writes `1`).
- **A converted script's `class_name`** is only preserved when it is the PascalCase of its snake
  case. Otherwise the global name changes (noted), and a `.tres`'s `script_class` and typed
  GDScript references follow it.
