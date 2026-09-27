# Diagnostics

Every message that Godot Verse writes itself starts with an ID, such as `VG1002`. This page lists
every ID, the message it prints, and what you can do about it.

```
res://scripts/settings_resource.verse:75: VG1002: Maybe is an option around a value the inspector has no empty slot for. Only a node or a resource can be left unassigned.
```

A message from the Verse compiler has no ID. The editor shows the compiler's text as the compiler
wrote it. When Godot Verse adds a sentence to a compiler message, only the added sentence carries
an ID:

```
Unknown identifier `GetPosition`. VG5101: Godot has get_position, but it is reachable as the property `Position`.
```

In the message column, a word in braces, such as `{name}`, stands for the value the message fills
in: a member, a class, a path, or a number.

## How IDs are numbered

An ID is `VG` and four digits. The first digit is the area the message belongs to:

| IDs | Area |
| --- | --- |
| VG1xxx | `@export` members that the inspector can't show or save |
| VG2xxx | Signals. VG20xx is a declaration that Godot never learns about; VG21xx is a signal used at run time in a way that can't reach Godot |
| VG3xxx | `@rpc` configurations that Godot is never given |
| VG4xxx | Running a script and loading the host. VG40xx is a Godot call that Godot refused; VG41xx is finding, loading, and starting the host; VG42xx is the per-frame pump and runtime-error reporting |
| VG5xxx | Building and analyzing a project. VG50xx is names, modules, and the build; VG51xx explains a Godot member that the mirror doesn't carry; VG52xx is a missing `using` |
| VG6xxx | Editor tools: the debugger (VG60xx), the export plugin (VG61xx), **Make Verse Module** (VG62xx), and **Convert to Verse** (VG63xx) |
| VG7xxx | The interpreter that runs an exported game on the `vm` backend. VG70xx is loading the cooked data; VG71xx is running it |

An ID never changes and is never reused. If a message's meaning changes, it gets a new ID. If only
its wording improves, it keeps its ID.

## Adding a diagnostic

The registry is [`include/verse_diagnostics.def`](../include/verse_diagnostics.def). Both the
extension (`src/`) and the interpreter (`vm/`) print from it, through
[`include/verse_diagnostics.h`](../include/verse_diagnostics.h), so the two can't word one message
two ways.

To add a message:

1. Add a `VERSE_DIAG(id, where, text)` row to the registry, with the next free ID in its area.
1. Print it with `verse_diag_text` in `vm/`, or with `verse_diagnostic` in `src/`. Don't print the
   sentence directly.
1. Add a row to this page.
1. Assert the ID in a test. In `tools/run_tests.py`, `diag("VG1002", "Maybe")` matches the ID and
   the values it was said about, on one line, and not the wording.

`tests/verse_diagnostics/test_verse_diagnostics.py` fails if two rows share an ID or a message, if
an ID that code prints or a test asserts isn't in the registry, if a registered ID is never
printed, or if this page and the registry list different IDs.

The UE host (`host/`) raises the VG21xx and VG40xx runtime messages from the same registry, so
an editor session, a game on the `host` backend and one on the `vm` backend print the same
sentence with the same ID.

## Export: `@export` members (VG1xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG1001 | {name} is a {class}, and the inspector may leave that slot empty. Declare it `?{class}` so the member can hold the empty case. | Declare the member as an option, for example `?node2d`, and initialize it to `false`. |
| VG1002 | {name} is an option around a value the inspector has no empty slot for. Only a node or a resource can be left unassigned. | Export the plain value instead, or make the member an option around a node or a resource. |
| VG1003 | {name} carries {attribute}, which describes {wants} -- and this member is not one. The attribute exists because the declared type cannot say what a value is for, so the two have to be written to agree. | Remove the attribute, or change the member's type to the one the attribute describes. |
| VG1004 | {name} refers to {class}, which is neither a node nor a resource, so the inspector has nothing to draw for it. Derive {class} from a Godot class the inspector can pick one of. | Derive the class from a Godot node or resource class, or stop exporting the member. |
| VG1005 | {name} refers to {class}, which is a GDScript class, and `@export` cannot carry a GDScript class reached through a generated binding yet. Export its native base class `{native}` instead. | Declare the member with the named native base class instead of the GDScript class. |
| VG1006 | {name} refers to {class}, which is a GDScript class, and `@export` cannot carry a GDScript class reached through a generated binding yet. | Declare the member with a Godot class the inspector can draw instead of the GDScript class. |
| VG1007 | {name} has a type godot-verse cannot carry to the inspector yet, so it is not exported. | Change the member to a type the inspector can show, or remove `@export`. If you think the type should be supported, report it. |
| VG1008 | {name} can be assigned in the inspector but not saved. Its class is not the one named after its file, so it has no script -- and a value survives a save only through one. Saving writes an empty sub-resource and the member reloads empty. Move the class into a file of its own. | Move the member's class into its own `.verse` file, named after the class. |

