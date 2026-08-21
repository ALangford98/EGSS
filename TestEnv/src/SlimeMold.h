#pragma once

// Physarum: three breeds of slime mould, competing for the desk.
//
// The base algorithm is two lines of idea. Each agent smells the trail ahead of
// it at three points -- left, centre, right -- and turns toward the strongest.
// Then it moves forward and leaves a little trail behind it. That is all.
// Everything a colony does -- veins, junctions, two strands finding each other
// and merging -- comes out of those two rules plus diffusion, and none of it is
// written down anywhere in this file.
//
// **What makes this version a fight is that a cell has an owner.** The trail map
// stores a strength and the breed that laid it, and an agent depositing on a
// rival's cell *subtracts* instead of adding -- taking the cell over when it
// drives the strength through zero. Territory therefore has fronts, the fronts
// move, and a breed that is out-depositing another eats into it. One float and
// one byte per cell, which is why this costs about what the single-colony
// version cost rather than three times as much: the obvious alternative is one
// trail map per breed, and that triples the blur, which is the expensive part.
//
// **The breeds differ in parameters only**, not in code. Sensor angle, sensor
// distance, speed, deposit and decay put physarum in visibly different regimes:
//
//   Veins    -- narrow sensors, long reach: the classic branching network
//   Foam     -- wide sensors, short reach, heavy deposit: packed cells with
//               dark borders between them, a genuinely different attractor
//               rather than the same picture at another scale
//   Filigree -- fast turning, faint deposit, long reach: thin restless
//               filaments that never quite settle
//
// `RivalWeight` is the one parameter with no single-colony meaning, and it is
// what makes them behave differently *toward each other*. Negative and an agent
// avoids enemy ground, so the breed defends what it holds; positive and it is
// drawn to it, so the breed pushes into the front. A colony of avoiders beside
// a colony of attackers is a border that only moves one way.
//
// All of it runs on the fixed step, so the demo records and replays exactly, and
// it is single-threaded on purpose: several threads depositing into one trail
// map add their floats in whatever order they arrive, and the run stops
// reproducing itself.
//
// Pointer: **left** attracts, **right** repels, **middle** feeds whichever breed
// owns the ground under it. Attract and repel bias the *turn* rather than the
// position, because pushing agents directly moves them without changing what
// they want, and the colony snaps back the moment you let go.

#include <Egss.h>
#include <imgui.h>

#include "Demo.h"
#include "DesktopWindows.h"

class SlimeMold : public DemoLayer
{
public:
	SlimeMold()
		: DemoLayer("SlimeMold"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
		// Everything a slider can move that the simulation then reads. A
		// recording samples these per fixed step, so a session where the speed
		// was dragged replays as itself.
		RegisterParam("Diffuse", &m_Diffuse);
		RegisterParam("PointerRadius", &m_PointerRadius);
		RegisterParam("PointerStrength", &m_PointerStrength);
		RegisterParam("Speed", &m_SpeedScale);
	}

	struct Breed
	{
		const char* Name = "";
		glm::vec3 Colour = { 1.0f, 1.0f, 1.0f };

		float MoveSpeed = 30.0f;      // cells a second
		float TurnSpeed = 200.0f;     // degrees a second
		float SensorAngle = 30.0f;    // degrees off the heading
		float SensorDistance = 9.0f;  // cells ahead

		// **Deposit over decay is the equilibrium strength**, and it is the
		// first thing to check when a breed looks wrong. A cell an agent keeps
		// visiting settles at about `Deposit / Decay`; above 1 it hits the
		// clamp, the whole colony renders as flat white, and every structural
		// parameter stops making any visible difference. Keep the ratio around
		// a third to a half.
		float Deposit = 3.0f;         // strength a second onto its own ground
		float Decay = 9.0f;           // strength lost a second, on its ground
		float Attack = 1.0f;          // multiplier when depositing on a rival
		float RivalWeight = -0.5f;    // <0 avoids enemy ground, >0 seeks it

		// **Crowd avoidance, and it is what makes a filled cell rather than a
		// line.** With the classic rule an agent always climbs toward more
		// trail, so everybody ends up on the same ridges and the bright parts
		// of the picture are one cell wide. Above `Peak`, this bends the
		// sensed value back down -- so ground that is already busy stops being
		// attractive and agents spread out to fill an area instead of piling
		// onto its spine. The dark borders are then where two spreading groups
		// meet and both turn back.
		//
		// Zero leaves the classic behaviour exactly as it was.
		float Inhibition = 0.0f;
		float Peak = 0.5f;

