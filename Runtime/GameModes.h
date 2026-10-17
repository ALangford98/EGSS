#pragma once

// What the person playing is doing, in a game that lets them do more than
// one thing: flying the camera about, being a character, standing at a
// terminal, or operating a launcher. Each is a mode; walking up to something
// with an InteractableComponent and pressing E is how they move between them,
// and the action E runs is a game-console command -- "possess Dummy",
// "terminal", "operate" -- so the scene file says what pressing E does.
//
//   Fly       a FlyCameraComponent camera: WASD, Space/Ctrl, Shift, hold
//             the right button to look. The default when nobody is a player.
//   Possess   a character taken over (CharacterController::Possess): it
//             walks as a player does and the camera follows it. E lets go.
//   Terminal  the game console, shown on a screen in the world (a
//             ScreenComponent of "@terminal"); the camera moves to face it
//             and the keyboard is the terminal's. Esc steps back.
//   Cannon    the launcher's controls: aim with the mouse -- the barrel
//             turns to land a shot where the cursor points -- see the
//             trajectory, pick an object and set it up on the HUD, click to
//             fire. E or Esc steps away.
//
// Step() runs on the fixed step and reads GS::Input, so modes, flying and
// walking record and replay like the rest of the game. Frame() is per
// frame, inside the host's ImGui frame: it draws the HUD and the screens in
// the world, and turns the cursor into an aim point -- which, like every
// change the UI makes, goes to the game as a command on the next step.

#include <GS.h>
#include <imgui.h>

#include "CharacterController.h"
#include "GameConsole.h"
#include "GameUI.h"
#include "Launcher.h"
#include "SceneRenderer.h"

#include <map>
#include <string>
#include <vector>

class GameModes
{
public:
	enum class Mode { Fly, Possess, Terminal, Cannon };

	GameModes(GS::Scene& scene, GS::PhysicsWorld3D& world, CharacterController& characters, LauncherSystem& launchers,
		GameConsole& console)
		: m_Scene(scene), m_World(world), m_Characters(characters), m_Launchers(launchers), m_Console(console) {}

	GameUI::Context& UI() { return m_UI; }