## Signals (VG2xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG2001 | {name} is a `var`, and a signal is an identity rather than a value. Its binding is made once against the object the member was built on, so reassigning it leaves the name pointing at nothing. Drop the `var`. | Remove `var` from the signal's declaration. |
| VG2002 | {name} is not `<public>`, so nothing outside the class can connect to it -- which is the only thing connecting ever is. Declare it `{name}<public>`. | No action. A current host never reports this; if you see it, rebuild the host. |
| VG2003 | {name} is on a class that does not derive from `object`, so Godot never gives it an object to register the signal on. Unlike GDScript, where every class is an Object with a signal table of its own, a plain Verse class has no Godot counterpart at all. | Declare the signal on a class that derives from a Godot class, such as `node`. |
| VG2004 | {name} has a payload whose field `{field}` is itself a struct. A struct payload becomes one Godot argument per top-level field, and Godot has no argument shape for a struct, so there is no second level to flatten into. Flatten the field, or carry it as one of the mirrored math types. | Flatten the nested struct field into the payload, or use one of the mirrored math types. |
| VG2005 | {name} has a payload argument `{field}` with no Godot type, so an emission would have nothing to carry it in. | Change the payload argument to a type Godot can carry, such as `int`, `string`, or a Godot class. |
| VG2006 | {name} carries no `@export_signal`, so Godot is never told about it: it cannot be connected in the Node panel, emitted to, or seen from GDScript. The attribute is what registers a member, the way `@export` is what sends one to the inspector. Write `@export_signal` on the line above `{name}`. | Add `@export_signal` on the line above the member. |
| VG2007 | {name} cannot be registered with Godot, so nothing can connect to it. | Rebuild the host and the extension from the same commit. The host sent a reason this build doesn't know. |
| VG2101 | The signal `{signal}` was never registered with Godot: {reason} Nothing was emitted. | Fix the declaration the reason names. Its own ID says what is wrong with it. |
| VG2102 | Cannot subscribe to `{signal}`: {reason} | Fix the declaration the reason names. Its own ID says what is wrong with it. |
| VG2103 | Cannot await `{signal}`: {reason} | Fix the declaration the reason names. Its own ID says what is wrong with it. |
| VG2104 | The payload of signal `{signal}` has no representation on the Godot wire, so nothing was emitted. | Report it. The analysis should have refused this payload shape before the game ran. |
| VG2105 | A signal was emitted through an unbound `signal`. One a script built for itself rather than declared as a member of a class Godot instantiated names nothing, the way `godot_array{}` does. | Emit a signal that is a member of a class Godot instantiates, not one the script built for itself. |
| VG2106 | Subscribe was called on an unbound `signal`, which names nothing. | Subscribe to a signal that is a member of a class Godot instantiates. |
| VG2107 | Emit was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal. An event a script builds for itself is a Verse event and nothing more -- `Signal` is how tasks are resumed through one. | Mark the event member `@export_signal`, or call its `Signal` method to resume Verse tasks only. |
| VG2108 | Subscribe was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal. | Mark the event member `@export_signal`. |
| VG2109 | Await was called on an unbound `signal`, which names nothing and so will never be emitted. | Await a signal that is a member of a class Godot instantiates. |
| VG2110 | Await was called on a Signal value that names no object and signal. | Build the value with `MakeSignal(Owner, Name)` from an object and a signal that exist. |
| VG2111 | Subscribe was called on a Signal value that names no object and signal. | Build the value with `MakeSignal(Owner, Name)` from an object and a signal that exist. |
| VG2112 | Subscribe was given a Verse function that is not a method bound to a live script instance, which is the only shape a Godot Callable can carry without outliving what it names. | Pass a method of a live script instance, such as `Self.OnScored`. |
| VG2113 | The signal `{signal}` was registered but could not be connected, so awaiting it would never resume. | Report it, with the scene that produced it. The member was registered and Godot refused the connection. |

