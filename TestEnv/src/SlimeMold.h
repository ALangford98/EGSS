#pragma once

// Physarum: a slime mould, as a few hundred thousand agents and a trail map.
//
// The whole algorithm is two lines of idea. Each agent smells the trail ahead
// of it at three points -- left, centre, right -- and turns toward the
// strongest. Then it moves forward and leaves a little trail behind it. That
// is all. Everything the colony does -- the veins, the branching, the way two
// strands find each other and merge -- comes out of those two rules plus
// diffusion, and none of it is written down anywhere in this file.
//
// The trail map is the only shared state, and it is what makes the agents a
// colony rather than a crowd: an agent cannot see another agent, only where one
// has been. Sensing *ahead* rather than at its own position is what closes the
// loop -- an agent that sampled where it already stood would only ever smell
// itself.
//
// **All of it runs on the fixed step**, like everything else here, so the demo
// records and replays exactly. That costs something real: the simulation is
// single-threaded, because several threads depositing into one trail map add
// their floats in whatever order they arrive and the run stops being
// reproducible. Measured below; it is not the bottleneck anyway.
//
// The controls are the three things a pointer can usefully do to a colony:
//
//   **left button**   attract -- agents within the radius steer toward you
//   **right button**  repel   -- they steer away, carving a channel
//   **middle button** feed    -- paints trail, which the colony grows into
//
// Attract and repel are a bias added to the sensing decision rather than a
// force added to the position: pushing agents directly moves them without
// changing what they *want*, and the colony snaps back the moment you stop.
// Biasing the turn makes the colony reorganise around you, which is the thing
// worth watching.

#include <Egss.h>
#include <imgui.h>

#include "Demo.h"

class SlimeMold : public DemoLayer
{
public:
	SlimeMold()
		: DemoLayer("SlimeMold"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
		// Everything a slider can move that the simulation then reads. A
		// recording samples these per fixed step, so a session where the
		// sensor angle was dragged replays as itself.
		RegisterParam("MoveSpeed", &m_MoveSpeed);
		RegisterParam("TurnSpeed", &m_TurnSpeed);
		RegisterParam("SensorAngle", &m_SensorAngle);
		RegisterParam("SensorDistance", &m_SensorDistance);
		RegisterParam("Deposit", &m_Deposit);
		RegisterParam("Decay", &m_Decay);
		RegisterParam("Diffuse", &m_Diffuse);
		RegisterParam("PointerRadius", &m_PointerRadius);
		RegisterParam("PointerStrength", &m_PointerStrength);
		RegisterParam("AgentCount", &m_AgentCount);
	}

	void OnDemoAttach() override
	{
		m_Trail.assign((size_t)s_Width * s_Height, 0.0f);
		m_Next.assign((size_t)s_Width * s_Height, 0.0f);
		m_Pixels.assign((size_t)s_Width * s_Height, 0u);

		m_Texture.reset(Egss::Texture2D::Create(s_Width, s_Height));

		BuildPalette();

		Reset();
	}

	// --- The simulation -----------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		float dt = step;

		// Sampled here rather than from an event, for the same reason Map
		// Building samples its clicks here: the mouse is in the replay stream
		// and events are not.
		ReadPointer();

		SenseAndMove(dt);
		DiffuseAndDecay(dt);

		m_Steps++;
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		Colourise();
		m_Texture->SetData(m_Pixels.data(),
			(unsigned int)(m_Pixels.size() * sizeof(unsigned int)));

		Egss::RenderCommand::SetClearColor({ 0.02f, 0.02f, 0.04f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer2D::BeginScene(m_Camera);
		Egss::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(3.2f, 1.8f), m_Texture);
		Egss::Renderer2D::EndScene();
	}

private:
	// A grid, not a texture the GPU writes: the agents read it as well as write
	// it, and reading back what a shader wrote is the one thing GPUs are bad at.
	// 16:9 so it fills the window without letterboxing, and so the wallpaper
	// version needs no second layout.
	static constexpr int s_Width = 512;
	static constexpr int s_Height = 288;

	struct Agent
	{
		float X = 0.0f, Y = 0.0f;
		float Angle = 0.0f;
	};