	// The screens built in, for a project without its own ui/hud.gsui and
	// ui/cannon.gsui -- and the text a project's copy starts from.
	static const char* HudScreen()
	{
		return R"json({
			"root": { "type": "panel", "style": "clear", "stretch": "both", "padding": 0, "children": [
				{ "type": "panel", "anchor": "top-left", "pos": [28, 28], "layout": "vertical", "style": "accent",
				  "padding": 14, "gap": 2, "children": [
					{ "type": "label", "text": "{hud.mode}", "style": "title" },
					{ "type": "label", "text": "walk up to things and press {keys.interact}", "style": "dim" } ] },
				{ "type": "panel", "anchor": "bottom", "pos": [0, 26], "padding": 12, "children": [
					{ "type": "label", "text": "{hud.hints}", "style": "dim" } ] }
			] } })json";
	}
	static const char* CannonScreen()
	{
		return R"json({
			"theme": { "accent": "#ff8a1c" },
			"root": { "type": "panel", "style": "clear", "stretch": "both", "padding": 0, "children": [
				{ "type": "crosshair", "bind": "aim", "stretch": "both" },
				{ "type": "panel", "anchor": "left", "pos": [28, 0], "size": [372, 0], "layout": "vertical",
				  "style": "accent", "padding": 18, "gap": 10, "children": [
					{ "type": "label", "text": "LAUNCHER", "style": "title" },
					{ "type": "label", "text": "{launcher.about}", "style": "dim" },
					{ "type": "list", "items": "presets", "bind": "launcher.preset", "command": "preset", "rows": 8, "style": "wheel" },
					{ "type": "slider", "text": "Size  m", "bind": "launcher.size", "min": 0.03, "max": 4, "log": true,
					  "command": "resize", "format": "%.2f" },
					{ "type": "slider", "text": "Mass  kg", "bind": "launcher.mass", "min": 0.05, "max": 2000, "log": true,
					  "command": "mass", "format": "%.3g" },
					{ "type": "slider", "text": "Speed  m/s", "bind": "launcher.speed", "min": 1, "max": 60,
					  "command": "speed", "format": "%.1f" },
					{ "type": "slider", "text": "Spin  rad/s", "bind": "launcher.spin", "min": -60, "max": 60,
					  "command": "spin", "format": "%.0f" },
					{ "type": "panel", "style": "clear", "layout": "horizontal", "padding": 0, "gap": 8, "children": [
						{ "type": "button", "text": "BALL", "command": "shape ball" },
						{ "type": "button", "text": "BOX", "command": "shape box" },
						{ "type": "button", "text": "ARC", "bind": "launcher.arc", "command": "arc toggle" } ] },
					{ "type": "panel", "style": "clear", "layout": "horizontal", "padding": 0, "gap": 8, "children": [
						{ "type": "button", "text": "  FIRE  ", "style": "accent", "command": "fire" },
						{ "type": "button", "text": "AUTO", "bind": "launcher.auto", "command": "auto toggle" },
						{ "type": "button", "text": "CLEAR", "command": "sweep" },
						{ "type": "button", "text": "RESET", "command": "reset" } ] }
				] },
				{ "type": "panel", "anchor": "bottom-right", "pos": [28, 28], "size": [360, 0], "layout": "vertical",
				  "padding": 18, "gap": 6, "children": [
					{ "type": "label", "text": "IMPACT", "style": "title" },
					{ "type": "label", "text": "{launcher.dims}" },
					{ "type": "label", "text": "{launcher.mass} kg at {launcher.speed} m/s  ({launcher.kmh} km/h)" },
					{ "type": "label", "text": "momentum  {launcher.momentum} N s", "style": "accent" },
					{ "type": "label", "text": "energy  {launcher.energy}", "style": "accent" },
					{ "type": "label", "text": "range {aim.range} m   flight {aim.time} s", "style": "dim" },
					{ "type": "label", "text": "too slow to reach -- more speed", "style": "accent", "visible": "aim.warn" } ] },
				{ "type": "panel", "anchor": "bottom", "pos": [0, 26], "padding": 12, "children": [
					{ "type": "label", "text": "{keys.shoot} fires   wheel picks an object   sliders set it   {keys.interact} or {keys.cancel} steps away", "style": "dim" } ] }
			] } })json";
	}
	Mode CurrentMode() const { return m_Mode; }

	// The mouse turns the camera with no button held while a character is
	// being played -- a game, rather than a scene being looked round --
	// and the host captures the pointer for it (WantsCursorCaptured).
	// Off by default: the editor's Play leaves the pointer free.
	void SetMouseLook(bool on) { m_MouseLookWanted = on; }
	// Being a character: one taken over, or the game's own player.
	bool Playing() const
	{
		return m_Mode == Mode::Possess
			|| (m_Mode == Mode::Fly && m_FlyCamera == GS::InvalidEntity && m_Characters.Controlled() != GS::InvalidEntity);
	}
	bool WantsCursorCaptured() const { return m_MouseLookWanted && Playing() && !m_Console.IsOpen(); }

	// The keyboard is the game's, not the terminal's.
	bool KeyboardFree() const { return m_Mode != Mode::Terminal; }

	void Start()
	{
		m_Mode = Mode::Fly;
		m_FlyCamera = GS::InvalidEntity;
		m_Terminal = GS::InvalidEntity;
		m_Focus = GS::InvalidEntity;
		m_Easing = false;
		m_MouseKnown = false;
		m_Path.clear();

		// A fly camera only when nobody is a player: a game with a player
		// already has its camera.
		if (m_Characters.Controlled() == GS::InvalidEntity)
		{
			auto& flies = m_Scene.View<GS::FlyCameraComponent>();
			for (size_t i = 0; i < flies.Size(); i++)
			{
				GS::EntityId id = flies.Owner(i);
				if (m_Scene.HasComponent<GS::CameraComponent>(id) && m_Scene.HasComponent<GS::TransformComponent>(id))
				{
					m_FlyCamera = id;
					break;
				}
			}
		}
		if (auto* t = Transform(m_FlyCamera))
		{
			TakeLook(*t);
		}

		auto& screens = m_Scene.View<GS::ScreenComponent>();
		for (size_t i = 0; i < screens.Size(); i++)
			if (screens.Components()[i].Screen == "@terminal")
				m_Terminal = screens.Owner(i);
		m_Console.SetEmbedded(m_Terminal != GS::InvalidEntity);

		RegisterScreens();
		m_UI.SetSubmit([this](const std::string& line) { m_Console.Submit(line, false); });
		m_Launchers.AttachUI(&m_UI);
		if (m_FlyCamera != GS::InvalidEntity)
			m_UI.Show("hud");
		RegisterCommands();
	}

	void Stop()
	{
		// The textures die with this; the scene must not keep pointing at
		// them (an editor keeping Play's changes keeps the components).
		auto& screens = m_Scene.View<GS::ScreenComponent>();
		for (size_t i = 0; i < screens.Size(); i++)
			screens.Components()[i].Texture.reset();
		m_Textures.clear();
		m_Console.SetEmbedded(false);
	}

	// ---- the fixed step ---------------------------------------------------

	void Step(float dt)
	{
		bool keys = !GS::ImGuiLayer::WantsKeyboard();
		const GS::InputMap& map = GS::InputMap::Active();
		bool e = keys && map.Down("interact");
		bool esc = keys && map.Down("cancel");
		bool click = map.Down("shoot") && !m_UIWantsPointer && !GS::ImGuiLayer::WantsMouse();
		bool eEdge = e && !m_EWas, escEdge = esc && !m_EscWas, clickEdge = click && !m_ClickWas;
		m_EWas = e;
		m_EscWas = esc;
		m_ClickWas = click;

		// The terminal opened or closed by its own key (` or Esc) is the
		// same as walking up to it or away.
		if (m_Console.IsEmbedded())
		{
			if (m_Console.IsOpen() && m_Mode != Mode::Terminal)
				EnterTerminal();
			else if (!m_Console.IsOpen() && m_Mode == Mode::Terminal)
				ExitTerminal();
		}

		switch (m_Mode)
		{
			case Mode::Fly:
				FlyStep(dt, keys);
				break;
			case Mode::Possess:
				if (eEdge || escEdge || !m_Scene.IsValid(m_Characters.Controlled()))
				{
					Release();
					eEdge = false;
				}
				break;
			case Mode::Cannon:
				if (eEdge || escEdge)
				{
					Leave();
					eEdge = false;
					break;
				}
				if (clickEdge)
					m_Launchers.Fire();
				Ease(dt);
				break;
			case Mode::Terminal:
				Ease(dt);
				break;
		}

		if (m_Mode == Mode::Fly || m_Mode == Mode::Possess)
		{
			UpdateFocus();
			if (eEdge && m_Focus != GS::InvalidEntity)
				if (auto* interact = m_Scene.GetComponent<GS::InteractableComponent>(m_Focus))
					m_Console.Execute(interact->Action, false);
		}
		else
			m_Focus = GS::InvalidEntity;

		m_Characters.SetInputEnabled(m_Mode == Mode::Fly || m_Mode == Mode::Possess);
		m_Characters.SetMouseLook(WantsCursorCaptured());
		PublishHud();
	}

	// ---- the frame --------------------------------------------------------

	void Frame(float x, float y, float width, float height)
	{
		ImGuiIO& io = ImGui::GetIO();
		m_Time += io.DeltaTime;

		GameUI::Pointer pointer;
		bool inside = io.MousePos.x >= x && io.MousePos.x < x + width && io.MousePos.y >= y && io.MousePos.y < y + height;
		if (inside)
		{
			pointer.Position = io.MousePos;
			pointer.Down = io.MouseDown[0];
			pointer.Clicked = ImGui::IsMouseClicked(0);
			pointer.Released = ImGui::IsMouseReleased(0);
			pointer.Wheel = io.MouseWheel;
		}

		DrawWorldScreens();

		GS::PerspectiveCamera camera(50.0f, width / std::max(height, 1.0f), 0.1f, 500.0f);
		bool haveCamera = SceneRenderer::FindActiveCamera(m_Scene, width / std::max(height, 1.0f), camera);

		if (m_Mode == Mode::Cannon && haveCamera)
			AimFromPointer(camera, pointer, inside, x, y, width, height);

		ImDrawList* list = ImGui::GetForegroundDrawList();
		// Lines anti-aliased as geometry, not sampled from the host's atlas,
		// whose layout differs between the editor and a player: the HUD is
		// then the same pixels in both. (The foreground list's flags are
		// reset every frame.)
		list->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
		list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
		m_UI.Draw(list, ImVec2(x, y), ImVec2(width, height), pointer);
		m_UIWantsPointer = m_UI.WantsPointer();

		// "[E] Take control" over whatever is in reach.
		if (m_Focus != GS::InvalidEntity && haveCamera)
			if (auto* interact = m_Scene.GetComponent<GS::InteractableComponent>(m_Focus))
			{
				glm::vec4 clip = camera.GetViewProjectionMatrix() * glm::vec4(m_FocusPoint + glm::vec3(0.0f, 0.3f, 0.0f), 1.0f);
				if (clip.w > 0.05f)
				{
					glm::vec2 ndc = glm::vec2(clip) / clip.w;
					ImVec2 at(x + (ndc.x * 0.5f + 0.5f) * width, y + (0.5f - ndc.y * 0.5f) * height);
					GameUI::Theme theme;
					GameUI::Prompt(list, at, GS::InputMap::Active().Keys("interact"), interact->Prompt, theme, std::max(0.5f, height / 1080.0f));
				}
			}
		list->PopClipRect();
		GS::ImGuiLayer::UploadTextures();
	}

	// The trajectory, while the cannon is being operated: glowing dots
	// along the path and a ring where it lands.
	void AppendDraws(std::vector<ExtraDraw>& out) const
	{
		if (m_Mode != Mode::Cannon || m_Path.size() < 2)
			return;
		const glm::vec4 orange(1.0f, 0.55f, 0.12f, 1.0f);
		std::shared_ptr<GS::Mesh> sphere = GS::MeshCache::Get("primitive:sphere");
		float carried = 0.0f;
		for (size_t i = 1; i < m_Path.size(); i++)
		{
			glm::vec3 a = m_Path[i - 1], b = m_Path[i];
			float length = glm::length(b - a);
			// A dot every 0.35 m of path, so its spacing shows the speed
			// does not -- the shape is what matters.
			for (float at = 0.35f - carried; at < length; at += 0.35f)
			{
				ExtraDraw dot;
				dot.Mesh = sphere;
				dot.Model = glm::translate(glm::mat4(1.0f), a + (b - a) * (at / length)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.055f));
				dot.Color = orange;
				dot.Emission = 3.0f;
				dot.CastShadow = false;
				out.push_back(dot);
				carried = length - at;
			}
			if (0.35f - carried >= length)
				carried += length;
		}
		if (m_PathHit)
		{
			glm::quat turn = RotationFromUp(m_PathNormal);
			for (int ring = 0; ring < 2; ring++)
			{
				ExtraDraw disc;
				disc.Mesh = GS::MeshCache::Get("primitive:cylinder");
				float d = ring == 0 ? 0.55f : 0.22f;
				disc.Model = glm::translate(glm::mat4(1.0f), m_PathEnd + m_PathNormal * (0.01f + 0.004f * ring)) * glm::mat4_cast(turn)
					* glm::scale(glm::mat4(1.0f), glm::vec3(d, 0.008f, d));
				disc.Color = ring == 0 ? orange : glm::vec4(1.0f, 0.95f, 0.85f, 1.0f);
				disc.Emission = ring == 0 ? 1.6f : 3.0f;
				disc.CastShadow = false;
				out.push_back(disc);
			}
		}
	}

