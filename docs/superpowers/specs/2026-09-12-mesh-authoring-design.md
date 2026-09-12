# Mesh authoring — design

The sub-project deferred, unnamed beyond one line, since the original
editor-scene-composition spec (2026-09-07): "Custom mesh authoring. Defining
a *new* mesh from scratch inside the editor... this sub-project only places
meshes that already exist (imported or preset)." Nothing further was ever
designed. This spec is that design.

## Goal

Let a user reshape a mesh already placed in the scene — move vertices,
extrude a face, delete a vertex or face, split an edge to add a vertex — from
inside the editor, and save the result as a real, reusable mesh file. Not a
general-purpose modeler: the four operations above, on a mesh that starts
from something already in the scene (a preset primitive or an imported file),
never from nothing.

## Why this shape

Three real constraints, checked rather than assumed, shaped every decision
below:

- **The engine's mesh format has no memory of polygon structure.**
  `MeshData`/`Mesh` store only a flat triangle-index buffer (`Submesh` names
  a range into it); `Mesh::CreateCube()` builds 24 vertices, not 8 — a cube
  corner needs a different vertex per face so each can carry that face's own
  normal. Editing that duplicated buffer directly would let a user move one
  corner of one face and leave the other two faces at that corner behind,
  cracking the mesh open. Editing needs a *welded* representation; producing
  final geometry needs to re-split it back into per-face vertices the same
  way the primitives already do.
- **No polygon (face) structure survives past triangulation either**, and
  general triangle-soup-to-polygon reconstruction is a hard problem this
  project has no reason to solve generally. The one thing that *is* true and
  cheap to check: two triangles that share an edge and have matching
  (coplanar) normals almost certainly came from the same authored face — a
  cube's two triangles per side always do. Grouping by that rule recovers a
  primitive's real faces without needing to know how the mesh was built,
  and degrades gracefully (a triangle with no coplanar neighbor is just its
  own one-triangle face) rather than failing on geometry it can't group.
- **The existing translate gizmo already does exactly the drag math this
  needs.** `EditorSceneView::GizmoPosition()` returns a single `glm::vec3*`
  that `UpdateGizmo`/`DrawGizmo` drag along a ray-axis projection; retargeting
  that pointer at a selected vertex (or, for extrude, a small group sharing
  one delta) reuses proven code instead of a second gizmo implementation.

## Scope

**In scope:** entering an edit session on an already-placed mesh (preset or
imported); move/extrude/delete/split on its welded, faceted topology; a live
GPU-side preview rebuilt after every change; exporting the result as a real
`.obj` (a new, small writer — none exists today, only a loader) that becomes
the entity's new `MeshComponent::SourcePath`, going through `MeshCache::Get`
like any other mesh; a local undo/redo stack for the editing session,
collapsing to one scene-level undo command on exit.

**Explicitly out of scope:**
- **Starting from nothing.** Every session starts from a real mesh already
  in the scene. Building a mesh up from zero vertices needs vertex-creation
  tools this doesn't have and wasn't asked for.
- **Multi-select / box-select.** One control point (or, mid-extrude, the one
  group the extrude just created) is ever being dragged at a time.
- **UV editing.** A rebuilt face's UVs are carried forward mechanically from
  whichever original vertex a control point came from; laying out new UVs
  for extruded/split geometry is a real, separate feature.
- **Editing dense, non-primitive-derived meshes** (an imported high-poly
  glTF character, say). Nothing prevents opening one, but the coplanar-face
  grouping and CPU-side picking below are sized for a primitive's tens of
  vertices, not tens of thousands — a future perf pass is a separate concern
  if this ever gets pointed at something that large.
- **A dedicated mesh-edit camera or viewport.** Editing happens in
  `EditorSceneView`'s existing viewport and free-fly camera, the same way
  Play mode reuses it rather than opening a second one.

## Design

### `EditableMesh` — the welded, faceted topology