	// --- Determinism --------------------------------------------------------
	//
	// Its own generator, seeded and stepped explicitly, rather than rand() or a
	// <random> engine with global state. Two runs must produce the same colony
	// from the same seed, and anything shared with the rest of the process
	// cannot promise that.
	unsigned int NextRandom()
	{
		// xorshift32. Cheap, and its period is far longer than a run.
		m_Random ^= m_Random << 13;
		m_Random ^= m_Random >> 17;
		m_Random ^= m_Random << 5;
		return m_Random;
	}

	float RandomUnit() { return (float)(NextRandom() >> 8) * (1.0f / 16777216.0f); }

	void Reset()
	{
		m_Random = 0x9E3779B9u ^ (unsigned int)m_Seed;
		m_Steps = 0;

		std::fill(m_Trail.begin(), m_Trail.end(), 0.0f);

		// Started on a disc pointing outward rather than scattered uniformly.
		// A uniform start is the same picture everywhere and takes a long time
		// to organise; a disc has an edge, and the edge is where the first
		// veins form.
		m_Agents.resize((size_t)std::max(1, m_AgentCount));

		float radius = 0.35f * (float)s_Height;

		for (Agent& agent : m_Agents)
		{
			float angle = RandomUnit() * 6.2831853f;
			float distance = std::sqrt(RandomUnit()) * radius;

			agent.X = (float)s_Width * 0.5f + std::cos(angle) * distance;
			agent.Y = (float)s_Height * 0.5f + std::sin(angle) * distance;
			agent.Angle = angle;
		}
	}

	int Wrap(int value, int limit) const
	{
		// The field is a torus. A colony that piles up against a wall spends
		// its life there; wrapping keeps every part of the picture equally
		// interesting, which is what a wallpaper wants.
		value %= limit;
		return value < 0 ? value + limit : value;
	}

	// One add rather than a modulo, which is sound because every caller is at
	// most one field away from inside: a sensor reaches `m_SensorDistance`
	// (capped at 32 by the slider) and an agent moves less than a cell a step,
	// against a 512-wide field. `Wrap` is still there for the code that has no
	// such bound.
	static int Fold(int value, int limit)
	{
		if (value < 0)
			return value + limit;

		return value >= limit ? value - limit : value;
	}

	float SampleAt(float x, float y) const
	{
		return m_Trail[(size_t)Fold((int)y, s_Height) * s_Width + Fold((int)x, s_Width)];
	}

	void SenseAndMove(float dt)
	{
		float sensorAngle = glm::radians(m_SensorAngle);
		float turn = glm::radians(m_TurnSpeed) * dt;
		float speed = m_MoveSpeed * dt;

		// cos and sin of the sensor offset, once for every agent in the step.
		float cosOffset = std::cos(sensorAngle);
		float sinOffset = std::sin(sensorAngle);

		for (Agent& agent : m_Agents)
		{
			// **The three sensor directions from one sine and one cosine.**
			// cos(a +/- s) = cos a cos s -/+ sin a sin s, which turns six trig
			// calls per agent into two -- and at 80,000 agents a step, trig was
			// the simulation. The angle-addition identity is exact; the floats
			// it produces differ in the last bits from calling cos(a + s)
			// directly, which is a different colony from the same seed, not a
			// wrong one.
			float cosA = std::cos(agent.Angle);
			float sinA = std::sin(agent.Angle);

			float distance = m_SensorDistance;

			float centre = SampleAt(agent.X + cosA * distance, agent.Y + sinA * distance);

			float left = SampleAt(
				agent.X + (cosA * cosOffset - sinA * sinOffset) * distance,
				agent.Y + (sinA * cosOffset + cosA * sinOffset) * distance);

			float right = SampleAt(
				agent.X + (cosA * cosOffset + sinA * sinOffset) * distance,
				agent.Y + (sinA * cosOffset - cosA * sinOffset) * distance);

			// The four cases are the whole steering rule. "Both sides better
			// than the middle" is the interesting one: turning a *random* way
			// there is what breaks the symmetry, and replacing it with "always
			// turn left" makes the colony visibly handed.
			if (centre > left && centre > right)
			{
				// straight on
			}
			else if (centre < left && centre < right)
			{
				agent.Angle += (RandomUnit() < 0.5f ? -turn : turn);
			}
			else if (left > right)
			{
				agent.Angle += turn;
			}
			else if (right > left)
			{
				agent.Angle -= turn;
			}

			ApplyPointer(agent, turn);

			// Steering changed the heading, so the step forward needs its own
			// pair -- two per agent in total rather than the eight this started
			// with.
			agent.X += std::cos(agent.Angle) * speed;
			agent.Y += std::sin(agent.Angle) * speed;

			// A move is well under a cell, so the position can only have left
			// the field by one width. fmod here was a library call per agent
			// per axis for a case that a comparison settles.
			if (agent.X < 0.0f) agent.X += (float)s_Width;
			else if (agent.X >= (float)s_Width) agent.X -= (float)s_Width;

			if (agent.Y < 0.0f) agent.Y += (float)s_Height;
			else if (agent.Y >= (float)s_Height) agent.Y -= (float)s_Height;

			int ix = Fold((int)agent.X, s_Width);
			int iy = Fold((int)agent.Y, s_Height);

			float& cell = m_Trail[(size_t)iy * s_Width + ix];
			cell = std::min(1.0f, cell + m_Deposit * dt);
		}
	}

