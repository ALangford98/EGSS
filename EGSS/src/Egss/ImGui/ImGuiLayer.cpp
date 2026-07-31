#include "egsspch.h"
#include "Egss/ImGui/ImGuiLayer.h"

#include "Egss/Application.h"
#include "Egss/Log.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

namespace Egss {

	ImGuiLayer::ImGuiLayer()
		: Layer("ImGuiLayer")
	{
	}

	ImGuiLayer::~ImGuiLayer()
	{
	}

	void ImGuiLayer::OnAttach()
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();

		// Deliberately NOT ImGuiConfigFlags_NavEnableKeyboard. With it, any
		// nav-focused widget makes io.WantCaptureKeyboard true, and OnEvent
		// below then marks every key event handled -- so a stray focused
		// button silently eats the game's keys. Without it the keyboard is
		// only captured while a text field is active, which is what a game
		// wants.
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

		ImGui::StyleColorsDark();

		GLFWwindow* window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());

		// The GLFW backend installs its own callbacks and chains to the ones
		// the window already set, so engine events keep flowing.
		ImGui_ImplGlfw_InitForOpenGL(window, true);
		ImGui_ImplOpenGL3_Init("#version 330");

		EGSS_CORE_INFO("ImGui {0} initialized", IMGUI_VERSION);
	}

	void ImGuiLayer::OnDetach()
	{
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
	}

	void ImGuiLayer::OnEvent(Event& e)
	{
		if (m_BlockEvents)
		{
			ImGuiIO& io = ImGui::GetIO();
			if (io.WantCaptureMouse && e.IsInCategory(EventCategoryMouse))
				e.SetHandled(true);
			if (io.WantCaptureKeyboard && e.IsInCategory(EventCategoryKeyboard))
				e.SetHandled(true);
		}
	}

	void ImGuiLayer::Begin()
	{
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		// A full-window dock target, created before any layer renders so that
		// every panel can dock into it. PassthruCentralNode leaves the middle
		// undrawn when nothing is docked there, rather than covering the
		// window with an opaque background.
		if (m_DockspaceEnabled)
			ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
	}

	void ImGuiLayer::End()
	{
		ImGuiIO& io = ImGui::GetIO();
		Application& app = Application::Get();
		io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	}

}