A new, editor-only, rendering-agnostic data structure (`EditableMesh.h`,
mirroring `TextBuffer.h`'s own "pure data and operations, no ImGui" shape):

```cpp
struct EditPoint
{
    glm::vec3 Position;   // object (local) space -- the entity's own Transform, not world space
};

struct EditFace
{
    std::vector<int> Points;   // indices into EditableMesh::Points, winding order
    glm::vec3 Normal;          // recomputed whenever the face's points move
};

class EditableMesh
{
public:
    static EditableMesh FromMeshData(const GS::MeshData& source);
    GS::MeshData Rebuild() const;   // re-splits per face, for both live preview and final export

    // Selection
    int PickPoint(...) const;   // nearest projected point within a pixel radius
    int PickFace(...) const;    // ray-triangle test against the face's own triangles

    // Operations
    void MovePoint(int point, const glm::vec3& newPosition);
    void ExtrudeFace(int face, std::vector<int>& outNewPoints);
    void DeletePoint(int point);
    void DeleteFace(int face);
    void SplitEdge(int pointA, int pointB, int& outNewPoint);

    bool CanDelete(int point_or_face) const;   // false if it would leave zero faces

    // Undo/redo -- same snapshot-per-operation shape TextBuffer's own
    // undo stack already uses: this data is small, and "copy the whole
    // thing" cannot itself have a replay bug.
    void PushUndo();
    void Undo();
    void Redo();
private:
    std::vector<EditPoint> m_Points;
    std::vector<EditFace> m_Faces;
    // ...undo/redo stacks, same shape as TextBuffer's.
};
```

### The four operations

- **Move**: reposition one `EditPoint`; recompute the normal of every
  `EditFace` that uses it.
- **Extrude**: duplicate the selected face's points into new, initially
  coincident `EditPoint`s; build one new "wall" `EditFace` per edge of the
  original face, connecting each original point to its duplicate; replace
  the original face's point list with the duplicates (so the cap of the
  extrusion is the new geometry, not the old). The caller then group-drags
  the new points as described under "Entry / exit flow" below.
- **Delete point**: remove every `EditFace` that references it, *then* the
  point itself. This is a cascade, not a special case: a triangle
  necessarily disappears entirely when any one of its three points is
  deleted, because a 2-point "face" isn't valid.
- **Delete face**: remove just that face. If doing so leaves any of its
  points touching zero remaining faces, drop those points too — an orphan
  with no face to belong to is a dead end for every operation this spec
  defines (nothing here builds a new face from loose points).
- **Split edge**: given two points that share at least one face, insert a
  new point at their midpoint into every face that contains both of them
  (in a manifold mesh, one or two), at the correct position in each face's
  winding order.
- **`CanDelete`**: refuses (returns `false`, and the caller does nothing) a
  point or face deletion that would leave zero faces afterward, rather than
  producing an unbuildable, empty mesh.

**Welding** (`FromMeshData`): group `source.Vertices` by position (within a
small epsilon — GPU-authored primitives place duplicates at *exactly* the
same float position, so this can be an exact-match hash, not a fuzzy one)
into `EditPoint`s, remembering which original vertex indices each one
replaces.

**Face grouping**: for each triangle in `source.Indices`, look at its
neighbors (triangles sharing an edge, found via the welded point indices);
merge into one `EditFace` when normals match within a tight angular epsilon.
This is a flood-fill over the triangle-adjacency graph, not a per-primitive
special case — it recovers a cube's 6 quads from 12 triangles, an imported
mesh's real faces from whatever coplanar triangle pairs it has, and simply
leaves a triangle that has no coplanar neighbor as its own one-triangle face
rather than failing.

**Rebuild**: the inverse of welding — walk `m_Faces`, and for each one emit
fresh, non-shared vertices (position from the `EditPoint`, normal from the
`EditFace`, UV carried forward from whichever original source vertex a point
first came from), fan-triangulating any face with more than 3 points. This
is what both the live preview (called after every edit) and the final
`.obj` export (called once, on Done) run.

