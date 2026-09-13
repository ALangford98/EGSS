# Editor theming — design

## Goal

Let the editor's whole look — the general UI chrome, the text editor's syntax
colors, the terminal's default colors, and which fonts each of those three
uses — be defined by a JSON theme file, switchable and previewable from a
small in-editor panel, with a curated set of picker controls in that panel
and full per-`ImGuiCol_` control available to anyone willing to hand-edit the
JSON. Ship two themes: `basic.json` (today's actual look, unchanged) and
`default.json` (a real Everforest palette, extended from the syntax-only one
that already exists), with `default.json` loading on first run.

## Why this shape

- **One font today, but the rendering path is already font-agnostic.**
  `ImGuiLayer::SetFontPath` loads exactly one `.ttf`
  (`DejaVuSansMono.ttf`) for the entire application, set once before
  `OnAttach`. But `CellGrid::Render()` — shared by `TextEditorPanel` and
  `TerminalPanel` — sizes its cells from `ImGui::GetFont()`/`GetFontSize()`,
  the *currently active* font, not a fixed reference. That means three
  independent font slots (Controls/Editor/Terminal) need no changes to
  `CellGrid` at all — just `ImGui::PushFont(...)`/`PopFont()` around each
  panel's own `m_Grid.Render()` call, and a way to load more than one font
  into the atlas.
- **Runtime font switching is a known, supported ImGui pattern, not new
  engine risk** — though which mechanism depends on the vendored version,
  and this project's is the newer one. This vendored ImGui (1.92.x)
  manages font textures itself through `ImTextureData`/`ImTextureStatus`,
  not the older `CreateFontsTexture`/`DestroyFontsTexture` pair some ImGui
  versions expose (checked against `imgui_impl_opengl3.h` directly, not
  assumed): a fresh `ImFontAtlas::Clear()` + re-add fonts + `Build()`
  leaves the new atlas's texture at its constructor-default status,
  `ImTextureStatus_WantCreate`, which
  `ImGui_ImplOpenGL3_RenderDrawData` already checks for and uploads every
  frame on its own. No manual texture-destroy call needed at all.
- **The terminal already has a real hook for this.** Nothing today ever
  calls `vterm_screen_set_default_colors` — libvterm just uses its own
  built-in default, and `TerminalPanel::CopyVTermScreenToGrid` reads
  whatever `vterm_screen_convert_color_to_rgb` resolves. Setting the
  default colors is one real libvterm call, not a workaround — the
  screen-level function (rather than the lower-level
  `vterm_state_set_default_colors`) is what the header itself documents as
  also re-coloring cells already on screen, which is what "you picked a
  new terminal theme" should actually do to a terminal with existing
  output in it.
- **A JSON *parser* already exists (`GS::Json`) but not a writer.** Written
  for glTF, read-only by design. Saving a theme needs a small hand-rolled
  writer in the same style `Instrumentor.cpp` already uses for
  `profile.json` — direct `ofstream <<`, not a new dependency.
- **5 roles, not 55 pickers.** `ImGuiCol_` has ~60 entries; asking for a
  color per entry in the panel would bury the "basic controls" ask you
  actually made. Everforest itself is a small palette (a background, a
  foreground, a handful of accents) applied broadly, which is what a
  5-role model does naturally. An `"advanced"` block in the JSON, keyed by
  the literal `ImGuiCol_` name, is the pressure-release valve for anything
  the 5 roles don't reach — "complex theming lives in a json file," exactly
  as asked, without inventing a second UI for it.

## Scope

**In scope:**
- A `Theme` struct: 5 UI color roles, 3 font slots (path + size), 2 terminal
  colors, the existing per-syntax editor colors (moved from the hardcoded
  `EverforestDark()` into per-theme data), and a raw `ImGuiCol_` override map.
- Hand-rolled JSON load (via `GS::JsonValue::Parse`, already vendored) and
  save (via `ofstream`, following `Instrumentor.cpp`'s pattern) for that
  struct, at `TestEnv/assets/themes/*.json`.
- Applying a theme: `ImGuiStyle::Colors[]` written from the 5 roles (plus
  advanced overrides layered on top), a live font-atlas rebuild for the 3
  font slots, `TerminalPanel` picking up its 2 colors and calling
  `vterm_state_set_default_colors`, and `TextEditorPanel` reading the
  syntax colors from the active theme instead of a hardcoded call.
- A new "Appearance" panel: a list of themes found in the folder (load on
  click), 5 color pickers, 3 font browse rows (reusing `FileBrowserPopup`,
  the same component "Add Script" already uses), 2 terminal color pickers,
  and "Save As" to write the panel's current values to a theme file.
- `basic.json` capturing today's real values (ImGui's stock dark theme
  colors reduced to the 5 roles, current font/size in all 3 slots,
  terminal at whatever libvterm's own default already is, today's syntax
  colors) so applying it changes nothing visually from what a fresh clone
  looks like right now.
- `default.json`: a full Everforest palette across all of the above,
  reusing the real, sourced values `EditorTheme.h`'s `EverforestDark()`
  already has for the syntax fields.
- Remembering the last-applied theme across runs, mirroring
  `EditorProject.h`'s existing `EditorLastScenePath()` pattern exactly (a
  small, not-checked-in text file beside the executable).

