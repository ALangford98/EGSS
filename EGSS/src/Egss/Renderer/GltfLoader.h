#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Mesh.h"

#include <glm/glm.hpp>

namespace Egss {

	// glTF 2.0 -- "the JPEG of 3D", and the format worth supporting after .obj.
	//
	// What it has that .obj does not, and why this exists:
	//
	//   * **A node hierarchy.** An .obj is one bag of triangles; a glTF is a
	//     tree of transforms, so a wheel can be a child of a car and be placed
	//     once rather than baked into the vertices.
	//   * **Real materials.** Metallic-roughness PBR with factors *and*
	//     textures, rather than .mtl's Phong exponent and a guess.
	//   * **Binary data.** Vertices are raw bytes in a buffer, not decimal text
	//     that has to be re-parsed and re-rounded. A model loads in one read
	//     and a memcpy instead of a million calls to strtod.
	//
	// Both containers are read: `.gltf` (JSON, with data beside it or inlined
	// as a data: URI) and `.glb` (the same JSON in a chunked binary wrapper).
	//
	// Not yet: skinning and animation. They are the reason to want glTF *next*,
	// and they are a separate piece -- the joints, weights and samplers have
	// nowhere to be played back to until there is something that poses a
	// skeleton, and a parser filling structures nothing reads is exactly the
	// "component with no system" this project declines to build.

	// One image, still compressed, exactly as the file supplied it.
	//
	// Two shapes because glTF has two: a `.gltf` names a PNG beside it, and a
	// `.glb` carries the PNG's bytes inside itself. Both end up here rather
	// than as a texture, for the same reason ObjMaterial is not an
	// Egss::Material -- parsing must not need a GL context, and a model should
	// be loadable in a test that has no window.
	struct EGSS_API GltfImage
	{
		std::string Name;

		// Set when the image is a file beside the model. Already resolved
		// against the model's directory and percent-decoded, so it can be
		// opened as it stands.
		std::string Path;

		// Set when the image was embedded -- a data: URI, or a bufferView in a
		// .glb. Compressed PNG/JPEG bytes, not pixels.
		std::vector<unsigned char> Bytes;
		std::string MimeType;

		bool IsEmbedded() const { return !Bytes.empty(); }
	};

	// glTF's metallic-roughness model. Every field is a factor *and* optionally
	// a texture, and the two multiply -- a white texture with a red factor is a
	// red surface, which is how one texture serves several materials.
	struct EGSS_API GltfMaterial
	{
		std::string Name;

		glm::vec4 BaseColour = glm::vec4(1.0f);
		float Metallic = 1.0f;
		float Roughness = 1.0f;
		glm::vec3 Emissive = glm::vec3(0.0f);

		// Indices into GltfModel::Images, or -1. glTF puts a `sampler` between
		// the material and the image; this collapses that, because nothing here
		// reads wrap modes yet and pretending otherwise would be a lie about
		// what the engine does.
		int BaseColourImage = -1;
		int MetallicRoughnessImage = -1;
		int NormalImage = -1;
		int OcclusionImage = -1;
		int EmissiveImage = -1;

		// Which UV set each of those samples. Almost always 0, and a model that
		// uses 1 and is drawn with 0 looks subtly, confusingly wrong.
		int BaseColourUV = 0;

		bool DoubleSided = false;

		enum class Alpha { Opaque, Mask, Blend };
		Alpha AlphaMode = Alpha::Opaque;
		float AlphaCutoff = 0.5f;
	};

	// One node of the scene tree.
	struct EGSS_API GltfNode
	{
		std::string Name;

		// Index into GltfModel::Meshes, or -1 for a node that only positions
		// its children -- which most nodes are.
		int Mesh = -1;

		std::vector<int> Children;

		// The node's own transform, relative to its parent. glTF supplies
		// either a matrix or a translation/rotation/scale triple; both arrive
		// here as a matrix, because composing TRS is the only thing anyone
		// does with it.
		glm::mat4 Local = glm::mat4(1.0f);
	};

	// A mesh with a place in the world: the flattened form of the tree.
	//
	// Worth having as well as the tree, because the tree is what the file says
	// and this is what a renderer wants -- and because the same mesh can appear
	// at several transforms, which is the whole point of the hierarchy.
	struct EGSS_API GltfInstance
	{
		int Node = -1;
		int Mesh = -1;
		glm::mat4 Transform = glm::mat4(1.0f);
	};

	struct EGSS_API GltfModel
	{
		// One MeshData per glTF mesh. A glTF mesh is a list of *primitives*,
		// each with its own material, which is exactly what Submesh already
		// describes -- so a mesh with three primitives is one vertex buffer and
		// three ranges, not three meshes.
		std::vector<MeshData> Meshes;

		std::vector<GltfMaterial> Materials;
		std::vector<GltfImage> Images;
		std::vector<GltfNode> Nodes;

		// Roots of the default scene, and every mesh-bearing node beneath them
		// with its transform already composed down from the root.
		std::vector<int> RootNodes;
		std::vector<GltfInstance> Instances;

		// Whatever the file said made it, which is the first thing worth
		// knowing when a model loads wrong.
		std::string Generator;

		// Bounds over every instance, in world space -- so, the bounds of the
		// thing as assembled, not of any one mesh. This is what frames a
		// camera on a model whose size you do not know.
		glm::vec3 BoundsMin = glm::vec3(0.0f);
		glm::vec3 BoundsMax = glm::vec3(0.0f);

		glm::vec3 BoundsCentre() const { return (BoundsMin + BoundsMax) * 0.5f; }
		glm::vec3 BoundsSize() const { return BoundsMax - BoundsMin; }
		float BoundsRadius() const { return glm::length(BoundsSize()) * 0.5f; }

		size_t TriangleCount() const;
	};

	class EGSS_API GltfLoader
	{
	public:
		// Reads `.gltf` or `.glb`, told apart by content rather than by
		// extension -- the magic is unambiguous and a misnamed file is common.
		//
		// Fills `out` and returns true, or leaves it alone, fills `error` and
		// returns false. Never throws and never partially reports success.
		static bool Load(const std::string& path, GltfModel& out, std::string& error);

		// The same, on bytes already in memory. `baseDirectory` is what
		// relative URIs resolve against; a model with no external references
		// (a .glb, or a .gltf with everything inlined) does not need one, which
		// is what makes this testable with no filesystem at all.
		static bool Parse(const unsigned char* bytes, size_t length,
			const std::string& baseDirectory, GltfModel& out, std::string& error);
	};

}
