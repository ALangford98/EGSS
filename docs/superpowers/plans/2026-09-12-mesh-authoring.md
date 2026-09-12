# Mesh Authoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a user reshape a mesh already placed in the scene (move a
vertex, extrude a face, delete a vertex/face, split an edge) from inside the
editor, and save the result as a real `.obj` the rest of the engine loads
the normal way.

**Architecture:** A new, editor-only, rendering-agnostic `EditableMesh`
(welded control points + coplanar-triangle-grouped faces) does all the
actual topology editing; `GS::Mesh`'s primitive/`.obj` construction is split
so raw `MeshData` can be pulled back out of it (today only a finished GPU
`Mesh*` comes back); a new `ObjWriter` turns edited `MeshData` back into a
real file; `EditorSceneView` gains an edit-mode UI that reuses the existing
translate-gizmo's drag math, retargeted at a selected control point instead
of an entity's Transform.

**Tech Stack:** C++17, GLM, the existing `GS::Scene`/`GS::Mesh` engine code
and `EditorSceneView`/Inspector ImGui panels. No new external dependency.

**Spec:** `docs/superpowers/specs/2026-09-12-mesh-authoring-design.md`

## Global Constraints

- **Testing convention for this repo, not a generic framework:** every task
  below adds a *temporary* self-test header (`XyzTest.h`, marked
  `// TEMPORARY -- delete after verifying`), wires one `Run()` call into
  `TestEnv/src/TestApp.cpp`'s `TestEnv()` constructor, builds with
  `./gs.py build`, runs with `./gs.py run -- --hide-window --lockstep
  --capture <path> --capture-step N`, reads the `GS_TRACE`'d pass/fail lines
  from the command output, and then **deletes the test header and the two
  lines that wired it in** before moving on. There is no permanent test
  suite in this project — do not add one.
- **Verify at all three configs** (`./gs.py build all`) before considering
  any task's code changes final, and confirm a `--demo Cube3D --hide-ui`
  capture is still byte-identical to before the task's changes wherever the
  task touches rendering-adjacent code (Tasks 1, 8, 9) — `--hide-ui` renders
  no ImGui, so this only catches an actual rendering regression, not a UI
  change.
- **Never commit, never push.** This repo's own rule: the owner commits
  their own work. Each task's last step is "mark the task done", not `git
  commit` — leave all changes staged in the working tree.
- **Compute expected values by hand before asserting them**, per this
  project's established verification habit — every test below states the
  hand-derived expected number *in* the test's own check message, not just
  in this plan.

---

### Task 1: Split `Mesh::Create*`/`Load` so raw `MeshData` can be pulled back out

**Files:**
- Modify: `GS/src/GS/Renderer/Mesh.h:105` (near `Load`), `:112-131` (the
  four `Create*` declarations)
- Modify: `GS/src/GS/Renderer/Mesh.cpp:146-149` (`Load`), `:146-186`
  (`CreateCube`), `:187-201` (`CreatePlane`), `:206-262` (`CreateSphere`),
  `:264-320` (`CreateCylinder`)
- Test: `TestEnv/src/MeshDataSplitTest.h` (temporary)

**Interfaces:**
- Produces: `static GS::MeshData GS::Mesh::CreateCubeData(float size = 1.0f)`,
  `CreatePlaneData(float size = 1.0f)`,
  `CreateSphereData(float radius = 0.5f, unsigned int segments = 32, unsigned int rings = 16)`,
  `CreateCylinderData(float radius = 0.5f, float halfHeight = 0.5f, unsigned int segments = 24)`,
  `static bool GS::Mesh::LoadData(const std::string& path, GS::MeshData& out, std::string& error)`
  — every later task that needs raw mesh data (Tasks 2, 8) calls these.
  `Mesh::CreateCube`/`CreatePlane`/`CreateSphere`/`CreateCylinder`/`Load`
  keep their exact existing signatures and behavior, now implemented in
  terms of the above.

This is a pure relocation — cut each function's `MeshData`-building body out
into a new function that returns it, and have the original become a one-line
wrapper. No geometry logic changes.

- [ ] **Step 1: Add the five new declarations to `Mesh.h`**

Add directly above each existing declaration (so the pairing reads clearly
in the header):

```cpp
		// Same geometry as CreateCube, minus the GPU upload -- the raw data
		// Mesh's own constructor would otherwise consume and discard.
		// Mesh authoring pulls this back out to build an editable topology
		// from a mesh that's already been placed.
		static MeshData CreateCubeData(float size = 1.0f);
		static Mesh* CreateCube(float size = 1.0f);

		static MeshData CreatePlaneData(float size = 1.0f);
		static Mesh* CreatePlane(float size = 1.0f);

		static MeshData CreateSphereData(float radius = 0.5f,
			unsigned int segments = 32, unsigned int rings = 16);
		static Mesh* CreateSphere(float radius = 0.5f,
			unsigned int segments = 32, unsigned int rings = 16);

		static MeshData CreateCylinderData(float radius = 0.5f, float halfHeight = 0.5f,
			unsigned int segments = 24);
		static Mesh* CreateCylinder(float radius = 0.5f, float halfHeight = 0.5f,
			unsigned int segments = 24);

		// Same as Load, minus the GPU upload and the "not a Mesh" glTF
		// rejection's log line (the caller decides what to do with `error`).
		static bool LoadData(const std::string& path, MeshData& out, std::string& error);
		static Mesh* Load(const std::string& path);
```

Remove the old single-line declarations these replace (`CreateCube`,
`CreatePlane`, `CreateSphere`, `CreateCylinder`, `Load` each already existed
once — don't end up with two declarations of the same function).

- [ ] **Step 2: Rewrite the five functions in `Mesh.cpp`**

Replace the existing `Mesh::CreateCube` body with:

```cpp
	MeshData Mesh::CreateCubeData(float size)
	{
		float h = size * 0.5f;

		const glm::vec3 faceNormals[6] = {
			{  0,  0,  1 }, {  0,  0, -1 },
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 }
		};

		const glm::vec3 faceCorners[6][4] = {
			{ {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} }, // +Z
			{ { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h} }, // -Z
			{ { h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h} }, // +X
			{ {-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h} }, // -X
			{ {-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h} }, // +Y
			{ {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h} }  // -Y
		};

		const glm::vec2 uvs[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

		MeshData data;
		data.Vertices.reserve(24);
		data.Indices.reserve(36);

		for (int face = 0; face < 6; face++)
		{
			unsigned int base = (unsigned int)data.Vertices.size();

			for (int corner = 0; corner < 4; corner++)
				data.Vertices.push_back({ faceCorners[face][corner], faceNormals[face], uvs[corner] });

			data.Indices.insert(data.Indices.end(), {
				base + 0, base + 1, base + 2,
				base + 2, base + 3, base + 0
			});
		}

		data.RecalculateBounds();
		return data;
	}

	Mesh* Mesh::CreateCube(float size)
	{
		return new Mesh(CreateCubeData(size), "Cube");
	}
```

Replace the existing `Mesh::CreatePlane` body with:

```cpp
	MeshData Mesh::CreatePlaneData(float size)
	{
		float h = size * 0.5f;

		MeshData data;
		data.Vertices = {
			{ {-h, 0.0f,  h }, { 0, 1, 0 }, { 0, 0 } },
			{ { h, 0.0f,  h }, { 0, 1, 0 }, { 1, 0 } },
			{ { h, 0.0f, -h }, { 0, 1, 0 }, { 1, 1 } },
			{ {-h, 0.0f, -h }, { 0, 1, 0 }, { 0, 1 } }
		};
		data.Indices = { 0, 1, 2, 2, 3, 0 };

		data.RecalculateBounds();
		return data;
	}

	Mesh* Mesh::CreatePlane(float size)
	{
		return new Mesh(CreatePlaneData(size), "Plane");
	}
```

Replace the existing `Mesh::CreateSphere` body with:

```cpp
	MeshData Mesh::CreateSphereData(float radius, unsigned int segments, unsigned int rings)
	{
		if (segments < 3) segments = 3;
		if (rings < 2) rings = 2;

		MeshData data;
		data.Vertices.reserve((size_t)(segments + 1) * (rings + 1));
		data.Indices.reserve((size_t)segments * rings * 6);

		for (unsigned int ring = 0; ring <= rings; ring++)
		{
			float v = (float)ring / (float)rings;
			float phi = v * glm::pi<float>();

			float y = std::cos(phi);
			float ringRadius = std::sin(phi);

			for (unsigned int segment = 0; segment <= segments; segment++)
			{
				float u = (float)segment / (float)segments;
				float theta = u * glm::two_pi<float>();

				glm::vec3 unit(ringRadius * std::cos(theta), y, ringRadius * std::sin(theta));

				data.Vertices.push_back({ unit * radius, unit, { u, 1.0f - v } });
			}
		}

		unsigned int stride = segments + 1;
		for (unsigned int ring = 0; ring < rings; ring++)
		{
			for (unsigned int segment = 0; segment < segments; segment++)
			{
				unsigned int a = ring * stride + segment;
				unsigned int b = a + stride;

				if (ring != 0)
					data.Indices.insert(data.Indices.end(), { a, a + 1, b });
				if (ring != rings - 1)
					data.Indices.insert(data.Indices.end(), { a + 1, b + 1, b });
			}
		}

		data.RecalculateBounds();
		return data;
	}

	Mesh* Mesh::CreateSphere(float radius, unsigned int segments, unsigned int rings)
	{
		return new Mesh(CreateSphereData(radius, segments, rings), "Sphere");
	}
