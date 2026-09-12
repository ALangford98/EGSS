#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

namespace GS {

	// One mesh per path, ever, for the run. A scene that places the same
	// preset shape or the same imported file on fifty entities should upload
	// its geometry once -- this is the thing both MeshComponent::SourcePath
	// and the placement panels resolve through, so "load" and "place" always
	// agree on what a given path means.
	//
	// Four names are not files: "primitive:cube", "primitive:sphere",
	// "primitive:plane" and "primitive:cylinder" build the matching
	// Mesh::Create* shape instead of touching disk, so a scene can reference
	// a preset the same way it references an imported one.
	class GS_API MeshCache
	{
	public:
		// Returns the cached mesh for `path`, building or loading it on first
		// request. Returns nullptr (and logs, via Mesh::Load's own logging)
		// for a path that names neither a primitive nor a loadable .obj -- a
		// missing mesh must not take the scene with it.
		static std::shared_ptr<Mesh> Get(const std::string& path);

		// Forgets everything cached. A test can use this to force a fresh
		// load, and Renderer::Shutdown calls it too: the store is a
		// function-local static, so left alone it would be destroyed after
		// main() returns, after the GL context is already gone -- too late
		// for the cached meshes' glDeleteVertexArrays/glDeleteBuffers calls
		// to be well-defined.
		static void Clear();

		// Every mesh currently cached -- for a caller that wants to add up
		// counts or sizes across all of them (the Profiler's asset stats,
		// say) without this class needing an opinion on what "size" means
		// for a mesh. A snapshot copy of the shared_ptrs, not a reference to
		// the store itself, so it's safe to walk even if something else
		// loads or Clear()s the cache while it's held.
		static std::vector<std::shared_ptr<Mesh>> All();
	};

}
