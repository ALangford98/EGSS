# Mesh UV Template Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the editor a way to export a paintable, chart-packed UV
template PNG for any placed mesh -- computing a fresh heuristic unwrap when
the mesh has no usable UVs, or using its existing UVs as-is otherwise -- via
one Inspector button next to "Edit Mesh".

**Architecture:** Two new, rendering-agnostic `GS::` classes operating on
`MeshData`: `UvUnwrap` (usability check, chart-growing unwrap, UV-island
detection) and `UvTemplateWriter` (rasterizes packed UVs into a wireframe +
tinted-fill PNG via the already-vendored `stb_image_write`). A small editor
wiring layer in `EditorSceneView.h` extracts the mesh-loading half of the
existing `StartMeshEdit` into a shared helper, adds an `ExportUvTemplate`
function that both a self-test and a new "Export UV Template" button call,
and re-saves the mesh's `.obj` (via the existing `ObjWriter`) only when a
fresh unwrap actually changed its UVs.

**Tech Stack:** C++17, GLM (already used throughout `GS::Renderer`),
`stb_image_write`/`stb_image` (already vendored in `GS/vendor/stb_image`,
already on `GS`'s premake `includedirs`). No new dependency.

**Spec:** `docs/superpowers/specs/2026-09-14-mesh-uv-template-export-design.md`

## Global Constraints

- Heuristic (non-conformal) unwrapping only -- no LSCM/sparse-solver work in
  this plan, per the spec's "Why this shape" section.
- `UvUnwrap::kChartAngleThresholdDegrees` is a single tuned `constexpr`
  constant, not user-exposed via any UI control in this plan.
- No new vendored dependency -- `UvTemplateWriter` reuses `stb_image_write`
  exactly the way `GS/src/GS/Debug/ScreenCapture.cpp` already does; do not
  add a second image-writing path.
- A mesh reload after a fresh unwrap must bypass `MeshCache::Get` and build
  `GS::Mesh` directly from the just-written `MeshData`, mirroring
  `EditorSceneView.h`'s existing "Edit Mesh" Done handler -- `MeshCache`
  caches one mesh per path forever, so re-exporting the same path twice
  would otherwise silently show the first export's stale geometry.
- `UvUnwrap::Unwrap` always rebuilds the mesh with one fresh, non-shared
  vertex per triangle corner and clears any pre-existing `Submeshes` (the
  chart-order rewrite invalidates old per-submesh index ranges; `GS::Mesh`'s
  constructor already auto-fills one default full-range submesh from an
  empty list -- see `Mesh.cpp:83-87` -- so this is safe, not a regression).
- Tests follow this project's self-test pattern: temporary `TEMPORARY --
  delete after verifying X` headers with `Check`/`Run`, wired into
  `TestApp.cpp`'s constructor (or, where a live scene/GL context is
  required, into `EditorSceneView::OnAttach`), deleted once verified --
  there is no test framework and the project does not want one.
- Explicitly out of scope in this plan (per the spec): manual seam editing,
  chart relaxation/distortion minimization, an interactive preview/undo
  loop for this feature, texture re-import, performance work for very
  dense meshes.

---

## Task 1: `GS::UvUnwrap` -- `HasUsableUVs`

**Files:**
- Create: `GS/src/GS/Renderer/UvUnwrap.h`
- Create: `GS/src/GS/Renderer/UvUnwrap.cpp`
- Modify: `GS/src/GS.h` (add `#include "GS/Renderer/UvUnwrap.h"` after
  `#include "GS/Renderer/ObjWriter.h"`, matching how every other public
  `GS/Renderer` header is aggregated there)
- Test: `TestEnv/src/UvUnwrapTest.h` (temporary)

**Interfaces:**
- Produces: `GS::UvUnwrap::HasUsableUVs(const GS::MeshData&) -> bool`. Also
  declares (but does not yet implement -- Tasks 2/3 fill these in) the
  class's other two public members and the `ChartAssignment` alias, so the
  header matches its final shape from this task onward and later tasks are
  pure additions, not header churn:
  `using ChartAssignment = std::vector<int>;`,
  `static ChartAssignment Unwrap(MeshData& data);`,
  `static ChartAssignment FindIslandsFromUVs(const MeshData& data);`.

- [ ] **Step 1: Capture the pre-change baseline regression capture**

Before touching any code, build and capture Cube3D's own known-good
`--hide-ui` frame, so Task 6 has something to diff against later. Run from
the repo root:

```sh
./gs.py build
mkdir -p /tmp/uv-export-verification
cd bin/Debug-linux-x86_64/TestEnv
./TestEnv --demo Cube3D --lockstep --hide-ui --hide-window \
    --capture /tmp/uv-export-verification/cube3d_baseline.png --capture-step 5
cd -
```

- [ ] **Step 2: Write the failing test**

```cpp
// TestEnv/src/UvUnwrapTest.h
// TEMPORARY -- delete after verifying UvUnwrap's HasUsableUVs, Unwrap, and
// FindIslandsFromUVs against known primitives and hand-built meshes.
#pragma once
#include <GS.h>
#include <GS/Renderer/UvUnwrap.h>
#include <GS/Renderer/Mesh.h>

namespace UvUnwrapTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		// CreateCubeData() already assigns a real per-face [0,1]^2 UV
		// square to every face (Mesh.cpp's own CreateCubeData) -- that is
		// usable as-is, not the degenerate case.
		GS::MeshData cube = GS::Mesh::CreateCubeData();
		Check(GS::UvUnwrap::HasUsableUVs(cube), "CreateCubeData's own per-face UVs are usable");

		// A hand-zeroed copy matches EditableMesh::Rebuild()'s own known,
		// disclosed gap (every point gets UV (0,0)) -- HasUsableUVs must
		// treat that the same as "never given UVs at all".
		GS::MeshData zeroed = cube;
		for (auto& v : zeroed.Vertices)
			v.TexCoord = { 0.0f, 0.0f };
		Check(!GS::UvUnwrap::HasUsableUVs(zeroed), "all-(0,0) UVs (Rebuild()'s known gap) are correctly flagged unusable");

		GS::MeshData empty;
		Check(!GS::UvUnwrap::HasUsableUVs(empty), "a mesh with no vertices is correctly flagged unusable");

		GS_TRACE("UvUnwrapTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it into `TestEnv/src/TestApp.cpp`'s constructor: add
`#include "UvUnwrapTest.h"` near the other includes and
`UvUnwrapTest::Run(); // TEMPORARY -- see UvUnwrapTest.h` in the
constructor body, right after any existing temporary test calls (or at the
end if none remain).

- [ ] **Step 3: Run to verify it fails**

Run: `./gs.py build` -- expected: fails to compile (`GS/Renderer/UvUnwrap.h`
does not exist yet).

- [ ] **Step 4: Write the header (full public shape) and the `HasUsableUVs` implementation**

