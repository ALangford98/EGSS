# Building 2D lighting, yourself

A staged plan for a 2D map with a light-emitting square whose light is blocked
by obstacles. Raycasting is the right tool — the ray fan in
`TestEnv/src/Physics2D.h` is already most of the algorithm.

Each stage stands on its own. Stop at any point and you still have something
that works.

---

## The one thing the engine is missing

`Renderer2D` draws **quads** and **lines**. A light cone is a **triangle fan**,
and there is no triangle primitive yet. So either:

- **Stage 3** adds `DrawTriangle` (recommended — it's the real fix, and it's a
  copy of the line batch you already have), or
- you approximate the cone with lines and thin quads and never touch the
  engine.

Start with the approximation, because it gets you a picture in an hour.

---

## Stage 0 — orientation (30 minutes)

1. Read **`docs/ENGINE.md`**, especially "The call path" and the five decisions.
2. Run the sandbox, switch demos with the **Demos** panel or F1.
3. In `TestEnv/src/Physics2D.h`, change `m_RayCount` and drag the "Ray origin"
   sliders. That ray fan is the thing you are about to turn into a light.

**Change numbers before you write any.** Grep for `TRY:` in `Breakout.h` and
`Cube3D.h` for suggestions.

---

## Stage 1 — a map (about an hour)

Make a new demo layer. Copy `TestEnv/src/Physics2D.h` to `Lighting.h` and cut it
right back: keep the class shell, the camera, `OnUpdate`, `OnImGuiRender`.

Register it:

1. `TestEnv/src/Demo.h` — add `Lighting` to the enum **before** `Count`, add a
   matching string to `s_DemoNames` in the same order (a `static_assert`
   catches a mismatch), add a case to `ShortName` in `DemoSelector.h`.
2. `TestEnv/src/TestApp.cpp` — `#include "Lighting.h"` and `PushLayer(new Lighting());`
3. **Run `./BuildProject.sh`** — premake expands file globs at generation time,
   so a new file is invisible until you regenerate. This will catch you out at
   least once.

Then build the map. Use the physics world purely as a spatial database — you
never have to call `Step`:

```cpp
m_World.Clear();
m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 0.5f, 0.2f }, { 0.20f, 0.06f }));
m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ -0.6f, -0.3f }, { 0.08f, 0.25f }));
// ...a dozen or so, scattered
```

Draw them with `Renderer2D::DrawQuad(body.Position, body.HalfExtents * 2.0f, colour)`.

**Milestone:** obstacles on screen, one draw call.

---

## Stage 2 — rays that stop at walls (about an hour)

Straight from `DrawRayFan` in `Physics2D.h`:

```cpp
for (int i = 0; i < m_RayCount; i++)
{
    float angle = (float)i / (float)m_RayCount * glm::two_pi<float>();
    glm::vec2 direction = { std::cos(angle), std::sin(angle) };

    Egss::RaycastHit hit = m_World.Raycast(m_LightPosition, direction, m_LightRadius);
    glm::vec2 end = hit.Hit ? hit.Point : m_LightPosition + direction * m_LightRadius;

    Egss::Renderer2D::DrawLine(m_LightPosition, end, lightColour);
}
```

**Milestone:** a starburst that stops at obstacles. Already recognisably light
and shadow. Push `m_RayCount` to 500 and it nearly looks solid.

Two things to notice:
- Every ray is one `Raycast`, and each tests **every body** — O(rays × bodies).
  Watch `Physics::Raycast` in the Profiler panel as you raise the count.
- The fan is regular, so edges shimmer as things move. Stage 4 fixes that
  properly.

---

## Stage 3 — `DrawTriangle` — **DONE, it's in the engine already**

Read `Renderer2D.cpp` and compare `FlushTriangles` with `FlushLines` — they are
the same shape, and both share `LineShader` because position+colour is all
either needs. What you can call now:

```cpp
Renderer2D::DrawTriangle(a, b, c, colour);                   // vec2 or vec3
Renderer2D::DrawTriangle(a, b, c, colA, colB, colC);         // per-corner colour
```

`Statistics::TriangleCount` counts them, and they cost **one** draw call
however many you submit.

The per-corner overload is the one you want for a light: bright at the centre
vertex, dim at the two outer ones, and the falloff comes free from the
hardware interpolating between them.

Verified: a right triangle with corners at world (0,0), (0.4,0), (0,0.4)
covered a 159x159 pixel box at exactly 50% fill — which is what a right
triangle should do.

Go straight to Stage 4.

## Stage 3 (reference) — how it was added

This is the interesting one, and it's a near-copy of the line batch. Read
`Renderer2D.cpp` and find everything with `Line` in the name — you are adding a
parallel set with `Triangle`.

**Backend** (`Egss/Renderer/RendererAPI.h`, `RenderCommand.h`,
`Platform/OpenGL/OpenGLRendererAPI.{h,cpp}`):

```cpp
// Alongside DrawLines
virtual void DrawTriangles(const std::shared_ptr<VertexArray>& vertexArray,
                           unsigned int vertexCount) = 0;

// OpenGL implementation — identical to DrawLines but GL_TRIANGLES
void OpenGLRendererAPI::DrawTriangles(const std::shared_ptr<VertexArray>& va, unsigned int count)
{
    va->Bind();
    glDrawArrays(GL_TRIANGLES, 0, count);
}
```