**Explicitly out of scope / deferred:**
- **A picker for every raw `ImGuiCol_`.** That's what `"advanced"` in the
  JSON is for.
- **The 16-color ANSI palette.** Only the terminal's default fg/bg, per
  scope.
- **Non-TTF fonts, font weight/style variants, or bundling new font
  files.** The panel points at whatever `.ttf` already exists on disk;
  nothing here ships a second font.
- **Watching the themes folder for external edits while running.** A
  theme is (re)read when explicitly selected in the panel, not polled.
- **Per-project themes.** One active theme, applied engine-wide, the same
  way the font and style are global today.

## Design

### `Theme` (new, `TestEnv/src/Theme.h`)

```cpp
struct ThemeFontSlot
{
    std::string Path;
    float Size = 16.0f;
};

struct Theme
{
    std::string Name;

    // The 5 roles. Hover/active variants (ButtonHovered, HeaderActive, ...)
    // are not stored -- they're Accent/FrameBackground lightened or
    // darkened by a fixed formula at Apply() time, the same relationship
    // ImGui's own built-in themes already have between a color and its
    // *Hovered/*Active siblings.
    ImU32 Background;
    ImU32 Foreground;
    ImU32 FrameBackground;
    ImU32 Accent;
    ImU32 Border;

    ThemeFontSlot ControlsFont;
    ThemeFontSlot EditorFont;
    ThemeFontSlot TerminalFont;

    ImU32 TerminalBackground;
    ImU32 TerminalForeground;

    // EditorTheme.h's fields, renamed with a Syntax prefix. Background and
    // Foreground stay independent values here rather than folding into the
    // general roles above -- checked, not assumed: today, right now, the
    // text editor already paints its own Everforest-toned background/
    // foreground (0x2d353b/0xd3c6aa) while the rest of the UI is still
    // ImGui's stock dark theme (~0x0f0f0f). Folding them would make
    // basic.json (whose whole job is reproducing that real, current,
    // two-tone state) silently repaint the text editor a different color.
    // default.json is free to set these equal to its own general
    // Background/Foreground -- Everforest is one coherent palette there --
    // but the struct doesn't force that. Only CurrentLineNumberFg is
    // dropped: today's EverforestDark() always sets it to exactly
    // Foreground with no exception anywhere in the code, so it is derived
    // at render time instead of stored.
    ImU32 SyntaxBackground;
    ImU32 SyntaxForeground;
    ImU32 SyntaxLineNumberFg;
    ImU32 SyntaxCursorColor;
    ImU32 SyntaxKeyword;
    ImU32 SyntaxStringLiteral;
    ImU32 SyntaxComment;
    ImU32 SyntaxSelectionBg;

    // Keyed by the literal enum name ("ImGuiCol_TitleBgActive"), applied
    // after the role mapping so it always wins. The JSON's "complex
    // theming" half; the panel never edits this map directly.
    std::unordered_map<std::string, ImU32> AdvancedOverrides;
};
```

### Role → `ImGuiCol_` mapping

Applied in `ThemeManager::Apply`, in this order: roles first (whole style
overwritten from the 5 values plus their derived hover/active variants),
then `AdvancedOverrides` layered on top by exact name. Representative
grouping (the implementation plan enumerates every one of the ~60 entries
against these five; this is the shape, not the exhaustive list):

