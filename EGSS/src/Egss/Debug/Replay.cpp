#include "egsspch.h"
#include "Egss/Debug/Replay.h"

#include "Egss/Application.h"
#include "Egss/Log.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

#include <fstream>
#include <filesystem>

namespace Egss {

	namespace {

		// "EGSSREC" plus a format version, so a file from an older layout is
		// refused with a message rather than read as garbage.
		const char s_Magic[8] = { 'E', 'G', 'S', 'S', 'R', 'E', 'C', '1' };

		struct Header
		{
			char Magic[8];
			unsigned int Version;
			int DemoIndex;
			float FixedTimestep;
			unsigned long long TotalSteps;
		};

		// One record is a step number and the state from that step onwards.
		// Written only when the state changes, which for a session where
		// somebody holds a key for a second is two records rather than sixty.
		struct Record
		{
			unsigned long long Step;
			InputSnapshot State;
		};

		Replay::Mode s_Mode = Replay::Mode::Off;

		std::ofstream s_Out;
		std::string s_OutPath;
		unsigned long long s_RecordedSteps = 0;
		bool s_HaveLast = false;
		InputSnapshot s_LastWritten;

		std::vector<Record> s_Records;
		size_t s_NextRecord = 0;
		Header s_PlaybackHeader = {};

		// The snapshot Input borrows a pointer to. Rewritten in place each
		// step rather than reassigned, so the pointer handed to Input stays
		// valid for the whole run.
		InputSnapshot s_Current;
		InputSnapshot s_Previous;

		bool s_Dispatching = false;
		unsigned long long s_Step = 0;

		// Pushes one synthesised event through the normal path, flagged so
		// Application can tell it from a live one.
		void Dispatch(Event& e)
		{
			s_Dispatching = true;
			Application::Get().OnEvent(e);
			s_Dispatching = false;
		}

	}

	bool Replay::StartRecording(const std::string& path, int demoIndex, float fixedTimestep)
	{
		// Recording and playing at once is not a mode. Asking for both used to
		// stop the playback and record the resulting *inputless* run, which
		// produced a plausible file with nothing in it -- the worst kind of
		// wrong answer. Refusing keeps the replay running, which is the half
		// the caller more likely meant.
		if (s_Mode == Mode::Playing)
		{
			EGSS_CORE_ERROR("Replay: cannot record while playing back; ignoring --record");
			return false;
		}

		Stop();

		std::error_code error;
		std::filesystem::path target(path);
		if (target.has_parent_path())
			std::filesystem::create_directories(target.parent_path(), error);

		s_Out.open(path, std::ios::binary | std::ios::trunc);
		if (!s_Out)
		{
			EGSS_CORE_ERROR("Replay: could not open '{0}' for recording", path);
			return false;
		}

		Header header = {};
		std::memcpy(header.Magic, s_Magic, sizeof(s_Magic));
		header.Version = 1;
		header.DemoIndex = demoIndex;
		header.FixedTimestep = fixedTimestep;
		header.TotalSteps = 0;   // patched by Stop, once it is known

		s_Out.write(reinterpret_cast<const char*>(&header), sizeof(header));

		s_OutPath = path;
		s_RecordedSteps = 0;
		s_HaveLast = false;
		s_Mode = Mode::Recording;

		EGSS_CORE_INFO("Replay: recording to '{0}' (demo {1}, step {2:.5f}s)",
			path, demoIndex, fixedTimestep);
		return true;
	}

	bool Replay::StartPlayback(const std::string& path)
	{
		Stop();

		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			EGSS_CORE_ERROR("Replay: could not open '{0}' for playback", path);
			return false;
		}

		Header header = {};
		in.read(reinterpret_cast<char*>(&header), sizeof(header));

		if (!in || std::memcmp(header.Magic, s_Magic, sizeof(s_Magic)) != 0)
		{
			EGSS_CORE_ERROR("Replay: '{0}' is not an EGSS recording", path);
			return false;
		}

		if (header.Version != 1)
		{
			EGSS_CORE_ERROR("Replay: '{0}' is version {1}, this build reads 1", path, header.Version);
			return false;
		}

		s_Records.clear();

		Record record;
		while (in.read(reinterpret_cast<char*>(&record), sizeof(record)))
			s_Records.push_back(record);

