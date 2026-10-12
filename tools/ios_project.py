#!/usr/bin/env python3
"""Writes an exported game as an Xcode project, to finish on a Mac (export
piece 7; docs/superpowers/specs/2026-10-12-ios-export-design.md).

    gs.py xcode --game <exported game/> --name <Game> --icon <png>
                --scripts <compiled_scripts.gen.h> --out <folder>

Nothing iOS can be built on Linux -- the SDK comes only with Xcode -- so this
writes everything Xcode needs and stops: the engine, runtime and player
source the game uses, its compiled scripts, the game folder, an Info.plist,
an app icon, and project.pbxproj. Opening <Game>.xcodeproj, choosing a
signing team and pressing Run is meant to be all that is left.

The project file is generated rather than kept as a template, because its
contents are mostly the source list, and a template would have to be edited
every time a .cpp is added to the engine -- the failure premake's
regenerate-every-build exists to prevent on the desktop.
"""
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The engine's platform layers other than this one are left out; each would
# include headers (GLFW, the NDK's) that iOS does not have.
OTHER_PLATFORMS = ("Platform/Glfw", "Platform/Android", "Platform/Windows")

IMGUI_FILES = ["imgui.h", "imconfig.h", "imgui_internal.h", "imstb_rectpack.h",
               "imstb_textedit.h", "imstb_truetype.h", "imgui.cpp", "imgui_draw.cpp",
               "imgui_tables.cpp", "imgui_widgets.cpp", "imgui_demo.cpp",
               "backends/imgui_impl_opengl3.h", "backends/imgui_impl_opengl3.cpp",
               "backends/imgui_impl_opengl3_loader.h"]

# The sources quickjs_premake5.lua compiles, and nothing else: quickjs-libc.c
# gives scripts file and process access and is deliberately left out
# everywhere. Every header is copied -- quickjs.c includes generated
# builtin-*.h files the premake list never names, which the first link
# check here found missing.
QUICKJS_SOURCES = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"]

FRAMEWORKS = ["UIKit", "QuartzCore", "OpenGLES", "AVFoundation", "AudioToolbox",
              "CoreAudio", "CoreFoundation", "Foundation"]

DEPLOYMENT_TARGET = "15.0"


def app_id(name):
    """com.gsgame.<letters and digits>, as the Android export names it."""
    segment = re.sub(r"[^a-z0-9]", "", name.lower()) or "game"
    if not segment[0].isalpha():
        segment = "g" + segment
    return "com.gsgame." + segment


def copy(source, destination):
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    shutil.copyfile(source, destination)


