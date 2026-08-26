#pragma once

#include <Egss.h>

// A static portal to a small, empty room. The portal itself -- a doorway
// frame near the lander -- shows a live rendered view of the room, the way
// a real window would, and walking through it teleports you in; walking
// back through the room's own doorway teleports you out.
//
// **Deliberately not placeable yet, and the room is deliberately empty.**
// Both are the next milestones. `Place()` already takes the portal's
// position and facing as parameters rather than hardcoding them, so making
// the portal a carried, placeable tool later only changes the caller here,
// not this class -- and stocking the room is additive physics bodies dropped
// at `m_RoomLocal`, the same way SolarSystem already scatters rocks.
//
// **The room's own physics, not the planet's.** A pocket dimension has no
// business asking a voxel field what the ground looks like, so it gets five
// static box colliders and a fixed "down" instead of the radial one every
// other surface here uses -- see the note on SpinAxis-style up-swapping in
// SolarSystem::ApplyGravity and ::UpdateSurface, which is the only other
// place "up" means anything other than "toward the planet's centre".
class PocketDimension
{
public:
	bool Valid() const { return m_Built; }
	bool InPocket() const { return m_InPocket; }

	// Once, from OnDemoAttach. `genericMaterial` is SolarSystem's own
	// rock/ship material (u_Color, u_LightPosition, u_LightColor, u_Sky,
	// u_Up) -- the room's slabs are lit the same simple way, just with their
	// own fixed light rather than the sun, since a pocket dimension is not
	// necessarily under one.
	void Build(const std::shared_ptr<Egss::Material>& genericMaterial,
		unsigned int windowSize = 512)
	{
		BuildSlabs();

		m_RoomMaterial = genericMaterial;

		Egss::MeshData quad;
		quad.Vertices = {
			{ {-1.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },
			{ { 1.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f} },
			{ { 1.0f, 0.0f,  1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} },
			{ {-1.0f, 0.0f,  1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} },
		};
		quad.Indices = { 0, 1, 2, 2, 3, 0 };
		quad.Submeshes.push_back({ "", -1, 0, (unsigned int)quad.Indices.size() });
		m_WindowQuad.reset(new Egss::Mesh(quad, "PortalWindow"));

		m_UnitCube.reset(Egss::Mesh::CreateCube(1.0f));

		BuildWindowShader();

		Egss::FramebufferSpecification spec;
		spec.Width = windowSize;
		spec.Height = windowSize;
		spec.Attachments = { Egss::FramebufferTextureFormat::RGBA8,
			Egss::FramebufferTextureFormat::DEPTH24STENCIL8 };
		m_Framebuffer.reset(Egss::Framebuffer::Create(spec));

		m_WindowTexture.reset(Egss::Texture2D::CreateFromHandle(
			m_Framebuffer->GetColorAttachmentRendererID(), windowSize, windowSize));

		m_InteriorCamera.SetProjection(70.0f, 1.0f, 0.1f, 50.0f);

		m_Built = true;
	}

	// Once per landing, from BuildSurfaceWorld, right after the player and
	// ship are added. `portalLocal`/`portalFacing` are the doorway you
	// actually walk through, in the site's local frame; `siteUp` is the
	// landing site's own up at the moment of landing, captured once rather
	// than recomputed, which is what makes the room's "down" fixed instead
	// of radial. The room itself sits far enough away, along its own facing,
	// that it can never overlap real terrain, the ship, or the rocks.
	void Place(Egss::PhysicsWorld3D& world, const glm::vec3& portalLocal,
		const glm::vec3& portalFacing, const glm::vec3& siteUp)
	{
		m_PortalLocal = portalLocal;
		m_Right = glm::normalize(glm::cross(siteUp, portalFacing));
		m_Up = siteUp;
		m_Forward = glm::normalize(portalFacing - siteUp * glm::dot(portalFacing, siteUp));

		// The room shares the portal's exact basis, just anchored somewhere
		// else -- so a lateral/height offset measured crossing one doorway
		// means the same thing crossing the other, and a velocity carries
		// straight over with no remapping.
		m_RoomLocal = portalLocal + m_Forward * 2000.0f;

		m_InPocket = false;
		m_HasPrevPosition = false;

		for (const Slab& slab : m_Slabs)
		{
			glm::vec3 centre = m_RoomLocal + m_Right * slab.Centre.x
				+ m_Up * slab.Centre.y + m_Forward * slab.Centre.z;

			Egss::RigidBody3D box = Egss::RigidBody3D::MakeStaticBox(centre, slab.HalfExtents);
			box.Orientation = glm::quat_cast(glm::mat3(m_Right, m_Up, m_Forward));

			world.AddBody(box);
		}
	}

	// Fixed step, from UpdateSurface after m_World.Step(dt).
	void UpdateCrossing(Egss::RigidBody3D& player)
	{
		if (!m_Built)
			return;

		glm::vec3 curr = player.Position;

		if (!m_HasPrevPosition)
		{
			m_PrevPlayerLocal = curr;
			m_HasPrevPosition = true;
			return;
		}

		float lateral, height;

		if (!m_InPocket)
		{
			if (CrossedPlane(m_PrevPlayerLocal, curr, m_PortalLocal, m_Forward,
				lateral, height))
			{
				player.Position = m_RoomLocal + m_Right * lateral + m_Up * height
					+ m_Forward * 1.0f;
				m_InPocket = true;
			}
		}
		else
		{
			if (CrossedPlane(m_PrevPlayerLocal, curr, m_RoomLocal, -m_Forward,
				lateral, height))
			{
				player.Position = m_PortalLocal + m_Right * lateral + m_Up * height
					- m_Forward * 1.0f;
				m_InPocket = false;
			}
		}

		m_PrevPlayerLocal = player.Position;
	}

	// **Raw site-local offsets, not scene positions.** `m_PortalLocal` and
	// `m_RoomLocal` are small next to `m_SiteFixed` (10 m and 2000 m against
	// a planet radius), and adding them to it -- to get a real position to
	// rotate into scene coordinates -- has to happen in double before
	// anything is cast to float, the same reason `BodyPlacement` returns its
	// centre in double. This class does not have `m_SiteFixed`, so it hands
	// the raw offset back and lets SolarSystem, which does, finish the sum.
	const glm::vec3& PortalLocal() const { return m_PortalLocal; }
	const glm::vec3& RoomLocal() const { return m_RoomLocal; }
	const glm::vec3& Right() const { return m_Right; }
	const glm::vec3& Forward() const { return m_Forward; }
	glm::vec3 Up() const { return m_Up; }

	// A flat floor at room-local y = 0, which is the only "ground" a pocket
	// dimension has.
	float HeightAboveFloor(const glm::vec3& siteLocalPlayerPos) const
	{
		return glm::dot(siteLocalPlayerPos - m_RoomLocal, m_Up);
	}

	// Once a frame, before the main scene's BeginScene -- the offscreen pass
	// needs its own Framebuffer bound, which cannot nest inside the window's.
	// Skipped whenever nothing would show it: no point painting a texture
	// nobody is about to sample.
	void RenderRoomToTexture()
	{
		if (!m_Built || m_InPocket)
			return;

		m_InteriorCamera.SetPosition(glm::vec3(0.0f, 1.6f, 0.5f));
		m_InteriorCamera.SetOrientation(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));

		m_Framebuffer->Bind();

		Egss::RenderCommand::SetClearColor({ 0.03f, 0.03f, 0.05f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::BeginScene(m_InteriorCamera);
		DrawShellLocal();
		Egss::Renderer::EndScene();

		m_Framebuffer->Unbind();

		Egss::Window& window = Egss::Application::Get().GetWindow();
		Egss::RenderCommand::SetViewport(0, 0, window.GetWidth(), window.GetHeight());
	}

	// The doorway, from outside: a quad showing whatever RenderRoomToTexture
	// just painted. `at`/`right`/`up`/`forward` are already fully resolved,
	// camera-relative scene vectors -- SolarSystem computes them (it has
	// `SiteFixed`/`ToScene`, this class does not) the same way it already
	// does for DrawShip and DrawRocks, so the portal turns with the planet
	// the same way they do.
	void DrawWindow(const glm::vec3& at, const glm::vec3& right,
		const glm::vec3& up, const glm::vec3& forward)
	{
		if (!m_Built || m_InPocket)
			return;

		auto material = Egss::Material::CreateInstance(m_WindowMaterial);
		material->SetTexture("u_Window", m_WindowTexture, 0);

		glm::mat4 transform(
			glm::vec4(right * s_DoorHalfWidth, 0.0f),
			glm::vec4(forward, 0.0f),
			glm::vec4(up * s_DoorHalfHeight, 0.0f),
			glm::vec4(at + up * s_DoorHalfHeight, 1.0f));

		Egss::Renderer::Submit(material, m_WindowQuad, transform);
	}

	// The room, from inside: the same five slabs RenderRoomToTexture just
	// drew into the offscreen camera's view, this time placed in the real
	// scene for the main camera. Same resolved-vector convention as
	// DrawWindow, `at` here being the room's own origin rather than the
	// portal's.
	void DrawInterior(const glm::vec3& at, const glm::vec3& right,
		const glm::vec3& up, const glm::vec3& forward)
	{
		if (!m_Built || !m_InPocket)
			return;

		auto material = Egss::Material::CreateInstance(m_RoomMaterial);
		material->Set("u_Color", glm::vec4(0.55f, 0.55f, 0.6f, 1.0f));
		material->Set("u_Emissive", 0.0f);
		material->Set("u_LightPosition", glm::vec3(0.3f, 1.0f, 0.2f) * 40.0f);
		material->Set("u_LightColor", glm::vec3(1.0f, 0.95f, 0.85f));
		material->Set("u_Sky", glm::vec3(0.12f, 0.12f, 0.16f));
		material->Set("u_Up", m_Up);

		for (const Slab& slab : m_Slabs)
		{
			glm::vec3 slabCentre = at + right * slab.Centre.x
				+ up * slab.Centre.y + forward * slab.Centre.z;

			glm::mat4 transform(
				glm::vec4(right * slab.HalfExtents.x * 2.0f, 0.0f),
				glm::vec4(up * slab.HalfExtents.y * 2.0f, 0.0f),
				glm::vec4(forward * slab.HalfExtents.z * 2.0f, 0.0f),
				glm::vec4(slabCentre, 1.0f));

			Egss::Renderer::Submit(material, m_UnitCube, transform);
		}
	}

private:
	struct Slab { glm::vec3 Centre; glm::vec3 HalfExtents; };

	// Five slabs in the room's own canonical axes (x = right, y = up,
	// z = forward, forward = 0 at the doorway) -- the sixth face, the
	// doorway itself, is simply not built. `Place`/`DrawInterior` turn these
	// into world positions by combining with whatever basis is current;
	// `DrawShellLocal` uses them directly as the offscreen pass's own axes.
	void BuildSlabs()
	{
		float w = s_RoomWidth, d = s_RoomDepth, h = s_RoomHeight, t = s_WallThickness;

		m_Slabs = {
			{ { 0.0f, -t * 0.5f, d * 0.5f }, { w * 0.5f, t * 0.5f, d * 0.5f } },       // floor
			{ { 0.0f, h + t * 0.5f, d * 0.5f }, { w * 0.5f, t * 0.5f, d * 0.5f } },    // ceiling
			{ { 0.0f, h * 0.5f, d + t * 0.5f }, { w * 0.5f, h * 0.5f, t * 0.5f } },    // back wall
			{ { -(w * 0.5f + t * 0.5f), h * 0.5f, d * 0.5f }, { t * 0.5f, h * 0.5f, d * 0.5f } }, // left
			{ { (w * 0.5f + t * 0.5f), h * 0.5f, d * 0.5f }, { t * 0.5f, h * 0.5f, d * 0.5f } },  // right
		};
	}

	// The offscreen pass's own self-contained scene: the room's canonical
	// axes stand in for world axes directly, since nothing outside the room
	// is ever visible in this render. No floating-origin arithmetic needed
	// at this scale -- the room is six metres across.
	void DrawShellLocal()
	{
		auto material = Egss::Material::CreateInstance(m_RoomMaterial);
		material->Set("u_Color", glm::vec4(0.55f, 0.55f, 0.6f, 1.0f));
		material->Set("u_Emissive", 0.0f);
		material->Set("u_LightPosition", glm::vec3(0.3f, 1.0f, 0.2f) * 40.0f);
		material->Set("u_LightColor", glm::vec3(1.0f, 0.95f, 0.85f));
		material->Set("u_Sky", glm::vec3(0.12f, 0.12f, 0.16f));
		material->Set("u_Up", glm::vec3(0.0f, 1.0f, 0.0f));

		for (const Slab& slab : m_Slabs)
		{
			glm::mat4 transform = glm::translate(glm::mat4(1.0f), slab.Centre)
				* glm::scale(glm::mat4(1.0f), slab.HalfExtents * 2.0f);

			Egss::Renderer::Submit(material, m_UnitCube, transform);
		}
	}

	// Whether the player crossed `planeLocal`'s plane moving in `forward`,
	// and if so, where on it -- the lateral/height offset a teleport carries
	// across so walking through slightly left of centre comes out slightly
	// left of centre on the other side. `right`/`up` are always this
	// object's own m_Right/m_Up: only the forward sign differs between the
	// entry check and the exit one, which is passed `-m_Forward` so both
	// share the same "was behind the plane, now at or past it" test.
	bool CrossedPlane(const glm::vec3& prevLocal, const glm::vec3& currLocal,
		const glm::vec3& planeLocal, const glm::vec3& forward,
		float& outLateral, float& outHeight) const
	{
		float prevF = glm::dot(prevLocal - planeLocal, forward);
		float currF = glm::dot(currLocal - planeLocal, forward);

		if (!(prevF < 0.0f && currF >= 0.0f))
			return false;

		glm::vec3 offset = currLocal - planeLocal;

		outLateral = glm::dot(offset, m_Right);
		outHeight = glm::dot(offset, m_Up);

		return std::abs(outLateral) <= s_DoorHalfWidth
			&& outHeight >= 0.0f && outHeight <= 2.0f * s_DoorHalfHeight;
	}

	void BuildWindowShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec2 v_TexCoord;

			void main()
			{
				v_TexCoord = a_TexCoord;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec2 v_TexCoord;

			uniform sampler2D u_Window;

			void main()
			{
				color = vec4(texture(u_Window, v_TexCoord).rgb, 1.0);
			}
		)";

		m_WindowShader.reset(Egss::Shader::Create("PortalWindow", vertexSrc, fragmentSrc));
		m_WindowMaterial = Egss::Material::Create(m_WindowShader);
	}

	std::vector<Slab> m_Slabs;

	std::shared_ptr<Egss::Mesh> m_UnitCube;
	std::shared_ptr<Egss::Mesh> m_WindowQuad;
	std::shared_ptr<Egss::Shader> m_WindowShader;
	std::shared_ptr<Egss::Material> m_WindowMaterial;
	std::shared_ptr<Egss::Material> m_RoomMaterial;
	std::shared_ptr<Egss::Framebuffer> m_Framebuffer;
	std::shared_ptr<Egss::Texture2D> m_WindowTexture;
	Egss::PerspectiveCamera m_InteriorCamera{ 70.0f, 1.0f, 0.1f, 50.0f };

	glm::vec3 m_PortalLocal{ 0.0f };
	glm::vec3 m_RoomLocal{ 0.0f };
	glm::vec3 m_Right{ 1.0f, 0.0f, 0.0f };
	glm::vec3 m_Up{ 0.0f, 1.0f, 0.0f };
	glm::vec3 m_Forward{ 0.0f, 0.0f, 1.0f };

	glm::vec3 m_PrevPlayerLocal{ 0.0f };
	bool m_HasPrevPosition = false;

	bool m_InPocket = false;
	bool m_Built = false;

	static constexpr float s_RoomWidth = 6.0f;
	static constexpr float s_RoomDepth = 6.0f;
	static constexpr float s_RoomHeight = 3.0f;
	static constexpr float s_WallThickness = 0.3f;
	static constexpr float s_DoorHalfWidth = 1.0f;
	static constexpr float s_DoorHalfHeight = 1.1f;
};
