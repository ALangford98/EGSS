#pragma once

// A mesh's editable topology: vertices welded by position into draggable
// control points, editor-only and rendering-agnostic -- pure data and
// operations, no ImGui, the same separation TextBuffer.h uses for the text
// editor. See docs/superpowers/specs/2026-09-12-mesh-authoring-design.md.

#include <GS.h>

#include <algorithm>
#include <glm/glm.hpp>
#include <map>
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
			wall.Normal = original.Normal;
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

protected:
	std::vector<EditPoint> m_Points;
	std::vector<EditFace> m_Faces;

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

			glm::vec3 crossProduct = glm::cross(b - a, c - a);
			float length = glm::length(crossProduct);
			// A degenerate cross product (a drag has put two of this face's
			// points on top of each other, or collinear) has nothing to
			// normalise -- keep the face's last valid normal rather than
			// reach the GPU with NaN. Same guard as Mesh.cpp's own
			// RecalculateNormals, applied here because dragging a point onto
			// another is reachable through the wired Move path, not just a
			// theoretical input.
			if (length > 1e-8f)
				face.Normal = crossProduct / length;
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
};

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

	mesh.GroupCoplanarFaces();

	return mesh;
}