```cpp
// GS/src/GS/Renderer/UvUnwrap.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

namespace GS {

	// Heuristic (non-conformal) UV unwrapping for a mesh with no usable
	// UVs, plus UV-island detection for a mesh whose UVs are kept as-is.
	// LSCM-style conformal parameterization was deliberately ruled out --
	// see docs/superpowers/specs/2026-09-14-mesh-uv-template-export-design.md
	// ("Why this shape") -- this engine has no sparse-linear-algebra
	// foundation to build one on.
	class GS_API UvUnwrap
	{
	public:
		// One entry per triangle (data.Indices.size() / 3), in the same
		// order as whatever MeshData it was produced against.
		using ChartAssignment = std::vector<int>;

		// All-zero (or near-zero UV-bounding-box area) UVs -- the output
		// of a mesh that was never textured, or of
		// EditableMesh::Rebuild()'s known (0,0)-everywhere gap. Either
		// way, not usable as a template source.
		static bool HasUsableUVs(const MeshData& data);

		// Charts `data` by triangle-normal similarity, planar-projects
		// and packs each chart into [0,1]^2, and rewrites `data` with one
		// fresh, non-shared vertex per triangle corner -- a chart seam
		// needs its own UV on each side, the same reason this engine
		// already duplicates vertices per face for flat normals (see
		// Mesh::CreateCubeData). Clears data.Submeshes/MaterialLibraries
		// (see Global Constraints). Returns the chart id of each
		// resulting triangle, in the same order as the rewritten
		// data.Indices.
		static ChartAssignment Unwrap(MeshData& data);

		// Read-only counterpart for a mesh whose existing UVs are kept:
		// two triangles sharing a mesh edge are the same island only when
		// their corresponding UV-space edge endpoints match too. No
		// mutation, no geometry decisions -- just reports where the
		// existing seams already are.
		static ChartAssignment FindIslandsFromUVs(const MeshData& data);

		// The angle threshold Unwrap()'s chart growth uses, tuned
		// empirically in Task 2 against CreateCubeData()/
		// CreateSphereData()/CreateCylinderData() -- not user-exposed.
		static constexpr float kChartAngleThresholdDegrees = 45.0f;

	private:
		struct Chart
		{
			std::vector<int> Triangles;
			glm::vec3 NormalSum{ 0.0f };
		};

		static std::vector<glm::vec3> ComputeTriangleNormals(const MeshData& data);
		static std::vector<std::vector<int>> BuildTriangleAdjacency(const MeshData& data);
		static std::vector<Chart> GrowCharts(const std::vector<glm::vec3>& triangleNormals,
			const std::vector<std::vector<int>>& adjacency);
		static void BuildOrthonormalBasis(const glm::vec3& normal, glm::vec3& outTangent, glm::vec3& outBitangent);
	};

}
```

```cpp
// GS/src/GS/Renderer/UvUnwrap.cpp
#include "gspch.h"
#include "GS/Renderer/UvUnwrap.h"

#include <limits>

namespace GS {

	bool UvUnwrap::HasUsableUVs(const MeshData& data)
	{
		if (data.Vertices.empty())
			return false;

		glm::vec2 uvMin(std::numeric_limits<float>::max());
		glm::vec2 uvMax(-std::numeric_limits<float>::max());
		for (const MeshVertex& v : data.Vertices)
		{
			uvMin = glm::min(uvMin, v.TexCoord);
			uvMax = glm::max(uvMax, v.TexCoord);
		}

		float area = (uvMax.x - uvMin.x) * (uvMax.y - uvMin.y);
		return area > 1e-6f;
	}

}
```

Leave `Unwrap`, `FindIslandsFromUVs`, and the private helpers undefined for
now -- nothing calls them yet, so the file links fine with only
`HasUsableUVs` implemented. Add the `GS.h` include:

```cpp
// GS/src/GS.h -- add immediately after the existing ObjWriter include
#include "GS/Renderer/ObjWriter.h"
#include "GS/Renderer/UvUnwrap.h"
```

- [ ] **Step 5: Run to verify it passes**

Run: `./gs.py build && ./gs.py run` from the repo root, then check the log
for `UvUnwrapTest: 3 passed, 0 failed`.

- [ ] **Step 6: Commit**

```bash
git add GS/src/GS/Renderer/UvUnwrap.h GS/src/GS/Renderer/UvUnwrap.cpp \
    GS/src/GS.h TestEnv/src/UvUnwrapTest.h TestEnv/src/TestApp.cpp
git commit -m "Add GS::UvUnwrap::HasUsableUVs"
```

---

## Task 2: `GS::UvUnwrap::Unwrap` -- chart growing, projection, packing

**Files:**
- Modify: `GS/src/GS/Renderer/UvUnwrap.cpp` (implement `Unwrap`,
  `ComputeTriangleNormals`, `BuildTriangleAdjacency`, `GrowCharts`,
  `BuildOrthonormalBasis`)
- Modify: `TestEnv/src/UvUnwrapTest.h` (add `Unwrap` checks to `Run()`)

**Interfaces:**
- Consumes: `GS::UvUnwrap::ChartAssignment` (Task 1), `GS::MeshData`/
  `GS::MeshVertex` (`GS/src/GS/Renderer/Mesh.h`, existing).
- Produces: `GS::UvUnwrap::Unwrap(MeshData&) -> ChartAssignment`, fully
  implemented -- this is what `FindIslandsFromUVs` (Task 3) and
  `UvTemplateWriter::Write` (Task 4) and the editor wiring (Task 5) all
  build on.

- [ ] **Step 1: Extend the failing test**

Add to `TestEnv/src/UvUnwrapTest.h`'s `Run()`, before the final
`GS_TRACE` line (and add `#include <unordered_set>` and
`#include <utility>` near the top of the file):

```cpp
		// Cube: 6 faces, 90 degrees apart -- well outside any reasonable
		// chart angle threshold, so this must always produce exactly 6
		// charts.
		{
			GS::MeshData cubeToUnwrap = GS::Mesh::CreateCubeData();
			auto charts = GS::UvUnwrap::Unwrap(cubeToUnwrap);

			std::unordered_set<int> distinctCharts(charts.begin(), charts.end());
			Check(distinctCharts.size() == 6, "cube unwraps to exactly 6 charts");

			bool allInBounds = true;
			for (auto& v : cubeToUnwrap.Vertices)
				if (v.TexCoord.x < -1e-4f || v.TexCoord.x > 1.0001f ||
					v.TexCoord.y < -1e-4f || v.TexCoord.y > 1.0001f)
					allInBounds = false;
			Check(allInBounds, "cube's packed UVs all land inside [0,1]^2");

			std::vector<glm::vec2> chartMin(6, glm::vec2(1e9f)), chartMax(6, glm::vec2(-1e9f));
			for (size_t t = 0; t < charts.size(); t++)
			{
				int c = charts[t];
				for (int k = 0; k < 3; k++)
				{
					glm::vec2 uv = cubeToUnwrap.Vertices[cubeToUnwrap.Indices[t * 3 + k]].TexCoord;
					chartMin[c] = glm::min(chartMin[c], uv);
					chartMax[c] = glm::max(chartMax[c], uv);
				}
			}
			auto overlaps = [](glm::vec2 aMin, glm::vec2 aMax, glm::vec2 bMin, glm::vec2 bMax) {
				return aMin.x < bMax.x && aMax.x > bMin.x && aMin.y < bMax.y && aMax.y > bMin.y;
			};
			bool anyOverlap = false;
			for (int i = 0; i < 6 && !anyOverlap; i++)
				for (int j = i + 1; j < 6; j++)
					if (overlaps(chartMin[i], chartMax[i], chartMin[j], chartMax[j]))
						anyOverlap = true;
			Check(!anyOverlap, "cube's 6 packed charts do not overlap");
		}

		// A single flat quad (2 coplanar triangles) must never be split.
		{
			GS::MeshData quad;
			quad.Vertices = {
				{ {0,0,0}, {0,0,1}, {0,0} },
				{ {1,0,0}, {0,0,1}, {0,0} },
				{ {1,1,0}, {0,0,1}, {0,0} },
				{ {0,1,0}, {0,0,1}, {0,0} },
			};
			quad.Indices = { 0,1,2, 2,3,0 };
			auto charts = GS::UvUnwrap::Unwrap(quad);
			std::unordered_set<int> distinctCharts(charts.begin(), charts.end());
			Check(distinctCharts.size() == 1, "a single flat quad unwraps to exactly 1 chart");
		}

		// Sphere/cylinder: report the actual chart counts the tuned angle
		// threshold produces -- this is the empirical check the constant
		// is picked from, not an assumption it lands in a good range.
		{
			GS::MeshData sphere = GS::Mesh::CreateSphereData();
			size_t sphereTriCount = sphere.TriangleCount();
			auto sphereCharts = GS::UvUnwrap::Unwrap(sphere);
			std::unordered_set<int> distinctSphere(sphereCharts.begin(), sphereCharts.end());
			GS_TRACE("UvUnwrapTest: sphere ({0} triangles) -> {1} charts", sphereTriCount, distinctSphere.size());
			Check(distinctSphere.size() > 1 && distinctSphere.size() < sphereTriCount / 2,
				"sphere charts into more than 1 and noticeably fewer than one-per-triangle");

			GS::MeshData cylinder = GS::Mesh::CreateCylinderData();
			size_t cylinderTriCount = cylinder.TriangleCount();
			auto cylinderCharts = GS::UvUnwrap::Unwrap(cylinder);
			std::unordered_set<int> distinctCylinder(cylinderCharts.begin(), cylinderCharts.end());
			GS_TRACE("UvUnwrapTest: cylinder ({0} triangles) -> {1} charts", cylinderTriCount, distinctCylinder.size());
			Check(distinctCylinder.size() > 1 && distinctCylinder.size() < cylinderTriCount / 2,
				"cylinder charts into more than 1 and noticeably fewer than one-per-triangle");
		}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./gs.py build` -- expected: link error, `Unwrap` is declared but not
