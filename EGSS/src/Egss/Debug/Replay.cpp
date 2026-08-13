#include "egsspch.h"
#include "Egss/Debug/Replay.h"

#include "Egss/Application.h"
#include "Egss/Debug/ReplayParams.h"
#include "Egss/Log.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

#include <fstream>
#include <filesystem>

namespace Egss {

	namespace {

		// "EGSSREC" plus a trailing character, so a file from an older layout is
		// refused with a message rather than read as garbage.
		//
		// The trailing '1' is **not** the format version -- `Header::Version` is,
		// and it is 2. The magic is deliberately left alone across version bumps:
		// changing it would make an older build report "not an EGSS recording",
		// which sends the reader looking for a corrupt file rather than for the
		// version mismatch it actually is.
		const char s_Magic[8] = { 'E', 'G', 'S', 'S', 'R', 'E', 'C', '1' };
		constexpr unsigned int s_Version = 2;

		// Names are fixed width so the table can be read as a block. 48 is
		// comfortably past "MapBuilding/Motor stiffness" and costs nothing worth
		// counting -- the table is written once per file.
		constexpr size_t s_MaxParamName = 48;

		struct Header
		{
			char Magic[8];
			unsigned int Version;
			int DemoIndex;
			float FixedTimestep;
			unsigned long long TotalSteps;
			// Version 2. The parameters this recording carries, declared once
			// here so the body can refer to them by index instead of by name.
			unsigned int ParamCount;
		};

		struct ParamEntry
		{
			char Name[s_MaxParamName];
			unsigned int Kind;
		};

		// The body is a stream of tagged chunks rather than one record type,
		// because there are now two things that change over time and they change
		// at different moments -- interleaving them by step keeps one clock.
		enum class ChunkTag : unsigned char
		{
			Input = 0,
			Params = 1
		};

		// One input record is a step number and the state from that step
		// onwards. Written only when the state changes, which for a session
		// where somebody holds a key for a second is two records rather than
		// sixty.
		struct InputChunk
		{
			unsigned long long Step;
			InputSnapshot State;
		};

		// One changed parameter. A step writes as many of these as changed,
		// preceded by the count -- on step 0 that is all of them, so a replay
		// starts from the values the recording started from rather than from
		// whatever the code's defaults happen to be today.
		struct ParamChange
		{
			unsigned int Index;
			unsigned int Bits;
		};

		Replay::Mode s_Mode = Replay::Mode::Off;

		std::ofstream s_Out;
		std::string s_OutPath;
		unsigned long long s_RecordedSteps = 0;
		bool s_HaveLast = false;
		bool s_HaveParams = false;
		InputSnapshot s_LastWritten;

		// Last sampled value per registered parameter, so only changes are
		// written. Sized at StartRecording and indexed the same way the file is.
		std::vector<unsigned int> s_LastParams;

		std::vector<InputChunk> s_Records;
		size_t s_NextRecord = 0;
		Header s_PlaybackHeader = {};

		// Parameter changes in file order, with the step each lands on. Kept
		// flat rather than grouped per step: the player walks a cursor forward,
		// which is the same shape as the input cursor beside it.
		struct TimedParam
		{
			unsigned long long Step;
			ParamChange Change;
		};

		std::vector<TimedParam> s_ParamRecords;
		size_t s_NextParamRecord = 0;

		// The names the file declares, and file index -> this build's registry
		// index. Mapped by name rather than by position: indices are a file's own
		// numbering and mean nothing across builds, so registering one more
		// slider in a demo would otherwise shift every parameter after it and
		// replay a recording with its values shuffled into the wrong variables.
		//
		// A name this build no longer has maps to -1 and is skipped, which is
		// what lets an old recording still replay its input.
		std::vector<std::string> s_ParamNames;
		std::vector<int> s_ParamMapping;
		bool s_ParamsResolved = false;

		// Deferred until the first step, because playback starts before the
		// layers that register parameters have been built. See StartPlayback.
		void ResolveParamMapping()
		{
			s_ParamsResolved = true;
			s_ParamMapping.clear();

			int missing = 0;
			for (const std::string& name : s_ParamNames)
			{
				int index = ReplayParams::FindByName(name);
				if (index < 0)
				{
					missing++;
					EGSS_CORE_WARN("Replay: this build has no parameter '{0}'", name);
				}

				s_ParamMapping.push_back(index);
			}

			if (missing > 0)
			{
				// Not fatal: the input still replays, and so does every parameter
				// that does still exist. Said out loud because the run will then
				// differ from the recording and the reason should not have to be
				// guessed at from the picture.
				EGSS_CORE_WARN("Replay: {0} of {1} recorded parameters are missing;"
					" they keep whatever this build's defaults are",
					missing, s_ParamNames.size());
			}
		}

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
		header.Version = s_Version;
		header.DemoIndex = demoIndex;
		header.FixedTimestep = fixedTimestep;
		header.TotalSteps = 0;   // patched by Stop, once it is known
		header.ParamCount = (unsigned int)ReplayParams::Count();

		s_Out.write(reinterpret_cast<const char*>(&header), sizeof(header));

		// The table, once. Whatever is registered when recording starts is what
		// this file describes -- which is why recording must not start before
		// the demos that register have been built.
		for (size_t i = 0; i < ReplayParams::Count(); i++)
		{
			ParamEntry entry = {};

			const std::string& name = ReplayParams::NameAt(i);
			if (name.size() >= s_MaxParamName)
			{
				EGSS_CORE_WARN("Replay: parameter name '{0}' is longer than {1} and"
					" will be truncated in the file", name, s_MaxParamName - 1);
			}

			std::strncpy(entry.Name, name.c_str(), s_MaxParamName - 1);
			entry.Kind = (unsigned int)ReplayParams::KindAt(i);

			s_Out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
		}

