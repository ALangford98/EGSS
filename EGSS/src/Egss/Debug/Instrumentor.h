#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <chrono>
#include <mutex>
#include <thread>

namespace Egss {

	// One completed timing, as produced by a scope timer.
	struct ProfileResult
	{
		std::string Name;
		// Microseconds on a steady clock.
		long long Start = 0;
		long long End = 0;
		unsigned int ThreadID = 0;
	};

	// Per-name totals for one frame, for the live view.
	struct ProfileEntry
	{
		std::string Name;
		unsigned int Calls = 0;
		double TotalMicros = 0.0;
	};

	// Two outputs from one set of timers:
	//
	//   * a **live** per-frame summary, which is what you want when the
	//     question is "what is this frame spending its time on";
	//   * a **Chrome trace** written to JSON, which is what you want when the
	//     question is "what happened during those three bad seconds". Load it
	//     into chrome://tracing or ui.perfetto.dev.
	//
	// The live view exists because VSync hides everything: with the swap
	// blocking until the display is ready, every frame reads the same 16.7ms
	// whether the work took 0.2ms or 12ms. Timing the scopes inside the frame
	// is the only way to see the difference.
	class EGSS_API Instrumentor
	{
	public:
		// Starts writing a Chrome trace. The live view runs regardless.
		static void BeginSession(const std::string& name, const std::string& filepath = "profile.json");
		static void EndSession();
		static bool IsSessionActive();
		static const std::string& GetSessionPath();

		// Called by the scope timers; safe from any thread.
		static void Submit(const ProfileResult& result);

		// Rolls the accumulator: this frame's totals become "last frame" and a
		// fresh one starts. Application::Run calls it once per frame.
		static void NextFrame();

		static const std::vector<ProfileEntry>& GetLastFrame();
		// Sum of every scope's total. Nested scopes are counted at each level,
		// so this is not "time in the frame" -- read the enclosing scope's own
		// entry for that.
		static double GetLastFrameMicros();
	};

	// RAII: times from construction to destruction, then submits.
	class EGSS_API InstrumentationTimer
	{
	public:
		InstrumentationTimer(const char* name)
			: m_Name(name), m_Start(std::chrono::steady_clock::now())
		{
		}

		~InstrumentationTimer()
		{
			auto end = std::chrono::steady_clock::now();

			ProfileResult result;
			result.Name = m_Name;
			result.Start = std::chrono::time_point_cast<std::chrono::microseconds>(m_Start)
				.time_since_epoch().count();
			result.End = std::chrono::time_point_cast<std::chrono::microseconds>(end)
				.time_since_epoch().count();
			result.ThreadID = (unsigned int)std::hash<std::thread::id>{}(std::this_thread::get_id());

			Instrumentor::Submit(result);
		}
	private:
		const char* m_Name;
		std::chrono::time_point<std::chrono::steady_clock> m_Start;
	};

}

// Compiled out entirely in Dist, so shipping builds carry no timing code.
#ifdef EGSS_PROFILE

	#if defined(__GNUC__) || defined(__clang__)
		#define EGSS_FUNC_SIG __PRETTY_FUNCTION__
	#elif defined(_MSC_VER)
		#define EGSS_FUNC_SIG __FUNCSIG__
	#else
		#define EGSS_FUNC_SIG "unknown"
	#endif

	// Two levels of indirection so __LINE__ expands before pasting, giving
	// each timer in a scope a distinct variable name.
	#define EGSS_PROFILE_JOIN2(a, b) a##b
	#define EGSS_PROFILE_JOIN(a, b) EGSS_PROFILE_JOIN2(a, b)

	#define EGSS_PROFILE_SCOPE(name) ::Egss::InstrumentationTimer EGSS_PROFILE_JOIN(egssTimer, __LINE__)(name)
	// Handy, but the name is a full signature -- prefer an explicit short one
	// for anything you intend to read in the live panel.
	#define EGSS_PROFILE_FUNCTION() EGSS_PROFILE_SCOPE(EGSS_FUNC_SIG)

	#define EGSS_PROFILE_BEGIN_SESSION(name, path) ::Egss::Instrumentor::BeginSession(name, path)
	#define EGSS_PROFILE_END_SESSION() ::Egss::Instrumentor::EndSession()
#else
	#define EGSS_PROFILE_SCOPE(name)
	#define EGSS_PROFILE_FUNCTION()
	#define EGSS_PROFILE_BEGIN_SESSION(name, path)
	#define EGSS_PROFILE_END_SESSION()
#endif