defined.

- [ ] **Step 3: Implement the private helpers and `Unwrap`**

```cpp
// GS/src/GS/Renderer/UvUnwrap.cpp -- add after HasUsableUVs, still inside namespace GS
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace {

	// Position-keyed, not index-keyed -- flat-shaded meshes duplicate a
	// vertex per face for its own normal, so two triangles that are
	// geometrically adjacent almost never share a vertex *index*, only a
	// vertex *position* (EditableMesh's own welding hits this same fact
	// for the same reason).
	struct EdgeKey
	{
		glm::vec3 A, B;
		bool operator==(const EdgeKey& other) const { return A == other.A && B == other.B; }
	};

	struct EdgeKeyHash
	{
		size_t operator()(const EdgeKey& key) const
		{
			auto h = [](float f) { return std::hash<float>{}(f); };
			size_t seed = 0;
			for (float f : { key.A.x, key.A.y, key.A.z, key.B.x, key.B.y, key.B.z })
				seed ^= h(f) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
			return seed;
		}
	};

	// Order-independent -- the same edge walked from either triangle must
	// hash the same.
	EdgeKey MakeEdgeKey(const glm::vec3& a, const glm::vec3& b)
	{
		if (a.x < b.x || (a.x == b.x && (a.y < b.y || (a.y == b.y && a.z < b.z))))
			return { a, b };
		return { b, a };
	}

}

namespace GS {

	std::vector<glm::vec3> UvUnwrap::ComputeTriangleNormals(const MeshData& data)
	{
		size_t triCount = data.TriangleCount();
		std::vector<glm::vec3> normals(triCount, glm::vec3(0.0f, 0.0f, 1.0f));
		for (size_t t = 0; t < triCount; t++)
		{
			const glm::vec3& a = data.Vertices[data.Indices[t * 3 + 0]].Position;
			const glm::vec3& b = data.Vertices[data.Indices[t * 3 + 1]].Position;
			const glm::vec3& c = data.Vertices[data.Indices[t * 3 + 2]].Position;
			glm::vec3 cross = glm::cross(b - a, c - a);
			float length = glm::length(cross);
			if (length > 1e-8f)   // same degenerate-triangle guard RecalculateNormalsTouching uses (EditableMesh.h)
				normals[t] = cross / length;
		}
		return normals;
	}

	std::vector<std::vector<int>> UvUnwrap::BuildTriangleAdjacency(const MeshData& data)
	{
		size_t triCount = data.TriangleCount();
		std::unordered_map<EdgeKey, std::vector<int>, EdgeKeyHash> edgeToTriangles;
		for (size_t t = 0; t < triCount; t++)
		{
			const glm::vec3& p0 = data.Vertices[data.Indices[t * 3 + 0]].Position;
			const glm::vec3& p1 = data.Vertices[data.Indices[t * 3 + 1]].Position;
			const glm::vec3& p2 = data.Vertices[data.Indices[t * 3 + 2]].Position;
			edgeToTriangles[MakeEdgeKey(p0, p1)].push_back((int)t);
			edgeToTriangles[MakeEdgeKey(p1, p2)].push_back((int)t);
			edgeToTriangles[MakeEdgeKey(p2, p0)].push_back((int)t);
		}

		std::vector<std::vector<int>> adjacency(triCount);
		for (auto& [key, tris] : edgeToTriangles)
			for (size_t i = 0; i < tris.size(); i++)
				for (size_t j = 0; j < tris.size(); j++)
					if (i != j)
						adjacency[tris[i]].push_back(tris[j]);
		return adjacency;
	}

	std::vector<UvUnwrap::Chart> UvUnwrap::GrowCharts(const std::vector<glm::vec3>& triangleNormals,
		const std::vector<std::vector<int>>& adjacency)
	{
		size_t triCount = triangleNormals.size();
		std::vector<bool> visited(triCount, false);
		std::vector<Chart> charts;
		float thresholdCos = std::cos(glm::radians(kChartAngleThresholdDegrees));

		for (size_t seed = 0; seed < triCount; seed++)
		{
			if (visited[seed])
				continue;

			Chart chart;
			std::vector<int> queue{ (int)seed };
			visited[seed] = true;

			while (!queue.empty())
			{
				int t = queue.back();
				queue.pop_back();
				chart.Triangles.push_back(t);
				chart.NormalSum += triangleNormals[t];

				glm::vec3 chartNormal = glm::normalize(chart.NormalSum);
				for (int neighbor : adjacency[t])
				{
					if (visited[neighbor])
						continue;
					float cosAngle = glm::clamp(glm::dot(triangleNormals[neighbor], chartNormal), -1.0f, 1.0f);
					if (cosAngle >= thresholdCos)
					{
						visited[neighbor] = true;
						queue.push_back(neighbor);
					}
				}
			}

			charts.push_back(std::move(chart));
		}

		return charts;
	}

	void UvUnwrap::BuildOrthonormalBasis(const glm::vec3& normal, glm::vec3& outTangent, glm::vec3& outBitangent)
	{
		glm::vec3 up = (std::abs(normal.y) > 0.99f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
		outTangent = glm::normalize(glm::cross(up, normal));
		outBitangent = glm::cross(normal, outTangent);
	}

	UvUnwrap::ChartAssignment UvUnwrap::Unwrap(MeshData& data)
	{
		std::vector<glm::vec3> triangleNormals = ComputeTriangleNormals(data);
		std::vector<std::vector<int>> adjacency = BuildTriangleAdjacency(data);
		std::vector<Chart> charts = GrowCharts(triangleNormals, adjacency);

		struct Bounds { glm::vec2 Min, Max; };
		std::vector<Bounds> localBounds(charts.size());
		std::vector<std::array<glm::vec2, 3>> cornerUV(triangleNormals.size());

		for (size_t c = 0; c < charts.size(); c++)
		{
			glm::vec3 chartNormal = glm::normalize(charts[c].NormalSum);
			glm::vec3 tangent, bitangent;
			BuildOrthonormalBasis(chartNormal, tangent, bitangent);

			glm::vec3 origin = data.Vertices[data.Indices[charts[c].Triangles[0] * 3]].Position;
			glm::vec2 minB(std::numeric_limits<float>::max());
			glm::vec2 maxB(-std::numeric_limits<float>::max());
			for (int t : charts[c].Triangles)
			{
				for (int k = 0; k < 3; k++)
				{
					const glm::vec3& pos = data.Vertices[data.Indices[t * 3 + k]].Position;
					glm::vec2 local(glm::dot(pos - origin, tangent), glm::dot(pos - origin, bitangent));
					cornerUV[t][k] = local;
					minB = glm::min(minB, local);
					maxB = glm::max(maxB, local);
				}
			}
			localBounds[c] = { minB, maxB };
		}

		// Shelf-pack the charts' local bounding boxes, tallest first.
		std::vector<int> order(charts.size());
		for (size_t i = 0; i < order.size(); i++)
			order[i] = (int)i;
		std::sort(order.begin(), order.end(), [&](int a, int b) {
			return (localBounds[a].Max.y - localBounds[a].Min.y) > (localBounds[b].Max.y - localBounds[b].Min.y);
		});

		float totalArea = 0.0f;
		for (auto& b : localBounds)
			totalArea += (b.Max.x - b.Min.x) * (b.Max.y - b.Min.y);
		float canvasWidth = std::sqrt(std::max(totalArea, 1e-6f));
		float margin = canvasWidth * 0.02f;

		std::vector<glm::vec2> chartOffset(charts.size());
		float shelfX = margin, shelfY = margin, shelfHeight = 0.0f;
		float packWidth = 0.0f, packHeight = 0.0f;
		for (int c : order)
		{
			float w = localBounds[c].Max.x - localBounds[c].Min.x;
			float h = localBounds[c].Max.y - localBounds[c].Min.y;
			if (shelfX + w + margin > canvasWidth && shelfX > margin)
			{
				shelfY += shelfHeight + margin;
				shelfX = margin;
				shelfHeight = 0.0f;
			}
			chartOffset[c] = glm::vec2(shelfX, shelfY) - localBounds[c].Min;
			shelfX += w + margin;
			shelfHeight = std::max(shelfHeight, h);
			packWidth = std::max(packWidth, shelfX);
			packHeight = std::max(packHeight, shelfY + shelfHeight + margin);
		}

		float packExtent = std::max(packWidth, packHeight);
		float scale = packExtent > 1e-6f ? 1.0f / packExtent : 1.0f;
		glm::vec2 centerOffset((packExtent - packWidth) * 0.5f, (packExtent - packHeight) * 0.5f);

		// Rebuild with one fresh, non-shared vertex per triangle corner --
		// a chart seam needs its own UV on each side, and the pre-existing
		// buffer may share a vertex between triangles that end up in
		// different charts.
		MeshData rebuilt;
		rebuilt.Vertices.reserve(triangleNormals.size() * 3);
		rebuilt.Indices.reserve(triangleNormals.size() * 3);
		ChartAssignment chartIdPerTriangle(triangleNormals.size());

		for (size_t c = 0; c < charts.size(); c++)
		{
			for (int t : charts[c].Triangles)
			{
				chartIdPerTriangle[t] = (int)c;
				for (int k = 0; k < 3; k++)
				{
					unsigned int srcIndex = data.Indices[t * 3 + k];
					MeshVertex vertex = data.Vertices[srcIndex];
					vertex.TexCoord = (cornerUV[t][k] + chartOffset[c]) * scale + centerOffset;
					rebuilt.Indices.push_back((unsigned int)rebuilt.Vertices.size());
					rebuilt.Vertices.push_back(vertex);
				}
			}
		}

		// Chart-order reorders triangles, which invalidates any old
		// per-submesh index ranges -- clearing (not copying) is safe:
		// Mesh's constructor auto-fills one default full-range submesh
		// from an empty list (Mesh.cpp:83-87).
		rebuilt.RecalculateBounds();
		data = std::move(rebuilt);

		return chartIdPerTriangle;
	}

}
```

