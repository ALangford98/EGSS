#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	// The simulation parameters a replay has to carry, because they do not
	// arrive as input.
	//
	// `Replay` records what a person does *through the keyboard and mouse*, and
	// that is genuinely everything a game reads -- until a debug panel exists. An
	// ImGui slider writes a variable directly: nothing about moving "Gravity"
	// from -9.8 to -2 passes through `Input`, so nothing about it was recorded,
	// and a replay of that session ran the whole thing at -9.8. The recording was
	// not wrong about the input. It simply described a different simulation.
	//
	// So a parameter is registered once, by pointer, and from then on the
	// recorder samples it every fixed step and the player writes it back. Which
	// makes the rule for a demo author one line per slider rather than a
	// serialisation method per demo -- and the parameters are then named in the
	// file, so a recording says what it was made with.
	//
	// **Sampled per fixed step, exactly like input**, which inherits the same
	// honest limitation: a slider nudged and returned inside one step is not
	// seen, in the same way a key tapped between two steps is not. At 60 Hz that
	// is 16 ms of dragging, and the alternative is a parameter stream with its
	// own clock, which is a second definition of when things happen.
	//
	// Registration is by raw pointer, so **whatever is registered has to outlive
	// the recording**. Demo layers live for the whole run, which is what makes
	// that safe here; anything shorter-lived should not be registered at all.
	class EGSS_API ReplayParams
	{
	public:
		// Four bytes and a type. A parameter is stored as its bit pattern rather
		// than as a variant, because the file wants a fixed-width field and the
		// only thing the recorder needs to know is whether two samples differ.
		enum class Kind : unsigned int
		{
			Float = 0,
			Int = 1,
			Bool = 2
		};

		static void RegisterFloat(const std::string& name, float* value);
		static void RegisterInt(const std::string& name, int* value);
		static void RegisterBool(const std::string& name, bool* value);

		// Registering the same name twice replaces the first -- a demo rebuilt
		// mid-run should not accumulate stale pointers.
		static void Clear();

		static size_t Count();
		static const std::string& NameAt(size_t index);
		static Kind KindAt(size_t index);

		// -1 when a recording names a parameter this build does not have, which
		// is what an old file against new code looks like. The caller decides
		// whether that is fatal; it is not, on its own.
		static int FindByName(const std::string& name);

		// The current value as raw bits, and back. Bool reads as 0 or 1.
		static unsigned int ReadBits(size_t index);
		static void WriteBits(size_t index, unsigned int bits);
	};

}
