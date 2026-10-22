#pragma once

// The cannon run: get round a course as fast as you can while cannons try
// to knock you off your feet.
//
// It is the parkour course's dummy, its moves and its dodge, on a course
// of pieces (RunCourse) with cannons covering the open stretches (Gunnery).
// The clock starts at the line and stops at the finish; a hit costs the
// time it takes to get up. The cannons charge before they fire -- a hiss
// from where they stand, a lamp blinking faster -- so the run is a matter
// of where you look: ahead, to find your way and time your moves, or at
// what is coming (hold Focus), to time your dodge. Markers at the edge of
// the picture say where a charge or a ball is when it is out of sight.
//
// He carries a sword and shield (Runtime/Armament.h): hold the shield up
// (the right button, which also looks at what is coming) to block a ball,
// raise it just as one arrives to parry it, swing (the left button) into
// one to send it back at its cannon -- two of those wreck one, or four
// cuts up close. `sword off` puts them away.
//
//   R    start again          N    a new course (from a new seed)
//   Esc  pause                `    the terminal: `course 4711` runs that
//                                  course, `course` the hand-made one
//
// Determinism: everything that moves does so in OnDemoFixedUpdate, from
// recorded input; the sounds, the markers and the pointer are the frame's.

#include <GS.h>
#include <imgui.h>

#include "CrashTest.h"
#include "Gunnery.h"
#include "RunCourse.h"

#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace CannonRunScreens
{
	inline const char* Hud()
	{
		return R"json({
			"theme": { "accent": "#ffb12e", "good": "#7fe3a4", "panel": "#0c1016d6", "dim": "#a9b3bd" },
			"root": { "type": "panel", "style": "clear", "stretch": "both", "padding": 0, "children": [
				{ "type": "panel", "anchor": "top", "pos": [0, 20], "layout": "vertical", "padding": 12, "gap": 0, "visible": "run.clock", "children": [
					{ "type": "label", "text": "{run.time}", "fontSize": 60, "align": "center" },
					{ "type": "label", "text": "{run.progress}", "style": "dim", "align": "center" } ] },
				{ "type": "panel", "anchor": "top", "pos": [0, 150], "style": "clear", "padding": 0, "visible": "run.split", "children": [
					{ "type": "label", "text": "{run.split}", "style": "accent", "fontSize": 34, "align": "center" } ] },
				{ "type": "panel", "anchor": "top-right", "pos": [28, 28], "layout": "vertical", "padding": 14, "gap": 2, "children": [
					{ "type": "label", "text": "HITS  {run.hits}", "style": "title" },
					{ "type": "label", "text": "dodges  {run.dodges}", "style": "dim" },
					{ "type": "label", "text": "blocked {run.blocks}   parried {run.parries}   returned {run.returns}", "style": "dim", "visible": "run.armed" },
					{ "type": "label", "text": "cannons wrecked  {run.wrecked}", "style": "accent", "visible": "run.armed" },
					{ "type": "label", "text": "{run.course}", "style": "dim" } ] },
				{ "type": "panel", "anchor": "top-left", "pos": [28, 28], "layout": "vertical", "padding": 14, "gap": 2, "visible": "run.move", "children": [
					{ "type": "label", "text": "{run.move}", "style": "title" } ] },
				{ "type": "panel", "anchor": "bottom", "pos": [0, 22], "padding": 12, "children": [
					{ "type": "label", "text": "{keys.dodge} dodge, with a direction     hold {keys.focus} look at what is coming     {keys.sprint} run     {keys.jump} jump / vault / climb     {keys.restart} restart     {keys.cancel} pause", "style": "dim", "visible": "run.unarmed" },
					{ "type": "label", "text": "{keys.attack} swing     hold {keys.block} shield up (raise it as one lands: parry)     Q dodge     {keys.sprint} run     {keys.jump} jump / vault / climb     {keys.restart} restart     {keys.cancel} pause", "style": "dim", "visible": "run.armed" } ] },
				{ "type": "panel", "anchor": "bottom", "pos": [0, 96], "layout": "vertical", "padding": 22, "gap": 6, "visible": "run.ready", "children": [
					{ "type": "label", "text": "CANNON RUN", "style": "big", "align": "center" },
					{ "type": "label", "text": "the clock starts at the line: get round, fast", "align": "center" },
					{ "type": "label", "text": "the cannons hiss and blink before they fire -- listen, look, dodge", "style": "dim", "align": "center" },
					{ "type": "label", "text": "best  {run.best}", "style": "accent", "align": "center", "visible": "run.hasbest" } ] },
				{ "type": "panel", "anchor": "center", "layout": "vertical", "padding": 26, "gap": 6, "visible": "run.finished", "children": [
					{ "type": "label", "text": "FINISHED", "style": "title", "align": "center" },
					{ "type": "label", "text": "{run.time}", "fontSize": 76, "align": "center" },
					{ "type": "label", "text": "{run.verdict}", "style": "good", "align": "center" },
					{ "type": "label", "text": "hit {run.hits} times, down {run.downs}, {run.dodges} dodges", "align": "center" },
					{ "type": "label", "text": "{run.wrecked} cannons wrecked, {run.returns} balls returned", "align": "center", "visible": "run.armed" },
					{ "type": "label", "text": "{keys.restart}  run it again          {keys.next}  a new course", "style": "dim", "align": "center" } ] },
				{ "type": "panel", "anchor": "center", "layout": "vertical", "padding": 26, "gap": 6, "visible": "run.paused", "children": [
					{ "type": "label", "text": "PAUSED", "style": "title", "align": "center" },
					{ "type": "label", "text": "{keys.cancel} to go on     {keys.restart} to start again", "style": "dim", "align": "center" } ] }
			] } })json";
	}
}