		// **Read the ramp backwards.** The cellular regime concentrates trail
		// into the *walls* between pockets, so drawn straight it is bright
		// lines around dark cells. Inverted it is what it looks like under a
		// microscope: filled cells with thin dark borders, each fading from a
		// bright middle to the wall it shares with its neighbour. Same
		// simulation, opposite ink.
		bool Invert = false;
	};

	static constexpr int s_BreedCount = 3;

	void OnDemoAttach() override
	{
		Egss::Application& app = Egss::Application::Get();

		// **A window and a desk want different cell sizes.** In a 1280x720
		// window, 8 px/cell is 160x90 cells and the veins come out as blocks;
		// across three monitors it is 960x495 and they are hairlines. The
		// default follows the mode, and the flag overrides either.
		// 10 rather than 8 because three breeds cost more than one did: the
		// ownership map is a second array the blur reads and writes, and the
		// fight adds a branch per deposit. Measured across the three-monitor
		// span, per fixed step at 30 Hz: 24.2 ms at 8 px/cell, 16.8 at 10,
		// 13.0 at 12. Ten is half a core, which is the most a wallpaper should
		// ask for.
		m_PixelsPerCell = app.IsWallpaper() ? 10 : 3;

		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		for (size_t i = 1; i + 1 < arguments.size(); i++)
		{
			if (arguments[i] == "--wallpaper-scale")
				m_PixelsPerCell = glm::clamp(std::atoi(arguments[i + 1].c_str()), 1, 64);

			if (arguments[i] == "--wallpaper-density")
				m_Density = glm::clamp((float)std::atof(arguments[i + 1].c_str()), 0.01f, 4.0f);
		}

		BuildBreeds();

		unsigned int pixelWidth = app.GetWindow().GetWidth();
		unsigned int pixelHeight = app.GetWindow().GetHeight();

		m_Width = std::max(64, (int)pixelWidth / m_PixelsPerCell);
		m_Height = std::max(64, (int)pixelHeight / m_PixelsPerCell);

		BuildScreens(app);

		m_AgentCount = glm::clamp((int)((float)m_Width * (float)m_Height * m_Density),
			1000, 2000000);

		// **A wallpaper does not need 60 Hz.** Nobody watches a slime mould for
		// temporal precision, and halving the rate halves what it costs the
		// machine it is decorating.
		if (app.IsWallpaper())
			app.SetFixedTimestep(1.0f / 30.0f);

		// Only as a wallpaper, and only if asked: reacting to windows in the
		// demo would make it non-reproducible for no benefit, since the demo is
		// a window itself.
		m_UseWindows = app.IsWallpaper();

		for (const std::string& argument : arguments)
			if (argument == "--no-windows")
				m_UseWindows = false;

		float aspect = (float)m_Width / (float)m_Height;
		m_Camera.SetProjection(-aspect, aspect, -1.0f, 1.0f);
		m_QuadSize = { 2.0f * aspect, 2.0f };

		EGSS_TRACE("Slime mould: {0}x{1} cells over {2}x{3} pixels, {4} agents, {5} screens",
			m_Width, m_Height, pixelWidth, pixelHeight, m_AgentCount, m_Screens.size());

		size_t cells = (size_t)m_Width * m_Height;

		m_Strength.assign(cells, 0.0f);
		m_NextStrength.assign(cells, 0.0f);
		m_Owner.assign(cells, (unsigned char)0);
		m_NextOwner.assign(cells, (unsigned char)0);
		m_Pixels.assign(cells, 0u);

		m_Texture.reset(Egss::Texture2D::Create(m_Width, m_Height));

		BuildPalettes();
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

		// Windows are polled rather than pushed at us, and only every eighth
		// step: a stat() is cheap but not free, and a wallpaper reacting an
		// eighth of a second late is a wallpaper reacting immediately.
		//
		// **This is the one thing in the demo that is not deterministic**, and
		// it cannot be: the desk is external state. It is confined to wallpaper
		// mode, so a recorded session of the demo still replays exactly.
		if (m_UseWindows && (m_Steps % 8) == 0 && m_Windows.Poll(m_Monitors))
		{
			RebuildBlocked();

			size_t blocked = 0;
			for (unsigned char cell : m_Blocked)
				blocked += cell;

			EGSS_TRACE("Desk: {0} windows, {1} of {2} cells covered ({3:.1f}%)",
				m_Windows.Rects().size(), blocked, m_Blocked.size(),
				100.0f * (float)blocked / (float)std::max<size_t>(1, m_Blocked.size()));
		}

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
		Egss::Renderer2D::DrawQuad(glm::vec2(0.0f), m_QuadSize, m_Texture);
		Egss::Renderer2D::EndScene();
	}

private:
	int m_Width = 512;
	int m_Height = 288;
	int m_PixelsPerCell = 3;

