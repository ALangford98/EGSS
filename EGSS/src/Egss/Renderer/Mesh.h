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
	// A run of indices drawn with one material. An .obj switches material with
	// `usemtl` partway through its faces, so a file with three materials is one
	// vertex buffer and three ranges into it -- not three meshes.
	struct Submesh
	{
		// The `usemtl` name, to be looked up in whatever the `mtllib` loaded.
		// Empty for faces declared before any `usemtl`, which is also what a
		// file with no materials at all produces.
		std::string Material;

		// glTF's answer to the same question. It numbers its materials and its
		// names are optional and need not be unique, so a name cannot stand in
		// for the index -- two materials called "" are two materials. Both
		// fields are here because neither format's answer can express the
		// other's; -1 means "this loader does not work that way".
		int MaterialIndex = -1;

		unsigned int FirstIndex = 0;
		unsigned int IndexCount = 0;
	};

	struct MeshData
	{
		std::vector<MeshVertex> Vertices;
		std::vector<unsigned int> Indices;

		// Always at least one, covering everything, so a caller never has to
		// special-case a file that named no materials.
		std::vector<Submesh> Submeshes;

		// `mtllib` references, in the order the file gave them and exactly as
		// written -- they are relative to the .obj, and only whoever opened it
		// knows where that was.
		std::vector<std::string> MaterialLibraries;

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

		// A tube about the **y** axis, open at both ends, from -halfHeight to
		// +halfHeight. No caps, because the one thing this exists for is the
		// shaft of a capsule, whose ends are covered by hemispheres.
		//
		// Open-ended also makes it the one primitive that survives a
		// non-uniform scale: scale x and z together for radius and y for
		// length, and it is still exactly a cylinder. A capsule mesh would
		// not be -- stretching one turns its round caps into ellipsoids -- so
		// a cylinder plus two spheres is how a capsule of any proportions gets
		// drawn correctly from two fixed meshes.
		static Mesh* CreateCylinder(float radius = 0.5f, float halfHeight = 0.5f,
			unsigned int segments = 24);

		const std::shared_ptr<VertexArray>& GetVertexArray() const { return m_VertexArray; }

		// **Per-instance data, attached once and refilled as often as you
		// like.** The buffer's layout carries a divisor, so its attributes
		// advance once per copy of the mesh rather than once per vertex --
		// see `BufferLayout` and `Renderer::SubmitInstanced`.
		//
		// It becomes part of this mesh's vertex array, which is the whole
		// point and also the caveat: a mesh with one attached should be drawn
		// instanced. A plain `Submit` still works and draws one copy using the
		// first entry in the buffer, which is a sensible thing to see and not
		// a sensible thing to rely on.
		void SetInstanceBuffer(const std::shared_ptr<VertexBuffer>& buffer)
		{
			m_InstanceBuffer = buffer;
			m_VertexArray->AddVertexBuffer(buffer);
		}

		const std::shared_ptr<VertexBuffer>& GetInstanceBuffer() const
		{
			return m_InstanceBuffer;
		}

		// One entry per material the file switched to, in draw order. Always at
		// least one: a mesh with no materials has a single unnamed submesh
		// covering everything, so a caller can loop unconditionally.
		const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
		const std::vector<std::string>& GetMaterialLibraries() const { return m_MaterialLibraries; }

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
		std::shared_ptr<VertexBuffer> m_InstanceBuffer;
		std::string m_Name;

		std::vector<Submesh> m_Submeshes;
		std::vector<std::string> m_MaterialLibraries;

		size_t m_VertexCount = 0;
		size_t m_TriangleCount = 0;

		glm::vec3 m_BoundsMin = glm::vec3(0.0f);
		glm::vec3 m_BoundsMax = glm::vec3(0.0f);
	};

}
