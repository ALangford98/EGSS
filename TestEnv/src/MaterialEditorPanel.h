#pragma once
// The procedural material editor: a node graph (imnodes) over a
// GS::MaterialGraph, a parameter inspector for the selected node, a strip
// previewing the four exported maps, and Save/Export. See
// docs/superpowers/specs/2026-09-30-procedural-material-graph-design.md.
//
// Everything that computes lives in GS/src/GS/Procedural/; this file only
// turns clicks into graph calls and evaluated images into textures.
#include <GS.h>
#include <imgui.h>
#include <imnodes.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "EditorProject.h"
#include "FileBrowserPopup.h"

// Read by EditorMenuBar, whose Ctrl+Z/Ctrl+Y act on the scene: while this
// panel has focus they belong to the graph instead, the same arrangement
// g_TextEditorFocused makes for Ctrl+F.
inline bool g_MaterialEditorFocused = false;

// Set by whoever edits a material (this panel) so anything showing it --
// the scene's linked entities -- can pick the change up. Null until the
// scene link exists to receive it.
using MaterialEditedCallback = void (*)(const std::string& path, GS::MaterialGraph& graph);
inline MaterialEditedCallback g_OnMaterialEdited = nullptr;

class MaterialEditorPanel
{
public:
	~MaterialEditorPanel()
	{
		if (m_NodesContext)
			ImNodes::DestroyContext(m_NodesContext);
	}

	const std::string& Path() const { return m_Path; }

	// Compared against the file as last saved, not against the undo
	// stack's settled text -- an edit made this frame counts too.
	bool HasUnsavedChanges() const
	{
		return !m_Path.empty() && GS::SerializeMaterialGraph(m_Graph) != m_Saved;
	}

	// Opening another material over unsaved edits waits for an answer
	// (a modal in the panel) rather than replacing the graph: a click in
	// Files, the Inspector's Edit, or New Material would otherwise throw the
	// edits away with no trace.
	enum class Pending { Save, Discard, Cancel };
	const std::string& PendingOpen() const { return m_PendingOpen; }

	void ResolvePendingOpen(Pending choice)
	{
		std::string path = std::move(m_PendingOpen);
		m_PendingOpen.clear();
		if (choice == Pending::Cancel || path.empty())
			return;
		if (choice == Pending::Save)
		{
			Save();
			if (HasUnsavedChanges())
				return;   // the save failed and said so; stay on what would be lost
		}
		else if (g_OnMaterialEdited)
		{
			// Linked meshes were following the edits being dropped; give
			// them the saved state back, or the scene keeps showing a
			// material no file holds.
			GS::MaterialGraph saved;
			std::string error;
			if (GS::DeserializeMaterialGraph(m_Saved, saved, error))
				g_OnMaterialEdited(m_Path, saved);
		}
		OpenNow(path);
	}

	// What an output node's thumbnail was last uploaded from: the node and
	// pin feeding it as well as that node's evaluation count. The count
	// alone collided -- relinking an output to another node that had run
	// as many times left the old picture up.
	static uint64_t OutputThumbStamp(GS::MaterialGraph& graph, int outputNode)
	{
		const GS::MaterialLink* link = graph.FindInputLink(outputNode, 0);
		if (!link)
			return 0;
		return ((uint64_t)(uint32_t)link->FromNode << 40) | ((uint64_t)(link->FromPin & 0xff) << 32)
			| (uint32_t)graph.EvalCount(link->FromNode);
	}
	GS::MaterialGraph& Graph() { return m_Graph; }

	bool Open(const std::string& path)
	{
		if (HasUnsavedChanges())
		{
			m_PendingOpen = path;
			m_FocusFrames = 10;
			return false;
		}
		return OpenNow(path);
	}

	bool OpenNow(const std::string& path)
	{
		GS::MaterialGraph graph;
		std::string error;
		if (!GS::LoadMaterialGraph(path, graph, error))
		{
			m_Status = error;
			m_StatusIsError = true;
			return false;
		}
		m_Graph = std::move(graph);
		m_Path = path;
		m_Undo.clear();
		m_Redo.clear();
		m_Committed = m_Saved = GS::SerializeMaterialGraph(m_Graph);
		ResetViewState();
		m_Status = "Opened " + std::filesystem::path(path).filename().string();
		m_StatusIsError = false;
		m_FocusFrames = 10;
		return true;
	}

