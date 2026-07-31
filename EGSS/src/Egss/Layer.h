#pragma once

#include "egsspch.h"
#include "Core.h"
#include "Timestep.h"
#include "Events/Event.h"

namespace Egss {

	// A slice of the application that can update, render, and consume events.
	// Layers are stacked; events travel from the top down so an overlay can
	// swallow input before the game sees it.
	class EGSS_API Layer
	{
	public:
		Layer(const std::string& name = "Layer");
		virtual ~Layer();

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep ts) {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event& event) {}

		inline const std::string& GetName() const { return m_DebugName; }
	protected:
		std::string m_DebugName;
	};

}
