# Procedural material graph

**Status:** approved in brainstorming 2026-09-30, ready for planning
**Scope:** piece 1 of 3 of `editor_roadmap.md` wishlist #11 ("Texture
editor"), redirected during brainstorming from a paint application to a
**procedural, node-graph material generator**. Pieces 2 (normal-map
rendering — wishlist #5) and 3 (roughness in shading) get their own specs.

## Problem and intent

The owner wants to author **procedural materials** — tileable stone, wood,
rust, grass — inside the editor rather than painting them, and to see them
live on objects in the scene. Brush/canvas painting, image import and 2D
sprite work are explicitly *not* the goal of this piece.

The owner asked for a **full PBR output set** (albedo, height, normal,
roughness). The renderer cannot show most of that yet:

- The editor's lit shader samples no texture at all
  (`EditorSceneView.h`, comment above `BuildShader`: every placed entity is
  flat-coloured).
- Shading is Blinn-Phong — `Material` maps `.mtl` specular colour and
  exponent (`u_SpecularColor`, `u_Shininess`); nothing shades with roughness
  or metallic. `GltfLoader` parses `metallicFactor`/`roughnessFactor` and no
  shader reads them.
- `MeshVertex` has no tangent, so a normal map cannot be sampled correctly
  (wishlist #5).

So the work splits into three independently verifiable pieces, built in
order: **(1) this generator**, which produces all four maps as files and
shows albedo live; (2) normal-map rendering; (3) roughness in shading — the
last is an owner decision (switch to Cook-Torrance/GGX, or keep Blinn-Phong
and derive per-pixel shininess) because it changes how every lit object in
the engine looks. Until 2 and 3 land, a generated material renders with its
albedo only.

**Success looks like:** building a stone or wood material as a node graph
in an editor panel, seeing it update on the entities using it as a slider
moves, and exporting four PNGs that tile seamlessly.

## Decisions made during brainstorming

1. **Procedural, not painted.** The roadmap entry's canvas/brush/layers
   half is out of this piece entirely.
2. **Full PBR set exported now**, renderer support deferred to pieces 2/3.
3. **Node graph** (Substance-Designer-shaped), chosen over a layer stack and
   over hand-written presets.
4. **Vendor `imnodes`** for the graph UI rather than hand-rolling one on
   `ImDrawList`. Risk: upstream imnodes predates ImGui 1.92, and this repo
   vendors **ImGui 1.92.9b** (docking). The first plan task is a compile
   spike; if it fails, stop and decide between patching imnodes and
   hand-rolling — do not build on top of an unverified dependency.
5. **Not generalised for wishlist #12 (visual scripting).** The node editor
   is material-specific. Generalise it when a second consumer exists, not
   before ("a component with no system").
6. **Export + live link.** PNGs and a `.mtl` for the existing path, plus a
   `MeshComponent::MaterialPath` that updates linked entities live.
7. **Own undo stack, not `EditorHistory`.** `EditorHistory` is scene-scoped
   by design (its header says so; every command resolves against
   `g_EditorScene`), and mesh authoring kept its own stack for the same
   reason.

## Architecture

| Unit | Location | Job | Depends on |
| --- | --- | --- | --- |
| Noise | `GS/src/GS/Procedural/Noise.h/.cpp` | Pure functions: tileable gradient (Perlin) noise with integer period, fBm, tileable Voronoi | glm |
| MaterialGraph | `GS/src/GS/Procedural/MaterialGraph.h/.cpp` | Data model, node evaluation, evaluator (topo sort, cycle rejection, cache, dirty propagation) | Noise |
| Serializer | `GS/src/GS/Procedural/MaterialGraphSerializer.h/.cpp` | `.gsmat` JSON read (`GS::JsonValue::Parse`) and write (hand-rolled, as elsewhere) | MaterialGraph, `GS/Json.h` |
| Export | `GS/src/GS/Procedural/MaterialExport.h/.cpp` | Four PNGs via `stb_image_write` + a `.mtl` | MaterialGraph |
| MaterialEditorPanel | `TestEnv/src/MaterialEditorPanel.h` | imnodes canvas, parameter inspector, node thumbnails, map preview, undo, Save/Export | all above, imnodes |
| Scene link | `Components.h`, `Scene.cpp`, `EditorSceneView.h` | `MeshComponent::MaterialPath`, shared evaluated-graph cache, albedo sampling in the editor shader | MaterialGraph |

**`GS/` holds no UI.** A graph can be built, evaluated and checked from a
self-test with no window and no GL context. The panel only translates
imnodes interaction into graph edits and uploads evaluated images to
`Texture2D`s.

**Images** are `float` per channel (Grey: 1 channel, Colour: 4, RGBA) at the
graph's resolution — a power of two from 256 to 2048, default 512 — and are
quantised to 8-bit only at export and upload, so chained blends and levels
do not lose precision at each step.

## Node set (v1)

Every node preserves the **tiling invariant**: if its inputs tile
seamlessly, its output does. Generators tile by construction (integer
periods); filters that read neighbours wrap at the edges.

**Generators** (no inputs → Grey)

- **Noise** — tileable Perlin fBm. Params: scale (integer period), octaves,
  lacunarity (integer, so every octave's period divides the image), gain,
  seed.
- **Voronoi** — tileable cellular noise. Params: scale (integer cells per
  side), seed, jitter ∈ [0,1], mode: F1 / F2−F1 / cell ID.
- **Pattern** — bricks/tiles. Params: rows, columns, mortar width, row
  offset. Second output: a random value per brick.
- **Constant** — one value.

**Filters** (Grey → Grey)

- **Levels** — in-min, in-max, gamma, out-min, out-max.
- **Invert**.
- **Blur** — box or Gaussian, radius, wrapping.
- **Warp** — displaces each pixel's lookup by a second Grey input × strength,
  wrapping.
- **Transform** — integer tiling, offset, 90° rotations (arbitrary rotation
  would break tiling).

**Combine**

- **Blend** — A, B, optional Grey mask; modes mix, add, subtract, multiply,
  overlay, min, max, difference. A and B must be the same type (Grey or
  Colour); output takes that type.

**Colour**

- **Gradient Map** — Grey → Colour through an editable ramp of up to 8 stops.
  The only way into Colour, which keeps pin typing simple.
- **Colour Constant**.

**Derived**

- **Height → Normal** — Sobel with strength, wrapping; outputs tangent-space
  normals packed as RGB, OpenGL convention (+Y up) — what piece 2 will read.

**Outputs** — Albedo (Colour), Height (Grey), Normal (Colour; if
unconnected, derived from Height with a default-strength Height → Normal),
Roughness (Grey). At most one of each per graph. Unconnected outputs export
flat defaults: albedo mid-grey (0.5, 0.5, 0.5, 1), height 0.5, normal
(0.5, 0.5, 1), roughness 0.5.

**Pin types are checked at connect time.** Grey→Colour is refused with a
tooltip rather than silently converted.

**Not in v1**, each a single node type later with no structural change:
paint/brush, image import, directional/anisotropic blur, curves, HSV adjust.

## Evaluation

- **Order:** topological sort over nodes that reach at least one output.
  Nodes that don't are skipped for export but still evaluated for their
  thumbnails.
- **Cycles:** a link that would create one is rejected at connect time; the
  graph is never in a cyclic state.
- **Cache and dirtiness:** each node caches its output image and a dirty
  flag. A parameter or link change dirties that node and everything
  downstream; nothing upstream recomputes.
- **Cost:** 512² fBm at 6 octaves is ~1.5M gradient-noise evaluations —
  estimated tens of ms single-threaded, 16× that at 2048². Evaluate on the
  main thread and **measure first** (profiler panel). While a slider is being
  dragged, evaluate at ¼ resolution; do one full-resolution pass on release.
  The reduced pass is preview-only, never exported. A worker thread
  evaluating a graph snapshot is the escape hatch if the measured full pass
  still stalls — not built until the numbers ask for it.
- **Determinism:** same graph + resolution → byte-identical images every run.
  No clocks, no `rand()`; per-node seeds are hashed from the node's seed
  parameter. Exported PNGs can be hash-compared as a regression check.

## Editor

- **Opening:** clicking a `.gsmat` in the File Tree, or File → New Material.
- **Adding nodes:** right-click on the canvas, menu grouped by the categories
  above.
- **Undo/redo:** per open graph, snapshot-based — each edit stores the
  pre-edit serialised `.gsmat` text (graphs are tens of nodes; snapshots are
  cheap and there is no second command hierarchy to keep in sync). Ctrl+Z
  routes to whichever panel has focus.
- **Save** writes the `.gsmat`, including node positions so the layout
  survives. **Export** writes the maps (below).

## Export and scene link

**Export** writes `<name>_albedo.png`, `<name>_height.png`,
`<name>_normal.png`, `<name>_roughness.png` (8-bit per channel via the
vendored `stb_image_write`) and `<name>.mtl` with `map_Kd` pointing at the
albedo, beside the `.gsmat`. Height is 8-bit — enough for a normal derived at
generation time; a 16-bit height for displacement is a later change if a
consumer appears. The export dialog says normal and roughness are written
but not yet rendered.

**Scene link**

- `MeshComponent` gains `std::string MaterialPath`, empty by default.
- `Scene::Save` writes it as its own line, `material <path>`, after the
  entity's `mesh` line, only when non-empty. `Scene::Load` reads it into the
  current entity's `MeshComponent`. Scenes written before this field load
  unchanged (no `material` line → empty path).
- The Inspector gets a Material field using the existing file browser,
  filtered to `.gsmat`.
- The editor holds **one evaluated graph per `.gsmat` path**, shared the way
  `MeshCache` shares geometry, with one albedo `Texture2D`. Each linked
  entity's material instance binds it. An edit re-uploads the texture, so
  every entity using the material updates the same frame.
- The editor lit shader gains `u_AlbedoMap` and `u_HasAlbedoMap`. When
  `u_HasAlbedoMap` is 0 the shader takes exactly today's path; when 1,
  albedo multiplies `u_Color`. Verified byte-identical below.
- Play mode renders through the same path.

## Failure handling

- **Malformed `.gsmat`:** the panel shows the parser's error and opens
  nothing; a linked entity falls back to its flat `Color` and logs one
  warning (not one per frame).
- **Unknown node type** (e.g. a file from a newer build): refuse the whole
  load, naming the type. Loading the rest and dropping the node would
  corrupt the file on the next save.
- **Link to a missing `.gsmat`:** same flat-colour fallback; the Inspector
  field shows it in red.
- **Export write failure:** reported; nothing half-written — each file is
  written to a temporary name and renamed into place.

## Verification

Per `CLAUDE.md`: compare against formulas the code does not know, via a
temporary self-test deleted afterwards.

**Noise and generators** (no GL):

- **Tiling:** for every generator and filter, value at `x` equals value at
  `x + period` (and likewise in y) — exact for noise, within float epsilon
  after Blur/Warp.
- **Perlin:** exactly 0 at integer lattice points; sample mean ≈ 0; range
  within the analytic bound ±√(N/4) (N = 2 → ±0.707). fBm variance ratio to
  a single octave matches Σ g²ⁱ.
- **Voronoi:** F1 = 0 at a feature point; F2−F1 ≥ 0 everywhere; with jitter
  0, F1 matches the closed-form distance to the nearest cell centre.
- **Pattern:** mortar pixel fraction matches the hand-computed area fraction
  from brick size and mortar width.

**Graph:**

- Topological order correct on a hand-built diamond; a cycle is rejected.
- Dirty propagation: editing a downstream node leaves upstream eval
  counters unchanged.
- Blend modes against hand-computed single-pixel values; Levels with
  gamma 1 is the linear remap, gamma γ against `pow`.
- **Height → Normal:** on `h = sin(2πx/P)`, the normal's x-component matches
  the analytic derivative `−k·(2π/P)·cos(2πx/P)` (normalised) — not the
  implementation's own finite difference.
- Serializer: save → load → save byte-identical text; evaluation before and
  after the round-trip byte-identical images.
- Determinism: two evaluations, and two process runs, give the same PNG hash.

**Scene link:**

- A scene with no `MaterialPath` anywhere: byte-identical `--hide-ui` capture
  before and after the shader change; Cube3D likewise.
- An entity linked to a solid-colour graph: its centre pixel read with
  `Framebuffer::ReadPixelRGBA` matches the hand-computed lit colour.
- An old scene file loads with `MaterialPath` empty.

**imnodes spike:** plan task 1 — compile imnodes against the vendored ImGui
in Debug, Release and Dist and render one node, before anything depends on
it.

**UI:** the same standing gap as the other editor panels — no GUI-automation
tool exists here. Graph editing is verified through the model and the panel
through a rendered capture, not a live click-through; the changelog says so.

All three build configs, every time.
