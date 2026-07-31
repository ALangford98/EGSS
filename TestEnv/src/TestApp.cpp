#include <Egss.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

// A hand-built 4x4 sprite atlas. Generated rather than loaded so the sandbox
// has no asset dependency -- Texture2D::Create(path) handles real image files.
static constexpr unsigned int s_CellSize = 16;
static constexpr unsigned int s_AtlasCells = 4;
static constexpr unsigned int s_AtlasSize = s_CellSize * s_AtlasCells;

// Picking IDs for the two non-tile sprites, kept above any tile index so they
// cannot collide with one.
static constexpr int s_SpinnerID = 1000000;
static constexpr int s_WideSpriteID = 1000001;

class Sandbox2D : public Egss::Layer
{
public:
	Sandbox2D()
		: Layer("Sandbox2D"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
	}

	void OnAttach() override
	{
		std::vector<unsigned int> pixels(s_AtlasSize * s_AtlasSize);

		// Each cell gets a distinct hue and motif, so it's obvious which
		// region a given sprite was cut from.
		const unsigned int cellColors[16] = {
			0xff4444dd, 0xff44dd44, 0xffdd4444, 0xffdddd44,
			0xffdd44dd, 0xff44dddd, 0xffdd8844, 0xff8844dd,
			0xff44dd88, 0xffdd4488, 0xff8888dd, 0xff88dd88,
			0xffdd8888, 0xffaaaaaa, 0xff666699, 0xffeeeeee
		};

		for (unsigned int cy = 0; cy < s_AtlasCells; cy++)
		{
			for (unsigned int cx = 0; cx < s_AtlasCells; cx++)
			{
				unsigned int fill = cellColors[cy * s_AtlasCells + cx];

				for (unsigned int y = 0; y < s_CellSize; y++)
				{
					for (unsigned int x = 0; x < s_CellSize; x++)
					{
						bool border = (x == 0 || y == 0 || x == s_CellSize - 1 || y == s_CellSize - 1);
						bool motif = ((x / 2 + y / 2) % 2) == 0;

						unsigned int color = border ? 0xff202020 : (motif ? fill : 0xff1a1a1a);

						unsigned int px = cx * s_CellSize + x;
						unsigned int py = cy * s_CellSize + y;
						pixels[py * s_AtlasSize + px] = color;
					}
				}
			}
		}

		m_Atlas.reset(Egss::Texture2D::Create(s_AtlasSize, s_AtlasSize));
		m_Atlas->SetData(pixels.data(), (unsigned int)(pixels.size() * sizeof(unsigned int)));

		// Cut all 16 cells out as individual sprites.
		for (unsigned int y = 0; y < s_AtlasCells; y++)
		{
			for (unsigned int x = 0; x < s_AtlasCells; x++)
			{
				m_Sprites.emplace_back(Egss::SubTexture2D::CreateFromCoords(
					m_Atlas, { (float)x, (float)y }, { (float)s_CellSize, (float)s_CellSize }));
			}
		}

		// A 2x1 sprite, showing spriteSize spanning more than one cell.
		m_WideSprite.reset(Egss::SubTexture2D::CreateFromCoords(
			m_Atlas, { 0.0f, 3.0f }, { (float)s_CellSize, (float)s_CellSize }, { 2.0f, 1.0f }));

		// Size is provisional -- the first frame resizes it to whatever the
		// viewport panel actually turns out to be. The RED_INTEGER attachment
		// is what picking reads back.
		Egss::FramebufferSpecification spec;
		spec.Width = 1280;
		spec.Height = 720;
		spec.Attachments = {
			Egss::FramebufferTextureFormat::RGBA8,
			Egss::FramebufferTextureFormat::RED_INTEGER,
			Egss::FramebufferTextureFormat::DEPTH24STENCIL8
		};
		m_Framebuffer.reset(Egss::Framebuffer::Create(spec));
	}