## Remote procedure calls: `@rpc` (VG3xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG3001 | {name}: `{word}` is not an @rpc word. It must be one of "call_local"/"call_remote" (local calls), "any_peer"/"authority" (permission), or "reliable"/"unreliable"/"unreliable_ordered" (transfer mode). | Use one of the listed words, separated by spaces, in the `@rpc` string. |
| VG3002 | {name}: {category} is given twice. Each of the three may be said no more than once. | Remove the repeated word, so each category appears once. |
| VG3003 | {name}: @rpc wants {wanted} in this position. | Pass the kind of value the message names in that position. |

## Running and loading the host (VG4xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG4001 | {verb} `{member}` on Godot object {handle}, which Godot has already freed. Test IsInstanceValid[...] before reaching through a reference the scene may have dropped. | Test the reference with `IsInstanceValid[...]` before you use it. |
| VG4002 | {verb} `{member}` on Godot object {handle}, and the value has no representation on the Verse bridge. This is a gap in the type table in tools/gen_verse_api.py. | Report it. The type table in `tools/gen_verse_api.py` has a gap. |
| VG4003 | {verb} `{member}` on Godot object {handle} with the wrong number of arguments. The generated Verse mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py. | Regenerate the mirror with `python tools/gen_verse_api.py` against the Godot you run. |
| VG4004 | {verb} `{member}` on Godot object {handle}, which has no such member. The generated Verse mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py. | Regenerate the mirror with `python tools/gen_verse_api.py` against the Godot you run. |
| VG4005 | {verb} a Godot container that names nothing. A container built in Verse -- `godot_array{}` and the like -- holds no Godot value; one has to come back from Godot. | Get the container from a Godot call. A container built in Verse holds no Godot value. |
| VG4006 | {verb} a Godot container the bridge no longer holds (reference {ref}). A reference is released when the Verse value holding it is collected, so this is a handle kept past the object that owned it. | Don't keep a container reference after the Verse value that held it is gone. |
| VG4007 | Godot returned a value tagged {tag} where the Verse bridge expected `{expected}`. The type table in tools/gen_verse_api.py and this build of Godot disagree. | Regenerate the mirror against the Godot you run. |
| VG4008 | Called `{member}` on Godot object {handle}, but this embedder cannot call Godot methods. | Run the script in an embedder that installs Godot's callbacks. |
| VG4009 | Godot has no method `{member}` on the value reference {ref} names. | Check the method name against the Godot type the value holds. |
| VG4010 | Godot would not make a `{class}`, so this class has no object to be. A Godot class that is abstract, or that the engine only ever hands out as a singleton, cannot be constructed -- derive from one that can, or reach the singleton through its accessor. | Derive from a Godot class that can be instantiated, or reach the singleton through its accessor. |
| VG4011 | {entry} was called from a thread other than the one Verse runs on, so it did not run. Call it from the main thread. | Call the method from the main thread, for example with `call_deferred`. |
| VG4101 | no Unreal checkout is configured, so there is no host to load. Set UE_ROOT to the checkout that built {dll}, or set Editor Settings > Verse > Host > Engine Dir to the same directory. | Set `UE_ROOT`, or set **Editor Settings > Verse > Host > Engine Dir**. |
| VG4102 | {setting} is set in project.godot, which commits one machine's paths to everyone who clones this project. Move it to Editor Settings > Verse > Host, or set UE_ROOT, and delete the [verse] section. It is still read for now. | Move the setting to **Editor Settings > Verse > Host**, or set `UE_ROOT`, and delete the `[verse]` section from `project.godot`. |
| VG4103 | verse/runtime/backend is 'vm', but this build has no interpreter compiled in (build with `scons verse_vm=yes`); using the host instead. | Build the extension with `scons verse_vm=yes`, or set `verse/runtime/backend` to `host`. |
| VG4104 | Verse needs the vm backend on Web, and this game was exported with verse/runtime/backend.web set to "{backend}": the UE host is a native DLL a browser cannot load. | Set `verse/runtime/backend.web` to `vm` and export again. |
| VG4105 | failed to load host library: {error} | Check that the host DLL exists at the configured path and was built for this machine. |
| VG4106 | Verse could not load {dll}: {error} | Export the project again so that the host DLL ships beside the game. |
| VG4107 | {dll} is {kind}; the Godot editor needs the editor host. Build it with `python tools/build_host.py`. | Build the editor host with `python tools/build_host.py`. |
| VG4108 | {dll} is {kind}; an exported game needs the runtime host. Build it with `python tools/build_host.py --target VerseHostRuntime`. | Build the runtime host with `python tools/build_host.py --target VerseHostRuntime` and export again. |
| VG4109 | Verse shipped the wrong host: {dll} is {kind}. | Build the runtime host and export again. |
| VG4110 | The previous run ended in a Verse host fatal error. What it recorded:\n{record} | Read the record that follows the message. It is what the host wrote before the process ended. |
| VG4111 | vh_init failed with status {status} | Read the error printed before this one; the host said why there. |
| VG4112 | Verse could not start (vh_init returned {status}). | Rebuild the host and the extension from the same commit, and export again. |
| VG4113 | The game ended in a Verse host fatal error:\n{record} | Read the record that follows the message. It is what the host wrote before the game ended. |
| VG4114 | VerseRuntime singleton is not available | Check that the extension loaded. Look for an earlier error from `VerseRuntime`. |
| VG4201 | tick called with no host loaded | Report it. The frame pump ran before a host was loaded. |
| VG4202 | the frame budget ({budget} ms, verse/runtime/frame_budget_ms) ran out with {jobs} queued job(s) left. They run next frame. The budget governs queued work only -- a task awaiting a Godot signal resumes inside the emission and is not budgeted. | Raise `verse/runtime/frame_budget_ms`, or queue less work per frame. |
| VG4203 | {count} more stack trace(s) from this error were dropped. | No action. It counts the repeats of the error above it. |
| VG4204 | The call that raised this was rolled back, so what it changed before the error is undone, except by the Godot methods listed in docs/nonatomic-methods.md. An error in a function Godot calls every frame can repeat for that reason. | No action. It explains that the failing call's changes were undone. |

