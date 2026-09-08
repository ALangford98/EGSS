#pragma once

// **The panels stop floating and the demo gets a viewport.**
//
// Every panel in this sandbox was an independent ImGui window dropped wherever
// it last happened to be, over a demo drawn across the whole framebuffer. That
// is fine for one panel and unreadable by the time there are four: they overlap
// the thing they are describing, they move when the window resizes, and the
// scene is always partly behind something.
//
// This lays them out: controls down the left with the demo selector under them,
// the profiler on the right, a spare pane along the bottom, and the demo itself
// in the middle. Nothing about the demos changed to make that happen.
//
// **Two things are load-bearing.**
//
// The layout is built with the docking builder *once*, into `imgui.ini`, and
// only if that file does not already describe it. So the arrangement is a
// starting point rather than a cage -- drag a panel somewhere better and it
// stays there, which is the whole reason to use docking rather than to place
// windows by hand every frame.
//
// And the demo is drawn into the central node by **setting the viewport**, not
// by rendering to a framebuffer and showing the texture. A framebuffer is the
// textbook answer and it would have broken every capture in this project:
// `--hide-ui` exists so an unattended run draws no panels at all, and with an
// off-screen target there would be nothing to blit it with. Setting the
// viewport degrades correctly instead -- no panels means the rect is the whole
// window, which is exactly what it was before this file existed.

#include <GS.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "Demo.h"
#include "DemoRegistry.h"
#include "EditorHistory.h"
#include "EditorProject.h"
#include "EditorSceneView.h"

// Off with `--no-editor`, because a layout is a preference and somebody
// debugging a single panel should not have to fight one.
inline bool g_EditorShell = true;

class EditorShell;
inline EditorShell* g_EditorShellInstance = nullptr;

class EditorShell : public GS::Layer
{
public:
	EditorShell() : Layer("EditorShell") {}

	void OnAttach() override
	{
		g_EditorShellInstance = this;

		const std::vector<std::string>& arguments =
			GS::Application::GetCommandLine();

		for (const std::string& argument : arguments)
			if (argument == "--no-editor")
				g_EditorShell = false;
	}

	// "Reset to default" (EditorMenuBar.h) needs a way back to the original
	// arrangement even after BuildLayout has already run once this session.
	// Setting m_Built false alone is not enough -- BuildLayout's own guard
	// treats an already-split dock as "leave it alone", which is exactly
	// what must be bypassed here and nowhere else.
	void ResetToDefaultLayout()
	{
		m_Built = false;
		m_ForceDefault = true;
	}

	void OnImGuiRender() override
	{
		if (!g_EditorShell)
		{
			// Hand the whole window back, or a run that turned the shell off
			// mid-session would keep drawing into the old rect.
			g_Viewport = ViewportRect();
			return;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();

		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

		// It covers the screen, so it must not steal focus or come forward
		// over the panels docked into it. Input still reaches the demo,
		// because the central node is passthru and a passthru node is a hole
		// rather than a surface.
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
			| ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground
			| ImGuiWindowFlags_NoDocking;

		ImGui::Begin("##EditorShell", nullptr, flags);

		ImGui::PopStyleVar(3);

		ImGuiID dock = ImGui::GetID("EditorDock");

		// **`PassthruCentralNode` is what leaves the middle transparent.**
		// Without it the central node paints itself and the demo behind it is
		// never seen -- which looks exactly like the scene failing to render.
		ImGui::DockSpace(dock, ImVec2(0.0f, 0.0f),
			ImGuiDockNodeFlags_PassthruCentralNode);

		if (!m_Built)
			BuildLayout(dock, viewport->WorkSize);

		// The central node's rectangle, in ImGui's screen coordinates. This is
		// read a frame after the panels were laid out, which is a frame of lag
		// nobody can see and avoids having to run the layout before the demo.
		// Recovered every frame, because the layout may have been loaded from
		// `imgui.ini` rather than built here -- in which case `BuildLayout`
		// returned early and never set it.
		if (g_DemoDock == 0)
			if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock))
				if (node->ChildNodes[0])
					g_DemoDock = (unsigned int)node->ChildNodes[0]->ID;

		if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock))
		{
			ImVec2 size = ImGui::GetIO().DisplaySize;

			// **OpenGL's origin is bottom-left and ImGui's is top-left.**
			// Reading the rect straight through puts the viewport upside down
			// in the window -- the demo appears at the top when the panels are
			// at the bottom, which reads as a layout bug rather than an axis
			// one.
			g_Viewport.X = (int)central->Pos.x;
			g_Viewport.Y = (int)(size.y - central->Pos.y - central->Size.y);
			g_Viewport.Width = (int)central->Size.x;
			g_Viewport.Height = (int)central->Size.y;
		}

		ImGui::End();

		// Named "Tools" now -- scene save/open/new moved to the File menu
		// (EditorMenuBar.h), and "Open as starting scene" moved there too,
		// below. What is left here is exactly the entity-creation controls,
		// which is what "Tools" means in a conventional editor layout.
		ImGui::Begin("Tools");

		ImGui::SeparatorText("Preset shapes");
		// One button per MeshCache primitive key -- see MeshCache::Get. Each
		// placed entity is immediately saveable, because its SourcePath is
		// exactly the key the cache resolved it through.
		struct Preset { const char* Label; const char* Path; };
		static const Preset presets[] = {
			{ "Cube", "primitive:cube" }, { "Sphere", "primitive:sphere" },
			{ "Plane", "primitive:plane" }, { "Cylinder", "primitive:cylinder" }
		};
		for (const Preset& preset : presets)
		{
			if (ImGui::Button(preset.Label))
				PlaceMesh(preset.Path, preset.Label);
			ImGui::SameLine();
		}
		// Not a preset shape -- a camera has no mesh or SourcePath to resolve
		// through MeshCache -- but it belongs beside them: both are "make a
		// new entity and select it" buttons for things a scene is built from.
		if (ImGui::Button("Camera"))
			PlaceCamera();
		ImGui::NewLine();

		ImGui::SeparatorText("Import");
		ImGui::TextDisabled(".obj only -- see the plan's note on why glTF stays in ModelDemo for now.");
		ImGui::InputText("##importpath", m_ImportPath, sizeof(m_ImportPath));
		ImGui::SameLine();
		if (ImGui::Button("Import"))
			PlaceMesh(m_ImportPath, "Imported");

		ImGui::End();

		ImGui::Begin("Terminal");
		ImGui::TextDisabled("Not built yet -- terminal sub-project.");
		ImGui::End();

		ImGui::Begin("Build Output");
		ImGui::TextDisabled("Not built yet.");
		ImGui::End();

		ImGui::Begin("Textures");
		ImGui::TextDisabled("Not built yet.");
		ImGui::End();
	}

