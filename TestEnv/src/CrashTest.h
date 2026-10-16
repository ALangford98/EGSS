#pragma once

// Crash test: a laboratory hall with a crash-test dummy standing on his mark,
// a cannon aimed at him with its computer beside it, and things to land in.
// Fly the camera about (WASD, hold the right mouse button to look), walk up
// to things and press E:
//
//   the dummy     take control of him and walk him about (E lets go)
//   the computer  the game terminal, on its screen (Esc steps back)
//   the cannon    its controls: aim with the mouse along the trajectory
//                 line, pick an object and set its size, mass and speed
//                 on the HUD, click to fire
//
// It is a scene, not a demo with code of its own: everything that happens is
// the runtime's -- GameSession, GameModes, the humanoid, the launcher, the
// game UI and its screens -- so it plays the same in the editor (File > Add
// Demo Scene > Crash Test, then Play), in an exported game, and here, where
// this layer simply runs a GameSession on the same scene. Its look is the
// engine's own models (GS/Procedural/Prefabs) under an EnvironmentComponent:
// sun through the skylights, shadows, and the hall's own lamps.

#include <GS.h>
#include <imgui.h>

#include "Demo.h"
#include "GameModes.h"
#include "GameSession.h"
#include "PlayMode.h"
#include "SceneRenderer.h"

#include <GS/Procedural/Prefabs.h>

#include <filesystem>
#include <fstream>
#include <memory>

namespace CrashTestScene
{
	// The rotation (degrees, as TransformComponent holds it) of a camera at
	// `from` looking at `to`.
	inline glm::vec3 Look(const glm::vec3& from, const glm::vec3& to)
	{
		return RotationLookingAlong(to - from);
	}

	inline GS::Entity Mesh(GS::Scene& scene, const char* name, const char* source, const glm::vec3& at,
		const glm::vec3& rotation = glm::vec3(0.0f), const glm::vec3& scale = glm::vec3(1.0f),
		const glm::vec4& colour = glm::vec4(1.0f))
	{
		GS::Entity entity = scene.CreateEntity(name);
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Rotation = rotation;
		transform->Scale = scale;
		GS::MeshComponent mesh;
		mesh.SourcePath = source;
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		mesh.Color = colour;
		entity.Add<GS::MeshComponent>(mesh);
		return entity;
	}

	inline GS::Entity Box(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec3& size,
		const glm::vec4& colour, float roughness = 0.5f, float metallic = 0.0f)
	{
		GS::Entity entity = Mesh(scene, name, "primitive:cube", at, glm::vec3(0.0f), size, colour);
		GS::MeshComponent* mesh = entity.Get<GS::MeshComponent>();
		mesh->Roughness = roughness;
		mesh->Metallic = metallic;
		return entity;
	}

	inline void Static(GS::Entity entity, float friction = 0.8f)
	{
		GS::PhysicsComponent body;
		body.Type = GS::BodyType::Static;
		body.Friction = friction;
		entity.Add<GS::PhysicsComponent>(body);
	}

	// A collider with nothing drawn: walls, the cannon's footprint.
	inline void Collider(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec3& size)
	{
		GS::Entity entity = Box(scene, name, at, size, glm::vec4(1.0f));
		entity.Get<GS::MeshComponent>()->Visible = false;
		Static(entity);
	}

	inline void Interact(GS::Entity entity, const char* action, const char* prompt, float range)
	{
		GS::InteractableComponent interact;
		interact.Action = action;
		interact.Prompt = prompt;
		interact.Range = range;
		entity.Add<GS::InteractableComponent>(interact);
	}

	inline void Camera(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec3& lookAt, bool active, bool fly = false)
	{
		GS::Entity entity = scene.CreateEntity(name);
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Rotation = Look(at, lookAt);
		GS::CameraComponent camera;
		camera.Fov = 55.0f;
		camera.Active = active;
		entity.Add<GS::CameraComponent>(camera);
		if (fly)
			entity.Add<GS::FlyCameraComponent>(GS::FlyCameraComponent{});
	}

	inline void Light(GS::Scene& scene, const char* name, const glm::vec3& at, const glm::vec4& colour, float radius)
	{
		GS::Entity entity = scene.CreateEntity(name);
		entity.Get<GS::TransformComponent>()->Position = at;
		GS::LightComponent light;
		light.Color = colour;
		light.Radius = radius;
		entity.Add<GS::LightComponent>(light);
	}

