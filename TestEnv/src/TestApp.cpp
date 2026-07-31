// Breakout, as a worked example of building on EGSS.
//
// Read docs/ENGINE.md first for the shape of the engine; this is the same
// material from the other side. Everything here is game code -- no engine
// internals are touched, and every engine call is one of the dozen or so in
// that document's API list.
//
// The previous sandbox (framebuffer viewport panel + mouse picking) is at
// commit a282175 if you want the editor-style setup back.
//
// Things marked TRY: are deliberate places to experiment.

#include <Egss.h>

#include <imgui.h>

// ---------------------------------------------------------------------------
// World units, not pixels.
//
// The camera decides how much of the world fits on screen, so a paddle 0.36
// wide is 0.36 wide at any resolution. Only the camera cares about aspect
// ratio; nothing below does. Half-height is fixed at 0.9, and half-width
// follows from the window's shape.
// ---------------------------------------------------------------------------
static constexpr float s_WorldHalfHeight = 0.9f;

static constexpr float s_PaddleY = -0.75f;
static constexpr float s_PaddleSpeed = 2.2f;
static const glm::vec2 s_PaddleSize = { 0.36f, 0.045f };

static constexpr float s_BallRadius = 0.028f;
static constexpr float s_BallStartSpeed = 1.1f;

static constexpr int s_BrickCols = 9;
static constexpr int s_BrickRows = 5;
static const glm::vec2 s_BrickSize = { 0.26f, 0.09f };
static constexpr float s_BrickGap = 0.02f;
static constexpr float s_BrickTop = 0.78f;

struct Brick
{
	glm::vec2 Position;
	glm::vec4 Color;
	bool Alive = true;
};

// Axis-aligned overlap test. Both arguments are centre + full size, which is
// the same convention Renderer2D::DrawQuad uses -- keeping one convention
// everywhere is what stops collision code turning into off-by-half bugs.
static bool Overlaps(const glm::vec2& aCentre, const glm::vec2& aSize,
	const glm::vec2& bCentre, const glm::vec2& bSize)
{
	return std::abs(aCentre.x - bCentre.x) * 2.0f < (aSize.x + bSize.x)
		&& std::abs(aCentre.y - bCentre.y) * 2.0f < (aSize.y + bSize.y);
}

class Breakout : public Egss::Layer
{
public:
	Breakout()
		: Layer("Breakout"), m_Camera(-1.6f, 1.6f, -s_WorldHalfHeight, s_WorldHalfHeight)
	{
	}

	// Called once when the layer is pushed. Load assets and build state here,
	// never in the constructor -- at construction time there is no GL context
	// yet, so a Texture2D::Create would fail.
	void OnAttach() override
	{
		// A flat colour would not need a texture at all (Renderer2D keeps a
		// white one for that), so this builds a small vertical gradient to
		// show the Create(w,h) + SetData path. Swap it for
		// Texture2D::Create("assets/bricks.png") once you have real art.
		constexpr unsigned int size = 8;
		std::vector<unsigned int> pixels(size * size);
		for (unsigned int y = 0; y < size; y++)
		{
			for (unsigned int x = 0; x < size; x++)
			{
				// 0xAABBGGRR -- little-endian RGBA8.
				unsigned int shade = 235 - y * 14;
				pixels[y * size + x] = 0xff000000 | (shade << 16) | (shade << 8) | shade;
			}
		}

		m_BrickTexture.reset(Egss::Texture2D::Create(size, size));
		m_BrickTexture->SetData(pixels.data(), (unsigned int)(pixels.size() * sizeof(unsigned int)));

		Reset();
	}

	void Reset()
	{
		m_Bricks.clear();

		// TRY: give the top rows more hit points by adding a Health field to
		// Brick and only clearing Alive when it reaches zero.
		const glm::vec4 rowColors[s_BrickRows] = {
			{ 0.90f, 0.30f, 0.30f, 1.0f },
			{ 0.90f, 0.60f, 0.25f, 1.0f },
			{ 0.85f, 0.85f, 0.30f, 1.0f },
			{ 0.35f, 0.80f, 0.40f, 1.0f },
			{ 0.35f, 0.60f, 0.90f, 1.0f }
		};

		float pitchX = s_BrickSize.x + s_BrickGap;
		float rowWidth = s_BrickCols * pitchX - s_BrickGap;

		for (int row = 0; row < s_BrickRows; row++)
		{
			for (int col = 0; col < s_BrickCols; col++)
			{
				Brick brick;
				brick.Position = {
					-rowWidth * 0.5f + pitchX * 0.5f + col * pitchX,
					s_BrickTop - row * (s_BrickSize.y + s_BrickGap)
				};
				brick.Color = rowColors[row];
				m_Bricks.push_back(brick);
			}
		}

		m_PaddleX = 0.0f;
		m_Score = 0;
		m_Lives = 3;
		m_GameOver = false;
		LaunchBall();
	}

