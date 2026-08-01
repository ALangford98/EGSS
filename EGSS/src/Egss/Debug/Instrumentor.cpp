#include "egsspch.h"
#include "Egss/Debug/Instrumentor.h"

#include "Egss/Log.h"

#include <fstream>

namespace Egss {

	namespace {

		struct InstrumentorState
		{
			std::mutex Mutex;

			// --- Chrome trace ---------------------------------------------
			std::ofstream Output;
			std::string SessionPath;
			bool SessionActive = false;
			unsigned int WrittenCount = 0;

			// --- Live view ------------------------------------------------
			// Names are stable across frames, so an index map keeps the panel
			// from reordering itself every frame.
			std::vector<ProfileEntry> Current;
			std::unordered_map<std::string, size_t> CurrentIndex;

			std::vector<ProfileEntry> LastFrame;
			double LastFrameMicros = 0.0;
		};

		InstrumentorState& State()
		{
			// Function-local static: constructed on first use, which avoids
			// depending on static initialisation order across translation
			// units.
			static InstrumentorState state;
			return state;
		}

		// Trace names end up inside a JSON string, so anything that would
		// terminate it early has to go.
		std::string Sanitise(const std::string& name)
		{
			std::string result = name;
			std::replace(result.begin(), result.end(), '"', '\'');
			std::replace(result.begin(), result.end(), '\n', ' ');
			return result;
		}

	}

	void Instrumentor::BeginSession(const std::string& name, const std::string& filepath)
	{
		InstrumentorState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);

		if (state.SessionActive)
		{
			EGSS_CORE_WARN("Instrumentor: session already running, ignoring BeginSession('{0}')", name);
			return;
		}

		state.Output.open(filepath);
		if (!state.Output.is_open())
		{
			EGSS_CORE_ERROR("Instrumentor: could not open '{0}' for writing", filepath);
			return;
		}

		state.SessionActive = true;
		state.SessionPath = filepath;
		state.WrittenCount = 0;

		// Chrome's trace format: a header, then one object per event.
		state.Output << "{\"otherData\":{\"session\":\"" << Sanitise(name) << "\"},\"traceEvents\":[";
		state.Output.flush();

		EGSS_CORE_INFO("Instrumentor: capturing to '{0}'", filepath);
	}

	void Instrumentor::EndSession()
	{
		InstrumentorState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);

		if (!state.SessionActive)
			return;

		state.Output << "]}";
		state.Output.flush();
		state.Output.close();
		state.SessionActive = false;

		EGSS_CORE_INFO("Instrumentor: wrote {0} events to '{1}'", state.WrittenCount, state.SessionPath);
	}

	bool Instrumentor::IsSessionActive()
	{
		InstrumentorState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);
		return state.SessionActive;
	}

	const std::string& Instrumentor::GetSessionPath()
	{
		return State().SessionPath;
	}

	void Instrumentor::Submit(const ProfileResult& result)
	{
		InstrumentorState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);

		double micros = (double)(result.End - result.Start);

		// --- Live accumulation ---
		auto it = state.CurrentIndex.find(result.Name);
		if (it == state.CurrentIndex.end())
		{
			state.CurrentIndex[result.Name] = state.Current.size();
			state.Current.push_back({ result.Name, 1, micros });
		}
		else
		{
			ProfileEntry& entry = state.Current[it->second];
			entry.Calls++;
			entry.TotalMicros += micros;
		}

		// --- Trace file ---
		if (!state.SessionActive)
			return;

		if (state.WrittenCount++ > 0)
			state.Output << ',';

		state.Output << "{\"cat\":\"function\",\"dur\":" << (result.End - result.Start)
			<< ",\"name\":\"" << Sanitise(result.Name)
			<< "\",\"ph\":\"X\",\"pid\":0,\"tid\":" << result.ThreadID
			<< ",\"ts\":" << result.Start << '}';
	}

	void Instrumentor::NextFrame()
	{
		InstrumentorState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);

		state.LastFrame.swap(state.Current);

		state.LastFrameMicros = 0.0;
		for (const ProfileEntry& entry : state.LastFrame)
			state.LastFrameMicros += entry.TotalMicros;

		state.Current.clear();
		state.CurrentIndex.clear();
	}

	const std::vector<ProfileEntry>& Instrumentor::GetLastFrame()
	{
		return State().LastFrame;
	}

	double Instrumentor::GetLastFrameMicros()
	{
		return State().LastFrameMicros;
	}

}