- [ ] **Step 4: Run to verify it passes**

Run: `./gs.py build && ./gs.py run`, check the log for
`UvUnwrapTest: 7 passed, 0 failed` and read the two `GS_TRACE` lines
reporting the sphere/cylinder chart counts.

- [ ] **Step 5: Tune the threshold against the actual measurement, if needed**

If either sphere or cylinder chart count printed in Step 4 looks wrong for
"a handful" (e.g. 1, or within a few of the triangle count), adjust
`kChartAngleThresholdDegrees` in `UvUnwrap.h` and repeat Steps 4-5 until
both report a plausible chart count -- report the final chosen value and
the measured counts, don't just accept whatever the first guess produced.

- [ ] **Step 6: Commit**

```bash
git add GS/src/GS/Renderer/UvUnwrap.h GS/src/GS/Renderer/UvUnwrap.cpp TestEnv/src/UvUnwrapTest.h
git commit -m "Implement GS::UvUnwrap::Unwrap (chart growth, projection, packing)"
```

---

## Task 3: `GS::UvUnwrap::FindIslandsFromUVs`

**Files:**
- Modify: `GS/src/GS/Renderer/UvUnwrap.cpp` (implement `FindIslandsFromUVs`)
- Modify: `TestEnv/src/UvUnwrapTest.h` (add island-detection checks)

**Interfaces:**
- Consumes: `BuildTriangleAdjacency` (Task 2, private to `UvUnwrap` but
  reusable within the same `.cpp`).
- Produces: `GS::UvUnwrap::FindIslandsFromUVs(const MeshData&) -> ChartAssignment`,
  used by the editor wiring (Task 5) whenever `HasUsableUVs` is true.

- [ ] **Step 1: Extend the failing test**

Add to `TestEnv/src/UvUnwrapTest.h`'s `Run()`, before the final
`GS_TRACE` line:

```cpp
		// Two UV-disjoint triangle pairs sharing no continuous UV edge
		// (each pair is its own quad, both quads placed far apart in UV
		// space) must report 2 islands; a mesh whose UVs are actually one
		// continuous chart (CreateCubeData()'s own single face, made of 2
		// triangles sharing an edge with matching UVs on both sides)
		// must report 1.
		{
			GS::MeshData twoIslands;
			twoIslands.Vertices = {
				{ {0,0,0}, {0,0,1}, {0.0f, 0.0f} },
				{ {1,0,0}, {0,0,1}, {0.1f, 0.0f} },
				{ {1,1,0}, {0,0,1}, {0.1f, 0.1f} },
				{ {0,1,0}, {0,0,1}, {0.0f, 0.1f} },
				{ {5,0,0}, {0,0,1}, {0.5f, 0.5f} },
				{ {6,0,0}, {0,0,1}, {0.6f, 0.5f} },
				{ {6,1,0}, {0,0,1}, {0.6f, 0.6f} },
				{ {5,1,0}, {0,0,1}, {0.5f, 0.6f} },
			};
			twoIslands.Indices = { 0,1,2, 2,3,0, 4,5,6, 6,7,4 };
			auto islands = GS::UvUnwrap::FindIslandsFromUVs(twoIslands);
			std::unordered_set<int> distinct(islands.begin(), islands.end());
			Check(distinct.size() == 2, "two UV-disjoint quads report 2 islands");

			GS::MeshData oneFace = GS::Mesh::CreateCubeData();
			// Keep only the first face's 2 triangles (indices 0..5), whose
			// 4 shared vertices already carry one continuous [0,1]^2 UV
			// square from CreateCubeData() itself.
			oneFace.Vertices.assign(oneFace.Vertices.begin(), oneFace.Vertices.begin() + 4);
			oneFace.Indices.assign(oneFace.Indices.begin(), oneFace.Indices.begin() + 6);
			auto oneIsland = GS::UvUnwrap::FindIslandsFromUVs(oneFace);
			std::unordered_set<int> distinctOne(oneIsland.begin(), oneIsland.end());
			Check(distinctOne.size() == 1, "one coplanar face's own 2 triangles report 1 island");
		}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./gs.py build` -- expected: link error, `FindIslandsFromUVs` is
declared but not defined.

- [ ] **Step 3: Implement `FindIslandsFromUVs`**