def copy_sources(out):
    """The source the player needs, laid out as in the repository so every
    #include resolves the same way. Returns the files to compile."""
    src = os.path.join(out, "src")
    compiled = []

    engine = os.path.join(ROOT, "GS", "src")
    for directory, _, names in os.walk(engine):
        relative_dir = os.path.relpath(directory, engine).replace(os.sep, "/")
        if any(relative_dir == p or relative_dir.startswith(p + "/") for p in OTHER_PLATFORMS):
            continue
        for filename in sorted(names):
            if not filename.endswith((".h", ".cpp", ".mm")):
                continue
            relative = os.path.join("GS", "src", os.path.relpath(os.path.join(directory, filename), engine))
            copy(os.path.join(ROOT, relative), os.path.join(src, relative))
            if filename.endswith((".cpp", ".mm")):
                compiled.append("src/" + relative.replace(os.sep, "/"))

    for filename in IMGUI_FILES:
        relative = "GS/vendor/imgui/" + filename
        copy(os.path.join(ROOT, relative), os.path.join(src, relative))
        if filename.endswith(".cpp"):
            compiled.append("src/" + relative)
    quickjs = os.path.join(ROOT, "GS", "vendor", "quickjs")
    for filename in sorted(os.listdir(quickjs)):
        if filename.endswith(".h") or filename in QUICKJS_SOURCES:
            relative = "GS/vendor/quickjs/" + filename
            copy(os.path.join(ROOT, relative), os.path.join(src, relative))
            if filename in QUICKJS_SOURCES:
                compiled.append("src/" + relative)
    for filename in sorted(os.listdir(os.path.join(ROOT, "GS", "vendor", "stb_image"))):
        relative = "GS/vendor/stb_image/" + filename
        copy(os.path.join(ROOT, relative), os.path.join(src, relative))
        if filename.endswith(".cpp"):
            compiled.append("src/" + relative)
    for filename in ("miniaudio.h", "miniaudio.c"):
        relative = "GS/vendor/miniaudio/" + filename
        copy(os.path.join(ROOT, relative), os.path.join(src, relative))
    # miniaudio must be compiled as Objective-C on iOS (it talks to
    # AVAudioSession); a one-line .m wrapper does that without renaming it.
    wrapper = os.path.join(src, "ios", "miniaudio.m")
    os.makedirs(os.path.dirname(wrapper), exist_ok=True)
    with open(wrapper, "w") as handle:
        handle.write("// Compiles miniaudio as Objective-C, which iOS requires.\n"
                     "#include \"../GS/vendor/miniaudio/miniaudio.c\"\n")
    compiled.append("src/ios/miniaudio.m")

    for tree in ("GS/vendor/spdlog/include", "GS/vendor/glm/glm"):
        shutil.copytree(os.path.join(ROOT, tree), os.path.join(src, tree), dirs_exist_ok=True)
    for name in sorted(os.listdir(os.path.join(ROOT, "Runtime"))):
        if name.endswith(".h"):
            copy(os.path.join(ROOT, "Runtime", name), os.path.join(src, "Runtime", name))
    for name in sorted(os.listdir(os.path.join(ROOT, "Player", "src"))):
        if name.endswith((".h", ".cpp")):
            copy(os.path.join(ROOT, "Player", "src", name), os.path.join(src, "Player", "src", name))
            if name.endswith(".cpp"):
                compiled.append("src/Player/src/" + name)
    return compiled


def copy_scripts(header, out):
    """The game's compiled scripts. GameExporter's registry header includes
    each by absolute path on this machine; the copy names them beside it."""
    scripts = os.path.join(out, "src", "scripts")
    os.makedirs(scripts, exist_ok=True)
    lines = []
    with open(header) as handle:
        for line in handle:
            match = re.match(r'#include "(/.+\.cpp)"', line.strip())
            if match:
                name = os.path.basename(match.group(1))
                shutil.copyfile(match.group(1), os.path.join(scripts, name))
                line = f'#include "{name}"\n'
            lines.append(line)
    with open(os.path.join(scripts, "compiled_scripts.gen.h"), "w") as handle:
        handle.writelines(lines)


# --- the app icon ----------------------------------------------------------
# iOS takes one 1024x1024 icon and makes the rest. The engine's default is
# 256x256 and there is no imaging library here, so 8-bit PNGs are decoded
# and scaled by nearest neighbour in a few lines (exact for 256 -> 1024).

