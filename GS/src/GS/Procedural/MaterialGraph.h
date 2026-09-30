#pragma once

#include "GS/Core.h"
#include "GS/Procedural/Image.h"

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace GS {

	// A procedural material as a node graph: generators (noise, cells,
	// bricks) feed filters and blends, and four output nodes name what the
	// material exports -- albedo, height, normal, roughness. See
	// docs/superpowers/specs/2026-09-30-procedural-material-graph-design.md.
	//
	// No UI and no GL here on purpose: a graph can be built and evaluated
	// from a self-test with no window, which is how every node's arithmetic
	// is checked. The editor panel (TestEnv/src/MaterialEditorPanel.h) only
	// turns clicks into the calls below.

	// Any is Blend's and Transform's: they take whichever type arrives and
	// pass it on, rather than each existing twice. The concrete type is
	// resolved per node from what is connected -- see OutputType.
	enum class PinType { Grey, Colour, Any };
	enum class ParamKind { Float, Int, Enum };

	enum class NodeType
	{
		Noise, Voronoi, Pattern, Constant,
		Levels, Invert, Blur, Warp, Transform,
		Blend, GradientMap, ColourConstant, HeightToNormal,
		OutAlbedo, OutHeight, OutNormal, OutRoughness,
		Count
	};

	struct PinInfo
	{
		const char* Name;
		PinType Type;
		// What an unconnected input reads as. Colour inputs read (d, d, d, 1).
		float Default;
	};

	struct ParamInfo
	{
		const char* Name;      // the .gsmat key, so renaming one breaks files
		ParamKind Kind;
		float Min, Max, Default;
		std::vector<const char*> Labels;   // Enum only
	};

	struct NodeTypeInfo
	{
		const char* Name;      // the .gsmat type key
		const char* Category;
		std::vector<PinInfo> Inputs, Outputs;
		std::vector<ParamInfo> Params;
		bool HasColour = false, HasRamp = false, IsOutput = false;
	};

	GS_API const NodeTypeInfo& GetNodeTypeInfo(NodeType type);
	GS_API bool NodeTypeFromName(const std::string& name, NodeType& out);
	GS_API int FindParam(NodeType type, const char* name);   // -1 if absent

	// Tangent-space normals from a Grey height image, packed n * 0.5 + 0.5,
	// OpenGL convention (+Y up). Free so the node and the self-test run the
	// same function on the same input.
	GS_API Image HeightToNormal(const Image& height, float strength);

	struct GradientStop
	{
		float Position;
		glm::vec4 Colour;
	};

	struct MaterialNode
	{
		int Id = 0;
		NodeType Type = NodeType::Constant;
		glm::vec2 Position = glm::vec2(0.0f);   // editor layout only; never dirties
		std::vector<float> Params;              // one per NodeTypeInfo::Params
		glm::vec4 Colour = glm::vec4(1.0f);     // ColourConstant
		std::vector<GradientStop> Ramp;         // GradientMap
	};

	struct MaterialLink
	{
		int FromNode, FromPin, ToNode, ToPin;
	};

	enum class ConnectResult { Ok, InvalidPin, TypeMismatch, Cycle };

	class GS_API MaterialGraph
	{
	public:
		enum class Output { Albedo, Height, Normal, Roughness };

		// -1 if an output node of that kind already exists -- a graph has at
		// most one of each, so "which albedo does the export mean" never has
		// two answers. `id` is for the loader, which must keep a file's ids.
		int AddNode(NodeType type, glm::vec2 position, int id = -1);
		void RemoveNode(int id);

		// Replaces whatever the input was connected to. Refuses, and leaves
		// the graph unchanged, rather than ever holding a cycle or a type the
		// input cannot take.
		ConnectResult Connect(int fromNode, int fromPin, int toNode, int toPin);
		void Disconnect(int toNode, int toPin);

		void SetParam(int id, int index, float value);
		void SetColour(int id, glm::vec4 colour);
		void SetRamp(int id, std::vector<GradientStop> ramp);
		void SetPosition(int id, glm::vec2 position);

		// Power of two, 256..2048; anything else is clamped down to one.
		// Clears every cache.
		void SetResolution(int resolution);
		int Resolution() const { return m_Resolution; }

		// 4 while a slider is being dragged, 1 otherwise: the whole graph
		// evaluates at a quarter of the resolution for a responsive preview.
		// Parameters are in UV units, so the result looks the same, less
		// sharp. Never used for export.
		void SetPreviewDivisor(int divisor);
		int EvalResolution() const { return m_Resolution / m_PreviewDivisor; }

		PinType OutputType(int nodeId, int pin) const;
		const Image& Evaluate(int nodeId, int pin);
		Image EvaluateOutput(Output which);
		int FindOutputNode(Output which) const;

		int EvalCount(int nodeId) const;
		const std::vector<MaterialNode>& Nodes() const { return m_Nodes; }
		const std::vector<MaterialLink>& Links() const { return m_Links; }
		const MaterialNode* FindNode(int id) const;
		const MaterialLink* FindInputLink(int toNode, int toPin) const;

		// Bumps on every change that can alter an image. The scene link
		// compares it to decide whether to re-upload a texture.
		uint64_t Version() const { return m_Version; }

	private:
		struct Cache
		{
			std::vector<Image> Outputs;
			bool Dirty = true;
			int EvalCount = 0;
		};

		MaterialNode* FindMutable(int id);
		int IndexOf(int id) const;
		bool Reaches(int from, int to) const;
		void MarkDirtyFrom(int id);
		void ClearAllCaches();
		PinType InputType(int nodeId, int pin) const;
		PinType ResolveAny(int nodeId) const;
		void PruneInvalidLinks();
		void EvaluateNode(int index);

		std::vector<MaterialNode> m_Nodes;
		std::vector<Cache> m_Caches;   // parallel to m_Nodes
		std::vector<MaterialLink> m_Links;
		int m_NextId = 1;
		int m_Resolution = 512;
		int m_PreviewDivisor = 1;
		uint64_t m_Version = 0;
	};

}