private:
	char m_ImportPath[256] = "assets/models/";

	// Places a mesh -- imported or preset alike, both resolve through
	// MeshCache -- at the origin, tags it, and selects it, so a placed
	// object is immediately the thing the Inspector is showing.
	void PlaceMesh(const std::string& path, const std::string& name)
	{
		std::shared_ptr<GS::Mesh> geometry = GS::MeshCache::Get(path);
		if (!geometry)
			return;

		GS::MeshComponent mesh;
		mesh.SourcePath = path;
		mesh.Geometry = geometry;

		GS::EntityId selection = EditorHistory::Push(
			std::make_unique<PlaceEntityCommand>(name, GS::TransformComponent{}, mesh, std::nullopt));

		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}

	// Places a default-constructed camera and selects it. Separate from
	// PlaceMesh rather than an overload of it: a camera has no mesh and
	// nothing to resolve through MeshCache, so the two have almost nothing
	// in common besides "make an entity and select it".
	void PlaceCamera()
	{
		GS::EntityId selection = EditorHistory::Push(std::make_unique<PlaceEntityCommand>(
			"Camera", GS::TransformComponent{}, std::nullopt, GS::CameraComponent{}));

		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}

	// Built once into `imgui.ini`. Every demo has a panel and only one of them
	// is ever visible, so they all dock to the same slot on the left and the
	// active one takes it.
	void BuildLayout(ImGuiID dock, ImVec2 size)
	{
		m_Built = true;

		// Already arranged, by this code on an earlier run or by hand since.
		// Leaving it alone is the difference between a starting point and a
		// cage.
		bool alreadyArranged = ImGui::DockBuilderGetNode(dock)
			&& ImGui::DockBuilderGetNode(dock)->IsSplitNode();

		if (alreadyArranged && !m_ForceDefault)
			return;

		m_ForceDefault = false;

		ImGui::DockBuilderRemoveNode(dock);
		ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dock, size);

		ImGuiID centre = dock;

		ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left,
			0.24f, nullptr, &centre);

		ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
			0.26f, nullptr, &centre);

		// The output well: terminal, build log, textures -- none built yet,
		// but the slot exists now so each lands here without another layout
		// pass, the same reasoning DockBuilderDockWindow below applies to it.
		ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,
			0.22f, nullptr, &centre);

		// Tools under the Outliner: creation controls read far less often
		// than the entity list above them, the same relationship the demo
		// selector used to have with the controls above it.
		ImGuiID lower = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down,
			0.40f, nullptr, &left);

		// The right column is split too: the Inspector under the Profiler.
		// Both describe "the thing currently selected" -- a component's live
		// values on top, a scope's timings below -- so they share a column
		// rather than fighting the left column for space.
		ImGuiID rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
			0.45f, nullptr, &right);

		// **The demo's own panel goes here, and `DemoLayer` puts it there by
		// id rather than by name.** Docking by title was tried first and is
		// too fragile: a demo's panel is titled whatever its author chose,
		// which is not the name in the registry, and a list of both in a third
		// file is exactly the kind of thing that silently falls out of step.
		//
		// **Unchanged from before this reshuffle: `g_DemoDock` is `left`, not
		// `lower`.** `left` is where the Outliner sits and where each demo's
		// own panel tabs in alongside it; `lower` is the smaller strip
		// reserved for the two lists (Demos, Tools) below it. Pointing
		// `g_DemoDock` at `lower` instead would dock every demo's own panel
		// into that small list strip rather than the main left column -- a
		// real regression this reshuffle must not introduce.
		g_DemoDock = (unsigned int)left;

		ImGui::DockBuilderDockWindow("Demos", lower);
		ImGui::DockBuilderDockWindow("Tools", lower);
		ImGui::DockBuilderDockWindow("Profiler", right);
		ImGui::DockBuilderDockWindow("Terminal", bottom);
		ImGui::DockBuilderDockWindow("Build Output", bottom);
		ImGui::DockBuilderDockWindow("Textures", bottom);

		ImGui::DockBuilderDockWindow("Outliner", left);
		ImGui::DockBuilderDockWindow("Inspector", rightLower);

		ImGui::DockBuilderFinish(dock);
	}

	bool m_Built = false;
	bool m_ForceDefault = false;
};
