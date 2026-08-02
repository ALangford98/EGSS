#pragma once

#include <Egss.h>
#include <imgui.h>

// Which demo is live, and the base class every demo derives from.
//
// This is sandbox machinery, not an engine pattern. A real game has one layer
// and none of this -- you would write your layer, PushLayer it, and stop. All
// of the below exists purely so several demos can share one binary.
//
// ---------------------------------------------------------------------------
// Adding a demo: two steps.
//
//   1. Write a class deriving from DemoLayer, overriding the OnDemo* hooks.
//   2. Add one line to the table in DemoRegistry.h.
//
// No enum, no parallel name array, no PushLayer. The registry holds the name
// and the factory together, so the two cannot drift apart -- which they did,
// repeatedly, when they lived in different files.
// ---------------------------------------------------------------------------

// Index into the registry. Deliberately not an enum: the registry is the one
// source of truth for what exists, and numbering it separately only created a
// second thing to keep in step.
using DemoId = int;
constexpr DemoId InvalidDemo = -1;

// Index into s_Demos. Change to whichever demo you are working on.
inline DemoId g_ActiveDemo = 3;

// Base for every demo layer.
//
// The point of it is the guard. Every pushed layer keeps running whichever
// demo is selected -- OnAttach, OnUpdate and OnEvent all fire for all of them
// -- so each demo used to open every override with:
//
//     if (g_ActiveDemo != Demo::Whatever)
//         return;
//
// four times per demo. Forgetting one is not a compile error; it is a demo
// quietly drawing over another, or a looping sound playing underneath a demo
// that never started it. Both of those actually happened here.
//
// Now the guard lives in one place and the Layer entry points are `final`, so
// a derived class cannot take the unguarded path even by accident.
class DemoLayer : public Egss::Layer
{
public:
	DemoLayer(const std::string& name)
		: Egss::Layer(name)
	{
	}

	// Assigned by the registry when the layer is created.
	void SetDemoId(DemoId id) { m_DemoId = id; }
	DemoId GetDemoId() const { return m_DemoId; }

	bool IsActive() const { return m_DemoId == g_ActiveDemo; }

	// --- Override these instead of the Layer ones -------------------------
	// Each is called only while this demo is the selected one.
	virtual void OnDemoAttach() {}
	virtual void OnDemoActivated() {}     // became the live demo
	virtual void OnDemoDeactivated() {}   // stopped being it -- stop loops here
	virtual void OnDemoFixedUpdate(Egss::Timestep step) { (void)step; }
	virtual void OnDemoUpdate(Egss::Timestep ts) { (void)ts; }
	virtual void OnDemoImGui() {}
	virtual void OnDemoEvent(Egss::Event& e) { (void)e; }

	// --- Layer, sealed ----------------------------------------------------
	// OnAttach is deliberately *not* guarded: assets have to be built whether
	// or not this demo is showing. Anything continuous -- a looping sound, a
	// timer -- belongs in OnDemoActivated instead, which is exactly the
	// mistake the audio bug was.
	void OnAttach() final { OnDemoAttach(); }

	void OnFixedUpdate(Egss::Timestep step) final
	{
		if (!IsActive())
			return;

		OnDemoFixedUpdate(step);
	}

	void OnUpdate(Egss::Timestep ts) final
	{
		// Activation edges are detected here rather than by the selector, so a
		// demo can start and stop continuous things without anything else
		// having to know it needs telling.
		bool active = IsActive();
		if (active != m_WasActive)
		{
			m_WasActive = active;

			if (active)
				OnDemoActivated();
			else
				OnDemoDeactivated();
		}

		if (!active)
			return;

		OnDemoUpdate(ts);
	}

	void OnImGuiRender() final
	{
		if (!IsActive())
			return;

		OnDemoImGui();
	}

	void OnEvent(Egss::Event& e) final
	{
		if (!IsActive())
			return;

		OnDemoEvent(e);
	}
private:
	DemoId m_DemoId = InvalidDemo;
	bool m_WasActive = false;
};