	struct Agent
	{
		float X = 0.0f, Y = 0.0f;
		float Angle = 0.0f;
		unsigned char Species = 0;
	};

	// A monitor, in cells rather than pixels.
	struct Screen
	{
		float X = 0.0f, Y = 0.0f;
		float Width = 0.0f, Height = 0.0f;
		std::string Name;
	};

	void BuildBreeds()
	{
		// **The classic network.** Narrow sensors looking a long way ahead is
		// what makes a filament rather than a smear: an agent commits to the
		// strongest trail in a narrow cone and follows it, and a trail followed
		// is a trail reinforced.
		Breed& veins = m_Breeds[0];
		veins.Name = "Veins";
		veins.Colour = { 0.42f, 1.00f, 0.38f };
		veins.MoveSpeed = 26.0f;
		veins.TurnSpeed = 200.0f;
		veins.SensorAngle = 28.0f;
		veins.SensorDistance = 11.0f;
		// Equilibrium 3/9 = 0.33 -- see the note on the ratio below.
		veins.Deposit = 3.0f;
		veins.Decay = 9.0f;
		veins.Attack = 1.0f;
		veins.RivalWeight = -0.6f;    // keeps to itself, defends its lines

		// **The foam.** Wide sensors with a short reach cannot follow a line:
		// at 70 degrees the three samples cover most of the forward half-plane,
		// so an agent turns toward whichever side is generally busier and ends
		// up circulating inside a pocket rather than running along a vein.
		// Heavy deposit against slow decay fills the pockets, and what stays
		// dark is the boundary between two of them, where agents from each side
		// turn back.
		Breed& foam = m_Breeds[1];
		foam.Name = "Foam";
		foam.Colour = { 0.95f, 0.60f, 0.18f };
		foam.MoveSpeed = 16.0f;
		foam.TurnSpeed = 340.0f;
		foam.SensorAngle = 50.0f;
		foam.SensorDistance = 6.0f;

		// Equilibrium 2.2/4.5 = 0.49. The first attempt used 6 against 4.5,
		// which is 1.33 -- above the clamp, so every visited cell pinned at
		// white and no amount of tuning the *structure* showed through. That
		// cost an afternoon of sweeping sensor angles against a picture that
		// could not change.
		foam.Deposit = 2.2f;
		foam.Decay = 4.5f;
		foam.Attack = 1.2f;
		foam.RivalWeight = 0.35f;     // pushes into a front rather than holding
		foam.Inhibition = 2.0f;       // what makes it fill areas, not lines
		foam.Peak = 0.30f;

		// **Filigree.** A faint deposit that decays fast never builds a trail
		// worth following for long, so the structure is always being rebuilt --
		// thin, restless, and far more mobile than the other two.
		Breed& filigree = m_Breeds[2];
		filigree.Name = "Filigree";
		filigree.Colour = { 0.40f, 0.66f, 1.00f };
		filigree.MoveSpeed = 34.0f;
		filigree.TurnSpeed = 520.0f;
		filigree.SensorAngle = 18.0f;
		filigree.SensorDistance = 16.0f;
		filigree.Deposit = 3.0f;      // equilibrium 3/14 = 0.21, deliberately faint
		filigree.Decay = 14.0f;
		filigree.Attack = 0.8f;
		filigree.RivalWeight = -0.2f;
	}

	void BuildScreens(Egss::Application& app)
	{
		m_Monitors = Egss::Window::GetMonitors();

		// Monitor rectangles are in the arrangement's coordinates and the
		// window sits at the arrangement's top-left, so subtracting that origin
		// puts them in window pixels -- and dividing puts them in cells.
		int originX = 0, originY = 0;

		if (app.IsWallpaper() && !m_Monitors.empty())
		{
			originX = m_Monitors[0].X;
			originY = m_Monitors[0].Y;

			for (const Egss::MonitorInfo& monitor : m_Monitors)
			{
				originX = std::min(originX, monitor.X);
				originY = std::min(originY, monitor.Y);
			}
		}

		m_OriginX = originX;
		m_OriginY = originY;

		m_Screens.clear();

		for (const Egss::MonitorInfo& monitor : m_Monitors)
		{
			Screen screen;
			screen.X = (float)(monitor.X - originX) / (float)m_PixelsPerCell;
			screen.Y = (float)(monitor.Y - originY) / (float)m_PixelsPerCell;
			screen.Width = (float)monitor.Width / (float)m_PixelsPerCell;
			screen.Height = (float)monitor.Height / (float)m_PixelsPerCell;
			screen.Name = monitor.Name;

			m_Screens.push_back(screen);
		}

		if (!app.IsWallpaper() || m_Screens.empty())
		{
			// In a window the three breeds share it, seeded in a row, so the
			// demo shows the same fight the desk does.
			m_Screens.clear();

			for (int i = 0; i < s_BreedCount; i++)
			{
				Screen screen;
				screen.Width = (float)m_Width / (float)s_BreedCount;
				screen.Height = (float)m_Height;
				screen.X = screen.Width * (float)i;
				screen.Y = 0.0f;
				screen.Name = m_Breeds[i].Name;

				m_Screens.push_back(screen);
			}
		}
	}

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

