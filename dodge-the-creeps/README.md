# Dodge the Creeps, in Verse

Godot's canonical first game, ported to Verse. The game is five `.verse` files under `scripts/` and
four scenes; there is no GDScript in it.

It is the yardstick [`docs/roadmap.md`](../docs/roadmap.md) set for this project, and
[`docs/dodge-the-creeps.md`](../docs/dodge-the-creeps.md) is the point of it: the eight places a
Godot author's habits have no spelling yet, each mapped to the requirement that will give them one.
Read that before reading the scripts, or the workarounds look like style.

## Running it

Point the two `verse/host/*` settings in `project.godot` at your own Unreal checkout first — they
name an absolute path and nothing portable can be committed. Then `scons target=editor` puts the
GDExtension in `addons/`, and:

    godot --path dodge-the-creeps

Without a window:

    godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd

`headless_check.gd` is the only GDScript here and is not part of the game: it presses Start, holds a
key, kills the player and asserts on what Godot sees of the Verse scripts. `--fixed-fps` is not
optional — headless, a `Timer` counts real seconds while the main loop runs as fast as it can.

The first run in a fresh checkout needs an import pass, which the editor does on open or
`godot --headless --path dodge-the-creeps --import` does without one.

## Licences

This is a port of [`2d/dodge_the_creeps`](https://github.com/godotengine/godot-demo-projects) from
Godot's demo projects, MIT licensed, © 2017 KidsCanCode — `LICENSE` is that licence, and the scenes
and assets here are its work. The scripts are this repository's.

`art/House In a Forest Loop.ogg` © 2012 [HorrorPen](https://opengameart.org/users/horrorpen),
[CC-BY 3.0](https://creativecommons.org/licenses/by/3.0/). Source:
https://opengameart.org/content/loop-house-in-a-forest

Images are from "Abstract Platformer", created in 2016 by kenney.nl,
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/). Source:
https://www.kenney.nl/assets/abstract-platformer

The font is "Xolonium", © 2011-2016 Severin Meyer, with Reserved Font Name Xolonium, SIL Open Font
License 1.1 — details in `fonts/LICENSE.txt`.
