#!/usr/bin/env python
import os
import subprocess
import sys

from SCons.Variables import BoolVariable

from methods import print_error, print_warning
from gdextension import generate as generate_gdextension, library_filename, verify_shlib_affixes

localEnv = Environment(tools=["default"], PLATFORM="")

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)
opts.Add(BoolVariable(
    "verse_vm", "Compile vm/ into the library and enable the vm backend "
    "(implied by platform=web, and by platform=windows target=template_release)", False))
opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:

    git submodule update --init --recursive""")
    sys.exit(1)

# godot-cpp carries a dump per Godot version and will not pick one for you: `api_version` is
# exported into its SConscript, which turns it into gdextension/extension_api-4-7.json. Bumping
# Godot means bumping this, the submodule if the dump is not in it yet, and compatibility_minimum
# in godot-verse.gdextension.in, and then regenerating the mirror.
env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs, "api_version": "4.7"})

if env.get("is_msvc", False):
    env["CXXFLAGS"].remove("/std:c++17")
    env["CXXFLAGS"].insert(0, "/std:c++20")
    env["CXXFLAGS"].insert(0, "/Zc:preprocessor")
else:
    env["CXXFLAGS"].remove("-std=c++17")
    env["CXXFLAGS"].insert(0, "-std=c++20")

env.Append(CPPPATH=["src/", "include/"])
sources = Glob("src/*.cpp")
# "Convert to Verse" is editor-only and carries a 3 MB table of the mirror (verse_gd_api.gen.h), so
# an export template -- which has no editor to offer it in -- is built without it.
if env["target"] != "editor":
    sources = [s for s in sources if not s.name.startswith(("verse_gd_", "verse_convert_menu"))]

# vm/ is the clean-room interpreter (docs/phase-7.5-design.md §2, §9): a godot-cpp-free library
# that also implements the runtime subset of the vh_* ABI. Web cannot LoadLibraryExW a host DLL at
# all (verse_host.cpp compiles that path out under #ifdef _WIN32), so it always needs vm/ built in;
# everywhere else it is opt-in with verse_vm=yes. VERSE_VM_STATIC is what lets src/ fill
# VerseHostLibrary directly from vm/'s functions instead of resolving them with GetProcAddress
# (src/verse_host.cpp's load_static). VERSE_HOST_IMPLEMENTATION is deliberately never defined here:
# that would export the vh_* symbols from this library with dllexport, which only
# tools/build_verse_vm.py's standalone DLL wants.
#
# Windows' template_release carries it too, on by default rather than opt-in there: it costs
# ~565 KiB as of T5.4 (measured: 2,969,600 bytes without it, 3,548,160 with -- vm/ is still
# growing, so treat the delta as approximate) and changes nothing about the host backend, which
# VERSE_VM_STATIC only sits beside -- verse_host.cpp's DLL loader is not compiled out on Windows
# the way it is on web. One release build this way carries both backends (T5.3), so
# `scons target=template_release` alone is enough for an export to choose either at
# `verse/runtime/backend`, and the export layer's default host-backend run needs nothing extra.
verse_vm = bool(env["verse_vm"]) or env["platform"] == "web" or \
    (env["platform"] == "windows" and env["target"] == "template_release")
if verse_vm:
    env.Append(CPPPATH=["vm/"])
    env.Append(CPPDEFINES=["VERSE_VM_STATIC"])
    sources += Glob("vm/*.cpp")

libname = "godot-verse"
addondir = "addons"
plugindir = f"{addondir}/{libname}"
libdir = f"{plugindir}/bin"
projectdir = "demo"

platformdir = f"{libdir}/{env['platform']}" if env["arch"] == "universal" else f"{libdir}/{env['platform']}-{env['arch']}"

verify_shlib_affixes(env)
lib_filename = library_filename(env["platform"], env["target"], env["precision"], env["threads"], libname)
build_plugin_action = env.SharedLibrary(
    f"{projectdir}/{platformdir}/{lib_filename}",
    source=sources,
)

# The libraries section is derived from the binaries present in bin/
# Always run, since a previous invocation may have left a library for another target behind
def write_gdextension(target, source, env):
    generate_gdextension(f"{projectdir}/{plugindir}", str(source[0]))

gdextension_action = env.Command(
    f"{projectdir}/{plugindir}/{libname}.gdextension",
    f"{libname}.gdextension.in",
    env.Action(write_gdextension, f"Generating {libname}.gdextension..."),
)
env.AlwaysBuild(gdextension_action)
env.Depends(gdextension_action, build_plugin_action)

copy_output_action = env.Install(addondir, f"{projectdir}/{plugindir}")
env.Depends(copy_output_action, build_plugin_action)
env.Depends(copy_output_action, gdextension_action)

# The port is the second committed project (docs/dodge-the-creeps.md), and a project cannot
# reference an addon above its own res://, so it gets the same copy demo/ gets. tests/integration
# is not here because tools/run_tests.py copies it in per run.
port_output_action = env.Install(f"dodge-the-creeps/{addondir}", f"{projectdir}/{plugindir}")
env.Depends(port_output_action, copy_output_action)

actions = [
    build_plugin_action,
    gdextension_action,
    copy_output_action,
    port_output_action,
]
Default(*actions)

# The UE commit the three host targets are built and measured against. Nothing under src/ compiles
# against UE, so this is a warning and never an error -- what it catches is a checkout that moved
# under binaries nothing else compares. VH_BUILD_HOST_ID digests host/ and the ABI header alone, so
# a host rebuilt on a different engine keeps the same id and a cook taken on one engine loads into a
# runtime host built on another without a word (docs/phase-7b-design.md §13.5).
ENGINE_COMMIT = "203d76492ebc201b95a505d16cbc045d5a38d37d"


def engine_head(engine):
    """The checkout's HEAD, or None when git cannot answer for it -- no git, or not a checkout."""
    try:
        result = subprocess.run(["git", "-C", engine, "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def warn_on_engine_mismatch():
    """Says so when the Unreal checkout is not at ENGINE_COMMIT.

    Resolution order is tools/run_tests.py's, less the flag it has and this does not: $UE_ROOT, then
    ../UnrealEngine. A machine with no checkout is silent rather than nagged, because the
    GDExtension builds without one and only the host targets need an engine at all.
    """
    root = os.environ.get("UE_ROOT")
    if root and not os.path.isdir(root):
        print_warning(f"UE_ROOT is {root}, which is not a directory, so the engine commit went unchecked.")
        return
    engine = root or os.path.join(Dir("#").abspath, os.pardir, "UnrealEngine")
    if not os.path.isdir(engine):
        return
    head = engine_head(engine)
    if head is None or head == ENGINE_COMMIT:
        return
    print_warning(
        f"the Unreal checkout at {os.path.normpath(engine)} is at {head[:10]}, not the "
        f"{ENGINE_COMMIT[:10]} this repo expects. Any host binary in bin/ was built against a "
        f"different engine: rebuild with tools/build_host.py, or update ENGINE_COMMIT in SConstruct "
        f"if the move is deliberate.")


# Last, so that godot-cpp's own configuration output does not bury it.
warn_on_engine_mismatch()