class CannonRun : public DemoLayer
{
public:
	CannonRun() : DemoLayer("CannonRun") {}

	void OnDemoAttach() override { m_Renderer.Init(); }

	void OnDemoActivated() override { Build(m_Seed); }

	void OnDemoDeactivated() override
	{
		Capture(false);
		Teardown();
	}

	void OnDemoFixedUpdate(GS::Timestep step) override
	{
		if (!m_Session)
			return;
		// Restart, a new course, pause: on the press, read as the step reads
		// everything else, so a recording replays them.
		const GS::InputMap& map = GS::InputMap::Active();
		bool keys = !GS::ImGuiLayer::WantsKeyboard() && !m_Session->Console().IsOpen();
		bool restart = keys && map.Down("restart"), next = keys && map.Down("next"), pause = keys && map.Down("cancel");
		bool restartEdge = restart && !m_RestartWas, nextEdge = next && !m_NextWas, pauseEdge = pause && !m_PauseWas;
		m_RestartWas = restart;
		m_NextWas = next;
		m_PauseWas = pause;
		if (restartEdge)
		{
			Build(m_Seed);
			return;
		}
		if (nextEdge && m_Phase != Phase::Running)
		{
			Build(NextSeed());
			return;
		}
		if (pauseEdge && m_Phase != Phase::Finished)
			m_Paused = !m_Paused;
		if (m_Paused)
		{
			Publish();
			return;
		}

		m_Session->FixedUpdate(step);
		Humanoid* rig = m_Session->Rig("Runner");
		m_Guns.Step(step, *m_Session, *m_Scene, rig, m_Phase == Phase::Running);

		// What the camera turns to while Focus is held, and which way an
		// unaimed dodge goes.
		glm::vec3 focus(0.0f);
		bool has = m_Guns.Focus(focus);
		m_Session->Characters().SetFocusTarget(focus, has);
		m_Session->Characters().SetDodgeAssist(m_Guns.Evade());

		if (rig)
			RunStep(step, *rig);
		Publish();
	}

	void OnDemoUpdate(GS::Timestep) override
	{
		if (!m_Session)
			return;
		Capture(m_Session->Modes().WantsCursorCaptured() && !m_Paused && m_Phase != Phase::Finished);

		ViewportRect view = DemoViewport();
		GS::RenderCommand::SetViewport((unsigned int)view.X, (unsigned int)view.Y, (unsigned int)view.Width, (unsigned int)view.Height);
		GS::RenderCommand::SetClearColor({ 0.62f, 0.74f, 0.88f, 1.0f });
		GS::RenderCommand::Clear();
		float aspect = (float)view.Width / (float)std::max(view.Height, 1);
		GS::PerspectiveCamera* camera = &m_Camera;
		if (!SceneRenderer::FindActiveCamera(*m_Scene, aspect, m_Camera))
		{
			m_Fallback.SetProjection(50.0f, aspect, 0.1f, 500.0f);
			camera = &m_Fallback;
		}
		Listen(*camera);
		GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		std::vector<ExtraDraw> extra;
		AppendHumanoidDraws(*m_Scene, m_Session.get(), extra);
		m_Guns.AppendDraws(extra);
		m_Renderer.Render(*m_Scene, *camera, &extra);
		GS::RenderCommand::SetCullFace(GS::CullFace::None);
	}