```cpp
// GS/src/GS/Renderer/UvUnwrap.cpp -- add inside namespace GS, after Unwrap
UvUnwrap::ChartAssignment UvUnwrap::FindIslandsFromUVs(const MeshData& data)
{
	size_t triCount = data.TriangleCount();
	std::vector<std::vector<int>> adjacency = BuildTriangleAdjacency(data);

	// Union-Find over triangles, joined across a shared mesh edge only
	// when both triangles agree on UV at both of that edge's endpoints --
	// that is exactly what "no seam here" means for an existing UV
	// layout.
	std::vector<int> parent(triCount);
	for (size_t i = 0; i < triCount; i++)
		parent[i] = (int)i;

	auto find = [&](int x) {
		while (parent[x] != x)
		{
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	auto unite = [&](int a, int b) {
		a = find(a); b = find(b);
		if (a != b)
			parent[a] = b;
	};

	constexpr float kUvEpsilon = 1e-4f;
	for (size_t t = 0; t < triCount; t++)
	{
		glm::vec3 positions[3] = {
			data.Vertices[data.Indices[t * 3 + 0]].Position,
			data.Vertices[data.Indices[t * 3 + 1]].Position,
			data.Vertices[data.Indices[t * 3 + 2]].Position,
		};
		glm::vec2 uvs[3] = {
			data.Vertices[data.Indices[t * 3 + 0]].TexCoord,
			data.Vertices[data.Indices[t * 3 + 1]].TexCoord,
			data.Vertices[data.Indices[t * 3 + 2]].TexCoord,
		};

		for (int neighbor : adjacency[t])
		{
			if ((size_t)neighbor <= t)
				continue;   // each pair once

			glm::vec3 shared[2];
			int sharedCount = 0;
			for (int k = 0; k < 3 && sharedCount < 2; k++)
			{
				for (int m = 0; m < 3; m++)
				{
					unsigned int nIdx = data.Indices[neighbor * 3 + m];
					if (data.Vertices[nIdx].Position == positions[k])
					{
						shared[sharedCount++] = positions[k];
						break;
					}
				}
			}
			if (sharedCount < 2)
				continue;   // adjacency came from a shared edge, but guard rather than assume

			bool uvMatches = true;
			for (int s = 0; s < 2 && uvMatches; s++)
			{
				glm::vec2 uvHere{};
				for (int k = 0; k < 3; k++)
					if (positions[k] == shared[s]) { uvHere = uvs[k]; break; }

				glm::vec2 uvThere{};
				for (int m = 0; m < 3; m++)
				{
					unsigned int nIdx = data.Indices[neighbor * 3 + m];
					if (data.Vertices[nIdx].Position == shared[s]) { uvThere = data.Vertices[nIdx].TexCoord; break; }
				}

				if (glm::length(uvHere - uvThere) > kUvEpsilon)
					uvMatches = false;
			}

			if (uvMatches)
				unite((int)t, neighbor);
		}
	}

	std::unordered_map<int, int> rootToChartId;
	ChartAssignment result(triCount);
	for (size_t t = 0; t < triCount; t++)
	{
		int root = find((int)t);
		auto it = rootToChartId.find(root);
		if (it == rootToChartId.end())
			it = rootToChartId.emplace(root, (int)rootToChartId.size()).first;
		result[t] = it->second;
	}
	return result;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `./gs.py build && ./gs.py run`, check the log for
`UvUnwrapTest: 9 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Renderer/UvUnwrap.cpp TestEnv/src/UvUnwrapTest.h
git commit -m "Implement GS::UvUnwrap::FindIslandsFromUVs"
```

---

## Task 4: `GS::UvTemplateWriter`

**Files:**
- Create: `GS/src/GS/Renderer/UvTemplateWriter.h`
- Create: `GS/src/GS/Renderer/UvTemplateWriter.cpp`
- Modify: `GS/src/GS.h` (add `#include "GS/Renderer/UvTemplateWriter.h"`
  right after the new `UvUnwrap.h` include)
- Test: `TestEnv/src/UvTemplateWriterTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::UvUnwrap::ChartAssignment` (Task 1), `GS::MeshData` (existing).
- Produces: `GS::UvTemplateWriter::Write(const std::string& path, int resolution, const MeshData& data, const UvUnwrap::ChartAssignment& chartIdPerTriangle, std::string& error) -> bool`,
  used by the editor wiring (Task 5).

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/UvTemplateWriterTest.h
// TEMPORARY -- delete after verifying UvTemplateWriter's output resolution
// and pixel placement against a known, hand-placed triangle.
#pragma once
#include <GS.h>
#include <GS/Renderer/UvTemplateWriter.h>
#include <stb_image.h>
#include <filesystem>