## Building and analyzing a project (VG5xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG5001 | "{name}" is not a Verse module name, so this directory is not a module and its scripts stay in the one above it. A module name is a letter or underscore followed by letters, digits or underscores -- gameplay.vmodule, not my-stuff.vmodule. | Rename the `.vmodule` file to a Verse identifier. The directory can keep its name. |
| VG5002 | {path} and {other} both declare `{name}` in {module}. A name may only be declared once per module. Put one of them in a module of its own -- right-click its directory in the FileSystem dock and choose "Make Verse Module" -- or rename one of the files. | Move one file into its own module with **Make Verse Module**, or rename one of the classes. |
| VG5003 | {path} and {other} both register the Godot class name `{name}`. ClassDB is one flat namespace and a module is deliberately not part of it, so two @global_class classes may not share a name however far apart they are. Rename one of the files. | Rename one of the files, so that each `@global_class` class has its own name. |
| VG5004 | `@global_class` on `{name}` registers nothing. Godot collects one global class per script file, and only `{stem}` -- the class named after this file -- can be that class. Move `{name}` into a file of its own to register it, or drop the attribute: a member typed as `{name}` still exports, filtered by its nearest Godot base class. | Move the class into its own `.verse` file, or remove `@global_class`. |
| VG5005 | `{name}` extends `{base}`, which is the generated binding for a class a *script* declares. That cannot work: Godot gives an object exactly one script instance, so the inherited methods would forward to a `{base}` that is not there -- `{name}`'s own script is the only one the node has (R-INT-6, R-INT-10). Extend the binding's own Godot base instead and hold the other node, or move the shared code into Verse. | Extend the binding's Godot base class instead, or move the shared code into Verse. |
| VG5006 | {path} derives from `{base}`, and more than one script answers to that name ({candidates}). The build resolves it through modules; the class picker cannot, so until this build finishes it offers Node as this script's base type. | No action. The build resolves the base; the class picker shows Node until it finishes. |
| VG5007 | this build ran against an incomplete binding roster, so its errors are withheld for one pass. {classes} is declared with no members, because the script it stands for names a Verse class -- loading it to describe it while a .verse is loading would be a cyclic load, so it is held back instead. A Verse file that names one of its *methods* does not compile on this pass, and a node that loses its script over it is reported by GDScript as a null value with nothing said about Verse. The next build describes it in full; what no build can repair is a node the scene has already finished instantiating. Reach the method through `Call`/`Callv` (R-INT-2) rather than naming it, and the cycle is broken. | Call the GDScript method through `Call` or `Callv` instead of naming it. |
| VG5008 | This file compiles, but {files} {verb}, so the project will not build and Play will be refused. Open {pronoun} to see why, or Project > Tools > Build Verse to log every error at once. | Open the named files and fix their errors. |
| VG5009 | the project did not build, so no new code was published. The editor's analysis -- diagnostics, completion and the shape of the exported properties -- is live either way; fix the errors and build again to replace what is running. | Fix the errors above this message and build again. |
| VG5101 | Godot has {member}, but it is reachable as the property {detail}. | Use the property the message names. |
| VG5102 | Godot has {member}, but a Verse function already answers to that name, so it is the property {detail}. | Use the property the message names. |
| VG5103 | Godot has {member}, but it is reachable as {detail}. | Use the name the message gives. |
| VG5104 | Godot has {member}, but it is reachable as {detail}, which is also what string interpolation uses. | Use the function the message names. |
| VG5105 | Godot has {member}, but it cannot be a property, so Godot's own {detail} carry it instead. | Use the getter and setter the message names. |
| VG5106 | Godot has {member}, but it is a Godot virtual returning {detail}, and an unoverridden virtual has to answer a value there is no way to write (R-NODE-7). | No Verse spelling exists yet (R-NODE-7). Report the virtual you need. |
| VG5107 | Godot has {member}, but it is static, and a static call has no Verse spelling yet (R-NODE-4). | No Verse spelling exists yet (R-NODE-4). Call it from GDScript, or report that you need it. |
| VG5108 | Godot has {member}, but it takes a variable number of arguments, which the bridge cannot carry. | No Verse spelling exists. Call it from GDScript. |
| VG5109 | Godot has {member}, but it takes a raw C pointer, which no scripting language can pass. | No action. No scripting language can pass a raw pointer. |
| VG5110 | Godot has {member}, but nothing can carry its {detail} across the boundary. | Report the type, so that the bridge can carry it. |
| VG5111 | Godot has {member}, but a name it shares with an inherited member won. | Use the inherited member that took the name. |
| VG5112 | Godot has {member}, but the math types are ordinary Verse rather than calls into Godot, and this one has not been written yet -- host/Verse/GodotMath.native.verse is where it goes. | Write the method in `host/Verse/GodotMath.native.verse`, or report it. |
| VG5113 | Godot has {member}, but Verse spells it {detail}. | Use the Verse spelling the message gives. |
| VG5114 | Godot has {member}, but its parameter or result is a Variant, which a script cannot spell -- the packers are module-scoped by R-TYPE-7, so there is no signature for it to have. | No action. A script can't spell a Variant. |
| VG5115 | Godot has {member}, but it has no Verse counterpart and is not dispatched to Godot yet. | Report it. A newer Godot added a utility the generator doesn't classify. |
| VG5116 | Godot has {member}, but this operator has not been written for those operands yet -- the math types are ordinary Verse, and host/Verse/GodotMath.native.verse is where it goes. | Write the operator in `host/Verse/GodotMath.native.verse`, or report it. |
| VG5117 | Godot has {member}, but it was skipped: {reason}. | Report it. The generator recorded a reason this build doesn't explain. |
| VG5201 | It is declared in a module this file does not import; add {using} at the top of the file. | Add the `using` line the message gives. The editor adds it for you when the file is open. |
| VG5202 | It is declared in more than one module, so which was meant is yours to say; add {using} at the top of the file. | Add the `using` line for the module you meant. |