	inline GS::Entity Dummy(GS::Scene& scene, const char* name, const glm::vec3& at, float yaw, const glm::vec4& colour, int behaviour)
	{
		GS::Entity dummy = Mesh(scene, name, "primitive:cube", at, glm::vec3(0.0f, yaw, 0.0f), glm::vec3(1.0f), colour);
		GS::NpcComponent npc;
		npc.Behaviour = behaviour;
		npc.Body = 2;        // humanoid
		npc.Look = 1;        // the crash-test dummy over his colliders
		npc.Speed = 1.2f;
		npc.WanderRadius = 2.5f;
		dummy.Add<GS::NpcComponent>(npc);
		Interact(dummy, (std::string("possess ") + name).c_str(), "Take control", 3.5f);
		return dummy;
	}

	// The hall is 30 m across (x), 40 m long (z) and 7.5 m to the roof. The
	// dummy stands on his mark at the origin facing +z, down the lane to
	// the cannon 11 m away; behind him an impact wall and crates.
	inline void Build(GS::Scene& scene)
	{
		const float halfX = 15.0f, backZ = -18.0f, frontZ = 22.0f, roofY = 7.6f;
		const float midZ = (backZ + frontZ) * 0.5f, halfZ = (frontZ - backZ) * 0.5f;

		// The look: sun through the skylights, an interior's dim ambient,
		// a little haze.
		GS::Entity environment = scene.CreateEntity("Environment");
		GS::EnvironmentComponent env;
		env.SunAzimuth = 28.0f;
		env.SunElevation = 57.0f;
		env.SunIntensity = 3.6f;
		env.SunColor = { 1.0f, 0.93f, 0.82f };
		// A pale sky: inside, the ambient is light off walls and floor as
		// much as off the sky, and a saturated blue tints everything in shade.
		env.SkyColor = { 0.56f, 0.66f, 0.8f };
		env.HorizonColor = { 0.8f, 0.8f, 0.78f };
		env.GroundColor = { 0.42f, 0.39f, 0.35f };
		env.AmbientIntensity = 0.4f;
		env.FogDensity = 0.006f;
		env.FogFalloff = 0.12f;
		env.Exposure = 1.1f;
		env.ShadowExtent = 22.0f;
		env.PointLightScale = 1.0f;
		environment.Add<GS::EnvironmentComponent>(env);

		// The floor: a slab whose top is y = 0, polished concrete with
		// saw-cut joints, and the painted test lane.
		GS::Entity floor = Box(scene, "Floor", { 0.0f, -0.5f, midZ }, { 2.0f * halfX, 1.0f, 2.0f * halfZ },
			{ 0.47f, 0.48f, 0.49f, 1.0f }, 0.38f);
		Static(floor);
		Mesh(scene, "Floor Joints", "model:floorgrid", { 0.0f, 0.0f, midZ });
		Mesh(scene, "Lane", "model:lane", { 0.0f, 0.0f, 0.0f });

		// Walls: bays of panel, and colliders behind them.
		for (int k = 0; k < 8; k++)
		{
			float x = -halfX + 2.0f + 4.0f * k;
			Mesh(scene, "Wall", "model:wallpanel", { x, 0.0f, backZ }, { 0.0f, 0.0f, 0.0f });
			Mesh(scene, "Wall", "model:wallpanel", { x, 0.0f, frontZ }, { 0.0f, 180.0f, 0.0f });
		}
		for (int k = 0; k < 10; k++)
		{
			float z = backZ + 2.0f + 4.0f * k;
			Mesh(scene, "Wall", "model:wallpanel", { -halfX, 0.0f, z }, { 0.0f, 90.0f, 0.0f });
			Mesh(scene, "Wall", "model:wallpanel", { halfX, 0.0f, z }, { 0.0f, -90.0f, 0.0f });
		}
		// The band between the wall bays and the roof.
		Box(scene, "Wall Top", { 0.0f, 6.8f, backZ - 0.05f }, { 2.0f * halfX, 1.6f, 0.2f }, { 0.22f, 0.25f, 0.3f, 1.0f });
		Box(scene, "Wall Top", { 0.0f, 6.8f, frontZ + 0.05f }, { 2.0f * halfX, 1.6f, 0.2f }, { 0.22f, 0.25f, 0.3f, 1.0f });
		Box(scene, "Wall Top", { -halfX - 0.05f, 6.8f, midZ }, { 0.2f, 1.6f, 2.0f * halfZ }, { 0.22f, 0.25f, 0.3f, 1.0f });
		Box(scene, "Wall Top", { halfX + 0.05f, 6.8f, midZ }, { 0.2f, 1.6f, 2.0f * halfZ }, { 0.22f, 0.25f, 0.3f, 1.0f });
		Collider(scene, "Wall Collider", { 0.0f, 3.5f, backZ - 0.3f }, { 2.0f * halfX, 7.0f, 0.4f });
		Collider(scene, "Wall Collider", { 0.0f, 3.5f, frontZ + 0.3f }, { 2.0f * halfX, 7.0f, 0.4f });
		Collider(scene, "Wall Collider", { -halfX - 0.3f, 3.5f, midZ }, { 0.4f, 7.0f, 2.0f * halfZ });
		Collider(scene, "Wall Collider", { halfX + 0.3f, 3.5f, midZ }, { 0.4f, 7.0f, 2.0f * halfZ });

		// Columns along the side walls, trusses across, the roof with its
		// three skylights, and lamps hung from the trusses.
		for (float z = backZ + 4.0f; z < frontZ - 1.0f; z += 8.0f)
		{
			Mesh(scene, "Column", "model:pillar", { -halfX + 0.35f, 0.0f, z }, { 0.0f, 90.0f, 0.0f });
			Mesh(scene, "Column", "model:pillar", { halfX - 0.35f, 0.0f, z }, { 0.0f, 90.0f, 0.0f });
			for (float x : { -10.0f, 0.0f, 10.0f })
				Mesh(scene, "Truss", "model:truss", { x, roofY - 0.6f, z });
			for (float x : { -3.5f, 3.5f, -10.5f, 10.5f })
				Mesh(scene, "Lamp", "model:lightpanel", { x, roofY - 1.75f, z }, { 0.0f, 90.0f, 0.0f });
		}
		Mesh(scene, "Roof", "model:roof", { 0.0f, roofY, midZ });

		// The hall's own lights: over the mark, over the cannon, and fills.
		const glm::vec4 lamp(1.0f, 0.93f, 0.82f, 1.0f);
		Light(scene, "Lamp Light", { 0.0f, 5.6f, 0.5f }, lamp * 1.2f, 14.0f);
		Light(scene, "Lamp Light", { 0.0f, 5.6f, 11.0f }, lamp, 13.0f);
		Light(scene, "Lamp Light", { -4.0f, 5.6f, -6.0f }, lamp * 0.8f, 13.0f);
		Light(scene, "Lamp Light", { 6.0f, 5.6f, 5.0f }, lamp * 0.7f, 13.0f);
		Light(scene, "Lamp Light", { -7.0f, 5.6f, 8.0f }, lamp * 0.7f, 13.0f);
		Light(scene, "Lamp Light", { 4.0f, 5.6f, -12.0f }, lamp * 0.7f, 13.0f);
		Light(scene, "Lamp Light", { 0.0f, 5.6f, 18.0f }, lamp * 0.6f, 13.0f);

		// The dummy on his mark, and a second one ambling about by the wall.
		Dummy(scene, "Dummy", { 0.0f, 0.0f, 0.0f }, 0.0f, { 0.98f, 0.8f, 0.1f, 1.0f }, 0);
		Dummy(scene, "Test Dummy B", { -6.5f, 0.0f, -3.0f }, 60.0f, { 0.95f, 0.42f, 0.12f, 1.0f }, 1);

		// The test population, wandering the far side of the hall: every one
		// a different body and a different walk -- heights from 1.35 to
		// 1.98 m, 35 to 115 kg, masculine to feminine builds -- to show what
		// the rig does with each.
		struct Walker { const char* Name; glm::vec3 At; GS::PhysiqueSettings Body; const char* Style; glm::vec4 Colour; };
		const Walker walkers[] = {
			{ "Tall Dummy",     { 7.0f, 0.0f, -8.0f }, { 1.98f, 88.0f, 0.15f }, "Confident", { 0.20f, 0.55f, 0.95f, 1.0f } },
			{ "Female Dummy",   { 9.5f, 0.0f, -4.0f }, { 1.64f, 62.0f, 1.0f },  "Feminine",  { 0.95f, 0.45f, 0.65f, 1.0f } },
			{ "Heavy Dummy",    { 6.0f, 0.0f, -1.0f }, { 1.75f, 115.0f, 0.3f }, "Tired",     { 0.55f, 0.75f, 0.30f, 1.0f } },
			{ "Small Dummy",    { 10.0f, 0.0f, 1.5f }, { 1.35f, 35.0f, 0.5f },  "Brisk",     { 0.20f, 0.80f, 0.80f, 1.0f } },
			{ "Elderly Dummy",  { 7.5f, 0.0f, 4.5f },  { 1.70f, 66.0f, 0.4f },  "Elderly",   { 0.75f, 0.70f, 0.60f, 1.0f } },
			{ "Drunk Dummy",    { 11.0f, 0.0f, -9.5f },{ 1.80f, 84.0f, 0.0f },  "Drunk",     { 0.60f, 0.30f, 0.80f, 1.0f } },
			{ "Limping Dummy",  { 9.0f, 0.0f, 7.5f },  { 1.82f, 76.0f, 0.1f },  "Limp",      { 0.90f, 0.25f, 0.20f, 1.0f } },
			{ "Sneaking Dummy", { 5.5f, 0.0f, -12.0f },{ 1.68f, 58.0f, 0.7f },  "Sneak",     { 0.25f, 0.25f, 0.30f, 1.0f } },
		};
		for (const Walker& w : walkers)
		{
			GS::Entity dummy = Dummy(scene, w.Name, w.At, 0.0f, w.Colour, 1);
			GS::NpcComponent* npc = dummy.Get<GS::NpcComponent>();
			npc->Physique = w.Body;
			npc->Gait = Humanoid::GaitPreset(w.Style);
			npc->WanderRadius = 3.5f;
		}

		// The impact wall behind him, crates to land in, barriers along the
		// lane, and a stack of crates for scenery.
		GS::Entity pad = Mesh(scene, "Impact Wall", "model:padwall", { 0.0f, 1.22f, -4.6f });
		Static(pad);
		for (int row = 0; row < 3; row++)
			for (int k = 0; k < 3 - row; k++)
			{
				float x = (k - 0.5f * (2 - row)) * 0.62f;
				GS::Entity crate = Mesh(scene, "Crate", "model:crate", { x, 0.3f + row * 0.6f, -2.2f }, glm::vec3(0.0f), glm::vec3(0.6f));
				GS::PhysicsComponent body;
				body.Mass = 15.0f;
				body.Friction = 0.7f;
				crate.Add<GS::PhysicsComponent>(body);
			}
		for (float z : { 3.0f, 5.2f, 7.4f })
			for (float x : { -2.6f, 2.6f })
				Static(Mesh(scene, "Barrier", "model:barrier", { x, 0.4f, z }, { 0.0f, 90.0f, 0.0f }));
		const glm::vec3 stacks[] = { { 9.5f, 0.0f, -10.0f }, { 10.6f, 0.0f, -10.4f }, { 10.0f, 0.0f, -9.0f }, { -12.8f, 0.0f, 18.0f } };
		for (const glm::vec3& at : stacks)
		{
			GS::Entity crate = Mesh(scene, "Crate", "model:crate", at + glm::vec3(0.0f, 0.5f, 0.0f), { 0.0f, at.x * 7.0f, 0.0f });
			GS::PhysicsComponent body;
			body.Mass = 40.0f;
			crate.Add<GS::PhysicsComponent>(body);
		}

		// The parkour yard, down the west side: a wall to vault (1 m, 30 cm
		// thick), a box to step onto (60 cm), a ledge to climb (1.6 m) and
		// a tower above it (3 m, 1.4 m higher) to climb again and drop off
		// the far side of into a roll. The Runner Dummy waits at the start;
		// take control of him and run at it (jump at the climbs).
		const float yardX = -8.5f;
		const glm::vec4 concrete(0.62f, 0.6f, 0.56f, 1.0f), painted(0.85f, 0.55f, 0.15f, 1.0f);
		Static(Box(scene, "Vault Wall", { yardX, 0.5f, 4.0f }, { 3.0f, 1.0f, 0.3f }, painted, 0.6f));
		Static(Box(scene, "Step Box", { yardX, 0.3f, 7.5f }, { 3.0f, 0.6f, 1.4f }, concrete, 0.7f));
		Static(Box(scene, "Ledge", { yardX, 0.8f, 11.0f }, { 3.0f, 1.6f, 2.4f }, concrete, 0.7f));
		Static(Box(scene, "Tower", { yardX, 1.5f, 13.6f }, { 3.0f, 3.0f, 2.8f }, { 0.5f, 0.52f, 0.55f, 1.0f }, 0.7f));
		// A wall along the hall's west side to run along (3.5 m, 8 m long:
		// run beside it and jump; jump again to kick off), and a ledge too
		// high to reach standing (2.35 m: jump to it and hang; A and D go
		// along it, W climbs, S lets go). Walking off the tower's far side
		// lowers him into a hang too; running off it, he drops and rolls.
		Static(Box(scene, "Run Wall", { -12.6f, 1.75f, 6.0f }, { 0.4f, 3.5f, 8.0f }, { 0.55f, 0.57f, 0.6f, 1.0f }, 0.7f));
		Static(Box(scene, "High Ledge", { -4.8f, 1.175f, 18.5f }, { 2.6f, 2.35f, 1.2f }, concrete, 0.7f));
		// Run the wall north and the Corner Wall across its end turns him
		// onto it; the Slant Wall, met at an angle running, is a tic-tac;
		// the pillars (a step up to them) are for standing precision jumps.
		// A run and a jump two metres short of the High Ledge is a cat leap.
		Static(Box(scene, "Corner Wall", { -11.3f, 1.75f, 10.2f }, { 2.2f, 3.5f, 0.4f }, { 0.55f, 0.57f, 0.6f, 1.0f }, 0.7f));
		{
			GS::Entity slant = Box(scene, "Slant Wall", { -4.8f, 1.75f, 14.0f }, { 3.0f, 3.5f, 0.4f }, { 0.55f, 0.57f, 0.6f, 1.0f }, 0.7f);
			slant.Get<GS::TransformComponent>()->Rotation = { 0.0f, 50.0f, 0.0f };
			Static(slant);
		}
		Static(Box(scene, "Pillar Step", { -10.0f, 0.25f, -13.6f }, { 1.0f, 0.5f, 1.0f }, concrete, 0.7f));
		for (float z : { -12.0f, -10.4f, -8.8f })
			Static(Box(scene, "Pillar", { -10.0f, 0.5f, z }, { 0.5f, 1.0f, 0.5f }, painted, 0.6f));
		Dummy(scene, "Runner Dummy", { yardX, 0.0f, 0.0f }, 0.0f, { 0.15f, 0.75f, 0.45f, 1.0f }, 0);

		// The cannon, 11 m down the lane, firing back along -z: its base
		// (with the computer), the yoke that turns, the barrel that tips --
		// the barrel is the launcher -- and the computer's screen.
		const glm::vec3 pivot(0.0f, 1.25f, 11.0f);
		Mesh(scene, "Cannon", "model:cannon.base", pivot, { 0.0f, 180.0f, 0.0f });
		Mesh(scene, "Launcher Yoke", "model:cannon.yoke", pivot, { 0.0f, 180.0f, 0.0f });
		GS::Entity launcher = Mesh(scene, "Launcher", "model:cannon.barrel", pivot, { 0.0f, 180.0f, 0.0f });
		GS::LauncherComponent fire;
		fire.Target = "Dummy";
		launcher.Add<GS::LauncherComponent>(fire);
		Interact(launcher, "operate", "Operate the cannon", 3.2f);
		Collider(scene, "Cannon Collider", { pivot.x, 0.06f, pivot.z }, { 1.6f, 0.12f, 1.6f });
		Collider(scene, "Cannon Collider", { pivot.x, 0.45f, pivot.z }, { 0.6f, 0.8f, 0.6f });

		glm::vec3 centre, facing;
		glm::vec2 half;
		GS::Prefabs::CannonScreen(centre, half, facing);
		// The base is turned half round: its (x, z) are the world's (-x, -z).
		glm::vec3 local = centre - glm::vec3(0.0f, pivot.y, 0.0f);
		glm::vec3 screenAt = pivot + glm::vec3(-local.x, local.y, -local.z);
		GS::Entity screen = Mesh(scene, "Terminal Screen", "model:screen", screenAt, { -10.0f, 0.0f, 0.0f },
			glm::vec3(2.0f * half.x, 2.0f * half.y, 1.0f));
		GS::ScreenComponent terminal;
		terminal.Screen = "@terminal";
		terminal.Width = 1024;
		terminal.Height = 640;
		terminal.Emission = 2.2f;
		screen.Add<GS::ScreenComponent>(terminal);
		Interact(screen, "terminal", "Use the terminal", 2.6f);

		// The lab's sign over the impact wall: a .gsui screen.
		GS::Entity sign = Mesh(scene, "Sign", "model:screen", { 0.0f, 4.6f, backZ + 0.16f }, glm::vec3(0.0f), { 9.0f, 2.25f, 1.0f });
		GS::ScreenComponent signScreen;
		signScreen.Screen = "crashlab-sign";
		signScreen.Width = 1600;
		signScreen.Height = 400;
		signScreen.Emission = 1.1f;
		sign.Add<GS::ScreenComponent>(signScreen);

		// Cameras: the one you fly (the view), and fixed ones -- "camera"
		// in the terminal lists them.
		Camera(scene, "Fly", { 6.8f, 2.6f, 15.5f }, { 0.0f, 1.0f, 3.0f }, true, true);
		Camera(scene, "Side", { 7.5f, 1.8f, 1.5f }, { 0.0f, 1.0f, 0.8f }, false);
		Camera(scene, "Behind Launcher", { 0.9f, 2.4f, 14.8f }, { 0.0f, 1.0f, 0.0f }, false);
		Camera(scene, "Wide", { 12.0f, 6.0f, 16.0f }, { 0.0f, 0.5f, 2.5f }, false);
		Camera(scene, "Close", { 2.6f, 1.4f, 2.4f }, { 0.0f, 1.1f, 0.0f }, false);
		Camera(scene, "Yard", { 1.5f, 4.2f, 9.5f }, { -8.5f, 1.4f, 11.0f }, false);
	}

