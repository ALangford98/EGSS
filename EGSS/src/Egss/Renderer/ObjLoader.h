#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Mesh.h"

namespace Egss {

	// Wavefront .obj, which is the smallest useful 3D format: plain text, one
	// item per line, and every viewer and modeller writes it.
	//
	// What is supported: v / vt / vn / f, faces of any size (fan-triangulated),
	// positive and negative indices, and files missing texture coordinates or
	// normals -- normals are generated when absent.
	//
	// What is not: materials (`mtllib` / `usemtl` are skipped), smoothing
	// groups, curves and free-form surfaces. Materials are deliberately left
	// for whenever a material system exists to receive them; parsing an .mtl
	// into nothing would be a lie about what the engine can do.
	class EGSS_API ObjLoader
	{
	public:
		// Fills `out` and returns true, or leaves it alone, fills `error` and
		// returns false. Never throws and never partially reports success.
		static bool Load(const std::string& path, MeshData& out, std::string& error);

		// Same parser, reading text already in memory -- which is what makes
		// the loader testable without touching the filesystem.
		static bool Parse(const char* text, size_t length, MeshData& out, std::string& error);
	};

}