	void OnDemoGameOverlay() override
	{
		if (!m_Session)
			return;
		ViewportRect view = DemoViewport();
		float displayHeight = ImGui::GetIO().DisplaySize.y;
		float x = (float)view.X, y = displayHeight - (float)(view.Y + view.Height), w = (float)view.Width, h = (float)view.Height;
		m_Session->DrawOverlay(x, y, w, h);
		Markers(x, y, w, h);
	}

private:
	enum class Phase { Ready, Running, Finished };

	uint32_t NextSeed()
	{
		m_SeedCounter++;
		return 1000u + (m_SeedCounter * 2654435761u >> 16) % 9000u;
	}

	void Teardown()
	{
		m_Guns.Clear(m_Scene.get());
		m_Guns.Attach(nullptr);
		m_Session.reset();
		m_Scene.reset();
	}

	// The course for `seed` (0 the hand-made one), the runner on the start,
	// the clock at zero. Rebuilt whole every time -- crates knocked about
	// and balls lying in the way are a new run's business to clear.
	void Build(uint32_t seed)
	{
		Teardown();
		m_Seed = seed;
		m_Layout = seed == 0 ? RunCourse::Gauntlet() : RunCourse::Generate(seed);
		m_Scene = std::make_unique<GS::Scene>();
		m_Built = RunCourse::Build(*m_Scene, m_Guns, m_Layout);

		// The runner: the crash test's dummy, in running blue, as the
		// game's own player (no taking over, no letting go).
		GS::Entity runner = CrashTestScene::Mesh(*m_Scene, "Runner", "primitive:cube", m_Built.Spawn,
			glm::vec3(0.0f, glm::degrees(m_Built.SpawnYaw), 0.0f), glm::vec3(1.0f), glm::vec4(0.16f, 0.46f, 0.95f, 1.0f));
		GS::PlayerComponent player;
		player.Body = 2;
		player.Look = 1;
		player.CameraDistance = 4.9f;
		player.CameraHeight = 0.75f;
		runner.Add<GS::PlayerComponent>(player);
		glm::vec3 F(std::sin(m_Built.SpawnYaw), 0.0f, std::cos(m_Built.SpawnYaw));
		CrashTestScene::Camera(*m_Scene, "Run Camera", m_Built.Spawn - F * 4.0f + glm::vec3(0.0f, 2.2f, 0.0f), m_Built.Spawn + F * 4.0f + glm::vec3(0.0f, 1.0f, 0.0f), true);

		m_Session = std::make_unique<GameSession>(*m_Scene, PlayMode::s_ScriptEngine);
		m_Session->Start();
		m_Guns.Attach(m_Session.get());
		m_Session->Modes().SetMouseLook(true);
		GameUI::Context& ui = m_Session->UI();
		ui.RegisterBuiltin("run", CannonRunScreens::Hud());
		ui.RegisterBuiltin("run-gate-start", RunCourse::GateScreen("START", "#8dffb0"));
		ui.RegisterBuiltin("run-gate-finish", RunCourse::GateScreen("FINISH", "#ffffff"));
		ui.Show("run");
		ui.Show("hud", false);
		// The sword and shield, unless put away.
		if (Humanoid* rig = m_Session->Rig("Runner"))
			rig->SetArmed(m_Armed);
		m_Session->Console().Register("sword", "[on|off]", "carry the sword and shield, or put them away",
			[this](const GameConsole::Args& a)
			{
				m_Armed = a.empty() ? !m_Armed : a[0] != "off";
				if (Humanoid* rig = m_Session->Rig("Runner"))
					rig->SetArmed(m_Armed);
				m_Session->Console().Print(m_Armed ? "sword and shield: carried" : "sword and shield: put away");
			});
		m_Session->Console().Register("course", "[seed]", "run a course: a number makes one from that seed; none, the one made by hand",
			[this](const GameConsole::Args& a)
			{
				float n = 0.0f;
				if (!a.empty() && !m_Session->Console().Number(a[0], n))
					return;
				m_PendingSeed = (int)std::max(n, 0.0f);
			});

		m_Phase = Phase::Ready;
		m_Paused = false;
		m_Time = 0.0f;
		m_NextGate = 0;
		m_Splits.clear();
		m_Downs = 0;
		m_WasDown = false;
		m_SplitShown = -1.0f;
		m_SplitText.clear();
		m_HaveLast = false;
		m_DodgesAtStart = 0;
	}