		std::fill(m_Strength.begin(), m_Strength.end(), 0.0f);
		std::fill(m_Owner.begin(), m_Owner.end(), (unsigned char)0);

		// **One breed per screen**, each starting as a disc at that monitor's
		// centre. A disc rather than a scatter because it has an edge, and the
		// edge is where the first structure forms.
		//
		// They are separate only at the start: the field is continuous across
		// the whole desk, so they grow into each other and the borders start
		// moving, which is the part worth watching.
		m_Agents.resize((size_t)std::max(1, m_AgentCount));

		size_t screens = std::max<size_t>(1, m_Screens.size());

		for (size_t i = 0; i < m_Agents.size(); i++)
		{
			// Dealt round-robin rather than in blocks, so a breed is not one
			// contiguous run of the agent array -- which would let whichever
			// breed came first deposit onto every contested cell before the
			// others got a turn, and win borders by array order.
			size_t which = i % screens;

			const Screen& screen = m_Screens[which];
			unsigned char species = (unsigned char)(which % s_BreedCount);

			float radius = 0.30f * std::min(screen.Width, screen.Height);

			float angle = RandomUnit() * 6.2831853f;
			float distance = std::sqrt(RandomUnit()) * radius;

			Agent& agent = m_Agents[i];
			agent.X = screen.X + screen.Width * 0.5f + std::cos(angle) * distance;
			agent.Y = screen.Y + screen.Height * 0.5f + std::sin(angle) * distance;
			agent.Angle = angle;
			agent.Species = species;
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
	// most one field away from inside: a sensor reaches `SensorDistance` and an
	// agent moves less than a cell a step, against a field hundreds wide.
	static int FoldTo(int value, int limit)
	{
		if (value < 0)
			return value + limit;

		return value >= limit ? value - limit : value;
	}

	// **Ground a window is sitting on.** Not a fourth breed and not an owner
	// value, because a window is temporary: the cells under it have to come
	// back as empty ground the moment it moves, with whatever the breeds do
	// next decided by them rather than by who held it before.
	//
	// Rebuilt only when the window list changes -- a drag reports continuously
	// and rasterising 300,000 cells per report would cost more than the
	// simulation.
	void RebuildBlocked()
	{
		m_Blocked.assign((size_t)m_Width * m_Height, (unsigned char)0);

		for (const DesktopWindows::Rect& rect : m_Windows.Rects())
		{
			// Arrangement pixels to cells, and inset by a cell so a window's
			// own edge is not blocked -- the colony crowding right up to the
			// border is the part that reads as reacting to it.
			int x0 = (int)((rect.X - (float)m_OriginX) / (float)m_PixelsPerCell);
			int y0 = (int)((rect.Y - (float)m_OriginY) / (float)m_PixelsPerCell);
			int x1 = x0 + (int)(rect.Width / (float)m_PixelsPerCell);
			int y1 = y0 + (int)(rect.Height / (float)m_PixelsPerCell);

			// The field's y runs up from the bottom and the desktop's runs down
			// from the top, so a window near the top of the screen is near the
			// *end* of the field. Getting this wrong is not subtle -- the
			// colony avoids a mirror image of your windows.
			int flippedY0 = m_Height - y1;
			int flippedY1 = m_Height - y0;

			for (int y = std::max(0, flippedY0); y < std::min(m_Height, flippedY1); y++)
				for (int x = std::max(0, x0); x < std::min(m_Width, x1); x++)
					m_Blocked[(size_t)y * m_Width + x] = 1;
		}
	}

	// What one breed makes of what is written at a point: its own trail read
	// straight, a rival's scaled by `RivalWeight` -- negative to avoid enemy
	// ground, positive to be drawn to it.
	float SenseAt(float x, float y, unsigned char species,
		float rivalWeight, float inhibition, float peak) const
	{
		size_t index = (size_t)FoldTo((int)y, m_Height) * m_Width + FoldTo((int)x, m_Width);

		// A window reads as strongly repellent rather than merely empty, so a
		// colony does not drift into the space behind it and sit there
		// invisibly: agents turn at the edge and the structure reorganises
		// around what is on screen.
		if (!m_Blocked.empty() && m_Blocked[index])
			return -1000.0f;

		float strength = m_Strength[index];
		float value = m_Owner[index] == species ? strength : strength * rivalWeight;

		// A tent rather than a ramp: attractive up to `peak`, less attractive
		// beyond it.
		if (inhibition > 0.0f && value > peak)
			value = peak - (value - peak) * inhibition;

		return value;
	}

	void SenseAndMove(float dt)
	{
		// Per-breed trigonometry, hoisted out of the agent loop: three breeds
		// means three sensor angles to prepare, not one per agent.
		struct Prepared
		{
			float CosOffset, SinOffset;
			float Turn, Speed, Distance, Deposit, Attack, RivalWeight;
			float Inhibition, Peak;
		};

		Prepared prepared[s_BreedCount];

		for (int i = 0; i < s_BreedCount; i++)
		{
			const Breed& breed = m_Breeds[i];
			float sensorAngle = glm::radians(breed.SensorAngle);

			prepared[i].CosOffset = std::cos(sensorAngle);
			prepared[i].SinOffset = std::sin(sensorAngle);
			prepared[i].Turn = glm::radians(breed.TurnSpeed) * dt;
			prepared[i].Speed = breed.MoveSpeed * m_SpeedScale * dt;
			prepared[i].Distance = breed.SensorDistance;
			prepared[i].Deposit = breed.Deposit * dt;
			prepared[i].Attack = breed.Attack;
			prepared[i].RivalWeight = breed.RivalWeight;
			prepared[i].Inhibition = breed.Inhibition;
			prepared[i].Peak = breed.Peak;
		}

		for (Agent& agent : m_Agents)
		{
			const Prepared& p = prepared[agent.Species];

			// **The three sensor directions from one sine and one cosine.**
			// cos(a +/- s) = cos a cos s -/+ sin a sin s, which turns six trig
			// calls per agent into two -- and at a few hundred thousand agents
			// a step, trig was the simulation.
			float cosA = std::cos(agent.Angle);
			float sinA = std::sin(agent.Angle);

			float distance = p.Distance;

			float centre = SenseAt(agent.X + cosA * distance,
				agent.Y + sinA * distance, agent.Species, p.RivalWeight,
				p.Inhibition, p.Peak);

			float left = SenseAt(
				agent.X + (cosA * p.CosOffset - sinA * p.SinOffset) * distance,
				agent.Y + (sinA * p.CosOffset + cosA * p.SinOffset) * distance,
				agent.Species, p.RivalWeight, p.Inhibition, p.Peak);

			float right = SenseAt(
				agent.X + (cosA * p.CosOffset + sinA * p.SinOffset) * distance,
				agent.Y + (sinA * p.CosOffset - cosA * p.SinOffset) * distance,
				agent.Species, p.RivalWeight, p.Inhibition, p.Peak);

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
				agent.Angle += (RandomUnit() < 0.5f ? -p.Turn : p.Turn);
			}
			else if (left > right)
			{
				agent.Angle += p.Turn;
			}
			else if (right > left)
			{
				agent.Angle -= p.Turn;
			}

			ApplyPointer(agent, p.Turn);

			agent.X += std::cos(agent.Angle) * p.Speed;
			agent.Y += std::sin(agent.Angle) * p.Speed;

			// A move is well under a cell, so the position can only have left
			// the field by one width.
			if (agent.X < 0.0f) agent.X += (float)m_Width;
			else if (agent.X >= (float)m_Width) agent.X -= (float)m_Width;

			if (agent.Y < 0.0f) agent.Y += (float)m_Height;
			else if (agent.Y >= (float)m_Height) agent.Y -= (float)m_Height;

			size_t index = (size_t)FoldTo((int)agent.Y, m_Height) * m_Width
				+ FoldTo((int)agent.X, m_Width);

			// Nothing is laid under a window. Without this a colony keeps
			// depositing behind one and reappears fully formed the moment it
			// moves, which looks like the wallpaper ignoring it.
			if (!m_Blocked.empty() && m_Blocked[index])
				continue;

			// --- the fight ---
			//
			// On its own ground a deposit builds. On a rival's it *erodes*, and
			// the cell changes hands when the erosion drives the strength
			// through zero -- so a front is a line of cells being fought over
			// rather than a wall, and the breed depositing harder moves it.
			if (m_Owner[index] == agent.Species)
			{
				m_Strength[index] = std::min(1.0f, m_Strength[index] + p.Deposit);
			}
			else
			{
				float remaining = m_Strength[index] - p.Deposit * p.Attack;

				if (remaining <= 0.0f)
				{
					m_Owner[index] = agent.Species;

					// Whatever was left of the attack lands as the new owner's
					// own trail, so taking a strong cell costs more than taking
					// a weak one: a breed arrives on hard-won ground with less
					// than it would have had on empty land.
					m_Strength[index] = std::min(1.0f, -remaining);
				}
				else
				{
					m_Strength[index] = remaining;
				}
			}
		}
	}

