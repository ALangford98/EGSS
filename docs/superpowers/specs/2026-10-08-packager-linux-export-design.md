# Packager and Linux export (export piece 2)

**Status:** approved 2026-10-08
**Scope:** piece 2 of the export roadmap in
`2026-10-07-game-runtime-player-design.md`. AppImage is a follow-up step.

## What a shipped game opens

Measured with an `LD_PRELOAD` open() logger (no strace on this machine)
against `GSPlayer`, apart from system and display libraries:

- BreakoutRecreation (compiled scripts): `project.gsproj`, `scene.txt`.
- A physics game with an interpreted script: also the `.gss` file and
  `assets/typescript.js` -- 8.9 MB, by a working-directory-relative path.
- Both: write `imgui.ini` into the working directory.

So a game is the player, `libGS.so` and its own files; the TypeScript
compiler is needed only to *compile* interpreted scripts, which can happen
at export.

## Decisions

1. **The exporter is editor-side C++** (`TestEnv/src/GameExporter.h`), run
   headless as `TestEnv --export <project> --out <dir>`. Piece 3's Export
   dialog calls the same code; the scan reuses the engine's parsers and the
   script compiler instead of re-implementing them in Python.
2. **Scripts ship as JavaScript.** `ScriptEngine` splits at its natural seam:
   compiling (import analysis, splicing, TS -> JS -- needs `typescript.js`)
   and running (wrap, evaluate, bind -- QuickJS only). A `.js` script path
   runs through the second half alone, and the TypeScript compiler is loaded
   only when something actually needs compiling.
3. **Compiled scripts are found per project:** a `<stem>.cpp` beside a
   script whose first line is `// GS-GENERATED:` is that script's graduated
   class. The exporter generates a registry header for exactly those and
   builds `GSPlayer` (Dist) against it, into `bin/Export-*` so the normal
   player build is never overwritten.
4. **A missing referenced file fails the export**, naming the file and what
   referenced it -- never a package that breaks on someone else's machine.
5. **Folder + `.tar.gz` first; AppImage is a follow-up** (it needs
   `appimagetool` fetched as `gs.py` fetches premake, a `.desktop` file and an
   icon).

## Package layout

```
<Game Name>/
  <game-name>      the player, renamed (lower case, spaces to hyphens)
  libGS.so         found through $ORIGIN
  game/            the project root
    project.gsproj
    <scenes, rewritten>
    <referenced files, project layout kept>
    external/...   referenced files from outside the project, original
                   relative layout kept so an .obj still finds its .mtl
```

Scene files are copied with every `mesh`/`material`/`script` path rewritten
to its place under `game/`; interpreted scripts become `<stem>.js` beside
where the `.gss` was. Compiled scripts ship no file (the class is in the
binary); their scene path is kept for the class name.

## Player changes

- Changes directory to its own folder at startup (after making the project
  argument absolute): a double-clicked game is not launched from there.
- With no argument, runs `game/` beside itself.
- Writes no `imgui.ini` (`GS::ImGuiLayer::SetIniFilename(nullptr)`).

## Scan

From `project.gsproj`: every scene; per scene, `mesh` (files only; `.obj`
-> `mtllib` -> its `map_*` textures, each relative to the file that names
it), `material` (`.gsmat`, self-contained), `script` (compiled sibling, or
compiled to JS -- the compile step reports its resolved imports, which are
not shipped separately: they are spliced into the `.js`).

## Verification

- `ScriptEngine` split: a script prepared from TS and from its exported JS
  gives a byte-identical capture; a JS-only game never opens
  `typescript.js` (open-logger).
- Export BreakoutRecreation and the falling-box game; unpack each tarball
  into a fresh folder, rename the source tree's `bin/` out of the way, run
  the game **from a different working directory**: capture byte-identical
  to editor Play at the same step, and the open-logger shows nothing opened
  outside the unpacked folder (system libraries aside).
- A scene referencing a missing file: export fails, naming it.
- Player run with no argument from elsewhere finds `game/`.
- Three configs build; editor regression captures unchanged.
