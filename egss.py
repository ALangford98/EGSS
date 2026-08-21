#!/usr/bin/env python3
"""One entry point for building and running EGSS.

    ./egss.py                 build debug and run it
    ./egss.py build           build debug
    ./egss.py build release   build release
    ./egss.py build all       build all three configs
    ./egss.py run [config]    build, then run from the binary's own directory
    ./egss.py clean [config]
    ./egss.py gen             regenerate project files only
    ./egss.py sanitize        build instrumented and run every demo under it
    ./egss.py windows         bridge KWin's window list to the wallpaper

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

Sanitizers
----------
`--sanitize` generates and builds with ASan and UBSan into `bin/<Config>-...-
sanitize/`, and works with any config. `./egss.py sanitize` does that and then
runs every demo under it in lockstep, which is the whole point: an instrumented
binary nobody runs catches nothing.

It exists because of 2026-08-17. A signed overflow in a hash let the compiler
delete a loop's exit test, `SpawnRocks` ran 99,482 times, the process reached
17 GB and was OOM killed -- in release only, and every warning flag the project
had was silent. UBSan named the file and line on its first run, at the end of an
afternoon that had produced no other answer. It was reached by hand that day and
there was no way to reach it again.
"""

import argparse
import hashlib
import os
import re
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
SANITIZE_STEPS = 300

# Leak detection left **on**, which was worth checking rather than assuming: the
# usual reason to disable it is a driver or a UI toolkit that leaks by design,
# and 300 steps of every demo reports nothing at all here. Turning it off "to
# reduce noise" would have thrown away a real check for noise that does not
# exist.
SANITIZE_ASAN_OPTIONS = "detect_leaks=1"

# TSan reports and keeps going, so one sweep names every racing pair it saw
# rather than the first. history_size buys deeper stacks for the *other* thread
# in a race, which is the half that is usually hard to identify.
SANITIZE_TSAN_OPTIONS = "halt_on_error=0:history_size=7"

SANITIZE_MODES = ["address", "thread"]

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


def generate(allow_fetch=True, sanitize=False):
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

    # Passed to premake, not to make: the flags live in the generated project
    # files, and so does the separate output directory that keeps instrumented
    # objects away from plain ones.
    if sanitize:
        command.append("--sanitize=" + sanitize)

    print("\033[90m$ " + " ".join(command) + "\033[0m")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)

    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)

    return result.returncode


def generated_with_sanitize():
    """Whether the project files sitting on disk were generated with --sanitize.

    Read out of the generated files rather than remembered in a state file of
    its own, which cannot go stale against them.

    This exists because `--no-gen` and `--sanitize` disagreeing is silent and
    genuinely confusing: the flags and the output directory both live in the
    generated project files, so `build --sanitize --no-gen` after a plain
    generation rebuilds the *plain* tree and reports success, and the
    instrumented binary you then run is whatever was there before.
    """
    name = "EGSS.vcxproj" if IS_WINDOWS else "Makefile"
    path = os.path.join(ROOT, "EGSS", name)

    try:
        with open(path) as handle:
            text = handle.read()
    except OSError:
        return None   # nothing generated yet; the caller will generate

    if "-fsanitize=thread" in text:
        return "thread"
    if "-fsanitize=address" in text:
        return "address"

    return False      # generated, and plain


def build(config, jobs, generate_first=True, allow_fetch=True, sanitize=None):
    if sanitize:
        missing = sanitizer_runtime_missing(sanitize)
        if missing:
            print(missing, file=sys.stderr)
            return 1

    if generate_first and generate(allow_fetch, sanitize) != 0:
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


def binary_dir(config, sanitize=None):
    system = "windows" if IS_WINDOWS else platform.system().lower()
    suffix = ("-sanitize-" + sanitize) if sanitize else ""
    return os.path.join(ROOT, "bin",
                        f"{config.capitalize()}-{system}-x86_64{suffix}", "TestEnv")


