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

		GS_TRACE("UvUnwrapTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
