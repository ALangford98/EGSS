# iOS export (export piece 7)

**Status:** built 2026-10-12, under the owner's "keep going with everything"
-- decisions made during the work, recorded here for review. **Never built
by Xcode, never run.** The iOS SDK exists only inside Xcode on macOS, so
nothing here could compile the Objective-C for real, link against UIKit, or
sign. The first Xcode build is the first real compile of
`Platform/iOS/iOSPlatform.mm`.

## Decisions

1. **The export is an Xcode project, not an app.** `--export-target ios`
   writes `<out>/ios/<Game>/` and a zip of it. The folder holds:
   - `<Game>.xcodeproj`;
   - `src/`: the engine, Runtime and player source, plus the vendored files
     they use, laid out as in the repository so every include resolves the
     same way;
   - `src/scripts/`: the game's compiled scripts, with a registry header
     that includes them by relative path;
   - `game/`: copied into the bundle as a folder;
   - `Info.plist`, `Assets.xcassets` with a 1024 px icon, and a README.
   - The remaining steps happen on the Mac: open the project, pick a signing
     team, press Run.
2. **The project file is generated** (`tools/ios_project.py`), not a
   template. Mostly it is the source list, and a template would go stale
   every time the engine gains a `.cpp`. Object ids are hashes of names, so
   regenerating gives the same file.
3. **OpenGL ES 3 through `CAEAGLLayer`.** OpenGL ES is deprecated on iOS
   but still shipped, and it lets the GLES renderer that web and Android use
   run unchanged. Long term the answer is Metal, natively or through ANGLE.
   **This is the decision most likely to need revisiting:** if a future SDK
   drops OpenGLES.framework, this export stops building.
4. **UIScene lifecycle** (`GSSceneDelegate`), not the older app-delegate
   window. Apple has said the scene lifecycle becomes mandatory after iOS 26.
   There is one scene: the engine is a singleton, so a second iPad window
   could not have a game of its own.
5. **No default framebuffer.** iOS draws into a framebuffer made from the
   layer. The engine now has `Framebuffer::SetScreenRendererID`, which is 0
   everywhere else, and `Unbind` returns there. This is the one renderer
   change.
6. **The run loop is UIKit's.** `main` calls `UIApplicationMain`. When the
   scene connects, the view and its context are created, then the
   application. A `CADisplayLink` calls `Application::RunFrame` once per
   refresh: the same split the web needed, used for the same reason.
   - The link is paused while the scene is inactive. iOS kills an app that
     issues GL commands in the background.
7. **Input matches Android.** One finger is mouse button 0, in pixels (touch
   points times the content scale), and the view admits a single touch. The
   GSS pointer and ImGui read it unchanged.
8. **Files are read in place**: the player starts in the bundle's resource
   folder, where `game/` sits, so there is no unpacking step as on Android.
9. **miniaudio** is compiled as Objective-C through a one-line `.m` wrapper,
   which its docs say iOS requires. It is built with `MA_NO_RUNTIME_LINKING`
   and links the audio frameworks directly; miniaudio notes that runtime
   linking can fail notarization. The wrapper is built without ARC, because
   miniaudio predates it.
10. **iOS 15+, iPhone and iPad, landscape**, bundle id
    `com.gsgame.<name>` (as on Android). Debug carries asserts; Release is
    the Dist configuration.

## What could be checked here

All of this was done with throwaway harnesses in the session scratchpad,
per the project's temporary-test rule:

- **Compile and link:** all 82 sources the project lists were compiled with
  the host clang, using the project's own defines and header search paths
  read back out of the generated `project.pbxproj`. The iOS framework's GL
  header was mapped to the host's GLES 3 headers.
  - They were linked together with stand-ins for exactly the four symbols
    `iOSPlatform.mm` defines (`Window::Create`, `Window::GetMonitors`,
    `Input::s_Instance`, `iOS::Main`).
  - Nothing else was undefined except miniaudio's Linux backends (JACK,
    PulseAudio, ALSA), which `MA_NO_RUNTIME_LINKING` links directly on a
    Linux host. So the source list is complete.
  - **This found a real gap.** `quickjs.c` includes generated `builtin-*.h`
    headers that `quickjs_premake5.lua` never names. The export now copies
    every QuickJS header.
- **The Objective-C++:** `iOSPlatform.mm` was syntax- and type-checked by
  clang against stand-in UIKit, QuartzCore and OpenGLES headers, written
  from the documented signatures. This only proves the file agrees with
  those signatures, not with the SDK.
  - As a control, a misspelled selector (`presentRenderBuffer:`) was
    injected, and the check rejected it.
- **The project file** was parsed by an independent OpenStep-plist reader.
  Every object id resolves to an object of the expected kind, and every file
  it names exists. Both configurations carry the expected defines.
- **The icon** was scaled 256 → 1024 by nearest neighbour, with alpha
  flattened onto black, because the App Store refuses alpha. Looked at, and
  correct.
- **Other targets:** desktop, web and Android builds and captures are
  unchanged and byte-identical.

## Not done

- Building in Xcode and running on a simulator or a device. The likely
  first findings are a deprecated-API warning made an error, the EAGL layer
  setup, or audio session behaviour.
- Signing and distribution: TestFlight and the App Store need the
  developer's account.
- The engine's `--capture` path reads `GL_BACK`, which is wrong with no
  default framebuffer. It only matters for automated checks, and none can
  run on iOS yet.
