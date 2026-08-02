#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace Egss {

	// One vertex of 3D geometry. Position, normal and texture coordinate is the
	// smallest set that supports lighting and texturing, and it is what the
	// layout in Mesh.cpp declares -- change one and you must change the other.
	//
	// Tangents would go here too once normal mapping exists. They are left out
	// rather than left zeroed, because a zeroed tangent is a bug waiting to be
	// found in a shader rather than at load time.
	struct MeshVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
	};

	// Geometry before it reaches the GPU.
	//
	// The loaders produce this and nothing else, so parsing a file involves no
	// GL calls, needs no context, and can be tested by looking at numbers. That
	// separation is the reason a mesh can be verified without a window.
	struct MeshData
	{
		std::vector<MeshVertex> Vertices;
		std::vector<unsigned int> Indices;

		// Filled by RecalculateBounds. Useful for framing a camera on a model
		// whose size you do not know in advance -- which is every loaded file.
		glm::vec3 BoundsMin = glm::vec3(0.0f);
		glm::vec3 BoundsMax = glm::vec3(0.0f);

		glm::vec3 BoundsCentre() const { return (BoundsMin + BoundsMax) * 0.5f; }
		glm::vec3 BoundsSize() const { return BoundsMax - BoundsMin; }
		// Radius of the sphere that contains the box, which is what a camera
		// wants: fit this and nothing pokes out of frame whatever the rotation.
		float BoundsRadius() const { return glm::length(BoundsSize()) * 0.5f; }

		void RecalculateBounds();

		// Area-weighted vertex normals, for geometry that carries none.
		// Weighting by area rather than averaging face normals equally stops
		// one small triangle from pulling a normal as hard as a large one.
		//
		// `keep` is an optional per-vertex mask of normals already supplied,
		// which are left untouched -- a file may give normals for some faces
		// and not others, and what it gave is better than anything derivable.
		void RecalculateNormals(const std::vector<bool>* keep = nullptr);

		size_t TriangleCount() const { return Indices.size() / 3; }
	};

	// Geometry on the GPU: a vertex array, plus the numbers worth knowing about
	// what is in it.
	//
	// A Mesh is not a model. There is no material, no transform and no
	// hierarchy here -- a transform belongs to whatever is *using* the mesh
	// (which is why TransformComponent exists), and materials are still
	// outstanding. Keeping those out means the same mesh can be drawn a
	// thousand times without being copied once.
	class EGSS_API Mesh
	{
	public:
		explicit Mesh(const MeshData& data, const std::string& name = "Mesh");

		// Loads an .obj. Returns nullptr and logs on failure rather than
		// throwing, since a missing model should not take the program with it.
		static Mesh* Load(const std::string& path);

		// --- Primitives -------------------------------------------------
		// Built rather than loaded, so a demo needs no asset files.

		// 24 vertices, not 8: a cube corner has three different normals
		// depending on which face you are on, and a vertex carries one.
		static Mesh* CreateCube(float size = 1.0f);
		// Facing +Y, so it works as a floor.
		static Mesh* CreatePlane(float size = 1.0f);
		// UV sphere. Segments run around the equator, rings from pole to pole.
		static Mesh* CreateSphere(float radius = 0.5f,
			unsigned int segments = 32, unsigned int rings = 16);

		const std::shared_ptr<VertexArray>& GetVertexArray() const { return m_VertexArray; }

		const std::string& GetName() const { return m_Name; }

		size_t GetVertexCount() const { return m_VertexCount; }
		size_t GetTriangleCount() const { return m_TriangleCount; }

		const glm::vec3& GetBoundsMin() const { return m_BoundsMin; }
		const glm::vec3& GetBoundsMax() const { return m_BoundsMax; }
		glm::vec3 GetBoundsCentre() const { return (m_BoundsMin + m_BoundsMax) * 0.5f; }
		glm::vec3 GetBoundsSize() const { return m_BoundsMax - m_BoundsMin; }
		float GetBoundsRadius() const { return glm::length(m_BoundsMax - m_BoundsMin) * 0.5f; }
	private:
		std::shared_ptr<VertexArray> m_VertexArray;
		std::string m_Name;

		size_t m_VertexCount = 0;
		size_t m_TriangleCount = 0;

		glm::vec3 m_BoundsMin = glm::vec3(0.0f);
		glm::vec3 m_BoundsMax = glm::vec3(0.0f);
	};

}
