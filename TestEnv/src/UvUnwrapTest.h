// TEMPORARY -- delete after verifying UvUnwrap's HasUsableUVs, Unwrap, and
// FindIslandsFromUVs against known primitives and hand-built meshes.
#pragma once
#include <GS.h>
#include <GS/Renderer/UvUnwrap.h>
#include <GS/Renderer/Mesh.h>
#include <unordered_set>

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

		GS_TRACE("UvUnwrapTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
