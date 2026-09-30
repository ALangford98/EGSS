#include "gspch.h"
#include "GS/Procedural/MaterialGraphSerializer.h"
#include "GS/Json.h"

#include <filesystem>
#include <fstream>

namespace GS {

	namespace {
		const int kVersion = 1;

		// %.9g is the shortest format that round-trips every float exactly,
		// which is what makes save -> load -> save byte-identical.
		std::string Num(float value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.9g", value);
			return buffer;
		}

		std::string Quote(const std::string& s)
		{
			std::string out = "\"";
			for (char c : s)
			{
				if (c == '"' || c == '\\')
					out += '\\';
				out += c;
			}
			return out + "\"";
		}

		std::string Where(const JsonValue& node, int index)
		{
			return "node " + std::to_string(index) + " (" + node["type"].GetString("?") + ")";
		}
	}

	std::string SerializeMaterialGraph(const MaterialGraph& graph)
	{
		std::string out = "{\n  \"version\": " + std::to_string(kVersion)
			+ ",\n  \"resolution\": " + std::to_string(graph.Resolution()) + ",\n  \"nodes\": [\n";
		const std::vector<MaterialNode>& nodes = graph.Nodes();
		for (size_t i = 0; i < nodes.size(); i++)
		{
			const MaterialNode& node = nodes[i];
			const NodeTypeInfo& info = GetNodeTypeInfo(node.Type);
			out += "    {\"id\": " + std::to_string(node.Id) + ", \"type\": " + Quote(info.Name)
				+ ", \"pos\": [" + Num(node.Position.x) + ", " + Num(node.Position.y) + "]";
			if (!info.Params.empty())
			{
				out += ", \"params\": {";
				for (size_t p = 0; p < info.Params.size(); p++)
					out += (p ? ", " : "") + Quote(info.Params[p].Name) + ": " + Num(node.Params[p]);
				out += "}";
			}
			if (info.HasColour)
				out += ", \"colour\": [" + Num(node.Colour.r) + ", " + Num(node.Colour.g) + ", "
					+ Num(node.Colour.b) + ", " + Num(node.Colour.a) + "]";
			if (info.HasRamp)
			{
				out += ", \"ramp\": [";
				for (size_t s = 0; s < node.Ramp.size(); s++)
				{
					const GradientStop& stop = node.Ramp[s];
					out += std::string(s ? ", " : "") + "[" + Num(stop.Position) + ", " + Num(stop.Colour.r) + ", "
						+ Num(stop.Colour.g) + ", " + Num(stop.Colour.b) + ", " + Num(stop.Colour.a) + "]";
				}
				out += "]";
			}
			out += i + 1 < nodes.size() ? "},\n" : "}\n";
		}
		out += "  ],\n  \"links\": [";
		const std::vector<MaterialLink>& links = graph.Links();
		for (size_t i = 0; i < links.size(); i++)
		{
			const MaterialLink& l = links[i];
			out += std::string(i ? ", " : "") + "[" + std::to_string(l.FromNode) + ", " + std::to_string(l.FromPin)
				+ ", " + std::to_string(l.ToNode) + ", " + std::to_string(l.ToPin) + "]";
		}
		return out + "]\n}\n";
	}