		s_PlaybackHeader = header;
		s_NextRecord = 0;
		s_Current = InputSnapshot();
		s_Previous = InputSnapshot();
		s_Mode = Mode::Playing;

		Input::SetPlaybackSnapshot(&s_Current);

		EGSS_CORE_INFO("Replay: playing '{0}' -- demo {1}, {2} steps, {3} records",
			path, header.DemoIndex, header.TotalSteps, s_Records.size());
		return true;
	}

	void Replay::Stop()
	{
		if (s_Mode == Mode::Recording && s_Out.is_open())
		{
			// The step count is only known now, so seek back and fill it in.
			// Playback needs it to know when the recording has run out, and
			// scanning the whole file to find the last record would work but
			// says nothing about steps after the final *change*.
			s_Out.seekp(offsetof(Header, TotalSteps), std::ios::beg);
			s_Out.write(reinterpret_cast<const char*>(&s_RecordedSteps), sizeof(s_RecordedSteps));
			s_Out.close();

			EGSS_CORE_INFO("Replay: recorded {0} steps to '{1}'", s_RecordedSteps, s_OutPath);
		}

		if (s_Mode == Mode::Playing)
			Input::SetPlaybackSnapshot(nullptr);

		s_Mode = Mode::Off;
	}

	Replay::Mode Replay::GetMode() { return s_Mode; }
	bool Replay::IsRecording() { return s_Mode == Mode::Recording; }
	bool Replay::IsPlaying() { return s_Mode == Mode::Playing; }
	bool Replay::IsDispatchingSyntheticEvent() { return s_Dispatching; }

	int Replay::GetRecordedDemoIndex()
	{
		return s_Mode == Mode::Playing ? s_PlaybackHeader.DemoIndex : -1;
	}

	unsigned long long Replay::GetTotalSteps()
	{
		return s_Mode == Mode::Playing ? s_PlaybackHeader.TotalSteps : 0;
	}

	bool Replay::PlaybackFinished()
	{
		return s_Mode == Mode::Playing && s_Step >= s_PlaybackHeader.TotalSteps;
	}

	void Replay::BeginStep(unsigned long long step)
	{
		s_Step = step;

		if (s_Mode == Mode::Recording)
		{
			InputSnapshot now = Input::CaptureSnapshot();
			s_RecordedSteps = step + 1;

			// Only on change. The first step always writes, so playback has a
			// state to start from even if nothing is ever pressed.
			if (!s_HaveLast || now != s_LastWritten)
			{
				Record record{ step, now };
				s_Out.write(reinterpret_cast<const char*>(&record), sizeof(record));

				s_LastWritten = now;
				s_HaveLast = true;
			}

			return;
		}

		if (s_Mode != Mode::Playing)
			return;

		s_Previous = s_Current;

		// Advance through every record due by now. More than one can fall on
		// the same step only in a corrupt file, but consuming them all keeps
		// the cursor honest rather than stalling.
		while (s_NextRecord < s_Records.size() && s_Records[s_NextRecord].Step <= step)
		{
			s_Current = s_Records[s_NextRecord].State;
			s_NextRecord++;
		}

		// Edges become events, so code that listens sees what code that polls
		// sees. A press that begins and ends inside one step is invisible here
		// by construction -- see the note in the header.
		for (int key = 0; key < InputSnapshot::MaxKeys; key++)
		{
			bool was = s_Previous.GetKey(key);
			bool is = s_Current.GetKey(key);
			if (was == is)
				continue;

			if (is)
			{
				KeyPressedEvent pressed(key, 0);
				Dispatch(pressed);
			}
			else
			{
				KeyReleasedEvent released(key);
				Dispatch(released);
			}
		}

		for (int button = 0; button < InputSnapshot::MaxMouseButtons; button++)
		{
			bool was = s_Previous.GetMouseButton(button);
			bool is = s_Current.GetMouseButton(button);
			if (was == is)
				continue;

			if (is)
			{
				MouseButtonPressedEvent pressed(button);
				Dispatch(pressed);
			}
			else
			{
				MouseButtonReleasedEvent released(button);
				Dispatch(released);
			}
		}

		if (s_Current.MouseX != s_Previous.MouseX || s_Current.MouseY != s_Previous.MouseY)
		{
			MouseMovedEvent moved(s_Current.MouseX, s_Current.MouseY);
			Dispatch(moved);
		}
	}

}
