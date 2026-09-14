# Mesh UV template export — design

Roadmap item #9 (`editor_roadmap.md`): "Mesh texture map exports (UV
unwrapping to a Photoshop-editable template)." The roadmap entry itself
flagged an open fork — primitives-only, where the existing `Create*Data`
UV layouts are trivial to template, or arbitrary meshes too, which needs
real seam-finding and chart packing. Scoped in brainstorming to the harder
option: arbitrary meshes, via a heuristic (not conformal/LSCM) unwrap.

## Goal

Given any placed mesh, produce a flat, chart-packed UV template image a
designer can paint over in Photoshop, and — when the mesh doesn't already
have a usable UV layout — compute and save real UVs into the mesh so the
template actually matches what gets textured. Not a general unwrapping
tool: one button, one deterministic pass, no manual seam authoring, no
preview/iterate loop.

## Why this shape

- **Conformal parameterization (LSCM-style) was ruled out.** It minimizes
  distortion well, but needs a sparse least-squares solver this engine has
  no linear-algebra foundation for (GLM only gives dense small vectors/
  matrices) — a research-grade sub-project on its own, not what "export a
  paintable template" needs to be useful. A heuristic projection unwrap
  (chart the mesh, flatten each chart by planar projection, pack the
  charts) gets real texture space on arbitrary geometry with a tractable
  amount of code, at the cost of visible distortion on strongly curved
  surfaces — an accepted, disclosed trade, not an oversight.
- **Charting needs mesh adjacency, which `MeshData` doesn't carry, but is
  cheap to derive.** `MeshData` is a flat vertex/index buffer — no
  neighbor-triangle information survives from whichever loader or
  primitive builder produced it. Two triangles are adjacent when they
  share an edge, and an edge is identified by its two endpoint
  *positions* (not vertex indices — flat-shaded meshes duplicate vertices
  per face for distinct normals, so index equality would miss real
  adjacency the way `EditableMesh`'s own welding already had to work
  around). A position-keyed edge→triangle map, built once, is all
  charting needs.
- **Fixed 6-direction box projection was considered and rejected as the
  primary heuristic.** It needs no adjacency graph at all, but forces
  every seam onto a cardinal-axis boundary regardless of the mesh's real
  shape — a sphere unwraps like a papercraft cube. Adjacency-driven chart
  growing (seed a chart, admit a neighbor while its normal stays within an
  angle threshold of the chart's running average) costs one more data
  structure and adapts to the mesh's actual curvature instead, which is
  the entire point of choosing "arbitrary meshes" as the target in the
  first place.
- **"Usable UVs" detection reuses a gap this project already knows
  about.** Mesh authoring's `EditableMesh::Rebuild()` hardcodes UV to
  `(0,0)` for every point (a disclosed, deferred item from that spec) —
  so "all UVs identical / near-zero bounding-box area" is not a
  hypothetical detection case, it's the actual current output of that
  feature. `HasUsableUVs` treats that mesh the same as one that was never
  given UVs at all, and both fall through to a fresh unwrap.
- **No new vendored dependency.** `stb_image_write` is already vendored
  (`GS/vendor/stb_image/stb_image_write.cpp`) and already used by
  `ScreenCapture.cpp` for PNG output — the template writer reuses it
  exactly, not a second image-writing path.
- **The reload-without-`MeshCache` pattern is copied from mesh
  authoring's own Done handler, not reinvented.** `MeshCache::Get(path)`
  caches one mesh per path for the process lifetime with no invalidation;
  re-exporting the same entity's `.obj` a second time would otherwise
  silently show the *first* export's stale cached geometry even though
  the file on disk is correct. Mesh authoring's Done already solved this
  by constructing `std::make_shared<GS::Mesh>(finalData, name)` directly
  from the data just written, bypassing the cache — this feature does the
  same when it rewrites UVs.

## Scope

**In scope:**
- Chart-growing unwrap for a mesh with no usable UVs (`GS::UvUnwrap`).
- UV-island detection from an *existing* UV layout, for the export path
  when a mesh's own UVs are kept as-is (needed so the template writer has
  chart boundaries to tint/outline regardless of which path filled the
  UVs).
- A wireframe + per-chart-tint template PNG at a user-chosen resolution
  (`GS::UvTemplateWriter`).
- One Inspector entry point: "Export UV Template" next to "Edit Mesh",
  plus a resolution field.
- Re-saving the mesh's `.obj` (via the existing `ObjWriter`) when UVs were
  freshly computed, so the mesh's own geometry file and the exported
  template stay in sync.

