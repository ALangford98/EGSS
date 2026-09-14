#include "gspch.h"
#include "GS/Renderer/UvUnwrap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
		// (packExtent - packWidth) etc. are raw pack-space quantities;
		// the offset is added to an already-*scaled* coordinate below, so
		// it needs the same scale applied -- otherwise the shorter axis's
		// content is shifted by a raw-space amount inside a unit-space
		// target and spills past 1.0 (caught by UvUnwrapTest's own
		// packed-UVs-in-bounds check, not assumed correct).
		glm::vec2 centerOffset(scale * (packExtent - packWidth) * 0.5f, scale * (packExtent - packHeight) * 0.5f);

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