	// The clock, the gates, the falls.
	void RunStep(float dt, Humanoid& rig)
	{
		if (m_PendingSeed >= 0)
		{
			uint32_t seed = (uint32_t)m_PendingSeed;
			m_PendingSeed = -1;
			Build(seed);
			return;
		}
		const GS::RigidBody3D& pelvis = m_Session->World().GetBody(rig.PelvisBody());
		glm::vec3 p = pelvis.Position;
		if (m_Phase == Phase::Running)
			m_Time += dt;
		if (rig.IsDown() && !m_WasDown && m_Phase == Phase::Running)
			m_Downs++;
		m_WasDown = rig.IsDown();
		if (m_SplitShown >= 0.0f)
			m_SplitShown += dt;

		// The next gate, or one of the two after it: a dive or a cut can
		// carry him a few metres off the track, past a gate's end, and a
		// gate missed that way used to leave every later one unpassable --
		// the clock ran on for ever (an autopilot's run of course 1001,
		// pushed 4 m wide in a cover gauntlet). The gates are 4 m wider
		// than the track, each side, and a later one counts those before it.
		if (m_HaveLast && m_Phase != Phase::Finished)
		{
			int last = std::min(m_NextGate + (m_Phase == Phase::Running ? 2 : 0), (int)m_Built.Gates.size() - 1);
			for (int k = m_NextGate; k <= last; k++)
			{
				const RunCourse::Gate& g = m_Built.Gates[(size_t)k];
				float before = glm::dot(m_Last - g.At, g.Forward), after = glm::dot(p - g.At, g.Forward);
				glm::vec3 across = glm::cross(g.Forward, glm::vec3(0.0f, 1.0f, 0.0f));
				if (before < 0.0f && after >= 0.0f && std::fabs(glm::dot(p - g.At, across)) <= g.HalfWidth + 4.0f)
				{
					while (m_NextGate <= k)
						Pass(rig);
					break;
				}
			}
		}
		m_Last = p;
		m_HaveLast = true;
	}

	void Pass(Humanoid& rig)
	{
		int gate = m_NextGate++;
		int last = (int)m_Built.Gates.size() - 1;
		if (gate == 0)
		{
			m_Phase = Phase::Running;
			m_Time = 0.0f;
			m_DodgesAtStart = rig.DodgeCount();
			return;
		}
		m_Splits.push_back(m_Time);
		// Against the best run of this course, at the same gate.
		auto best = m_BestSplits.find(m_Seed);
		if (best != m_BestSplits.end() && m_Splits.size() <= best->second.size())
		{
			float d = m_Time - best->second[m_Splits.size() - 1];
			char text[32];
			std::snprintf(text, sizeof text, "%s%.2f", d < 0.0f ? "-" : "+", std::fabs(d));
			m_SplitText = text;
			m_SplitShown = 0.0f;
		}
		if (gate == last)
		{
			m_Phase = Phase::Finished;
			auto it = m_Best.find(m_Seed);
			m_NewBest = it == m_Best.end() || m_Time < it->second;
			if (m_NewBest)
			{
				m_Best[m_Seed] = m_Time;
				m_BestSplits[m_Seed] = m_Splits;
			}
		}
	}

	static std::string Clock(float seconds)
	{
		int minutes = (int)(seconds / 60.0f);
		float rest = seconds - 60.0f * (float)minutes;
		char text[32];
		std::snprintf(text, sizeof text, "%d:%05.2f", minutes, rest);
		return text;
	}

