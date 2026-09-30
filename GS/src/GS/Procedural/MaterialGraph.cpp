#include "gspch.h"
#include "GS/Procedural/MaterialGraph.h"

namespace GS {

	namespace Detail {
		void EvaluateNodeBody(const MaterialNode& node, const std::vector<const Image*>& inputs,
			int n, int outChannels, std::vector<Image>& outputs);
	}

	namespace {
		int ChannelsOf(PinType type) { return type == PinType::Colour ? 4 : 1; }

		MaterialGraph::Output OutputOf(NodeType type)
		{
			switch (type)
			{
			case NodeType::OutAlbedo: return MaterialGraph::Output::Albedo;
			case NodeType::OutHeight: return MaterialGraph::Output::Height;
			case NodeType::OutNormal: return MaterialGraph::Output::Normal;
			default:                  return MaterialGraph::Output::Roughness;
			}
		}

		// A flat normal is (0, 0, 1), packed -- not the (d, d, d) every other
		// colour default is, so it cannot come from PinInfo::Default.
		Image FlatDefault(MaterialGraph::Output which, int n)
		{
			static const float albedo[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
			static const float normal[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
			static const float half = 0.5f;
			switch (which)
			{
			case MaterialGraph::Output::Albedo: return Image::Filled(n, n, 4, albedo);
			case MaterialGraph::Output::Normal: return Image::Filled(n, n, 4, normal);
			default:                            return Image::Filled(n, n, 1, &half);
			}
		}
	}

	// ---- Structure ----------------------------------------------------------

	int MaterialGraph::AddNode(NodeType type, glm::vec2 position, int id)
	{
		const NodeTypeInfo& info = GetNodeTypeInfo(type);
		if (info.IsOutput)
			for (const MaterialNode& node : m_Nodes)
				if (node.Type == type)
					return -1;

		if (id < 0)
			id = m_NextId;
		else if (IndexOf(id) >= 0)
			return -1;
		m_NextId = std::max(m_NextId, id + 1);

		MaterialNode node;
		node.Id = id;
		node.Type = type;
		node.Position = position;
		for (const ParamInfo& param : info.Params)
			node.Params.push_back(param.Default);
		if (info.HasRamp)
			node.Ramp = { { 0.0f, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) }, { 1.0f, glm::vec4(1.0f) } };

		m_Nodes.push_back(node);
		m_Caches.emplace_back();
		m_Version++;
		return id;
	}

	void MaterialGraph::RemoveNode(int id)
	{
		int index = IndexOf(id);
		if (index < 0)
			return;

		// Dirty what it fed *before* the links go, or there is no path left
		// to walk to find it.
		for (const MaterialLink& link : m_Links)
			if (link.FromNode == id)
				MarkDirtyFrom(link.ToNode);

		m_Links.erase(std::remove_if(m_Links.begin(), m_Links.end(),
			[id](const MaterialLink& l) { return l.FromNode == id || l.ToNode == id; }), m_Links.end());
		m_Nodes.erase(m_Nodes.begin() + index);
		m_Caches.erase(m_Caches.begin() + index);
		PruneInvalidLinks();
		m_Version++;
	}

