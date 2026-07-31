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
	private:
		bool m_BlockEvents = true;
		bool m_DockspaceEnabled = true;
	};

}
