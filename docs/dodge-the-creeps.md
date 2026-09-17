# Dodge the Creeps — the port, and what it cost

**Status:** 2026-09-12 · **it plays, and it is idiomatic now.** Written at the close of Phase 2
stage 7; re-ported in place at the close of **Phase 4**, which is what the walls below were
measured for. The project is [`dodge-the-creeps/`](../dodge-the-creeps); the game is four `.verse`
files and four scenes, with no GDScript in it.

**Seven of the eight walls are down**, the seventh at the close of **Phase 5**, which re-ported
`hud.verse` in place. Read the table for what each cost while it stood — that is what the document
is for, and it is still the record of what the absences were worth. What changed in the port is in
§"After Phase 4" and §"After Phase 5" at the end.

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
— 30 checks, one line each. `--fixed-fps` is not optional, and the reason is in that file. The port
is **not** wired into `tools/run_tests.py`: a yardstick that gates the build stops being an honest
measure of how far the bridge has got.

---

## The walls

| | what a GDScript author writes | what the port wrote instead | requirement | state |
| --- | --- | --- | --- | --- |
| **1** | `$AnimatedSprite2D.play()` | an `@export`-ed typed slot per child, filled in by the scene | **R-SCN-6** | **down** (Phase 4 stage 1) |
| **2** | `signal hit` / `hit.emit()` | one engine signal wired to two scripts in the scene file | **R-SIG-1, R-SIG-2** | **down** (stage 4) |
| ~~**3**~~ | `await $MessageTimer.timeout` | a four-state enum, a second Timer node, two handlers | **R-SIG-5** | **down** (Phase 5) |
| **4** | `velocity.normalized() * speed`, `PI`, `randf()` | `scripts/vectors.verse`, six functions and three constants | **R-SCN-3** / OQ-11 | **down** (stage 6) |
| **5** | `mob.linear_velocity = v` on an instantiated scene | `Mob.Set("linear_velocity", VariantVector2(V))` | **R-SCN-6** | **down** (stage 1) |
| **6** | `get_tree().call_group(&"mobs", &"queue_free")` | walk `GetNodesInGroup("mobs")` and free each | vararg, R-SCN-2 permits | **standing**, and permitted |
| **7** | `node.callv("method", [args])` | nothing: it is spellable and cannot be given arguments | **R-INT-2** | **down** (stage 2) |
| **8** | *(no counterpart)* | `<transacts>` on every helper, or it fails at its first call site | — | **narrowed twice, not gone** — Phase 4.5 made reading Godot `<reads>`, Phase 5 widened the handler |

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
needed before Phase 5 took it away. It works, and the inspector is arguably a better place for the wiring than a string in the
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

### 3 · No `await`, so a seven-line sequence became a state machine — **closed in Phase 5**

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

What replaced seven lines was a `hud_phase` enum, an extra node, and two timeout handlers that read
the phase to decide which step they are — 30 lines for the same behaviour, and the sequence was no
longer readable in one place. This was the wall that most changed the *shape* of a script rather
than its spelling, which is why it was the last one left.

**Phase 5 closed it, and it cost less than this document assumed.** Verse's concurrency does have a
meeting point with a Godot signal: `signal(t)` holds a `/Verse.org/Verse` `event(t)` and
answers a typed payload from it, in ordinary Verse with no native and no ABI. What `hud.verse` says
now is what `hud.gd` says:

```verse
	# main.verse -- the handler for the player's own `Hit`
	GameOver<public>():void =
		...
		if (Overlay := Hud?):
			spawn{Overlay.ShowGameOver()}

	# hud.verse
	ShowGameOver<public>()<suspends>:void =
		ShowMessage("Game Over")
		if (Countdown := MessageTimer?):
			Countdown.Timeout().Await()
		if (Message := MessageLabel?):
			Message.SetText("Dodge the
Creeps")
			Message.Show()
		if (Tree := GetTree[], Wait := Tree.CreateTimer[1.0]):
			Wait.Timeout().Await()
		if (Button := StartButton?):
			Button.Show()
```