```

Replace the existing `Mesh::CreateCylinder` body with:

```cpp
	MeshData Mesh::CreateCylinderData(float radius, float halfHeight, unsigned int segments)
	{
		if (segments < 3) segments = 3;

		MeshData data;
		data.Vertices.reserve((size_t)(segments + 1) * 2);
		data.Indices.reserve((size_t)segments * 6);

		for (int end = 0; end < 2; end++)
		{
			float y = end == 0 ? -halfHeight : halfHeight;
			float v = end == 0 ? 0.0f : 1.0f;

			for (unsigned int segment = 0; segment <= segments; segment++)
			{
				float u = (float)segment / (float)segments;
				float theta = u * glm::two_pi<float>();

				glm::vec3 outward(std::cos(theta), 0.0f, std::sin(theta));

				data.Vertices.push_back({
					{ outward.x * radius, y, outward.z * radius },
					outward,
					{ u, v } });
			}
		}

		unsigned int stride = segments + 1;
		for (unsigned int segment = 0; segment < segments; segment++)
		{
			unsigned int bottom = segment;
			unsigned int top = stride + segment;

			data.Indices.insert(data.Indices.end(), { bottom, bottom + 1, top });
			data.Indices.insert(data.Indices.end(), { bottom + 1, top + 1, top });
		}

		data.RecalculateBounds();
		return data;
	}

	Mesh* Mesh::CreateCylinder(float radius, float halfHeight, unsigned int segments)
	{
		return new Mesh(CreateCylinderData(radius, halfHeight, segments), "Cylinder");
	}
```

Replace the existing `Mesh::Load` body with:

```cpp
	bool Mesh::LoadData(const std::string& path, MeshData& out, std::string& error)
	{
		size_t dot = path.find_last_of('.');
		std::string extension = dot == std::string::npos ? "" : path.substr(dot);
		if (extension == ".gltf" || extension == ".glb")
		{
			error = "a glTF is a scene, not a mesh -- use GltfLoader::Load and build a Mesh per GltfModel::Meshes entry";
			return false;
		}

		return ObjLoader::Load(path, out, error);
	}

	Mesh* Mesh::Load(const std::string& path)
	{
		MeshData data;
		std::string error;

		if (!Mesh::LoadData(path, data, error))
		{
			GS_CORE_ERROR("Mesh::Load failed: {0}", error);
			return nullptr;
		}

		size_t slash = path.find_last_of("/\\");
		std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

		GS_CORE_INFO("Loaded '{0}': {1} vertices, {2} triangles",
			name, data.Vertices.size(), data.TriangleCount());

		return new Mesh(data, name);
	}
```

- [ ] **Step 3: Build all three configs**

Run: `./gs.py build all`
Expected: clean build, no errors, in Debug/Release/Dist.

- [ ] **Step 4: Write and run the temporary self-test**

Create `TestEnv/src/MeshDataSplitTest.h`:

```cpp
// TEMPORARY -- delete after verifying the Mesh::Create*/Load data split.
#pragma once
#include <GS.h>

namespace MeshDataSplitTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS::MeshData cube = GS::Mesh::CreateCubeData(1.0f);
		Check(cube.Vertices.size() == 24, "CreateCubeData: 24 vertices (expected -- 4 per face x 6 faces)");
		Check(cube.Indices.size() == 36, "CreateCubeData: 36 indices (12 triangles)");
		Check(std::abs(cube.BoundsMin.x - (-0.5f)) < 1e-5f && std::abs(cube.BoundsMax.x - 0.5f) < 1e-5f,
			"CreateCubeData: bounds are exactly ±0.5 for size=1");

		GS::MeshData plane = GS::Mesh::CreatePlaneData(1.0f);
		Check(plane.Vertices.size() == 4 && plane.Indices.size() == 6,
			"CreatePlaneData: 4 vertices, 6 indices (2 triangles)");

		// Sphere/cylinder triangle counts follow a less trivial formula --
		// checked here by consistency with the wrapper instead of re-deriving
		// it by hand (see Step 4's next checks), which is the thing actually
		// at risk in this task (did the wrapper end up calling the same
		// function with the same arguments), not the geometry formula itself.
		GS::MeshData sphere = GS::Mesh::CreateSphereData(0.5f, 32, 16);
		Check(sphere.Vertices.size() == (size_t)(32 + 1) * (16 + 1),
			"CreateSphereData: (segments+1)*(rings+1) vertices");

		std::unique_ptr<GS::Mesh> cubeMesh(GS::Mesh::CreateCube(1.0f));
		Check(cubeMesh->GetVertexCount() == cube.Vertices.size() && cubeMesh->GetTriangleCount() == cube.TriangleCount(),
			"CreateCube's wrapper produces a Mesh with exactly CreateCubeData's own counts");

		std::unique_ptr<GS::Mesh> sphereMesh(GS::Mesh::CreateSphere(0.5f, 32, 16));
		Check(sphereMesh->GetVertexCount() == sphere.Vertices.size() && sphereMesh->GetTriangleCount() == sphere.TriangleCount(),
			"CreateSphere's wrapper produces a Mesh with exactly CreateSphereData's own counts");

		// A real asset already logged elsewhere in this project as "60
		// vertices, 20 triangles" (TestEnv/assets/models/icosahedron.obj) --
		// an external, independent reference, not re-derived from this code.
		GS::MeshData icosahedron;
		std::string error;
		bool loaded = GS::Mesh::LoadData("assets/models/icosahedron.obj", icosahedron, error);
		Check(loaded && icosahedron.Vertices.size() == 60 && icosahedron.TriangleCount() == 20,
			"LoadData('icosahedron.obj') matches the known 60 vertices / 20 triangles");

		GS_TRACE("MeshDataSplitTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

Wire it in: add `#include "MeshDataSplitTest.h"   // TEMPORARY` near the other
includes in `TestEnv/src/TestApp.cpp`, and `MeshDataSplitTest::Run();   //
TEMPORARY -- delete after verifying` as the first line of `TestEnv()`'s
constructor body.

Run:
```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/meshdatasplit.png --capture-step 2
```
Expected: `MeshDataSplitTest: 8 passed, 0 failed` in the output (grep for
`MeshDataSplitTest` and `FAIL`).

- [ ] **Step 5: Byte-identical Cube3D regression check**

Run before and after this task's changes (if not already captured from a
clean checkout, capture the "before" first by stashing — or, since this is
the very first task, treat the pre-task tree as "before"):
```sh
./gs.py run -- --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/cube3d_before.png --capture-step 5
./gs.py run -- --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/cube3d_after.png --capture-step 5
sha256sum /tmp/cube3d_before.png /tmp/cube3d_after.png
```
Expected: identical hashes. Cube3D loads meshes through `Mesh::CreateCube`/
`Load` — if either wrapper's behavior changed, its render would differ.

- [ ] **Step 6: Remove the temporary test**

Delete `TestEnv/src/MeshDataSplitTest.h` and the two lines in
`TestEnv/src/TestApp.cpp` that referenced it. Rebuild to confirm
`TestApp.cpp` still compiles clean with no dangling reference.

- [ ] **Step 7: Mark task done**

No commit — leave the `Mesh.h`/`Mesh.cpp` changes staged in the working
tree for the owner to review and commit.

---

### Task 2: `EditableMesh` — types, welding, ungrouped faces

**Files:**
- Create: `TestEnv/src/EditableMesh.h`
- Test: `TestEnv/src/EditableMeshWeldTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::Mesh::CreateCubeData(float)` (Task 1)
- Produces: `struct EditPoint { glm::vec3 Position; }`;
  `struct EditFace { std::vector<int> Points; glm::vec3 Normal; }`;
  `class EditableMesh` with
  `static EditableMesh FromMeshData(const GS::MeshData& source)`,
  `int PointCount() const`, `int FaceCount() const`,
  `const EditPoint& Point(int index) const`,
  `const EditFace& Face(int index) const` — every later `EditableMesh` task
  (3-6) adds methods to this same class.

