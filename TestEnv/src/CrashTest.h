#pragma once

// Crash test: a humanoid crash-test dummy, a launcher aimed at him, and a
// stack of crates behind him -- a scene to show the physics off in. What
// the launcher fires (a ball or a box, its size, mass and speed) is set
// from the game's terminal while it runs (` opens it; help lists the
// commands), not in the editor, so the demo is driven from inside the game.
//
// It is a scene, not a demo with code of its own: everything that happens
// is the runtime's -- GameSession, the humanoid, LauncherComponent and the
// game console -- so it plays the same in the editor (File > Add Demo
// Scene > Crash Test, then Play), in an exported game, and here, where
// this layer simply runs a GameSession on the same scene.

#include <GS.h>
#include <imgui.h>

#include "Demo.h"
#include "GameSession.h"
#include "PlayMode.h"
#include "SceneRenderer.h"

#include <memory>

namespace CrashTestScene
{
	// The rotation (degrees, as TransformComponent holds it) of a camera at
	// `from` looking at `to`. SceneRenderer looks along Rx Ry Rz (0, 0, -1)
	// = (-sin b, cos b sin a, -cos b cos a) for rotation (a, b, 0), so
	// b = asin(-dx) and a = atan2(dy, -dz).
	inline glm::vec3 Look(const glm::vec3& from, const glm::vec3& to)
	{
		glm::vec3 d = glm::normalize(to - from);
		float b = std::asin(glm::clamp(-d.x, -1.0f, 1.0f));
		float a = std::atan2(d.y, -d.z);
		return glm::degrees(glm::vec3(a, b, 0.0f));
	}

	inline GS::Entity Box(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec3& size,
		const glm::vec4& colour)
	{
		GS::Entity entity = scene.CreateEntity(name);
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Scale = size;
		GS::MeshComponent mesh;
		mesh.SourcePath = "primitive:cube";
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		mesh.Color = colour;
		entity.Add<GS::MeshComponent>(mesh);
		return entity;
	}

	inline void Camera(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec3& lookAt, bool active)
	{
		GS::Entity entity = scene.CreateEntity(name);
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Rotation = Look(at, lookAt);
		GS::CameraComponent camera;
		camera.Fov = 50.0f;
		camera.Active = active;
		entity.Add<GS::CameraComponent>(camera);
	}

	inline void Light(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec4& colour)
	{
		GS::Entity entity = scene.CreateEntity(name);
		entity.Get<GS::TransformComponent>()->Position = at;
		GS::LightComponent light;
		light.Color = colour;
		light.Radius = 30.0f;
		entity.Add<GS::LightComponent>(light);
	}

	inline void Build(GS::Scene& scene)
	{
		// The floor: a slab whose top is y = 0. (A plane has no thickness,
		// and a body built from its bounds would be a box of none.)
		GS::Entity ground = Box(scene, "Ground", { 0.0f, -0.5f, 2.0f }, { 40.0f, 1.0f, 40.0f }, { 0.30f, 0.33f, 0.38f, 1.0f });
		GS::PhysicsComponent floor;
		floor.Type = GS::BodyType::Static;
		floor.Friction = 0.8f;
		ground.Add<GS::PhysicsComponent>(floor);

		// The test lane, painted on: no body.
		Box(scene, "Lane", { 0.0f, 0.005f, 5.5f }, { 2.6f, 0.01f, 13.0f }, { 0.42f, 0.45f, 0.50f, 1.0f });
		for (int k = 0; k < 6; k++)
			Box(scene, "Lane Mark", { 0.0f, 0.012f, 1.0f + 2.0f * k }, { 0.12f, 0.01f, 0.8f }, { 0.95f, 0.8f, 0.2f, 1.0f });

		// The dummy: a humanoid NPC that stands still, in crash-test yellow.
		GS::Entity dummy = scene.CreateEntity("Dummy");
		GS::MeshComponent dummyColour;
		dummyColour.SourcePath = "primitive:cube";
		dummyColour.Geometry = GS::MeshCache::Get(dummyColour.SourcePath);
		dummyColour.Color = { 0.98f, 0.82f, 0.12f, 1.0f };
		dummy.Add<GS::MeshComponent>(dummyColour);
		GS::NpcComponent npc;
		npc.Behaviour = 0;   // idle
		npc.Body = 2;        // humanoid
		dummy.Add<GS::NpcComponent>(npc);

		// The launcher, eleven metres down the lane, aimed at his chest.
		Box(scene, "Launcher Stand", { 0.0f, 0.5f, 11.0f }, { 0.5f, 1.0f, 0.5f }, { 0.25f, 0.27f, 0.30f, 1.0f });
		GS::Entity launcher = Box(scene, "Launcher", { 0.0f, 1.25f, 11.0f }, { 0.32f, 0.32f, 0.9f }, { 0.75f, 0.2f, 0.15f, 1.0f });
		GS::LauncherComponent fire;
		fire.Target = "Dummy";
		launcher.Add<GS::LauncherComponent>(fire);

		// Crates behind him to land in: a pyramid of 0.6 m boxes.
		for (int row = 0; row < 3; row++)
			for (int k = 0; k < 3 - row; k++)
			{
				float x = (k - 0.5f * (2 - row)) * 0.62f;
				GS::Entity crate = Box(scene, "Crate", { x, 0.3f + row * 0.6f, -2.2f }, glm::vec3(0.6f), { 0.66f, 0.47f, 0.26f, 1.0f });
				GS::PhysicsComponent body;
				body.Mass = 15.0f;
				body.Friction = 0.7f;
				crate.Add<GS::PhysicsComponent>(body);
			}

		// A wall off to the side, for a dummy knocked sideways.
		GS::Entity wall = Box(scene, "Wall", { -3.4f, 1.0f, -0.5f }, { 0.4f, 2.0f, 6.0f }, { 0.55f, 0.56f, 0.6f, 1.0f });
		GS::PhysicsComponent wallBody;
		wallBody.Type = GS::BodyType::Static;
		wall.Add<GS::PhysicsComponent>(wallBody);

		Light(scene, "Light", { 2.0f, 6.0f, 2.0f }, { 1.4f, 1.35f, 1.25f, 1.0f });
		Light(scene, "Light Lane", { -2.0f, 6.0f, 9.0f }, { 1.1f, 1.1f, 1.15f, 1.0f });
		Light(scene, "Light Back", { -3.0f, 5.0f, -4.0f }, { 0.9f, 0.95f, 1.1f, 1.0f });

		Camera(scene, "Side", { 7.5f, 1.8f, 1.5f }, { 0.0f, 1.0f, 0.8f }, true);
		Camera(scene, "Behind Launcher", { 0.9f, 2.3f, 14.5f }, { 0.0f, 1.0f, 0.0f }, false);
		Camera(scene, "Wide", { 9.0f, 6.5f, 11.0f }, { 0.0f, 0.5f, 2.5f }, false);
		Camera(scene, "Close", { 2.6f, 1.4f, 2.4f }, { 0.0f, 1.1f, 0.0f }, false);
	}
}