The `if (X := Y?)` lines are wall 1's residue rather than this wall's — the GDScript writes `$Name`
where the port holds an optional — and everything else is line for line. **The `hud_phase` enum, the
second Timer node, and both timeout handlers that existed only to read the phase are gone**; the one
timeout handler that remains is the plain one the GDScript also has, which hides the message.
`hud.verse` went from 55 lines of code to 44, and `hud.tscn` lost a node and a connection.

The second wait needs no node at all: `GetTree[].CreateTimer[1.0].Timeout()` is a mirrored accessor
like any other, which is the half of this wall that forced the extra node in the first place.

**Two things about the spelling are not what a GDScript author would guess, and both are language
facts rather than bridge choices.** An *awaiting* body cannot carry an effect specifier — Verse's own
`awaitable.Await` is `no_rollback`, exactly as `signalable.Signal` is — and a virtual **cannot** be
written `<suspends>`, because the specifier makes it a different function and the compiler answers
*"could not find a parent function to override"*. So the sequence is a named `<suspends>` method and
the handler reaches it with **`spawn`**.

**One thing had to change for that to be reachable**, and it is the connection between this wall and
wall 8. `GameOver` is a `Subscribe` handler, `Subscribe` fixed its callback at `<transacts>`, and a
`<transacts>` body may not `spawn` an awaiting one — so the chain `Hit.Subscribe(GameOver)` →
`GameOver` → `ShowGameOver` was blocked at the first link. Phase 5 widened `Subscribe`'s callback to
specifier-less, which is what Verse's own `subscribable` interface declares and what our comment
claiming to follow it got backwards. Measured: every existing `<transacts>` handler still satisfies
it, and nothing else in this port changed — `GameOver` is the one handler that dropped its
specifier, and it did so because it spawns.

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
				Mob.Set("position", VariantVector2(Spawn.Position))
				Mob.Set("rotation", VariantFloat(Direction))
				Mob.Set("linear_velocity", VariantVector2(Velocity))
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

A module-scope function with no effect specifier is `no_rollback`. So this compiles on its own:

```
	V2Length<public>(A:vector2):float = Sqrt(A.X * A.X + A.Y * A.Y)
```

and fails elsewhere with

    This invocation calls a function (`V2Length`) that has the 'no_rollback' effect, which is not
    allowed by its context.

— a message naming an effect the author never wrote down, pointing at a caller rather than at the
declaration that needs fixing.

**Why the context refuses it was recorded wrongly here, and Phase 4's probes corrected it.** This
document said a Godot callback runs inside a transaction and that this is what narrows the context.
It is not: the host's AutoRTFM transaction is a runtime arrangement the Verse effect checker cannot
see, and an override of `Ready` calling a specifier-less helper compiles perfectly well
(`tests/verse_probe`, and `docs/phase-4-design.md` §1.3). What actually refuses it is **failure**:

- a **failure context** — the condition of `if (X := F[])`, an option unwrap `Slot?`, a failable
  index — must be able to unwind, so what it invokes has to be rollbackable. A `no_rollback` callee
  is refused there and nowhere else;
- `<decides>` **alone does not make a function rollbackable**, because an explicit specifier replaces
  the default set rather than adding to it, and the default set contains `no_rollback`. A failable
  helper therefore needs `<decides><transacts>`, which is what `V2Normalized` carries;
- and *that* is the cascade: once `V2Normalized` says `<transacts>` it is narrowed, so its own call
  to a specifier-less `V2Length` is refused in turn. One failable helper pulls `<transacts>` onto
  everything it touches, which is why **every** function in `vectors.verse` carries it.

The port is full of failure contexts — every `?` unwrap of the seventeen inspector slots is one, and
so is every cast R-SCN-6 will add — so in this codebase the trap fires almost immediately, which is
what made the wrong explanation look right.