	void Publish()
	{
		GameUI::Context& ui = m_Session->UI();
		Humanoid* rig = m_Session->Rig("Runner");
		ui.Set("run.time", Clock(m_Time));
		ui.Set("run.clock", m_Phase == Phase::Ready ? "0" : "1");
		int gates = std::max((int)m_Built.Gates.size() - 1, 1);
		ui.Set("run.progress", m_Phase == Phase::Ready ? std::string()
			: std::to_string(std::max(m_NextGate - 1, 0)) + " / " + std::to_string(gates));
		ui.Set("run.split", m_SplitShown >= 0.0f && m_SplitShown < 3.0f ? m_SplitText : std::string());
		ui.Set("run.hits", std::to_string(m_Guns.Hits()));
		ui.Set("run.downs", std::to_string(m_Downs));
		ui.Set("run.dodges", std::to_string(rig ? rig->DodgeCount() - (m_Phase == Phase::Ready ? rig->DodgeCount() : m_DodgesAtStart) : 0));
		ui.Set("run.course", m_Seed == 0 ? "the gauntlet" : "course " + std::to_string(m_Seed));
		bool armed = rig && rig->Armed();
		ui.Set("run.armed", armed ? "1" : "0");
		ui.Set("run.unarmed", armed ? "0" : "1");
		ui.Set("run.blocks", std::to_string(m_Guns.Blocks()));
		ui.Set("run.parries", std::to_string(m_Guns.Parries()));
		ui.Set("run.returns", std::to_string(m_Guns.Returns()));
		ui.Set("run.wrecked", std::to_string(m_Guns.Wrecked()) + " / " + std::to_string(m_Guns.CannonCount()));
		ui.Set("run.ready", m_Phase == Phase::Ready && !m_Paused ? "1" : "0");
		ui.Set("run.finished", m_Phase == Phase::Finished ? "1" : "0");
		ui.Set("run.paused", m_Paused ? "1" : "0");
		auto best = m_Best.find(m_Seed);
		ui.Set("run.hasbest", best != m_Best.end() ? "1" : "0");
		ui.Set("run.best", best != m_Best.end() ? Clock(best->second) : std::string());
		ui.Set("run.verdict", m_NewBest ? "a new best" : best != m_Best.end() ? "best " + Clock(best->second) : std::string());
		// The move he is in, as a word: VAULT, DIVE.
		std::string move = rig ? rig->MoveName() : "";
		for (char& ch : move)
			ch = (char)std::toupper((unsigned char)ch);
		ui.Set("run.move", move);
	}

	// Where the ears are: the camera, moving as it moves.
	void Listen(const GS::PerspectiveCamera& camera)
	{
		GS::AudioListener listener;
		listener.Position = camera.GetPosition();
		listener.Forward = camera.GetForward();
		listener.Up = camera.GetUp();
		float dt = ImGui::GetIO().DeltaTime;
		listener.Velocity = m_HaveListener && dt > 0.0f ? (camera.GetPosition() - m_LastListener) / dt : glm::vec3(0.0f);
		m_LastListener = camera.GetPosition();
		m_HaveListener = true;
		GS::AudioEngine::SetListener(listener);
	}

	// The pointer is the camera's while he is being run, and the desktop's
	// otherwise -- paused, finished, the terminal open, another demo.
	void Capture(bool on)
	{
		if (on == m_Captured)
			return;
		m_Captured = on;
		GS::Application::Get().GetWindow().SetCursorCaptured(on);
	}