	ConnectResult MaterialGraph::Connect(int fromNode, int fromPin, int toNode, int toPin)
	{
		const MaterialNode* from = FindNode(fromNode);
		const MaterialNode* to = FindNode(toNode);
		if (!from || !to)
			return ConnectResult::InvalidPin;
		const NodeTypeInfo& fromInfo = GetNodeTypeInfo(from->Type);
		const NodeTypeInfo& toInfo = GetNodeTypeInfo(to->Type);
		if (fromPin < 0 || fromPin >= (int)fromInfo.Outputs.size()
			|| toPin < 0 || toPin >= (int)toInfo.Inputs.size())
			return ConnectResult::InvalidPin;

		if (fromNode == toNode || Reaches(toNode, fromNode))
			return ConnectResult::Cycle;

		PinType source = OutputType(fromNode, fromPin);
		PinType target = toInfo.Inputs[toPin].Type;
		if (target != PinType::Any)
		{
			if (source != target)
				return ConnectResult::TypeMismatch;
		}
		else
		{
			// Every Any input of one node carries the same type -- that type
			// *is* the node's type. Another Any input already connected with a
			// different one means these two cannot meet here.
			for (const MaterialLink& link : m_Links)
				if (link.ToNode == toNode && link.ToPin != toPin
					&& toInfo.Inputs[link.ToPin].Type == PinType::Any
					&& OutputType(link.FromNode, link.FromPin) != source)
					return ConnectResult::TypeMismatch;

			// Re-typing a node whose output already feeds something would
			// silently invalidate those links. Refused, conservatively, even
			// where the downstream input is itself Any and might have coped.
			PinType current = ResolveAny(toNode);
			if (current != source)
				for (const MaterialLink& link : m_Links)
					if (link.FromNode == toNode)
						return ConnectResult::TypeMismatch;
		}

		m_Links.erase(std::remove_if(m_Links.begin(), m_Links.end(),
			[&](const MaterialLink& l) { return l.ToNode == toNode && l.ToPin == toPin; }), m_Links.end());
		m_Links.push_back({ fromNode, fromPin, toNode, toPin });
		MarkDirtyFrom(toNode);
		m_Version++;
		return ConnectResult::Ok;
	}

	void MaterialGraph::Disconnect(int toNode, int toPin)
	{
		size_t before = m_Links.size();
		m_Links.erase(std::remove_if(m_Links.begin(), m_Links.end(),
			[&](const MaterialLink& l) { return l.ToNode == toNode && l.ToPin == toPin; }), m_Links.end());
		if (m_Links.size() == before)
			return;
		MarkDirtyFrom(toNode);
		PruneInvalidLinks();
		m_Version++;
	}

	// Taking an input away can re-type an Any node (a Blend whose only Colour
	// input went is Grey again), which can leave a link downstream carrying a
	// type its input does not take. Those links are dropped rather than left
	// for the evaluator to trip over -- repeated until nothing changes,
	// because dropping one can re-type the next node along.
	void MaterialGraph::PruneInvalidLinks()
	{
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (size_t i = 0; i < m_Links.size(); i++)
			{
				const MaterialLink& link = m_Links[i];
				if (OutputType(link.FromNode, link.FromPin) != InputType(link.ToNode, link.ToPin))
				{
					int to = link.ToNode;
					m_Links.erase(m_Links.begin() + i);
					MarkDirtyFrom(to);
					changed = true;
					break;
				}
			}
		}
	}

	// ---- Parameters ---------------------------------------------------------

	void MaterialGraph::SetParam(int id, int index, float value)
	{
		MaterialNode* node = FindMutable(id);
		if (!node || index < 0 || index >= (int)node->Params.size())
			return;
		const ParamInfo& info = GetNodeTypeInfo(node->Type).Params[index];
		value = glm::clamp(value, info.Min, info.Max);
		if (info.Kind != ParamKind::Float)
			value = std::round(value);
		if (node->Params[index] == value)
			return;
		node->Params[index] = value;
		MarkDirtyFrom(id);
		m_Version++;
	}

	void MaterialGraph::SetColour(int id, glm::vec4 colour)
	{
		if (MaterialNode* node = FindMutable(id))
		{
			node->Colour = glm::clamp(colour, 0.0f, 1.0f);
			MarkDirtyFrom(id);
			m_Version++;
		}
	}

	void MaterialGraph::SetRamp(int id, std::vector<GradientStop> ramp)
	{
		if (MaterialNode* node = FindMutable(id))
		{
			// Sorted here, once, so SampleRamp can walk it in order and the
			// editor may hand stops over in whatever order it holds them.
			std::stable_sort(ramp.begin(), ramp.end(),
				[](const GradientStop& a, const GradientStop& b) { return a.Position < b.Position; });
			if (ramp.size() > 8)
				ramp.resize(8);
			node->Ramp = std::move(ramp);
			MarkDirtyFrom(id);
			m_Version++;
		}
	}