A point created by extrude or split has no original source vertex to carry
a UV forward from — it inherits the UV of the point it was created *from*
(the face-corner it was extruded from; whichever endpoint of the split edge
comes first in that face's winding order). Not correct UV layout — nothing
here produces one — just a defined, non-crashing value consistent with UV
editing being explicitly out of scope.

### Getting `MeshData` back out of `Mesh::Create*`/`Mesh::Load`

Both only ever return a finished GPU `Mesh*` today, with no `MeshData` kept
around afterward. Splitting each into a `MeshData`-returning half and a
thin `Mesh*`-wrapping half that calls it (`Mesh::CreateCubeData(...)` +
`Mesh::CreateCube(...)` calling it, same pattern for `CreatePlane`/
`CreateSphere`/`CreateCylinder`/`Load`) is a small, mechanical adaptation to
existing code — not new scaffolding, and it costs the existing callers
nothing (the wrapping half's signature is unchanged).

### Entry / exit flow

1. Inspector: a selected entity with a valid `MeshComponent` gets an **"Edit
   Mesh"** button (hidden/disabled with no mesh, an invalid one, or while
   `PlayMode::IsPlaying()`). Clicking it gets a `MeshData` for the entity's
   current mesh (the split functions above, keyed off `SourcePath`) and
   builds an `EditableMesh` from it.
2. While a session is open, `EditorSceneView`'s picking and gizmo retarget
   for *that entity only*: a click picks a point/edge/face (converting the
   click ray and every point's position through the entity's own Transform,
   since `EditableMesh` stores object-space positions but picking/dragging
   happen in world space) instead of picking a whole entity; a small overlay
   (reusing `Renderer2D::DrawLine`, the same mechanism `DrawCameraRays`/
   `DrawLightGizmos` already use) marks every point so there's something to
   click before anything is selected. Entity selection elsewhere in the
   editor is locked until the session ends — no silent loss of in-progress
   edits from clicking away.
3. `GizmoPosition()` grows a group form: dragging one selected point moves
   just it; an extrude's freshly-created points move together by the same
   delta. The underlying ray-axis math is unchanged either way.
4. Every operation calls `EditableMesh::Rebuild()` and feeds the result into
   a live-updated preview `Mesh`, so the shape visibly changes as you work.
5. **Done**: `Rebuild()` once more, write it via the new `ObjWriter` to a
   real `.obj` next to the project (name derived from the entity's tag —
   see the Inspector's own ID field — with a numeric suffix on collision),
   point `MeshComponent::SourcePath`/`Geometry` at it through the normal
   `MeshCache::Get` path, and push one `EditFieldCommand`-shaped scene-level
   undo entry for the whole session. **Cancel**: discard the `EditableMesh`,
   touch nothing.

### `ObjWriter`

A small, real exporter — `v`/`vn`/`f` lines from a `MeshData`, mirroring
`ObjLoader`'s own format understanding in reverse. No materials file is
written (nothing here edits materials); a rebuilt mesh keeps flat vertex
colors or whatever it already had via `MeshComponent::Color`, unaffected by
this feature.

## Explicitly deferred

Starting from nothing (no existing mesh); multi-select; UV editing; a
dedicated mesh-edit viewport/camera; performance work for dense,
non-primitive meshes; material/UV-aware export.

## Testing

Temporary self-tests (deleted once they've done their job, same as every
other feature in this project), all directly checkable without a live ImGui
frame:

- Welding a known `MeshData` (start from `Mesh::CreateCube()`'s own 24-vertex
  data) collapses to exactly 8 `EditPoint`s.
- Face grouping on that same cube data recovers exactly 6 `EditFace`s, each
  with 4 points.
- Move/extrude/delete/split each checked against hand-derived before/after
  point and face counts and positions, per their definitions above.
- `CanDelete` correctly refuses an operation that would leave zero faces.
- `ObjWriter` round-trip: write a small known mesh, read it back via the
  real, existing `ObjLoader`, and confirm vertex count, triangle count, and
  positions match.
- The edit-mode UI (overlay, picking, gizmo retarget) confirmed with a
  visual capture, forced open via a temporary hook, the same way every other
  UI feature in this editor has been checked this session. The live mouse
  click-through itself stays the disclosed, unautomatable gap every other UI
  feature here already has — no GUI-automation tool exists in this
  environment.