- **Background** — `WindowBg`, `ChildBg`, `PopupBg`, `TitleBg`,
  `TitleBgCollapsed`, `MenuBarBg`, `ScrollbarBg`, `DockingEmptyBg`,
  `TableRowBg`/`TableRowBgAlt` (the latter a slight lighten, so alternating
  rows stay visible against flat Background).
- **Foreground** — `Text`, `InputTextCursor` (a caret reads as text-colored).
  `TextDisabled` is Foreground at reduced alpha, not a stored value.
- **FrameBackground** — `FrameBg`, `Tab`, `TabDimmed`.
- **Accent** — every interactive/active/selected color: `Button*`,
  `Header*`, `CheckMark`, `CheckboxSelectedBg`, `SliderGrab*`,
  `FrameBgHovered`/`FrameBgActive`, `TabHovered`, `TabSelected` (+
  `TabSelectedOverline`), `TabDimmedSelected` (+ its overline),
  `TitleBgActive`, `TextSelectedBg`, `TextLink`, `PlotLines`,
  `PlotHistogram` (+ their `*Hovered` siblings, lightened further),
  `DragDropTarget`, `NavCursor`, `DockingPreview`, `UnsavedMarker`,
  `NavWindowingHighlight`.
- **Border** — `Border`, `BorderShadow`, `Separator*`, `ScrollbarGrab*`,
  `ResizeGrip*`, `TableBorderStrong`, `TableBorderLight`, `TreeLines`.
- **Fixed, not role-derived** — `NavWindowingDimBg`, `ModalWindowDimBg`,
  `DragDropTargetBg`: screen-dimming overlays, defined as Background at a
  constant ~35% alpha regardless of theme, the same way ImGui's own
  built-in themes treat them as a darken-everything effect rather than a
  themed color.

Hover = role lightened 15% toward white; Active = role darkened 10% toward
black — one small `Lighten(ImU32, float)`/`Darken(ImU32, float)` pair, a
plain per-channel lerp.

### JSON format

Hex strings (`"#rrggbb"`, alpha implied 255 unless `"#rrggbbaa"` given),
matching how a human would actually type a color rather than 4 float
fields:

```json
{
  "name": "Everforest",
  "colors": {
    "background": "#2d353b",
    "foreground": "#d3c6aa",
    "frameBackground": "#3a4147",
    "accent": "#a7c080",
    "border": "#4f585e"
  },
  "fonts": {
    "controls": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 },
    "editor":   { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 },
    "terminal": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 }
  },
  "terminal": {
    "background": "#2d353b",
    "foreground": "#d3c6aa"
  },
  "editorSyntax": {
    "background": "#2d353b",
    "foreground": "#d3c6aa",
    "keyword": "#e67e80",
    "stringLiteral": "#a7c080",
    "comment": "#859289",
    "lineNumber": "#7a8478",
    "cursor": "#d3c6aa",
    "selectionBg": "#424a50"
  },
  "advanced": {
    "ImGuiCol_TitleBgActive": "#3a4147"
  }
}
```

Loading: `GS::JsonValue::Parse`, then a small `ParseHexColor(const
std::string&, ImU32& out)` per field, with a named fallback (today's
`Theme` default) on a missing or malformed key — the same "never throws,
always has a fallback" contract `JsonValue`'s own accessors already follow.
Saving: hand-written `ofstream <<`, one line per field, mirroring
`Instrumentor.cpp`'s style rather than pulling in a serializer for a file
this small.

### `ThemeManager` (new, `TestEnv/src/ThemeManager.h`)

```cpp
namespace ThemeManager {
    // Every *.json directly inside the themes folder, filenames only.
    std::vector<std::string> ListThemes();

    bool LoadThemeFile(const std::string& filename, Theme& out, std::string& error);
    bool SaveThemeFile(const std::string& filename, const Theme& theme);

    // Writes ImGuiStyle::Colors[] (roles, then advanced overrides), queues
    // the 3 font slots for ImGuiLayer's next-frame reload, and updates
    // Current() -- TerminalPanel/TextEditorPanel read Current() themselves
    // (see below), so Apply doesn't reach into either panel directly.
    void Apply(const Theme& theme);

    const Theme& Current();

    // Load + Apply + remember as last-applied (mirrors EditorLastScenePath).
    bool ApplyByFilename(const std::string& filename);

    inline const char* ThemesFolder() { return "assets/themes"; }
    inline const char* LastThemePath() { return "editor_last_theme.txt"; }
}
```