namespace UvTemplateWriterTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::MeshData data;
		// A triangle covering roughly the UV square's lower-left region:
		// (0.1,0.1) - (0.6,0.1) - (0.1,0.6). Its centroid is well inside;
		// the opposite corner (0.9,0.9) is well outside.
		data.Vertices = {
			{ {0,0,0}, {0,0,1}, {0.1f, 0.1f} },
			{ {1,0,0}, {0,0,1}, {0.6f, 0.1f} },
			{ {0,1,0}, {0,0,1}, {0.1f, 0.6f} },
		};
		data.Indices = { 0, 1, 2 };
		GS::UvUnwrap::ChartAssignment chartIds = { 0 };

		const std::string path = "uv_template_test_output.png";
		std::string error;
		bool wrote = GS::UvTemplateWriter::Write(path, 64, data, chartIds, error);
		Check(wrote, "Write reports success: " + error);

		int w = 0, h = 0, channels = 0;
		stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
		Check(pixels != nullptr, "written PNG can be read back");
		if (pixels)
		{
			Check(w == 64 && h == 64, "output resolution matches the requested 64x64");

			int cx = (int)((0.1f + 0.6f + 0.1f) / 3.0f * 64.0f);
			int cy = (int)((0.1f + 0.1f + 0.6f) / 3.0f * 64.0f);
			uint8_t insideAlpha = pixels[(cy * 64 + cx) * 4 + 3];
			Check(insideAlpha > 0, "a pixel inside the triangle is non-transparent");

			int ox = (int)(0.9f * 64.0f), oy = (int)(0.9f * 64.0f);
			uint8_t outsideAlpha = pixels[(oy * 64 + ox) * 4 + 3];
			Check(outsideAlpha == 0, "a pixel outside the triangle stays transparent");

			stbi_image_free(pixels);
		}

		std::filesystem::remove(path);
		GS_TRACE("UvTemplateWriterTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it into `TestEnv/src/TestApp.cpp`'s constructor the same way as
`UvUnwrapTest`.

- [ ] **Step 2: Run to verify it fails**

Run: `./gs.py build` -- expected: fails to compile (`GS/Renderer/
UvTemplateWriter.h` does not exist yet).

- [ ] **Step 3: Write the implementation**

```cpp
// GS/src/GS/Renderer/UvTemplateWriter.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"
#include "GS/Renderer/UvUnwrap.h"

namespace GS {

	// Rasterizes a mesh's (already [0,1]-ranged) packed UVs into a
	// wireframe + per-chart-tint PNG a designer can paint over.
	class GS_API UvTemplateWriter
	{
	public:
		static bool Write(const std::string& path, int resolution, const MeshData& data,
			const UvUnwrap::ChartAssignment& chartIdPerTriangle, std::string& error);
	};

}
```

```cpp
// GS/src/GS/Renderer/UvTemplateWriter.cpp
#include "gspch.h"
#include "GS/Renderer/UvTemplateWriter.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace GS {

	namespace {

		// A small, fixed pastel palette -- cycled by chart id, not
		// computed from the mesh, so two charts are always visually
		// distinguishable regardless of how many there are.
		constexpr int kPaletteSize = 12;
		const uint8_t kPalette[kPaletteSize][3] = {
			{255,214,214}, {214,255,214}, {214,214,255}, {255,255,214},
			{255,214,255}, {214,255,255}, {255,234,214}, {214,255,234},
			{234,214,255}, {255,214,234}, {234,255,214}, {214,234,255},
		};

		void SetPixel(std::vector<uint8_t>& buffer, int resolution, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
		{
			if (x < 0 || y < 0 || x >= resolution || y >= resolution)
				return;
			size_t i = (size_t)(y * resolution + x) * 4;
			buffer[i + 0] = r; buffer[i + 1] = g; buffer[i + 2] = b; buffer[i + 3] = a;
		}

		// Bresenham -- a template wireframe needs a visible line, not an
		// anti-aliased one.
		void DrawLine(std::vector<uint8_t>& buffer, int resolution, glm::ivec2 p0, glm::ivec2 p1)
		{
			int dx = std::abs(p1.x - p0.x), sx = p0.x < p1.x ? 1 : -1;
			int dy = -std::abs(p1.y - p0.y), sy = p0.y < p1.y ? 1 : -1;
			int err = dx + dy;
			glm::ivec2 p = p0;
			while (true)
			{
				SetPixel(buffer, resolution, p.x, p.y, 60, 60, 60, 255);
				if (p == p1)
					break;
				int e2 = 2 * err;
				if (e2 >= dy) { err += dy; p.x += sx; }
				if (e2 <= dx) { err += dx; p.y += sy; }
			}
		}

		void FillTriangle(std::vector<uint8_t>& buffer, int resolution, glm::vec2 a, glm::vec2 b, glm::vec2 c, const uint8_t color[3])
		{
			int minX = std::max(0, (int)std::floor(std::min({ a.x, b.x, c.x })));
			int maxX = std::min(resolution - 1, (int)std::ceil(std::max({ a.x, b.x, c.x })));
			int minY = std::max(0, (int)std::floor(std::min({ a.y, b.y, c.y })));
			int maxY = std::min(resolution - 1, (int)std::ceil(std::max({ a.y, b.y, c.y })));

			float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
			if (std::abs(area) < 1e-6f)
				return;   // degenerate triangle in UV space -- nothing to fill

			for (int y = minY; y <= maxY; y++)
			{
				for (int x = minX; x <= maxX; x++)
				{
					glm::vec2 p((float)x + 0.5f, (float)y + 0.5f);
					float w0 = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / area;
					float w1 = ((c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x)) / area;
					float w2 = 1.0f - w0 - w1;
					if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
						SetPixel(buffer, resolution, x, y, color[0], color[1], color[2], 160);
				}
			}
		}

	}

	bool UvTemplateWriter::Write(const std::string& path, int resolution, const MeshData& data,
		const UvUnwrap::ChartAssignment& chartIdPerTriangle, std::string& error)
	{
		if (resolution <= 0)
		{
			error = "resolution must be positive";
			return false;
		}

		std::vector<uint8_t> buffer((size_t)resolution * resolution * 4, 0);

		size_t triCount = data.TriangleCount();
		for (size_t t = 0; t < triCount; t++)
		{
			glm::vec2 uv0 = data.Vertices[data.Indices[t * 3 + 0]].TexCoord * (float)resolution;
			glm::vec2 uv1 = data.Vertices[data.Indices[t * 3 + 1]].TexCoord * (float)resolution;
			glm::vec2 uv2 = data.Vertices[data.Indices[t * 3 + 2]].TexCoord * (float)resolution;

			int chartId = (t < chartIdPerTriangle.size()) ? chartIdPerTriangle[t] : 0;
			const uint8_t* color = kPalette[((chartId % kPaletteSize) + kPaletteSize) % kPaletteSize];
			FillTriangle(buffer, resolution, uv0, uv1, uv2, color);
		}
		for (size_t t = 0; t < triCount; t++)
		{
			glm::ivec2 uv0 = glm::ivec2(data.Vertices[data.Indices[t * 3 + 0]].TexCoord * (float)resolution);
			glm::ivec2 uv1 = glm::ivec2(data.Vertices[data.Indices[t * 3 + 1]].TexCoord * (float)resolution);
			glm::ivec2 uv2 = glm::ivec2(data.Vertices[data.Indices[t * 3 + 2]].TexCoord * (float)resolution);
			DrawLine(buffer, resolution, uv0, uv1);
			DrawLine(buffer, resolution, uv1, uv2);
			DrawLine(buffer, resolution, uv2, uv0);
		}

		std::error_code fsError;
		std::filesystem::path target(path);
		if (target.has_parent_path())
			std::filesystem::create_directories(target.parent_path(), fsError);

		int written = stbi_write_png(path.c_str(), resolution, resolution, 4, buffer.data(), resolution * 4);
		if (written == 0)
		{
			error = "stbi_write_png failed to write '" + path + "'";
			return false;
		}
		return true;
	}

}
```

Add the `GS.h` include:

```cpp
// GS/src/GS.h
#include "GS/Renderer/UvUnwrap.h"
#include "GS/Renderer/UvTemplateWriter.h"
```

- [ ] **Step 4: Run to verify it passes**

Run: `./gs.py build && ./gs.py run`, check the log for
`UvTemplateWriterTest: 4 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Renderer/UvTemplateWriter.h GS/src/GS/Renderer/UvTemplateWriter.cpp \
    GS/src/GS.h TestEnv/src/UvTemplateWriterTest.h TestEnv/src/TestApp.cpp
git commit -m "Add GS::UvTemplateWriter"
```

---

## Task 5: Editor wiring -- "Export UV Template"

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h`:
  - Extract `StartMeshEdit`'s primitive-vs-file loading branch
    (`TestEnv/src/EditorSceneView.h:951-969`) into a shared member function
    `LoadMeshDataForExport`.
  - Add a new member function `ExportUvTemplate`.
  - Add the "Export UV Template" button + resolution field to the
    Inspector, right after the existing "Edit Mesh" button
    (`TestEnv/src/EditorSceneView.h:451`).
  - Add `int m_UvExportResolution = 1024;` to the private member block,
    right after `GS::EntityId m_MeshEditEntity = GS::InvalidEntity;`
    (`TestEnv/src/EditorSceneView.h:1878`).
  - Add a temporary end-to-end self-test call inside `OnAttach()`
    (`TestEnv/src/EditorSceneView.h:61-69`).

**Interfaces:**
- Consumes: `GS::UvUnwrap::HasUsableUVs`/`Unwrap`/`FindIslandsFromUVs`
  (Tasks 1-3), `GS::UvTemplateWriter::Write` (Task 4), `GS::ObjWriter::Save`
  (existing), `EditorHistory`/`EditFieldCommand` (existing, already used by
  the "Edit Mesh" Done handler).
- Produces: `LoadMeshDataForExport(GS::MeshComponent*, GS::MeshData&, std::string&) -> bool`
  and `ExportUvTemplate(GS::EntityId, GS::MeshComponent*, int, std::string&) -> bool`,
  both member functions of `EditorSceneView`.

- [ ] **Step 1: Extract `LoadMeshDataForExport` and update `StartMeshEdit`**

Replace `StartMeshEdit`'s body (`TestEnv/src/EditorSceneView.h:951-982`)
with:

```cpp
	// Shared by StartMeshEdit and ExportUvTemplate -- both need the same
	// primitive-vs-file loading branch for a MeshComponent's current
	// geometry.
	bool LoadMeshDataForExport(GS::MeshComponent* mesh, GS::MeshData& outData, std::string& error)
	{
		if (mesh->SourcePath.rfind("primitive:cube", 0) == 0)
			outData = GS::Mesh::CreateCubeData();
		else if (mesh->SourcePath.rfind("primitive:plane", 0) == 0)
			outData = GS::Mesh::CreatePlaneData();
		else if (mesh->SourcePath.rfind("primitive:sphere", 0) == 0)
			outData = GS::Mesh::CreateSphereData();
		else if (mesh->SourcePath.rfind("primitive:cylinder", 0) == 0)
			outData = GS::Mesh::CreateCylinderData();
		else
			return GS::Mesh::LoadData(mesh->SourcePath, outData, error);
		return true;
	}

	// Shared by the Inspector's "Edit Mesh" button and the Outliner's
	// context-menu equivalent -- both need the same load-and-open logic on
	// whatever entity/mesh they were given, not necessarily m_Selected in
	// the context-menu case (right-clicking a row selects it first, but the
	// order that happens in relative to this call shouldn't matter).
	void StartMeshEdit(GS::EntityId entity, GS::MeshComponent* mesh)
	{
		GS::MeshData data;
		std::string error;
		bool loaded = LoadMeshDataForExport(mesh, data, error);
		if (!loaded)
			GS_ERROR("Edit Mesh: could not load '{0}': {1}", mesh->SourcePath, error);

		// A session with nothing in it has nothing to pick, drag, or
		// delete -- the only way out would be Done, which would then
		// write an empty .obj and replace this entity's real Geometry
		// with it. Better to leave the entity untouched and log why.
		if (loaded && !data.Vertices.empty())
		{
			m_MeshEditEntity = entity;
			m_MeshEditSession = EditableMesh::FromMeshData(data);
			m_MeshEditSelectedPoint = -1;
			m_MeshEditActive = true;
		}
	}

	// Shared by the Inspector's "Export UV Template" button and the
	// self-test below -- factored out so its branch/save/write logic can
	// be exercised without a live ImGui frame, the same reason
	// LoadMeshDataForExport itself was factored out above.
	bool ExportUvTemplate(GS::EntityId entity, GS::MeshComponent* mesh, int resolution, std::string& error)
	{
		GS::MeshData data;
		if (!LoadMeshDataForExport(mesh, data, error))
			return false;
		if (data.Vertices.empty())
		{
			error = "mesh has no vertices";
			return false;
		}

		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
		std::string exportName = tag ? tag->Name : "Mesh";

		GS::UvUnwrap::ChartAssignment chartIds;
		if (!GS::UvUnwrap::HasUsableUVs(data))
		{
			chartIds = GS::UvUnwrap::Unwrap(data);

			// Same collision-suffixed path Done (Edit Mesh) already
			// computes -- deliberately identical, not re-derived.
			std::string exportPath = "assets/" + exportName + ".obj";
			if (mesh->SourcePath != exportPath && std::filesystem::exists(exportPath))
			{
				int suffix = 1;
				std::string candidate;
				do
				{
					candidate = "assets/" + exportName + std::to_string(suffix) + ".obj";
					suffix++;
				} while (std::filesystem::exists(candidate) && candidate != mesh->SourcePath);
				exportPath = candidate;
			}

			if (!GS::ObjWriter::Save(exportPath, data, error))
				return false;

			std::string oldPath = mesh->SourcePath;
			mesh->SourcePath = exportPath;
			// Bypasses MeshCache -- same reason Edit Mesh's own Done does
			// (see its comment at this file's Done handler): MeshCache
			// caches one mesh per path forever, so re-exporting the same
			// path twice would otherwise show the first export's stale
			// geometry.
			mesh->Geometry = std::make_shared<GS::Mesh>(data, exportName);
			EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, std::string>>(
				entity, &GS::MeshComponent::SourcePath, oldPath, exportPath));
		}
		else
		{
			chartIds = GS::UvUnwrap::FindIslandsFromUVs(data);
		}

		return GS::UvTemplateWriter::Write("assets/" + exportName + "_uv.png", resolution, data, chartIds, error);
	}