## Editor tools (VG6xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG6001 | Godot's debugger is active, but the Verse VM already has a debugger attached -- verse/host/enable_debugger opened Epic's socket debugger at startup. Breakpoints in the script editor will not fire. Turn that setting off to debug through Godot instead. | Turn off `verse/host/enable_debugger` to debug through Godot. |
| VG6101 | Verse needs the vm backend on Web, and this preset reads verse/runtime/backend as "{backend}": the UE host is a native DLL a browser cannot load. Set verse/runtime/backend.web to "vm" in Project Settings, or remove the override that changed it. | Set `verse/runtime/backend.web` to `vm` in **Project Settings**. |
| VG6102 | Could not rewrite {path} to drop the host DLL for the vm backend; the export will carry it anyway. | Check that the `.gdextension` file is writable. The export still works; it ships the host DLL too. |
| VG6103 | Verse does not export to {platform} yet, and this project has Verse scripts in it. See docs/phase-7-design.md §14. | Export to Windows or Web. |
| VG6104 | Verse cannot tell which platform this export is for; no platform feature tag was set. | Check the export preset. It carries no platform feature tag. |
| VG6105 | The Verse project did not compile, so nothing was cooked. Fix the errors in the Output panel and export again. | Fix the errors in the **Output** panel and export again. |
| VG6106 | The Verse cooker is not at {path}. Build it with `python tools/build_host.py --target VerseHostCooker`, or set Editor Settings > Verse > Host > Cooker Path to it. | Build the cooker with `python tools/build_host.py --target VerseHostCooker`, or set **Editor Settings > Verse > Host > Cooker Path**. |
| VG6107 | Could not write {path} | Check that the export cache directory is writable. |
| VG6108 | verse_cook exited {status}; the export carries no Verse. To see the engine's own log, run it by hand: "{cooker}" "{manifest}" "{work}" --verbose | Run the command the message gives to see the cooker's own log. |
| VG6201 | {marker} already exists, so this directory is already a Verse module. | No action. The directory is already a module. |
| VG6202 | Could not create {marker}. | Check that the directory is writable. |
| VG6203 | {marker} is named after its directory, and `{name}` is not a Verse module name. Rename the file -- a letter or underscore followed by letters, digits or underscores -- and the directory can keep the name it has. | Rename the `.vmodule` file to a Verse identifier. |
| VG6301 | could not write {path}. | Check that the file is writable, and convert it again. |