def launch(config, forwarded=None, sanitize=None):
    directory = binary_dir(config, sanitize)
    executable = os.path.join(directory, "TestEnv.exe" if IS_WINDOWS else "TestEnv")

    if not os.path.isfile(executable):
        sys.exit(f"No binary at {executable} -- build it first.")

    forwarded = list(forwarded or [])
    print("\033[90m$ cd " + directory + " && ./TestEnv "
          + " ".join(forwarded) + "\033[0m")

    # From its own directory, so imgui.ini, profile.json, screenshots and
    # recordings all land beside it -- and so assets resolve, since they are
    # loaded by path relative to the executable.
    return subprocess.call([executable] + forwarded, cwd=directory,
                           env=sanitizer_env(sanitize) if sanitize else None)


def sanitizer_runtime_missing(mode):
    """The sanitizer's runtime library, checked by trying to link against it.

    Returns None when it works, or a message naming what to install.

    **Probed rather than looked for on disk.** `gcc -print-file-name=libtsan.so`
    happily names a path that exists and is a 38-byte linker script pointing at
    a `libtsan.so.2.0.0` that does not -- which is exactly the state this machine
    was in, and it fails at the link of libEGSS.so with
    `ld.bfd: cannot find /usr/lib64/libtsan.so.2.0.0`, several minutes into a
    build, saying nothing about a package. Compiling an empty program takes a
    fifth of a second and cannot be fooled.
    """
    if IS_WINDOWS:
        return None   # MSVC has its own story here; untested, like the rest.

    flag = "-fsanitize=thread" if mode == "thread" else "-fsanitize=address"

    with tempfile.TemporaryDirectory() as tmp:
        probe = subprocess.run(
            ["cc", flag, "-x", "c", "-", "-o", os.path.join(tmp, "probe")],
            input="int main(void) { return 0; }",
            capture_output=True, text=True)

    if probe.returncode == 0:
        return None

    package = {
        "thread": "libtsan (Fedora), libtsan2 (Debian/Ubuntu)",
        "address": "libasan (Fedora), libasan8 (Debian/Ubuntu)",
    }[mode]

    return (f"The {flag} runtime is not installed, so this build would fail at "
            f"the link.\n  Install: {package}\n"
            f"  The compiler said: {probe.stderr.strip().splitlines()[-1] if probe.stderr.strip() else 'nothing'}")


def sanitizer_env(mode):
    """Environment for an instrumented run.

    `print_stacktrace` is what turns "there is undefined behaviour somewhere"
    into a file and a line, which is the entire reason this exists.
    """
    env = dict(os.environ)

    if mode == "thread":
        env.setdefault("TSAN_OPTIONS", SANITIZE_TSAN_OPTIONS)
        return env

    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")
    env.setdefault("ASAN_OPTIONS", SANITIZE_ASAN_OPTIONS)
    return env


# Libraries whose internals are not this project's problem. Used to *classify*
# TSan reports, never to suppress them: a suppression matching one of these
# would also swallow a real race of ours that merely passes through ALSA on its
# way to the mixer, which is exactly where our races live.
THIRD_PARTY_LIBS = ("libasound", "libxcb", "libX11", "libGL", "libEGL", "libpulse",
                    "iris_dri", "swrast", "libtsan", "ld-linux", "libc.so",
                    "radeonsi", "zink", "libwayland")


def classify_tsan(output):
    """Split TSan reports into (ours, third-party).

    A report counts as ours when the **innermost** frame of a racing access is
    our own source. Not "our source appears near the top", which was the first
    attempt and called every demo a failure: the libxcb race during
    `glfwCreateWindow` puts `recvmsg` and `_xcb_in_read` at frames #0 and #1 and
    our `LinuxWindow` constructor at #2, where it is the *caller* of the racing
    read and not the thing that raced.

    TSan's own interceptors (`libtsan`) sit at #0 for anything it wraps -- a
    memcpy, a recvmsg -- so they are skipped to find the frame that actually
    performed the access.
    """
    blocks = re.findall(r"(WARNING: ThreadSanitizer:.*?SUMMARY: ThreadSanitizer:.*?\n)",
                        output, re.S)

    ours, theirs = [], []

    for block in blocks:
        accesses = re.findall(
            r"(?:Write|Read|Atomic write|Atomic read|Previous write|Previous read"
            r"|Previous atomic write|Previous atomic read).*?:\n((?:\s+#\d+ .*\n){1,4})",
            block)

        def innermost(access):
            for frame in access.strip().splitlines():
                if "libtsan" in frame:
                    continue          # an interceptor, not the access itself
                return frame
            return ""

        mine = any(re.search(r"src/(Egss|Platform)/|src/\w+\.(h|cpp)", frame)
                   and not any(lib in frame for lib in THIRD_PARTY_LIBS)
                   for frame in (innermost(access) for access in accesses))

        (ours if mine else theirs).append(block)

    return ours, theirs