	// A mark on what is coming: a ring round a charging cannon or a ball in
	// sight, an arrow at the edge of the picture for one out of it -- orange
	// while it charges, red once it is in the air, bigger the sooner.
	void Markers(float x, float y, float w, float h)
	{
		if (m_Phase != Phase::Running || m_Paused)
			return;
		GS::PerspectiveCamera camera(50.0f, w / std::max(h, 1.0f), 0.1f, 500.0f);
		if (!SceneRenderer::FindActiveCamera(*m_Scene, w / std::max(h, 1.0f), camera))
			return;
		ImDrawList* list = ImGui::GetForegroundDrawList();
		list->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
		float scale = std::max(0.6f, h / 1080.0f);
		glm::mat4 vp = camera.GetViewProjectionMatrix();
		for (const Gunnery::Threat& t : m_Guns.Threats())
		{
			float urgency = glm::clamp(1.0f - t.Eta / 1.5f, 0.15f, 1.0f);
			ImU32 colour = t.Ball ? IM_COL32(255, 70, 50, (int)(140 + 115 * urgency)) : IM_COL32(255, 170, 40, (int)(120 + 120 * urgency));
			glm::vec4 clip = vp * glm::vec4(t.Position, 1.0f);
			bool behind = clip.w <= 0.05f;
			glm::vec2 ndc = glm::vec2(clip) / std::max(std::fabs(clip.w), 1e-3f);
			if (behind)
				ndc = -ndc;
			bool inside = !behind && std::fabs(ndc.x) < 0.95f && std::fabs(ndc.y) < 0.92f;
			if (inside)
			{
				// A ring that tightens on it as it comes: wide when there is
				// time, close when there is not.
				ImVec2 at(x + (ndc.x * 0.5f + 0.5f) * w, y + (0.5f - ndc.y * 0.5f) * h);
				float r = (t.Ball ? 22.0f : 30.0f) * scale * (2.2f - 1.3f * urgency);
				list->AddCircle(at, r, IM_COL32(0, 0, 0, 90), 40, 6.0f * scale);
				list->AddCircle(at, r, colour, 40, 3.5f * scale);
				continue;
			}
			// Out of sight: an arrow on an ellipse inside the edge, pointing out.
			glm::vec2 d = glm::length(ndc) > 1e-4f ? glm::normalize(ndc) : glm::vec2(0.0f, -1.0f);
			ImVec2 at(x + (0.5f + 0.44f * d.x) * w, y + (0.5f - 0.42f * d.y) * h);
			glm::vec2 out(d.x, -d.y), side(-out.y, out.x);
			float size = 22.0f * scale * (0.7f + 0.7f * urgency);
			ImVec2 tip(at.x + out.x * size, at.y + out.y * size);
			ImVec2 a(at.x - out.x * size * 0.5f + side.x * size * 0.7f, at.y - out.y * size * 0.5f + side.y * size * 0.7f);
			ImVec2 b(at.x - out.x * size * 0.5f - side.x * size * 0.7f, at.y - out.y * size * 0.5f - side.y * size * 0.7f);
			list->AddTriangleFilled(tip, a, b, colour);
		}
		list->PopClipRect();
	}

	ViewportRect DemoViewport() const
	{
		if (g_Viewport.Valid())
			return g_Viewport;
		GS::Window& window = GS::Application::Get().GetWindow();
		return { 0, 0, (int)window.GetWidth(), (int)window.GetHeight() };
	}

	std::unique_ptr<GS::Scene> m_Scene;
	std::unique_ptr<GameSession> m_Session;
	Gunnery m_Guns;
	RunCourse::Layout m_Layout;
	RunCourse::Built m_Built;
	SceneRenderer m_Renderer;
	GS::PerspectiveCamera m_Camera{ 50.0f, 16.0f / 9.0f, 0.1f, 500.0f };
	GS::PerspectiveCamera m_Fallback{ 50.0f, 16.0f / 9.0f, 0.1f, 500.0f };

	uint32_t m_Seed = 0, m_SeedCounter = 0;
	int m_PendingSeed = -1;
	Phase m_Phase = Phase::Ready;
	bool m_Paused = false;
	float m_Time = 0.0f;
	int m_NextGate = 0;
	std::vector<float> m_Splits;
	std::map<uint32_t, float> m_Best;
	std::map<uint32_t, std::vector<float>> m_BestSplits;
	bool m_NewBest = false;
	int m_Downs = 0;
	bool m_WasDown = false;
	uint32_t m_DodgesAtStart = 0;
	float m_SplitShown = -1.0f;
	std::string m_SplitText;
	glm::vec3 m_Last{ 0.0f };
	bool m_HaveLast = false;
	bool m_RestartWas = false, m_NextWas = false, m_PauseWas = false;
	bool m_Captured = false;
	bool m_Armed = true;
	glm::vec3 m_LastListener{ 0.0f };
	bool m_HaveListener = false;
};
