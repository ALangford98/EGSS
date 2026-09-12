# Entity grouping — design

## Goal

Let the editor treat several entities as one unit for organization and
movement: a named group shown together in the Outliner, a multi-selection in
the viewport (Ctrl+click) that drags every selected entity from one shared
gizmo, and a keyboard shortcut (Ctrl+G) that turns a multi-selection into a
persisted group. A right-click "Add to Group" opens a dialog that manages
every group in the scene, not just the entity that was clicked.

## Why this shape

- **A group is a tag, not an object.** The data model decision made up
  front: `GroupComponent{ std::string GroupName }` on each member, no
  separate group entity or registry. This means **a group's existence is
  entirely defined by who currently carries its name** — there is no way to
  have an "empty" group with zero members, because nothing outside the
  members themselves would remember it exists. Every flow below that creates
  a group (Ctrl+G, the dialog's "new group" field) therefore always creates
  it *with* at least one member in the same action, never in two steps.
  Consequence taken deliberately, not discovered late: it keeps Save/Load,
  Duplicate, and Delete exactly as mechanical as every other component this
  editor already has (one optional string, same shape as `ScriptComponent`'s
  path or the mesh's `SourcePath`), instead of adding a second serialized
  collection (a group registry) that could disagree with what the components
  actually say.
- **Single membership, not a set.** One `std::string` per entity means an
  entity is in at most one group. This is what makes the "already belongs to
  a group" confirmation in the Ctrl+G flow meaningful — joining a new group
  is a move, not an addition, so it can silently overwrite something the
  user forgot about unless asked first.
- **Multi-selection is session state, groups are persisted state, and they
  are deliberately not the same thing.** Ctrl+click builds a working set to
  act on right now (drag together, or promote to a group); nothing about
  selecting several entities changes the scene file. Only Ctrl+G, or the
  management dialog, writes a `GroupComponent`. This is explicit in the
  request ("This shouldn't automatically group them") and it's also what
  keeps the multi-select gizmo simple: it never has to ask "is this
  temporary selection secretly a group," because it never is.
- **The gizmo already separates "where it's drawn" from "what it writes"
  once** — mesh authoring's `m_MeshEditSessionWorldPoint` is a stand-in
  world position that `GizmoPosition()` returns instead of a transform's
  `Position` directly, kept in sync before/after each drag. Multi-select
  reuses exactly that shape: a centroid stand-in position, with the actual
  per-entity writes happening as a side effect of the stand-in moving,
  instead of a second gizmo implementation.

## Scope

**In scope:**
- `GroupComponent`, serialized with the scene, carried through Duplicate and
  Delete like every other component.
- Outliner rows grouped under a collapsible header per `GroupName`;
  ungrouped entities render exactly as they do today.
- Ctrl+click in the viewport *or* the Outliner adds/removes an entity from a
  transient multi-selection; a plain click clears it.
- A translate-only gizmo at the multi-selection's centroid, dragging every
  selected entity's `TransformComponent::Position` by the same world-space
  delta, undoing as one step.
- Ctrl+G: with 2+ entities multi-selected, prompts for a group name (a
  confirmation first if any already belong to a group) and writes
  `GroupComponent` to all of them.
- "Add to Group" in the entity context menu opens a modal that manages every
  group in the scene: create (name + the invoking entity), rename, delete,
  and add/remove any entity's membership.

**Explicitly out of scope / deferred:**
- **Nested groups.** A group can't contain another group — falls out of the
  single-string data model; nothing in the request asked for it.
- **Rotate/scale as a rigid group.** The multi-select gizmo only translates
  (per your answer) — rotating or scaling several entities about a shared
  pivot needs a per-member relative-transform recompute, a materially
  bigger piece of math than a shared position delta.
- **Multi-entity Inspector editing.** The Inspector keeps editing exactly
  one entity (`m_Selected`); the gizmo is the only thing that acts on the
  whole multi-selection.
- **Drag-and-drop in the Outliner** (dragging a row into/out of a group
  header). Group membership changes only through Ctrl+G or the dialog.
- **A group's own transform/pivot as persisted state.** The centroid is
  recomputed from members every frame it's needed, never stored.

## Design

### `GroupComponent`

```cpp
// Components.h
struct GroupComponent
{
    std::string GroupName;
};
```

Serialized like `ScriptComponent`'s path: `Scene::Save` writes a `group
<name>` line only when `GroupName` is non-empty (an entity with an empty
name isn't meaningfully grouped, same reasoning the mesh/script blocks
already use for their own optional strings); `Scene::Load` adds it back via
`current.Add<GroupComponent>(...)`.

`DeleteEntityCommand` and `DuplicateEntityCommand` (`EditorHistory.h`) each
gain an `std::optional<GS::GroupComponent> m_Group`, captured/restored
exactly where `m_Physics` already is — mechanical, not new logic. A
duplicated entity keeps its source's group; nothing about "Copy" implies
leaving the group behind.

### Outliner: grouped display

Today's loop is one flat `for (EntityId : GetEntities())` calling
`ImGui::Selectable` per row. This becomes two passes over the same
`GetEntities()` call (still read-only, so no new iteration-safety concern
beyond what already exists for Duplicate/Delete):

1. Build `std::map<std::string, std::vector<EntityId>>` by scanning each
   entity's `GroupComponent` (skipping entities with none). `std::map`
   rather than `unordered_map` — alphabetical group order is a reasonable
   free default and it costs nothing at editor-scene sizes.
2. Render: for each group, `ImGui::TreeNode(groupName.c_str())`; inside,
   the existing per-row `Selectable` + `DrawEntityContextMenu` block,
   unchanged, for each member. After all groups, entities with no
   `GroupComponent` render exactly as today, at the top level.

A group header itself gets a small context menu (right-click the
`TreeNode`): "Select All" (populates `m_MultiSelection` with every member —
see below; this is what makes an existing group draggable as a unit without
re-Ctrl+clicking every member) and "Ungroup" (clears `GroupComponent` from
every member) — the same no-confirmation rule as the dialog's own "Delete
Group" below, for the same reason: both are explicit, deliberate actions a
user reached for on purpose, not a fast shortcut that could fire by
accident.

### Multi-selection

```cpp
std::vector<GS::EntityId> m_MultiSelection;
```

- **Plain click** (Outliner row or viewport, existing code paths): clears
  `m_MultiSelection`, sets `m_Selected` as today. No behavior change when
  nothing is ever Ctrl+clicked — this is why the common single-selection
  path stays exactly as fast and simple as it is now.
- **Ctrl+click**: toggle `entity` in `m_MultiSelection` (erase if present,
  push_back if not); set `m_Selected` to it either way, so the Inspector
  always reflects the most recently touched entity. Checked with the same
  `GS_KEY_LEFT_CONTROL`/`GS_KEY_RIGHT_CONTROL` pair `EditorMenuBar.h`
  already polls for its own Ctrl+F.
- **Validity**: at the top of `OnImGuiRender`, erase any now-invalid entity
  from `m_MultiSelection` (deleted since last frame) — the same defensive
  shape `ValidateMeshEditSession` already applies to `m_MeshEditEntity`.
- **Highlighting**: `DrawSelectionBox`'s existing two-pass loop
  (hovered/selected) grows a third pass over `m_MultiSelection` when it has
  2+ entries (1 or 0 entries: identical to today, drawing only through
  `m_Selected`/`m_Hovered` as now).

### Multi-select gizmo

`GizmoPosition()` grows one more case, checked before the existing
single-entity one:

```cpp
if (m_MultiSelection.size() > 1)
    return &m_MultiSelectionPivot;
```

`m_MultiSelectionPivot` is a plain `glm::vec3`, recomputed as the mean of
every member's `TransformComponent::Position` whenever the selection changes
and once more right before a drag begins (mirroring
`SyncMeshEditWorldPoint{Before,After}Gizmo`'s naming and placement) — the
drag math in `UpdateGizmo` (`*target = m_DragStartPosition + axisDirection *
(t - m_DragStartT)`) is completely unchanged, because it only ever writes
through whatever pointer `GizmoPosition()` handed it.

What differs is what happens *after* that write. When the drag target is
the pivot, a per-member start snapshot taken at drag-start
(`std::vector<std::pair<EntityId, glm::vec3>> m_MultiSelectionStartPositions`,
captured the same frame `m_DragStartPosition` is) lets every frame compute
one shared delta (`*target - m_DragStartPosition`) and apply it: `member's
Position = capturedStart + delta`. The pivot moves once; every member moves
by the same amount, so relative spacing within the group never changes —
exactly "dragged with a single gizmo," not "everything snaps to the pivot."

**Undo**: a new `MultiMoveCommand` (`EditorHistory.h`), parallel to
`EditFieldCommand<TransformComponent, glm::vec3>` but over several entities
at once — constructed with the same before/after position pairs
`UpdateGizmo`'s existing "drag just ended" branch already assembles for the
single-entity case, so `Undo`/`Redo` apply all of them together and one
`Ctrl+Z` reverses the whole gesture, not one member at a time.

### Ctrl+G

Polled in `OnUpdate`/`OnEvent` the same way `EditorMenuBar.h` polls Ctrl+F:
`GS_KEY_G` pressed while Ctrl is held, with `m_MultiSelection.size() >= 2`
(otherwise a no-op — nothing to group).

1. If any selected entity already has a non-empty `GroupComponent::GroupName`
   (whether the same group or several different ones), open a confirm
   dialog first: *"N of these are already in a group — move them?"* Cancel
   aborts the whole Ctrl+G with nothing changed.
2. On confirm (or immediately, if nothing needed confirming): a small popup
   for the new group's name, seeded empty, `ImGuiInputTextFlags_EnterReturnsTrue`
   the same as the rename field elsewhere. On submit, every entity in
   `m_MultiSelection` gets `AddComponent<GroupComponent>({name})` (overwriting
   any existing one).

Not pushed onto `EditorHistory` — same precedent as "Add Script"/"Add
Physics" today, neither of which is undoable either; group assignment joins
that existing, already-accepted category of un-undoable structural edits
rather than introducing undo for only some component adds.

### The "Manage Groups" dialog

Opened via a new "Add to Group" entry in `DrawEntityContextMenu`, which
remembers which entity invoked it (`m_GroupDialogEntity`) but the dialog
itself lists and acts on **every** group, per "full management":

- Built by the same group-name scan the Outliner uses (called fresh each
  time the dialog is open — cheap, and it means the dialog can never show a
  group that Save/Load or another session action just emptied out).
- One `CollapsingHeader` per existing group:
  - Rename (`InputText`, seed-on-appear, `EnterReturnsTrue` → rewrites
    `GroupName` on every current member — same field-edit shape used
    everywhere else in this editor, just applied N times instead of once).
  - "Delete Group" — clears `GroupComponent` from every member.
  - Each member listed with its own "Remove from Group" button.
  - A combo of every entity *not* currently in this group (grouped or not),
    with an "Add" button — moves it in, silently overwriting any previous
    group exactly like Ctrl+G's step 2 does. No confirmation here: this
    dialog is already an explicit, deliberate management action, not a fast
    shortcut that could fire by accident, which is the distinction that
    earns Ctrl+G its confirm step and this one doesn't need it.
- A "New Group" text field + "Create" button, pre-filled with nothing,
  always paired with the invoking `m_GroupDialogEntity` as its first (and,
  until more are added via the combo above, only) member — the same "never
  empty" rule Ctrl+G follows, for the same reason. Like every other action
  in this dialog, this silently overwrites `m_GroupDialogEntity`'s existing
  group membership if it had one — no confirmation, per the no-confirmation
  rule above.

## Testing

Temporary self-tests (deleted once they've done their job), all directly
checkable against the ECS without a live ImGui frame — the same shape used
to verify `DuplicateEntityCommand` and `PhysicsComponent`:

- `GroupComponent` Save/Load round trip: an entity with a group name
  survives a Save + Clear + Load with the exact string intact.
- `DuplicateEntityCommand`/`DeleteEntityCommand` carry `GroupComponent`
  through Redo/Undo, same assertions already used for `PhysicsComponent`.
- `MultiMoveCommand`: move N entities by a delta, assert every position
  updated; `Undo` puts every one back to its exact captured start; `Redo`
  reapplies the same delta to all of them.
- Centroid math: a hand-picked set of positions checked against
  `glm::vec3` mean by hand, not by re-deriving the same averaging code the
  implementation uses.

Interactive pieces (Ctrl+click, Ctrl+G's popups, the management dialog,
actual mouse drag-through) get the same disclosed, standing gap every other
editor UI feature here already has — no GUI-automation tool exists in this
environment. Where useful, a temporary hook forces a dialog open for a
visual capture the way mesh authoring's overlay was checked, but the live
click-through itself stays unverified.