	void MaterialGraph::SetPosition(int id, glm::vec2 position)
	{
		if (MaterialNode* node = FindMutable(id))
			node->Position = position;
	}

	void MaterialGraph::SetResolution(int resolution)
	{
		int clamped = 256;
		while (clamped * 2 <= std::min(resolution, 2048))
			clamped *= 2;
		if (clamped == m_Resolution)
			return;
		m_Resolution = clamped;
		ClearAllCaches();
		m_Version++;
	}

	void MaterialGraph::SetPreviewDivisor(int divisor)
	{
		divisor = divisor >= 4 ? 4 : 1;
		if (divisor == m_PreviewDivisor)
			return;
		m_PreviewDivisor = divisor;
		ClearAllCaches();
		m_Version++;
	}

	// ---- Typing -------------------------------------------------------------

	// An Any node takes the type of its lowest-numbered connected Any input,
	// and is Grey with none. Connect keeps every Any input agreeing, so
	// "lowest-numbered" only matters in the moment an input is replaced.
	PinType MaterialGraph::ResolveAny(int nodeId) const
	{
		const MaterialNode* node = FindNode(nodeId);
		if (!node)
			return PinType::Grey;
		const NodeTypeInfo& info = GetNodeTypeInfo(node->Type);
		for (int pin = 0; pin < (int)info.Inputs.size(); pin++)
			if (info.Inputs[pin].Type == PinType::Any)
				if (const MaterialLink* link = FindInputLink(nodeId, pin))
					return OutputType(link->FromNode, link->FromPin);
		return PinType::Grey;
	}

	PinType MaterialGraph::OutputType(int nodeId, int pin) const
	{
		const MaterialNode* node = FindNode(nodeId);
		if (!node)
			return PinType::Grey;
		PinType declared = GetNodeTypeInfo(node->Type).Outputs[pin].Type;
		return declared == PinType::Any ? ResolveAny(nodeId) : declared;
	}

	PinType MaterialGraph::InputType(int nodeId, int pin) const
	{
		const MaterialNode* node = FindNode(nodeId);
		if (!node)
			return PinType::Grey;
		PinType declared = GetNodeTypeInfo(node->Type).Inputs[pin].Type;
		return declared == PinType::Any ? ResolveAny(nodeId) : declared;
	}

	// ---- Evaluation ---------------------------------------------------------

	const Image& MaterialGraph::Evaluate(int nodeId, int pin)
	{
		int index = IndexOf(nodeId);
		// Memoised depth-first evaluation is the topological sort: a node
		// runs only once all it reads have, and each runs at most once per
		// change, however many paths reach it. Connect refusing cycles is
		// what guarantees this recursion ends.
		if (m_Caches[index].Dirty)
			EvaluateNode(index);
		return m_Caches[index].Outputs[pin];
	}

	void MaterialGraph::EvaluateNode(int index)
	{
		const MaterialNode node = m_Nodes[index];   // a copy: evaluating inputs must not invalidate it
		const NodeTypeInfo& info = GetNodeTypeInfo(node.Type);
		const int n = EvalResolution();

		// Defaults live here, sized up front so the pointers into it stay put.
		std::vector<Image> defaults;
		defaults.reserve(info.Inputs.size());
		std::vector<const Image*> inputs;
		for (int pin = 0; pin < (int)info.Inputs.size(); pin++)
		{
			if (const MaterialLink* link = FindInputLink(node.Id, pin))
			{
				int from = link->FromNode, fromPin = link->FromPin;
				inputs.push_back(&Evaluate(from, fromPin));
				continue;
			}
			PinType type = InputType(node.Id, pin);
			float d = info.Inputs[pin].Default;
			const float value[4] = { d, d, d, 1.0f };
			defaults.push_back(Image::Filled(n, n, ChannelsOf(type), value));
			inputs.push_back(&defaults.back());
		}

		// Evaluate() above may have run other nodes, but never added or
		// removed any, so `index` still names this node's cache.
		Cache& cache = m_Caches[index];
		cache.Outputs.assign(info.Outputs.size(), Image());
		int outChannels = info.Outputs.empty() ? 1 : ChannelsOf(OutputType(node.Id, 0));
		Detail::EvaluateNodeBody(node, inputs, n, outChannels, cache.Outputs);
		cache.Dirty = false;
		cache.EvalCount++;
	}

