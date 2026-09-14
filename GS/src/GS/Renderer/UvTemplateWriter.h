#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"
#include "GS/Renderer/UvUnwrap.h"

namespace GS {

	// Rasterizes a mesh's (already [0,1]-ranged) packed UVs into a
	// wireframe + per-chart-tint PNG a designer can paint over.
	class GS_API UvTemplateWriter
	{
	public:
		static bool Write(const std::string& path, int resolution, const MeshData& data,
			const UvUnwrap::ChartAssignment& chartIdPerTriangle, std::string& error);
	};

}