// The scene, run here as well as exported: a GameSession on it, drawn with
// the editor's own renderer.
class CrashTest : public DemoLayer
{
public:
	CrashTest() : DemoLayer("CrashTest") {}

	void OnDemoAttach() override
	{
		m_Renderer.Init();
	}

	void OnDemoActivated() override
	{
		m_Scene = std::make_unique<GS::Scene>();
		CrashTestScene::Build(*m_Scene);
		m_Session = std::make_unique<GameSession>(*m_Scene, PlayMode::s_ScriptEngine);
		m_Session->Start();
	}

	void OnDemoDeactivated() override
	{
		m_Session.reset();
		m_Scene.reset();
	}

	void OnDemoFixedUpdate(GS::Timestep step) override
	{
		if (m_Session)
			m_Session->FixedUpdate(step);
	}

	void OnDemoUpdate(GS::Timestep) override
	{
		if (!m_Session)
			return;
		ViewportRect view = DemoViewport();
		GS::RenderCommand::SetViewport((unsigned int)view.X, (unsigned int)view.Y, (unsigned int)view.Width, (unsigned int)view.Height);
		GS::RenderCommand::SetClearColor({ 0.52f, 0.62f, 0.74f, 1.0f });
		GS::RenderCommand::Clear();

		float aspect = (float)view.Width / (float)std::max(view.Height, 1);
		GS::PerspectiveCamera* camera = &m_Camera;
		if (!SceneRenderer::FindActiveCamera(*m_Scene, aspect, m_Camera))
		{
			m_Fallback.SetProjection(50.0f, aspect, 0.1f, 500.0f);
			camera = &m_Fallback;
		}
		GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		std::vector<ExtraDraw> humanoids;
		AppendHumanoidDraws(*m_Scene, m_Session.get(), humanoids);
		m_Renderer.Render(*m_Scene, *camera, &humanoids);
		GS::RenderCommand::SetCullFace(GS::CullFace::None);
	}

	void OnDemoImGui() override
	{
		if (!m_Session)
			return;
		ViewportRect view = DemoViewport();
		float displayHeight = ImGui::GetIO().DisplaySize.y;
		m_Session->DrawOverlay((float)view.X, displayHeight - (float)(view.Y + view.Height), (float)view.Width, (float)view.Height);
	}

	void OnExportToScene(GS::Scene& scene) override
	{
		CrashTestScene::Build(scene);
	}

	GameSession* Session() { return m_Session.get(); }

private:
	ViewportRect DemoViewport() const
	{
		if (g_Viewport.Valid())
			return g_Viewport;
		GS::Window& window = GS::Application::Get().GetWindow();
		return { 0, 0, (int)window.GetWidth(), (int)window.GetHeight() };
	}

	std::unique_ptr<GS::Scene> m_Scene;
	std::unique_ptr<GameSession> m_Session;
	SceneRenderer m_Renderer;
	GS::PerspectiveCamera m_Camera{ 50.0f, 16.0f / 9.0f, 0.1f, 500.0f };
	GS::PerspectiveCamera m_Fallback{ 50.0f, 16.0f / 9.0f, 0.1f, 500.0f };
};