At this stage, faces are **not** grouped yet — `FromMeshData` emits one
`EditFace` per source triangle (3 points each). Task 3 replaces that with
real, coplanar-grouped faces; this task's own test checks the ungrouped
shape on purpose, and is deleted before Task 3 changes the behavior it
checks (both are temporary, so there's nothing to keep in sync).

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/EditableMeshWeldTest.h`:

```cpp
// TEMPORARY -- delete after verifying EditableMesh's welding.
#pragma once
#include <GS.h>

#include "EditableMesh.h"

namespace EditableMeshWeldTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS::MeshData cubeData = GS::Mesh::CreateCubeData(1.0f);
		EditableMesh mesh = EditableMesh::FromMeshData(cubeData);

		Check(mesh.PointCount() == 8, "24 source vertices weld down to 8 control points (a cube's real corner count)");
		Check(mesh.FaceCount() == 12, "no grouping yet -- one EditFace per source triangle (12)");

		bool allCorners = true;
		for (int i = 0; i < mesh.PointCount(); i++)
		{
			const glm::vec3& p = mesh.Point(i).Position;
			bool isCorner = std::abs(std::abs(p.x) - 0.5f) < 1e-5f
				&& std::abs(std::abs(p.y) - 0.5f) < 1e-5f
				&& std::abs(std::abs(p.z) - 0.5f) < 1e-5f;
			allCorners = allCorners && isCorner;
		}
		Check(allCorners, "every welded point sits at one of the cube's 8 corners (±0.5, ±0.5, ±0.5)");

		for (int i = 0; i < mesh.FaceCount(); i++)
			Check(mesh.Face(i).Points.size() == 3, "face " + std::to_string(i) + " has exactly 3 points (a raw triangle)");

		GS_TRACE("EditableMeshWeldTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 2: Wire in and run to confirm it fails to compile (EditableMesh.h doesn't exist yet)**

Add the include and a `EditableMeshWeldTest::Run();` call in
`TestApp.cpp`'s constructor (same pattern as Task 1's Step 4), then:
```sh
./gs.py build
```
Expected: FAIL — `EditableMesh.h: No such file or directory`.

- [ ] **Step 3: Write `EditableMesh.h`**

```cpp
#pragma once

// A mesh's editable topology: vertices welded by position into draggable
// control points, editor-only and rendering-agnostic -- pure data and
// operations, no ImGui, the same separation TextBuffer.h uses for the text
// editor. See docs/superpowers/specs/2026-09-12-mesh-authoring-design.md.

#include <GS.h>

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

struct EditPoint
{
	glm::vec3 Position;   // object (local) space -- the caller applies the entity's own Transform, this never stores world space
};

struct EditFace
{
	std::vector<int> Points;             // indices into EditableMesh's points, winding order
	glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
};

class EditableMesh
{
public:
	// Welds source.Vertices by exact position match into EditPoints --
	// GPU-authored primitives place duplicates at *exactly* the same float
	// value, so no epsilon is needed here. One EditFace per source triangle
	// for now; GroupCoplanarFaces (Task 3) merges these into real faces.
	static EditableMesh FromMeshData(const GS::MeshData& source);

	int PointCount() const { return (int)m_Points.size(); }
	int FaceCount() const { return (int)m_Faces.size(); }
	const EditPoint& Point(int index) const { return m_Points[(size_t)index]; }
	const EditFace& Face(int index) const { return m_Faces[(size_t)index]; }

protected:
	std::vector<EditPoint> m_Points;
	std::vector<EditFace> m_Faces;
};
```

- [ ] **Step 4: Add the `.cpp`-equivalent (inline, header-only like every other `TestEnv/src` file) welding implementation**

Add to the bottom of `EditableMesh.h`, above the closing of the file (still
inside the class isn't right for a free function -- add it as a member
below the public section, replacing the earlier declaration-only stub with
a full definition):

```cpp
inline EditableMesh EditableMesh::FromMeshData(const GS::MeshData& source)
{
	EditableMesh mesh;

	// glm::vec3 has no std::hash specialization -- a small local hasher
	// keyed on the three floats' own hashes is enough for one use site,
	// rather than pulling in a vector-hashing library.
	struct PositionHash
	{
		size_t operator()(const glm::vec3& p) const
		{
			size_t h1 = std::hash<float>()(p.x);
			size_t h2 = std::hash<float>()(p.y);
			size_t h3 = std::hash<float>()(p.z);
			return h1 ^ (h2 << 1) ^ (h3 << 2);
		}
	};
	struct PositionEqual
	{
		bool operator()(const glm::vec3& a, const glm::vec3& b) const { return a == b; }
	};

	std::vector<int> weldedIndexOf(source.Vertices.size(), -1);
	std::unordered_map<glm::vec3, int, PositionHash, PositionEqual> pointIndexOfPosition;

	for (size_t i = 0; i < source.Vertices.size(); i++)
	{
		const glm::vec3& position = source.Vertices[i].Position;

		auto existing = pointIndexOfPosition.find(position);
		if (existing != pointIndexOfPosition.end())
		{
			weldedIndexOf[i] = existing->second;
			continue;
		}

		int newIndex = (int)mesh.m_Points.size();
		mesh.m_Points.push_back({ position });
		pointIndexOfPosition[position] = newIndex;
		weldedIndexOf[i] = newIndex;
	}

	for (size_t i = 0; i + 2 < source.Indices.size(); i += 3)
	{
		EditFace face;
		face.Points = {
			weldedIndexOf[source.Indices[i]],
			weldedIndexOf[source.Indices[i + 1]],
			weldedIndexOf[source.Indices[i + 2]]
		};

		const glm::vec3& a = mesh.m_Points[(size_t)face.Points[0]].Position;
		const glm::vec3& b = mesh.m_Points[(size_t)face.Points[1]].Position;
		const glm::vec3& c = mesh.m_Points[(size_t)face.Points[2]].Position;
		face.Normal = glm::normalize(glm::cross(b - a, c - a));

		mesh.m_Faces.push_back(face);
	}

	return mesh;
}
```

Move this definition so it appears after the `EditableMesh` class body in
the file (a free-standing `inline` definition, same as how `RenderBufferToGrid`
sits below `TextBuffer`'s own class in `TextEditorPanel.h` rather than
inside it) -- remove the semicolon-only declaration from inside the class
and replace it with this real definition referencing `EditableMesh::` outside
the class, OR keep the whole thing defined in-class if simpler; either
compiles, keep it in-class (inline in the class body) for this codebase's
existing convention (`TextBuffer`'s own methods are all defined in-class).

- [ ] **Step 5: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/editablemeshweld.png --capture-step 2
```
Expected: `EditableMeshWeldTest: 15 passed, 0 failed` (8 point-corner
checks + 12 face-point-count checks + 3 count/point checks — the exact
number isn't load-bearing, 0 failed is).

- [ ] **Step 6: Remove the temporary test**

Delete `EditableMeshWeldTest.h` and its two `TestApp.cpp` lines.
`EditableMesh.h` itself stays — it's the real deliverable.

- [ ] **Step 7: Mark task done**

---

### Task 3: Coplanar face grouping + `Rebuild`

**Files:**
- Modify: `TestEnv/src/EditableMesh.h` (add `GroupCoplanarFaces`, called from
  `FromMeshData`; add `Rebuild`)
- Test: `TestEnv/src/EditableMeshFaceTest.h` (temporary)

**Interfaces:**
- Consumes: Task 2's `EditableMesh`/`EditPoint`/`EditFace`
- Produces: `GS::MeshData EditableMesh::Rebuild() const` — Tasks 8/9 call
  this after every edit for the live preview, and once more on Done for
  export. `FromMeshData`'s behavior changes: it now returns properly
  grouped faces (a cube: 6 faces of 4 points each, not 12 of 3).

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/EditableMeshFaceTest.h`:

```cpp
// TEMPORARY -- delete after verifying face grouping and Rebuild.
#pragma once
#include <GS.h>

#include "EditableMesh.h"

namespace EditableMeshFaceTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS::MeshData cubeData = GS::Mesh::CreateCubeData(1.0f);
		EditableMesh mesh = EditableMesh::FromMeshData(cubeData);

		Check(mesh.PointCount() == 8, "still 8 welded points");
		Check(mesh.FaceCount() == 6, "12 triangles group into 6 real quad faces (a cube's actual face count)");

		bool allQuads = true;
		for (int i = 0; i < mesh.FaceCount(); i++)
			allQuads = allQuads && (mesh.Face(i).Points.size() == 4);
		Check(allQuads, "every grouped face has exactly 4 points");

		GS::MeshData rebuilt = mesh.Rebuild();
		Check(rebuilt.Vertices.size() == 24, "Rebuild re-splits back to 24 vertices (4 per face x 6, matching the original per-face-normal layout)");
		Check(rebuilt.Indices.size() == 36, "Rebuild re-triangulates 6 quads into 36 indices (12 triangles)");
		Check(rebuilt.TriangleCount() == 12, "TriangleCount agrees: 12");

		GS_TRACE("EditableMeshFaceTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

Wire it in, `./gs.py build` — expected FAIL: `FaceCount()` still returns 12
(ungrouped) and `Rebuild` doesn't exist yet.

- [ ] **Step 2: Add `GroupCoplanarFaces` and call it from `FromMeshData`**

Add as a new private method on `EditableMesh`, and call it as the last line
of `FromMeshData` before `return mesh;`:

```cpp
	void GroupCoplanarFaces()
	{
		int triangleCount = (int)m_Faces.size();
		std::vector<bool> visited(triangleCount, false);

		auto edgeKey = [](int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); };
		std::map<std::pair<int, int>, std::vector<int>> trianglesOnEdge;
		for (int t = 0; t < triangleCount; t++)
		{
			const EditFace& tri = m_Faces[(size_t)t];
			for (int e = 0; e < 3; e++)
				trianglesOnEdge[edgeKey(tri.Points[(size_t)e], tri.Points[(size_t)((e + 1) % 3)])].push_back(t);
		}

		// Authored primitives are exactly coplanar; this epsilon only
		// absorbs float noise from cross/normalize, not real angle
		// differences -- two genuinely different faces meeting at an edge
		// are never this close to parallel by coincidence in practice.
		constexpr float kNormalDotEpsilon = 0.999f;

		std::vector<EditFace> groupedFaces;
		for (int start = 0; start < triangleCount; start++)
		{
			if (visited[(size_t)start])
				continue;

			std::vector<int> group;
			std::vector<int> stack = { start };
			visited[(size_t)start] = true;
			while (!stack.empty())
			{
				int t = stack.back();
				stack.pop_back();
				group.push_back(t);

				const EditFace& tri = m_Faces[(size_t)t];
				for (int e = 0; e < 3; e++)
				{
					auto key = edgeKey(tri.Points[(size_t)e], tri.Points[(size_t)((e + 1) % 3)]);
					for (int neighbor : trianglesOnEdge[key])
					{
						if (visited[(size_t)neighbor])
							continue;
						if (glm::dot(tri.Normal, m_Faces[(size_t)neighbor].Normal) < kNormalDotEpsilon)
							continue;
						visited[(size_t)neighbor] = true;
						stack.push_back(neighbor);
					}
				}
			}

			// Boundary-loop reconstruction: a directed edge (p,q) belongs to
			// the merged face's outer boundary exactly when no triangle in
			// the group contributes the reverse edge (q,p) -- an edge shared
			// between two same-group triangles always appears once in each
			// direction (their windings are both CCW seen from outside), so
			// it cancels out and only genuinely-outer edges survive.
			std::map<std::pair<int, int>, int> directedCount;
			for (int t : group)
			{
				const EditFace& tri = m_Faces[(size_t)t];
				for (int e = 0; e < 3; e++)
					directedCount[{tri.Points[(size_t)e], tri.Points[(size_t)((e + 1) % 3)]}]++;
			}

			std::map<int, int> nextPoint;
			for (int t : group)
			{
				const EditFace& tri = m_Faces[(size_t)t];
				for (int e = 0; e < 3; e++)
				{
					int p = tri.Points[(size_t)e], q = tri.Points[(size_t)((e + 1) % 3)];
					if (directedCount.find({ q, p }) == directedCount.end())
						nextPoint[p] = q;
				}
			}

			EditFace face;
			face.Normal = m_Faces[(size_t)start].Normal;
			int startPoint = nextPoint.begin()->first;
			int current = startPoint;
			do
			{
				face.Points.push_back(current);
				current = nextPoint[current];
			} while (current != startPoint);

			groupedFaces.push_back(face);
		}

		m_Faces = groupedFaces;
	}
```

Add `#include <map>` to the top of `EditableMesh.h`. Call `mesh.GroupCoplanarFaces();`
as the last statement of `FromMeshData`, immediately before `return mesh;`.

- [ ] **Step 3: Add `Rebuild`**

```cpp
	GS::MeshData Rebuild() const
	{
		GS::MeshData data;

		for (const EditFace& face : m_Faces)
		{
			unsigned int base = (unsigned int)data.Vertices.size();

			for (size_t i = 0; i < face.Points.size(); i++)
			{
				GS::MeshVertex vertex;
				vertex.Position = m_Points[(size_t)face.Points[i]].Position;
				vertex.Normal = face.Normal;
				// UV: no layout information exists for a rebuilt/edited
				// face (out of scope, see the spec) -- (0,0) is a defined,
				// non-crashing placeholder, not a claim of correctness.
				vertex.TexCoord = { 0.0f, 0.0f };
				data.Vertices.push_back(vertex);
			}

			// Fan triangulation from the face's first point -- correct for
			// any convex polygon, which every face this class can produce
			// (a primitive's own quads, or one built by this class's own
			// operations) is.
			for (size_t i = 1; i + 1 < face.Points.size(); i++)
			{
				data.Indices.push_back(base);
				data.Indices.push_back(base + (unsigned int)i);
				data.Indices.push_back(base + (unsigned int)i + 1);
			}
		}

		data.RecalculateBounds();
		return data;
	}
```

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/editablemeshface.png --capture-step 2
```
Expected: `EditableMeshFaceTest: 6 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

- [ ] **Step 6: Mark task done**

---

### Task 4: `MovePoint`, `DeletePoint`, `DeleteFace`, `CanDelete`

**Files:**
- Modify: `TestEnv/src/EditableMesh.h`
- Test: `TestEnv/src/EditableMeshEditTest.h` (temporary)

**Interfaces:**
- Consumes: Task 3's `EditableMesh` (grouped faces, `Rebuild`)
- Produces: `void MovePoint(int point, const glm::vec3& newPosition)`,
  `void DeletePoint(int point)`, `void DeleteFace(int face)`,
  `bool CanDelete(int point) const`, `bool CanDeleteFace(int face) const` —
  Tasks 5, 8, 9 call these.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/EditableMeshEditTest.h`:

```cpp
// TEMPORARY -- delete after verifying move/delete.
#pragma once
#include <GS.h>

#include "EditableMesh.h"

namespace EditableMeshEditTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		{
			EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));
			mesh.MovePoint(0, { 5.0f, 5.0f, 5.0f });
			Check(mesh.Point(0).Position == glm::vec3(5.0f, 5.0f, 5.0f), "MovePoint sets the exact new position");
		}
		{
			// A cube's point 0 (per CreateCubeData's own +Z face, corner
			// (-h,-h,h)) touches exactly 3 of the cube's 6 faces (every
			// corner of a cube touches 3 faces) -- deleting it should
			// remove all 3, leaving 3.
			EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));
			int facesBefore = mesh.FaceCount();
			mesh.DeletePoint(0);
			Check(facesBefore == 6 && mesh.FaceCount() == 3,
				"deleting one cube corner removes the 3 faces that touched it (6 -> 3)");
			Check(mesh.PointCount() == 7, "the deleted point itself is gone (8 -> 7)");
		}
		{
			EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));
			mesh.DeleteFace(0);
			Check(mesh.FaceCount() == 5, "deleting one face leaves the other 5 (6 -> 5)");
			// Every point of a cube is shared by 3 faces, so removing one
			// face never orphans a point (each of its 4 points still has 2
			// other faces) -- all 8 points remain.
			Check(mesh.PointCount() == 8, "no point is orphaned by deleting just 1 of a cube's 6 faces");
		}
		{
			// Whittle a cube down to its last face and confirm the guard
			// refuses to go further.
			EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));
			for (int i = 0; i < 5; i++)
				mesh.DeleteFace(0);
			Check(mesh.FaceCount() == 1, "5 deletions leave exactly 1 face");
			Check(!mesh.CanDeleteFace(0), "CanDeleteFace refuses to remove the last remaining face");
		}

		GS_TRACE("EditableMeshEditTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 2: Confirm it fails to build** (the four methods don't exist yet)

- [ ] **Step 3: Implement the four operations**

```cpp
	void MovePoint(int point, const glm::vec3& newPosition)
	{
		m_Points[(size_t)point].Position = newPosition;
		RecalculateNormalsTouching(point);
	}

	bool CanDeleteFace(int face) const { return m_Faces.size() > 1; }

	bool CanDelete(int point) const
	{
		// Deleting a point removes every face that touches it -- refuse if
		// that would be every remaining face.
		int facesTouchingPoint = 0;
		for (const EditFace& face : m_Faces)
			if (std::find(face.Points.begin(), face.Points.end(), point) != face.Points.end())
				facesTouchingPoint++;
		return facesTouchingPoint < (int)m_Faces.size();
	}

	void DeletePoint(int point)
	{
		std::vector<EditFace> remaining;
		for (const EditFace& face : m_Faces)
			if (std::find(face.Points.begin(), face.Points.end(), point) == face.Points.end())
				remaining.push_back(face);
		m_Faces = remaining;

		RemovePointAndReindex(point);
	}

	void DeleteFace(int face)
	{
		m_Faces.erase(m_Faces.begin() + face);
		DropOrphanedPoints();
	}

private:
	void RecalculateNormalsTouching(int point)
	{
		for (EditFace& face : m_Faces)
		{
			if (std::find(face.Points.begin(), face.Points.end(), point) == face.Points.end())
				continue;

			const glm::vec3& a = m_Points[(size_t)face.Points[0]].Position;
			const glm::vec3& b = m_Points[(size_t)face.Points[1]].Position;
			const glm::vec3& c = m_Points[(size_t)face.Points[2]].Position;
			face.Normal = glm::normalize(glm::cross(b - a, c - a));
		}
	}

	// Removes `point` and shifts every face's indices above it down by one,
	// so point indices stay a dense 0..N-1 range (Rebuild and the tests
	// both assume that).
	void RemovePointAndReindex(int point)
	{
		m_Points.erase(m_Points.begin() + point);
		for (EditFace& face : m_Faces)
			for (int& p : face.Points)
				if (p > point)
					p--;
	}

	void DropOrphanedPoints()
	{
		std::vector<bool> used(m_Points.size(), false);
		for (const EditFace& face : m_Faces)
			for (int p : face.Points)
				used[(size_t)p] = true;

		// Walk from the end so RemovePointAndReindex's shifting never
		// invalidates an index this loop hasn't visited yet.
		for (int i = (int)m_Points.size() - 1; i >= 0; i--)
			if (!used[(size_t)i])
				RemovePointAndReindex(i);
	}
public:
```

(The `private:`/`public:` markers above show where these land relative to
the class's existing public operations -- add `#include <algorithm>` for
`std::find`.)

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/editablemeshedit.png --capture-step 2
```
Expected: `EditableMeshEditTest: 7 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

- [ ] **Step 6: Mark task done**

---

### Task 5: `ExtrudeFace`, `SplitEdge`

**Files:**
- Modify: `TestEnv/src/EditableMesh.h`
- Test: `TestEnv/src/EditableMeshExtrudeTest.h` (temporary)

**Interfaces:**
- Consumes: Task 4's `EditableMesh`
- Produces: `void ExtrudeFace(int face, std::vector<int>& outNewPoints)`,
  `int SplitEdge(int pointA, int pointB)` — Task 9's UI calls both directly.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/EditableMeshExtrudeTest.h`:

```cpp
// TEMPORARY -- delete after verifying extrude/split.
#pragma once
#include <GS.h>

#include "EditableMesh.h"

namespace EditableMeshExtrudeTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		{
			EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));
			int pointsBefore = mesh.PointCount();   // 8
			int facesBefore = mesh.FaceCount();     // 6

			std::vector<int> newPoints;
			mesh.ExtrudeFace(0, newPoints);

			Check(newPoints.size() == 4, "extruding a quad face produces 4 new points");
			Check(mesh.PointCount() == pointsBefore + 4, "8 -> 12 points after extruding one quad");
			// +1 for the new cap (the old face's point list now names the
			// new points), +4 for the 4 new side walls connecting old to new.
			Check(mesh.FaceCount() == facesBefore + 4, "6 -> 10 faces: the 4 new walls (the cap face is reused, not new)");

			for (int i = 0; i < 4; i++)
				Check(mesh.Point(newPoints[(size_t)i]).Position == mesh.Point(mesh.Face(0).Points[(size_t)i]).Position,
					"new point " + std::to_string(i) + " starts exactly coincident with its original corner");
		}
		{
			EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));
			int a = mesh.Face(0).Points[0];
			int b = mesh.Face(0).Points[1];
			int pointsBefore = mesh.PointCount();

			int newPoint = mesh.SplitEdge(a, b);

			Check(mesh.PointCount() == pointsBefore + 1, "splitting an edge adds exactly 1 point");
			glm::vec3 expectedMidpoint = (mesh.Point(a).Position + mesh.Point(b).Position) * 0.5f;
			Check(mesh.Point(newPoint).Position == expectedMidpoint, "the new point sits at the exact midpoint");

			// A cube is closed and watertight -- every edge, including this
			// one, is shared by exactly 2 faces (there is no such thing as
			// a "boundary" edge on a cube). SplitEdge must thread the new
			// point into *both*, not just the one it was picked from.
			int facesContainingNewPoint = 0;
			for (int f = 0; f < mesh.FaceCount(); f++)
				if (std::find(mesh.Face(f).Points.begin(), mesh.Face(f).Points.end(), newPoint) != mesh.Face(f).Points.end())
					facesContainingNewPoint++;
			Check(facesContainingNewPoint == 2,
				"the new point is threaded into both faces that share the split edge (found in " + std::to_string(facesContainingNewPoint) + ")");
		}

		GS_TRACE("EditableMeshExtrudeTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 2: Confirm it fails to build**

- [ ] **Step 3: Implement `ExtrudeFace` and `SplitEdge`**

```cpp
	void ExtrudeFace(int faceIndex, std::vector<int>& outNewPoints)
	{
		EditFace original = m_Faces[(size_t)faceIndex];
		int pointCount = (int)original.Points.size();

		outNewPoints.clear();
		for (int oldPoint : original.Points)
		{
			int newIndex = (int)m_Points.size();
			m_Points.push_back({ m_Points[(size_t)oldPoint].Position });
			outNewPoints.push_back(newIndex);
		}

		// One wall per edge of the original face, connecting each original
		// point to its new duplicate -- wound so the wall faces outward,
		// matching the direction the cap face's own extrusion moves.
		for (int i = 0; i < pointCount; i++)
		{
			int oldA = original.Points[(size_t)i];
			int oldB = original.Points[(size_t)((i + 1) % pointCount)];
			int newA = outNewPoints[(size_t)i];
			int newB = outNewPoints[(size_t)((i + 1) % pointCount)];

			EditFace wall;
			wall.Points = { oldA, oldB, newB, newA };
			const glm::vec3& a = m_Points[(size_t)wall.Points[0]].Position;
			const glm::vec3& b = m_Points[(size_t)wall.Points[1]].Position;
			const glm::vec3& c = m_Points[(size_t)wall.Points[2]].Position;
			wall.Normal = glm::normalize(glm::cross(b - a, c - a));
			m_Faces.push_back(wall);
		}

		// The cap: the original face's slot now names the new (duplicate)
		// points instead of the old ones -- the old ones stay behind as the
		// base the walls connect to.
		m_Faces[(size_t)faceIndex].Points = outNewPoints;
	}

	int SplitEdge(int pointA, int pointB)
	{
		glm::vec3 midpoint = (m_Points[(size_t)pointA].Position + m_Points[(size_t)pointB].Position) * 0.5f;
		int newPoint = (int)m_Points.size();
		m_Points.push_back({ midpoint });

		for (EditFace& face : m_Faces)
		{
			int indexA = -1, indexB = -1;
			for (size_t i = 0; i < face.Points.size(); i++)
			{
				if (face.Points[i] == pointA) indexA = (int)i;
				if (face.Points[i] == pointB) indexB = (int)i;
			}
			if (indexA < 0 || indexB < 0)
				continue;

			// Insert the new point between A and B in whichever order they
			// actually appear in this face's own winding.
			int laterIndex = std::max(indexA, indexB);
			bool adjacentWrap = (std::abs(indexA - indexB) == (int)face.Points.size() - 1);
			int insertAt = adjacentWrap ? 0 : laterIndex;
			face.Points.insert(face.Points.begin() + insertAt, newPoint);
		}

		return newPoint;
	}
```

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/editablemeshextrude.png --capture-step 2
```
Expected: `EditableMeshExtrudeTest: 10 passed, 0 failed` (3 + 4-in-a-loop = 7
checks for extrude, 3 for split-edge).

- [ ] **Step 5: Remove the temporary test**

- [ ] **Step 6: Mark task done**

---

### Task 6: Undo/redo for `EditableMesh`

**Files:**
- Modify: `TestEnv/src/EditableMesh.h`
- Test: `TestEnv/src/EditableMeshUndoTest.h` (temporary)

**Interfaces:**
- Consumes: Tasks 2-5's `EditableMesh`
- Produces: `void PushUndo()`, `void Undo()`, `void Redo()`,
  `bool CanUndo() const`, `bool CanRedo() const` — Task 9's UI calls
  `PushUndo()` before each operation and wires `Undo`/`Redo` to Ctrl+Z/
  Ctrl+Shift+Z while an edit session is open.

Same snapshot-per-operation shape `TextBuffer`'s own undo stack already
uses (`TestEnv/src/TextBuffer.h`): copy the whole (small) state before an
edit, rather than diffing it.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/EditableMeshUndoTest.h`:

```cpp
// TEMPORARY -- delete after verifying EditableMesh's undo/redo.
#pragma once
#include <GS.h>

#include "EditableMesh.h"

namespace EditableMeshUndoTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));

		mesh.PushUndo();
		mesh.MovePoint(0, { 9.0f, 9.0f, 9.0f });
		Check(mesh.Point(0).Position == glm::vec3(9.0f, 9.0f, 9.0f), "move applied");

		mesh.PushUndo();
		mesh.DeleteFace(0);
		Check(mesh.FaceCount() == 5, "delete applied (6 -> 5)");

		mesh.Undo();
		Check(mesh.FaceCount() == 6, "undo reverts the delete (5 -> 6)");
		Check(mesh.Point(0).Position == glm::vec3(9.0f, 9.0f, 9.0f), "the earlier move is still in effect after undoing the delete");

		mesh.Undo();
		Check(mesh.Point(0).Position != glm::vec3(9.0f, 9.0f, 9.0f), "a second undo reverts the move too");
		Check(!mesh.CanUndo(), "nothing left to undo");

		mesh.Redo();
		mesh.Redo();
		Check(mesh.FaceCount() == 5, "redoing both steps re-applies the delete (back to 5 faces)");
		Check(!mesh.CanRedo(), "nothing left to redo");

		GS_TRACE("EditableMeshUndoTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 2: Confirm it fails to build**

- [ ] **Step 3: Implement the undo stack**

```cpp
	bool CanUndo() const { return !m_UndoStack.empty(); }
	bool CanRedo() const { return !m_RedoStack.empty(); }

	void PushUndo()
	{
		m_UndoStack.push_back({ m_Points, m_Faces });
		m_RedoStack.clear();
	}

	void Undo()
	{
		if (m_UndoStack.empty())
			return;
		m_RedoStack.push_back({ m_Points, m_Faces });
		Snapshot state = m_UndoStack.back();
		m_UndoStack.pop_back();
		m_Points = state.Points;
		m_Faces = state.Faces;
	}

	void Redo()
	{
		if (m_RedoStack.empty())
			return;
		m_UndoStack.push_back({ m_Points, m_Faces });
		Snapshot state = m_RedoStack.back();
		m_RedoStack.pop_back();
		m_Points = state.Points;
		m_Faces = state.Faces;
	}

private:
	struct Snapshot
	{
		std::vector<EditPoint> Points;
		std::vector<EditFace> Faces;
	};
	std::vector<Snapshot> m_UndoStack;
	std::vector<Snapshot> m_RedoStack;
public:
```

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/editablemeshundo.png --capture-step 2
```
Expected: `EditableMeshUndoTest: 8 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

- [ ] **Step 6: Mark task done**

---

### Task 7: `ObjWriter`

**Files:**
- Create: `GS/src/GS/Renderer/ObjWriter.h`, `GS/src/GS/Renderer/ObjWriter.cpp`
- Modify: `GS/src/GS.h` (add the new header to the umbrella include, next to
  `ObjLoader.h`'s own line)
- Modify: `GS/premake5.lua` or wherever `GS`'s file glob lives — confirm
  new `.cpp`/`.h` files under `GS/src` are picked up automatically by the
  existing glob (this project already globs `GS/src/**.cpp`/`**.h` per
  `CLAUDE.md`'s own build notes — no premake change should be needed, but
  run `./gs.py gen` explicitly in Step 4 below to be sure rather than
  assuming)
- Test: `TestEnv/src/ObjWriterTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::MeshData` (already exists), `GS::ObjLoader::Parse`
  (already exists, used only for this task's round-trip test)
- Produces: `static std::string GS::ObjWriter::Write(const GS::MeshData& data)`,
  `static bool GS::ObjWriter::Save(const std::string& path, const GS::MeshData& data, std::string& error)`
  — Task 9's "Done" step calls `Save`.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/ObjWriterTest.h`:

```cpp
// TEMPORARY -- delete after verifying ObjWriter.
#pragma once
#include <GS.h>

namespace ObjWriterTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS::MeshData original = GS::Mesh::CreateCubeData(1.0f);

		std::string text = GS::ObjWriter::Write(original);
		Check(!text.empty(), "Write produces non-empty text");

		GS::MeshData roundTripped;
		std::string error;
		bool parsed = GS::ObjLoader::Parse(text.c_str(), text.size(), roundTripped, error);
		Check(parsed, "the real, existing ObjLoader can parse what ObjWriter wrote (error: " + error + ")");
		Check(roundTripped.Vertices.size() == original.Vertices.size(),
			"round-tripped vertex count matches (expected " + std::to_string(original.Vertices.size()) + ")");
		Check(roundTripped.TriangleCount() == original.TriangleCount(),
			"round-tripped triangle count matches (expected " + std::to_string(original.TriangleCount()) + ")");

		bool positionsMatch = true;
		for (size_t i = 0; i < original.Vertices.size() && i < roundTripped.Vertices.size(); i++)
			if (glm::distance(original.Vertices[i].Position, roundTripped.Vertices[i].Position) > 1e-4f)
				positionsMatch = false;
		Check(positionsMatch, "every vertex position survives the round trip");

		GS_TRACE("ObjWriterTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 2: Confirm it fails to build** (`ObjWriter` doesn't exist)

- [ ] **Step 3: Write `ObjWriter.h`**

```cpp
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

namespace GS {

	// The write side of ObjLoader -- v/vt/vn/f text, one vertex/triangle at
	// a time. No materials (mtllib/usemtl) are written, matching what
	// ObjLoader itself reads: this project's material system is a separate,
	// later concern, and a rebuilt/edited mesh keeps whatever flat colour
	// or file-sourced material it already had via MeshComponent, untouched
	// by this.
	class GS_API ObjWriter
	{
	public:
		static std::string Write(const MeshData& data);
		static bool Save(const std::string& path, const MeshData& data, std::string& error);
	};

}
```

- [ ] **Step 4: Write `ObjWriter.cpp`**

```cpp
#include "gspch.h"
#include "GS/Renderer/ObjWriter.h"

#include <fstream>
#include <sstream>

namespace GS {

	std::string ObjWriter::Write(const MeshData& data)
	{
		std::ostringstream out;

		for (const MeshVertex& vertex : data.Vertices)
			out << "v " << vertex.Position.x << " " << vertex.Position.y << " " << vertex.Position.z << "\n";

		for (const MeshVertex& vertex : data.Vertices)
			out << "vt " << vertex.TexCoord.x << " " << vertex.TexCoord.y << "\n";

		for (const MeshVertex& vertex : data.Vertices)
			out << "vn " << vertex.Normal.x << " " << vertex.Normal.y << " " << vertex.Normal.z << "\n";

		// .obj indices are 1-based. Each MeshVertex already fuses position/
		// uv/normal together, so a face corner uses the same index three
		// times (v/vt/vn) rather than three separate index streams.
		for (size_t i = 0; i + 2 < data.Indices.size(); i += 3)
		{
			unsigned int a = data.Indices[i] + 1;
			unsigned int b = data.Indices[i + 1] + 1;
			unsigned int c = data.Indices[i + 2] + 1;
			out << "f " << a << "/" << a << "/" << a
				<< " " << b << "/" << b << "/" << b
				<< " " << c << "/" << c << "/" << c << "\n";
		}

		return out.str();
	}

	bool ObjWriter::Save(const std::string& path, const MeshData& data, std::string& error)
	{
		std::ofstream file(path);
		if (!file.is_open())
		{
			error = "could not open '" + path + "' for writing";
			return false;
		}

		file << Write(data);
		return true;
	}

}
```

- [ ] **Step 5: Add `ObjWriter.h` to the `GS.h` umbrella**

In `GS/src/GS.h`, add `#include "GS/Renderer/ObjWriter.h"` on the line
directly after the existing `#include "GS/Renderer/ObjLoader.h"`.

- [ ] **Step 6: Regenerate and build all three configs**

```sh
./gs.py gen
./gs.py build all
```
Expected: clean build; `ObjWriter.cpp` shows up in the build log the way
every other newly-added `.cpp` under `GS/src` does (confirms the file glob
picked it up with no premake change needed).

- [ ] **Step 7: Wire in and run the test**

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/objwriter.png --capture-step 2
```
Expected: `ObjWriterTest: 5 passed, 0 failed`.

- [ ] **Step 8: Remove the temporary test**

- [ ] **Step 9: Mark task done**

---

### Task 8: Edit-mode entry, overlay, and picking

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h`

**Interfaces:**
- Consumes: `EditableMesh` (Tasks 2-6), `GS::Mesh::CreateCubeData` and
  friends + `GS::Mesh::LoadData` (Task 1)
- Produces: `bool m_MeshEditActive`, `EditableMesh m_MeshEditSession`,
  `int m_MeshEditSelectedPoint` — Task 9 consumes all three to wire up
  dragging and the four operations.

This task makes an edit session **enterable and visible** (an "Edit Mesh"
button, every control point drawn as a small overlay cross, clicking one
highlights it) but not yet **editable** — no drag, no operations, no Done/
Cancel. That's Task 9. Splitting here means this task's own capture-based
verification (does the overlay render, does a click select the right
point) doesn't depend on anything Task 9 adds.

- [ ] **Step 1: Add entry state and the Inspector button**

In `EditorSceneView.h`, add near the other `m_Edit*` members (the ones
`m_EditBefore*` etc. already sit beside):

```cpp
	bool m_MeshEditActive = false;
	EditableMesh m_MeshEditSession;
	int m_MeshEditSelectedPoint = -1;
```

Add `#include "EditableMesh.h"` near the top of `EditorSceneView.h`,
alongside its other includes.

In the Inspector's Mesh section (where `mesh.Color`/`mesh.Visible` are
drawn), add an "Edit Mesh" button:

```cpp
			if (ImGui::Button("Edit Mesh") && !m_MeshEditActive && !PlayMode::IsPlaying())
			{
				GS::MeshData data;
				if (mesh->SourcePath.rfind("primitive:cube", 0) == 0)
					data = GS::Mesh::CreateCubeData();
				else if (mesh->SourcePath.rfind("primitive:plane", 0) == 0)
					data = GS::Mesh::CreatePlaneData();
				else if (mesh->SourcePath.rfind("primitive:sphere", 0) == 0)
					data = GS::Mesh::CreateSphereData();
				else if (mesh->SourcePath.rfind("primitive:cylinder", 0) == 0)
					data = GS::Mesh::CreateCylinderData();
				else
				{
					std::string error;
					GS::Mesh::LoadData(mesh->SourcePath, data, error);
				}

				m_MeshEditSession = EditableMesh::FromMeshData(data);
				m_MeshEditSelectedPoint = -1;
				m_MeshEditActive = true;
			}
```

(This sits inside the existing `if (auto* mesh = g_EditorScene.
GetComponent<GS::MeshComponent>(m_Selected))` block, so `mesh` is already
in scope.)

- [ ] **Step 2: Lock outliner selection while a session is open**

Find the Outliner's `ImGui::Selectable(tag->Name.c_str(), ...)` call (the
one building the entity list) and guard it:

```cpp
			ImGui::BeginDisabled(m_MeshEditActive);
			if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
				m_Selected = entity;
			ImGui::EndDisabled();
```

- [ ] **Step 3: Draw the point overlay**

Add a new method, called from the same place `DrawLightGizmos()` is called
(right after it, inside the `Renderer2D::BeginScene`/`EndScene` block):

```cpp
	void DrawMeshEditOverlay()
	{
		if (!m_MeshEditActive)
			return;

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		glm::mat4 toWorld = transform->GetTransform();

		for (int i = 0; i < m_MeshEditSession.PointCount(); i++)
		{
			glm::vec3 worldPos = glm::vec3(toWorld * glm::vec4(m_MeshEditSession.Point(i).Position, 1.0f));
			glm::vec4 color = (i == m_MeshEditSelectedPoint)
				? glm::vec4(1.0f, 1.0f, 0.4f, 1.0f)
				: glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);

			constexpr float s = 0.03f;
			GS::Renderer2D::DrawLine(worldPos - glm::vec3(s, 0, 0), worldPos + glm::vec3(s, 0, 0), color);
			GS::Renderer2D::DrawLine(worldPos - glm::vec3(0, s, 0), worldPos + glm::vec3(0, s, 0), color);
			GS::Renderer2D::DrawLine(worldPos - glm::vec3(0, 0, s), worldPos + glm::vec3(0, 0, s), color);
		}
	}
```

Add the call: `DrawMeshEditOverlay();` immediately after the existing
`DrawLightGizmos();` line.

- [ ] **Step 4: Add point picking**

Add a method used by the mouse-click handling added in Task 9, but written
now since it belongs with the rest of the picking code:

```cpp
	int PickMeshEditPoint(const glm::vec2& mouse) const
	{
		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return -1;

		glm::mat4 toWorld = transform->GetTransform();

		int best = -1;
		float bestDistance = 12.0f;   // pixels -- generous enough to click a small on-screen cross without needing pixel precision
		for (int i = 0; i < m_MeshEditSession.PointCount(); i++)
		{
			glm::vec3 worldPos = glm::vec3(toWorld * glm::vec4(m_MeshEditSession.Point(i).Position, 1.0f));
			glm::vec2 screen;
			if (!WorldToScreen(worldPos, screen))
				continue;

			float distance = glm::length(mouse - screen);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = i;
			}
		}
		return best;
	}
```

(`WorldToScreen` and `m_Camera` are the same ones the existing gizmo code
already uses — this method is `const` because it doesn't need to be
anything else, matching the class's existing style for read-only helpers.)

- [ ] **Step 5: Build all three configs**

```sh
./gs.py build all
```
Expected: clean build.

- [ ] **Step 6: Visual verification (temporary hook)**

Add a temporary public method to `EditorSceneView` (removed in this same
step's cleanup below) that does exactly what the "Edit Mesh" button's body
does, callable from outside without clicking anything:

```cpp
	// TEMPORARY -- delete after visually verifying mesh-edit entry/overlay/picking.
	void DebugEnterMeshEditOn(GS::EntityId entity)
	{
		Select(entity);
		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
		if (!mesh)
			return;

		GS::MeshData data;
		if (mesh->SourcePath.rfind("primitive:cube", 0) == 0)
			data = GS::Mesh::CreateCubeData();
		else
		{
			std::string error;
			GS::Mesh::LoadData(mesh->SourcePath, data, error);
		}

		m_MeshEditSession = EditableMesh::FromMeshData(data);
		m_MeshEditSelectedPoint = -1;
		m_MeshEditActive = true;
	}
```

In `EditorShell.h`'s `OnAttach`, after a scene with one placed cube has
loaded:

```cpp
		// TEMPORARY -- delete after visually verifying mesh-edit entry/overlay/picking.
		if (g_EditorSceneView && g_EditorScene.GetEntityCount() > 0)
			g_EditorSceneView->DebugEnterMeshEditOn(g_EditorScene.GetEntities()[0]);
```

Build, run with `--hide-window --lockstep --capture <path> --capture-step
3` against a scene with one placed cube, and confirm visually (crop and
view the capture) that 8 small crosses appear around the cube's corners.
Then remove **both** temporary additions: `DebugEnterMeshEditOn` from
`EditorSceneView.h`, and the `OnAttach` hook from `EditorShell.h`.

- [ ] **Step 7: Byte-identical Cube3D regression check**

Same as Task 1's Step 5 — confirm a `--demo Cube3D --hide-ui` capture is
unchanged (this task's new code only runs when `m_MeshEditActive` is true,
which nothing sets outside the Inspector button).

- [ ] **Step 8: Mark task done**

---

### Task 9: Gizmo retarget, the four operations wired to input, Done/Cancel

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h`

**Interfaces:**
- Consumes: everything above — `EditableMesh`'s full operation set (Tasks
  2-6), `ObjWriter::Save` (Task 7), Task 8's entry/overlay/picking state
- Produces: nothing further consumes this — it's the last task

- [ ] **Step 1: Retarget `GizmoPosition` to support a mesh-edit selection**

Find `GizmoPosition()` (returns `glm::vec3*`, used by `UpdateGizmo`/
`DrawGizmo`/`AxisScreenDistance`) and change it to check mesh-edit state
first:

```cpp
	glm::vec3* GizmoPosition()
	{
		if (m_MeshEditActive && m_MeshEditSelectedPoint >= 0)
			return &m_MeshEditSessionWorldPoint;   // see Step 2 -- kept in sync each frame, since EditableMesh stores object space and the gizmo drags in world space

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		return transform ? &transform->Position : nullptr;
	}
```

Add the new member `glm::vec3 m_MeshEditSessionWorldPoint{ 0.0f };` next to
`m_MeshEditSelectedPoint`.

- [ ] **Step 2: Keep the world-space mirror in sync, and write drags back to object space**

This needs **two** separate calls, not one, because they must straddle
`UpdateGizmo()` itself: refreshing the world mirror from the real
(object-space) point must happen *before* `UpdateGizmo()` runs (so a
freshly-selected point, or one an Undo just moved, has the right starting
position if a drag begins this very frame); writing a drag's result back
into object space must happen *after* `UpdateGizmo()` runs (so it captures
the delta *this* frame's drag actually applied, not last frame's). One
function called at one point in the frame cannot correctly do both.

```cpp
	// Before UpdateGizmo(): make the world-space mirror agree with the real,
	// object-space point -- unless a drag is already in progress, in which
	// case UpdateGizmo is the one actively moving the mirror this frame and
	// this must not stomp that with a stale value computed before the drag.
	void SyncMeshEditWorldPointBeforeGizmo()
	{
		if (!m_MeshEditActive || m_MeshEditSelectedPoint < 0 || m_DragAxis >= 0)
			return;

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		glm::vec3 objectSpace = m_MeshEditSession.Point(m_MeshEditSelectedPoint).Position;
		m_MeshEditSessionWorldPoint = glm::vec3(transform->GetTransform() * glm::vec4(objectSpace, 1.0f));
	}

	// After UpdateGizmo(): if it just moved the mirror (a drag was in
	// progress when it ran), write that new world position back into
	// EditableMesh's own object-space storage via the inverse transform.
	void SyncMeshEditWorldPointAfterGizmo()
	{
		if (!m_MeshEditActive || m_MeshEditSelectedPoint < 0 || m_DragAxis < 0)
			return;

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		glm::vec3 newObjectSpace = glm::vec3(glm::inverse(transform->GetTransform()) * glm::vec4(m_MeshEditSessionWorldPoint, 1.0f));
		m_MeshEditSession.MovePoint(m_MeshEditSelectedPoint, newObjectSpace);
		m_MeshEditPreviewDirty = true;
	}
```

Add `bool m_MeshEditPreviewDirty = false;` next to the other new members.
Call `SyncMeshEditWorldPointBeforeGizmo();` immediately **before** the
existing `UpdateGizmo();` call in `OnUpdate`, and
`SyncMeshEditWorldPointAfterGizmo();` immediately **after** it.

- [ ] **Step 3: Rebuild the live preview mesh when the topology changes**

```cpp
	void RebuildMeshEditPreviewIfNeeded()
	{
		if (!m_MeshEditActive || !m_MeshEditPreviewDirty)
			return;

		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected);
		if (mesh)
			mesh->Geometry.reset(new GS::Mesh(m_MeshEditSession.Rebuild(), "MeshEditPreview"));

		m_MeshEditPreviewDirty = false;
	}
```

Call it right after `SyncMeshEditWorldPointAfterGizmo();` (so it picks up
a drag's result the same frame). Step 5's operation buttons (Delete Point,
etc.) also set `m_MeshEditPreviewDirty = true` directly from their own
`OnImGuiRender`-side click handlers, which run *after* this frame's
`OnUpdate` already has — those are picked up and rebuilt on the *next*
frame's `OnUpdate` instead of the same one. A one-frame-late preview
update for a button click is an invisible, acceptable simplification;
it is not the same class of problem the drag-sync ordering above was,
which would have baked a genuinely *wrong* final vertex position into the
mesh, not just delayed a redraw.

- [ ] **Step 4: Wire point-picking into the mouse-click handling**

Find where mouse clicks are already handled for entity picking/selection
(the same input pass `UpdateGizmo`'s `justPressed` reads from) and add,
guarded so it only applies while a session is open and the click isn't on
a gizmo handle:

```cpp
		if (m_MeshEditActive && justPressed && m_HoverAxis < 0)
		{
			int picked = PickMeshEditPoint(mouse);
			if (picked >= 0)
			{
				m_MeshEditSession.PushUndo();
				m_MeshEditSelectedPoint = picked;
			}
		}
```

(Placed in `UpdateGizmo`, right after `justPressed`/`down` are computed and
before the existing `if (!down)` early-exit block, so it runs on the same
click that would otherwise start a gizmo drag — picking a new point and
beginning to drag it happen in the same click-down the ordinary gizmo flow
already uses for a whole entity.)

- [ ] **Step 5: Add Extrude / Delete Point / Delete Face / Split Edge buttons and Done / Cancel**

Add a small panel drawn only while `m_MeshEditActive`, in the Inspector
right after the "Edit Mesh" button from Task 8:

```cpp
			if (m_MeshEditActive)
			{
				ImGui::SeparatorText("Mesh Edit");

				bool hasSelection = m_MeshEditSelectedPoint >= 0;

				if (!hasSelection) ImGui::BeginDisabled();
				if (ImGui::Button("Delete Point") && m_MeshEditSession.CanDelete(m_MeshEditSelectedPoint))
				{
					m_MeshEditSession.PushUndo();
					m_MeshEditSession.DeletePoint(m_MeshEditSelectedPoint);
					m_MeshEditSelectedPoint = -1;
					m_MeshEditPreviewDirty = true;
				}
				if (!hasSelection) ImGui::EndDisabled();

				if (ImGui::Button("Undo##meshedit") && m_MeshEditSession.CanUndo())
				{
					m_MeshEditSession.Undo();
					m_MeshEditPreviewDirty = true;
				}
				ImGui::SameLine();
				if (ImGui::Button("Redo##meshedit") && m_MeshEditSession.CanRedo())
				{
					m_MeshEditSession.Redo();
					m_MeshEditPreviewDirty = true;
				}

				if (ImGui::Button("Done"))
				{
					GS::MeshData finalData = m_MeshEditSession.Rebuild();

					// TagComponent::Name for the export filename -- fetched
					// independently rather than assumed in scope: the ID
					// field's own `if (auto* tag = ...)` block (further up
					// this same Inspector) is a separate `if`, already
					// closed by here. A missing tag (shouldn't happen --
					// every entity gets one at creation) falls back to a
					// generic name rather than crashing on a null dereference.
					auto* entityTag = g_EditorScene.GetComponent<GS::TagComponent>(m_Selected);
					std::string exportName = entityTag ? entityTag->Name : "Mesh";
					std::string exportPath = "assets/" + exportName + ".obj";

					// Collision suffix -- default entity names are literally
					// "Cube"/"Sphere"/etc. (PlaceEntityCommand's own
					// defaults), so two never-renamed entities colliding on
					// the same export path is a real, likely case, not a
					// hypothetical one. Skipped when this entity's own
					// SourcePath already *is* the target path -- re-editing
					// the same entity a second time should overwrite its own
					// previous export, not spawn Cube1.obj, Cube2.obj, ...
					// forever.
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

					std::string error;
					if (GS::ObjWriter::Save(exportPath, finalData, error))
					{
						std::string oldPath = mesh->SourcePath;
						mesh->SourcePath = exportPath;
						mesh->Geometry = GS::MeshCache::Get(exportPath);
						EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, std::string>>(
							m_Selected, &GS::MeshComponent::SourcePath, oldPath, exportPath));
					}

					m_MeshEditActive = false;
					m_MeshEditSelectedPoint = -1;
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
				{
					auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
					if (transform)
						mesh->Geometry = GS::MeshCache::Get(mesh->SourcePath);   // discard the live preview -- reload the entity's real, unchanged mesh

					m_MeshEditActive = false;
					m_MeshEditSelectedPoint = -1;
				}
			}
```

(`mesh` is already in scope — this whole block sits inside the existing
`if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected))`
block from Task 8's Step 1, which is where the "Edit Mesh" button itself
lives too.)

- [ ] **Step 6: Build all three configs**

```sh
./gs.py build all
```
Expected: clean build.

- [ ] **Step 7: Visual verification (temporary hook)**

Re-add `DebugEnterMeshEditOn` (Task 8, Step 6) plus one more temporary
method that drives a move and a save without needing real mouse input:

```cpp
	// TEMPORARY -- delete after visually verifying the move+Done pipeline.
	void DebugMoveMeshEditPointAndSave(int point, const glm::vec3& newObjectSpacePosition)
	{
		m_MeshEditSession.PushUndo();
		m_MeshEditSession.MovePoint(point, newObjectSpacePosition);
		m_MeshEditPreviewDirty = true;
		RebuildMeshEditPreviewIfNeeded();

		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected);
		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(m_Selected);
		if (!mesh || !tag)
			return;

		GS::MeshData finalData = m_MeshEditSession.Rebuild();
		std::string exportPath = "assets/" + tag->Name + ".obj";
		std::string error;
		if (GS::ObjWriter::Save(exportPath, finalData, error))
		{
			mesh->SourcePath = exportPath;
			mesh->Geometry = GS::MeshCache::Get(exportPath);
		}

		m_MeshEditActive = false;
	}
