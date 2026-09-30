# State

**This is the only doc in the repo that is allowed to go stale, and it is kept
short so that fixing it is cheap.** Everything else — `docs/ENGINE.md`, the
trap list, the changelog — describes things that stay true once written. This
file describes where the work is *this week*, which does not.

It exists because the alternative was worse. `docs/HANDOVER.md` used to open
with a "Current state" section naming the commit `main` sat at, and it drifted
22 commits and one abandoned workflow behind reality. A session that read it —
as `CLAUDE.md` told it to, at a cost of about 53k tokens — came away with a
*confidently wrong* answer to "what are we working on", while `git log`, which
costs about 200 tokens and cannot drift, sat unread. **The expensive source was
the least current one.** So the volatile part was cut down to this page.

## Read this first, and read it with `git log`

```sh
git log --oneline -15
git status --short
git branch --show-current
```

**Where those disagree with this file, they win.** The owner commits their own
work between sessions and sometimes while a reply is being written, so any git
state you did not just observe is stale — including the branch name below.
## Where the work is

The live area is **the editor** — `TestEnv/src/Editor*.h`, `PlayMode.h`,
`ScriptEngine.h` (GSS runtime and the GSS-to-C++ transpiler) — not
`TerrainLab.h`, which was the live area before the editor pivot of 2026-09-07
and is now quiet. `editor_roadmap.md` is the todo list: its wishlist is
ordered easiest to hardest, and each item says what it still needs from the
owner before it can be scoped.

Last landed, newest first. **One line each; the write-up lives in
`docs/CHANGELOG.md`** under the same date — this list used to hold the full
write-ups and reached 1,100 lines before they were moved out on 2026-09-30.

- **2026-09-30 — normal maps for linked materials** (piece 2 of 3): the
  editor shader renders the graph's Normal output, frame from screen-space
  derivatives, no vertex tangents.
- **2026-09-30 — procedural material graph** (wishlist #11, piece 1 of 3):
  node-graph editor panel, `.gsmat`, four-map export, live scene link.
  Only albedo renders; normal mapping (#5) and roughness shading follow.
- **2026-09-28 — GSS array stdlib** (wishlist #3): `console.*`, `number[]`,
  `.length`/`.map`/`.filter`/`.toArray()` in the compiled path. `.find`,
  array literals and a collection-returning native call are deferred —
  see the spec's "Explicitly out of scope".
- **2026-09-15 — keyboard look in the editor view** (`307133f`).
- **2026-09-14 — mesh UV template export** (wishlist #9).
- **2026-09-13/14 — multiplayer foundation.**
- **2026-09-13 — Scenes panel / multi-scene projects** (`bc11510`, spec
  `2026-09-13-scene-tree-design.md`; no changelog entry).
- **2026-09-13 — editor theming** (wishlist #2, `302071d`; no changelog
  entry — the spec is the record).
- **2026-09-13 — entity grouping** (`a22ed36`; no changelog entry).
- **2026-09-13 — physics simulation during Play.**
- **2026-09-12 — right-click context menu** (wishlist #1; no changelog
  entry).
- **2026-09-12 — mesh authoring.**
- **2026-09-07 → 09-11 — the editor pivot**: scene composition, menu bar,
  terminal, text editor, scripting runtime, file tree, Play/Stop, Breakout
  recreation, cross-entity scripting, imports, the transpiler and static-link
  execution, both Breakout scripts graduated to C++, lighting, project I/O,
  profiler, Inspector ID field. One changelog entry each.

## What is next

Nothing is in progress. Open threads, none started:

- **GSS transpiler follow-ups** — `.find` (needs an optional/no-match type),
  a collection-returning native call such as `scene.findAllByTag`, array
  literals, vec3-typed class members, and script bindings for sound and
  physics (the roadmap's "foundational gaps" names the last as the real
  blocker for gameplay code).
- **Procedural materials, piece 3** — roughness in shading, which needs an
  owner decision (Cook-Torrance/GGX, or roughness-to-shininess in the
  existing Blinn-Phong). Per-vertex tangents are the upgrade path if normal
  maps spread beyond the editor shader.
- **Text editor Ctrl+Z also undoes the scene** — `EditorMenuBar::DoUndo`
  has no focus guard; a one-line fix, found during the material work.
- **Mesh authoring follow-ups** — face/edge-picking UI (its own sub-project),
  and the deferred fixes listed in `editor_roadmap.md`.
- **Breakout's recreation still has no bricks** — possible since
  `scene.spawn` landed; nobody has asked for it.
- **Only Cube3D implements `OnExportToScene`**; whether the other demos should
  is undecided.
- **Character attributes** — deferred by the owner; **ask before starting**.
- **Editor click-through** — lighting, project I/O, text editor and profiler
  have never been exercised by real mouse/keyboard input; no GUI-automation
  tool exists here.

## When this file is wrong

Fix it in the same commit as the work that made it wrong. It is one page on
purpose: if updating it feels like a chore, it has grown too big. A landing
gets **one line** here and its write-up in `docs/CHANGELOG.md`; durable facts
about a subsystem belong in `docs/HANDOVER.md`'s subsystem table. Older plans
in `docs/superpowers/plans/` tell their final task to "add a Last landed
bullet in the same style" as the full write-ups — that instruction is what
grew this file, so read it as "one line here, the write-up in the changelog".
