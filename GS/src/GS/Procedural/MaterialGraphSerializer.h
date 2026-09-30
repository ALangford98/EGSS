#pragma once

#include "GS/Core.h"
#include "GS/Procedural/MaterialGraph.h"

#include <string>

namespace GS {

	// .gsmat: a material graph as JSON, one node per line so a diff of two
	// versions reads as which nodes changed. Parameters are keyed by name,
	// so a file survives a node gaining a parameter (the new one takes its
	// default) and a typo is an error rather than a silently ignored key.
	//
	// Loading is all-or-nothing. A file with one bad node is refused whole:
	// loading the rest and dropping it would lose that node for good the
	// next time the graph is saved.
	GS_API std::string SerializeMaterialGraph(const MaterialGraph& graph);
	GS_API bool DeserializeMaterialGraph(const std::string& text, MaterialGraph& out, std::string& error);

	GS_API bool SaveMaterialGraph(const std::string& path, const MaterialGraph& graph, std::string& error);
	GS_API bool LoadMaterialGraph(const std::string& path, MaterialGraph& out, std::string& error);

}