```

In `EditorShell.h`'s `OnAttach`, after `DebugEnterMeshEditOn(...)`:

```cpp
		// TEMPORARY -- delete after visually verifying the move+Done pipeline.
		g_EditorSceneView->DebugMoveMeshEditPointAndSave(0, { 3.0f, 3.0f, 3.0f });
```

Build, run with `--hide-window --lockstep --hide-ui --capture <path>
--capture-step 3`, and confirm visually that the cube's corner has
actually moved far off to one side (a corner at object-space `(3, 3, 3)`
should be unmistakable against the rest of a unit cube). Separately,
confirm `assets/Cube.obj` (or whatever the scene's placed entity is
tagged) now exists on disk and that `Mesh::LoadData` on it produces a
vertex at `(3, 3, 3)` — read it back with a quick, temporary check in the
same hook, or open the file directly and confirm the `v 3 3 3` line is
present.

Remove `DebugEnterMeshEditOn`, `DebugMoveMeshEditPointAndSave`, and both
`OnAttach` hook lines once this passes.

- [ ] **Step 8: Byte-identical Cube3D regression check**

Same as prior tasks — confirm `--demo Cube3D --hide-ui` is unchanged.

- [ ] **Step 9: Mark task done**

---

## Self-Review Notes

- **Spec coverage:** welding/face-grouping (Tasks 2-3), all four operations
  (Tasks 4-5), undo/redo (Task 6), the `Mesh::Create*`/`Load` split (Task
  1), `ObjWriter` (Task 7), entry/overlay/picking (Task 8), gizmo retarget
  and Done/Cancel (Task 9) — every section of the spec has a task.
- **Reordering vs. the spec's own bottom-up suggestion:** the
  `Mesh::Create*`/`Load` split moved to Task 1, ahead of `EditableMesh`,
  because `EditableMesh`'s own tests need real sample data (a cube's
  actual 24-vertex/12-triangle shape) to assert anything meaningful
  against — `EditableMesh` came first in discussion, but genuinely depends
  on the split for its own verification.
- **Task 8/9 split:** entry+overlay+picking (visible, clickable, but inert)
  is independently reviewable from gizmo-retarget+operations+Done/Cancel
  (makes it actually functional) — a reviewer could accept Task 8 and
  reject Task 9 without the acceptance being meaningless.