def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"icon '{path}' is not a PNG")
    offset, idat, header = 8, b"", None
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        offset += 12 + length
    width, height, depth, colour, _, _, interlace = header
    channels = {2: 3, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace:
        sys.exit(f"icon '{path}': only 8-bit RGB/RGBA non-interlaced PNGs are supported")
    raw, stride = zlib.decompress(idat), width * channels
    rows, previous, position = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[position], bytearray(raw[position + 1:position + 1 + stride])
        position += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up, corner = previous[i], previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append(line)
        previous = line
    return width, height, channels, rows


def write_icon(source, destination, size=1024):
    width, height, channels, rows = read_png(source)
    out = bytearray()
    for y in range(size):
        row = rows[y * height // size]
        out.append(0)
        for x in range(size):
            pixel = row[(x * width // size) * channels:(x * width // size) * channels + channels]
            # The App Store refuses an icon with an alpha channel: flattened
            # onto black, which the default icon's opaque square is already.
            if channels == 4:
                alpha = pixel[3]
                pixel = bytes(c * alpha // 255 for c in pixel[:3])
            out += pixel
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    with open(destination, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(out), 9)) + chunk(b"IEND", b""))


# --- Info.plist --------------------------------------------------------------

def plist(value, indent=0):
    pad = "\t" * indent
    if isinstance(value, bool):
        return pad + ("<true/>" if value else "<false/>")
    if isinstance(value, str):
        escaped = value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        return pad + f"<string>{escaped}</string>"
    if isinstance(value, list):
        return "\n".join([pad + "<array>"] + [plist(v, indent + 1) for v in value] + [pad + "</array>"])
    lines = [pad + "<dict>"]
    for key, item in value.items():
        lines.append(pad + "\t" + f"<key>{key}</key>")
        lines.append(plist(item, indent + 1))
    return "\n".join(lines + [pad + "</dict>"])


def write_info_plist(path, name):
    landscape = ["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"]
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": name,
        "CFBundleExecutable": "$(EXECUTABLE_NAME)",
        "CFBundleIdentifier": "$(PRODUCT_BUNDLE_IDENTIFIER)",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "$(PRODUCT_NAME)",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "LSRequiresIPhoneOS": True,
        "ITSAppUsesNonExemptEncryption": False,
        "UIRequiredDeviceCapabilities": ["arm64", "opengles-3"],
        "UIRequiresFullScreen": True,
        "UIStatusBarHidden": True,
        "UILaunchScreen": {},
        "UISupportedInterfaceOrientations": landscape,
        "UISupportedInterfaceOrientations~ipad": landscape,
        # One window scene: the engine is a singleton, so a second iPad
        # window could not have a game of its own.
        "UIApplicationSceneManifest": {
            "UIApplicationSupportsMultipleScenes": False,
            "UISceneConfigurations": {
                "UIWindowSceneSessionRoleApplication": [{
                    "UISceneConfigurationName": "Default",
                    "UISceneDelegateClassName": "GSSceneDelegate",
                }],
            },
        },
    }
    with open(path, "w") as handle:
        handle.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                     '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
                     '<plist version="1.0">\n' + plist(info) + "\n</plist>\n")


# --- project.pbxproj ---------------------------------------------------------
# The old-style (OpenStep) property list Xcode reads: objects keyed by 24-hex
# ids, which are derived from a name here so regenerating gives the same file.

def object_id(key):
    return hashlib.md5(key.encode()).hexdigest()[:24].upper()


def quote(value):
    if re.fullmatch(r"[A-Za-z0-9_$./]+", value):
        return value
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def serialise(value, indent):
    pad = "\t" * indent
    if isinstance(value, dict):
        inner = "".join(f"{pad}\t{quote(k)} = {serialise(v, indent + 1)};\n" for k, v in value.items())
        return "{\n" + inner + pad + "}"
    if isinstance(value, list):
        inner = "".join(f"{pad}\t{serialise(v, indent + 1)},\n" for v in value)
        return "(\n" + inner + pad + ")"
    return quote(str(value))


FILE_TYPES = {".cpp": "sourcecode.cpp.cpp", ".c": "sourcecode.c.c", ".mm": "sourcecode.cpp.objcpp",
              ".m": "sourcecode.c.objc"}


def write_pbxproj(path, name, compiled):
    objects = {}
    target = object_id("target")
    project = object_id("project")
    product = object_id("product")
    sources_phase, resources_phase, frameworks_phase = object_id("sources"), object_id("resources"), object_id("frameworks")

    source_refs, source_builds = [], []
    for relative in compiled:
        ref, build = object_id("ref:" + relative), object_id("build:" + relative)
        objects[ref] = {"isa": "PBXFileReference", "lastKnownFileType": FILE_TYPES[os.path.splitext(relative)[1]],
                        "path": relative, "sourceTree": "SOURCE_ROOT"}
        entry = {"isa": "PBXBuildFile", "fileRef": ref}
        # Vendored C, and miniaudio's Objective-C, which predates ARC: their
        # warnings are someone else's, and ARC would reject miniaudio.
        if relative.endswith(".m"):
            entry["settings"] = {"COMPILER_FLAGS": "-fno-objc-arc -w"}
        elif "/vendor/" in relative:
            entry["settings"] = {"COMPILER_FLAGS": "-w"}
        objects[build] = entry
        source_refs.append(ref)
        source_builds.append(build)

    resources = []
    for key, path_name, file_type in (("game", "game", "folder"),
                                      ("assets", "Assets.xcassets", "folder.assetcatalog")):
        ref, build = object_id("ref:" + key), object_id("build:" + key)
        objects[ref] = {"isa": "PBXFileReference", "lastKnownFileType": file_type,
                        "path": path_name, "sourceTree": "<group>"}
        objects[build] = {"isa": "PBXBuildFile", "fileRef": ref}
        resources.append((ref, build))
    plist_ref = object_id("ref:Info.plist")
    objects[plist_ref] = {"isa": "PBXFileReference", "lastKnownFileType": "text.plist.xml",
                          "path": "Info.plist", "sourceTree": "<group>"}
    objects[product] = {"isa": "PBXFileReference", "explicitFileType": "wrapper.application",
                        "includeInIndex": "0", "path": name + ".app", "sourceTree": "BUILT_PRODUCTS_DIR"}

    sources_group, products_group, main_group = object_id("group:sources"), object_id("group:products"), object_id("group:main")
    objects[sources_group] = {"isa": "PBXGroup", "children": source_refs, "name": "Source", "sourceTree": "<group>"}
    objects[products_group] = {"isa": "PBXGroup", "children": [product], "name": "Products", "sourceTree": "<group>"}
    objects[main_group] = {"isa": "PBXGroup", "children": [r for r, _ in resources] + [plist_ref, sources_group, products_group],
                           "sourceTree": "<group>"}

    objects[sources_phase] = {"isa": "PBXSourcesBuildPhase", "buildActionMask": "2147483647",
                              "files": source_builds, "runOnlyForDeploymentPostprocessing": "0"}
    objects[resources_phase] = {"isa": "PBXResourcesBuildPhase", "buildActionMask": "2147483647",
                                "files": [b for _, b in resources], "runOnlyForDeploymentPostprocessing": "0"}
    objects[frameworks_phase] = {"isa": "PBXFrameworksBuildPhase", "buildActionMask": "2147483647",
                                 "files": [], "runOnlyForDeploymentPostprocessing": "0"}

    search_paths = ["$(SRCROOT)/src/GS/src", "$(SRCROOT)/src/GS/src/Platform/GLES/shim",
                    "$(SRCROOT)/src/GS/vendor/spdlog/include", "$(SRCROOT)/src/GS/vendor/glm",
                    "$(SRCROOT)/src/GS/vendor/imgui", "$(SRCROOT)/src/GS/vendor/stb_image",
                    "$(SRCROOT)/src/GS/vendor/miniaudio", "$(SRCROOT)/src/GS/vendor/quickjs",
                    "$(SRCROOT)/src/Runtime", "$(SRCROOT)/src/scripts"]
    common_defines = ["GS_PLATFORM_IOS", "GS_GLES", "IMGUI_IMPL_OPENGL_ES3", "GL_SILENCE_DEPRECATION",
                      "MA_NO_RUNTIME_LINKING", "_GNU_SOURCE",
                      "GS_COMPILED_SCRIPTS_HEADER=<compiled_scripts.gen.h>"]
    linker = []
    for framework in FRAMEWORKS:
        linker += ["-framework", framework]

    def target_settings(debug):
        return {
            "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "CLANG_ENABLE_OBJC_ARC": "YES",
            "CODE_SIGN_STYLE": "Automatic",
            "DEVELOPMENT_TEAM": "",
            "GCC_C_LANGUAGE_STANDARD": "gnu11",
            "GCC_PREPROCESSOR_DEFINITIONS": common_defines + (["GS_DEBUG", "GS_ENABLE_ASSERTS"] if debug else ["GS_DIST"]),
            "GENERATE_INFOPLIST_FILE": "NO",
            "HEADER_SEARCH_PATHS": search_paths,
            "INFOPLIST_FILE": "Info.plist",
            "IPHONEOS_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
            "OTHER_LDFLAGS": linker,
            "PRODUCT_BUNDLE_IDENTIFIER": app_id(name),
            "PRODUCT_NAME": "$(TARGET_NAME)",
            "SDKROOT": "iphoneos",
            "TARGETED_DEVICE_FAMILY": "1,2",
        }

    def project_settings(debug):
        settings = {"SDKROOT": "iphoneos", "IPHONEOS_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
                    "ONLY_ACTIVE_ARCH": "YES" if debug else "NO",
                    "GCC_OPTIMIZATION_LEVEL": "0" if debug else "s",
                    "DEBUG_INFORMATION_FORMAT": "dwarf" if debug else "dwarf-with-dsym",
                    "ENABLE_TESTABILITY": "YES" if debug else "NO"}
        return settings

    lists = {}
    for owner, settings_for in (("project", project_settings), ("target", target_settings)):
        configurations = []
        for config, debug in (("Debug", True), ("Release", False)):
            config_id = object_id(f"config:{owner}:{config}")
            objects[config_id] = {"isa": "XCBuildConfiguration", "buildSettings": settings_for(debug), "name": config}
            configurations.append(config_id)
        list_id = object_id("configlist:" + owner)
        objects[list_id] = {"isa": "XCConfigurationList", "buildConfigurations": configurations,
                            "defaultConfigurationIsVisible": "0", "defaultConfigurationName": "Release"}
        lists[owner] = list_id

    objects[target] = {"isa": "PBXNativeTarget", "buildConfigurationList": lists["target"],
                       "buildPhases": [sources_phase, frameworks_phase, resources_phase],
                       "buildRules": [], "dependencies": [], "name": name, "productName": name,
                       "productReference": product, "productType": "com.apple.product-type.application"}
    objects[project] = {"isa": "PBXProject",
                        "attributes": {"BuildIndependentTargetsInParallel": "1", "LastUpgradeCheck": "1600"},
                        "buildConfigurationList": lists["project"], "compatibilityVersion": "Xcode 14.0",
                        "developmentRegion": "en", "hasScannedForEncodings": "0", "knownRegions": ["en", "Base"],
                        "mainGroup": main_group, "productRefGroup": products_group,
                        "projectDirPath": "", "projectRoot": "", "targets": [target]}

    document = {"archiveVersion": "1", "classes": {}, "objectVersion": "56",
                "objects": objects, "rootObject": project}
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write("// !$*UTF8*$!\n" + serialise(document, 0) + "\n")


def main(game, name, icon, scripts, out):
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    compiled = copy_sources(out)
    copy_scripts(scripts, out)
    shutil.copytree(game, os.path.join(out, "game"))

    icons = os.path.join(out, "Assets.xcassets", "AppIcon.appiconset")
    os.makedirs(icons)
    write_icon(icon, os.path.join(icons, "icon-1024.png"))
    with open(os.path.join(icons, "Contents.json"), "w") as handle:
        json.dump({"images": [{"filename": "icon-1024.png", "idiom": "universal", "platform": "ios",
                               "size": "1024x1024"}], "info": {"author": "gs", "version": 1}}, handle, indent=2)
    with open(os.path.join(out, "Assets.xcassets", "Contents.json"), "w") as handle:
        json.dump({"info": {"author": "gs", "version": 1}}, handle, indent=2)

    write_info_plist(os.path.join(out, "Info.plist"), name)
    write_pbxproj(os.path.join(out, name + ".xcodeproj", "project.pbxproj"), name, compiled)
    with open(os.path.join(out, "README.txt"), "w") as handle:
        handle.write(f"""{name} -- a GS game for iOS, as an Xcode project.

Needs a Mac with Xcode 15 or later (iOS {DEPLOYMENT_TARGET}+ devices).

1. Open "{name}.xcodeproj".
2. Select the "{name}" target > Signing & Capabilities, and choose your Team
   (a free Apple ID works for your own devices). If the bundle identifier
   {app_id(name)} is taken, change it there.
3. Pick your iPhone or iPad (or a simulator) and press Run.

The game itself is in game/; src/ is the engine source it runs on, and
src/scripts/ its compiled scripts. This project was generated on Linux and
has not been built by the exporter -- the first build is Xcode's.
""")
    print(f"Xcode project: {os.path.join(out, name + '.xcodeproj')} ({len(compiled)} sources)")
    return 0


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for flag in ("--game", "--name", "--icon", "--scripts", "--out"):
        parser.add_argument(flag, required=True)
    args = parser.parse_args()
    sys.exit(main(os.path.abspath(args.game), args.name, os.path.abspath(args.icon),
                  os.path.abspath(args.scripts), os.path.abspath(args.out)))
