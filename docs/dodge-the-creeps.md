# Dodge the Creeps — the port, and what it cost

**Status:** 2026-09-12 · **it plays.** Phase 2, Stage 7. The project is
[`dodge-the-creeps/`](../dodge-the-creeps); the game is five `.verse` files and four scenes, with no
GDScript in it.

Godot's canonical first game is the yardstick this project set itself:
[`roadmap.md`](roadmap.md) says a Godot developer who is not the author should be able to build a
real project and not hit a wall, and Dodge the Creeps is the smallest real project there is. The
roadmap expected this attempt to stop somewhere and be committed half-broken so the next attempt
could start where it stopped. It did not stop. What follows is the price of that, wall by wall,
because **the port completing does not mean the bridge is finished** — it means seven things a
GDScript author writes without thinking had to be written some other way, each of them a
requirement with a phase attached, and one trap with no GDScript counterpart at all.

    godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd

`headless_check.gd` is the only GDScript in the directory and is not part of the game: it presses
Start, holds a movement key, kills the player, and asserts on what Godot sees of the Verse scripts
— 29 checks, one line each. `--fixed-fps` is not optional, and the reason is in that file. The port
is **not** wired into `tools/run_tests.py`: a yardstick that gates the build stops being an honest
measure of how far the bridge has got.

---

## The walls

| | what a GDScript author writes | what the port writes instead | requirement | phase |
| --- | --- | --- | --- | --- |
| **1** | `$AnimatedSprite2D.play()` | an `@export`-ed typed slot per child, filled in by the scene | **R-SCN-6** | 4 |
| **2** | `signal hit` / `hit.emit()` | one engine signal wired to two scripts in the scene file | **R-SIG-1, R-SIG-2** | 4 |
| **3** | `await $MessageTimer.timeout` | a four-state enum, a second Timer node, two handlers | **R-SIG-5** | 4 |
| **4** | `velocity.normalized() * speed`, `PI`, `randf()` | `scripts/vectors.verse`, six functions and three constants | **R-SCN-3** / OQ-11 | 4 |
| **5** | `mob.linear_velocity = v` on an instantiated scene | `Mob.Set("linear_velocity", VariantFromVector2(V))` | **R-SCN-6** | 4 |
| **6** | `get_tree().call_group(&"mobs", &"queue_free")` | walk `GetNodesInGroup("mobs")` and free each | vararg, R-SCN-2 permits | — |
| **7** | `node.callv("method", [args])` | nothing: it is spellable and cannot be given arguments | **R-INT-2** | see below |
| **8** | *(no counterpart)* | `<transacts>` on every helper in a library file, or it fails at its first call site | — | — |

Wall 7 is the one that corrects an earlier document, and wall 8 is the only one that is not a
missing feature. Both are below.

### 1 · No `is`/`as`, so `$Child` becomes an inspector slot

`GetNode` returns a `node`. Nothing turns that `node` into the `animated_sprite2d` whose `Play()`
the next line wants, because the failable cast **R-SCN-6** asks for is specified, de-risked and not
built ([`phase-2-design.md` §11](phase-2-design.md)). So every `$Name` in the four GDScript files
becomes an `@export` slot with the child's own type, and the scene fills it in:

```
	@export
	var Sprite<public>:?animated_sprite2d = false
```

Seventeen of the port's eighteen object-typed slots exist for no other reason: one per node
lookup the GDScript does with `$Name` or `get_node`, and the eighteenth is the extra timer wall 3
needs. It works, and the inspector is arguably a better place for the wiring than a string in the
script — but three things
are worse. The scene file now carries knowledge the script used to carry, so reading the script no
longer tells you what it touches. A child renamed in the editor empties the slot silently, where
`$ScoreTimer` would have failed loudly at the call. And because nothing can force a value into an
inspector slot, every one of them is an `?option` the script has to unwrap, so a three-line method
becomes five:

```
	GameOver<public>(Body:node2d):void =
		if (Countdown := ScoreTimer?):
			Countdown.Stop()
		if (Countdown := MobTimer?):
			Countdown.Stop()
```

That is `$ScoreTimer.stop(); $MobTimer.stop()` in the original. The option is honest — the slot
really can be empty — and it is still the single largest source of added lines in the port.