At startup (`LoadEditorProjectFromCommandLine`, alongside the existing
last-scene logic): read `LastThemePath()`; if present and that file still
exists in the themes folder, `ApplyByFilename` it; otherwise
`ApplyByFilename("default.json")`.

### Wiring the two panels to a live theme, without `ThemeManager` reaching in

`TerminalPanel` and `TextEditorPanel` each read `ThemeManager::Current()`
themselves, the same pull-per-frame shape this editor already uses
elsewhere (`ReadHoveredEntity()` recomputing every frame rather than being
pushed a value):

- `TextEditorPanel`: drop the hardcoded `EditorTheme m_Theme =
  EverforestDark();` member entirely; `RenderBufferToGrid` takes
  `ThemeManager::Current()`'s syntax fields directly at render time (a
  handful of `ImU32` copies — cheap, and it means a theme switch shows up
  in the text editor on the very next frame with no cache to invalidate).
- `TerminalPanel`: compare `ThemeManager::Current()`'s
  `TerminalBackground`/`TerminalForeground` against two cached members each
  frame; call `vterm_screen_set_default_colors(m_Screen, ...)` only when
  they differ, so a steady-state frame costs one integer comparison, not a
  libvterm call.
- Both wrap their existing `m_Grid.Render()` call in
  `ImGui::PushFont(...)`/`PopFont()`, using new getters on `ImGuiLayer`
  (`GetEditorFont()`, `GetTerminalFont()`) reached via
  `GS::Application::Get().GetImGuiLayer()`, the same accessor
  `GS::Application::Get().GetWindow()` already demonstrates is fine to call
  from anywhere.

### Live font reload (`ImGuiLayer` changes)

```cpp
// New on ImGuiLayer, callable at runtime (unlike the existing static
// SetFontPath, which only works pre-OnAttach):
void RequestFontReload(const ThemeFontSlot& controls, const ThemeFontSlot& editor, const ThemeFontSlot& terminal);
ImFont* GetControlsFont() const;   // == io.FontDefault after a reload
ImFont* GetEditorFont() const;
ImFont* GetTerminalFont() const;
```

`RequestFontReload` just stores the three requested slots and sets a
`m_FontReloadPending` flag — it must not touch `io.Fonts` mid-frame. At the
very top of `ImGuiLayer::Begin()`, **before** `ImGui_ImplOpenGL3_NewFrame()`
(line 106 today): if the flag is set, `io.Fonts->Clear()`, add the three
fonts (each falling back to `AddFontDefault()` on a load failure, same
guard `OnAttach` already has), `io.FontDefault = <controls font>`,
`io.Fonts->Build()`, clear the flag — nothing else. The next
`ImGui_ImplOpenGL3_RenderDrawData` call picks up the new atlas's texture
(freshly constructed at `ImTextureStatus_WantCreate`) on its own; no manual
destroy/recreate call exists to make in this backend version.

### The "Appearance" panel (new, `TestEnv/src/AppearancePanel.h`)

A `Layer`, pushed in `TestApp.cpp` alongside `ProfilerPanel`/`TerminalPanel`.
`ImGui::Begin("Appearance")`:

- A list (one `Selectable` per entry from `ThemeManager::ListThemes()`);
  clicking one calls `ApplyByFilename` and refreshes the panel's own
  editable fields from `ThemeManager::Current()`.
- 5 `ImGui::ColorEdit4` pickers, one per role, applying immediately via
  `ThemeManager::Apply` on edit (same `IsItemDeactivatedAfterEdit`-gated
  pattern already used throughout `EditorSceneView`'s Inspector, so a drag
  doesn't call `Apply` — and thus doesn't touch `ImGuiStyle`/queue a font
  reload — on every intermediate frame).
- 3 rows (Controls/Editor/Terminal), each a path field + "Browse..."
  reusing `FileBrowserPopup` filtered to `.ttf`, plus a size drag-float.