		// Deliberately not sampled here. The first step writes every parameter
		// because nothing has been written yet, and sampling now would record
		// values from before the demo's first update had run.
		s_LastParams.assign(ReplayParams::Count(), 0);
		s_HaveParams = false;

		s_OutPath = path;
		s_RecordedSteps = 0;
		s_HaveLast = false;
		s_Mode = Mode::Recording;

		EGSS_CORE_INFO("Replay: recording to '{0}' (demo {1}, step {2:.5f}s, {3} parameters)",
			path, demoIndex, fixedTimestep, header.ParamCount);
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

		if (header.Version != s_Version)
		{
			EGSS_CORE_ERROR("Replay: '{0}' is version {1}, this build reads {2}",
				path, header.Version, s_Version);
			return false;
		}

		s_Records.clear();
		s_ParamRecords.clear();
		s_ParamMapping.clear();

		// The table is read now and *mapped later*. Playback starts from the
		// Application constructor, because TestEnv asks the recording which
		// scene it belongs to before it builds its layers -- and those layers
		// are what register the parameters. Resolving names here would find an
		// empty registry and report every parameter missing, which is precisely
		// what the first version of this did.
		s_ParamNames.clear();

		for (unsigned int i = 0; i < header.ParamCount; i++)
		{
			ParamEntry entry = {};
			if (!in.read(reinterpret_cast<char*>(&entry), sizeof(entry)))
			{
				EGSS_CORE_ERROR("Replay: '{0}' ends inside its parameter table", path);
				return false;
			}

			// A name written at full width has no terminator.
			entry.Name[s_MaxParamName - 1] = '\0';
			s_ParamNames.push_back(entry.Name);
		}

		s_ParamsResolved = false;

		// The body: tagged chunks, in step order.
		unsigned char tag = 0;
		while (in.read(reinterpret_cast<char*>(&tag), sizeof(tag)))
		{
			if ((ChunkTag)tag == ChunkTag::Input)
			{
				InputChunk chunk;
				if (!in.read(reinterpret_cast<char*>(&chunk), sizeof(chunk)))
					break;

				s_Records.push_back(chunk);
			}
			else if ((ChunkTag)tag == ChunkTag::Params)
			{
				unsigned long long step = 0;
				unsigned int count = 0;

				if (!in.read(reinterpret_cast<char*>(&step), sizeof(step)))
					break;
				if (!in.read(reinterpret_cast<char*>(&count), sizeof(count)))
					break;

				for (unsigned int i = 0; i < count; i++)
				{
					ParamChange change = {};
					if (!in.read(reinterpret_cast<char*>(&change), sizeof(change)))
						break;

					s_ParamRecords.push_back({ step, change });
				}
			}
			else
			{
				// An unknown tag means the rest of the stream cannot be located,
				// since chunks are variable width and only the tag says how wide
				// this one is. Stop rather than guess.
				EGSS_CORE_ERROR("Replay: '{0}' has an unknown chunk tag {1}; stopping"
					" after {2} input records", path, (int)tag, s_Records.size());
				break;
			}
		}

		s_PlaybackHeader = header;
		s_NextRecord = 0;
		s_NextParamRecord = 0;
		s_Current = InputSnapshot();
		s_Previous = InputSnapshot();
		s_Mode = Mode::Playing;

		Input::SetPlaybackSnapshot(&s_Current);

		EGSS_CORE_INFO("Replay: playing '{0}' -- demo {1}, {2} steps, {3} input records,"
			" {4} parameter changes over {5} parameters",
			path, header.DemoIndex, header.TotalSteps, s_Records.size(),
			s_ParamRecords.size(), header.ParamCount);
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
				unsigned char tag = (unsigned char)ChunkTag::Input;
				InputChunk chunk{ step, now };

				s_Out.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
				s_Out.write(reinterpret_cast<const char*>(&chunk), sizeof(chunk));

				s_LastWritten = now;
				s_HaveLast = true;
			}

			// The same rule for parameters: everything on the first step, only
			// what moved after that. A slider held still costs nothing, and a
			// slider dragged for a second costs one entry per step it changed
			// on, which is what it genuinely is.
			std::vector<ParamChange> changed;
			for (size_t i = 0; i < s_LastParams.size(); i++)
			{
				unsigned int bits = ReplayParams::ReadBits(i);
				if (s_HaveParams && bits == s_LastParams[i])
					continue;

				s_LastParams[i] = bits;
				changed.push_back({ (unsigned int)i, bits });
			}
			s_HaveParams = true;

			if (!changed.empty())
			{
				unsigned char tag = (unsigned char)ChunkTag::Params;
				unsigned int count = (unsigned int)changed.size();

				s_Out.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
				s_Out.write(reinterpret_cast<const char*>(&step), sizeof(step));
				s_Out.write(reinterpret_cast<const char*>(&count), sizeof(count));
				s_Out.write(reinterpret_cast<const char*>(changed.data()),
					changed.size() * sizeof(ParamChange));
			}

			return;
		}

		if (s_Mode != Mode::Playing)
			return;

		if (!s_ParamsResolved)
			ResolveParamMapping();

		// Parameters before input, so a handler that reacts to a key this step
		// reads the values the recording had when that key went down rather than
		// the previous step's.
		while (s_NextParamRecord < s_ParamRecords.size()
			&& s_ParamRecords[s_NextParamRecord].Step <= step)
		{
			const TimedParam& timed = s_ParamRecords[s_NextParamRecord];
			s_NextParamRecord++;

			int index = timed.Change.Index < s_ParamMapping.size()
				? s_ParamMapping[timed.Change.Index] : -1;

			if (index >= 0)
				ReplayParams::WriteBits((size_t)index, timed.Change.Bits);
		}

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