def demo_shortnames():
    """The demos, read out of DemoRegistry.h rather than repeated here.

    Adding a demo is meant to be one line in that file, and a sweep that has to
    be edited as well is a sweep that quietly stops covering the newest thing --
    which is the one most likely to need it.
    """
    path = os.path.join(ROOT, "TestEnv", "src", "DemoRegistry.h")
    names = []

    with open(path) as handle:
        for line in handle:
            match = re.search(r'^\s*\{\s*"[^"]*",\s*"([^"]+)"', line)
            if match:
                names.append(match.group(1))

    return names


def sanitize_sweep(config, jobs, steps, mode="address", allow_fetch=True,
                   generate_first=True):
    """Build instrumented, then run every demo under it and report.

    Lockstep and hidden, so the sweep is the same work every time and does not
    take the keyboard off whatever the machine is being used for. Each demo is
    its own process: a sanitizer report is most useful with the demo that
    produced it named beside it, and one demo aborting must not cost the rest.
    """
    if build(config, jobs, generate_first=generate_first,
             allow_fetch=allow_fetch, sanitize=mode) != 0:
        return 1

    directory = binary_dir(config, sanitize=mode)
    executable = os.path.join(directory, "TestEnv.exe" if IS_WINDOWS else "TestEnv")

    if not os.path.isfile(executable):
        sys.exit(f"No binary at {executable}")

    demos = demo_shortnames()
    if not demos:
        sys.exit("No demos found in TestEnv/src/DemoRegistry.h")

    label = "TSan" if mode == "thread" else "ASan + UBSan"
    print(f"\nSweeping {len(demos)} demos, {steps} steps each, "
          f"{config} + {label}\n")

    findings = 0

    for name in demos:
        # The race detector gets the audio stress as well: the demos on their
        # own play a handful of sounds, and the voice-pool races found on
        # 2026-08-21 only appear once slots are being recycled under pressure.
        # Nothing to add for ASan -- it sees a use-after-free whether the
        # allocation churns or not.
        extra = ["--audio-stress"] if mode == "thread" else []

        result = subprocess.run(
            [executable, "--demo", name, "--lockstep", "--hide-ui",
             "--hide-window", "--exit-after", str(steps)] + extra,
            cwd=directory, env=sanitizer_env(mode),
            capture_output=True, text=True)

        output = result.stdout + result.stderr

        # UBSan prints "runtime error:" and carries on; ASan prints a banner and
        # aborts; TSan prints "WARNING: ThreadSanitizer: <what>" per racing
        # pair. All matched rather than relying on the exit code, because a
        # recoverable report leaves it at zero.
        if mode == "thread":
            # Classified rather than counted. A TSan run against a desktop
            # driver stack is never empty, and a sweep that reports "5 findings"
            # every time is a sweep nobody reads.
            mine, other = classify_tsan(output)

            reports = [re.search(r"SUMMARY: ThreadSanitizer: (.*)", block).group(1)
                       for block in mine
                       if re.search(r"SUMMARY: ThreadSanitizer: (.*)", block)]

            noise = len(other)
        else:
            reports = [line for line in output.splitlines()
                       if "runtime error:" in line or "ERROR: AddressSanitizer" in line
                       or "ERROR: LeakSanitizer" in line]
            noise = 0

        unique = list(dict.fromkeys(reports))

        # TSan exits 66 when it reported anything at all, including reports this
        # sweep has classified as somebody else's. That is not a crash.
        expected_exit = (0, 66) if mode == "thread" else (0,)

        if unique or unexpected_exit:
            findings += len(unique)
            status = "\033[31mFAIL\033[0m"
        else:
            status = "\033[32m ok \033[0m"

        # The exit code is only worth printing when it is not the one this mode
        # expects -- TSan exits 66 whenever it reported anything at all, our
        # findings or somebody else's, so printing it every line trains the eye
        # to ignore the one time it means something.
        unexpected_exit = result.returncode not in expected_exit

        print(f"  [{status}] {name}"
              + (f"  ({noise} third-party)" if noise else "")
              + (f"  (exit {result.returncode})" if unexpected_exit else ""))

        for line in unique[:12]:
            print(f"           {line.strip()}")

        if len(unique) > 12:
            print(f"           ... and {len(unique) - 12} more")

    print(f"\n{len(demos)} demos, {findings} sanitizer reports\n")
    return 1 if findings else 0