**Explicitly out of scope:**
- **Manual seam editing.** Chart boundaries are whatever the angle
  threshold produces; there's no UI to mark/clear a seam by hand.
- **Chart relaxation / distortion-minimization pass.** The projection is
  a single planar flatten per chart, not iteratively improved.
- **Interactive preview before export.** This is compute-then-write, not
  an editing session the way "Edit Mesh" is — no live overlay, no undo/
  redo, nothing to cancel out of once resolution is chosen.
- **Re-importing a painted texture.** This feature only produces the
  template; wiring a finished texture back onto the mesh's material is a
  separate, later concern (the same way mesh authoring left material
  editing untouched).
- **Non-primitive-derived, very dense meshes at real-time speed.** The
  chart-growing flood fill and the O(charts²) rectangle-packing step are
  sized like mesh authoring's own coplanar-face grouping — fine for the
  meshes this editor actually deals with, not profiled against a
  hundred-thousand-triangle import.

## Design

### `GS::UvUnwrap` (`GS/src/GS/Renderer/UvUnwrap.h/.cpp`)

Pure geometry over `MeshData`, no GL, no ImGui — same separation
`EditableMesh` and `ObjWriter` already keep.

```cpp
namespace GS {
    // All-zero (or near-zero UV-bounding-box area) UVs -- the output of
    // a mesh that was never textured, or of EditableMesh::Rebuild()'s
    // known (0,0)-everywhere gap. Either way, not usable as-is.
    bool HasUsableUVs(const MeshData& data);

    // Charts an unwrapped or already-UV'd mesh into an id per triangle.
    // Returned in both cases below so UvTemplateWriter never needs to
    // know which path produced its input.
    using ChartAssignment = std::vector<int>;  // one entry per triangle (Indices.size()/3)

    // Builds edge adjacency (position-keyed), greedily grows charts
    // (seed an unvisited triangle, BFS over adjacency, admit a neighbor
    // while its normal is within kChartAngleThresholdDegrees of the
    // chart's running average normal), fits an orthonormal basis per
    // chart from its final average normal, projects every member
    // vertex into that chart's local 2D space, and packs all charts'
    // 2D AABBs into [0,1]^2 (shelf packing, small fixed margin between
    // charts) -- writing the result into MeshVertex::TexCoord.
    ChartAssignment Unwrap(MeshData& data);

    // Read-only counterpart for a mesh whose existing UVs are kept:
    // two triangles are in the same island when they share a mesh edge
    // AND their corresponding UV-space edge endpoints match (the
    // standard "find UV islands" check). No geometry decisions, no
    // mutation -- just tells UvTemplateWriter where the existing seams
    // already are.
    ChartAssignment FindIslandsFromUVs(const MeshData& data);
}
```

`kChartAngleThresholdDegrees` is a single tuned constant, not
user-exposed (no slider — YAGNI for a first version). Picked empirically
during testing (see below) against `CreateCubeData()` and
`CreateSphereData()`: steep enough that a cube's 90°-apart faces always
separate into 6 distinct charts, shallow enough that a coarsely-tessellated
curved primitive still groups into a handful of charts rather than one
per triangle — verified against those two shapes' actual chart counts,
not asserted from an external tool's default.

**Packing.** Chart 2D footprints come out of the planar projection in the
mesh's own world units, which can vary wildly in scale between meshes (or
even between charts of a lopsided mesh). Packing operates in that native
unit space using a shelf packer (charts sorted tallest-first, placed
left-to-right in growing rows, a fixed margin between neighbors) with an
initial shelf width of `sqrt(total chart area)` — a standard, cheap
starting estimate for a roughly-square pack, not tuned further. The
packed result's overall bounding box is then uniformly scaled and
translated once into `[0,1]²`, preserving aspect ratio (letterboxed
inside the unit square if the pack isn't itself square) — this is the
only normalization step; nothing during packing itself assumes a
particular unit scale.

### `GS::UvTemplateWriter` (`GS/src/GS/Renderer/UvTemplateWriter.h/.cpp`)

```cpp
namespace GS {
    // Rasterizes `data`'s (already [0,1]-ranged) UVs at resolution x
    // resolution: each triangle filled with a pale tint keyed by
    // chartId[i] % kPaletteSize (a small fixed pastel palette, cycled --
    // not computed from the mesh), plus a wireframe outline over every
    // triangle edge. Transparent (alpha 0) outside any triangle.
    bool Write(const std::string& path, int resolution, const MeshData& data,
               const std::vector<int>& chartIdPerTriangle, std::string& error);
}
```

Rasterization is a plain edge-function/barycentric triangle fill into an
in-memory RGBA buffer (`std::vector<uint8_t>`, 4 bytes/pixel), written
out via `stbi_write_png(path, resolution, resolution, 4, buffer.data(), resolution*4)`
— the exact call shape `ScreenCapture.cpp` already uses. No new image
library, no new file format.

### Editor wiring (`EditorSceneView.h`)

`StartMeshEdit`'s existing primitive-vs-`SourcePath` branch (lines
~951–969: `primitive:cube`/`plane`/`sphere`/`cylinder` → the matching
`Create*Data()`, anything else → `Mesh::LoadData`) is lifted into a small
shared free function, `LoadMeshDataForExport(MeshComponent* mesh,
MeshData& outData, std::string& error)`, used by both `StartMeshEdit` and
the new button — the same branching logic, not duplicated a second time.