	void LaunchBall()
	{
		m_BallPosition = { m_PaddleX, s_PaddleY + 0.08f };
		m_BallVelocity = { s_BallStartSpeed * 0.45f, s_BallStartSpeed };
		m_BallStuck = true;
	}

	// The whole game. Called once per frame with the seconds elapsed since the
	// last one -- multiply every rate by it and the game runs the same on a
	// 60Hz and a 144Hz display.
	void OnUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		if (!m_Paused && !m_GameOver)
			Step(ts);

		Draw();
	}

	void Step(Egss::Timestep ts)
	{
		// --- Paddle ------------------------------------------------------
		// Polling, not events: this asks "is the key down right now", which is
		// what continuous movement wants. An event fires once per press.
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT) || Egss::Input::IsKeyPressed(EGSS_KEY_A))
			m_PaddleX -= s_PaddleSpeed * ts;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT) || Egss::Input::IsKeyPressed(EGSS_KEY_D))
			m_PaddleX += s_PaddleSpeed * ts;

		float limit = m_WorldHalfWidth - s_PaddleSize.x * 0.5f;
		m_PaddleX = std::max(-limit, std::min(limit, m_PaddleX));

		// The ball rides the paddle until launched. Launching is handled in
		// OnEvent, not here: polling only sees a key that is *still* down when
		// the frame runs, so a quick tap between two frames is missed entirely.
		// Continuous movement wants polling; one-shot actions want events.
		if (m_BallStuck)
		{
			m_BallPosition = { m_PaddleX, s_PaddleY + 0.08f };
			return;
		}

		// --- Ball --------------------------------------------------------
		m_BallPosition += m_BallVelocity * (float)ts;

		// Walls. Reflecting one velocity component is the whole of a bounce.
		// Snapping the position back to the wall first stops the ball getting
		// stuck jittering inside it on a slow frame.
		if (m_BallPosition.x - s_BallRadius < -m_WorldHalfWidth)
		{
			m_BallPosition.x = -m_WorldHalfWidth + s_BallRadius;
			m_BallVelocity.x = std::abs(m_BallVelocity.x);
		}
		else if (m_BallPosition.x + s_BallRadius > m_WorldHalfWidth)
		{
			m_BallPosition.x = m_WorldHalfWidth - s_BallRadius;
			m_BallVelocity.x = -std::abs(m_BallVelocity.x);
		}

		if (m_BallPosition.y + s_BallRadius > s_WorldHalfHeight)
		{
			m_BallPosition.y = s_WorldHalfHeight - s_BallRadius;
			m_BallVelocity.y = -std::abs(m_BallVelocity.y);
		}

		// --- Paddle collision ---------------------------------------------
		glm::vec2 ballSize = { s_BallRadius * 2.0f, s_BallRadius * 2.0f };
		glm::vec2 paddleCentre = { m_PaddleX, s_PaddleY };

		if (m_BallVelocity.y < 0.0f && Overlaps(m_BallPosition, ballSize, paddleCentre, s_PaddleSize))
		{
			m_BallVelocity.y = std::abs(m_BallVelocity.y);

			// Where it hit steers it: -1 at the left edge, +1 at the right.
			// This is the difference between Breakout and a physics toy -- the
			// player needs control over the angle.
			float offset = (m_BallPosition.x - m_PaddleX) / (s_PaddleSize.x * 0.5f);
			m_BallVelocity.x = offset * s_BallStartSpeed;

			// TRY: multiply the velocity by 1.02 here so rallies get harder.
		}

		// --- Bricks -------------------------------------------------------
		for (Brick& brick : m_Bricks)
		{
			if (!brick.Alive)
				continue;

			if (!Overlaps(m_BallPosition, ballSize, brick.Position, s_BrickSize))
				continue;

			brick.Alive = false;
			m_Score += 10;

			// Reflect on whichever axis was less overlapped -- the cheap way to
			// tell a side hit from a top hit. Good enough for Breakout; a real
			// physics pass would test the swept path instead of the end state.
			float overlapX = (s_BrickSize.x + ballSize.x) * 0.5f - std::abs(m_BallPosition.x - brick.Position.x);
			float overlapY = (s_BrickSize.y + ballSize.y) * 0.5f - std::abs(m_BallPosition.y - brick.Position.y);

			if (overlapY < overlapX)
				m_BallVelocity.y = -m_BallVelocity.y;
			else
				m_BallVelocity.x = -m_BallVelocity.x;

			break;  // one brick per frame keeps the bounce predictable
		}

		// --- Losing and winning -------------------------------------------
		if (m_BallPosition.y < -s_WorldHalfHeight - 0.1f)
		{
			if (--m_Lives <= 0)
				m_GameOver = true;
			else
				LaunchBall();
		}

		bool anyLeft = false;
		for (const Brick& brick : m_Bricks)
			anyLeft |= brick.Alive;

		if (!anyLeft)
			m_GameOver = true;
	}

	void Draw()
	{
		Egss::Renderer2D::ResetStats();

		Egss::RenderCommand::SetClearColor({ 0.07f, 0.07f, 0.09f, 1.0f });
		Egss::RenderCommand::Clear();

		// Everything between BeginScene and EndScene accumulates into one
		// vertex buffer. Nothing reaches the driver until EndScene flushes it,
		// which is why the whole board costs one draw call.
		Egss::Renderer2D::BeginScene(m_Camera);

		for (const Brick& brick : m_Bricks)
		{
			if (brick.Alive)
			{
				// Texture supplies the shading, the colour multiplies it. All
				// bricks share one texture, so they all land in one batch.
				Egss::Renderer2D::DrawQuad(brick.Position, s_BrickSize, m_BrickTexture, 1.0f, brick.Color);
			}
		}

		Egss::Renderer2D::DrawQuad({ m_PaddleX, s_PaddleY }, s_PaddleSize,
			glm::vec4(0.85f, 0.85f, 0.90f, 1.0f));

		Egss::Renderer2D::DrawQuad(m_BallPosition, { s_BallRadius * 2.0f, s_BallRadius * 2.0f },
			glm::vec4(1.00f, 0.95f, 0.60f, 1.0f));

		Egss::Renderer2D::EndScene();
	}

	// Events, unlike polling, fire once per change -- right for actions.
	void OnEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			// The only place aspect ratio matters. Half-height stays fixed, so
			// a wider window shows more world rather than stretching it.
			if (e.GetHeight() == 0)
				return false;

			m_WorldHalfWidth = ((float)e.GetWidth() / (float)e.GetHeight()) * s_WorldHalfHeight;
			m_Camera.SetProjection(-m_WorldHalfWidth, m_WorldHalfWidth,
				-s_WorldHalfHeight, s_WorldHalfHeight);

			return false;  // false = don't consume it; other layers still see it
		});

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& e)
		{
			// GetRepeatCount() > 0 means the OS auto-repeat is firing, which
			// you almost never want for an action.
			if (e.GetRepeatCount() > 0)
				return false;

			if (e.GetKeyCode() == EGSS_KEY_SPACE)
				m_BallStuck = false;
			if (e.GetKeyCode() == EGSS_KEY_R)
				Reset();
			if (e.GetKeyCode() == EGSS_KEY_P)
				m_Paused = !m_Paused;

			return false;
		});
	}

	// Debug UI. Runs every frame, after the game has drawn and before the
	// buffer swap.
	void OnImGuiRender() override
	{
		auto stats = Egss::Renderer2D::GetStats();

		ImGui::Begin("Breakout");

		ImGui::Text("Score: %d", m_Score);
		ImGui::Text("Lives: %d", m_Lives);

		if (m_GameOver)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Game over - R to restart");
		else if (m_BallStuck)
			ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Space to launch");
		else if (m_Paused)
			ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Paused");

		ImGui::Separator();
		ImGui::Text("Left/Right or A/D  move");
		ImGui::Text("Space              launch");
		ImGui::Text("P / R              pause / restart");

		ImGui::Separator();
		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Text("Draw calls: %u", stats.DrawCalls);
		ImGui::Text("Quads:      %u", stats.QuadCount);

		ImGui::Separator();
		if (ImGui::Button("Restart"))
			Reset();

		ImGui::End();
	}

private:
	Egss::OrthographicCamera m_Camera;
	float m_WorldHalfWidth = 1.6f;

	std::shared_ptr<Egss::Texture2D> m_BrickTexture;
	std::vector<Brick> m_Bricks;

	float m_PaddleX = 0.0f;
	glm::vec2 m_BallPosition = { 0.0f, 0.0f };
	glm::vec2 m_BallVelocity = { 0.0f, 0.0f };
	bool m_BallStuck = true;

	int m_Score = 0;
	int m_Lives = 3;
	bool m_GameOver = false;
	bool m_Paused = false;

	float m_FrameTime = 0.0f;
};

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{
		PushLayer(new Breakout());
	}
};

// The one function the engine requires of you.
Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}