def main():
    parser = argparse.ArgumentParser(description="Build and run EGSS.")
    parser.add_argument("command", nargs="?", default="run",
                        choices=["build", "run", "clean", "gen", "sanitize", "windows"])
    parser.add_argument("config", nargs="?", default="debug",
                        choices=CONFIGS + ["all"])
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--no-gen", action="store_true",
                        help="skip regenerating project files")
    parser.add_argument("--no-fetch", action="store_true",
                        help="fail instead of downloading premake5 if it is missing")
    parser.add_argument("--sanitize", action="store_true",
                        help="build with ASan and UBSan, into "
                             "bin/<Config>-...-sanitize-address")
    parser.add_argument("--thread", action="store_true",
                        help="build with TSan instead -- races rather than "
                             "memory errors. The two cannot be combined")
    parser.add_argument("--steps", type=int, default=SANITIZE_STEPS,
                        help=f"fixed steps per demo in a sweep (default {SANITIZE_STEPS})")
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

    # One mode, however it was asked for. --thread wins because it is the more
    # specific request, and `sanitize` implies instrumenting even if neither
    # flag was given.
    mode = "thread" if args.thread else ("address" if args.sanitize else None)
    if args.command == "sanitize" and mode is None:
        mode = "address"

    if args.command == "windows":
        # Runs until interrupted. Its own script rather than something built in
        # here, because it needs a D-Bus main loop and this does not.
        bridge = os.path.join(ROOT, "tools", "egss-windows.py")
        return subprocess.call([sys.executable, bridge])

    if args.command == "gen":
        return generate(allow_fetch=not args.no_fetch, sanitize=mode)

    if args.command == "sanitize":
        if args.config == "all":
            sys.exit("Pick one config to sweep.")

        # Same guard as below, and it matters more here: a sweep that ran a
        # binary built the other way would report no findings, which reads as a
        # pass. TSan finding nothing in an ASan build is the same false all-clear
        # as either finding nothing in a plain one.
        skip_gen = args.no_gen and generated_with_sanitize() == mode

        if args.no_gen and not skip_gen:
            print("--no-gen ignored: the project files were generated for "
                  + str(generated_with_sanitize()) + ", not " + mode)

        return sanitize_sweep(args.config, args.jobs, args.steps, mode=mode,
                              allow_fetch=not args.no_fetch,
                              generate_first=not skip_gen)

    configs = CONFIGS if args.config == "all" else [args.config]

    if args.command == "clean":
        for config in configs:
            run(["make", f"config={config}", "clean"])
        return 0

    # Generate once even when building several configs.
    #
    # --no-gen is overridden when the project files on disk were generated the
    # other way round, because skipping the regeneration there does not skip
    # work, it builds the wrong tree.
    on_disk = generated_with_sanitize()
    wanted = mode if mode else False
    skip = args.no_gen and (on_disk is None or on_disk == wanted)

    if args.no_gen and not skip:
        print("--no-gen ignored: the project files were generated for "
              + str(on_disk) + ", not " + str(wanted))

    if not skip and generate(allow_fetch=not args.no_fetch, sanitize=mode) != 0:
        return 1

    for config in configs:
        if build(config, args.jobs, generate_first=False, sanitize=mode) != 0:
            return 1

    if args.command == "run":
        if args.config == "all":
            sys.exit("Pick one config to run.")
        return launch(args.config, forwarded, sanitize=mode)

    return 0


if __name__ == "__main__":
    sys.exit(main())
