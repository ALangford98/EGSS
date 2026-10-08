# Packager and Linux Export Implementation Plan

> Executed natively, committed on `main`.

**Spec:** `docs/superpowers/specs/2026-10-08-packager-linux-export-design.md`

## Review Focus

1. A project referencing an engine asset by a binary-relative path
   (`assets/models/x.obj`) -- copied under `external/` and rewritten.
2. A game name with characters unsafe in a file name -- sanitised.
3. Exporting twice into the same `--out` -- the old package is replaced,
   not merged with stale files.
4. A script importing a module -- the module is spliced into the `.js`,
   not missing at run time.

### Task 1: Split ScriptEngine; .js scripts in GameSession
- [ ] CompileScriptToJS / PrepareEntityScriptFromJS; TS loaded lazily.
- [ ] Byte-identical capture TS vs JS; JS game never opens typescript.js.

### Task 2: Player: chdir, default `game/`, no imgui.ini
- [ ] `GS::ImGuiLayer::SetIniFilename`; player changes.

### Task 3: GameExporter + `--export` + per-project registry build
- [ ] Scan, compile, registry, build into bin/Export-*, stage, rewrite, tar.
- [ ] Missing-file failure; re-export replaces.

### Task 4: End-to-end verification and docs
- [ ] Both games: unpack elsewhere, run from another cwd, byte-identical,
      opens nothing outside; docs; commit.
