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
		// (see the plan's Global Constraints). Returns the chart id of
		// each resulting triangle, in the same order as the rewritten
		// data.Indices.
		static ChartAssignment Unwrap(MeshData& data);

		// Read-only counterpart for a mesh whose existing UVs are kept:
		// two triangles sharing a mesh edge are the same island only when
		// their corresponding UV-space edge endpoints match too. No
		// mutation, no geometry decisions -- just reports where the
		// existing seams already are.
		static ChartAssignment FindIslandsFromUVs(const MeshData& data);

		// The angle threshold Unwrap()'s chart growth uses, tuned
		// empirically against CreateCubeData()/CreateSphereData()/
		// CreateCylinderData() -- not user-exposed.
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
