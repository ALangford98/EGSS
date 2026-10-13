# Project save/load, reworked (project-centric)

**Status:** owner chose the project-centric model (option A) on 2026-10-13
and asked for it to be scoped and built in one go. The decisions below were
made during that work and are recorded for review.

## Why

What was wrong before, found by reading the code:

1. **A relaunch reopened the last *scene*, not the project.** Afterwards the
   Scenes panel was empty, Export was disabled, and Save wrote only the
   scene file.
2. **Asset paths were stored relative to the editor's executable folder.**
   Script and material picks became `../../../../MyGame/x.gss` from
   `bin/<Config>/TestEnv/`, so moving or copying a project broke every link.
3. **Save As lost most of the project.** It wrote one `scene.txt` and a
   manifest naming only that scene, dropped every other scene and asset, and
   silently overwrote any project already in the target folder.
4. **New Project wrote straight into the chosen folder.** The name was only a
   label, so picking `~/Documents` put project files directly in Documents,
   overwriting anything already there.
5. **Open Project reported failure only in the log**, and there was no
   recent-projects list.
6. **Saving had gaps:**
   - Ctrl+S did nothing for the scene.
   - Quitting threw unsaved work away without asking.
   - Nothing showed which project was open or that it had unsaved changes.
   - Save was disabled on a fresh start and after Open Demo.
7. **Small things:**
   - Switching scenes didn't change which scene the project opens with.
   - The dialogs shared their text buffers.

## The model

- **The editor always works inside a project.** A project is a folder
  holding `project.gsproj`.
  - With none open, a **Projects** window lists recent projects plus New
    Project and Open Project. It is an ordinary window, not a modal, so the
    demo selector and the rest of the UI stay usable.
- **At startup:**
  - `--project` opens that project.
  - Otherwise the most recent project is reopened, if it still exists.
  - Otherwise the Projects window shows.
  - `--scene <file>` still opens a loose scene, for the capture harness and
    automation. It is the only way to get a scene without a project.
  - `--demo` is unchanged.
  - `editor_last_scene.txt` is no longer read or written.
- **Every scene belongs to the open project.**
- **Asset paths are project-relative.** When a project opens it becomes
  `GS::Assets` root, the same seam the player uses.
  - An asset picked inside the project is stored relative to the project.
  - One picked outside is stored absolute, and the Inspector marks it as
    outside the project. The exporter already copies outside assets into
    `external/`.
  - Editor-side reads of these paths go through `Assets::Resolve`.
- **Older projects are migrated on open, in memory.** A relative path that
  doesn't resolve under the project but does from the working directory
  (the old convention) is rewritten. It becomes project-relative if the file
  is inside the project, absolute otherwise.
  - Rewrites are counted in the log.
  - The scene opens marked unsaved, so the next save writes the new form.
    Nothing is written without the user saving.

## File menu

| Item | Does |
| --- | --- |
| New Project… | Name + Location (a parent folder). Shows "Creates: `<Location>/<Name>/`". Refuses a folder that exists and isn't empty. Creates `scenes/Main.txt` and the manifest, then opens it. |
| Open Project… | Pick `project.gsproj` (or type its folder). Errors are shown in the dialog. |
| Open Recent ▸ | Up to 10 projects, most recent first; missing ones are greyed out. |
| Close Project | Back to the Projects window. |
| Save Scene (Ctrl+S) | Writes the current scene, and the manifest records it as the scene to open with. |
| Save Scene As… | A new scene in this project (`scenes/<name>.txt`), which becomes the current one. |
| Rename Project… | Unchanged: renames the manifest name only. |
| Duplicate Project… | Copies the whole folder to `<Location>/<Name>/`, renames it in the copy's manifest, and opens the copy. Replaces the old project-level Save As. |
| Export Game… | Unchanged. |
| New Material… | Unchanged, with its own dialog fields. |
| Add Demo Scene ▸ | Replaces Open Demo. The demo's content becomes a new scene in the current project, saved and opened. Needs a project. |

Every action that replaces the open scene goes through the existing
unsaved-changes guard: Open Project, Open Recent, Close Project, switching
scenes, Add Demo Scene, and **quitting**.

## Saving and quitting

- **Ctrl+S saves the scene.** It is left alone when the text editor has
  focus, because there it already saves and builds the script.
- **Quitting asks first.** The window's close request asks the editor
  before the application stops: a new `Application::SetCloseRequestHandler`
  in the engine, plus `Application::Close()`.
  - If the scene or the material editor has unsaved work, the same Unsaved
    Changes modal appears, and choosing Save saves both.
- **Unsaved changes are visible.** The window title is
  `<Project> — <Scene>*  ·  GS Editor`.
- **Switching scenes** in the Scenes panel updates which scene the
  manifest opens with.

## Recent projects

- **Stored per user, not per build:** in
  `$XDG_CONFIG_HOME/gs-editor/recent-projects.txt` (or `~/.config/...`), one
  absolute path per line.
  - Beside the binary (where `imgui.ini` lives) would give Debug and Release
    separate lists.
  - **This is the one file the editor now writes outside the repository.**
    It is the open question for the owner.
- **Automated runs leave it alone.** A run with `--capture`, `--play` or
  `--export` neither reads nor writes it, so test runs don't rewrite the
  owner's list or reopen a project behind a capture.

## Not changed

- **The project and scene file formats.** New projects keep their scenes
  under `scenes/`; older projects keep whatever paths their manifest names.
- **The demos, the exporter and the player**, apart from the repository's
  own BreakoutRecreation project, whose script paths become
  project-relative (`paddle.gss`).

## Verification

- **A temporary self-test** drives the project functions with no UI against
  a scratch folder and a scratch `XDG_CONFIG_HOME`, checking file contents
  rather than return values:
  - create, save, Save Scene As, switch, duplicate (the copy opens and its
    links resolve);
  - refusal of a non-empty folder;
  - migration of an old-style project;
  - the recent-projects list, and that automated runs don't touch it.
- **Regression checks:**
  - The existing editor, material and cube captures stay byte-identical.
  - BreakoutRecreation in editor Play (`--project ... --start-play`) and in
    GSPlayer stays identical at step 150.
  - BreakoutRecreation still exports and runs.
- **A UI capture** of the Projects window and the File menu, looked at by
  eye.