Nothing here is wrong, and the fix is one word. But it is the first thing a Godot author hits on
their first library file, and it is invisible in the file they have to change.

**Phase 4.5 did the two things the paragraph above asked for, and a third the paragraph did not
know to ask for.** The trap still exists — a helper that writes still needs `<transacts>` — but:

- ~~the **diagnostic says where the fix goes**~~ and ~~the **`.verse` template says it before it
  happens**~~. **Both were removed after the by-hand session** (`by-hand-findings.md` B6 and B7),
  and the second reason is the one worth keeping: the appended sentence keyed on glitch 3512 and
  the callee's package alone and never on *which* effect had been refused, so a `suspends` refusal
  from a Godot signal took the `transacts` branch and told the author to write the one word an
  awaiting body may not carry. It was confident, wrong, and had survived a phase. The rule now is
  that the bridge annotates a diagnostic only where the bridge is what the author is confused by —
  a skipped Godot member, a module that is not imported — and the compiler's own text stands
  everywhere else. The template is GDScript's, two comments long, for the same reason: six lines of
  caveat is not an introduction;
- and the cascade is **much shorter**, because reading Godot no longer starts it. `V2Length`'s
  problem was never Godot — it is pure arithmetic — but a helper that reads `Position` or calls
  `GetChildCount()` used to be forced to `<transacts>` by the read alone, and is now spellable as
  `<reads>`: Godot's 6728 const-and-answering methods carry that effect since Phase 4.5, and a
  `<reads>` function is callable from a `<transacts>` one. A helper that only *looks* at the scene
  no longer infects anything.

`vectors.verse` is gone, so the file this wall was measured on no longer exists; what would replace
it today is a `<reads>` file with no `<transacts>` in it at all.

**Phase 5 narrows it once more, in the place this port felt it most.** `hud.verse`'s own comment
records the trap arriving through a *signal handler* — "a `Subscribe` handler must be `<transacts>`,
an explicit specifier replaces the default set rather than adding to it… so a handler cannot call a
specifier-less method". With `Subscribe`'s callback widened to specifier-less (§3), that is no longer
true: a handler carries the default set like any other unspecified function, and the three methods
`main.verse` calls on the HUD stop needing the word. **What survives is narrower still** — a helper
that genuinely *writes* and is called from a genuinely narrowed body, which is a much smaller
population than "every helper a signal handler touches". `hud.verse`'s comment needs deleting with
the port's rewrite, and this row's state should be re-measured then rather than predicted here.

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
- **The Input singleton**: `GetInputSingleton()` then `IsActionPressed["move_right"]`. No
  `@GlobalScope` needed, because Godot's singletons are objects and Phase 2 generated an accessor
  for each. It was `GetInputSingleton[]` inside an `if` until the 39 singletons Godot registers
  before any scene loads became total (spec R-TYPE-4), which took a level of indentation off
  `_Process` and made the line the one a GDScript author would write.
- **Typed containers with objects in them**: `GetNodesInGroup("mobs")` is a `typed_array(node)`
  whose elements are nodes a script calls `QueueFree()` on. This is the gap Phase 2 named as the
  most-hit one in the mirror, and walking a group is the shape scene code actually has.
- **`variant` as a parameter**: `Set`, `SetDeferred` and the `Variant<GodotType>` builders carry
  the mob's spawn state and the player's deferred collision disable.
- **Enums and `@export_group`**: `packed_scene_gen_edit_state.Disabled` and
  `node_internal_mode.Disabled` are named values. The port used to declare an enum of its own too,
  `hud_phase`, and Phase 5 deleted it — a state machine for a sequence that can now be written as
  a sequence.
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


---

## After Phase 4

The re-port was the phase's exit gate, and it was done in place rather than beside the old one: one
yardstick, one maintenance burden, and each wall visibly falling. What the diff says, against the
port this document was originally written about:

- **`scripts/vectors.verse` is gone.** It was forty-six lines of hand-written arithmetic and three
  constants, and the file OQ-11 was opened about. `velocity.Normalized() * Speed` and
  `(Position + Velocity * Delta).Clamp(Vector2Statics.Zero, ScreenSize)` are ordinary Verse now.
- **Seventeen inspector slots became four lines of lookup.** `main` had nine object-typed exports
  and now has one — `MobScene`, which *should* be an export, because a PackedScene is a resource
  the designer chooses and not a child the script can look up. The others are `GetNode` and a cast
  in `_Ready`, which is what GDScript's `@onready var sprite = $AnimatedSprite2D` is.
- **The scene files stopped carrying what the scripts carry.** `node_paths=PackedStringArray(...)`
  is gone from all four, and so are the two connections that existed only because a script could
  not declare a signal: `Player.body_entered` to main's `GameOver`, and `HUD/StartButton.pressed`
  to main's `NewGame`. The player declares `Hit` and the HUD declares `StartGame`; main subscribes
  to both in `_Ready`. `GameOver` no longer takes a body it never looked at.
- **The mob is configured through its own properties.** `Object.set("linear_velocity", ...)` with a
  hand-built variant became `set Mob.LinearVelocity = Velocity`, behind one cast of what
  `PackedScene.instantiate` answered.
- **Godot's own randomness.** `randf_range` and `randi_range` rather than Verse's `GetRandomFloat`,
  which is R-AUD-2's one exception: a Verse-side RNG would silently ignore `seed()`, so a project
  that seeds for a replay would get a different game.

**The timer connections stayed in the scene files**, and that is not a wall: connecting a node's own
signal through the Node panel is what a Godot author does, and `Timer.Timeout().Subscribe(...)` is
now a spelling rather than the only one. The scene-file path is also the one the port is the
regression test for.

**Wall 8 fired again during the re-port**, exactly where §"8" says it would. `Subscribe` takes a
`<transacts>` callback, so `main.GameOver` and `main.NewGame` had to narrow — and narrowing them
refused their calls to `hud.ShowGameOver`, `hud.UpdateScore`, `hud.ShowMessage` and `player.Start`,
which carried the default (wider) effect set. Four declarations in two other files had to change,
and the compiler reported it at the *call* site each time. That is the trap Phase 4.5 inherits.

---

## After Phase 5

One file changed, which is the measurement: **wall 3 was the whole of what Phase 5 owed this
yardstick**, and closing it touched `hud.verse`, six lines of `main.verse` and two lines of
`hud.tscn`.

- **`hud.verse`: 55 lines of code to 44**, and the shape is the GDScript's again. Gone: the
  `hud_phase` enum, the `TitleTimer` node and its declaration, `OnTitleTimeout`, and the half of
  `OnMessageTimeout` that read the phase to decide which step it was in.
- **`hud.tscn` lost a node and a connection.** The scene is now the scene the original game ships.
- **`main.GameOver` dropped its `<transacts>`** and gained `spawn{Overlay.ShowGameOver()}`. That is
  the one line wall 8's narrowing still shows in this port, and it reads as what it is: this
  handler starts something that outlives it.
- **`headless_check.gd`'s two HUD checks now name what they are testing** — an await resuming,
  rather than a timer advancing a state machine. Both still pass at `--fixed-fps 60`, which is
  D7's rule holding: the port awaits Godot's timers and never `Sleep`, so it keeps the engine's
  clock and the checks stay frame-deterministic.

**Wall 8 did not fire this time**, and that is worth recording because it fired at every previous
re-port. Phase 4.5 made reading Godot `<reads>` and Phase 5 made a handler specifier-less; between
them, the one direction left that still forces the word is a helper that *writes* and is called
from a body that was narrowed on purpose. Nothing in this game is that shape.
