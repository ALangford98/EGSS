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
//
// **Left click puts a block where you are pointing**, which is the part the
// demo is named for. Aiming is `Heightfield3D::Raycast` -- the mouse is a line
// through the world, and the block goes where that line first meets the ground.
// Blocks are already-placed geometry too, so pointing at one stacks on it, and
// pointing at its side builds outwards from it.
//
// Everything about the placement is settled on a *fixed step*, from
// `Input::GetMousePosition` rather than from an event. The mouse is recorded in
// the replay stream and events are not, so a building session records and plays
// back exactly like a walk does -- which would not be true if a click placed a
// block the moment it arrived.
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
		// puts a *dynamic box* on the edge of sliding at atan(0.7), a little
		// under 35 degrees. It does not do the same for him: his root is
		// kinematic and never asks a contact for the force that would break, so
		// the number governs what you drop on the map rather than who walks on
		// it. What limits him is the hip climb rate -- see the panel.
		ground.Friction = 0.7f;
		ground.Restitution = 0.0f;

		m_World.AddBody(ground);
	}

	// --- Building -----------------------------------------------------------
	//
	// A block is remembered as a **cell and a base height**, not as a body
	// handle. `PhysicsWorld3D` has no `RemoveBody`, and could not easily have
	// one while a handle is an index into a vector -- taking a body out of the
	// middle would shift every handle after it, including the twelve the
	// ragdoll's joints hold. So the record here is the truth and the bodies are
	// built from it, which makes undo a rebuild rather than a removal. It costs
	// the character his footing; the alternative costs an engine change that
	// nothing else has asked for yet.
	static constexpr float s_BlockSize = 1.0f;

	struct Block
	{
		glm::ivec2 Cell{ 0 };
		float Base = 0.0f;
	};

	// Where a block would go if you clicked now.
	struct Aim
	{
		bool Valid = false;
		glm::ivec2 Cell{ 0 };
		glm::vec3 Centre{ 0.0f };
	};

	glm::vec2 CentreOf(const glm::ivec2& cell) const
	{
		return { ((float)cell.x + 0.5f) * s_BlockSize,
				 ((float)cell.y + 0.5f) * s_BlockSize };
	}

	glm::ivec2 CellAt(float x, float z) const
	{
		return { (int)std::floor(x / s_BlockSize), (int)std::floor(z / s_BlockSize) };
	}

	// The highest terrain under a cell's footprint, sampled at the corners and
	// the middle rather than at the centre alone. A block placed by its centre
	// height sinks into the uphill half of any slope, and a static box buried in
	// the ground is something the character walks up to and stops at for no
	// visible reason.
	//
	// **Nine samples give the exact maximum here, and that is a property of the
	// numbers rather than a good guess.** A block is 1 m and the terrain's cells
	// are 0.5 m, on aligned grids, so the footprint's corners, edge midpoints
	// and centre are all lattice points, and its boundary runs along triangle
	// edges. The surface is planar within each triangle, so its maximum over the
	// footprint has to be attained at one of those nine -- measured against a
	// 9x9 sweep, the error was 0.000000 m over sixty blocks. Change `s_BlockSize`
	// to something that is not two terrain cells and this quietly becomes an
	// approximation again.
	float TerrainTopIn(const glm::ivec2& cell) const
	{
		glm::vec2 centre = CentreOf(cell);
		float half = s_BlockSize * 0.5f;

		float top = -std::numeric_limits<float>::max();
		for (float dz : { -half, 0.0f, half })
		{
			for (float dx : { -half, 0.0f, half })
				top = std::max(top, m_Terrain.HeightAt(centre.x + dx, centre.y + dz));
		}

		return top;
	}

	// What a new block in this cell would rest on: the ground, or the top of
	// whatever is already stacked there.
	float ColumnTop(const glm::ivec2& cell) const
	{
		float top = TerrainTopIn(cell);
		for (const Block& block : m_Blocks)
		{
			if (block.Cell == cell)
				top = std::max(top, block.Base + s_BlockSize);
		}
		return top;
	}

	glm::vec3 BlockCentre(const Block& block) const
	{
		glm::vec2 centre = CentreOf(block.Cell);
		return { centre.x, block.Base + s_BlockSize * 0.5f, centre.y };
	}

	// A mouse position is a line through the world. The same unprojection as
	// Cube3D's ScreenRay: the near and far plane points that project to this
	// pixel, which the perspective divide makes different.
	void ScreenRay(glm::vec3& outOrigin, glm::vec3& outDirection) const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();
		std::pair<float, float> mouse = Egss::Input::GetMousePosition();

		float x = (mouse.first / (float)window.GetWidth()) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.second / (float)window.GetHeight()) * 2.0f;

		glm::mat4 inverse = glm::inverse(m_Camera.GetViewProjectionMatrix());

		glm::vec4 nearPoint = inverse * glm::vec4(x, y, -1.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);

		nearPoint /= nearPoint.w;
		farPoint /= farPoint.w;

		outOrigin = glm::vec3(nearPoint);
		outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
	}

	void UpdateAim()
	{
		m_Aim = Aim();

		if (ImGui::GetIO().WantCaptureMouse || m_FreeCamera)
			return;

		glm::vec3 origin(0.0f), direction(0.0f);
		ScreenRay(origin, direction);

		m_Aim = AimAlong(origin, direction);
	}

	// Where a ray would put a block. Separate from the mouse on purpose: this is
	// the part with the geometry in it, and it can be asked a question without a
	// window, a cursor or a person.
	Aim AimAlong(const glm::vec3& origin, const glm::vec3& direction) const
	{
		Aim aim;

		// The ground first, then everything already built; nearest wins. A block
		// is axis-aligned and never turned, so `AgainstAabb` is the whole of its
		// ray test -- the slab method the engine already had.
		float nearest = m_Reach;
		glm::vec3 point(0.0f), normal(0.0f);
		bool found = false;

		float distance = 0.0f;
		glm::vec3 hitPoint(0.0f), hitNormal(0.0f);
		if (m_Terrain.Field()->Raycast(origin, direction, nearest,
			distance, hitPoint, hitNormal))
		{
			nearest = distance;
			point = hitPoint;
			normal = hitNormal;
			found = true;
		}

		for (const Block& block : m_Blocks)
		{
			glm::vec3 centre = BlockCentre(block);

			Egss::Aabb box;
			box.Min = centre - glm::vec3(s_BlockSize * 0.5f);
			box.Max = centre + glm::vec3(s_BlockSize * 0.5f);

			float t = 0.0f;
			glm::vec3 boxNormal(0.0f);
			if (!Egss::Raycast3D::AgainstAabb(origin, direction, nearest, box, t, boxNormal))
				continue;

			if (t > nearest)
				continue;

			nearest = t;
			point = origin + direction * t;
			normal = boxNormal;
			found = true;
		}

		if (!found)
			return aim;

		// Stepped off the surface along its normal before asking which cell this
		// is. Pointing at the top of a block lands back in the same cell and
		// stacks; pointing at a side lands in the next one along and builds
		// outwards. Without the nudge the hit sits exactly on a cell boundary
		// and which side of it wins is a rounding accident.
		glm::vec3 seed = point + normal * (s_BlockSize * 0.25f);

		glm::ivec2 cell = CellAt(seed.x, seed.z);
		glm::vec2 centre = CentreOf(cell);

		// Off the edge of the map there is nothing to build on, in the same
		// sense that there is nothing to walk on.
		if (!m_Terrain.Contains(centre.x, centre.y))
			return aim;

		aim.Valid = true;
		aim.Cell = cell;
		aim.Centre = { centre.x, ColumnTop(cell) + s_BlockSize * 0.5f, centre.y };
		return aim;
	}

	void AddBlockBody(const Block& block)
	{
		Egss::RigidBody3D body = Egss::RigidBody3D::MakeStaticBox(
			BlockCentre(block), glm::vec3(s_BlockSize * 0.5f));

		body.Friction = 0.7f;
		body.Restitution = 0.0f;

		m_World.AddBody(body);
	}

	void PlaceBlock()
	{
		if (!m_Aim.Valid || (int)m_Blocks.size() >= m_MaxBlocks)
			return;

		Block block;
		block.Cell = m_Aim.Cell;
		block.Base = m_Aim.Centre.y - s_BlockSize * 0.5f;

		m_Blocks.push_back(block);
		AddBlockBody(block);
	}

	// The whole scene, then everything built on it. Used wherever the base class
	// would have called BuildScene alone -- a rebuild that forgets the blocks is
	// a rebuild that deletes the demo's point.
	void RebuildScene()
	{
		BuildScene();

		for (const Block& block : m_Blocks)
			AddBlockBody(block);
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

		// The block that would be placed. Drawn a little under size so it never
		// z-fights with the ground or with the block it is sitting on -- two
		// coplanar faces at the same depth flicker between them per pixel, and
		// it reads as the preview being broken rather than as a depth tie.
		// Not under --hide-ui: it follows the mouse, so a capture that includes
		// it is a capture of where the cursor was, and two runs stop matching.
		if (m_Aim.Valid && !Egss::Application::Get().IsUIHidden())
		{
			m_SceneMaterial->Set("u_Color", glm::vec4(0.95f, 0.85f, 0.35f, 1.0f));
			Egss::Renderer::Submit(m_SceneMaterial, m_Cube,
				glm::scale(glm::translate(glm::mat4(1.0f), m_Aim.Centre),
					glm::vec3(s_BlockSize * 0.96f)));
		}
	}

	// --- Panel --------------------------------------------------------------

	void OnDemoImGui() override
	{
		ImGui::Begin("Map");

		ImGui::Text("Left click builds   backspace undoes   delete clears");
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

		// What actually stops him, which is **not** friction. This line used to
		// read "he slides above atan(0.7) = 35 deg", and that is a true fact
		// about a *dynamic box* on this surface and a false one about him: his
		// root is kinematic, so the tangential force a slope needs is never
		// asked of a contact and the friction cone never binds. Measured on a
		// constant-slope ramp, he walks up 40 degrees at a full 1.30 m/s with
		// zero slip, where the box would have been sliding five degrees back.
		//
		// The real limit is that the hips may only rise at m_ClimbRate, so a
		// *sustained* climb needs speed * tan(theta) <= m_ClimbRate. Measured
		// against that: walking survives 42 deg and caps at 43, running
		// survives 16 and caps at 17, both within a degree of the formula. Past
		// it the hips fall behind, the pelvis sinks about a metre over ten
		// seconds, and once it dips under the surface the ground probe finds
		// nothing below it and he goes down.
		ImGui::Text("Sustained climb: %.1f deg walking, %.1f deg running",
			glm::degrees(std::atan(m_ClimbRate / std::max(m_WalkSpeed, 0.01f))),
			glm::degrees(std::atan(m_ClimbRate / std::max(m_RunSpeed, 0.01f))));

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

			// Undo, and clear. Both go through a rebuild rather than taking a
			// body out of the world, for the reason on the Block record: a
			// handle is an index, and the ragdoll's joints hold twelve of them.
			// The character is put back on his feet as a side effect, which is
			// the visible price of not having RemoveBody.
			if (key.GetKeyCode() == EGSS_KEY_BACKSPACE && !m_Blocks.empty())
			{
				m_Blocks.pop_back();
				RebuildScene();
				return true;
			}
			if (key.GetKeyCode() == EGSS_KEY_DELETE && !m_Blocks.empty())
			{
				m_Blocks.clear();
				RebuildScene();
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

		// Aimed and placed on the simulation's clock, not on the frame's. The
		// mouse is in the replay stream, so a building session records and plays
		// back exactly; an event handler placing a block on arrival would not,
		// because events are re-synthesised from the snapshot rather than
		// replayed themselves. Edge-detected here for the same reason -- holding
		// the button down should place one block, not sixty a second.
		UpdateAim();

		bool placing = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT)
			&& !ImGui::GetIO().WantCaptureMouse;

		if (placing && !m_WasPlacing)
			PlaceBlock();

		m_WasPlacing = placing;

		// Off the edge of the map is a real fall, not a clamped ledge -- the
		// field reports no ground outside its extent on purpose. Something has
		// to end it, though, or walking west for twenty seconds leaves the demo
		// watching a figure accelerate downwards for ever.
		if (m_World.GetBody(m_Pelvis).Position.y < m_Terrain.Lowest() - 20.0f)
			RebuildScene();
	}

	// --- Rebuilding ---------------------------------------------------------

	void Regenerate()
	{
		m_Terrain.Generate();
		BuildTerrainMesh();

		// Anything built on the old map is dropped with it. A block remembers
		// the height it was resting at, and the ground that height came from no
		// longer exists -- keeping them would leave a row of cubes hanging over
		// a valley that was a ridge a moment ago.
		m_Blocks.clear();

		// The whole scene, not just the ground: the character is standing on a
		// map that no longer exists, and the heightfield body holds a pointer
		// to the old field. Rebuilding is also what resets the rig, which the
		// base class already gets right.
		RebuildScene();
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

	std::vector<Block> m_Blocks;
	Aim m_Aim;
	bool m_WasPlacing = false;

	// How far you can build from the camera, and how much of it there can be.
	// The reach is a real limit rather than a tidy one: past it a block is a
	// couple of pixels and placing it is guesswork.
	float m_Reach = 40.0f;
	int m_MaxBlocks = 400;

	Terrain m_Terrain;
	std::shared_ptr<Egss::Mesh> m_TerrainMesh;
	std::shared_ptr<Egss::Shader> m_Shader;
	size_t m_TriangleCount = 0;
};
