#pragma once

// `--warmup <demo>` and `--warmup-steps <n>`: run a different demo first, then
// switch to the one the run is actually about.
//
// This exists to ask one question, and the answer should be boring:
//
//     Does a demo's captured frame depend on which demo ran before it?
//
// It should not, and it does. Persistent GL state -- cull face, depth writing,
// the blend mode -- is established once in `OpenGLRendererAPI::Init` and then
// changed by whichever demo wants it changed. Nothing re-establishes it at a
// frame or a layer boundary, so a demo that leaves it altered silently
// reconfigures every demo that runs after it. That is exactly how OpenWorld's
// water went invisible: CelShading left `CullFace` at `Back`, and the water is
// a single-sided quad you are usually looking at from below.
//
// The reason a bug like that costs a whole session is that the demo which
// breaks is not the demo that broke it, so reading the broken demo's code
// tells you nothing. A cheap way to *provoke* it is worth more than any amount
// of staring.
//
// Why the step counting works out
// -------------------------------
// `DemoLayer` seals the is-this-demo-active guard, so an inactive demo does
// not fixed-update, does not update, and does not draw. Warming another demo
// up therefore costs the target demo nothing -- it still sees exactly one
// `OnDemoFixedUpdate` per step from the moment it becomes active, and exactly
// one `OnDemoUpdate` per frame.
//
// So a run warmed up for W steps and captured at step W+T shows the target
// after exactly T steps of its own, which is what makes it comparable, pixel
// for pixel, against a plain run captured at step T. Any difference between
// those two images is the warmup demo leaking into the target.
//
// `OnAttach` is deliberately *not* guarded, so every demo's resources are built
// either way and no demo is being seen "cold" in one arm and warm in the other.
//
//     ./TestEnv --demo OpenWorld --lockstep --hide-ui \
//               --capture a.png --capture-step 120
//     ./TestEnv --demo OpenWorld --warmup Cel --warmup-steps 60 --lockstep \
//               --hide-ui --capture b.png --capture-step 180
//
// Identical files, or something leaked.

#include <Egss.h>

#include "DemoRegistry.h"

class DemoWarmup : public Egss::Layer
{
public:
	DemoWarmup()
		: Layer("DemoWarmup")
	{
	}

	// Both entry points initialise, because which one runs first depends on
	// whether the accumulator had a whole step in it. Under `--lockstep` it is
	// always the fixed one; interactively, frame zero can beat it.
	void OnFixedUpdate(Egss::Timestep step) override
	{
		(void)step;

		Initialise();

		if (m_Target == InvalidDemo)
			return;

		// `>=` rather than `==` so a step missed for any reason still switches
		// rather than warming up for the rest of the run.
		if (m_Steps >= m_WarmupSteps)
		{
			g_ActiveDemo = m_Target;
			m_Target = InvalidDemo;
			return;
		}

		m_Steps++;
	}

	void OnUpdate(Egss::Timestep ts) override
	{
		(void)ts;
		Initialise();
	}
private:
	// Deferred rather than done in the constructor: `PushAllDemos` is what
	// settles `g_ActiveDemo` from `--demo`, and this layer is pushed before the
	// demos so that it switches ahead of them within a step.
	void Initialise()
	{
		if (m_Initialised)
			return;

		m_Initialised = true;

		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		auto valueOf = [&arguments](const std::string& flag) -> std::string
		{
			for (size_t i = 1; i + 1 < arguments.size(); i++)
			{
				if (arguments[i] == flag)
					return arguments[i + 1];
			}

			return {};
		};

		std::string wanted = valueOf("--warmup");
		if (wanted.empty())
			return;

		int warmup = FindDemo(wanted);
		if (warmup < 0)
		{
			EGSS_WARN("--warmup '{0}' matches no demo", wanted);
			return;
		}

		std::string steps = valueOf("--warmup-steps");
		if (!steps.empty())
			m_WarmupSteps = std::atoi(steps.c_str());

		// Warming a demo up with itself is not an error -- it is the control
		// arm of the matrix, and it has to cost the same steps as any other
		// cell or the comparison is not like for like.
		m_Target = g_ActiveDemo;
		g_ActiveDemo = warmup;

		EGSS_INFO("Warmup: {0} for {1} steps, then {2}",
			s_Demos[warmup].ShortName, m_WarmupSteps, s_Demos[m_Target].ShortName);
	}

	// Index or short name, the same two spellings `--demo` takes.
	static int FindDemo(const std::string& wanted)
	{
		if (!wanted.empty() && std::isdigit((unsigned char)wanted[0]))
		{
			int index = std::atoi(wanted.c_str());
			return (index >= 0 && index < s_DemoCount) ? index : -1;
		}

		for (int d = 0; d < s_DemoCount; d++)
		{
			const char* name = s_Demos[d].ShortName;

			if (std::strlen(name) != wanted.size())
				continue;

			bool same = true;
			for (size_t c = 0; c < wanted.size() && same; c++)
				same = std::tolower((unsigned char)name[c]) == std::tolower((unsigned char)wanted[c]);

			if (same)
				return d;
		}

		return -1;
	}

	bool m_Initialised = false;
	DemoId m_Target = InvalidDemo;
	int m_WarmupSteps = 120;
	int m_Steps = 0;
};
