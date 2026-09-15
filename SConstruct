#!/usr/bin/env python
import os
import sys

from methods import print_error
from gdextension import generate as generate_gdextension, library_filename, verify_shlib_affixes

localEnv = Environment(tools=["default"], PLATFORM="")

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)
opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:

    git submodule update --init --recursive""")
    sys.exit(1)

# The API dump lives here, not in the submodule: godot-cpp's pinned commit carries 4.6 and
# a modified submodule is reverted by the `git submodule update` the message above asks for.
env["gdextension_dir"] = os.path.abspath("gdextension")

env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

if env.get("is_msvc", False):
    env["CXXFLAGS"].remove("/std:c++17")
    env["CXXFLAGS"].insert(0, "/std:c++20")
    env["CXXFLAGS"].insert(0, "/Zc:preprocessor")
else:
    env["CXXFLAGS"].remove("-std=c++17")
    env["CXXFLAGS"].insert(0, "-std=c++20")

env.Append(CPPPATH=["src/", "include/"])
sources = Glob("src/*.cpp")

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