	// Attract and repel, as a bias on the turn rather than on the position --
	// see the note at the top of the file.
	void ApplyPointer(Agent& agent, float turn)
	{
		if (m_PointerMode == PointerMode::None || m_PointerMode == PointerMode::Feed)
			return;

		float dx = m_PointerX - agent.X;
		float dy = m_PointerY - agent.Y;

		// Shortest way round the torus, or agents on the far edge chase the
		// pointer the long way and the field develops a scar down the seam.
		if (dx > s_Width * 0.5f) dx -= s_Width;
		if (dx < -s_Width * 0.5f) dx += s_Width;
		if (dy > s_Height * 0.5f) dy -= s_Height;
		if (dy < -s_Height * 0.5f) dy += s_Height;

		float distanceSquared = dx * dx + dy * dy;
		if (distanceSquared > m_PointerRadius * m_PointerRadius)
			return;

		float wanted = std::atan2(dy, dx);
		if (m_PointerMode == PointerMode::Repel)
			wanted += 3.14159265f;

		// The signed angle to turn through, wrapped into (-pi, pi] so the agent
		// always takes the short way round.
		float delta = wanted - agent.Angle;
		while (delta > 3.14159265f) delta -= 6.2831853f;
		while (delta < -3.14159265f) delta += 6.2831853f;

		// Falls off with distance, so the edge of the radius is not a cliff the
		// colony can be seen to break along.
		float falloff = 1.0f - std::sqrt(distanceSquared) / m_PointerRadius;

		agent.Angle += glm::clamp(delta, -turn, turn) * m_PointerStrength * falloff;
	}

	void DiffuseAndDecay(float dt)
	{
		// A 3x3 box blur, then a multiply. Diffusion is what turns a line of
		// deposits into something with width for a neighbouring agent to smell,
		// and decay is what stops the whole field saturating to white -- with
		// either one missing there is no structure, just noise or a solid
		// block.
		float keep = std::exp(-m_Decay * dt);
		float blend = glm::clamp(m_Diffuse * dt, 0.0f, 1.0f);

		// The interior and the border are separate loops, which is worth the
		// duplication: the wrap is two integer divisions per sample, and with
		// 147,456 cells reading nine neighbours each that was most of the step.
		// Only the four edges can wrap, and they are 2(W+H) cells of 147,456.
		//
		// **The summation order is deliberately the same in both loops**, and
		// the same as the version that had a wrap on every sample: floating
		// point addition does not associate, so reordering these nine terms
		// changes the result in the last bits and the demo stops reproducing
		// its own recordings.
		auto blur = [&](int x, int y, int up, int mid, int down, int left, int right)
		{
			float sum =
				m_Trail[up + left] + m_Trail[up + x] + m_Trail[up + right] +
				m_Trail[mid + left] + m_Trail[mid + x] + m_Trail[mid + right] +
				m_Trail[down + left] + m_Trail[down + x] + m_Trail[down + right];

			float blurred = sum * (1.0f / 9.0f);
			float value = m_Trail[mid + x] + (blurred - m_Trail[mid + x]) * blend;

			m_Next[mid + x] = value * keep;
		};

		for (int y = 0; y < s_Height; y++)
		{
			int up = Wrap(y - 1, s_Height) * s_Width;
			int mid = y * s_Width;
			int down = Wrap(y + 1, s_Height) * s_Width;

			blur(0, y, up, mid, down, s_Width - 1, 1);

			for (int x = 1; x < s_Width - 1; x++)
				blur(x, y, up, mid, down, x - 1, x + 1);

			blur(s_Width - 1, y, up, mid, down, s_Width - 2, 0);
		}

		m_Trail.swap(m_Next);
	}

