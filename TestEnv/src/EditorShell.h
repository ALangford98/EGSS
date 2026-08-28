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

#include <Egss.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "Demo.h"
#include "DemoRegistry.h"

// Off with `--no-editor`, because a layout is a preference and somebody
// debugging a single panel should not have to fight one.
inline bool g_EditorShell = true;

class EditorShell : public Egss::Layer
{
public:
	EditorShell() : Layer("EditorShell") {}

	void OnAttach() override
	{
		const std::vector<std::string>& arguments =
			Egss::Application::GetCommandLine();

		for (const std::string& argument : arguments)
			if (argument == "--no-editor")
				g_EditorShell = false;
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

		// The spare pane. Named rather than left blank so it has somewhere to
		// dock to before there is anything to put in it.
		ImGui::Begin("Assets");
		ImGui::TextDisabled("Nothing here yet.");
		ImGui::TextDisabled("Docked bottom-centre, ready for a texture browser");
		ImGui::TextDisabled("or whatever the next thing needs a pane for.");
		ImGui::End();
	}

private:
	// Built once into `imgui.ini`. Every demo has a panel and only one of them
	// is ever visible, so they all dock to the same slot on the left and the
	// active one takes it.
	void BuildLayout(ImGuiID dock, ImVec2 size)
	{
		m_Built = true;

		// Already arranged, by this code on an earlier run or by hand since.
		// Leaving it alone is the difference between a starting point and a
		// cage.
		if (ImGui::DockBuilderGetNode(dock)
			&& ImGui::DockBuilderGetNode(dock)->IsSplitNode())
			return;

		ImGui::DockBuilderRemoveNode(dock);
		ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dock, size);

		ImGuiID centre = dock;

		ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left,
			0.24f, nullptr, &centre);

		ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
			0.26f, nullptr, &centre);

		ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,
			0.22f, nullptr, &centre);

		// The selector goes under the controls rather than beside them: it is
		// a list that is read top to bottom and it is used far less often than
		// whatever is above it.
		ImGuiID lower = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down,
			0.40f, nullptr, &left);

		// **The demo's own panel goes here, and `DemoLayer` puts it there by
		// id rather than by name.** Docking by title was tried first and is
		// too fragile: a demo's panel is titled whatever its author chose,
		// which is not the name in the registry, and a list of both in a third
		// file is exactly the kind of thing that silently falls out of step.
		g_DemoDock = (unsigned int)left;

		ImGui::DockBuilderDockWindow("Demos", lower);
		ImGui::DockBuilderDockWindow("Profiler", right);
		ImGui::DockBuilderDockWindow("Assets", bottom);

		ImGui::DockBuilderFinish(dock);
	}

	bool m_Built = false;
};