	// The sign: a .gsui screen like any HUD, drawn into a texture.
	inline const char* SignScreen()
	{
		return R"json({
			"theme": { "accent": "#ffc21a", "text": "#f2f4f6", "dim": "#9aa4ae" },
			"root": { "type": "panel", "style": "clear", "stretch": "both", "padding": 0, "children": [
				{ "type": "panel", "stretch": "both", "pos": [24, 24], "padding": 0 },
				{ "type": "panel", "anchor": "center", "layout": "vertical", "style": "clear", "padding": 0, "gap": 10, "children": [
					{ "type": "label", "text": "CRASH TEST LAB", "style": "big", "fontSize": 150, "align": "center" },
					{ "type": "label", "text": "BAY 1  //  HIGH ENERGY IMPACT  //  KEEP CLEAR", "fontSize": 82,
					  "style": "accent", "align": "center" } ] }
			] } })json";
	}

	// The screens this scene uses, into a project's ui/ folder, unless it
	// already has them (they may have been edited).
	inline void WriteScreens(const std::string& projectPath)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path ui = fs::path(projectPath) / "ui";
		fs::create_directories(ui, ec);
		auto write = [&](const char* name, const char* text)
		{
			fs::path file = ui / name;
			if (fs::exists(file))
				return;
			std::ofstream out(file, std::ios::binary);
			out << text << "\n";
		};
		write("crashlab-sign.gsui", SignScreen());
		write("cannon.gsui", GameModes::CannonScreen());
		write("hud.gsui", GameModes::HudScreen());
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
		// Here there is no project to hold ui/crashlab-sign.gsui.
		m_Session->UI().RegisterBuiltin("crashlab-sign", CrashTestScene::SignScreen());
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

	void OnDemoGameOverlay() override
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

	void OnExportFiles(const std::string& projectPath) override
	{
		CrashTestScene::WriteScreens(projectPath);
	}

	GameSession* Session() { return m_Session.get(); }
	GS::Scene* Scene() { return m_Scene.get(); }

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
