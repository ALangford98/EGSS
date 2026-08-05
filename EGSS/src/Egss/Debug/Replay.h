#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Input.h"

namespace Egss {

	// Recording and playback of input, in the sense Quake and Doom meant by a
	// `.dem` file: not a video, but the input that produced one, replayed
	// through the same simulation to reproduce it exactly.
	//
	// Called "replay" rather than "demo" only because `TestEnv` already uses
	// "demo" for Breakout, Physics2D and the rest, and one word for two things
	// in the same codebase is how you end up reading the wrong file.
	//
	// **Input is sampled once per fixed step, never per frame.** That is the
	// whole trick. Frames come and go at whatever rate the machine manages;
	// steps are the simulation's own clock, so a recording indexed by them
	// replays the same way on a slower machine, on a faster one, and with the
	// window being dragged around mid-run.
	//
	// The cost of that choice, stated plainly: a key tapped and released
	// entirely between two steps is never seen, so it is never recorded. At
	// 60 Hz that is a 16 ms press. Doom had the same property for the same
	// reason and nobody minded.
	//
	// What is *not* captured, and would desynchronise a replay if it changed
	// mid-recording: ImGui slider values, which mutate simulation parameters
	// directly rather than going through input. Record from defaults, or
	// expect divergence from the moment a slider moves.
	class EGSS_API Replay
	{
	public:
		enum class Mode
		{
			Off = 0,
			Recording,
			Playing
		};

		// `demoIndex` is stored so playback can select the same scene without
		// the caller having to remember which one the file belongs to.
		static bool StartRecording(const std::string& path, int demoIndex, float fixedTimestep);
		static bool StartPlayback(const std::string& path);

		// Flushes and patches the header. Safe to call when nothing is active.
		static void Stop();

		static Mode GetMode();
		static bool IsRecording();
		static bool IsPlaying();

		// From the file header, once playback has started. -1 if unknown.
		static int GetRecordedDemoIndex();

		// The step this recording ends at. Playback past it is finished.
		static unsigned long long GetTotalSteps();
		static bool PlaybackFinished();

		// Called by Application once per fixed step, before the layers run.
		//
		// Recording: samples the effective input and appends it if it changed.
		// Playing: installs the recorded snapshot for this step and dispatches
		// the press/release events its edges imply, so code that listens for
		// events sees the same thing code that polls does.
		static void BeginStep(unsigned long long step);

		// True while Replay is pushing a synthesised event through
		// Application::OnEvent. Live hardware events are dropped during
		// playback, and this is how the two are told apart.
		static bool IsDispatchingSyntheticEvent();
	};

}