**Renderer2D** (`Egss/Renderer/Renderer2D.{h,cpp}`):

1. `TriangleVertex { glm::vec3 Position; glm::vec4 Color; }` — same layout as
   `LineVertex`, so **reuse `s_Data.LineShader`**. No new shader needed.
2. In `Renderer2DData`: `TriangleVertexArray`, `TriangleVertexBuffer`,
   `TriangleVertexCount`, `TriangleVertexBufferBase/Ptr`, and a
   `MaxTriangleVertices`.
3. In `Init`, copy the line batch's setup verbatim, changing the names.
4. Reset the counters in `StartBatch`.
5. Add `FlushTriangles()` next to `FlushLines()`, and call it from `Flush()`.
6. Public: `DrawTriangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec4& colour)`.
7. Add `TriangleCount` to `Statistics`.

**Two gotchas that will bite:**

- **Depth.** Depth testing is on, higher `z` is *nearer*, and at equal `z` the
  **first** thing drawn wins — not the last. Give the light a higher `z` than
  the map or it will not appear. See the note in `docs/ENGINE.md`.
- **Blending.** `OpenGLRendererAPI::Init` sets
  `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`. Light usually wants
  *additive* (`GL_ONE, GL_ONE`) so overlapping lights brighten. Either add a
  `RenderCommand::SetBlendMode`, or accept alpha blending for now — it works,
  it just doesn't accumulate.

**Verify it before building on it.** Draw one triangle with known corners and
check it covers the pixels you expect. A wrong winding or a stale
`glDrawArrays` count is much easier to find now than inside the light code.

---

## Stage 4 — a real visibility polygon (about two hours)

The regular fan wastes rays and shimmers. The classic algorithm casts rays
**only at obstacle corners**, which is exact and usually needs far fewer rays.

```
1. Collect angles:
     for each body, for each of its 4 corners:
         angle = atan2(corner.y - light.y, corner.x - light.x)
         push angle, angle - 0.0001, angle + 0.0001
2. Sort the angles.
3. Raycast along each; record the hit point (or the radius limit on a miss).
4. Walk the sorted hits and emit a triangle per adjacent pair:
     DrawTriangle(lightPos, hit[i], hit[i+1], colour)
   ...including the wrap from last back to first.
```

**Why the ±0.0001 matters.** A ray aimed exactly at a corner hits the corner
and stops. The two nudged rays slip *past* it on either side and travel on to
whatever is behind — which is what draws the shadow's edge. Without them you
get spikes and gaps, and it will look broken in a way that is very hard to
diagnose. Do not skip it.

Box corners from a `RigidBody2D`:

```cpp
glm::vec2 c[4] = {
    body.Position + glm::vec2(-body.HalfExtents.x, -body.HalfExtents.y),
    body.Position + glm::vec2( body.HalfExtents.x, -body.HalfExtents.y),
    body.Position + glm::vec2( body.HalfExtents.x,  body.HalfExtents.y),
    body.Position + glm::vec2(-body.HalfExtents.x,  body.HalfExtents.y)
};
```

Circles have no corners — either skip them, keep everything rectangular, or
approximate each circle with 8 sample angles.

**Milestone:** crisp shadows with maybe 60 rays instead of 500, and no
shimmer. Compare the ray count and the `Physics::Raycast` timing against
Stage 2 — that comparison is the whole point.

---

## Stage 5 — things to play with

- **Falloff.** Fade the triangle colour with distance from the light. Per-vertex
  colour already exists, so give the outer two vertices a dimmer colour than
  the centre.
- **Several lights.** Different colours, additive blending. Now you want the
  blend-mode change from Stage 3.
- **Soft edges.** Cast three rays a degree or two apart and average — the same
  trick the audio occlusion uses to grade its answer.
- **A moving light.** Attach it to the mouse. `Input::GetMousePosition` is in
  window coordinates; you need the inverse of the camera's view-projection to
  get world coordinates. Fiddly, and worth doing once.
- **Make it a game.** Light reveals, shadow hides. That is a whole genre.
- **Sound too.** You already have `SetVoiceOcclusion` and `Raycast` — a source
  in shadow could be muffled by the same rays that darken it.

---

## Working habits that paid off here

- **Regenerate after adding files.** `./BuildProject.sh`. The symptom is a
  linker error about a symbol that is obviously right there.
- **Check numbers, not just pictures.** Nearly every real bug in this project
  was found by printing a value and comparing it to one worked out by hand —
  and several apparent bugs turned out to be a wrong expectation instead. When
  something looks wrong, compute what it *should* be before changing code.
- **Suspect the measurement.** Twice a feature looked completely broken and
  was fine: the raycast test measured a window shorter than the shortest comb
  delay, and an audio test rendered fewer frames than the smoothing needed.
- **The Profiler panel is the only honest timing.** VSync makes every frame
  read ~16.7ms regardless. And build **Release** before drawing conclusions:
  physics was 21x slower in Debug, the broadphase 110x.
- **One draw call is the goal.** Watch the counter. If it climbs, something
  broke batching — usually a texture change or a new primitive type.