A new Inspector button, "Export UV Template", next to "Edit Mesh" (same
guards: hidden/disabled with no mesh, an invalid one, or while
`PlayMode::IsPlaying()`), plus an `InputInt` for resolution (default
`1024`, stored per-session in the panel, not persisted). On click:

1. `LoadMeshDataForExport` — bail (logged, same `GS_ERROR` pattern as
   "Edit Mesh") on failure or an empty mesh.
2. `HasUsableUVs(data)` — if false: `auto chartIds = Unwrap(data);` then
   `ObjWriter::Save` to the same collision-suffixed `assets/<name>.obj`
   path Done already computes, update `mesh->SourcePath` and rebuild
   `mesh->Geometry` directly from `data` (bypassing `MeshCache`, exactly
   as Done does) — a real `EditFieldCommand` undo entry pushed the same
   way, since this can silently change the mesh's geometry file on disk.
   If false but the UVs are used as-is: `auto chartIds = FindIslandsFromUVs(data);`
   with no file/UV mutation at all.
3. `UvTemplateWriter::Write("assets/<name>_uv.png", resolution, data, chartIds, error)` —
   logged on failure, same as every other save-failure path in this file.

### Error handling

- Degenerate/collinear triangles during chart-normal or basis
  computation are guarded the same way `RecalculateNormalsTouching`'s
  existing NaN-guard handles a degenerate cross product elsewhere in this
  codebase — skipped/zeroed rather than propagated into a NaN UV.
- `ObjWriter::Save` and `UvTemplateWriter::Write` failures are logged
  (`GS_ERROR`) and leave the entity/file state exactly as it was before
  the click — no partial write, no closed session to silently lose
  (there is no session here to close).
- The button itself is disabled under the same conditions "Edit Mesh"
  already disables under — a mesh that failed to load has no button to
  press, not a button that fails when pressed.

## Explicitly deferred

Manual seam editing; chart relaxation/distortion minimization; an
interactive preview/undo loop for this feature specifically; texture
re-import; performance work for very dense meshes.

## Testing

Temporary self-tests (deleted once they've done their job), directly
checkable without a live ImGui frame:

- `HasUsableUVs`: false for an all-`(0,0)` mesh (the `Rebuild()` case)
  and for a mesh with no UV variance at all; true for a mesh with a real,
  varied UV layout.
- `Unwrap(CreateCubeData())`: exactly 6 charts (the angle threshold
  correctly separates every 90°-apart cube face), every packed UV inside
  `[0,1]²`, no two charts' packed AABBs overlapping (checked pairwise).
- `Unwrap` on a single flat quad (2 coplanar triangles): exactly 1 chart.
- `Unwrap(CreateSphereData())`/`Unwrap(CreateCylinderData())`: sanity
  numbers for the tuned angle threshold — chart count in the "a handful,"
  not "one per triangle" or "one for the whole sphere," range; this is
  the empirical check the threshold constant is picked from, reported
  with the actual counts, not assumed to land right.
- `FindIslandsFromUVs`: given a hand-built `MeshData` with two known,
  UV-disjoint triangle pairs, correctly reports 2 islands; given a mesh
  whose UVs are actually one continuous chart, reports 1.
- `UvTemplateWriter::Write`: produces a file of the exact requested
  resolution (checked via `stbi_image`'s own read-back, already vendored
  alongside the writer) with visibly non-transparent pixels only inside
  the expected triangle footprints for a small hand-placed test triangle.
- Visual confirmation: a real capture of an exported template PNG for a
  non-trivial mesh (not just a primitive), opened and checked for
  visually distinct, non-overlapping tinted charts with a wireframe
  overlay — the actual button click-through stays the disclosed,
  unautomatable gap every other UI feature in this editor already has.