## The interpreter (VG7xxx)

| ID | Message | What to do |
| --- | --- | --- |
| VG7001 | This runtime runs cooked Verse only, and vh_init was given no cooked directory. | Report it. The interpreter runs only an exported game's cooked data. |
| VG7002 | Verse data not found at {path}. The export is incomplete; export the project again. | Export the project again. |
| VG7003 | This game's Verse data was cooked by a different build of godot-verse (cooked {cooked}, host {host}/verse_vm). Export the project again. | Export the project again with this build of godot-verse. |
| VG7004 | {program} and {sidecar} were written by different cooks. Export the project again. | Export the project again. |
| VG7005 | {path} was written by sidecar version {version}; this host reads version {wanted}. Re-export the project. | Export the project again with this build of godot-verse. |
| VG7006 | {path} is not a valid class sidecar: {what}. | Export the project again. If it repeats, report it. |
| VG7007 | {path} is not valid JSON | Export the project again. If it repeats, report it. |
| VG7010 | {path} is not a Verse program: it does not begin with VBC1. | Export the project again. |
| VG7011 | {path} is format version {version}; this runtime reads version {wanted}. Export the project again. | Export the project again with this build of godot-verse. |
| VG7012 | {path} was written for another Verse op set (schema {schema}); this runtime reads schema {wanted}. Export the project again with a matching build. | Export the project again with a build of godot-verse made from the same engine commit. |
| VG7013 | {path} uses the op {op} in {procedure}, which this runtime does not implement. | Report it, with the procedure named in the message. |
| VG7014 | {path} names no {definition} definition, which this runtime needs. Export the project again. | Export the project again. |
| VG7015 | {path} is malformed: {what}. | Export the project again. If it repeats, report it. |
| VG7016 | {path} is truncated or malformed: {what}. | Export the project again. If it repeats, report it. |
| VG7101 | The result of {function} could not be handed to the host: {why}. | Report it, with the function named in the message. |
| VG7102 | The native function {native} is not implemented by this runtime. | Report it, with the native function named in the message. |
| VG7103 | VM invariant violated: {what} at {where} | Report it, with the location in the message. |
| VG7104 | This runtime cannot run {what} yet: {where} | Report it. The interpreter can't run that construct yet. |
| VG7105 | Stage-1 interpreter cannot wait: {where} needs a value that is not yet known | Report it, with the location in the message. |