```

- [ ] **Step 2: Run to verify it still compiles**

Run: `./gs.py build` -- expected: succeeds (this step is a pure refactor
plus two new, not-yet-called member functions).

- [ ] **Step 3: Add the Inspector button**

In the Inspector block, right after the existing "Edit Mesh" button
(`TestEnv/src/EditorSceneView.h:451`, `if (ImGui::Button("Edit Mesh") && ...)`),
insert:

```cpp
			ImGui::SetNextItemWidth(80.0f);
			ImGui::InputInt("##UvExportResolution", &m_UvExportResolution);
			m_UvExportResolution = glm::clamp(m_UvExportResolution, 16, 8192);
			ImGui::SameLine();
			if (ImGui::Button("Export UV Template") && !m_MeshEditActive && !PlayMode::IsPlaying())
			{
				std::string error;
				if (!ExportUvTemplate(m_Selected, mesh, m_UvExportResolution, error))
					GS_ERROR("Export UV Template: failed for '{0}': {1}", mesh->SourcePath, error);
			}
```

Add the member variable, right after `GS::EntityId m_MeshEditEntity = GS::InvalidEntity;`
(`TestEnv/src/EditorSceneView.h:1878`):

```cpp
	int m_UvExportResolution = 1024;
```

- [ ] **Step 4: Run to verify it still compiles**

Run: `./gs.py build`.

- [ ] **Step 5: Add a temporary end-to-end self-test in `OnAttach`**

`ExportUvTemplate` needs a live `g_EditorScene` and (for the `HasUsableUVs`
== false branch) a real GL-context-backed `GS::Mesh` construction, so this
can't run from `TestApp`'s constructor the way `UvUnwrapTest`/
`UvTemplateWriterTest` do -- it needs to run once the editor layer has
actually attached, the same reason CLAUDE.md's own convention says
"GL-dependent checks need a live context, so they run inside a demo's
update rather than at construction." Add this at the end of
`EditorSceneView::OnAttach()` (`TestEnv/src/EditorSceneView.h:61-69`,
after the existing `BuildTarget(); BuildShader();` calls):

```cpp
		// TEMPORARY -- verifies ExportUvTemplate's end-to-end wiring
		// (both the "usable UVs, no rewrite" and "unusable UVs, fresh
		// unwrap + re-save" branches) against a real scene. Delete this
		// block, UvUnwrapTest.h, and UvTemplateWriterTest.h together once
		// verified.
		{
			// Case 1: CreateCubeData()'s own per-face UVs are usable ->
			// FindIslandsFromUVs path, no .obj rewrite.
			GS::Entity usableEntity = g_EditorScene.CreateEntity("UvExportTestCube");
			g_EditorScene.AddComponent<GS::TransformComponent>(usableEntity.GetId());
			auto& usableMesh = g_EditorScene.AddComponent<GS::MeshComponent>(usableEntity.GetId());
			usableMesh.SourcePath = "primitive:cube";
			usableMesh.Geometry = std::shared_ptr<GS::Mesh>(GS::Mesh::CreateCube());
			std::string usableError;
			bool usableOk = ExportUvTemplate(usableEntity.GetId(), &usableMesh, 512, usableError);
			GS_TRACE("UvExportTest (usable UVs): {0} ({1}), SourcePath still '{2}'",
				usableOk ? "ok" : "FAIL", usableError, usableMesh.SourcePath);
			g_EditorScene.DestroyEntity(usableEntity.GetId());

			// Case 2: all-(0,0) UVs (matching EditableMesh::Rebuild()'s
			// known gap) -> Unwrap + ObjWriter re-save path.
			GS::MeshData zeroed = GS::Mesh::CreateCubeData();
			for (auto& v : zeroed.Vertices)
				v.TexCoord = { 0.0f, 0.0f };
			std::string saveError;
			GS::ObjWriter::Save("assets/uv_export_test_zeroed.obj", zeroed, saveError);

			GS::Entity unusableEntity = g_EditorScene.CreateEntity("UvExportTestZeroed");
			g_EditorScene.AddComponent<GS::TransformComponent>(unusableEntity.GetId());
			auto& unusableMesh = g_EditorScene.AddComponent<GS::MeshComponent>(unusableEntity.GetId());
			unusableMesh.SourcePath = "assets/uv_export_test_zeroed.obj";
			unusableMesh.Geometry = std::make_shared<GS::Mesh>(zeroed, "ZeroedCube");
			std::string unusableError;
			bool unusableOk = ExportUvTemplate(unusableEntity.GetId(), &unusableMesh, 512, unusableError);
			GS_TRACE("UvExportTest (unusable UVs -> unwrap): {0} ({1}), SourcePath now '{2}'",
				unusableOk ? "ok" : "FAIL", unusableError, unusableMesh.SourcePath);
			g_EditorScene.DestroyEntity(unusableEntity.GetId());
		}
