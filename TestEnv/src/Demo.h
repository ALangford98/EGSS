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
// Where a demo should draw. Filled by `EditorShell`; invalid means "the whole
// window", which is what `--hide-ui` and `--no-editor` leave it as.
struct ViewportRect
{
	int X = 0;
	int Y = 0;
	int Width = 0;
	int Height = 0;

	bool Valid() const { return Width > 0 && Height > 0; }
};

inline ViewportRect g_Viewport;

// Set by `EditorShell`; zero means there is no editor layout.
inline unsigned int g_DemoDock = 0;

using DemoId = int;
constexpr DemoId InvalidDemo = -1;

// Index into s_Demos. Change to whichever demo you are working on.
//
// **16 is the terrain lab**, which is where the ground, the weather and the
// vegetation are being worked on. It is the last entry because the array's
// order is the recording file format -- see the note in `DemoRegistry.h` --
// so a new demo is appended and never inserted, whatever it is for.
inline DemoId g_ActiveDemo = 16;

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

	// --- Replayable parameters --------------------------------------------
	//
	// **One line per slider that reaches the simulation.** A recording captures
	// what a person does through the keyboard and mouse; an ImGui slider is
	// neither, so moving one used to desynchronise every replay of that session
	// silently. Register the variable and the recorder samples it per fixed step
	// and the player writes it back. See `Egss::ReplayParams`.
	//
	// Names are prefixed with the demo's own, because "Gravity" belongs to
	// Physics2D *and* to Physics3D and a recording that confused the two would
	// replay one demo's slider into another's world.
	//
	// Only register what the *simulation* reads. A colour, a "show colliders"
	// checkbox or a camera angle changes what you see and not what happens, and
	// recording those would make a replay fail to match for reasons that do not
	// matter -- while adding entries to every file.
	void RegisterParam(const std::string& name, float* value)
	{
		Egss::ReplayParams::RegisterFloat(GetName() + "/" + name, value);
	}

	void RegisterParam(const std::string& name, int* value)
	{
		Egss::ReplayParams::RegisterInt(GetName() + "/" + name, value);
	}

	void RegisterParam(const std::string& name, bool* value)
	{
		Egss::ReplayParams::RegisterBool(GetName() + "/" + name, value);
	}

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

		// **The demo draws into the editor's central pane, not the window.**
		//
		// Setting the viewport is all it takes, and it is done here rather
		// than in each demo so that none of them had to change -- and so that
		// a demo written later gets it without knowing the shell exists.
		//
		// It degrades to the old behaviour on its own: `g_Viewport` is invalid
		// until the shell has laid the panels out, and stays invalid under
		// `--hide-ui` and `--no-editor`, in which case nothing here fires and
		// the demo owns the whole framebuffer exactly as before. That is what
		// keeps every existing capture byte-comparable.
		//
		// The camera's aspect ratio is *not* touched. A demo that built its
		// projection for 16:9 keeps it, so a narrow pane letterboxes rather
		// than distorting -- which is the right failure, because a stretched
		// scene is the kind of wrong that is easy to look at and not notice.
		if (g_Viewport.Valid())
			Egss::RenderCommand::SetViewport((unsigned int)g_Viewport.X,
				(unsigned int)g_Viewport.Y, (unsigned int)g_Viewport.Width,
				(unsigned int)g_Viewport.Height);

		OnDemoUpdate(ts);

		// Put it back, or ImGui draws its panels into the demo's pane.
		if (g_Viewport.Valid())
		{
			Egss::Window& window = Egss::Application::Get().GetWindow();

			Egss::RenderCommand::SetViewport(0, 0, window.GetWidth(),
				window.GetHeight());
		}
	}

	void OnImGuiRender() final
	{
		if (!IsActive())
			return;

		// **Put the demo's panel in the editor's left column.**
		//
		// `FirstUseEver` rather than `Always`: this is where a panel starts,
		// not where it is held. Dragging it somewhere better is the point of
		// having docking at all, and `imgui.ini` remembers the choice.
		//
		// Set here rather than in the shell because only this line runs
		// immediately before the demo opens its window, and `SetNextWindow*`
		// applies to whichever `Begin` comes next.
		if (g_DemoDock != 0)
			ImGui::SetNextWindowDockID((ImGuiID)g_DemoDock,
				ImGuiCond_FirstUseEver);

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