	// Creates the file at once, so an open material always has a path and
	// Save never needs to ask where. The starting graph is the smallest one
	// that shows something: noise through a ramp into albedo and height.
	bool NewMaterial(const std::string& path)
	{
		GS::MaterialGraph graph;
		int noise = graph.AddNode(GS::NodeType::Noise, glm::vec2(40.0f, 60.0f));
		int ramp = graph.AddNode(GS::NodeType::GradientMap, glm::vec2(300.0f, 40.0f));
		int albedo = graph.AddNode(GS::NodeType::OutAlbedo, glm::vec2(560.0f, 40.0f));
		int height = graph.AddNode(GS::NodeType::OutHeight, glm::vec2(560.0f, 220.0f));
		graph.Connect(noise, 0, ramp, 0);
		graph.Connect(ramp, 0, albedo, 0);
		graph.Connect(noise, 0, height, 0);
		std::string error;
		if (!GS::SaveMaterialGraph(path, graph, error))
		{
			m_Status = error;
			m_StatusIsError = true;
			return false;
		}
		return Open(path);
	}

	void OnImGuiRender()
	{
		if (!m_NodesContext)
		{
			// Created on first draw, not at construction: it needs ImGui's
			// context, which the ImGui layer creates after the layers exist.
			m_NodesContext = ImNodes::CreateContext();
			ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyCtrl;
		}
		ImNodes::SetCurrentContext(m_NodesContext);

		// Asked for over several frames, not one: at startup the dock
		// restores its own selected tab after this runs, and one request
		// loses to it.
		if (m_FocusFrames > 0)
		{
			ImGui::SetNextWindowFocus();
			m_FocusFrames--;
		}
		ImGui::Begin("Material");
		g_MaterialEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		DrawUnsavedPrompt();

		if (m_Path.empty())
		{
			ImGui::TextDisabled("No material open. File > New Material..., or click a .gsmat in Files.");
			DrawStatus();
			ImGui::End();
			return;
		}

		DrawToolbar();

		// Drag previews at a quarter resolution: decided from last frame's
		// "is anything being dragged", which is the only answer available
		// before this frame's widgets run.
		m_Graph.SetPreviewDivisor(m_Dragging ? 4 : 1);
		EvaluateAndUpload();

		float inspectorWidth = 280.0f;
		ImVec2 avail = ImGui::GetContentRegionAvail();
		float previewHeight = 128.0f;
		ImGui::BeginChild("##graph", ImVec2(avail.x - inspectorWidth, avail.y - previewHeight), false);
		DrawGraph();
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##inspector", ImVec2(0.0f, avail.y - previewHeight), true);
		DrawInspector();
		ImGui::EndChild();
		DrawPreviewStrip();

		m_Dragging = ImGui::IsAnyItemActive() || ImNodes::IsAnyAttributeActive();
		if (!m_Dragging)
			CommitIfChanged();
		HandleShortcuts();

		ImGui::End();
	}

private:
	// ---- Toolbar and status ----------------------------------------------

	void DrawToolbar()
	{
		bool unsaved = m_Committed != m_Saved;
		ImGui::TextUnformatted(std::filesystem::path(m_Path).filename().string().c_str());
		if (unsaved)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(unsaved)");
		}
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			Save();
		ImGui::SameLine();
		if (ImGui::Button("Export"))
			Export();
		ImGui::SameLine();
		ImGui::BeginDisabled(m_Undo.empty());
		if (ImGui::Button("Undo"))
			Undo();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(m_Redo.empty());
		if (ImGui::Button("Redo"))
			Redo();
		ImGui::EndDisabled();