private:
	GS::TransformComponent* Transform(GS::EntityId id)
	{
		return id != GS::InvalidEntity && m_Scene.IsValid(id) ? m_Scene.GetComponent<GS::TransformComponent>(id) : nullptr;
	}

	GS::EntityId Find(const std::string& name)
	{
		for (GS::EntityId id : m_Scene.GetEntities())
			if (auto* tag = m_Scene.GetComponent<GS::TagComponent>(id); tag && GameConsole::Lower(tag->Name) == GameConsole::Lower(name))
				return id;
		return GS::InvalidEntity;
	}

	static glm::quat RotationFromUp(const glm::vec3& normal)
	{
		glm::vec3 up(0.0f, 1.0f, 0.0f);
		glm::vec3 axis = glm::cross(up, normal);
		float s = glm::length(axis);
		if (s < 1e-5f)
			return normal.y > 0.0f ? glm::quat(1, 0, 0, 0) : glm::angleAxis(glm::pi<float>(), glm::vec3(1, 0, 0));
		return glm::angleAxis(std::atan2(s, glm::dot(up, normal)), axis / s);
	}

	// Whichever camera is being looked through: the fly camera, or the
	// scene's active one.
	GS::EntityId ViewCamera()
	{
		if (m_FlyCamera != GS::InvalidEntity)
			return m_FlyCamera;
		auto& cameras = m_Scene.View<GS::CameraComponent>();
		for (size_t i = 0; i < cameras.Size(); i++)
			if (cameras.Components()[i].Active)
				return cameras.Owner(i);
		return GS::InvalidEntity;
	}

	void MakeActive(GS::EntityId camera)
	{
		auto& cameras = m_Scene.View<GS::CameraComponent>();
		for (size_t i = 0; i < cameras.Size(); i++)
			cameras.Components()[i].Active = cameras.Owner(i) == camera;
	}

	void FlyStep(float dt, bool keys)
	{
		GS::TransformComponent* t = Transform(m_FlyCamera);
		if (!t)
			return;
		if (m_Easing)
		{
			// Back from the terminal or the cannon to where the camera was.
			Ease(dt);
			if (!m_Easing)
			{
				TakeLook(*t);
			}
			return;
		}
		auto* fly = m_Scene.GetComponent<GS::FlyCameraComponent>(m_FlyCamera);

		// Look: the pointer's travel since the last step while the right
		// button is held -- per step, not per frame, so it replays.
		glm::vec2 mouse(GS::Input::GetMouseX(), GS::Input::GetMouseY());
		const GS::InputMap& map = GS::InputMap::Active();
		bool look = map.Down("look") && !GS::ImGuiLayer::WantsMouse();
		if (look && m_MouseKnown)
		{
			glm::vec2 delta = mouse - m_LastMouse;
			m_Yaw -= delta.x * fly->LookSpeed;
			m_Pitch = glm::clamp(m_Pitch - delta.y * fly->LookSpeed, -89.0f, 89.0f);
		}
		m_LastMouse = mouse;
		m_MouseKnown = true;

		glm::vec3 move(0.0f);
		float speed = fly->Speed;
		if (keys)
		{
			glm::vec3 forward = LookDirection(m_Pitch, m_Yaw);
			glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
			if (map.Down("forward")) move += forward;
			if (map.Down("back")) move -= forward;
			if (map.Down("right")) move += right;
			if (map.Down("left")) move -= right;
			if (map.Down("jump")) move.y += 1.0f;
			if (map.Down("down")) move.y -= 1.0f;
			if (map.Down("sprint")) speed *= 3.0f;
		}
		if (glm::length(move) > 1e-4f || look)
			MakeActive(m_FlyCamera);   // flying takes the view back from a fixed camera
		if (glm::length(move) > 1e-4f)
			t->Position += glm::normalize(move) * speed * dt;
		t->Rotation = RotationLookingAlong(LookDirection(m_Pitch, m_Yaw));
	}

	// The fly camera's pitch and yaw from where `t` looks: a head's, not
	// the rotation's own angles (SceneRenderer's LookDirection says why).
	void TakeLook(const GS::TransformComponent& t)
	{
		glm::vec2 look = PitchYawOf(ForwardFromRotation(t.Rotation));
		m_Pitch = look.x;
		m_Yaw = look.y;
	}

	// The camera eased towards m_Goal: position and look direction, about
	// 1/7 s to close each e-fold of the gap.
	void Ease(float dt)
	{
		GS::TransformComponent* t = Transform(ViewCamera());
		if (!t || !m_Easing)
			return;
		float k = 1.0f - std::exp(-dt * 7.0f);
		t->Position += (m_GoalPosition - t->Position) * k;
		glm::vec3 now = ForwardFromRotation(t->Rotation);
		glm::vec3 next = glm::normalize(now + (m_GoalDirection - now) * k);
		t->Rotation = RotationLookingAlong(next);
		if (glm::length(m_GoalPosition - t->Position) < 0.002f && glm::dot(next, m_GoalDirection) > 0.99999f)
		{
			t->Position = m_GoalPosition;
			t->Rotation = RotationLookingAlong(m_GoalDirection);
			m_Easing = false;
		}
	}

	void EaseTo(const glm::vec3& position, const glm::vec3& direction)
	{
		m_GoalPosition = position;
		m_GoalDirection = glm::normalize(direction);
		m_Easing = true;
	}

	void SaveReturn()
	{
		if (auto* t = Transform(ViewCamera()))
		{
			m_ReturnPosition = t->Position;
			m_ReturnDirection = ForwardFromRotation(t->Rotation);
		}
	}

	// ---- moving between modes ---------------------------------------------

	bool Possess(const std::string& name)
	{
		GS::EntityId id = Find(name);
		if (id == GS::InvalidEntity)
			return false;
		if (m_Mode == Mode::Cannon) Leave();
		if (m_Mode == Mode::Terminal)
		{
			m_Console.SetOpen(false);
			ExitTerminal();
		}
		GS::EntityId camera = ViewCamera();
		if (!m_Characters.Possess(m_Scene, id, camera, m_World))
			return false;
		MakeActive(camera);
		m_Easing = false;
		m_Mode = Mode::Possess;
		m_UI.Show("hud");
		return true;
	}

	void Release()
	{
		m_Characters.Release();
		m_Mode = Mode::Fly;
		if (auto* t = Transform(m_FlyCamera))
		{
			TakeLook(*t);
		}
		m_MouseKnown = false;
	}

	void EnterTerminal()
	{
		auto* screen = Transform(m_Terminal);
		if (!screen)
			return;
		if (m_Mode == Mode::Possess) Release();
		if (m_Mode == Mode::Cannon) Leave();
		if (!m_Easing)
			SaveReturn();
		MakeActive(ViewCamera());
		// The screen faces its local +z; the camera stands off it, looking in.
		glm::vec3 facing = -ForwardFromRotation(screen->Rotation);
		EaseTo(screen->Position + facing * 0.62f, -facing);
		m_Mode = Mode::Terminal;
		m_UI.Show("hud", false);
	}

	void ExitTerminal()
	{
		m_Mode = Mode::Fly;
		EaseTo(m_ReturnPosition, m_ReturnDirection);
		if (m_FlyCamera != GS::InvalidEntity)
			m_UI.Show("hud");
	}

	bool Operate()
	{
		GS::EntityId launcher = m_Launchers.LauncherEntity();
		auto* t = Transform(launcher);
		auto* s = launcher != GS::InvalidEntity ? m_Scene.GetComponent<GS::LauncherComponent>(launcher) : nullptr;
		if (!t || !s)
			return false;
		if (m_Mode == Mode::Possess) Release();
		if (m_Mode == Mode::Terminal) { m_Console.SetOpen(false); ExitTerminal(); }
		if (!m_Easing)
			SaveReturn();
		MakeActive(ViewCamera());
		// The operator's place: behind the breech and above it, looking down
		// the way it points when it was walked up to -- and staying there, so
		// the cursor's aim point does not move under it as the barrel turns.
		float yaw = glm::radians(s->AimYaw);
		glm::vec3 forward(std::sin(yaw), 0.0f, std::cos(yaw));
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
		glm::vec3 at = t->Position - forward * 3.4f + glm::vec3(0.0f, 1.15f, 0.0f) + right * 0.35f;
		glm::vec3 look = t->Position + forward * 10.0f - glm::vec3(0.0f, 0.55f, 0.0f);
		EaseTo(at, look - at);
		s->Manual = true;
		m_Mode = Mode::Cannon;
		m_UI.Show("hud", false);
		m_UI.Show("cannon");
		return true;
	}

	void Leave()
	{
		m_Mode = Mode::Fly;
		m_UI.Show("cannon", false);
		m_Path.clear();
		EaseTo(m_ReturnPosition, m_ReturnDirection);
		if (m_FlyCamera != GS::InvalidEntity)
			m_UI.Show("hud");
	}

	// What is in reach and looked at: the nearest interactable within its
	// range and 30 degrees of the view -- 55 of a walking character's way,
	// whose camera is behind it.
	void UpdateFocus()
	{
		m_Focus = GS::InvalidEntity;
		GS::TransformComponent* view = Transform(ViewCamera());
		if (!view)
			return;
		glm::vec3 from = view->Position;
		glm::vec3 forward = ForwardFromRotation(view->Rotation);
		float cone = std::cos(glm::radians(30.0f));
		GS::EntityId controlled = m_Characters.Controlled();
		if (m_Mode == Mode::Possess && m_Characters.ControlledPosition(m_World, from))
		{
			forward.y = 0.0f;
			forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
			cone = std::cos(glm::radians(55.0f));
		}
		float best = 1e9f;
		auto& interactables = m_Scene.View<GS::InteractableComponent>();
		for (size_t i = 0; i < interactables.Size(); i++)
		{
			GS::EntityId id = interactables.Owner(i);
			auto* t = Transform(id);
			if (!t || id == controlled)
				continue;
			const GS::InteractableComponent& interact = interactables.Components()[i];
			// A character is reached at its chest, not its feet.
			bool character = m_Scene.HasComponent<GS::NpcComponent>(id) || m_Scene.HasComponent<GS::PlayerComponent>(id);
			glm::vec3 point = t->Position + (character ? glm::vec3(0.0f, 1.1f, 0.0f) : glm::vec3(0.0f));
			glm::vec3 to = point - from;
			if (m_Mode == Mode::Possess)
				to.y = 0.0f;
			float distance = glm::length(to);
			if (distance > interact.Range || distance < 1e-4f)
				continue;
			if (glm::dot(to / distance, forward) < cone && distance > 0.8f)
				continue;
			if (distance < best)
			{
				best = distance;
				m_Focus = id;
				m_FocusPoint = point;
			}
		}
	}

	// ---- the cannon's aim -------------------------------------------------

	void AimFromPointer(GS::PerspectiveCamera& camera, const GameUI::Pointer& pointer, bool inside,
		float x, float y, float width, float height)
	{
		if (inside && !m_UIWantsPointer)
		{
			// The cursor's ray, from the inverse of the camera's view and
			// projection, and where it first meets the world.
			glm::vec2 ndc((pointer.Position.x - x) / width * 2.0f - 1.0f, 1.0f - (pointer.Position.y - y) / height * 2.0f);
			glm::mat4 inverse = glm::inverse(camera.GetViewProjectionMatrix());
			glm::vec4 nearPoint = inverse * glm::vec4(ndc, -1.0f, 1.0f);
			glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
			glm::vec3 a = glm::vec3(nearPoint) / nearPoint.w, b = glm::vec3(farPoint) / farPoint.w;
			glm::vec3 direction = glm::normalize(b - a);
			GS::PhysicsWorld3D::RayHit hit = m_World.Raycast(a, direction, 200.0f);
			glm::vec3 point;
			bool found = hit.Hit;
			if (found)
				point = hit.Point;
			else if (direction.y < -1e-3f)
			{
				point = a + direction * (-a.y / direction.y);
				found = true;
			}
			if (found && (!m_HaveAim || glm::length(point - m_AimPoint) > 0.01f))
			{
				m_AimPoint = point;
				m_HaveAim = true;
				char line[96];
				std::snprintf(line, sizeof line, "aimat %.3f %.3f %.3f", point.x, point.y, point.z);
				m_Console.Submit(line, false);
			}
			m_UI.Set("aim.x", (pointer.Position.x - x) / width);
			m_UI.Set("aim.y", (pointer.Position.y - y) / height);
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		}
		else
			m_UI.Set("aim.x", -1.0f);

		float time = 0.0f;
		m_Path = m_Launchers.Trajectory(m_PathHit, m_PathEnd, m_PathNormal, time);
		m_UI.Set("aim.warn", m_Launchers.OutOfRange() ? "1" : "0");
		m_UI.Set("aim.hit", m_PathHit ? "1" : "0");
		if (auto* t = Transform(m_Launchers.LauncherEntity()))
		{
			glm::vec3 d = (m_PathHit ? m_PathEnd : m_AimPoint) - t->Position;
			char buffer[32];
			std::snprintf(buffer, sizeof buffer, "%.1f", glm::length(glm::vec2(d.x, d.z)));
			m_UI.Set("aim.range", buffer);
			std::snprintf(buffer, sizeof buffer, "%.2f", time);
			m_UI.Set("aim.time", buffer);
		}
	}

	// ---- screens ----------------------------------------------------------

	void DrawWorldScreens()
	{
		auto& screens = m_Scene.View<GS::ScreenComponent>();
		for (size_t i = 0; i < screens.Size(); i++)
		{
			GS::ScreenComponent& screen = screens.Components()[i];
			GS::EntityId id = screens.Owner(i);
			int w = std::clamp(screen.Width, 16, 4096), h = std::clamp(screen.Height, 16, 4096);
			GameUI::Texture& texture = m_Textures[id];
			ImDrawList* list = texture.Begin(w, h);
			glm::vec4 clear(0.0f, 0.0f, 0.0f, 1.0f);
			if (screen.Screen == "@terminal")
				m_Console.DrawTerminal(list, (float)w, (float)h, m_Mode == Mode::Terminal, m_Time, "GS CRASH LAB  //  LAUNCH CONTROL");
			else if (GameUI::Screen* ui = m_UI.Find(screen.Screen))
			{
				clear = glm::vec4(0.02f, 0.03f, 0.04f, 1.0f);
				m_UI.DrawScreen(*ui, list, ImVec2(0, 0), ImVec2((float)w, (float)h), GameUI::Pointer{});
			}
			texture.End(clear);
			screen.Texture = texture.Get();
		}
	}

	// The hints, and every action's key as `keys.<action>` for screens to
	// show ("{keys.interact} to use"), from the project's Controls: rebind
	// Interact to F and every prompt says F.
	void PublishHud()
	{
		const GS::InputMap& map = GS::InputMap::Active();
		for (const GS::InputMap::Action& action : map.Actions())
			m_UI.Set("keys." + action.Id, map.Keys(action.Id));
		std::string move = map.Keys("forward") + map.Keys("left") + map.Keys("back") + map.Keys("right");
		std::string mode = "FREE CAMERA";
		std::string hints = move + " fly   " + map.Keys("jump") + " / " + map.Keys("down") + " up, down   " + map.Keys("sprint")
			+ " faster   hold " + map.Keys("look") + " to look   " + map.Keys("fire") + " fire   " + map.Keys("terminal") + " terminal";
		if (m_Mode == Mode::Possess)
		{
			mode = "CONTROLLING THE DUMMY";
			// What a jump does here: over the wall in front, onto the ledge.
			const Humanoid* rig = m_Characters.ControlledRig();
			std::string jump = rig && rig->ParkourHint()[0] ? rig->ParkourHint() : "jump";
			hints = move + " walk   " + map.Keys("sprint") + " run   " + map.Keys("jump") + " " + jump + "   hold " + map.Keys("look")
				+ " to look   " + map.Keys("interact") + " let go";
			// Hanging, the keys are his: along the edge, up, down.
			if (rig && rig->IsHanging())
				hints = map.Keys("left") + " " + map.Keys("right") + " along the edge   " + map.Keys("forward") + " or " + map.Keys("jump")
					+ " climb up   " + map.Keys("back") + " drop   hold " + map.Keys("look") + " to look   " + map.Keys("interact") + " let go";
		}
		m_UI.Set("hud.mode", mode);
		m_UI.Set("hud.hints", hints);
	}

	void RegisterScreens()
	{
		m_UI.RegisterBuiltin("hud", HudScreen());
		m_UI.RegisterBuiltin("cannon", CannonScreen());
	}

	void RegisterCommands()
	{
		GameConsole& c = m_Console;
		c.Register("possess", "<name>", "become a character: walk it about (E lets go)", [this](const GameConsole::Args& a)
		{
			std::string name;
			for (const std::string& w : a)
				name += (name.empty() ? "" : " ") + w;
			if (name.empty() || !Possess(name))
				m_Console.Bad("no character called '" + name + "'");
			else
				m_Console.Good("you are " + name + " -- E to let go");
		});
		c.Register("release", "", "let go of the character", [this](const GameConsole::Args&)
		{
			if (m_Mode == Mode::Possess)
				Release();
		});
		c.Register("terminal", "", "step up to the terminal (or open it, where it is not in the world)", [this](const GameConsole::Args&)
		{
			m_Console.SetOpen(true);
		});
		c.Register("operate", "", "take the launcher's controls: aim with the mouse, click to fire", [this](const GameConsole::Args&)
		{
			if (!Operate())
				m_Console.Bad("there is no launcher to operate");
		});
		c.Register("leave", "", "step away from the launcher", [this](const GameConsole::Args&)
		{
			if (m_Mode == Mode::Cannon)
				Leave();
		});
		c.Register("view", "<x> <y> <z> [<at x> <at y> <at z>]", "put the fly camera there, looking at a point", [this](const GameConsole::Args& a)
		{
			float v[6] = { 0, 0, 0, 0, 1, 0 };
			bool look = a.size() >= 6;
			if (a.size() < 3)
			{
				m_Console.Bad("view 2 1.7 3 0 1.2 0");
				return;
			}
			for (size_t i = 0; i < (look ? 6u : 3u); i++)
				if (!m_Console.Number(a[i], v[i]))
					return;
			auto* t = Transform(m_FlyCamera);
			if (!t)
			{
				m_Console.Bad("this game has no fly camera");
				return;
			}
			if (m_Mode == Mode::Possess)
				Release();
			t->Position = glm::vec3(v[0], v[1], v[2]);
			if (look)
				t->Rotation = RotationLookingAlong(glm::vec3(v[3], v[4], v[5]) - t->Position);
			TakeLook(*t);
			m_Easing = false;
			MakeActive(m_FlyCamera);
		});
		c.Register("ui", "show|hide <screen> | set <name> <value>", "the game's screens (ui/<name>.gsui) and the values they show",
			[this](const GameConsole::Args& a)
		{
			if (a.size() >= 2 && (a[0] == "show" || a[0] == "hide"))
			{
				if (!m_UI.Show(a[1], a[0] == "show"))
					m_Console.Bad("no screen '" + a[1] + "'");
				return;
			}
			if (a.size() >= 3 && a[0] == "set")
			{
				std::string value;
				for (size_t i = 2; i < a.size(); i++)
					value += (i > 2 ? " " : "") + a[i];
				m_UI.Set(a[1], value);
				return;
			}
			m_Console.Bad("ui show cannon, ui hide hud, or ui set hud.mode HELLO");
		});
	}

	GS::Scene& m_Scene;
	GS::PhysicsWorld3D& m_World;
	CharacterController& m_Characters;
	LauncherSystem& m_Launchers;
	GameConsole& m_Console;
	GameUI::Context m_UI;

	Mode m_Mode = Mode::Fly;
	bool m_MouseLookWanted = false;   // SetMouseLook
	GS::EntityId m_FlyCamera = GS::InvalidEntity;
	GS::EntityId m_Terminal = GS::InvalidEntity;
	float m_Yaw = 0.0f, m_Pitch = 0.0f;
	glm::vec2 m_LastMouse{ 0.0f };
	bool m_MouseKnown = false;
	bool m_EWas = false, m_EscWas = false, m_ClickWas = false;
	bool m_UIWantsPointer = false;

	GS::EntityId m_Focus = GS::InvalidEntity;
	glm::vec3 m_FocusPoint{ 0.0f };

	bool m_Easing = false;
	glm::vec3 m_GoalPosition{ 0.0f }, m_GoalDirection{ 0.0f, 0.0f, -1.0f };
	glm::vec3 m_ReturnPosition{ 0.0f }, m_ReturnDirection{ 0.0f, 0.0f, -1.0f };

	glm::vec3 m_AimPoint{ 0.0f };
	bool m_HaveAim = false;
	std::vector<glm::vec3> m_Path;
	bool m_PathHit = false;
	glm::vec3 m_PathEnd{ 0.0f }, m_PathNormal{ 0.0f, 1.0f, 0.0f };

	std::map<GS::EntityId, GameUI::Texture> m_Textures;
	float m_Time = 0.0f;
};