### 2 · No script-declared signals, so the scene wires one engine signal to two scripts

`player.gd` declares `signal hit`, emits it when something touches the player, and `main.tscn`
connects it to `game_over`. `hud.gd` does the same with `start_game`. A Verse script can declare
neither (**R-SIG-1**) and emit neither (**R-SIG-2**): `Object.add_user_signal` needs a `godot_array`
a script cannot build (wall 7) and `emit_signal` is a vararg.

What the port does instead is connect **Godot's own** signal to both scripts:

```
[connection signal="body_entered" from="Player" to="." method="GameOver"]
[connection signal="pressed" from="HUD/StartButton" to="." method="NewGame"]
```

`player.tscn` connects the same `body_entered` to the player's own handler. The behaviour is
identical and the design is worse in a specific way: "what does it mean for the player to be hit"
used to be one line in `player.gd` and is now a fact about two scene files. It also leaves a visible
scar, because main's handler now receives the collided body it has no use for:

```
	# It takes a body it does not use because the signal it is standing in for -- the player's
	# own `hit` -- cannot be declared in Verse.
	GameOver<public>(Body:node2d):void =
```

The half of signals that **does** work is the half that matters most for a port, and it is wall-free:
see [what needed no workaround](#what-needed-no-workaround).

### 3 · No `await`, so a seven-line sequence becomes a state machine

`hud.gd`'s game-over sequence is the most compact code in the original game:

```gdscript
func show_game_over():
	show_message("Game Over")
	await $MessageTimer.timeout
	$MessageLabel.text = "Dodge the\nCreeps"
	$MessageLabel.show()
	await get_tree().create_timer(1).timeout
	$StartButton.show()
```

A Verse script cannot await a Godot signal (**R-SIG-5**), and Verse's own concurrency — `sync`,
`race`, `block` — has no meeting point with one. The second wait is worse than the first: a
`SceneTreeTimer`'s `timeout` is a signal on an object nothing in the scene can connect to, so the
port adds a **second Timer node** that the GDScript version does not have.

What replaces seven lines is a `hud_phase` enum, an extra node, and two timeout handlers that read
the phase to decide which step they are — 30 lines for the same behaviour, and the sequence is no
longer readable in one place. This is the wall that most changes the *shape* of a script rather than
its spelling.

### 4 · No `@GlobalScope` and no math-type methods, so the port ships `vectors.verse`

`player.gd`'s `_process` is vector arithmetic from top to bottom, and none of it has a
spelling:
`Vector2.ZERO`, `+=` on a vector, `* speed`, `.length()`, `.normalized()`, `.clamp()`, `.rotated()`,
`PI`, `randf()`, `randf_range()`. Godot's sixteen math types are **data** in the mirror — fields and
no methods — and `@GlobalScope`'s functions have no object handle to ride a call on, which is why
both left Phase 2 together as **OQ-11** with **R-SCN-3** behind it.

So the port writes [`scripts/vectors.verse`](../dodge-the-creeps/scripts/vectors.verse): `V2Add`,
`V2Scale`, `V2Length`, `V2Normalized`, `V2Clamp`, `V2Rotated`, and `Pi` with two fractions of it.
This is the wall the port felt most, because unlike the others it is not in one place — it is in
every frame of the player's movement, and it turns

```gdscript
	position += velocity * delta
	position = position.clamp(Vector2.ZERO, screen_size)
```

into

```
		set Position = V2Clamp(V2Add(Position, V2Scale(Velocity, Delta)), vector2{}, ScreenSize)
```

Two things about this wall are better than they look. The file is a **library file** — no class named
after itself — which is [R-LANG-6](spec.md)'s third clause working exactly as Phase 2 built it: every
sibling script sees those names with nothing written to import them. And half of what the GDScript
needed did not need a workaround at all, because Verse's own standard library already has it:
`Sqrt`, `Clamp`, `Cos`, `Sin`, `GetRandomFloat` and `GetRandomInt` are what the port calls, which is
**R-AUD-2**'s rule ("where Verse's stdlib has a counterpart, Verse's spelling wins") landing as
intended rather than as a compromise. When OQ-11 is answered, `vectors.verse` should shrink to
nothing but `Pi` — and if the answer to "can `operator'+'` be defined at all" is yes, to nothing.

### 5 · `Instantiate` returns a `node`, so the mob is configured through `Object.Set`

This is wall 1 again, in the one place an inspector slot cannot help: the mob does not exist until
the spawn timer fires.

```gdscript
	var mob = mob_scene.instantiate()
	mob.position = mob_spawn_location.position
	mob.rotation = direction
	mob.linear_velocity = velocity
```

`PackedScene.Instantiate` is typed to return `node`, and `LinearVelocity` is on `rigid_body2d`. With
no cast, the only route left is the untyped one — which does work, and is worth recording as working,
because it is the whole point of Phase 2 having made `variant` nameable:

```
			if (Mob := Scene.Instantiate[packed_scene_gen_edit_state.Disabled]):
				Mob.Set("position", VariantFromVector2(Spawn.Position))
				Mob.Set("rotation", VariantFromFloat(Direction))
				Mob.Set("linear_velocity", VariantFromVector2(Velocity))
```

The cost is the cost of any stringly-typed call: `"linear_velocty"` compiles. The mirror's whole
argument is that a Godot member should be a name the compiler checks, and here the port hands that
back. R-SCN-6 is what buys it back, and `phase-2-design.md` §11 already says what it needs —
`VhAsObject`, a cast at every object return, and a handle→class cache.

### 6 · Varargs are not mirrored, so a group is walked

`get_tree().call_group(&"mobs", &"queue_free")` has no mirror, because `call_group` is a vararg and
vararg is one of the three skip categories **R-SCN-2** permits. The replacement is three lines and
is better code:

```
		if (Tree := GetTree[]):
			for (Mob : Tree.GetNodesInGroup("mobs").ToArray()):
				Mob.QueueFree()
```

It is in the list not because it hurt but because it is the first time a permitted skip has turned
up in the yardstick, and it turned up as an inconvenience rather than a blocker. That is the
evidence R-SCN-2's bar was set in the right place.

### 7 · `Object.callv` is reachable and uncallable — a correction

**R-INT-2** (call a method dynamically by name) is recorded as **done**, and the integration test
that proves it is real: `CallSibling(Target, Method, Args:godot_array)` calls a GDScript method and
reads the result back. Look at where `Args` comes from, though — `node.call("CallSibling", self,
"_double", [21])`, from GDScript. Every array in that test was handed to Verse by Godot.

A script that wants to make such a call *on its own initiative* cannot, because `Callv` takes its
arguments as a `godot_array` and there is no way to make one. The only `godot_array` a script can
hold is one Godot gave it:

```
	Args := godot_array{}            # compiles
	Target.Callv("queue_free", Args) # ERROR: Cannot convert argument 2 from Nil to Array
```

The container wrappers are `<public>` so they can be named in a signature, and their `Ref` is not
public — deliberately, per R-TYPE-7 — so `godot_array{}` is a wrapper around reference 0, which
crosses as `Nil`. There is no `MakeArray`, and `VhRefNew` is module-scoped. The same gap makes
`AddUserSignal` unusable (wall 2), and it makes every mirrored method that takes an `Array` or a
`Dictionary` — 90 methods in the mirror, and another 83 taking a typed one — a call a script can
only make by passing on a container it was given.

So **R-INT-2 is `part`, not `done`**: dispatch by name works, and originating the call does not.
What it needs is small, and it is `spec.md` §6's to name rather than this document's: a way to make
an empty container and add to it. `VhRefNew` is already the native function for the first half, and
the generator already emits the typed accessors for the second.

Probing this also turned up a **misleading diagnostic**, fixed in the same commit: reference 0 was
reported as "a Godot container the bridge no longer holds … a handle kept past the object that
owned it", which sends the author looking for a lifetime bug they do not have. Reference 0 never
named anything, and it now says so.

### 8 · A library file's helpers must say `<transacts>`

Not a missing feature — a trap, and the only wall the port hit that no requirement covers.

A module-scope function with no effect specifier is `no_rollback`, and a `no_rollback` function
cannot be called from inside a transaction. Every Godot callback runs inside one, because the host
opens a transaction around `Ready`, `Process` and every signal handler. So this compiles on its
own:

```
	V2Length<public>(A:vector2):float = Sqrt(A.X * A.X + A.Y * A.Y)
```

and fails at its first call site, in another file, with

    This invocation calls a function (`V2Length`) that has the 'no_rollback' effect, which is not
    allowed by its context.

— a message naming an effect the author never wrote down, pointing at the caller rather than at the
declaration that needs fixing. Every function in `vectors.verse` says `<transacts>` for this reason,
and writing `<decides>` *instead of* `<transacts>` on the one failable helper reproduced it, because
an explicit specifier **replaces** the default set rather than adding to it.

Nothing here is wrong, and the fix is one word. But it is the first thing a Godot author hits on
their first library file, and it is invisible in the file they have to change. Worth a line in the
eventual authoring guide, and worth considering whether the `.verse` template and the R-SCN-2-style
diagnostic machinery can say it at the declaration.

---

## What needed no workaround

A yardstick that only lists walls measures nothing. All of the following is what the port leans on,
and none of it needed a second attempt:

- **A connection carried in a scene file to a Verse method works** — **R-SIG-4**'s load-bearing
  half, and what makes walls 2 and 3 survivable rather than fatal. Ten connections across all four
  scene files reach
  Verse methods by name: five `Timer.timeout`s, `Button.pressed` to two different scripts,
  `Area2D.body_entered` to two different scripts, and
  `VisibleOnScreenNotifier2D.screen_exited`. A Verse method is spelled `OnMobTimerTimeout` rather
  than `_on_mob_timer_timeout`, which is the only visible difference. The scene files here were
  written by hand; connecting through the editor's Node panel, and `_make_function` writing the
  handler for you, are the untested half.
- **An `@export` slot typed as another script's class** resolves to the node the scene assigns, and
  the receiving script's own methods are on it. This is what lets main call `Player.Start(Pos)` and
  `Hud.ShowGameOver()` — three scripts calling each other's methods, all of it compile-checked.
- **An `@export` slot typed as a Resource** — `?packed_scene` — is a resource picker that resolves,
  which is how the mob scene reaches main.
- **The Input singleton**: `GetInputSingleton[]` then `IsActionPressed("move_right")`. No
  `@GlobalScope` needed, because Godot's singletons are objects and Phase 2 generated an accessor
  for each.
- **Typed containers with objects in them**: `GetNodesInGroup("mobs")` is a `typed_array(node)`
  whose elements are nodes a script calls `QueueFree()` on. This is the gap Phase 2 named as the
  most-hit one in the mirror, and walking a group is the shape scene code actually has.
- **`variant` as a parameter**: `Set`, `SetDeferred` and the `VariantFrom<GodotType>` builders carry
  the mob's spawn state and the player's deferred collision disable.
- **Enums, `@export_group`, and a script's own enum**: `packed_scene_gen_edit_state.Disabled` and
  `node_internal_mode.Disabled` are named values, and `hud_phase` is the port's own.
- **`[]string` out of Godot**: `SpriteFrames.GetAnimationNames()` is a Verse array, indexable with
  Verse's own failable indexing.
- **Library files** — `vectors.verse`, R-LANG-6's third clause.
- **Five scripts in a `gameplay` module** — `scripts/gameplay.vmodule` and nothing else. They
  were in one flat scope when this was written, with no name collisions, which is the constraint
  Phase 3's modules removed and which had not bitten at four files. Moving them is what found
  the qualified-name bug in that phase: an `@export` slot typed as another script's class stopped
  resolving, and no test in the repo could see it.

## What is not ported

- **The audio is called but not heard**: `Music.Play()` and `DeathSound.Play()` run and raise
  nothing; headless has no device to play them on.
- **`VisibleOnScreenNotifier2D` is checked by emitting `screen_exited`**, not by letting the
  notifier fire it. The Verse handler is what is under test, and whether Godot's visibility system
  reports anything without a drawn viewport is its own question.
- **Nobody has played it with a window.** Every claim above is the headless check's; the port is
  owed one windowed run before Phase 3 closes.
