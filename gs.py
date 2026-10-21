#!/usr/bin/env python3
"""One entry point for building and running GS.

    ./gs.py                 build debug and run it
    ./gs.py build           build debug
    ./gs.py build release   build release
    ./gs.py build all       build all three configs
    ./gs.py run [config]    build, then run from the binary's own directory
    ./gs.py clean [config]
    ./gs.py prune           report what the build trees cost, and offer to
                              delete the ones nothing is using
    ./gs.py gen             regenerate project files only
    ./gs.py sanitize        build instrumented and run every demo under it
    ./gs.py windows         bridge KWin's window list to the wallpaper

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
sanitize/`, and works with any config. `./gs.py sanitize` does that and then
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
import stat
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


# Tools the game exporter downloads on first use, each pinned to a version
# and to the SHA-256 its publisher lists for it (GitHub's release-asset
# digests), checked before anything is written -- the same rule premake
# follows above. Installed under vendor/bin/<tool>/, which is gitignored.
TOOLS = {
    # AppImage: one double-clickable file for a Linux export (piece 2).
    "appimagetool": [
        ("https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
         "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
         "appimagetool"),
        ("https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
         "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
         "runtime-x86_64"),
    ],
    # Windows export (piece 4): a self-contained Clang + MinGW-w64 + UCRT
    # cross toolchain. Unpacked, one top-level folder stripped.
    "llvm-mingw": [
        ("https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/"
         "llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz",
         "bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21",
         "unpack"),
    ],
    # Web export (piece 5): the Emscripten SDK, cloned at a pinned tag whose
    # commit is checked (git's equivalent of the checksum), then told to
    # install and activate that same release -- emsdk verifies the toolchain
    # it downloads against its own manifest.
    "emsdk": [
        ("https://github.com/emscripten-core/emsdk.git",
         "dd8e25632640cfc1fb570c7fa4cc374e8a5e5a72",
         "git:6.0.11"),
    ],
    # Android export (piece 6): the NDK (the native compiler), the SDK pieces
    # an APK is assembled from without Gradle (build-tools: aapt2, zipalign,
    # apksigner; the platform's android.jar), and a JDK for apksigner and
    # keytool. Google's repository manifest publishes SHA-1 for its packages
    # (repository2-3.xml) -- weaker than SHA-256 but the publisher's own, so
    # those are checked against it rather than a hash taken on first use.
    "android": [
        ("https://dl.google.com/android/repository/android-ndk-r29-linux.zip",
         "sha1:87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b", "unpack:ndk"),
        ("https://dl.google.com/android/repository/build-tools_r37_linux.zip",
         "sha1:70954e99f4c3d9d46ee70fa32624672fe7cd6ebe", "unpack:build-tools"),
        ("https://dl.google.com/android/repository/platform-36_r02.zip",
         "sha1:2c1a80dd4d9f7d0e6dd336ec603d9b5c55a6f576", "unpack:platform"),
        ("https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.20.1%2B1/"
         "OpenJDK17U-jdk_x64_linux_hotspot_17.0.20.1_1.tar.gz",
         "3808d1d15e3ec6bd5b84057fb5d84c33d8a1536a258146bcea2e603fc726e08e", "unpack:jdk"),
    ],
    # Verifying a Windows export on this Linux machine: a portable Wine,
    # used to *run* the exported game, never shipped with it.
    "wine": [
        ("https://github.com/Kron4ek/Wine-Builds/releases/download/11.18/wine-11.18-amd64.tar.xz",
         "5717663b0541afe99efee507e1eac88d452b8adff0d3a5065abeed527890a3e8",
         "unpack"),
    ],
}


def target_folder(config, target):
    """The bin/ folder premake writes for a target: its system, architecture
    and suffix -- e.g. Dist-linux-ARM64-android (premake5.lua's outputdir).
    GameExporter spells the same names; keep the two in step."""
    return config.capitalize() + {
        "windows": "-windows-x86_64",
        "web": "-linux-x86_64-web",
        "android": "-linux-ARM64-android",
    }.get(target, "-linux-x86_64")


ANDROID_MIN_SDK = 26
ANDROID_TARGET_SDK = 36

ANDROID_MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{app_id}" android:versionCode="1" android:versionName="1.0">
  <uses-sdk android:minSdkVersion="{min_sdk}" android:targetSdkVersion="{target_sdk}"/>
  <uses-feature android:glEsVersion="0x00030000" android:required="true"/>
  <application android:label="{label}" android:icon="@mipmap/icon"
      android:hasCode="false" android:extractNativeLibs="false"
      android:appCategory="game" android:isGame="true">
    <activity android:name="android.app.NativeActivity" android:exported="true"
        android:launchMode="singleTask" android:screenOrientation="sensorLandscape"
        android:theme="@android:style/Theme.NoTitleBar.Fullscreen"
        android:configChanges="orientation|screenSize|screenLayout|smallestScreenSize|keyboard|keyboardHidden|navigation|uiMode|density">
      <meta-data android:name="android.app.lib_name" android:value="gsplayer"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
"""


def android_app_id(name):
    """com.gsgame.<name>: letters and digits of the game's name, lower case,
    and a letter first -- what a Java package segment must be."""
    segment = re.sub(r"[^a-z0-9]", "", name.lower()) or "game"
    if not segment[0].isalpha():
        segment = "g" + segment
    return "com.gsgame." + segment


def package_apk(lib, game, name, icon, out):
    """Assemble, align and sign an APK with no Gradle and no Java code: a
    NativeActivity manifest and the icon linked by aapt2, then the player's
    library and the game added as plain zip entries.

    The library is stored, not deflated, and zipalign -P 16 puts it on a
    16 KB boundary: with extractNativeLibs="false" Android maps it straight
    out of the APK, which needs both (and 16 KB pages are what Android 15+
    devices with large pages require). The game goes under assets/ beside
    game.files, the list the platform layer unpacks from, because an APK's
    asset folders cannot be walked for subfolders at run time."""
    if fetch_tool("android") != 0:
        return 1
    sdk = tool_dir("android")
    tools = os.path.join(sdk, "build-tools")
    llvm = os.path.join(sdk, "ndk", "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")
    jdk_bin = os.path.join(sdk, "jdk", "bin")
    environment = dict(os.environ, JAVA_HOME=os.path.join(sdk, "jdk"),
                       PATH=jdk_bin + os.pathsep + os.environ.get("PATH", ""))
    app_id = android_app_id(name)
    print(f"APK {app_id}: {os.path.basename(out)}")

    with tempfile.TemporaryDirectory() as tmp:
        def tool(*command):
            result = subprocess.run(list(command), env=environment, capture_output=True, text=True)
            if result.returncode != 0:
                sys.exit(f"{os.path.basename(command[0])} failed:\n{result.stdout}{result.stderr}")

        label = (name.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                     .replace('"', "&quot;"))
        manifest = os.path.join(tmp, "AndroidManifest.xml")
        with open(manifest, "w") as handle:
            handle.write(ANDROID_MANIFEST.format(app_id=app_id, label=label,
                         min_sdk=ANDROID_MIN_SDK, target_sdk=ANDROID_TARGET_SDK))
        os.makedirs(os.path.join(tmp, "res", "mipmap"))
        shutil.copyfile(icon, os.path.join(tmp, "res", "mipmap", "icon.png"))
        tool(os.path.join(tools, "aapt2"), "compile", "--dir", os.path.join(tmp, "res"),
             "-o", os.path.join(tmp, "res.zip"))
        base = os.path.join(tmp, "base.apk")
        tool(os.path.join(tools, "aapt2"), "link", "-o", base, "--manifest", manifest,
             "-I", os.path.join(sdk, "platform", "android.jar"), os.path.join(tmp, "res.zip"))

        # Symbols are for the debugger, not the phone: stripped, the Dist
        # library is a fraction of its size.
        stripped = os.path.join(tmp, "libgsplayer.so")
        tool(os.path.join(llvm, "llvm-strip"), "--strip-unneeded", lib, "-o", stripped)

        files = []
        for directory, _, names in os.walk(game):
            for filename in sorted(names):
                path = os.path.join(directory, filename)
                files.append(os.path.relpath(path, os.path.dirname(game)).replace(os.sep, "/"))
        files.sort()
        # The build's identity, first line of game.files: a hash of every
        # file shipped, so the device unpacks again exactly when something
        # changed (Platform/Android compares it with the copy it unpacked).
        digest = hashlib.sha256()
        for relative in files:
            digest.update(relative.encode() + b"\0")
            with open(os.path.join(os.path.dirname(game), relative), "rb") as handle:
                digest.update(handle.read())
        listing = f"gs-files 1 {digest.hexdigest()[:16]}\n" + "".join(f + "\n" for f in files)

        with zipfile.ZipFile(base, "a") as apk:
            apk.write(stripped, "lib/arm64-v8a/libgsplayer.so", compress_type=zipfile.ZIP_STORED)
            apk.writestr("assets/game.files", listing, compress_type=zipfile.ZIP_DEFLATED)
            for relative in files:
                apk.write(os.path.join(os.path.dirname(game), relative), "assets/" + relative,
                          compress_type=zipfile.ZIP_DEFLATED)

        aligned = os.path.join(tmp, "aligned.apk")
        tool(os.path.join(tools, "zipalign"), "-P", "16", "-f", "4", base, aligned)

        # A debug key, made once per machine and kept beside the SDK, so
        # every export from here is signed by the same key and installs
        # over the last one. A store release needs the developer's own key.
        keystore = os.path.join(sdk, "debug.keystore")
        if not os.path.isfile(keystore):
            tool(os.path.join(jdk_bin, "keytool"), "-genkeypair", "-keystore", keystore,
                 "-storepass", "android", "-alias", "androiddebugkey", "-keypass", "android",
                 "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
                 "-dname", "CN=Android Debug,O=Android,C=US")
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        # No v4 sidecar (.idsig): it only serves adb incremental installs.
        tool(os.path.join(tools, "apksigner"), "sign", "--ks", keystore, "--v4-signing-enabled", "false",
             "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", out, aligned)
        tool(os.path.join(tools, "apksigner"), "verify", out)
    print(f"APK signed: {out}")
    return 0


def tool_dir(name):
    return os.path.join(ROOT, "vendor", "bin", name)


def fetch_tool(name):
    """Download, verify and install one pinned tool. Already-present files
    are left alone, so this is cheap to call before every use."""
    if name not in TOOLS:
        sys.exit(f"Unknown tool '{name}'. Known: {', '.join(sorted(TOOLS))}")
    directory = tool_dir(name)
    os.makedirs(directory, exist_ok=True)
    for url, expected_sha, filename in TOOLS[name]:
        if filename.startswith("git:"):
            version = filename[4:]
            marker = os.path.join(directory, ".installed-" + version)
            if os.path.isfile(marker):
                continue
            if not os.path.isdir(os.path.join(directory, ".git")):
                shutil.rmtree(directory, ignore_errors=True)
                if subprocess.call(["git", "clone", "--quiet", "--depth", "1", "--branch", version,
                                    url, directory]) != 0:
                    sys.exit(f"Could not clone {url} at {version}")
            head = subprocess.check_output(["git", "-C", directory, "rev-parse", "HEAD"], text=True).strip()
            if head != expected_sha:
                sys.exit(f"{name} {version}: commit {head} is not the pinned {expected_sha}. Refusing to use it.")
            tool = os.path.join(directory, "emsdk")
            if subprocess.call([tool, "install", version]) != 0 or \
               subprocess.call([tool, "activate", version]) != 0:
                sys.exit(f"{name}: install/activate {version} failed")
            open(marker, "w").close()
            continue
        target = os.path.join(directory, filename)
        unpack = filename == "unpack" or filename.startswith("unpack:")
        # "unpack:<sub>" unpacks into a subfolder, for tools made of several archives.
        unpack_into = os.path.join(directory, filename[7:]) if filename.startswith("unpack:") else directory
        marker = os.path.join(directory, ".unpacked-" + expected_sha.split(":")[-1][:16])
        if (unpack and os.path.isfile(marker)) or (not unpack and os.path.isfile(target)):
            continue
        print(f"fetching {url}")
        try:
            with urllib.request.urlopen(url, timeout=600) as response:
                payload = response.read()
        except Exception as error:
            sys.exit(f"Could not download {url}: {error}")
        algorithm, _, expected_digest = expected_sha.rpartition(":")
        actual_sha = hashlib.new(algorithm or "sha256", payload).hexdigest()
        if actual_sha != expected_digest:
            sys.exit(f"{filename}: checksum does not match.\n  expected {expected_sha}\n"
                     f"  got      {actual_sha}\nRefusing to install it.")
        if unpack:
            # Verified above, then unpacked with the "data" filter, which
            # refuses absolute paths, ".." escapes and device files -- a
            # crafted archive cannot write outside the tool's folder. The
            # single top-level folder these archives have is stripped.
            with tempfile.TemporaryDirectory() as tmp:
                archive = os.path.join(tmp, "archive")
                with open(archive, "wb") as handle:
                    handle.write(payload)
                if url.endswith(".zip"):
                    # zipfile has no "data" filter: each member's path is
                    # checked by hand, and the Unix mode bits it would drop
                    # are restored -- otherwise the NDK's compilers unpack
                    # without their execute bit. So are symlinks, which
                    # zipfile writes out as small files holding the target's
                    # name: the NDK's `clang` is one (-> clang-21), and ran
                    # as a shell script saying "clang-21: command not found".
                    with zipfile.ZipFile(archive) as bundle:
                        base = os.path.realpath(tmp + "/x")
                        for info in bundle.infolist():
                            dest = os.path.realpath(os.path.join(base, info.filename))
                            if not dest.startswith(base + os.sep) and dest != base:
                                sys.exit(f"{name}: archive member escapes its folder: {info.filename}")
                            if stat.S_ISLNK(info.external_attr >> 16):
                                link = bundle.read(info).decode()
                                pointed = os.path.realpath(os.path.join(os.path.dirname(dest), link))
                                if os.path.isabs(link) or not pointed.startswith(base + os.sep):
                                    sys.exit(f"{name}: archive link escapes its folder: {info.filename} -> {link}")
                                os.makedirs(os.path.dirname(dest), exist_ok=True)
                                os.symlink(link, dest)
                                continue
                            bundle.extract(info, base)
                            mode = (info.external_attr >> 16) & 0o777
                            if mode and not info.is_dir():
                                os.chmod(dest, mode)
                else:
                    with tarfile.open(archive) as bundle:
                        bundle.extractall(tmp + "/x", filter="data")
                top = os.listdir(tmp + "/x")
                root = os.path.join(tmp, "x", top[0]) if len(top) == 1 else tmp + "/x"
                os.makedirs(unpack_into, exist_ok=True)
                for entry in os.listdir(root):
                    shutil.move(os.path.join(root, entry), os.path.join(unpack_into, entry))
            open(marker, "w").close()
            continue
        with open(target, "wb") as handle:
            handle.write(payload)
        os.chmod(target, 0o755)
    print(f"{name} ready in {os.path.relpath(directory, ROOT)}")
    return 0


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
        path = os.path.join(ROOT, "GS", "vendor", name)
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


def generate(allow_fetch=True, sanitize=False, player_scripts=None, target_os=None,
             web=False, web_game=None, android_ndk=None):
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
    if player_scripts:
        command.append("--player-scripts=" + player_scripts)
    if target_os:
        command.append("--os=" + target_os)
    if web:
        command.append("--web")
    if web_game:
        command.append("--web-game=" + web_game)
    if android_ndk:
        command += ["--android", "--android-ndk=" + android_ndk]

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
    name = "GS.vcxproj" if IS_WINDOWS else "Makefile"
    path = os.path.join(ROOT, "GS", name)

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
        # open GS.sln in Visual Studio and build normally.
        msbuild = shutil.which("msbuild")
        if not msbuild:
            print("msbuild not on PATH -- open GS.sln in Visual Studio instead.")
            return 1

        return run([msbuild, "GS.sln", f"/p:Configuration={config.capitalize()}",
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
    was in, and it fails at the link of libGS.so with
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

        mine = any(re.search(r"src/(GS|Platform)/|src/\w+\.(h|cpp)", frame)
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

    # **The generated project files are global state, and a sweep takes
    # minutes.**
    #
    # `generate` rewrites the makefiles for whichever mode it was asked for, so
    # a plain `./gs.py build` started while a sweep is running points the
    # *same* makefiles at the plain output directory. The sweep then builds --
    # successfully -- into a tree it is not about to look in, and dies much
    # later with a `FileNotFoundError` naming a path that was there when it
    # checked.
    #
    # That is exactly what happened on 2026-08-31, and the traceback was worse
    # than useless: Python reads source lazily when it *formats* a traceback,
    # so having edited this file in the meantime, every quoted line was from
    # somewhere else entirely.
    #
    # Asking again here cannot make the race impossible -- nothing short of a
    # lock can -- but it turns a puzzle into a sentence.
    if generated_with_sanitize() != mode:
        sys.exit("The project files were regenerated for "
                 + str(generated_with_sanitize()) + " while this sweep was "
                 "building for " + mode + ".\nSomething else ran gs.py at "
                 "the same time; the makefiles are shared. Run the sweep on "
                 "its own.")

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

        # The exit code is only worth reporting when it is not the one this
        # mode expects -- TSan exits 66 whenever it reported anything at all,
        # ours or somebody else's, so printing it every line trains the eye to
        # ignore the one time it means something.
        #
        # **This has to be computed before it is read.** It was assigned at the
        # bottom of the loop and read at the top, so the sweep raised
        # UnboundLocalError on its first demo and never ran a second -- and the
        # traceback goes to stdout with an exit code of zero, so it looked from
        # outside like a sweep that had passed quietly.
        unexpected_exit = result.returncode not in expected_exit

        if unique or unexpected_exit:
            findings += len(unique)
            status = "\033[31mFAIL\033[0m"
        else:
            status = "\033[32m ok \033[0m"

        print(f"  [{status}] {name}"
              + (f"  ({noise} third-party)" if noise else "")
              + (f"  (exit {result.returncode})" if unexpected_exit else ""))

        for line in unique[:12]:
            print(f"           {line.strip()}")

        if len(unique) > 12:
            print(f"           ... and {len(unique) - 12} more")

    print(f"\n{len(demos)} demos, {findings} sanitizer reports\n")
    return 1 if findings else 0


# Every build tree premake can produce, whether or not it exists today. The
# sanitizer suffixes come from `binary_dir`, so this list and that function have
# to agree -- which is why it is built from `CONFIGS` rather than written out.
def build_trees():
    trees = []

    for config in CONFIGS:
        name = config.capitalize() + "-linux-x86_64"

        trees.append((name, False))

        for mode in ("address", "thread"):
            trees.append((name + "-sanitize-" + mode, True))

        # The suffix used before the mode was part of the name. Still on disk
        # for anyone who built one back then, and nothing regenerates it.
        trees.append((name + "-sanitize", True))

    return trees


def tree_size(path):
    """Bytes under a directory, or 0 if it is not there.

    `du` would be one line and one process, but this script runs on Windows
    too and the whole point of it is that there is one entry point.
    """
    total = 0

    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass

    return total


def megabytes(value):
    return f"{value / (1024.0 * 1024.0):,.0f} MB"


def prune(keep, delete):
    """**What the build trees cost, and which of them nothing is using.**

    Build output is gitignored and entirely regenerable, and it is also by far
    the largest thing in the checkout -- 7.1 GB against 1.2 MB of engine
    source, when this was written. Most of it is sanitizer trees: an
    instrumented `libGS.so` carries so much debug info that one config's
    shared library alone is 293 MB, and there are four such configs.

    Reporting is the default and deleting is opt-in, because the cost of
    getting a tree back is a full rebuild of it and only the person at the
    keyboard knows whether they are about to want one. **Check that a sweep
    is not running first**: `./gs.py sanitize` runs out of one of these
    trees, and deleting it underneath itself is a confusing way to fail.
    """
    rows = []

    for name, instrumented in build_trees():
        size = tree_size(os.path.join(ROOT, "bin", name)) \
            + tree_size(os.path.join(ROOT, "bin-int", name))

        if size > 0:
            rows.append((name, size, instrumented))

    if not rows:
        print("No build trees on disk.")
        return 0

    rows.sort(key=lambda row: -row[1])

    total = sum(row[1] for row in rows)

    print(f"\n{len(rows)} build trees, {megabytes(total)} in bin and bin-int\n")

    going = 0

    for name, size, instrumented in rows:
        # A sanitizer tree is rebuilt by `./gs.py sanitize` whenever it is
        # next wanted, and nothing else reads it. Keeping one between sweeps
        # buys a faster sweep and costs about 700 MB.
        stale = instrumented and name not in keep

        going += size if stale else 0

        print(f"  {'delete' if stale and delete else 'keep  ' if not stale else 'stale '}"
              f"  {name:<44} {megabytes(size):>10}")

    print(f"\n{megabytes(going)} in trees nothing is using.")

    # **Worktrees, reported and not touched.** The abandoned handover
    # workflow left a full checkout *and* a second clone of every submodule
    # behind each one, which came to 2.5 GB -- more than the engine, the
    # history and the assets put together. They are not deleted here because
    # a worktree is git's to remove: `git worktree remove` unregisters it as
    # well as unlinking it, and an `rmtree` would leave git believing in a
    # directory that is not there.
    strays = []

    for parent in (os.path.join(ROOT, ".claude", "worktrees"),):
        if not os.path.isdir(parent):
            continue

        for name in sorted(os.listdir(parent)):
            path = os.path.join(parent, name)

            if os.path.isdir(path):
                strays.append((name, tree_size(path)
                    + tree_size(os.path.join(ROOT, ".git", "worktrees", name))))

    if strays:
        print("\nWorktrees, with their own build output and submodules:\n")

        for name, size in strays:
            print(f"  {name:<52} {megabytes(size):>10}")

        print("\n  git worktree remove --force .claude/worktrees/<name>")
        print("  git worktree prune\n")

    if not delete:
        print("Pass --delete to remove the stale build trees. They rebuild on"
              " the next sweep.\n")
        return 0

    for name, _, instrumented in rows:
        if not instrumented or name in keep:
            continue

        for parent in ("bin", "bin-int"):
            path = os.path.join(ROOT, parent, name)

            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)

    print("Removed.\n")
    return 0


# --- The trap index ---------------------------------------------------------
#
# `docs/HANDOVER.md` carries 258 traps and costs ~31k tokens to read whole,
# which is why sessions skipped it and hit the same walls again. The index is
# one line per trap -- ~4k tokens -- so the list can be *scanned* cheaply and
# the body reached with `grep -n` for the phrase.
#
# It is generated rather than written because a hand-kept index of a list that
# grows every session is a second thing to forget, and this repo has already
# been bitten once by a doc section drifting 22 commits behind what it claimed
# to describe. `--check` exits non-zero when it has drifted, so the failure is
# loud rather than silent.

HANDOVER = os.path.join(ROOT, "docs", "HANDOVER.md")

TRAPS_HEADING = "## Traps that have bitten more than once"
TRAPS_END = "## Conventions"
INDEX_BEGIN = "<!-- trap-index: ./gs.py traps regenerates this -->"
INDEX_END = "<!-- end trap-index -->"


def trap_bullets(body):
    """Every top-level bullet in the traps section, wrapped lines joined."""
    bullets, current = [], None

    for line in body:
        if line.startswith("- "):
            if current:
                bullets.append(current)
            current = line[2:].strip()
        elif current is not None and line.strip():
            # A trap runs until the next bullet or a blank line followed by
            # something that is not a continuation; nested bullets and code
            # fences belong to the trap above them.
            current += " " + line.strip()
        elif current is not None:
            bullets.append(current)
            current = None

    if current:
        bullets.append(current)

    return bullets


def trap_leadin(bullet):
    """The bold opening clause, which is the trap stated in one sentence."""
    fixed = bullet.startswith("~~")
    text = bullet[2:] if fixed else bullet

    match = re.match(r"\*\*(.+?)\*\*", text)

    if not match:
        return None, fixed

    return re.sub(r"\s+", " ", match.group(1)).strip().rstrip("."), fixed


def build_trap_index(lines):
    """The index block, and the slice of `lines` it belongs in."""
    try:
        start = next(i for i, l in enumerate(lines)
                     if l.startswith(TRAPS_HEADING))
        end = next(i for i, l in enumerate(lines) if l.startswith(TRAPS_END))
    except StopIteration:
        sys.exit("docs/HANDOVER.md no longer has the headings the index is "
                 "built from; fix TRAPS_HEADING/TRAPS_END in gs.py.")

    # Skip any existing index, or its own entries get indexed next time.
    body_from = start + 1
    if INDEX_BEGIN in lines:
        body_from = lines.index(INDEX_END) + 1

    entries = []
    for bullet in trap_bullets(lines[body_from:end]):
        text, fixed = trap_leadin(bullet)
        if text:
            entries.append(("- ~~%s~~ (fixed)" if fixed else "- %s") % text)

    block = [
        INDEX_BEGIN,
        "",
        "### Index",
        "",
        "One line per trap, generated by `./gs.py traps`. Scanning this "
        "costs about",
        "4k tokens against 31k for the section itself -- `grep -n` the phrase "
        "to land on",
        "the trap that explains it. **Do not edit by hand.**",
        "",
    ] + entries + ["", INDEX_END]

    return block, start, end


def trap_index(check=False):
    lines = open(HANDOVER, encoding="utf-8").read().split("\n")
    block, start, end = build_trap_index(lines)

    already = lines[start + 1:lines.index(INDEX_END) + 1] \
        if INDEX_BEGIN in lines else None
    wanted = [""] + block

    if already == wanted:
        print(f"trap index up to date ({sum(1 for l in block if l.startswith('- '))} traps)")
        return 0

    if check:
        print("trap index is stale -- run ./gs.py traps")
        return 1

    if already is not None:
        lines[start + 1:lines.index(INDEX_END) + 1] = wanted
    else:
        lines[start + 1:start + 1] = wanted

    open(HANDOVER, "w", encoding="utf-8").write("\n".join(lines))
    print(f"trap index rewritten ({sum(1 for l in block if l.startswith('- '))} traps)")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Build and run GS.")
    parser.add_argument("command", nargs="?", default="run",
                        choices=["build", "run", "clean", "gen", "sanitize",
                                 "prune", "windows", "traps", "fetch", "apk"])
    parser.add_argument("config", nargs="?", default="debug",
                        choices=CONFIGS + ["all"] + list(TOOLS))
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
    parser.add_argument("--check", action="store_true",
                        help="traps: report drift instead of rewriting the index")
    parser.add_argument("--delete", action="store_true",
                        help="with `prune`, actually remove the trees it "
                             "lists rather than only reporting them")
    parser.add_argument("--keep", action="append", metavar="TREE",
                        help="with `prune`, a tree to spare by name; may be "
                             "given more than once")
    parser.add_argument("--web-game", metavar="FOLDER",
                        help="with --target web: the game folder preloaded into the page")
    parser.add_argument("--target", choices=["linux", "windows", "web", "android"], default="linux",
                        help="with `build`: cross-build GSPlayer for another "
                             "platform (windows: llvm-mingw, fetched on first use)")
    parser.add_argument("--player-scripts", metavar="HEADER",
                        help="with `build`: build only GSPlayer, against this "
                             "compiled-script registry header, into "
                             "bin/Export-* -- what TestEnv --export runs")
    parser.add_argument("--lib", help="apk: the built libgsplayer.so")
    parser.add_argument("--game", help="apk: the exported game/ folder")
    parser.add_argument("--name", help="apk: the game's name (label, application id)")
    parser.add_argument("--icon", help="apk: the launcher icon (PNG)")
    parser.add_argument("--out", help="apk: where to write the signed .apk")
    parser.add_argument("--steps", type=int, default=SANITIZE_STEPS,
                        help=f"fixed steps per demo in a sweep (default {SANITIZE_STEPS})")
    # Everything after a bare -- goes to TestEnv rather than to this script,
    # which is the only way to reach its flags: the binary lives beside its
    # assets under bin/, so running it by hand means knowing the config-
    # dependent path. `./gs.py run -- --demo Breakout --record run.rec`.
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

    # `fetch <tool>`: the second positional is the tool's name, not a config.
    if args.command == "fetch":
        return fetch_tool(args.config)

    if args.command == "apk":
        if not all((args.lib, args.game, args.name, args.icon, args.out)):
            sys.exit("apk: --lib, --game, --name, --icon and --out are all needed")
        return package_apk(os.path.abspath(args.lib), os.path.abspath(args.game), args.name,
                           os.path.abspath(args.icon), os.path.abspath(args.out))

    if args.command == "windows":
        # Runs until interrupted. Its own script rather than something built in
        # here, because it needs a D-Bus main loop and this does not.
        bridge = os.path.join(ROOT, "tools", "gs-windows.py")
        return subprocess.call([sys.executable, bridge])

    if args.command == "traps":
        return trap_index(check=args.check)

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

    # An export build of the player: generated with the option, only that
    # target built, then the project files regenerated plain -- so a later
    # bare `make` builds the normal player, not this project's. --target
    # windows cross-compiles the same way, with llvm-mingw as the compiler:
    # premake's --os=windows picks the projects' Windows filters, and the
    # gmake2 Makefiles take CC/CXX/AR from the environment.
    if args.player_scripts or args.target != "linux":
        if args.command != "build" or args.config == "all" or IS_WINDOWS:
            sys.exit("--player-scripts / --target: `build <one config>`, on Linux")
        header = os.path.abspath(args.player_scripts) if args.player_scripts else None
        environment = dict(os.environ)
        if args.target == "windows":
            if fetch_tool("llvm-mingw") != 0:
                return 1
            toolchain = os.path.join(tool_dir("llvm-mingw"), "bin", "x86_64-w64-mingw32-")
            environment.update(CC=toolchain + "clang", CXX=toolchain + "clang++",
                               AR=toolchain + "ar", RC=toolchain + "windres")
        if args.target == "web":
            if fetch_tool("emsdk") != 0:
                return 1
            emscripten = os.path.join(tool_dir("emsdk"), "upstream", "emscripten")
            environment.update(CC=os.path.join(emscripten, "emcc"), CXX=os.path.join(emscripten, "em++"),
                               AR=os.path.join(emscripten, "emar"),
                               EM_CONFIG=os.path.join(tool_dir("emsdk"), ".emscripten"))
        # Android: the NDK's own clang, told its target and API level by
        # name -- aarch64-linux-android26-clang is a wrapper that adds
        # --target=aarch64-linux-android26. API 26 (Android 8) is the floor:
        # AAudio, which miniaudio prefers, starts there.
        android_ndk = None
        if args.target == "android":
            if fetch_tool("android") != 0:
                return 1
            android_ndk = os.path.join(tool_dir("android"), "ndk")
            llvm = os.path.join(android_ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")
            environment.update(CC=os.path.join(llvm, "aarch64-linux-android26-clang"),
                               CXX=os.path.join(llvm, "aarch64-linux-android26-clang++"),
                               AR=os.path.join(llvm, "llvm-ar"))
        web_game = os.path.abspath(args.web_game) if args.web_game else None
        if generate(allow_fetch=not args.no_fetch, player_scripts=header,
                    target_os="windows" if args.target == "windows" else None,
                    web=args.target == "web", web_game=web_game,
                    android_ndk=android_ndk) != 0:
            return 1
        # make cannot tell that the -D naming the header changed: an export
        # of a second game kept the first game's objects, and shipped the
        # first game's compiled scripts. The export player's own folders are
        # cleared so it is always compiled against the header just given.
        # The folder premake actually writes for this target: the *target's*
        # system, and the web build's -web suffix. Using the host's name here
        # once cleared the wrong folder for Windows and the web -- a second web
        # export then shipped the first game, baked into the reused .data.
        for tree in ("bin-int", "bin"):
            shutil.rmtree(os.path.join(ROOT, tree,
                "Export-" + target_folder(args.config, args.target), "GSPlayer"),
                ignore_errors=True)
        # A plain web build bakes --web-game into its .data at link time, and
        # make relinks only when an object changed: removing the page forces
        # the link that re-reads the game folder.
        if args.target == "web":
            for name in ("GSPlayer.html", "GSPlayer.data"):
                try:
                    os.remove(os.path.join(ROOT, "bin", f"{args.config.capitalize()}-linux-x86_64-web", "GSPlayer", name))
                except FileNotFoundError:
                    pass
        code = run(["make", f"-j{args.jobs}", f"config={args.config}", "GSPlayer"], env=environment)
        generate(allow_fetch=not args.no_fetch)
        return code

    if args.command == "prune":
        return prune(keep=set(args.keep or []), delete=args.delete)

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
