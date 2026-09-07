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

**Branch: `main`.** The owner asked to stay on it directly for the docs work;
`density-in-double` merged in `cf266b8` and is finished with.

The live area is **`TestEnv/src/TerrainLab.h`** (~11k lines) and the headers
around it — `Critters.h`, `Vegetation.h`, `Rocks.h`, `Climate.h`. It is a
first-person terrain demo that grew a felling-and-building game: you cut trees,
buck them into logs, rive planks, and raise structures out of a grid-snapped
kit of boards, logs and stone.

Last landed, newest first:

- **Chunk meshes merge into fixed 3x3x3 groups, on the planet too.**
  `VoxelPlanet.h`'s chunks now draw the same way TerrainLab's do: 1,459
  meshed chunks down to 167 groups on a landed Earth. Harder than the flat
  field — chunks stream, so a group is dirtied by `StreamAround`/
  `EvictBeyond` rather than an edit, and each member's vertices have to be
  re-expressed against the group's own double-precision origin before being
  concatenated. Verified by a self-test showing triangle counts exactly
  preserved under grouping, plus byte-identical captures across repeated
  runs and Debug/Release. See the changelog, 2026-09-07. This finishes the
  roadmap item that started with TerrainLab's 93-down-to-9 (changelog,
  2026-09-04).
- **The context pipeline.** This file, a generated trap index
  (`./gs.py traps`, `--check` to detect drift), and a tiered read ladder in
  `CLAUDE.md`. Answering "what are we working on" cold went from ~252k tokens
  and wrong to ~3.9k and right.
- **Instancing.** Panels and animals batched: 764 draw calls → 33, submission
  1420 µs → 66 µs. Verified by rendering both ways and comparing pixels —
  byte-identical, 3,686,400 of 3,686,400 samples. Frame time did not move,
  because 16.667 ms is vsync.
- **`./gs.py prune`.** The checkout was 9.7 GB against 1.2 MB of source.
  Now 1.7 GB.
- **Life.** Boids, an Ornstein–Uhlenbeck lek swarm, active Brownian beetles —
  each checked against a formula `Critters.h` does not evaluate.

## What is next

**In flight: the editor pivot.** Boot the app into an editor environment
(building on `EditorShell.h`) rather than a selected demo — import meshes,
compose scenes in `GS::Scene`, attach logic, and eventually play the result
in place. Being brainstormed as a sequence of sub-projects: (1) editor boot +
scene composition, (2) a mesh authoring tool, (3) a logic/module attachment
mechanism, (4) play-in-editor. See `docs/superpowers/specs/` for the design
doc once it lands.

Unstarted, in the order they were last discussed:

- **Multiplayer/networking foundation.** Nothing exists yet — no transport, no
  replication, no session model. Explicitly out of scope for the editor pivot
  above; scope it out as its own sub-project once the editor's scene model
  exists to replicate.
- **Character attributes.** `s_Strength` in `TerrainLab.h` is deliberately a
  constant with a hook where a stat should be — carry capacity trained by use.
  The owner deferred this ("further down the line"), so **ask before starting
  it**.

## When this file is wrong

Fix it in the same commit as the work that made it wrong. It is one page on
purpose: if updating it feels like a chore, it has grown too big, and the
subsystem table in `docs/HANDOVER.md` is where durable facts belong instead.