	void OnUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		// The panel size is only known after ImGui has laid it out, so this
		// acts on last frame's measurement. One frame of lag while dragging is
		// invisible, and it avoids resizing mid-frame with the target bound.
		const Egss::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();
		if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f &&
			((unsigned int)m_ViewportSize.x != spec.Width || (unsigned int)m_ViewportSize.y != spec.Height))
		{
			m_Framebuffer->Resize((unsigned int)m_ViewportSize.x, (unsigned int)m_ViewportSize.y);

			// Without this the scene stretches: the projection has to track
			// the panel's shape, not the window's.
			float aspect = m_ViewportSize.x / m_ViewportSize.y;
			m_Camera.SetProjection(-aspect * m_ZoomLevel, aspect * m_ZoomLevel, -m_ZoomLevel, m_ZoomLevel);
		}

		// Only drive the camera when the scene panel has focus, so arrow keys
		// typed into a widget don't also pan the world.
		if (m_ViewportFocused)
		{
			if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))
				m_CameraPosition.x -= m_CameraMoveSpeed * ts;
			else if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT))
				m_CameraPosition.x += m_CameraMoveSpeed * ts;

			if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))
				m_CameraPosition.y -= m_CameraMoveSpeed * ts;
			else if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))
				m_CameraPosition.y += m_CameraMoveSpeed * ts;
		}

		m_Camera.SetPosition(m_CameraPosition);
		m_Rotation += ts * 45.0f;

		Egss::Renderer2D::ResetStats();

		// Everything between Bind and Unbind lands in the framebuffer's
		// texture rather than the window.
		m_Framebuffer->Bind();

		Egss::RenderCommand::SetClearColor({ 0.08f, 0.08f, 0.1f, 1.0f });
		Egss::RenderCommand::Clear();

		// glClear only carries a float colour, so the integer attachment needs
		// clearing on its own. -1 is the "nothing here" ID.
		m_Framebuffer->ClearAttachment(1, -1);

		Egss::Renderer2D::BeginScene(m_Camera);

		// A tilemap. Every tile is a different sprite, but all 16 sprites come
		// from one atlas -- so this is one texture slot and one draw call, no
		// matter how many distinct tiles are on screen.
		for (int y = 0; y < m_MapSize; y++)
		{
			for (int x = 0; x < m_MapSize; x++)
			{
				// Deterministic pseudo-random tile choice.
				unsigned int pick = (unsigned int)((x * 7 + y * 13 + x * y * 3) % m_Sprites.size());

				int id = y * m_MapSize + x;

				// Uses last frame's pick result, so the highlight lags by a
				// frame. At 60fps that isn't perceptible.
				glm::vec4 tint = (id == m_HoveredEntity)
					? glm::vec4(2.0f, 2.0f, 2.0f, 1.0f)
					: glm::vec4(1.0f);

				float fx = (x - m_MapSize / 2.0f) * 0.13f;
				float fy = (y - m_MapSize / 2.0f) * 0.13f;
				Egss::Renderer2D::DrawQuad({ fx, fy }, { 0.125f, 0.125f }, m_Sprites[pick], 1.0f, tint, id);
			}
		}

		// A rotating sprite and the 2x1 wide sprite, both from the same atlas.
		// Their IDs sit above any tile index so they can't collide.
		Egss::Renderer2D::DrawRotatedQuad({ -1.15f, 0.0f, 0.1f }, { 0.4f, 0.4f },
			m_Rotation, m_Sprites[m_SelectedSprite], 1.0f, glm::vec4(1.0f), s_SpinnerID);
		Egss::Renderer2D::DrawQuad({ 1.15f, 0.0f, 0.1f }, { 0.6f, 0.3f }, m_WideSprite,
			1.0f, glm::vec4(1.0f), s_WideSpriteID);

		Egss::Renderer2D::EndScene();

		// Read back while the framebuffer is still bound. The mouse is in
		// window coordinates, so it has to be rebased onto the panel and
		// flipped, since GL's origin is bottom-left.
		auto [mouseX, mouseY] = Egss::Input::GetMousePosition();
		float localX = mouseX - m_ViewportBounds[0].x;
		float localY = m_ViewportSize.y - (mouseY - m_ViewportBounds[0].y);

		if (localX >= 0.0f && localY >= 0.0f && localX < m_ViewportSize.x && localY < m_ViewportSize.y)
			m_HoveredEntity = m_Framebuffer->ReadPixel(1, (int)localX, (int)localY);
		else
			m_HoveredEntity = -1;

		m_Framebuffer->Unbind();

		// The window itself is now only ever covered by ImGui, but it still
		// needs clearing -- otherwise the areas no panel covers keep whatever
		// was left there.
		Egss::RenderCommand::SetClearColor({ 0.05f, 0.05f, 0.06f, 1.0f });
		Egss::RenderCommand::Clear();
	}

	void OnImGuiRender() override
	{
		auto stats = Egss::Renderer2D::GetStats();

		// Without a starting size the window auto-fits its content, and its
		// only content is an image sized from the window -- so it collapses to
		// nothing and never recovers.
		ImGui::SetNextWindowSize(ImVec2(900.0f, 520.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(340.0f, 40.0f), ImGuiCond_FirstUseEver);

		// No padding, so the image sits flush against the panel border.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportFocused = ImGui::IsWindowFocused();

		ImVec2 available = ImGui::GetContentRegionAvail();
		m_ViewportSize = { available.x, available.y };

		// Where the image lands on screen, which is what the mouse position
		// has to be rebased onto. Taken before the image is drawn, since the
		// cursor is at its top-left corner at this point.
		ImVec2 imagePos = ImGui::GetCursorScreenPos();
		m_ViewportBounds[0] = { imagePos.x, imagePos.y };
		m_ViewportBounds[1] = { imagePos.x + available.x, imagePos.y + available.y };

		// UVs are flipped vertically: GL's origin is bottom-left, ImGui's is
		// top-left, so an unflipped image renders upside down.
		ImGui::Image((ImTextureID)(uintptr_t)m_Framebuffer->GetColorAttachmentRendererID(),
			available, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

		ImGui::End();
		ImGui::PopStyleVar();

		ImGui::Begin("Renderer2D");
		ImGui::Text("Frame time: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Separator();
		ImGui::Text("Draw calls: %u", stats.DrawCalls);
		ImGui::Text("Quads:      %u", stats.QuadCount);
		ImGui::Text("Vertices:   %u", stats.GetTotalVertexCount());
		ImGui::Text("Indices:    %u", stats.GetTotalIndexCount());
		ImGui::Separator();
		ImGui::Text("Viewport:   %ux%u", m_Framebuffer->GetSpecification().Width,
			m_Framebuffer->GetSpecification().Height);
		ImGui::Separator();
		ImGui::Separator();

		if (m_HoveredEntity == s_SpinnerID)
			ImGui::Text("Hovered:    spinner");
		else if (m_HoveredEntity == s_WideSpriteID)
			ImGui::Text("Hovered:    wide sprite");
		else if (m_HoveredEntity >= 0)
			ImGui::Text("Hovered:    tile %d  (%d, %d)", m_HoveredEntity,
				m_HoveredEntity % m_MapSize, m_HoveredEntity / m_MapSize);
		else
			ImGui::Text("Hovered:    -");

		ImGui::Separator();
		ImGui::Text("%zu sprites, all from one atlas", m_Sprites.size());
		ImGui::SliderInt("Map size", &m_MapSize, 1, 100);
		ImGui::Text("(%d tiles)", m_MapSize * m_MapSize);
		ImGui::SliderInt("Spinner sprite", &m_SelectedSprite, 0, (int)m_Sprites.size() - 1);
		ImGui::End();
	}
private:
	std::shared_ptr<Egss::Texture2D> m_Atlas;
	std::vector<std::shared_ptr<Egss::SubTexture2D>> m_Sprites;
	std::shared_ptr<Egss::SubTexture2D> m_WideSprite;

	std::shared_ptr<Egss::Framebuffer> m_Framebuffer;
	glm::vec2 m_ViewportSize = { 0.0f, 0.0f };
	// Top-left and bottom-right of the image in screen space.
	glm::vec2 m_ViewportBounds[2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
	bool m_ViewportFocused = false;
	int m_HoveredEntity = -1;

	Egss::OrthographicCamera m_Camera;
	glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
	float m_CameraMoveSpeed = 2.0f;
	float m_ZoomLevel = 0.9f;

	int m_MapSize = 20;
	int m_SelectedSprite = 0;
	float m_Rotation = 0.0f;
	float m_FrameTime = 0.0f;
};

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{
		PushLayer(new Sandbox2D());
	}
	~TestEnv()
	{

	}
};



Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}
