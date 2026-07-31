#include <Egss.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

// A hand-built 4x4 sprite atlas. Generated rather than loaded so the sandbox
// has no asset dependency -- Texture2D::Create(path) handles real image files.
static constexpr unsigned int s_CellSize = 16;
static constexpr unsigned int s_AtlasCells = 4;
static constexpr unsigned int s_AtlasSize = s_CellSize * s_AtlasCells;

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
	}

	void OnUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))
			m_CameraPosition.x -= m_CameraMoveSpeed * ts;
		else if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT))
			m_CameraPosition.x += m_CameraMoveSpeed * ts;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))
			m_CameraPosition.y -= m_CameraMoveSpeed * ts;
		else if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))
			m_CameraPosition.y += m_CameraMoveSpeed * ts;

		m_Camera.SetPosition(m_CameraPosition);
		m_Rotation += ts * 45.0f;

		Egss::Renderer2D::ResetStats();

		Egss::RenderCommand::SetClearColor({ 0.08f, 0.08f, 0.1f, 1.0f });
		Egss::RenderCommand::Clear();

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

				float fx = (x - m_MapSize / 2.0f) * 0.13f;
				float fy = (y - m_MapSize / 2.0f) * 0.13f;
				Egss::Renderer2D::DrawQuad({ fx, fy }, { 0.125f, 0.125f }, m_Sprites[pick]);
			}
		}

		// A rotating sprite and the 2x1 wide sprite, both from the same atlas.
		Egss::Renderer2D::DrawRotatedQuad({ -1.15f, 0.0f, 0.1f }, { 0.4f, 0.4f },
			m_Rotation, m_Sprites[m_SelectedSprite]);
		Egss::Renderer2D::DrawQuad({ 1.15f, 0.0f, 0.1f }, { 0.6f, 0.3f }, m_WideSprite);

		Egss::Renderer2D::EndScene();
	}

	void OnImGuiRender() override
	{
		auto stats = Egss::Renderer2D::GetStats();

		ImGui::Begin("Renderer2D");
		ImGui::Text("Frame time: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Separator();
		ImGui::Text("Draw calls: %u", stats.DrawCalls);
		ImGui::Text("Quads:      %u", stats.QuadCount);
		ImGui::Text("Vertices:   %u", stats.GetTotalVertexCount());
		ImGui::Text("Indices:    %u", stats.GetTotalIndexCount());
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

	Egss::OrthographicCamera m_Camera;
	glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
	float m_CameraMoveSpeed = 2.0f;

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
