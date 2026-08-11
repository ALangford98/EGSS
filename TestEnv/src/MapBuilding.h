#pragma once

#include "Ragdoll.h"
#include "Terrain.h"

// Map Building: the character from the Ragdoll demo, walking on generated
// terrain.
//
// The terrain is described by a **seed** and nothing else. Type the same number
// in and you get the same map back, on any run and in any build -- which is the
// difference between a generated world and a disposable one.
//
// **This is a subclass of `Ragdoll`, and that is the point.** The rig is
// thirteen bodies, twelve joints, a gait, a balance controller and about a
// hundred and fifty constants that were each measured rather than guessed. A
// second copy of that would be a second thing to keep in step, and the first
// time one was tuned and the other was not, the two demos would quietly stop
// being the same character. What Map Building actually changes is the ground
// and the light, so those are what it overrides:
//
//   * `BuildGround` puts a heightfield in the world instead of a floor slab
//   * `SpawnPoint` puts the figure on the surface rather than at y = 0
//   * `DrawWorld` draws the terrain mesh, which has no collider primitive
//   * `ClearColour` and `SetSceneLighting` swap the point light for a sun
//
// Everything else is inherited unchanged, and that it works at all is a
// property of the rig rather than of this file: every question the character
// asks about the ground already goes through `PhysicsWorld3D::GroundHeightBelow`
// rather than assuming a floor at zero. Nothing in the gait needed touching.
//
// **WASD** walks, **shift** runs, **space** jumps, **G** ragdolls -- all of it
// as in Ragdoll. **R** regenerates with a new seed, and **[** and **]** step
// the seed by one so neighbouring maps can be compared.
class MapBuilding : public Ragdoll
{
public:
	MapBuilding()
		: Ragdoll("Map Building")
	{
		// The camera starts further back than the Ragdoll's: there is a map to
		// see, not just a figure.
		m_CameraDistance = 6.0f;
	}

	void OnDemoAttach() override
	{
		// The map has to exist before the base class builds a scene on it:
		// BuildGround and SpawnPoint are both called from there and both read
		// the terrain.
		m_Terrain.Generate();
		BuildTerrainMesh();

		Ragdoll::OnDemoAttach();

		// After the base class, which sets up the Physics3D material for a
		// scene a few metres across.
		BuildSunShader();
	}

private:
	// --- The world ----------------------------------------------------------

	void BuildGround() override
	{
		Egss::RigidBody3D ground = Egss::RigidBody3D::MakeHeightfield(
			{ 0.0f, 0.0f, 0.0f }, m_Terrain.Field());

		// The same friction the character's own bodies carry, so the combined
		// coefficient is 0.7 -- which is what the slope tests were run at, and
		// puts the angle he can stand on at atan(0.7), a little under 35
		// degrees. Steeper than that and he slides, which is correct and worth
		// going and finding on the map.
		ground.Friction = 0.7f;
		ground.Restitution = 0.0f;

		m_World.AddBody(ground);
	}

	glm::vec3 SpawnPoint() const override
	{
		// A flat-ish spot near the middle, found by looking rather than
		// assumed: dropping the rig onto a steep face means it starts sliding
		// before it has finished assembling.
		glm::vec2 at = FlattestSpotNear({ 0.0f, 0.0f }, 12.0f);
		return { at.x, m_Terrain.HeightAt(at.x, at.y) + m_DropHeight, at.y };
	}

	// The most level place within `range` of `around`, by the steepest slope
	// found over a small patch rather than by the slope at one point -- a
	// single sample can be level on the lip of a drop.
	glm::vec2 FlattestSpotNear(const glm::vec2& around, float range) const
	{
		glm::vec2 best = around;
		float bestSlope = std::numeric_limits<float>::max();

		const int steps = 24;
		for (int iz = 0; iz <= steps; iz++)
		{
			for (int ix = 0; ix <= steps; ix++)
			{
				glm::vec2 at = around + glm::vec2(
					-range + 2.0f * range * (float)ix / (float)steps,
					-range + 2.0f * range * (float)iz / (float)steps);

				if (!m_Terrain.Contains(at.x, at.y))
					continue;

				// Over a patch the size of the figure's stance, so a spot is
				// only level if there is room to stand on it.
				float worst = 0.0f;
				for (float dz = -0.4f; dz <= 0.4f; dz += 0.4f)
				{
					for (float dx = -0.4f; dx <= 0.4f; dx += 0.4f)
					{
						glm::vec3 normal = m_Terrain.NormalAt(at.x + dx, at.y + dz);
						worst = std::max(worst, 1.0f - normal.y);
					}
				}

				if (worst < bestSlope)
				{
					bestSlope = worst;
					best = at;
				}
			}
		}

		return best;
	}

	// --- Drawing ------------------------------------------------------------

	glm::vec4 ClearColour() const override { return { 0.42f, 0.56f, 0.72f, 1.0f }; }

