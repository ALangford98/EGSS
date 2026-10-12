# Android export (export piece 6)

**Status:** built 2026-10-12, under the owner's "keep going with everything"
-- decisions made during the work, recorded here for review. **Verified
statically only: it has not run on a device or an emulator** (this machine
has no `/dev/kvm`, and no phone was attached). The first run on real
hardware is the open item.

## Decisions

1. **A NativeActivity, no Java and no Gradle.** `GSPlayer` builds as
   `libgsplayer.so`; the manifest names `android.app.NativeActivity` with
   `android.app.lib_name = gsplayer` and `hasCode="false"`. NDK
   `native_app_glue` (compiled in from the NDK's source) calls `android_main`,
   which `EntryPoint.h` defines beside `main`. Gradle would be a second build
   system for one manifest and three tool calls.
2. **Toolchain fetched and pinned** by `gs.py fetch android`: NDK r29,
   build-tools 37 (aapt2, zipalign, apksigner), platform 36 (`android.jar`),
   Temurin JDK 17 (apksigner, keytool). Google publishes SHA-1 for its
   repository packages, so those are checked against SHA-1 -- the publisher's
   digest, rather than a SHA-256 taken on first download.
3. **arm64-v8a only, API 26+ (Android 8.0), target 36.** arm64 is every
   current phone; 26 is where AAudio (miniaudio's preference) starts. x86_64
   (emulators, Chromebooks) is one more `--target` away and not built yet.
4. **A `--android` premake option, like `--web`.** Generated on the Linux
   host and built by the NDK's `aarch64-linux-android26-clang`; ARM64, so
   premake hands it no `-m64` or `/usr/lib64`; `GS_PLATFORM_ANDROID` +
   `GS_GLES`; the engine links statically into the one library; libc++
   static, so the APK carries one `.so`.
5. **`GS_GLES` names the GL dialect, not the platform.** The browser's GLES
   paths (shader rewrite, `glClearBufferfv`, element buffer binding) are now
   guarded by `GS_GLES`, which web and Android both define.
6. **Platform/Android** is the window and input layer GLFW is elsewhere.
   - The **EGL context lives as long as the game**; the **surface is made and
     unmade with the activity's window**. Between the two, the window reports
     0x0 (which `Application` already reads as minimised), and `OnUpdate`
     sleeps in the event queue.
   - **A lost context is not recovered** (some drivers drop it in the
     background). It is reported in the log rather than drawn as garbage:
     rebuilding every GL object is beyond the engine today.
   - **Touch is the mouse:** the first finger down is button 0 at its
     position until it lifts; other fingers are ignored. ImGui is fed from the
     same events (`ImGuiLayer::OnEvent`) instead of its Android backend, which
     would read the input queue a second time.
   - **Back** is left to the system (it closes the game). **Logging** goes to
     logcat (`adb logcat -s GS APPLICATION`).
   - **`android_main` ends the process** after the game exits. Android may
     keep the process and call `android_main` again, with the Application
     singleton and spdlog's named loggers still set from the last run.
7. **The game is unpacked from the APK** into the app's private storage on
   first start, then the player runs it from there with ordinary file I/O, as
   on every other platform: `Assets::Resolve`, `std::ifstream` and stb_image
   are all unchanged.
   - The APK carries `assets/game/...` plus `assets/game.files`, a list of
     every file. An APK's asset directories cannot be walked for
     subdirectories at run time.
   - The list's first line is a hash of everything shipped, so the device
     unpacks again exactly when the build changed.
   - The list is written last, so an interrupted unpack is redone.
8. **GSS pointer input** (landed separately, before this): a phone has no
   keys, so `input.isPointerDown()`, `pointerX()` and `pointerY()` exist in
   both execution paths. The pointer is the mouse at a desk and the finger on
   a phone, and its position is a fraction of the window.
9. **`gs.py apk` assembles the package:**
   - aapt2 compiles the icon and links the manifest.
   - The library is stored uncompressed, and `zipalign -P 16` puts it on a
     16 KB boundary. With `extractNativeLibs="false"`, Android maps it
     straight out of the APK, which needs both; 16 KB pages are required on
     Android 15+ devices with large pages.
   - The library is stripped.
   - apksigner signs with v2 and v3, and no v4 sidecar.
   - The **debug key is made once per machine**, in
     `vendor/bin/android/debug.keystore`, so every export from this machine
     installs over the last one. A store release needs the developer's own key
     (deferred).
10. **Application id** `com.gsgame.<letters and digits of the name>`, version
    1, landscape (`sensorLandscape`), fullscreen. `configChanges` covers
    rotation, so a turn of the phone resizes the surface instead of restarting
    the activity. The project manifest has no id or orientation field yet;
    both are obvious next fields.

## Verification (static)

The build ran clean in all three configs: Debug, Release, and Dist through
the exporter. Its only warnings were in vendored fmt.

Then, for BreakoutRecreation (compiled scripts) and the falling-box game (an
interpreted script, shipped as JS):

- `aapt2 dump badging`: package `com.gsgame.breakoutrecreation`, min 26,
  target 36, `uses-gl-es 0x30000`, native code `arm64-v8a`, launchable
  `android.app.NativeActivity`.
- `zipalign -c -P 16 4`: verified.
- `apksigner verify`: v2 and v3.
- The library's data starts at offset 16384 in the APK, and begins `\x7fELF`.
- `llvm-readelf`: every LOAD segment is aligned to 0x4000.
  - `android_main` and `ANativeActivity_onCreate` are exported.
  - NEEDED lists only `libandroid`, `libEGL`, `libGLESv3`, `liblog`, `libm`,
    `libdl` and `libc`.
  - `-Wl,--no-undefined` means nothing was left unresolved.
- The Dist library holds the registry's `Paddle` and `Ball`.
- The falling-box APK carries `game/push.js` in `game.files`.

The toolchain fetch itself had a bug that only the NDK exposed. Python's
`zipfile` writes a symlink as a file containing the target's name, so `clang`
ran as a shell script ("clang-21: command not found"). Links are now
recreated, and refused if they point outside the tool's folder.

## Not done

- **Running it.** The first device run will likely find something: EGL
  config selection, the surface lifecycle, the unpack path, audio. Do it with
  `adb install -r`, then `adb logcat -s GS APPLICATION`.
- Passing engine flags such as `--lockstep --capture` to an installed game
  needs the activity's intent extras, read over JNI. Without that, the
  byte-identical capture check the other targets had cannot be run.
- x86_64, a release keystore, and an app id and orientation in the project
  manifest.
