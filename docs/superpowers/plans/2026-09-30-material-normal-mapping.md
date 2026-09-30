# Material Normal Mapping Implementation Plan

> Executed natively (owner's no-subagents rule), committed directly on `main`.

**Goal:** Render the Normal output of linked `.gsmat` materials in the editor.

**Architecture:** A second texture per `MaterialLibrary` entry; a
derivative-based tangent frame in the editor lit shader behind
`u_HasNormalMap`.

**Spec:** `docs/superpowers/specs/2026-09-30-material-normal-mapping-design.md`

## Global Constraints

- `u_HasNormalMap == 0` must render byte-identically to before (captures).
- Normal map convention: tangent space, +Y along +v, packed `n * 0.5 + 0.5`.
- Normal map on texture slot 1; albedo stays on slot 0.
- Checks live in a `TEMPORARY` self-test, deleted at the end.

## Review Focus

1. A linked material whose Normal output is absent and Height absent: flat
   normal (0.5,0.5,1) — renders like no normal map, within quantisation.
2. A mesh unlinked after being linked: `u_HasNormalMap` returns to 0.
3. A resolution change during live editing: the normal texture is recreated
   at the new size, not `SetData`'d across it.

---

### Task 1: Baselines, then MaterialLibrary normal textures

- [ ] Capture baselines (unlinked editor scene, Cube3D) before any change.
- [ ] Self-test: `MaterialLibrary::Normal(path)` non-null for a valid file,
      null for a missing one; after a live edit at 512 its width is 512; a
      graph with neither Normal nor Height gives a flat-normal texture.
- [ ] Implement `Entry::Normal`, `Normal(path)`, upload in load + `NotifyEdited`.
- [ ] Commit.

### Task 2: Shader and binding

- [ ] Add `u_NormalMap`/`u_HasNormalMap` and the cotangent frame; bind per
      entity every frame (set 0 when unlinked).
- [ ] Byte-identical captures vs baselines.
- [ ] Hand-computed tilted-normal pixel; flat-normal match; opposite tilt.
- [ ] Stone capture under a raking light.
- [ ] Three-config build; commit.

### Task 3: Finish

- [ ] Delete the self-test; grep `TEMPORARY`.
- [ ] Changelog, STATE one-liner, roadmap #5/#11.
- [ ] Commit.