	void SetSceneLighting() override
	{
		m_SceneMaterial->Set("u_SunDirection", glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)));
		m_SceneMaterial->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_SceneMaterial->Set("u_SkyColor", glm::vec3(0.42f, 0.52f, 0.66f));
	}

	void DrawWorld() override
	{
		if (!m_TerrainMesh)
			return;

		m_SceneMaterial->Set("u_Color", glm::vec4(0.38f, 0.52f, 0.30f, 1.0f));
		Egss::Renderer::Submit(m_SceneMaterial, m_TerrainMesh, glm::mat4(1.0f));
	}

	// --- Panel --------------------------------------------------------------

	void OnDemoImGui() override
	{
		ImGui::Begin("Map");

		ImGui::Text("R new seed   [ ] step seed");
		ImGui::Separator();

		bool rebuild = false;

		int seed = (int)m_Terrain.Seed;
		if (ImGui::InputInt("Seed", &seed))
		{
			m_Terrain.Seed = (uint32_t)seed;
			rebuild = true;
		}

		ImGui::Text("Checksum %08X -- the same seed always gives this",
			m_Terrain.Checksum());
		ImGui::Text("%d x %d samples over %.0f m, %.2f to %.2f m high",
			m_Terrain.Resolution, m_Terrain.Resolution, m_Terrain.Extent,
			m_Terrain.Lowest(), m_Terrain.Highest());
		ImGui::Text("%zu triangles", m_TriangleCount);

		glm::vec3 stood = m_World.GetBody(m_Pelvis).Position;
		glm::vec3 normal = m_Terrain.NormalAt(stood.x, stood.z);
		ImGui::Text("Standing at %.1f, %.1f on a %.1f deg slope",
			stood.x, stood.z, glm::degrees(std::acos(glm::clamp(normal.y, -1.0f, 1.0f))));

		// atan(0.7) with the friction both bodies carry. Past this he cannot
		// stand up whatever the gait does, which is a fact about statics and
		// not about the controller.
		ImGui::Text("He slides above %.1f deg", glm::degrees(std::atan(0.7f)));

		rebuild |= ImGui::SliderFloat("Amplitude (m)", &m_Terrain.Amplitude, 1.0f, 30.0f);
		rebuild |= ImGui::SliderFloat("Frequency", &m_Terrain.Frequency, 0.5f, 8.0f);
		rebuild |= ImGui::SliderInt("Octaves", &m_Terrain.Octaves, 1, 8);
		rebuild |= ImGui::SliderFloat("Gain", &m_Terrain.Gain, 0.2f, 0.8f);

		if (rebuild)
			Regenerate();

		ImGui::End();

		// And the character's own panel, unchanged.
		Ragdoll::OnDemoImGui();
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);
		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& key)
		{
			if (key.GetKeyCode() == EGSS_KEY_R)
			{
				m_Terrain.Seed = m_Terrain.Seed * 1664525u + 1013904223u;
				Regenerate();
				return true;
			}
			if (key.GetKeyCode() == EGSS_KEY_LEFT_BRACKET)
			{
				m_Terrain.Seed--;
				Regenerate();
				return true;
			}
			if (key.GetKeyCode() == EGSS_KEY_RIGHT_BRACKET)
			{
				m_Terrain.Seed++;
				Regenerate();
				return true;
			}
			return false;
		});

		if (!e.IsHandled())
			Ragdoll::OnDemoEvent(e);
	}

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		Ragdoll::OnDemoFixedUpdate(step);

		// Off the edge of the map is a real fall, not a clamped ledge -- the
		// field reports no ground outside its extent on purpose. Something has
		// to end it, though, or walking west for twenty seconds leaves the demo
		// watching a figure accelerate downwards for ever.
		if (m_World.GetBody(m_Pelvis).Position.y < m_Terrain.Lowest() - 20.0f)
			BuildScene();
	}

	// --- Rebuilding ---------------------------------------------------------

	void Regenerate()
	{
		m_Terrain.Generate();
		BuildTerrainMesh();

		// The whole scene, not just the ground: the character is standing on a
		// map that no longer exists, and the heightfield body holds a pointer
		// to the old field. Rebuilding is also what resets the rig, which the
		// base class already gets right.
		BuildScene();
	}

	void BuildTerrainMesh()
	{
		Egss::MeshData data = m_Terrain.BuildMesh();
		m_TriangleCount = data.TriangleCount();
		m_TerrainMesh = std::make_shared<Egss::Mesh>(data, "Terrain");
	}

	// The map's own shader, because the Physics3D one cannot light it.
	//
	// That shader has a point light with `1 / (1 + 0.015 d^2)` attenuation,
	// which is right for a scene a few metres across and useless at sixty:
	// at 70 m the light contributes 1.3% and the terrain is lit by ambient
	// alone, which is why it came out flat and black. Outdoors wants a *sun* --
	// a direction with no position and no falloff -- plus a sky term that fills
	// the shadowed side, so slopes facing away are still readable.
	//
	// The character is drawn with it too. Two shaders would mean the figure
	// and the ground he stands on disagreeing about where the light is.
	void BuildSunShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				v_WorldPosition = world.xyz;
				v_Normal = mat3(transpose(inverse(u_Transform))) * a_Normal;
				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_WorldPosition;
			in vec3 v_Normal;

			uniform vec4 u_Color;
			uniform vec3 u_SunDirection;   // points *from* the sun
			uniform vec3 u_SunColor;
			uniform vec3 u_SkyColor;

			void main()
			{
				vec3 normal = normalize(v_Normal);
				vec3 toSun = normalize(-u_SunDirection);

				float sun = max(dot(normal, toSun), 0.0);

				// Hemisphere ambient: full sky looking up, half of it looking
				// sideways, none looking down. A flat ambient term makes every
				// slope the same shade, which is the one thing terrain must
				// not do.
				float sky = 0.5 + 0.5 * normal.y;

				vec3 lit = u_Color.rgb * (u_SkyColor * sky * 0.55 + u_SunColor * sun * 0.9);

				color = vec4(lit, 1.0);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("MapBuilding", vertexSrc, fragmentSrc));
		m_SceneMaterial = Egss::Material::Create(m_Shader);
	}

	Terrain m_Terrain;
	std::shared_ptr<Egss::Mesh> m_TerrainMesh;
	std::shared_ptr<Egss::Shader> m_Shader;
	size_t m_TriangleCount = 0;
};