	// Attract and repel, as a bias on the turn rather than on the position.
	void ApplyPointer(Agent& agent, float turn)
	{
		if (m_PointerMode == PointerMode::None || m_PointerMode == PointerMode::Feed)
			return;

		float dx = m_PointerX - agent.X;
		float dy = m_PointerY - agent.Y;

		// Shortest way round the torus, or agents on the far edge chase the
		// pointer the long way and the field develops a scar down the seam.
		if (dx > m_Width * 0.5f) dx -= m_Width;
		if (dx < -m_Width * 0.5f) dx += m_Width;
		if (dy > m_Height * 0.5f) dy -= m_Height;
		if (dy < -m_Height * 0.5f) dy += m_Height;

		float distanceSquared = dx * dx + dy * dy;
		if (distanceSquared > m_PointerRadius * m_PointerRadius)
			return;

		float wanted = std::atan2(dy, dx);
		if (m_PointerMode == PointerMode::Repel)
			wanted += 3.14159265f;

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
		// Diffusion is what turns a line of deposits into something with width
		// for a neighbouring agent to smell; decay is what stops the field
		// saturating. With either missing there is no structure, just noise or
		// a solid block.
		//
		// **Ownership diffuses too, and it does it by strength.** A cell's next
		// owner is whichever neighbour holds the most trail, which makes a
		// front advance where one side is denser and hold where the two are
		// even. Doing it by majority instead gives a front that jitters,
		// because nine cells vote in steps of one ninth.
		float blend = glm::clamp(m_Diffuse * dt, 0.0f, 1.0f);

		float keep[s_BreedCount];
		for (int i = 0; i < s_BreedCount; i++)
			keep[i] = std::exp(-m_Breeds[i].Decay * dt);

		auto cell = [&](int x, int mid, int up, int down, int left, int right)
		{
			// **One pass over the nine neighbours, not two.** The sum and the
			// strongest-neighbour search read the same values, and splitting
			// them into two loops over an `offsets[9]` array cost about 4 ms a
			// step at wallpaper size -- the array is written to the stack and
			// the field is read twice, at 475,000 cells a step.
			//
			// The nine terms are still summed in a fixed order: float addition
			// does not associate, and reordering them changes the result in the
			// last bits, which is enough to stop a recording reproducing.
			float sum = 0.0f;

			int best = mid + x;
			float bestStrength = m_Strength[mid + x];

			const int rows[3] = { up, mid, down };
			const int columns[3] = { left, x, right };

			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 3; c++)
				{
					int at = rows[r] + columns[c];
					float value = m_Strength[at];

					sum += value;

					// Ties go to the cell's current owner, which is what stops
					// a perfectly balanced border flickering between breeds.
					if (value > bestStrength)
					{
						bestStrength = value;
						best = at;
					}
				}
			}

