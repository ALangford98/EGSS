#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

namespace GS {

	// The write side of ObjLoader -- v/vt/vn/f text, one vertex/triangle at
	// a time. No materials (mtllib/usemtl) are written, matching what
	// ObjLoader itself reads: this project's material system is a separate,
	// later concern, and a rebuilt/edited mesh keeps whatever flat colour
	// or file-sourced material it already had via MeshComponent, untouched
	// by this.
	class GS_API ObjWriter
	{
	public:
		static std::string Write(const MeshData& data);
		static bool Save(const std::string& path, const MeshData& data, std::string& error);
	};

}
