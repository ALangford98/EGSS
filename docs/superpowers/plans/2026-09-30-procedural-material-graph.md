# Procedural Material Graph Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **This run:** executed natively (owner's standing rule: no subagents by
> default), and the owner waived the plan-review gate ("write the spec then
> start implementing"). So this plan fixes interfaces, file layout and the
> checks each task must pass; the code itself lives in the files, not
> duplicated here.

**Goal:** A node-graph procedural material generator in the editor that
exports tileable albedo/height/normal/roughness PNGs and updates linked scene
entities live.

**Architecture:** Pure CPU generation in `GS/src/GS/Procedural/` (noise,
graph model + evaluator, `.gsmat` serializer, PNG export) with no UI and no
GL, so every number can be checked from a self-test. `TestEnv/src/
MaterialEditorPanel.h` drives it through vendored `imnodes`. The scene link
adds `MeshComponent::MaterialPath` and an albedo sampler to the editor shader.

**Tech Stack:** C++17, glm, ImGui 1.92.9b (docking) + imnodes, stb_image_write,
`GS::JsonValue` (existing parser).

**Spec:** `docs/superpowers/specs/2026-09-30-procedural-material-graph-design.md`

## Global Constraints

- Images are `float` per channel; Grey = 1 channel, Colour = 4 (RGBA). 8-bit only at export/upload.
- Resolution: power of two, 256–2048, default 512.
- Tiling invariant: every node's output tiles if its inputs do. Neighbour reads wrap.
- Determinism: no clocks, no `rand()`; same graph + resolution → byte-identical output.
- Image row 0 is v = 0. The engine loads PNGs with `stbi_set_flip_vertically_on_load(1)`, so export writes with `stbi_flip_vertically_on_write(1)` set **explicitly every call** (`ScreenCapture.cpp` leaves that global set).
- Parameters are resolution-independent (radii/strengths in UV units), so a ¼-res preview and the export look alike.
- `GS/` code never includes imgui or imnodes.
- Never commit to `main`; this branch is `material-graph`. Build all three configs before calling a task done.
- Temporary checks live in `TestEnv/src/MaterialGraphTest.h` (marked `TEMPORARY`), grown per task, deleted in the last task.

## Review Focus

1. **Loading a `.gsmat` written by hand or by a future build** — unknown node type, missing params, a link to a nonexistent pin, two Albedo outputs: refused with a named error, never a crash or a partial graph. Pinned in Task 4.
2. **Deleting a node that has links in and out** — both sides' links go, downstream nodes go dirty and fall back to input defaults. Pinned in Task 3.
3. **Blend with mismatched or changing input types** — connecting a Colour into A while B is Grey is refused; reconnecting A to Colour while the Blend already feeds a Grey-only input is refused. Pinned in Task 3.
4. **Changing resolution mid-session** — every cache invalidates and images come back at the new size; linked scene textures are recreated at the new size, not written through a size-mismatched `SetData` (which asserts). Pinned in Tasks 3 and 6.
5. **A scene entity linked to a `.gsmat` that is deleted or broken while the editor is open** — flat-colour fallback, one warning, no per-frame spam. Pinned in Task 6.

---

### Task 1: Vendor imnodes and wire it into the build

**Files:**
- Create: submodule `GS/vendor/imnodes` (done in the spike: upstream `eb36902`, compiles clean against ImGui 1.92.9b)
- Modify: `GS/vendor/imgui_premake5.lua` (add `imnodes/imnodes.cpp`, `imnodes/imnodes.h`, include dir `imnodes`)
- Modify: `premake5.lua` (`IncludeDir["imnodes"]`, added to TestEnv's includedirs)

**Interfaces:** Produces `#include <imnodes.h>` usable from `TestEnv/`.

- [ ] Add the files and include dir; `./gs.py build all` succeeds in Debug, Release, Dist.
- [ ] Commit.

The "render one node" half of the spike is checked in Task 5, the first place a context exists to render into.

### Task 2: Tileable noise

**Files:**
- Create: `GS/src/GS/Procedural/Noise.h`, `GS/src/GS/Procedural/Noise.cpp`
- Create: `TestEnv/src/MaterialGraphTest.h` (TEMPORARY), call from `TestApp.cpp`

**Interfaces (Produces):**

```cpp
namespace GS::Noise {
    uint32_t Hash(uint32_t x);                        // integer avalanche hash
    uint32_t Hash(int x, int y, uint32_t seed);
    float    HashToUnit(uint32_t h);                  // [0,1)
    // p in lattice units; period in lattice cells, >= 1 each axis.
    // Unit-length random gradients, quintic fade. Exactly 0 at lattice points.
    float Perlin(glm::vec2 p, glm::ivec2 period, uint32_t seed);
    // sum_{i<octaves} gain^i * Perlin(p*lac^i, period*lac^i, seed+i). Unnormalised.
    float Fbm(glm::vec2 p, glm::ivec2 period, int octaves, int lacunarity, float gain, uint32_t seed);
    struct VoronoiResult { float F1; float F2; uint32_t CellId; };
    // One feature point per cell at centre + (hash - 0.5) * jitter; 3x3 search; wraps by period.
    VoronoiResult Voronoi(glm::vec2 p, glm::ivec2 period, float jitter, uint32_t seed);
}
```

**Checks (self-test, against formulas not the implementation):**
- `Perlin` is exactly 0 at 100 integer lattice points.
- Tiling: `Perlin(p) == Perlin(p + period)` exactly in x and y over 1,000 points; same for `Fbm` and `Voronoi`.
- Over a 512² grid: |mean| < 0.02; max |value| ≤ √2/2.
- fBm variance ratio (octaves 4, gain 0.5, lacunarity 2) vs single octave ≈ Σ g²ⁱ = 1.328, within 10%.
- Voronoi: F1 = 0 at a feature point (within 1e-6); F2 ≥ F1 everywhere; jitter 0 → F1 equals the closed-form distance to the nearest cell centre `(floor(p)+0.5)`.

- [ ] Write the checks, run (fails to link: no Noise), implement, run to 0 FAIL, three-config build, commit.

### Task 3: Graph model and evaluator, all node types

**Files:**
- Create: `GS/src/GS/Procedural/Image.h` (header-only)
- Create: `GS/src/GS/Procedural/MaterialGraph.h`, `GS/src/GS/Procedural/MaterialGraph.cpp`
- Create: `GS/src/GS/Procedural/MaterialNodes.cpp` (node type table + per-node evaluation, kept out of the evaluator file)

**Interfaces (Produces):**

```cpp
namespace GS {
struct Image {
    int Width = 0, Height = 0, Channels = 1;
    std::vector<float> Data;
    static Image Filled(int w, int h, int channels, const float* value);
    float& At(int x, int y, int c);  float At(int x, int y, int c) const;
    float Wrap(int x, int y, int c) const;              // integer wrap
    float Bilinear(float u, float v, int c) const;      // UV, wraps
};

enum class PinType { Grey, Colour, Any };               // Any: Blend's polymorphic pins
enum class ParamKind { Float, Int, Enum };
enum class NodeType { Noise, Voronoi, Pattern, Constant, Levels, Invert, Blur,
    Warp, Transform, Blend, GradientMap, ColourConstant, HeightToNormal,
    OutAlbedo, OutHeight, OutNormal, OutRoughness, Count };

struct PinInfo   { const char* Name; PinType Type; float Default; };   // Default: unconnected value
struct ParamInfo { const char* Name; ParamKind Kind; float Min, Max, Default; std::vector<const char*> Labels; };
struct NodeTypeInfo {
    const char* Name; const char* Category;             // Name is the .gsmat key
    std::vector<PinInfo> Inputs, Outputs; std::vector<ParamInfo> Params;
    bool HasColour = false, HasRamp = false, IsOutput = false;
};
const NodeTypeInfo& GetNodeTypeInfo(NodeType type);
Image HeightToNormal(const Image& height, float strength);   // used by the node and the test
bool NodeTypeFromName(const std::string& name, NodeType& out);

struct GradientStop { float Position; glm::vec4 Colour; };
struct MaterialNode {
    int Id; NodeType Type; glm::vec2 Position;
    std::vector<float> Params; glm::vec4 Colour{1.0f}; std::vector<GradientStop> Ramp;
};
struct MaterialLink { int FromNode, FromPin, ToNode, ToPin; };

enum class ConnectResult { Ok, InvalidPin, TypeMismatch, Cycle };

class MaterialGraph {
public:
    int  AddNode(NodeType type, glm::vec2 position, int id = -1);  // -1 if an output of that kind exists
    void RemoveNode(int id);                                        // drops its links, dirties downstream
    ConnectResult Connect(int fromNode, int fromPin, int toNode, int toPin); // replaces the input's link
    void Disconnect(int toNode, int toPin);
    void SetParam(int id, int index, float value);
    void SetColour(int id, glm::vec4 colour);
    void SetRamp(int id, std::vector<GradientStop> ramp);
    void SetPosition(int id, glm::vec2 position);                   // does not dirty
    void SetResolution(int resolution);                              // clamps to pow2 256..2048; clears caches
    void SetPreviewDivisor(int divisor);                             // 1 or 4; clears caches on change
    int  Resolution() const; int EvalResolution() const;             // Resolution / divisor
    PinType OutputType(int nodeId, int pin) const;                   // resolves Any
    const Image& Evaluate(int nodeId, int pin);                      // lazy, cached
    enum class Output { Albedo, Height, Normal, Roughness };
    Image EvaluateOutput(Output which);                              // flat default if absent
    int  FindOutputNode(Output which) const;
    int  EvalCount(int nodeId) const;                                // for tests
    const std::vector<MaterialNode>& Nodes() const; const std::vector<MaterialLink>& Links() const;
    const MaterialNode* FindNode(int id) const;
    uint64_t Version() const;                                        // bumps on any content change
};
}
```

Node semantics (UV u,v ∈ [0,1); row 0 = v 0):
- Noise: params scale(Int 1–64, 4), octaves(Int 1–10, 5), lacunarity(Int 2–4, 2), gain(0–1, 0.5), seed(Int). Out = `0.5 + 0.5 * fbm / (0.7071 * Σgⁱ)` — provably in [0,1].
- Voronoi: scale(Int, 6), jitter(0–1, 1), mode(Enum F1/F2−F1/Cell ID), seed. F1 and F2−F1 are divided by √2 cell units, clamped to 1; cell ID → `HashToUnit`.
- Pattern: rows(Int, 8), columns(Int, 4), mortar(0–0.5, 0.05, fraction of a brick's height), offset(0–1, 0.5), seed. Outputs: Mask (1 brick / 0 mortar), Random (per brick).
- Constant(value 0.5). ColourConstant (colour).
- Levels: in-min 0, in-max 1, gamma 1, out-min 0, out-max 1: `t = clamp((x-inMin)/(inMax-inMin)); out = outMin + (outMax-outMin) * pow(t, 1/gamma)`.
- Invert: `1 - x`.
- Blur: radius (0–0.1 UV), mode Box/Gaussian; separable, wrapping; radius in px = radius * EvalResolution.
- Warp: inputs Image, Offset(default 0.5 = none); strength (0–0.5 UV); sample `Image.Bilinear(u + (o-0.5)*s, v + (o-0.5)*s)`.
- Transform: tile(Int 1–16), offsetU/offsetV (0–1), rotation(Enum 0/90/180/270). Rotation acts on the fractional UV about the centre, then tile, then offset, bilinear.
- Blend: A, B (Any), Mask (Grey, default 1); mode mix/add/subtract/multiply/overlay/min/max/difference; opacity(0–1, 1). `r = lerp(a, f(a,b), mask*opacity)`, clamped [0,1]; overlay `a<0.5 ? 2ab : 1-2(1-a)(1-b)`. Alpha channel is blended the same way.
- GradientMap: Grey → Colour through the ramp (≤ 8 stops, sorted by position, linear between, clamped past the ends). Default ramp black→white.
- HeightToNormal: strength (0–10, 1). Sobel `gx`, `gy` normalised to a derivative per UV unit (÷ (8/EvalResolution)); `n = normalize(-k·gx, -k·gy, 1)`; packed `n*0.5+0.5`, alpha 1.
- Outputs: single input of the output's type; Normal with nothing connected but Height connected derives through a default HeightToNormal.
- Unconnected inputs use `PinInfo::Default` (Grey → that value; Colour → (d,d,d,1)).

Connect rules: output pin types resolve through Any (Blend's output takes A's type, else B's, else Grey). Reject if the source type differs from a typed target, if the Blend's other Any input is connected with a different type, if connecting would change a Blend's resolved type while its output already feeds a typed input of the other type, or if `toNode` reaches `fromNode` (cycle). Replacing an input's existing link is allowed.

**Checks:**
- Topological order: diamond Noise→(Levels, Invert)→Blend→Albedo-via-GradientMap evaluates; each node evaluated once (`EvalCount`).
- Cycle: connecting Blend's output back into Levels is `Cycle`; the graph's links are unchanged.
- Dirty: `SetParam` on GradientMap re-evaluates GradientMap only; Noise's `EvalCount` unchanged.
- Blend at one pixel for all 8 modes against hand values (a = 0.25, b = 0.75): mix 0.75, add 1, subtract 0, multiply 0.1875, overlay 0.375, min 0.25, max 0.75, difference 0.5.
- Levels gamma 1 on 0.3 with in 0.2–0.6 → 0.25; gamma 2 → `pow(0.25, 0.5)` = 0.5.
- HeightToNormal on `h = 0.5 + 0.5 sin(2π·2u)`, built by hand as an `Image` and passed to the free function `GS::HeightToNormal(const Image&, float strength)` — the same function the node calls: `n.x` matches `-k·h'/sqrt(1 + (k·h')²)` with `h' = 0.5·2π·2·cos(2π·2u)` within 1e-3.
- Pattern mortar fraction: brick height `hb = 1/rows`, width `wb = 1/columns`, mortar thickness `t = mortar·hb` in both directions (half on each side of every edge). Brick fraction = `(hb−t)(wb−t)/(hb·wb)`; rows 8, columns 4, mortar 0.1 → mortar fraction 0.145. Measured within 1%.
- Tiling of every generator/filter: column 0 vs the wrap of column W (evaluate at the same UV via Bilinear at u = 0 and u = 1) within 1e-5.
- Review Focus 2: removing Levels from the diamond removes its 2 links; Blend is dirty and re-evaluates with A = default.
- Review Focus 3: Colour into Blend.A while B holds Grey → `TypeMismatch`.
- Review Focus 4: `SetResolution(256)` → `Evaluate` returns 256² images; `SetResolution(300)` clamps to 256.
- Determinism: evaluating the same graph twice gives byte-identical Albedo data.

- [ ] Write checks, run (fails), implement, run to 0 FAIL, three-config build, commit (may be two commits: model/evaluator, then node bodies, if developed that way).

### Task 4: `.gsmat` serializer and export

**Files:**
- Create: `GS/src/GS/Procedural/MaterialGraphSerializer.h/.cpp`
- Create: `GS/src/GS/Procedural/MaterialExport.h/.cpp`

**Interfaces (Produces):**

```cpp
namespace GS {
std::string SerializeMaterialGraph(const MaterialGraph& graph);
bool DeserializeMaterialGraph(const std::string& text, MaterialGraph& out, std::string& error);
bool SaveMaterialGraph(const std::string& path, const MaterialGraph& graph, std::string& error);
bool LoadMaterialGraph(const std::string& path, MaterialGraph& out, std::string& error);
// Writes <dir>/<name>_{albedo,height,normal,roughness}.png and <name>.mtl, each via temp+rename.
bool ExportMaterial(MaterialGraph& graph, const std::string& gsmatPath, std::string& error);
std::vector<uint8_t> ToRGBA8(const Image& image);   // Grey replicated to RGB, alpha 255
}
```

Format: `{"version":1,"resolution":512,"nodes":[{"id":1,"type":"Noise","pos":[x,y],"params":{"scale":4,...},"colour":[r,g,b,a],"ramp":[[pos,r,g,b,a],...]}],"links":[[from,fromPin,to,toPin]]}`. Params keyed by name; a missing param takes its default; an unknown param name is an error (typo protection). Numbers written with `%.9g` so floats round-trip exactly.

**Checks:**
- save → load → save text is byte-identical; Albedo before/after is byte-identical.
- Review Focus 1: unknown node type → error naming it and `out` untouched; link to pin 7 of a 1-output node → error; two OutAlbedo → error; malformed JSON → the parser's error.
- Export writes 4 PNGs + `.mtl` into a scratch directory; reloading `_albedo.png` with stb (flip on load) gives pixel (0,0) equal to the graph's Albedo at row 0 (orientation check).
- Two exports of the same graph → identical file hashes.

- [ ] Write checks, run (fails), implement, run to 0 FAIL, three-config build, commit.

### Task 5: MaterialEditorPanel

**Files:**
- Create: `TestEnv/src/MaterialEditorPanel.h`
- Modify: `TestEnv/src/EditorShell.h` (replace the "Textures" stub window; route `.gsmat` clicks from the File Tree; dock "Material" into the centre beside Scene/Editor)
- Modify: `TestEnv/src/EditorMenuBar.h` (File → New Material…, View → Material)

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces: `class MaterialEditorPanel { void OnImGuiRender(); void Open(const std::string& path); void NewMaterial(const std::string& path); MaterialGraph* Graph(); const std::string& Path() const; bool IsFocused() const; }` and `inline MaterialEditorPanel* g_MaterialEditor`.

Behaviour: imnodes canvas (created lazily on first render with `ImNodes::CreateContext()`); pin ids `node*16 + pin` (inputs) and `node*16 + 8 + pin` (outputs), link ids = index. Right-click → categorised add menu (output kinds greyed out when present). Selected node → parameter inspector (sliders from `ParamInfo`, colour edit, ramp stop list). Thumbnails 64 px per node from its first output; preview strip of the four outputs. While any slider is active: `SetPreviewDivisor(4)`; on release: `SetPreviewDivisor(1)`. Undo/redo: snapshot = serialised text before each edit; Ctrl+Z/Ctrl+Y only while the panel window is focused. Save (Ctrl+S when focused) and Export buttons; errors shown inline in red. Refused connections show the `ConnectResult` as a tooltip for ~2 s.

**Checks:**
- Build all three configs.
- `--capture` of the editor with a sample `.gsmat` opened (a `--open-material <path>` flag, parsed in `EditorShell::OnAttach`) shows nodes, links and thumbnails; inspect the PNG.
- Measure a full 512² evaluation of the sample stone graph with the profiler (`GS_PROFILE_SCOPE`) and record the number in the changelog.

- [ ] Implement, build, capture, commit.

### Task 6: Scene link

**Files:**
- Modify: `GS/src/GS/Scene/Components.h` (`MeshComponent::MaterialPath`)
- Modify: `GS/src/GS/Scene/Scene.cpp` (save `material <path>` after `mesh`; load it)
- Create: `TestEnv/src/MaterialLibrary.h` (one evaluated graph + albedo `Texture2D` per path; reload on file change via the panel; one warning per broken path)
- Modify: `TestEnv/src/EditorSceneView.h` (shader: `u_AlbedoMap` sampler + `u_HasAlbedoMap`; pass `v_TexCoord`; Inspector Material field; bind per entity)

**Interfaces:**
- Produces: `namespace MaterialLibrary { std::shared_ptr<GS::Texture2D> Albedo(const std::string& path); void NotifyEdited(const std::string& path, GS::MaterialGraph& graph); }` — `Albedo` returns nullptr for a broken/missing path (logged once per path per failure). `NotifyEdited` re-uploads, recreating the texture when the resolution changed (Review Focus 4).

**Checks:**
- Byte-identical `--hide-ui --lockstep --capture-step` capture of an editor scene with no `material` lines, before and after; same for `--demo Cube3D`.
- A plane linked to a ColourConstant (0.2, 0.4, 0.6) graph: centre pixel via `Framebuffer::ReadPixelRGBA` equals the hand-computed lit colour for the scene's light (computed in the test from the shader's own documented terms: ambient + Lambert, specular off-axis).
- An old scene file (checked-in `BreakoutRecreation` scene) loads unchanged and re-saves byte-identically.
- Review Focus 5: delete the linked `.gsmat` while open → flat colour, exactly one warning in the log over 120 frames.

- [ ] Implement, run checks, three-config build, commit.

### Task 7: Finish

- [ ] `./gs.py sanitize` over the self-test.
- [ ] Delete `MaterialGraphTest.h` and its `TestApp.cpp` call; `grep -rn TEMPORARY TestEnv/src` is empty.
- [ ] Changelog entry (what was built, the measurements, the UI click-through gap), `docs/STATE.md` one-liner, `editor_roadmap.md` #11 marked piece 1 of 3 done.
- [ ] Three-config build, commit.