	Image MaterialGraph::EvaluateOutput(Output which)
	{
		const int n = EvalResolution();
		int id = FindOutputNode(which);
		const MaterialLink* link = id >= 0 ? FindInputLink(id, 0) : nullptr;
		if (link)
		{
			int from = link->FromNode, fromPin = link->FromPin;
			return Evaluate(from, fromPin);
		}

		// No normal wired in, but a height is: derive one, which is what the
		// normal output means for most materials anyway.
		if (which == Output::Normal)
		{
			int height = FindOutputNode(Output::Height);
			if (const MaterialLink* h = height >= 0 ? FindInputLink(height, 0) : nullptr)
			{
				int from = h->FromNode, fromPin = h->FromPin;
				return HeightToNormal(Evaluate(from, fromPin), 1.0f);
			}
		}
		return FlatDefault(which, n);
	}

	int MaterialGraph::FindOutputNode(Output which) const
	{
		for (const MaterialNode& node : m_Nodes)
			if (GetNodeTypeInfo(node.Type).IsOutput && OutputOf(node.Type) == which)
				return node.Id;
		return -1;
	}

	// ---- Bookkeeping --------------------------------------------------------

	int MaterialGraph::EvalCount(int nodeId) const
	{
		int index = IndexOf(nodeId);
		return index < 0 ? 0 : m_Caches[index].EvalCount;
	}

	const MaterialNode* MaterialGraph::FindNode(int id) const
	{
		int index = IndexOf(id);
		return index < 0 ? nullptr : &m_Nodes[index];
	}

	MaterialNode* MaterialGraph::FindMutable(int id)
	{
		int index = IndexOf(id);
		return index < 0 ? nullptr : &m_Nodes[index];
	}

	const MaterialLink* MaterialGraph::FindInputLink(int toNode, int toPin) const
	{
		for (const MaterialLink& link : m_Links)
			if (link.ToNode == toNode && link.ToPin == toPin)
				return &link;
		return nullptr;
	}

	int MaterialGraph::IndexOf(int id) const
	{
		for (size_t i = 0; i < m_Nodes.size(); i++)
			if (m_Nodes[i].Id == id)
				return (int)i;
		return -1;
	}

	bool MaterialGraph::Reaches(int from, int to) const
	{
		std::vector<int> stack = { from };
		std::vector<int> seen;
		while (!stack.empty())
		{
			int id = stack.back();
			stack.pop_back();
			if (id == to)
				return true;
			if (std::find(seen.begin(), seen.end(), id) != seen.end())
				continue;
			seen.push_back(id);
			for (const MaterialLink& link : m_Links)
				if (link.FromNode == id)
					stack.push_back(link.ToNode);
		}
		return false;
	}

	void MaterialGraph::MarkDirtyFrom(int id)
	{
		std::vector<int> stack = { id };
		while (!stack.empty())
		{
			int current = stack.back();
			stack.pop_back();
			int index = IndexOf(current);
			if (index < 0 || m_Caches[index].Dirty)
				continue;   // already dirty means everything below it is too
			m_Caches[index].Dirty = true;
			for (const MaterialLink& link : m_Links)
				if (link.FromNode == current)
					stack.push_back(link.ToNode);
		}
	}

	void MaterialGraph::ClearAllCaches()
	{
		for (Cache& cache : m_Caches)
			cache.Dirty = true;
	}

}