			unsigned char owner = m_Owner[best];

			float blurred = sum * (1.0f / 9.0f);
			float value = m_Strength[mid + x] + (blurred - m_Strength[mid + x]) * blend;

			// Under a window the ground is scrubbed rather than decayed, so
			// moving a window uncovers empty land and the breeds race for it.
			if (!m_Blocked.empty() && m_Blocked[mid + x])
			{
				m_NextStrength[mid + x] = 0.0f;
				m_NextOwner[mid + x] = 0;
				return;
			}

			m_NextStrength[mid + x] = value * keep[owner];
			m_NextOwner[mid + x] = owner;
		};

		for (int y = 0; y < m_Height; y++)
		{
			int up = Wrap(y - 1, m_Height) * m_Width;
			int mid = y * m_Width;
			int down = Wrap(y + 1, m_Height) * m_Width;

			cell(0, mid, up, down, m_Width - 1, 1);

			for (int x = 1; x < m_Width - 1; x++)
				cell(x, mid, up, down, x - 1, x + 1);

			cell(m_Width - 1, mid, up, down, m_Width - 2, 0);
		}

		m_Strength.swap(m_NextStrength);
		m_Owner.swap(m_NextOwner);
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
		// so the mapping is a scale.
		m_PointerX = (mouse.first / width) * (float)m_Width;
		m_PointerY = (1.0f - mouse.second / height) * (float)m_Height;

		m_PointerMode = feed ? PointerMode::Feed
			: (repel ? PointerMode::Repel : PointerMode::Attract);

		if (m_PointerMode == PointerMode::Feed)
			Feed();
	}

	// Feeds whichever breed already holds the ground under the pointer, so the
	// mouse is a thumb on the scales of a border rather than a fourth player.
	void Feed()
	{
		int radius = std::max(1, (int)m_PointerRadius / 3);

		size_t centre = (size_t)FoldTo((int)m_PointerY, m_Height) * m_Width
			+ FoldTo((int)m_PointerX, m_Width);

		unsigned char species = m_Owner[centre];

		for (int dy = -radius; dy <= radius; dy++)
		{
			for (int dx = -radius; dx <= radius; dx++)
			{
				float falloff = 1.0f - std::sqrt((float)(dx * dx + dy * dy)) / (float)(radius + 1);
				if (falloff <= 0.0f)
					continue;

				size_t index = (size_t)Wrap((int)m_PointerY + dy, m_Height) * m_Width
					+ Wrap((int)m_PointerX + dx, m_Width);

				// Ground held strongly by somebody else does not flip from a
				// feed: the pointer helps a breed grow, it does not teleport a
				// border.
				if (m_Owner[index] != species && m_Strength[index] > falloff * 0.5f)
					continue;

				m_Owner[index] = species;
				m_Strength[index] = std::min(1.0f, m_Strength[index] + falloff * 0.5f);
			}
		}
	}

	// --- Presentation -------------------------------------------------------

	// One 256-entry ramp per breed, because the alternative is a `pow` per pixel
	// per frame -- and the colour is a function of one number, so it is a table.
	void BuildPalettes()
	{
		for (int breed = 0; breed < s_BreedCount; breed++)
		{
			const glm::vec3& colour = m_Breeds[breed].Colour;

			for (int i = 0; i < 256; i++)
			{
				float t = (float)i / 255.0f;

				if (m_Breeds[breed].Invert)
					t = 1.0f - t;

				// Slight gamma so the faint exploratory trails stay visible
				// instead of crushing to black.
				t = std::pow(t, 0.75f);

				// Toward white at the top of the ramp, so the busiest ground
				// reads as brighter as well as more saturated. Without it each
				// breed is one flat colour and the structure inside it
				// disappears.
				float highlight = glm::clamp(t * 1.7f - 0.7f, 0.0f, 1.0f);

				float r = glm::clamp(colour.r * t + highlight, 0.0f, 1.0f);
				float g = glm::clamp(colour.g * t + highlight, 0.0f, 1.0f);
				float b = glm::clamp(colour.b * t + highlight, 0.0f, 1.0f);

				unsigned int red = (unsigned int)(r * 255.0f);
				unsigned int green = (unsigned int)(g * 255.0f);
				unsigned int blue = (unsigned int)(b * 255.0f);

				m_Palettes[breed][i] = 0xFF000000u | (blue << 16) | (green << 8) | red;
			}
		}
	}

	void Colourise()
	{
		float scale = m_Exposure * 255.0f;

		for (size_t i = 0; i < m_Strength.size(); i++)
		{
			int index = (int)(m_Strength[i] * scale);
			m_Pixels[i] = m_Palettes[m_Owner[i]][index < 0 ? 0 : (index > 255 ? 255 : index)];
		}
	}

	void OnDemoImGui() override
	{
		ImGui::Begin("Slime mould");

		ImGui::Text("%d agents on %d x %d, %.2f ms/frame",
			(int)m_Agents.size(), m_Width, m_Height, m_FrameTime);
		ImGui::Text("step %d", m_Steps);

		if (m_UseWindows)
			ImGui::Text("%zu windows on the desk", m_Windows.Rects().size());
		ImGui::TextDisabled("left attract   right repel   middle feed");

		ImGui::Separator();

		// Who is winning, counted rather than eyeballed: a border that looks
		// even can still be moving steadily one way.
		int held[s_BreedCount] = {};
		for (size_t i = 0; i < m_Owner.size(); i++)
			if (m_Strength[i] > 0.02f)
				held[m_Owner[i]]++;

		float total = (float)std::max<size_t>(1, m_Strength.size());

		for (int i = 0; i < s_BreedCount; i++)
		{
			const Breed& breed = m_Breeds[i];
			ImGui::TextColored(ImVec4(breed.Colour.r, breed.Colour.g, breed.Colour.b, 1.0f),
				"%-9s %5.1f%% of the field", breed.Name, 100.0f * (float)held[i] / total);
		}

		ImGui::Separator();
		ImGui::SliderFloat("Speed", &m_SpeedScale, 0.1f, 3.0f, "%.2fx");
		ImGui::SliderFloat("Diffuse", &m_Diffuse, 0.1f, 60.0f, "%.1f /s");
		ImGui::SliderFloat("Exposure", &m_Exposure, 0.2f, 8.0f);

		ImGui::Separator();

		if (ImGui::BeginTabBar("breeds"))
		{
			for (int i = 0; i < s_BreedCount; i++)
			{
				if (!ImGui::BeginTabItem(m_Breeds[i].Name))
					continue;

				Breed& breed = m_Breeds[i];

				ImGui::SliderFloat("Move speed", &breed.MoveSpeed, 1.0f, 120.0f, "%.0f cells/s");
				ImGui::SliderFloat("Turn speed", &breed.TurnSpeed, 10.0f, 720.0f, "%.0f deg/s");
				ImGui::SliderFloat("Sensor angle", &breed.SensorAngle, 1.0f, 90.0f, "%.0f deg");
				ImGui::SliderFloat("Sensor distance", &breed.SensorDistance, 1.0f, 32.0f, "%.1f cells");
				ImGui::SliderFloat("Deposit", &breed.Deposit, 0.1f, 20.0f);
				ImGui::SliderFloat("Decay", &breed.Decay, 0.01f, 20.0f, "%.2f /s");
				ImGui::SliderFloat("Attack", &breed.Attack, 0.0f, 3.0f);
				ImGui::SliderFloat("Rival weight", &breed.RivalWeight, -2.0f, 2.0f);
				ImGui::SliderFloat("Inhibition", &breed.Inhibition, 0.0f, 4.0f);
				ImGui::SliderFloat("Peak", &breed.Peak, 0.05f, 1.0f);

				if (ImGui::ColorEdit3("Colour", &breed.Colour.x))
					BuildPalettes();

				if (ImGui::Checkbox("Invert", &breed.Invert))
					BuildPalettes();

				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::Separator();
		ImGui::SliderFloat("Pointer radius", &m_PointerRadius, 4.0f, 160.0f, "%.0f cells");
		ImGui::SliderFloat("Pointer strength", &m_PointerStrength, 0.0f, 4.0f);

		ImGui::Text("%d x %d cells at %d px/cell, %.2f agents/cell",
			m_Width, m_Height, m_PixelsPerCell, m_Density);

		ImGui::InputInt("Seed", &m_Seed);

		if (ImGui::Button("Reset"))
			Reset();

		ImGui::End();
	}

	enum class PointerMode { None, Attract, Repel, Feed };

	Egss::OrthographicCamera m_Camera;

	Breed m_Breeds[s_BreedCount];

	std::vector<Agent> m_Agents;
	std::vector<float> m_Strength, m_NextStrength;
	std::vector<unsigned char> m_Owner, m_NextOwner;
	std::vector<unsigned int> m_Pixels;
	std::shared_ptr<Egss::Texture2D> m_Texture;

	unsigned int m_Palettes[s_BreedCount][256] = {};

	DesktopWindows m_Windows;
	std::vector<unsigned char> m_Blocked;
	bool m_UseWindows = false;
	int m_OriginX = 0, m_OriginY = 0;

	std::vector<Egss::MonitorInfo> m_Monitors;
	std::vector<Screen> m_Screens;
	glm::vec2 m_QuadSize = { 3.2f, 1.8f };

	unsigned int m_Random = 0x9E3779B9u;
	int m_Seed = 1;
	int m_Steps = 0;
	float m_FrameTime = 0.0f;

	// Agents per cell. Density rather than a count, so the same number means
	// the same look on a laptop panel and across three monitors.
	float m_Density = 0.55f;
	int m_AgentCount = 120000;

	// One dial over every breed's speed, so the whole thing can be slowed
	// without retuning three sets of parameters against each other.
	float m_SpeedScale = 0.6f;

	// Diffusion is a property of the medium rather than of a breed, so it is
	// shared: three breeds diffusing at different rates would need three trail
	// maps, which is the cost this design exists to avoid.
	float m_Diffuse = 20.0f;

	float m_PointerRadius = 60.0f;
	float m_PointerStrength = 1.5f;
	float m_Exposure = 1.6f;

	PointerMode m_PointerMode = PointerMode::None;
	float m_PointerX = 0.0f, m_PointerY = 0.0f;
};
