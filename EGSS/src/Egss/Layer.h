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

		// Simulation. Called zero or more times per frame with a step that is
		// always the same length, so results do not depend on framerate and a
		// replay reproduces exactly. Anything physical belongs here.
		virtual void OnFixedUpdate(Timestep fixedStep) {}

		// Presentation. Called exactly once per frame with the real elapsed
		// time. Camera feel, animation, and drawing belong here; use
		// Application::Get().GetInterpolationAlpha() to blend between the last
		// two simulation states so motion stays smooth.
		virtual void OnUpdate(Timestep ts) {}

		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event& event) {}

		inline const std::string& GetName() const { return m_DebugName; }
	protected:
		std::string m_DebugName;
	};

}