- 2 `ImGui::ColorEdit4` pickers for the terminal's background/foreground.
- "Save As" — a text field for a filename + button, calling
  `SaveThemeFile` with the panel's current in-memory `Theme` (not
  re-reading `ThemeManager::Current()`, so unsaved picker tweaks are what
  gets written).

### `basic.json` / `default.json`

`basic.json`'s 5 roles are the real values read straight from
`imgui_draw.cpp`'s `StyleColorsDark()` (not guessed): Background from
`WindowBg` `(0.06,0.06,0.06,0.94)` → `#0f0f0ff0`, Foreground from `Text`
`(1,1,1,1)` → `#ffffffff`, FrameBackground from `FrameBg`
`(0.16,0.29,0.48,0.54)` → `#294a7a8a`, Accent from `Button`
`(0.26,0.59,0.98,0.40)` → `#4296fa66`, Border from `Border`
`(0.43,0.43,0.50,0.50)` → `#6e6e8080`. This is a **5-role approximation of**
`StyleColorsDark()`, not a byte-identical copy — collapsing ~60 distinct
`ImGuiCol_` values down to 5 necessarily loses some of them (`ChildBg`,
notably, is transparent black `(0,0,0,0)` in the real default so nested
child windows show through to their parent, which the Background role
would otherwise make opaque) — so `basic.json`'s `"advanced"` block carries
`"ImGuiCol_ChildBg": "#00000000"` to preserve that one specifically, since
it's the one case where the collapse produces a visibly different result
rather than just a slightly different shade of the same dark gray.

Terminal colors are libvterm's own real compiled-in default, read from
`GS/vendor/libvterm/src/pen.c`'s `vterm_state_newpen` (`default_fg` =
`(240,240,240)` = `#f0f0f0`, `default_bg` = `(0,0,0)` = `#000000`), not
probed or guessed. All 3 font slots stay `DejaVuSansMono.ttf`/16. The
`editorSyntax` block is today's real `EverforestDark()` values, including
its own `Background`/`Foreground` (`#2d353b`/`#d3c6aa`) — moving that
struct's content unmodified into this file preserves current visual
behavior exactly for the one part of the screen that's already Everforest
today, per this project's usual "the measurement, not the code" caution
about accidentally changing behavior while relocating it.

`default.json`: `editorSyntax` unchanged from `basic.json` (Everforest is
already correct there), extended with a full-style Everforest role set —
`Background`/`Foreground` from `EditorTheme.h`'s own `bg0`/`fg` (already
sourced from `everforest.vim`, matching the syntax block's own values so
the whole editor reads as one coherent surface instead of two), `Accent`
from the same file's green, `FrameBackground`/`Border` from Everforest's
published `bg2`/`bg4` steps (`#3d484d`/`#4f585e`), the same
dark/medium-contrast palette family `bg0`/`fg` already come from — same
fonts, terminal colors matching `Background`/`Foreground`.

## Testing

Temporary self-tests (deleted after, per this project's convention), all
directly checkable without a live ImGui frame:

- Hex-color round trip: parse `"#2d353b"`, confirt it decodes to the exact
  `IM_COL32(0x2d, 0x35, 0x3b, 255)`; parse `"#2d353b80"` and confirm the
  alpha byte; a malformed string falls back to the given default rather
  than crashing.
- `SaveThemeFile` then `LoadThemeFile` round-trips a hand-built `Theme`
  exactly (every role, font slot, terminal color, syntax color, and at
  least one `AdvancedOverrides` entry survive byte-for-byte).
- `Lighten`/`Darken` checked against hand-computed values for a couple of
  known inputs (pure black, pure white, a mid-tone), not re-derived from
  the same formula the implementation uses.
- `ListThemes()` against a temporary directory seeded with known filenames,
  confirming exactly those `.json` names come back and a non-`.json` file
  in the same folder is excluded.

Interactive/visual pieces (the panel's pickers, the live font-atlas
rebuild actually redrawing with a new font, the terminal actually
recoloring) get the same disclosed, standing gap every other editor UI
feature here already has — checked with a forced-open visual capture where
useful, live click-through unverified, no GUI-automation tool in this
environment.
