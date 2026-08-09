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

premake5 itself is gitignored, so a fresh clone and every new worktree used to
fail on the first build with a link to go and download it. It is now fetched
automatically -- a pinned version, checksummed before it is unpacked. Pass
--no-fetch to get the old failure instead.

`run` matters too: the executable reads and writes imgui.ini and profile.json
relative to the working directory, so it has to be launched from beside the
binary or it will quietly use a different layout file each time.
"""

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
CONFIGS = ["debug", "release", "dist"]
IS_WINDOWS = platform.system() == "Windows"

# Pinned rather than "latest" so two clones cannot generate project files with
# different premake versions and disagree about the result. beta7 is what the
# project has been developed against; the extracted binary is byte-identical to
# the one that was previously downloaded by hand (sha256 5e1a55dc...), so
# pinning it changed nothing about the build.
PREMAKE_VERSION = "5.0.0-beta7"
PREMAKE_BASE_URL = (
    "https://github.com/premake/premake-core/releases/download/v" + PREMAKE_VERSION
)

# Keyed by platform.system(). The archive checksum, not the binary's -- it is
# what can be checked before anything is unpacked or made executable.
#
# These were recorded by downloading each archive once. That protects against a
# corrupted or substituted download later; it is not an independent audit of
# what upstream published on the day.
PREMAKE_ARCHIVES = {
    "Linux": (
        f"premake-{PREMAKE_VERSION}-linux.tar.gz",
        "805114ae7002fe90b643630aa0947476565071bfcefa237d97f95df836e0f2f1",
        "premake5",
    ),
    "Darwin": (
        f"premake-{PREMAKE_VERSION}-macosx.tar.gz",
        "f7a6d4978960aefbc0b2c522f211eedf014e6a2783b04a096c3716213c1bff67",
        "premake5",
    ),
    "Windows": (
        f"premake-{PREMAKE_VERSION}-windows.zip",
        "8baec7b265fd43050f006ef47000457fa93b6fafbb1ead7e333e862a467c2e95",
        "premake5.exe",
    ),
}


def run(command, **kwargs):
    """Run a command in the project root, echoing it first."""
    print("\033[90m$ " + " ".join(command) + "\033[0m")
    return subprocess.call(command, cwd=ROOT, **kwargs)


def fetch_premake(dest_dir, binary_name):
    """Download and unpack the pinned premake into dest_dir.

    The binary is gitignored, so a fresh clone and every new worktree started
    without one. That failed at the first build with a message telling you to
    go and find it yourself, which is a poor first five minutes -- and the
    submodules have exactly the same problem, so the failure usually arrived
    twice.
    """
    system = platform.system()
    entry = PREMAKE_ARCHIVES.get(system)

    if entry is None:
        sys.exit(
            f"No premake build is pinned for {system}.\n"
            "Add one to PREMAKE_ARCHIVES, or drop a premake5 binary into "
            "vendor/bin/premake/ by hand."
        )

    archive_name, expected_sha, member = entry
    url = f"{PREMAKE_BASE_URL}/{archive_name}"

    print(f"premake5 not found; fetching {PREMAKE_VERSION} from {url}")

    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, archive_name)

        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                payload = response.read()
        except Exception as error:
            sys.exit(
                f"Could not download premake: {error}\n"
                f"Fetch {url} by hand and unpack {member} into "
                "vendor/bin/premake/"
            )

        # Verified before anything is unpacked, so a bad payload never reaches
        # the filesystem as an executable.
        actual_sha = hashlib.sha256(payload).hexdigest()
        if actual_sha != expected_sha:
            sys.exit(
                "premake archive checksum does not match.\n"
                f"  expected {expected_sha}\n"
                f"  got      {actual_sha}\n"
                "Refusing to unpack it."
            )

        with open(archive, "wb") as handle:
            handle.write(payload)

        os.makedirs(dest_dir, exist_ok=True)
        target = os.path.join(dest_dir, binary_name)

        # One named member, not extractall: the archive holds a single file,
        # and naming it means a crafted archive cannot write anywhere else.
        # The copy has to happen while the archive is still open -- a member
        # handle does not outlive it.
        if archive_name.endswith(".zip"):
            with zipfile.ZipFile(archive) as bundle:
                with bundle.open(member) as source, open(target, "wb") as handle:
                    shutil.copyfileobj(source, handle)
        else:
            with tarfile.open(archive) as bundle:
                # The linux and macos tarballs store it as ./premake5.
                names = bundle.getnames()
                inner = member if member in names else "./" + member
                source = bundle.extractfile(inner)

                if source is None:
                    sys.exit(f"{archive_name} did not contain {member}")

                with source, open(target, "wb") as handle:
                    shutil.copyfileobj(source, handle)

    # Archive members carry their own mode, which the copy above discards.
    os.chmod(target, 0o755)
    print(f"premake5 {PREMAKE_VERSION} installed at {os.path.relpath(target, ROOT)}")

    return target


def check_submodules():
    """Fail early and legibly if the vendor submodules were never checked out.

    Same first-five-minutes problem as the missing premake binary: a fresh
    clone or a new worktree has empty vendor directories, and the build gets a
    long way in before dying on a missing glfw header. Unlike premake, the fix
    is a git operation on the user's own repository, so this names the command
    rather than running it.
    """
    def empty(name):
        path = os.path.join(ROOT, "EGSS", "vendor", name)
        # Absent and present-but-empty are the same failure to the build.
        return not os.path.isdir(path) or not os.listdir(path)

    missing = [n for n in ("glfw", "glm", "imgui", "spdlog") if empty(n)]

    if missing:
        sys.exit(
            "These vendor submodules are empty: " + ", ".join(missing) + "\n"
            "Run:  git submodule update --init --recursive"
        )


def premake_path(allow_fetch=True):
    name = "premake5.exe" if IS_WINDOWS else "premake5"
    directory = os.path.join(ROOT, "vendor", "bin", "premake")
    path = os.path.join(directory, name)

    if os.path.isfile(path):
        return path

    if not allow_fetch:
        sys.exit(
            f"premake5 not found at vendor/bin/premake/{name}, and --no-fetch "
            "was given.\nDownload it from https://premake.github.io/download"
        )

    return fetch_premake(directory, name)


def generate(allow_fetch=True):
    """Regenerate project files. Cheap, so it is not conditional.

    Output is swallowed unless it fails. Because this now runs on every build,
    premake's banner -- including a deprecation notice about the gmake2 action
    on newer versions -- would otherwise be printed dozens of times a day. The
    action name is left as gmake2 deliberately: newer premake accepts it with a
    warning, older premake needs it, and the warning is not worth breaking
    anyone's setup over.
    """
    check_submodules()

    action = "vs2022" if IS_WINDOWS else "gmake2"
    command = [premake_path(allow_fetch), action]

    print("\033[90m$ " + " ".join(command) + "\033[0m")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)

    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)

    return result.returncode


def build(config, jobs, generate_first=True, allow_fetch=True):
    if generate_first and generate(allow_fetch) != 0:
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


def launch(config, forwarded=None):
    directory = binary_dir(config)
    executable = os.path.join(directory, "TestEnv.exe" if IS_WINDOWS else "TestEnv")

    if not os.path.isfile(executable):
        sys.exit(f"No binary at {executable} -- build it first.")

    forwarded = list(forwarded or [])
    print("\033[90m$ cd " + directory + " && ./TestEnv "
          + " ".join(forwarded) + "\033[0m")

    # From its own directory, so imgui.ini, profile.json, screenshots and
    # recordings all land beside it -- and so assets resolve, since they are
    # loaded by path relative to the executable.
    return subprocess.call([executable] + forwarded, cwd=directory)


def main():
    parser = argparse.ArgumentParser(description="Build and run EGSS.")
    parser.add_argument("command", nargs="?", default="run",
                        choices=["build", "run", "clean", "gen"])
    parser.add_argument("config", nargs="?", default="debug",
                        choices=CONFIGS + ["all"])
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--no-gen", action="store_true",
                        help="skip regenerating project files")
    parser.add_argument("--no-fetch", action="store_true",
                        help="fail instead of downloading premake5 if it is missing")
    # Everything after a bare -- goes to TestEnv rather than to this script,
    # which is the only way to reach its flags: the binary lives beside its
    # assets under bin/, so running it by hand means knowing the config-
    # dependent path. `./egss.py run -- --demo Breakout --record run.rec`.
    #
    # Split by hand rather than with argparse.REMAINDER, which eats the `--`
    # itself and then offers the first forwarded flag to the `config`
    # positional -- so `run -- --demo Breakout` failed with "invalid choice:
    # '--demo'" instead of forwarding anything.
    argv = sys.argv[1:]
    forwarded = []
    if "--" in argv:
        separator = argv.index("--")
        forwarded = argv[separator + 1:]
        argv = argv[:separator]

    args = parser.parse_args(argv)

    if args.command == "gen":
        return generate(allow_fetch=not args.no_fetch)

    configs = CONFIGS if args.config == "all" else [args.config]

    if args.command == "clean":
        for config in configs:
            run(["make", f"config={config}", "clean"])
        return 0

    # Generate once even when building several configs.
    if not args.no_gen and generate(allow_fetch=not args.no_fetch) != 0:
        return 1

    for config in configs:
        if build(config, args.jobs, generate_first=False) != 0:
            return 1

    if args.command == "run":
        if args.config == "all":
            sys.exit("Pick one config to run.")
        return launch(args.config, forwarded)

    return 0


if __name__ == "__main__":
    sys.exit(main())