	bool DeserializeMaterialGraph(const std::string& text, MaterialGraph& out, std::string& error)
	{
		JsonValue root;
		if (!JsonValue::Parse(text, root, error))
			return false;
		if (!root.IsObject())
		{
			error = "a .gsmat is a JSON object";
			return false;
		}
		int version = root["version"].GetInt(0);
		if (version != kVersion)
		{
			error = "unsupported .gsmat version " + std::to_string(version) + " (this build reads "
				+ std::to_string(kVersion) + ")";
			return false;
		}

		// Built aside and swapped in only once all of it loaded -- a refused
		// file leaves the caller's graph exactly as it was.
		MaterialGraph graph;
		graph.SetResolution(root["resolution"].GetInt(512));

		const JsonValue& nodes = root["nodes"];
		for (size_t i = 0; i < nodes.Size(); i++)
		{
			const JsonValue& n = nodes[i];
			NodeType type;
			std::string typeName = n["type"].GetString();
			if (!NodeTypeFromName(typeName, type))
			{
				error = "node " + std::to_string(i) + ": unknown node type '" + typeName
					+ "' (written by a newer build?)";
				return false;
			}
			int id = n["id"].GetInt(-1);
			if (id < 1 || graph.FindNode(id))
			{
				error = Where(n, (int)i) + ": missing or duplicate id " + std::to_string(id);
				return false;
			}
			glm::vec2 pos(n["pos"][0].GetFloat(), n["pos"][1].GetFloat());
			if (graph.AddNode(type, pos, id) < 0)
			{
				error = Where(n, (int)i) + ": a graph holds at most one " + typeName;
				return false;
			}

			const JsonValue& params = n["params"];
			for (size_t p = 0; p < params.Size() && params.IsObject(); p++)
			{
				const std::string& key = params.KeyAt(p);
				int index = FindParam(type, key.c_str());
				if (index < 0)
				{
					error = Where(n, (int)i) + ": unknown parameter '" + key + "'";
					return false;
				}
				graph.SetParam(id, index, params.ValueAt(p).GetFloat());
			}
			if (n.Has("colour"))
			{
				const JsonValue& c = n["colour"];
				graph.SetColour(id, glm::vec4(c[0].GetFloat(), c[1].GetFloat(), c[2].GetFloat(), c[3].GetFloat(1.0f)));
			}
			if (n.Has("ramp"))
			{
				std::vector<GradientStop> ramp;
				const JsonValue& r = n["ramp"];
				for (size_t s = 0; s < r.Size(); s++)
				{
					const JsonValue& stop = r[s];
					ramp.push_back({ stop[0].GetFloat(), glm::vec4(stop[1].GetFloat(), stop[2].GetFloat(),
						stop[3].GetFloat(), stop[4].GetFloat(1.0f)) });
				}
				graph.SetRamp(id, ramp);
			}
		}

		const JsonValue& links = root["links"];
		for (size_t i = 0; i < links.Size(); i++)
		{
			const JsonValue& l = links[i];
			int from = l[0].GetInt(), fromPin = l[1].GetInt(), to = l[2].GetInt(), toPin = l[3].GetInt();
			ConnectResult result = graph.Connect(from, fromPin, to, toPin);
			if (result != ConnectResult::Ok)
			{
				const char* why = result == ConnectResult::InvalidPin ? "no such node or pin"
					: result == ConnectResult::TypeMismatch ? "pin types do not match" : "it would make a cycle";
				error = "link " + std::to_string(i) + " (" + std::to_string(from) + ":" + std::to_string(fromPin)
					+ " -> " + std::to_string(to) + ":" + std::to_string(toPin) + "): " + why;
				return false;
			}
		}

		out = std::move(graph);
		return true;
	}

	bool SaveMaterialGraph(const std::string& path, const MaterialGraph& graph, std::string& error)
	{
		std::string temp = path + ".tmp";
		{
			std::ofstream file(temp, std::ios::binary);
			if (!(file << SerializeMaterialGraph(graph)))
			{
				error = "could not write '" + temp + "'";
				return false;
			}
		}
		std::error_code ec;
		std::filesystem::rename(temp, path, ec);
		if (ec)
		{
			error = "could not replace '" + path + "': " + ec.message();
			std::filesystem::remove(temp, ec);
			return false;
		}
		return true;
	}

	bool LoadMaterialGraph(const std::string& path, MaterialGraph& out, std::string& error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			error = "could not open '" + path + "'";
			return false;
		}
		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (!DeserializeMaterialGraph(text, out, error))
		{
			error = path + ": " + error;
			return false;
		}
		return true;
	}

}