		ImGui::SameLine();
		static const int sizes[] = { 256, 512, 1024, 2048 };
		int current = 0;
		for (int i = 0; i < 4; i++)
			if (sizes[i] == m_Graph.Resolution())
				current = i;
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::Combo("##res", &current, "256\0" "512\0" "1024\0" "2048\0"))
			m_Graph.SetResolution(sizes[current]);

		ImGui::SameLine();
		ImGui::TextDisabled("evaluated in %.1f ms", m_LastEvalMs);
		DrawStatus();
	}

	void DrawUnsavedPrompt()
	{
		if (!m_PendingOpen.empty() && !ImGui::IsPopupOpen("Unsaved material"))
			ImGui::OpenPopup("Unsaved material");
		if (!ImGui::BeginPopupModal("Unsaved material", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		ImGui::Text("%s has unsaved changes.", std::filesystem::path(m_Path).filename().string().c_str());
		ImGui::TextDisabled("Opening %s", std::filesystem::path(m_PendingOpen).filename().string().c_str());
		Pending choice = Pending::Cancel;
		bool answered = false;
		if (ImGui::Button("Save and open")) { choice = Pending::Save; answered = true; }
		ImGui::SameLine();
		if (ImGui::Button("Discard and open")) { choice = Pending::Discard; answered = true; }
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) { choice = Pending::Cancel; answered = true; }
		if (answered)
		{
			ResolvePendingOpen(choice);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void DrawStatus()
	{
		if (m_Status.empty())
			return;
		if (m_StatusIsError)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Status.c_str());
		else
			ImGui::TextDisabled("%s", m_Status.c_str());
	}

	void Save()
	{
		std::string error;
		if (GS::SaveMaterialGraph(m_Path, m_Graph, error))
		{
			m_Saved = m_Committed = GS::SerializeMaterialGraph(m_Graph);
			m_Status = "Saved";
			m_StatusIsError = false;
		}
		else
		{
			m_Status = error;
			m_StatusIsError = true;
		}
	}

	void Export()
	{
		std::string error;
		if (GS::ExportMaterial(m_Graph, m_Path, error))
		{
			// Said every time, because the files are there and it would be
			// natural to assume they are used.
			m_Status = "Exported 4 maps + .mtl. Only albedo renders yet -- normal and roughness "
				"wait on normal mapping and roughness shading.";
			m_StatusIsError = false;
		}
		else
		{
			m_Status = error;
			m_StatusIsError = true;
		}
	}

	// ---- Evaluation and textures -----------------------------------------

	struct Thumb
	{
		std::shared_ptr<GS::Texture2D> Texture;
		int EvalCount = -1;
		uint64_t Stamp = ~0ull;   // output nodes: see OutputThumbStamp
		int Size = 0;
	};

	static void Upload(std::shared_ptr<GS::Texture2D>& texture, const GS::Image& image)
	{
		if (!texture || (int)texture->GetWidth() != image.Width)
		{
			// Recreated, never SetData'd across a size change -- SetData
			// asserts the size matches, and resolution changes are normal here.
			texture.reset(GS::Texture2D::Create(image.Width, image.Height));
			texture->SetSmooth(true);
		}
		std::vector<uint8_t> pixels = GS::ToRGBA8(image);
		texture->SetData(pixels.data(), (unsigned int)pixels.size());
	}

	// Every node is evaluated, including ones that feed no output -- their
	// thumbnails are how a half-built branch is seen while it is built. A
	// node re-uploads only when its EvalCount moved, i.e. it really ran.
	void EvaluateAndUpload()
	{
		auto start = std::chrono::steady_clock::now();
		bool ranAnything = false;
		for (const GS::MaterialNode& node : m_Graph.Nodes())
		{
			const GS::NodeTypeInfo& info = GS::GetNodeTypeInfo(node.Type);
			Thumb& thumb = m_Thumbs[node.Id];
			if (info.IsOutput)
			{
				const GS::MaterialLink* link = m_Graph.FindInputLink(node.Id, 0);
				if (!link)
				{
					thumb.Texture.reset();
					continue;
				}
				// An output node's picture is what arrives at it.
				int from = link->FromNode, pin = link->FromPin;
				m_Graph.Evaluate(from, pin);
				uint64_t stamp = OutputThumbStamp(m_Graph, node.Id);
				if (stamp != thumb.Stamp || !thumb.Texture)
				{
					Upload(thumb.Texture, m_Graph.Evaluate(from, pin));
					thumb.Stamp = stamp;
					ranAnything = true;
				}
				continue;
			}
			m_Graph.Evaluate(node.Id, 0);
			if (m_Graph.EvalCount(node.Id) != thumb.EvalCount)
			{
				Upload(thumb.Texture, m_Graph.Evaluate(node.Id, 0));
				thumb.EvalCount = m_Graph.EvalCount(node.Id);
				ranAnything = true;
			}
		}

		if (m_Graph.Version() != m_PreviewVersion)
		{
			for (int i = 0; i < 4; i++)
				Upload(m_Previews[i], m_Graph.EvaluateOutput((GS::MaterialGraph::Output)i));
			m_PreviewVersion = m_Graph.Version();
			ranAnything = true;
			if (g_OnMaterialEdited)
				g_OnMaterialEdited(m_Path, m_Graph);
		}

		if (ranAnything)
			m_LastEvalMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	static void Image(const std::shared_ptr<GS::Texture2D>& texture, float size)
	{
		if (!texture)
		{
			ImGui::Dummy(ImVec2(size, size));
			return;
		}
		// v flipped: row 0 of a GL texture is its bottom, ImGui's is the top.
		ImGui::Image((ImTextureID)(intptr_t)texture->GetRendererID(), ImVec2(size, size),
			ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
	}

	// ---- The graph -------------------------------------------------------

	static int InputAttr(int node, int pin) { return node * 16 + pin; }
	static int OutputAttr(int node, int pin) { return node * 16 + 8 + pin; }

	static unsigned int PinColour(GS::PinType type)
	{
		return type == GS::PinType::Colour ? IM_COL32(230, 150, 60, 255) : IM_COL32(170, 170, 170, 255);
	}

	void DrawGraph()
	{
		// Captured here, not in the add menu: inside a popup, GetWindowPos
		// is the popup's, and new nodes landed wherever that happened to be.
		m_CanvasOrigin = ImGui::GetCursorScreenPos();
		ImNodes::BeginNodeEditor();

		for (int id : m_PushPositions)
			if (const GS::MaterialNode* node = m_Graph.FindNode(id))
				ImNodes::SetNodeGridSpacePos(id, ImVec2(node->Position.x, node->Position.y));
		m_PushPositions.clear();

		for (const GS::MaterialNode& node : m_Graph.Nodes())
		{
			const GS::NodeTypeInfo& info = GS::GetNodeTypeInfo(node.Type);
			ImNodes::BeginNode(node.Id);
			ImNodes::BeginNodeTitleBar();
			ImGui::TextUnformatted(info.Name);
			ImNodes::EndNodeTitleBar();

			for (int pin = 0; pin < (int)info.Inputs.size(); pin++)
			{
				// An Any input shows the type its node has resolved to.
				GS::PinType type = info.Inputs[pin].Type == GS::PinType::Any
					? m_Graph.OutputType(node.Id, 0) : info.Inputs[pin].Type;
				ImNodes::PushColorStyle(ImNodesCol_Pin, PinColour(type));
				ImNodes::BeginInputAttribute(InputAttr(node.Id, pin));
				ImGui::TextUnformatted(info.Inputs[pin].Name);
				ImNodes::EndInputAttribute();
				ImNodes::PopColorStyle();
			}

			Image(m_Thumbs[node.Id].Texture, 64.0f);

			for (int pin = 0; pin < (int)info.Outputs.size(); pin++)
			{
				ImNodes::PushColorStyle(ImNodesCol_Pin, PinColour(m_Graph.OutputType(node.Id, pin)));
				ImNodes::BeginOutputAttribute(OutputAttr(node.Id, pin));
				float width = ImGui::CalcTextSize(info.Outputs[pin].Name).x;
				ImGui::Indent(64.0f - width);
				ImGui::TextUnformatted(info.Outputs[pin].Name);
				ImGui::Unindent(64.0f - width);
				ImNodes::EndOutputAttribute();
				ImNodes::PopColorStyle();
			}
			ImNodes::EndNode();
		}

		const std::vector<GS::MaterialLink>& links = m_Graph.Links();
		for (int i = 0; i < (int)links.size(); i++)
			ImNodes::Link(i, OutputAttr(links[i].FromNode, links[i].FromPin), InputAttr(links[i].ToNode, links[i].ToPin));

		bool canvasHovered = ImNodes::IsEditorHovered();
		ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
		ImNodes::EndNodeEditor();

		// Positions: imnodes owns them while a node is dragged; the graph
		// keeps a copy so they are saved and survive undo. Only a real move is
		// copied back -- imnodes stores editor space and converts through the
		// panning, and the last-bit round-off of that trip would otherwise
		// read as an edit: a phantom undo step and "(unsaved)" on open.
		for (const GS::MaterialNode& node : m_Graph.Nodes())
		{
			ImVec2 p = ImNodes::GetNodeGridSpacePos(node.Id);
			if (std::abs(p.x - node.Position.x) > 0.01f || std::abs(p.y - node.Position.y) > 0.01f)
				m_Graph.SetPosition(node.Id, glm::vec2(p.x, p.y));
		}

		int start = 0, end = 0;
		if (ImNodes::IsLinkCreated(&start, &end))
		{
			// Either end may be the output: a link can be dragged backwards.
			if (start % 16 < 8)
				std::swap(start, end);
			if (start % 16 >= 8 && end % 16 < 8)
			{
				GS::ConnectResult result = m_Graph.Connect(start / 16, start % 16 - 8, end / 16, end % 16);
				if (result != GS::ConnectResult::Ok)
				{
					m_Status = result == GS::ConnectResult::Cycle ? "Refused: that link would make a cycle."
						: result == GS::ConnectResult::TypeMismatch ? "Refused: pin types differ (grey vs colour)."
						: "Refused: no such pin.";
					m_StatusIsError = true;
				}
			}
		}
		int destroyed = 0;
		if (ImNodes::IsLinkDestroyed(&destroyed) && destroyed < (int)m_Graph.Links().size())
		{
			GS::MaterialLink link = m_Graph.Links()[destroyed];
			m_Graph.Disconnect(link.ToNode, link.ToPin);
		}

		if (canvasHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		{
			m_AddAt = ImGui::GetMousePos();
			ImGui::OpenPopup("AddNode");
		}
		DrawAddMenu();
		m_SelectedNode = -1;
		if (ImNodes::NumSelectedNodes() == 1)
			ImNodes::GetSelectedNodes(&m_SelectedNode);
	}

	void DrawAddMenu()
	{
		if (!ImGui::BeginPopup("AddNode"))
			return;
		static const char* categories[] = { "Generators", "Filters", "Combine", "Colour", "Derived", "Outputs" };
		for (const char* category : categories)
		{
			if (!ImGui::BeginMenu(category))
				continue;
			for (int t = 0; t < (int)GS::NodeType::Count; t++)
			{
				const GS::NodeTypeInfo& info = GS::GetNodeTypeInfo((GS::NodeType)t);
				if (std::strcmp(info.Category, category) != 0)
					continue;
				bool present = info.IsOutput && m_Graph.FindOutputNode(OutputKind((GS::NodeType)t)) >= 0;
				if (ImGui::MenuItem(info.Name, nullptr, false, !present))
				{
					// Grid space = screen - canvas origin - panning; the
					// canvas origin is this child window's top-left.
					ImVec2 pan = ImNodes::EditorContextGetPanning();
					glm::vec2 grid(m_AddAt.x - m_CanvasOrigin.x - pan.x, m_AddAt.y - m_CanvasOrigin.y - pan.y);
					int id = m_Graph.AddNode((GS::NodeType)t, grid);
					if (id >= 0)
						m_PushPositions.push_back(id);
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}

	static GS::MaterialGraph::Output OutputKind(GS::NodeType type)
	{
		switch (type)
		{
		case GS::NodeType::OutAlbedo: return GS::MaterialGraph::Output::Albedo;
		case GS::NodeType::OutHeight: return GS::MaterialGraph::Output::Height;
		case GS::NodeType::OutNormal: return GS::MaterialGraph::Output::Normal;
		default:                      return GS::MaterialGraph::Output::Roughness;
		}
	}

	// ---- Inspector -------------------------------------------------------

	void DrawInspector()
	{
		const GS::MaterialNode* node = m_Graph.FindNode(m_SelectedNode);
		if (!node)
		{
			ImGui::TextDisabled("Select one node to edit it.");
			ImGui::Spacing();
			ImGui::TextDisabled("Right-click the canvas to add a node.");
			ImGui::TextDisabled("Delete removes the selection.");
			ImGui::TextDisabled("Ctrl+click a pin to detach a link.");
			return;
		}
		const GS::NodeTypeInfo& info = GS::GetNodeTypeInfo(node->Type);
		int id = node->Id;
		ImGui::SeparatorText(info.Name);
		Image(m_Thumbs[id].Texture, ImGui::GetContentRegionAvail().x);

		for (int i = 0; i < (int)info.Params.size(); i++)
		{
			const GS::ParamInfo& param = info.Params[i];
			float value = m_Graph.FindNode(id)->Params[i];
			switch (param.Kind)
			{
			case GS::ParamKind::Float:
				if (ImGui::SliderFloat(param.Name, &value, param.Min, param.Max, "%.3f"))
					m_Graph.SetParam(id, i, value);
				break;
			case GS::ParamKind::Int:
			{
				int v = (int)value;
				if (ImGui::SliderInt(param.Name, &v, (int)param.Min, (int)param.Max))
					m_Graph.SetParam(id, i, (float)v);
				break;
			}
			case GS::ParamKind::Enum:
			{
				int v = (int)value;
				if (ImGui::Combo(param.Name, &v, param.Labels.data(), (int)param.Labels.size()))
					m_Graph.SetParam(id, i, (float)v);
				break;
			}
			}
		}

		if (info.HasColour)
		{
			glm::vec4 colour = m_Graph.FindNode(id)->Colour;
			if (ImGui::ColorEdit4("colour", &colour.x))
				m_Graph.SetColour(id, colour);
		}

		if (info.HasRamp)
			DrawRamp(id);
	}

	void DrawRamp(int id)
	{
		std::vector<GS::GradientStop> ramp = m_Graph.FindNode(id)->Ramp;
		bool changed = false;
		int remove = -1;
		ImGui::SeparatorText("Ramp");
		for (int s = 0; s < (int)ramp.size(); s++)
		{
			ImGui::PushID(s);
			ImGui::SetNextItemWidth(60.0f);
			changed |= ImGui::DragFloat("##pos", &ramp[s].Position, 0.005f, 0.0f, 1.0f, "%.2f");
			ImGui::SameLine();
			changed |= ImGui::ColorEdit4("##col", &ramp[s].Colour.x, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			if (ramp.size() > 1 && ImGui::SmallButton("x"))
				remove = s;
			ImGui::PopID();
		}
		if (remove >= 0)
		{
			ramp.erase(ramp.begin() + remove);
			changed = true;
		}
		if (ramp.size() < 8 && ImGui::SmallButton("+ stop"))
		{
			ramp.push_back({ 1.0f, ramp.empty() ? glm::vec4(1.0f) : ramp.back().Colour });
			changed = true;
		}
		if (changed)
			m_Graph.SetRamp(id, ramp);
	}

	void DrawPreviewStrip()
	{
		static const char* names[] = { "Albedo", "Height", "Normal", "Roughness" };
		for (int i = 0; i < 4; i++)
		{
			if (i)
				ImGui::SameLine();
			ImGui::BeginGroup();
			Image(m_Previews[i], 96.0f);
			ImGui::TextDisabled("%s", names[i]);
			ImGui::EndGroup();
		}
	}

	// ---- Undo ------------------------------------------------------------

	// Snapshot undo by diffing: whenever nothing is mid-drag, the graph's
	// text is compared with the last settled text, and a difference is one
	// undo step. Every kind of edit -- a slider, a link, a move, a ramp stop
	// -- is caught the same way with no command per action, and a whole
	// slider drag is one step because nothing commits until it is released.
	void CommitIfChanged()
	{
		std::string now = GS::SerializeMaterialGraph(m_Graph);
		if (now == m_Committed)
			return;
		m_Undo.push_back(m_Committed);
		m_Redo.clear();
		m_Committed = std::move(now);
	}

	void Restore(const std::string& text)
	{
		GS::MaterialGraph graph;
		std::string error;
		if (!GS::DeserializeMaterialGraph(text, graph, error))
			return;   // our own serialisation; failing would be a bug, and changing nothing is safest
		int divisor = m_Graph.Resolution() / m_Graph.EvalResolution();
		m_Graph = std::move(graph);
		m_Graph.SetPreviewDivisor(divisor);
		m_Committed = text;
		ResetViewState();
	}

	void Undo()
	{
		if (m_Undo.empty())
			return;
		m_Redo.push_back(m_Committed);
		std::string text = m_Undo.back();
		m_Undo.pop_back();
		Restore(text);
	}

	void Redo()
	{
		if (m_Redo.empty())
			return;
		m_Undo.push_back(m_Committed);
		std::string text = m_Redo.back();
		m_Redo.pop_back();
		Restore(text);
	}

	void ResetViewState()
	{
		m_Thumbs.clear();
		m_PreviewVersion = ~0ull;
		m_PushPositions.clear();
		for (const GS::MaterialNode& node : m_Graph.Nodes())
			m_PushPositions.push_back(node.Id);
		if (m_NodesContext)
		{
			ImNodes::SetCurrentContext(m_NodesContext);
			ImNodes::ClearNodeSelection();
			ImNodes::ClearLinkSelection();
		}
	}

	void HandleShortcuts()
	{
		if (!g_MaterialEditorFocused || ImGui::GetIO().WantTextInput)
			return;
		bool ctrl = ImGui::GetIO().KeyCtrl;
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo();
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			// Links first: their ids are indices, which removing a node shifts.
			int linkCount = ImNodes::NumSelectedLinks();
			if (linkCount > 0)
			{
				std::vector<int> ids(linkCount);
				ImNodes::GetSelectedLinks(ids.data());
				std::vector<GS::MaterialLink> doomed;
				for (int id : ids)
					if (id < (int)m_Graph.Links().size())
						doomed.push_back(m_Graph.Links()[id]);
				for (const GS::MaterialLink& link : doomed)
					m_Graph.Disconnect(link.ToNode, link.ToPin);
				ImNodes::ClearLinkSelection();
			}
			int nodeCount = ImNodes::NumSelectedNodes();
			if (nodeCount > 0)
			{
				std::vector<int> ids(nodeCount);
				ImNodes::GetSelectedNodes(ids.data());
				for (int id : ids)
				{
					m_Graph.RemoveNode(id);
					m_Thumbs.erase(id);
				}
				ImNodes::ClearNodeSelection();
			}
		}
	}

	ImNodesContext* m_NodesContext = nullptr;
	GS::MaterialGraph m_Graph;
	std::string m_Path;
	std::string m_Committed, m_Saved;
	std::vector<std::string> m_Undo, m_Redo;
	std::unordered_map<int, Thumb> m_Thumbs;
	std::shared_ptr<GS::Texture2D> m_Previews[4];
	uint64_t m_PreviewVersion = ~0ull;
	std::vector<int> m_PushPositions;
	std::string m_Status;
	bool m_StatusIsError = false;
	bool m_Dragging = false;
	int m_FocusFrames = 0;
	std::string m_PendingOpen;
	int m_SelectedNode = -1;
	ImVec2 m_AddAt, m_CanvasOrigin;
	float m_LastEvalMs = 0.0f;
};

inline MaterialEditorPanel* g_MaterialEditor = nullptr;
