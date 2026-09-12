#include "gspch.h"
#include "GS/Renderer/MeshCache.h"

namespace GS {

	static std::unordered_map<std::string, std::shared_ptr<Mesh>>& Store()
	{
		static std::unordered_map<std::string, std::shared_ptr<Mesh>> store;
		return store;
	}

	std::shared_ptr<Mesh> MeshCache::Get(const std::string& path)
	{
		auto& store = Store();

		auto existing = store.find(path);
		if (existing != store.end())
			return existing->second;

		Mesh* built = nullptr;

		if (path == "primitive:cube")
			built = Mesh::CreateCube();
		else if (path == "primitive:sphere")
			built = Mesh::CreateSphere();
		else if (path == "primitive:plane")
			built = Mesh::CreatePlane();
		else if (path == "primitive:cylinder")
			built = Mesh::CreateCylinder();
		else
			built = Mesh::Load(path);

		std::shared_ptr<Mesh> mesh(built);

		// A failure is not cached: a typo'd path fixed later in the same run
		// must get a fresh attempt, not a permanently poisoned nullptr from
		// the first try.
		if (mesh)
			store[path] = mesh;

		return mesh;
	}

	void MeshCache::Clear()
	{
		Store().clear();
	}

	std::vector<std::shared_ptr<Mesh>> MeshCache::All()
	{
		std::vector<std::shared_ptr<Mesh>> meshes;
		meshes.reserve(Store().size());
		for (auto& [path, mesh] : Store())
			meshes.push_back(mesh);
		return meshes;
	}

}
