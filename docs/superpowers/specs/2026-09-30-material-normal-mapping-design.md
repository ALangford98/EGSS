# Normal mapping for linked procedural materials

**Status:** approved 2026-09-30
**Scope:** piece 2 of 3 of wishlist #11 (procedural materials), which is also
the editor's half of wishlist #5 (normal maps). Piece 1 is
`2026-09-30-procedural-material-graph-design.md`.

## Problem

The material graph exports a tangent-space normal map (OpenGL convention,
+Y along +v), and `MaterialLibrary` links materials to scene meshes, but only
albedo is rendered. A stone material looks flat.

## Decision: tangent frame from screen-space derivatives

Chosen over per-vertex tangents. `MeshVertex` has no tangent, and adding one
changes the vertex format of every mesh in the engine — ~30 producers, the
terrain and planet chunks among them — for a feature only the editor shader
uses. Instead the fragment shader rebuilds the frame per pixel from
`dFdx`/`dFdy` of world position and UV (Schüler's cotangent frame):

```
dp1 = dFdx(p);  dp2 = dFdy(p);  duv1 = dFdx(uv);  duv2 = dFdy(uv)
dp2perp = cross(dp2, N);  dp1perp = cross(N, dp1)
T = dp2perp * duv1.x + dp1perp * duv2.x
B = dp2perp * duv1.y + dp1perp * duv2.y
invmax = inversesqrt(max(dot(T,T), dot(B,B)))
N' = normalize(mat3(T*invmax, B*invmax, N) * (sample * 2 - 1))
```

T and B point along increasing u and v on the surface, so mirrored UVs are
handled by the derivatives' own sign. **Known cost:** T and B are constant
across a triangle, so on low-poly curved meshes the bump direction can shift
slightly at triangle edges; flat surfaces are exact. Per-vertex tangents
remain the upgrade if normal maps spread beyond the editor.

## Design

- **MaterialLibrary** entries hold a second texture, the graph's Normal output
  (wired, or derived from Height at `kDefaultNormalStrength`). `Albedo(path)`
  keeps its signature; a new `Normal(path)` returns the normal texture (null
  when unavailable). The panel's live `NotifyEdited` uploads both.
- **Editor lit shader** gains `u_NormalMap` (slot 1) and `u_HasNormalMap`. With
  the flag 0 the arithmetic is exactly today's `normalize(v_Normal)`.
- Per entity, every frame, `u_HasNormalMap` is set both ways (instances keep
  stale values otherwise), as `u_HasAlbedoMap` already is.
- No scene-format change.

## Out of scope

Per-vertex tangents; normal maps from imported `.mtl`/glTF; roughness (piece 3).

## Verification

- Byte-identical `--hide-ui --lockstep` captures of an unlinked editor scene
  and of Cube3D against baselines taken before the change.
- A wall facing the camera, linked to a graph whose Normal output is a
  constant tangent-space tilt, one light at a known position, no specular at
  the centre (checked): the centre pixel matches `ambient + Lambert` computed
  by hand from the tilted normal, attenuation and light colour. A flat
  normal matches the unlinked wall within 8-bit quantisation. The opposite
  tilt darkens where this one brightens (handedness).
- A capture of the stone material under a raking light reads as grooved.
- All three configs build.
