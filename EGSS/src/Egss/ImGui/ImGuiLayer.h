#pragma once

#include "Egss/Layer.h"

#include "Egss/Events/ApplicationEvent.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

namespace Egss {

	// Pushed as an overlay by Application, so it sits above every game layer
	// and sees input first.
	class EGSS_API ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer();

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& e) override;

		// Bracket the per-layer OnImGuiRender calls.
		void Begin();
		void End();

		// When set, ImGui consumes mouse and keyboard input that lands on its
		// windows instead of letting it fall through to the game.
		void BlockEvents(bool block) { m_BlockEvents = block; }

		// A game that draws to the whole window wants this off; an editor
		// wants it on.
		void EnableDockspace(bool enable) { m_DockspaceEnabled = enable; }

		// Lets a panel be dragged clean out of the window and become its own
		// OS window. Must be set before OnAttach -- the flag is read once when
		// the ImGui context is created, and the backend builds or skips its
		// platform interface on the strength of it.
		//
		// Off by default. Every extra viewport is a real window with its own
		// GL context, which is a cost a game that never undocks a panel should
		// not pay; and see the changelog for what it does to the mouse
		// coordinate space.
		void EnableViewports(bool enable) { m_ViewportsEnabled = enable; }
		bool ViewportsEnabled() const { return m_ViewportsEnabled; }
	private:
		bool m_BlockEvents = true;
		bool m_DockspaceEnabled = true;
		bool m_ViewportsEnabled = false;
	};

}
