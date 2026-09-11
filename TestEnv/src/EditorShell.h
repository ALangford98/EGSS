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
#include "FileTreePanel.h"
#include "ScriptEngine.h"
#include "TerminalPanel.h"
#include "TextEditorPanel.h"

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
		m_TextEditor.SetScriptEngine(&m_ScriptEngine);

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
		// over the panels docked into it. Input reaches the demo through the
		// "Scene" window docked into the central node below, not through a
		// passthru hole -- there is always a real window there now.
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
			| ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground
			| ImGuiWindowFlags_NoDocking;

		ImGui::Begin("##EditorShell", nullptr, flags);

		ImGui::PopStyleVar(3);

		ImGuiID dock = ImGui::GetID("EditorDock");

		// **No `PassthruCentralNode` here.** That flag exists to leave an
		// *empty* central node transparent -- but "Scene" and "Editor" are
		// always docked into this one, so it is never empty, and the flag's
		// own fallback for a non-empty central node is worse than not having
		// it: ImGui still paints the whole dockspace with `ImGuiCol_WindowBg`
		// every frame in that case (imgui.cpp's DockNodeUpdate, the
		// `render_dockspace_bg` / `central_node_hole` branch), because the
		// "hole" it would normally cut out only applies while the node is
		// empty. That fill lands on top of the demo's already-rendered pixels
		// -- raw GL, drawn earlier in the frame, outside ImGui's draw list --
		// and was measured to darken Cube3D's lit icosahedron from
		// (251,179,248) to functionally black. Leaving the flag off skips
		// that whole code path.
		// A second, distinct ImGui fill path (imgui.cpp:19529-19534) paints
		// ImGuiCol_DockingEmptyBg over the *empty* central node every frame
		// -- this is separate from the non-empty-node fill the comment above
		// already explains, and it now fires unconditionally since removing
		// PassthruCentralNode also removed that fill's own zero-color
		// fallback. Neutralized here rather than relied upon never
		// happening: an empty central node is a real, reachable state (a
		// stale imgui.ini/layout preset from before Scene/Editor existed,
		// or both tabs dragged out at runtime -- see the migration safety
		// net below this DockSpace() call for the actual recovery).
		ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, IM_COL32(0, 0, 0, 0));
		ImGui::DockSpace(dock, ImVec2(0.0f, 0.0f));
		ImGui::PopStyleColor();

		// **The window `DockSpace()` actually hosts docking in is not
		// "##EditorShell".** It creates its own internal child window (named
		// "<caller>/DockSpace_<id>") to be the real dock host -- confirmed
		// against the vendored ImGui source and a standalone probe against
		// it (io.WantCaptureMouse over "Scene"'s rect stayed true even after
		// giving "Scene" NoMouseInputs and cutting a hit-test hole on
		// `ImGui::GetCurrentWindow()` captured right after
		// `Begin("##EditorShell", ...)` -- that pointer is the wrong window).
		// The right one is the root dock node's own `HostWindow`, the same
		// pointer imgui.cpp's `DockNodeUpdate` uses internally for exactly
		// this. Fetched here, right after `DockSpace()`, because `BuildLayout`
		// below may destroy and recreate the *node* (DockBuilderRemoveNode/
		// AddNode) on the first frame -- the underlying host *window* (found
		// by name) is unaffected by that and this pointer stays valid.
		ImGuiWindow* hostWindow = nullptr;
		if (ImGuiDockNode* rootNode = ImGui::DockBuilderGetNode(dock))
			hostWindow = rootNode->HostWindow;

		if (!m_Built)
			BuildLayout(dock, viewport->WorkSize);

		// Migration safety net: an imgui.ini saved before "Scene"/"Editor"
		// existed, a layout preset saved before this change, or both tabs
		// dragged out of the central node at runtime all leave it with
		// nothing docked in it. Re-dock them the same frame this is
		// detected, rather than let the node sit empty -- Part A above
		// keeps that state from looking broken, but the demo/editor still
		// need somewhere to actually render, so this closes the gap for
		// real rather than just hiding its symptom.
		if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock))
		{
			if (central->Windows.Size == 0)
			{
				ImGui::DockBuilderDockWindow("Scene", central->ID);
				ImGui::DockBuilderDockWindow("Editor", central->ID);
				ImGui::DockBuilderFinish(dock);
			}
		}

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

		ImGui::End();

		// "Scene" and "Editor" tab together in the center node. g_Viewport
		// reflects whichever one is the selected tab this frame (or neither,
		// same invalid-rect meaning the !g_EditorShell branch above already
		// uses) -- ImGui::Begin() returns false for a docked window that
		// isn't the currently selected tab, which is exactly the signal we
		// need. Under --hide-ui neither Begin() call below ever runs (this
		// whole function returns before reaching them, via the !g_EditorShell
		// check, on any run that never draws UI at all -- and on a normal
		// run where the UI later gets hidden mid-session, OnImGuiRender
		// itself is simply never invoked by Application.cpp), so g_Viewport
		// reverts to the full-window rect exactly as before this change.
		g_Viewport = ViewportRect();

		// NoMouseInputs so FindHoveredWindowEx (imgui.cpp) skips "Scene"
		// entirely during its hover search rather than treating it as a
		// normal window that happens to draw nothing -- otherwise every
		// demo's own mouse handling (WASD look, click-to-select, gizmo drag;
		// all of it gated on `!io.WantCaptureMouse`) goes dead the instant
		// the mouse is over the viewport, because ImGui now believes a real
		// window owns that input. This is not a corner case: it is the
		// default, every-frame state whenever the editor shell is on.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool sceneVisible = ImGui::Begin("Scene", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground
			| ImGuiWindowFlags_NoMouseInputs);
		ImGui::PopStyleVar();

		if (sceneVisible)
		{
			ImVec2 pos = ImGui::GetCursorScreenPos();
			ImVec2 size = ImGui::GetContentRegionAvail();
			ImVec2 displaySize = ImGui::GetIO().DisplaySize;

			// Same axis flip the old central-node code needed: OpenGL's
			// origin is bottom-left, ImGui's is top-left.
			g_Viewport.X = (int)pos.x;
			g_Viewport.Y = (int)(displaySize.y - pos.y - size.y);
			g_Viewport.Width = (int)size.x;
			g_Viewport.Height = (int)size.y;

			// NoMouseInputs alone only stops "Scene" itself from claiming
			// the hover -- the search then falls through to whatever is
			// still underneath it with no hole cut, which is the dockspace
			// host window and, beneath that, "##EditorShell" itself (both
			// span the full work area and neither has NoMouseInputs; either
			// one would still swallow the input). Cutting the hole on both
			// -- exactly what ImGui's own native `PassthruCentralNode`/
			// `central_node_hole` mechanism did (imgui.cpp ~line 19511-19513,
			// `SetWindowHitTestHole(host_window, ...)` followed by the same
			// call on `host_window->ParentWindow`) -- is what actually
			// reopens the hole all the way through to the demo. Verified
			// empirically with a standalone probe against this vendored
			// ImGui before writing this: neither hole alone was sufficient,
			// only both together closed `io.WantCaptureMouse`.
			if (hostWindow)
			{
				ImGui::SetWindowHitTestHole(hostWindow, pos, size);
				if (hostWindow->ParentWindow)
					ImGui::SetWindowHitTestHole(hostWindow->ParentWindow, pos, size);
			}
		}
		ImGui::End();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		m_TextEditor.OnImGuiRender();
		ImGui::PopStyleVar();

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
		ImGui::SameLine();
		if (ImGui::Button("Light"))
			PlaceLight();
		ImGui::NewLine();

		ImGui::SeparatorText("Import");
		ImGui::TextDisabled(".obj only -- see the plan's note on why glTF stays in ModelDemo for now.");
		ImGui::InputText("##importpath", m_ImportPath, sizeof(m_ImportPath));
		ImGui::SameLine();
		if (ImGui::Button("Import"))
			PlaceMesh(m_ImportPath, "Imported");

		ImGui::End();

		std::string clickedFile = m_FileTree.OnImGuiRender();
		if (!clickedFile.empty())
			m_TextEditor.OpenFile(clickedFile);

		m_Terminal.OnImGuiRender();

		ImGui::Begin("Build Output");
		if (!m_TextEditor.LastRunError().empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_TextEditor.LastRunError().c_str());
		}
		else if (!m_TextEditor.LastRunOutput().empty())
		{
			ImGui::TextUnformatted(m_TextEditor.LastRunOutput().c_str());
		}
		else
		{
			ImGui::TextDisabled("Run a script from the Editor tab to see its output here.");
		}
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

	// Same shape as PlaceCamera -- no mesh, nothing to resolve through
	// MeshCache -- with std::nullopt for the camera slot so the command's
	// four-way constructor still reads as "everything this entity has".
	void PlaceLight()
	{
		GS::EntityId selection = EditorHistory::Push(std::make_unique<PlaceEntityCommand>(
			"Light", GS::TransformComponent{}, std::nullopt, std::nullopt, GS::LightComponent{}));

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

		ImGui::DockBuilderDockWindow("Files", left);
		ImGui::DockBuilderDockWindow("Outliner", left);
		ImGui::DockBuilderDockWindow("Inspector", rightLower);

		ImGui::DockBuilderDockWindow("Scene", centre);
		ImGui::DockBuilderDockWindow("Editor", centre);

		ImGui::DockBuilderFinish(dock);
	}

	bool m_Built = false;
	bool m_ForceDefault = false;
	TerminalPanel m_Terminal;
	ScriptEngine m_ScriptEngine;
	TextEditorPanel m_TextEditor;
	FileTreePanel m_FileTree;
};