```

- [ ] **Step 6: Run and verify by hand**

```sh
./gs.py build
cd bin/Debug-linux-x86_64/TestEnv
./TestEnv --hide-window --capture /tmp/uv-export-verification/attach_check.png --capture-step 5
cd -
```

Check the log for both `UvExportTest` lines reporting `ok`, then confirm
the files actually landed correctly:

```sh
ls -la bin/Debug-linux-x86_64/TestEnv/assets/UvExportTestCube_uv.png \
       bin/Debug-linux-x86_64/TestEnv/assets/ZeroedCube_uv.png \
       bin/Debug-linux-x86_64/TestEnv/assets/uv_export_test_zeroed.obj
```

All three should exist. Open (or `file`) the two PNGs and confirm they are
512x512 and visually show a wireframe cube net with 6 distinctly-tinted
charts, not a blank or malformed image.

- [ ] **Step 7: Commit**

```bash
git add TestEnv/src/EditorSceneView.h
git commit -m "Wire up the editor's Export UV Template button"
```

---

## Task 6: End-to-end verification, cleanup, and changelog

**Files:** none created; removals and doc updates only.

**Interfaces:** none new.

- [ ] **Step 1: Remove the temporary self-tests and test artifacts**

They did their job in Tasks 1-5. Delete the temporary files, their
`#include`s/`::Run()` calls in `TestApp.cpp`, and the temporary block added
to `EditorSceneView::OnAttach` in Task 5 (Step 5):

```bash
rm TestEnv/src/UvUnwrapTest.h TestEnv/src/UvTemplateWriterTest.h
# then hand-edit TestApp.cpp to remove the two #include lines and two
# ::Run() calls, and EditorSceneView.h to remove the TEMPORARY block from
# OnAttach (Task 5, Step 5) -- leave the button/resolution field/
# LoadMeshDataForExport/ExportUvTemplate themselves, those are permanent.
```

Also remove the test .obj/PNG artifacts the Task 5 self-test wrote into
the sandbox's real `assets/` directory (they are test output, not real
content):

```bash
rm -f bin/Debug-linux-x86_64/TestEnv/assets/UvExportTestCube_uv.png \
      bin/Debug-linux-x86_64/TestEnv/assets/ZeroedCube_uv.png \
      bin/Debug-linux-x86_64/TestEnv/assets/uv_export_test_zeroed.obj \
      TestEnv/assets/uv_export_test_zeroed.obj
git status --short   # confirm nothing else unexpected shows up under assets/
```

- [ ] **Step 2: Clean three-config build**

```sh
./gs.py build all
```

Expected: Debug, Release, and Dist all succeed. Per CLAUDE.md's own
verification habit, Release has caught real bugs Debug did not -- don't
skip it.

- [ ] **Step 3: Compare the Cube3D regression capture**

```sh
cd bin/Debug-linux-x86_64/TestEnv
./TestEnv --demo Cube3D --lockstep --hide-ui --hide-window \
    --capture /tmp/uv-export-verification/cube3d_after.png --capture-step 5
cd -
cmp /tmp/uv-export-verification/cube3d_baseline.png /tmp/uv-export-verification/cube3d_after.png
```

Expected: `cmp` reports no difference (byte-identical). This whole feature
never touches `Cube3D.h` or its shader, so this is the same "nothing else
regressed" check every prior feature in this project has run.

- [ ] **Step 4: Add the README changelog entry**

Per this project's convention ("the README changelog is part of the
work"), insert a new `### YYYY-MM-DD (mesh UV template export)` entry (use
today's actual date) immediately after the `# Changelog` heading in
`README.md` (currently at line 1269, immediately before the existing
`### 2026-09-14 (multiplayer foundation...)` entry at line 1271) --
newest-first, matching every entry above it. Describe what was actually
built (heuristic chart-growing unwrap, planar-per-chart projection, shelf
packing, the wireframe+tint PNG writer, the Inspector button) and report
the real measurements from this plan's own steps: the final tuned
`kChartAngleThresholdDegrees` value and the actual cube/sphere/cylinder
chart counts from Task 2 Step 4-5, and the pass/fail counts from every
self-test run in Tasks 1-5 before they were deleted.

- [ ] **Step 5: Update `docs/STATE.md`**

Add a new bullet at the top of the "Last landed, newest first:" list
(`docs/STATE.md`, right after the line "Last landed, newest first:" and
before the current top entry, "Multiplayer/networking foundation"),
matching that list's own level of detail: what was built, the real
chart-count/self-test numbers from Step 4 above, and the disclosed scope
limits from the spec (no manual seam editing, no chart relaxation, no
texture re-import, `Submeshes` cleared on a fresh unwrap).

- [ ] **Step 6: Commit**

```bash
git add README.md docs/STATE.md TestEnv/src/TestApp.cpp TestEnv/src/EditorSceneView.h
git commit -m "Verify mesh UV template export end-to-end; changelog entry"
```

---

## Self-Review Notes

**Spec coverage:** `HasUsableUVs` -> Task 1. Chart-growing `Unwrap`
(adjacency, growth, planar projection, shelf packing, vertex rebuild,
`Submeshes` clearing) -> Task 2. `FindIslandsFromUVs` -> Task 3.
`UvTemplateWriter` (wireframe + tint rasterization, `stb_image_write`) ->
Task 4. Editor wiring (`LoadMeshDataForExport` extraction, the Inspector
button, the resolution field, the `HasUsableUVs` branch, the
`MeshCache`-bypassing reload, the collision-suffixed `.obj` path) -> Task
5. Testing section's cube/quad/sphere/cylinder/overlap/bounds checks ->
Task 2 Steps 1-5. `HasUsableUVs` true/false cases -> Task 1 Step 2.
`FindIslandsFromUVs` island checks -> Task 3 Step 1.
`UvTemplateWriter::Write` resolution/pixel checks -> Task 4 Step 1. Visual
confirmation against a non-trivial mesh -> Task 5 Step 6. Three-config
build + Cube3D regression capture -> Task 6 Steps 2-3. README changelog ->
Task 6 Step 4.

**Placeholder scan:** No TBD/TODO; every code step is real, complete code;
the two steps that can't have a pre-written literal value (the tuned
angle-threshold constant in Task 2 Step 5, the changelog/STATE.md prose in
Task 6 Steps 4-5) instead specify exactly what real, measured content to
report, matching this project's own established plan convention (see
`docs/superpowers/plans/2026-09-13-multiplayer-foundation.md`'s own Task
13 Step 5).

**Type consistency:** `GS::UvUnwrap::ChartAssignment` (`std::vector<int>`)
is defined once in Task 1 and used identically by `Unwrap`/
`FindIslandsFromUVs` (Tasks 2-3), `UvTemplateWriter::Write` (Task 4), and
`ExportUvTemplate` (Task 5) -- no renaming across tasks.
`LoadMeshDataForExport(GS::MeshComponent*, GS::MeshData&, std::string&) -> bool`
is defined once in Task 5 Step 1 and used identically by the rewritten
`StartMeshEdit` and by `ExportUvTemplate` in the same step.