	void ReadPointer()
	{
		m_PointerMode = PointerMode::None;

		// A click on the panel is a click on the panel. Without this, dragging
		// a slider also drags the colony around behind it.
		if (ImGui::GetIO().WantCaptureMouse)
			return;

		bool attract = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT);
		bool repel = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_RIGHT);
		bool feed = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_MIDDLE);

		if (!attract && !repel && !feed)
			return;

		Egss::Window& window = Egss::Application::Get().GetWindow();
		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();

		if (width <= 0.0f || height <= 0.0f)
			return;

		std::pair<float, float> mouse = Egss::Input::GetMousePosition();

		// Straight from window pixels to grid cells: the quad fills the view,
		// so the mapping is a scale. Going through the camera would be the same
		// number with two more matrices in it.
		m_PointerX = (mouse.first / width) * (float)s_Width;
		m_PointerY = (1.0f - mouse.second / height) * (float)s_Height;

		m_PointerMode = feed ? PointerMode::Feed
			: (repel ? PointerMode::Repel : PointerMode::Attract);

		if (m_PointerMode == PointerMode::Feed)
			Feed();
	}

	void Feed()
	{
		int radius = (int)m_PointerRadius / 3;

		for (int dy = -radius; dy <= radius; dy++)
		{
			for (int dx = -radius; dx <= radius; dx++)
			{
				float falloff = 1.0f - std::sqrt((float)(dx * dx + dy * dy)) / (float)(radius + 1);
				if (falloff <= 0.0f)
					continue;

				int x = Wrap((int)m_PointerX + dx, s_Width);
				int y = Wrap((int)m_PointerY + dy, s_Height);

				float& cell = m_Trail[(size_t)y * s_Width + x];
				cell = std::min(1.0f, cell + falloff * 0.5f);
			}
		}
	}

	// --- Presentation -------------------------------------------------------

	// Two-stop ramp through a colour the trail never reaches on its own, so the
	// busiest veins read as brighter *and* warmer. A single-hue ramp makes the
	// whole thing look flat at a glance, which matters more for something you
	// see behind your windows all day than for a demo.
	//
	// **Baked into 256 entries**, because the alternative is a `pow` per pixel
	// per frame: 147,456 of them, which measured as most of what the frame cost
	// once the blur was fixed. The trail is one value per cell, so the colour is
	// a function of one number, so it is a table.
	void BuildPalette()
	{
		for (int i = 0; i < 256; i++)
		{
			float t = (float)i / 255.0f;

			// Slight gamma so the faint exploratory trails stay visible instead
			// of crushing to black.
			t = std::pow(t, 0.75f);

			float r = glm::clamp(t * 1.6f - 0.35f, 0.0f, 1.0f);
			float g = glm::clamp(t * 1.35f, 0.0f, 1.0f);
			float b = glm::clamp(0.18f + t * 0.9f, 0.0f, 1.0f);

			unsigned int red = (unsigned int)(r * 255.0f);
			unsigned int green = (unsigned int)(g * 255.0f);
			unsigned int blue = (unsigned int)(b * 255.0f * (0.25f + 0.75f * t));

			m_Palette[i] = 0xFF000000u | (blue << 16) | (green << 8) | red;
		}
	}

	void Colourise()
	{
		// Exposure stays outside the table so dragging it is still immediate --
		// it scales the *index*, and only the ramp's shape is baked.
		float scale = m_Exposure * 255.0f;

		for (size_t i = 0; i < m_Trail.size(); i++)
		{
			int index = (int)(m_Trail[i] * scale);
			m_Pixels[i] = m_Palette[index < 0 ? 0 : (index > 255 ? 255 : index)];
		}
	}

	void OnDemoImGui() override
	{
		ImGui::Begin("Slime mould");

		ImGui::Text("%d agents on %d x %d, %.2f ms/frame",
			(int)m_Agents.size(), s_Width, s_Height, m_FrameTime);
		ImGui::Text("step %d", m_Steps);
		ImGui::TextDisabled("left attract   right repel   middle feed");

		ImGui::Separator();

		ImGui::SliderFloat("Move speed", &m_MoveSpeed, 1.0f, 120.0f, "%.0f cells/s");
		ImGui::SliderFloat("Turn speed", &m_TurnSpeed, 10.0f, 720.0f, "%.0f deg/s");
		ImGui::SliderFloat("Sensor angle", &m_SensorAngle, 1.0f, 90.0f, "%.0f deg");
		ImGui::SliderFloat("Sensor distance", &m_SensorDistance, 1.0f, 32.0f, "%.1f cells");

		ImGui::Separator();

		ImGui::SliderFloat("Deposit", &m_Deposit, 0.1f, 20.0f);
		ImGui::SliderFloat("Decay", &m_Decay, 0.01f, 4.0f, "%.2f /s");
		ImGui::SliderFloat("Diffuse", &m_Diffuse, 0.1f, 60.0f, "%.1f /s");

		ImGui::Separator();

		ImGui::SliderFloat("Pointer radius", &m_PointerRadius, 4.0f, 160.0f, "%.0f cells");
		ImGui::SliderFloat("Pointer strength", &m_PointerStrength, 0.0f, 4.0f);

		ImGui::Separator();

		ImGui::SliderFloat("Exposure", &m_Exposure, 0.2f, 8.0f);
		ImGui::SliderInt("Agents", &m_AgentCount, 1000, 400000);
		ImGui::InputInt("Seed", &m_Seed);

		if (ImGui::Button("Reset"))
			Reset();

		ImGui::End();
	}

	enum class PointerMode { None, Attract, Repel, Feed };

	Egss::OrthographicCamera m_Camera;

	std::vector<Agent> m_Agents;
	std::vector<float> m_Trail, m_Next;
	std::vector<unsigned int> m_Pixels;
	std::shared_ptr<Egss::Texture2D> m_Texture;

	unsigned int m_Random = 0x9E3779B9u;
	int m_Seed = 1;
	int m_Steps = 0;
	float m_FrameTime = 0.0f;

	// Defaults found by watching it: fast enough to organise inside a few
	// seconds, slow enough that the veins hold still once they have.
	float m_MoveSpeed = 42.0f;
	float m_TurnSpeed = 220.0f;
	float m_SensorAngle = 30.0f;
	float m_SensorDistance = 9.0f;

	// **Deposit and decay are a ratio, and the ratio is the picture.** A cell an
	// agent visits every step settles at deposit/(1 - keep) -- here 0.05 per
	// step against a 14% loss, so about 0.36, comfortably below the clamp. The
	// first attempt had deposit 6/s against decay 0.6/s, which is 0.1 a step
	// against a 1% loss: every cell reached the clamp within a few seconds and
	// the field came out solid white with the *low* ground showing as holes.
	// If it ever looks like that again, this ratio is why.
	float m_Deposit = 3.0f;
	float m_Decay = 9.0f;
	float m_Diffuse = 20.0f;

	float m_PointerRadius = 60.0f;
	float m_PointerStrength = 1.5f;

	float m_Exposure = 2.2f;
	int m_AgentCount = 80000;

	unsigned int m_Palette[256] = {};

	PointerMode m_PointerMode = PointerMode::None;
	float m_PointerX = 0.0f, m_PointerY = 0.0f;
};
