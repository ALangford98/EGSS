# File tree panel — brief spec

First of four sub-projects (file tree, entity/component scripting, recreate
one demo, text-editor refinements) working toward: recreate any existing
demo inside the editor, each demo deconstructed into its own project so
its parts are independently editable. Kept short per the owner's request.

## Goal

Browse and open any file in the currently open project from a new dock
panel, instead of typing a path into the text editor's inline field by
hand.

## Design

- **`TestEnv/src/FileTreePanel.h`** (new) — no persistent tree state of its
  own. Walks `std::filesystem` live, once per frame, rooted at
  `g_EditorProjectPath` (`EditorProject.h`, already exists). Each directory
  is an `ImGui::TreeNode`; ImGui tracks open/closed state per-ID itself
  (persisted via `imgui.ini`, same as every other collapsible thing in this
  editor already), so there's no tree-mirroring data structure to keep in
  sync -- a file created or deleted outside the editor just shows up
  differently next frame. Files are `ImGui::Selectable` leaves. Dot-files
  are skipped. If no project is open (`g_EditorProjectPath` empty), shows
  a placeholder ("Open or create a project to browse its files"), same
  empty-state convention every other panel in this editor already uses.
- **`TestEnv/src/TextEditorPanel.h`** (modified) -- the Open button's logic
  (currently inline in `DrawFileBar()`'s click handler) is factored into a
  public `bool OpenFile(const std::string& path)` method, so both the Open
  button and a file-tree click go through the same path.
- **`TestEnv/src/EditorShell.h`** (modified) -- owns the new
  `FileTreePanel`, docks it as a new panel (left as a fresh dock target,
  not forced into an existing column -- easy to drag wherever once it
  exists, same as every panel in this editor already is). Each frame,
  forwards a file-tree click into `m_TextEditor.OpenFile(path)`.

## Testing

The filesystem-walking logic itself has no ImGui dependency worth
factoring out separately -- it's a straightforward recursive directory
listing, not complex enough to warrant the same free-function-for-testing
treatment `CopyVTermScreenToGrid`/`RenderBufferToGrid` got. Verified by a
live capture (a real project folder with a few files/subfolders, confirm
the tree renders correctly) and a manual click-through (open a file from
the tree, confirm it loads in the editor) -- disclosed as manual, per
every other UI-interaction check in this editor pivot.
