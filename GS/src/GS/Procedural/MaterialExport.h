#pragma once

#include "GS/Core.h"
#include "GS/Procedural/MaterialGraph.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GS {

	// Writes the four maps beside the .gsmat -- <name>_albedo.png, _height,
	// _normal, _roughness -- plus <name>.mtl whose map_Kd names the albedo,
	// so any mesh can use the material through the existing .mtl path. Each
	// file goes to a temporary name first and is renamed into place, so a
	// failed write never leaves a truncated PNG where a good one was.
	// Always at full resolution: a preview divisor in effect is lifted for
	// the export and restored after.
	GS_API bool ExportMaterial(MaterialGraph& graph, const std::string& gsmatPath, std::string& error);

	// 8-bit RGBA, rows in the image's own order (row 0 = v 0). Grey is
	// replicated to RGB with alpha 255.
	GS_API std::vector<uint8_t> ToRGBA8(const Image& image);

}
