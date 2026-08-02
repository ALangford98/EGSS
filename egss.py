#!/usr/bin/env python3
"""One entry point for building and running EGSS.

    ./egss.py                 build debug and run it
    ./egss.py build           build debug
    ./egss.py build release   build release
    ./egss.py build all       build all three configs
    ./egss.py run [config]    build, then run from the binary's own directory
    ./egss.py clean [config]
    ./egss.py gen             regenerate project files only

Why this exists
---------------
Regenerating project files takes about 0.2 seconds -- the same as a no-op
build. That is cheap enough that this script simply always does it, which
deletes the single most confusing failure in the project: premake expands its
file globs at *generation* time, so a newly added .cpp is invisible to the
build until it is regenerated. The symptom is an undefined-symbol error for a
function that is plainly right there in the file you just wrote.

Pass --no-gen if you ever want to skip it.

`run` matters too: the executable reads and writes imgui.ini and profile.json
relative to the working directory, so it has to be launched from beside the
binary or it will quietly use a different layout file each time.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
CONFIGS = ["debug", "release", "dist"]
IS_WINDOWS = platform.system() == "Windows"


def run(command, **kwargs):
    """Run a command in the project root, echoing it first."""
    print("\033[90m$ " + " ".join(command) + "\033[0m")
    return subprocess.call(command, cwd=ROOT, **kwargs)


def premake_path():
    name = "premake5.exe" if IS_WINDOWS else "premake5"
    path = os.path.join(ROOT, "vendor", "bin", "premake", name)

    if not os.path.isfile(path):
        sys.exit(
            f"premake5 not found at vendor/bin/premake/{name}\n"
            "It is gitignored, so a fresh clone will not have it.\n"
            "Download it from https://premake.github.io/download"
        )

    return path


def generate():
    """Regenerate project files. Cheap, so it is not conditional.

    Output is swallowed unless it fails. Because this now runs on every build,
    premake's banner -- including a deprecation notice about the gmake2 action
    on newer versions -- would otherwise be printed dozens of times a day. The
    action name is left as gmake2 deliberately: newer premake accepts it with a
    warning, older premake needs it, and the warning is not worth breaking
    anyone's setup over.
    """
    action = "vs2022" if IS_WINDOWS else "gmake2"
    command = [premake_path(), action]

    print("\033[90m$ " + " ".join(command) + "\033[0m")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)

    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)

    return result.returncode


def build(config, jobs, generate_first=True):
    if generate_first and generate() != 0:
        return 1

    if IS_WINDOWS:
        # Untested from here -- developed on Linux. If msbuild is not on PATH,
        # open EGSS.sln in Visual Studio and build normally.
        msbuild = shutil.which("msbuild")
        if not msbuild:
            print("msbuild not on PATH -- open EGSS.sln in Visual Studio instead.")
            return 1

        return run([msbuild, "EGSS.sln", f"/p:Configuration={config.capitalize()}",
                    "/p:Platform=x64", f"/m:{jobs}"])

    return run(["make", f"-j{jobs}", f"config={config}"])


def binary_dir(config):
    system = "windows" if IS_WINDOWS else platform.system().lower()
    return os.path.join(ROOT, "bin", f"{config.capitalize()}-{system}-x86_64", "TestEnv")


def launch(config):
    directory = binary_dir(config)
    executable = os.path.join(directory, "TestEnv.exe" if IS_WINDOWS else "TestEnv")

    if not os.path.isfile(executable):
        sys.exit(f"No binary at {executable} -- build it first.")

    print("\033[90m$ cd " + directory + " && ./TestEnv\033[0m")

    # From its own directory, so imgui.ini and profile.json land beside it.
    return subprocess.call([executable], cwd=directory)


def main():
    parser = argparse.ArgumentParser(description="Build and run EGSS.")
    parser.add_argument("command", nargs="?", default="run",
                        choices=["build", "run", "clean", "gen"])
    parser.add_argument("config", nargs="?", default="debug",
                        choices=CONFIGS + ["all"])
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--no-gen", action="store_true",
                        help="skip regenerating project files")
    args = parser.parse_args()

    if args.command == "gen":
        return generate()

    configs = CONFIGS if args.config == "all" else [args.config]

    if args.command == "clean":
        for config in configs:
            run(["make", f"config={config}", "clean"])
        return 0

    # Generate once even when building several configs.
    if not args.no_gen and generate() != 0:
        return 1

    for config in configs:
        if build(config, args.jobs, generate_first=False) != 0:
            return 1

    if args.command == "run":
        if args.config == "all":
            sys.exit("Pick one config to run.")
        return launch(args.config)

    return 0


if __name__ == "__main__":
    sys.exit(main())
