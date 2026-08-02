#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	// An entity is an integer, not an object.
	//
	// The low bits index a slot; the high bits are a generation counter bumped
	// every time that slot is reused. That is what makes a handle to a dead
	// entity *detectably* dead rather than silently pointing at whatever was
	// created next -- the same lesson the audio VoiceHandle taught, and worth
	// paying for again here because entity handles get stored far more widely.
	using EntityId = unsigned int;

	constexpr EntityId InvalidEntity = 0;

	namespace EntityIds {

		// 20 bits of index, 12 of generation: a million live entities and 4096
		// reuses before a handle can alias. Generation 0 is reserved so that
		// InvalidEntity is never a real handle.
		constexpr unsigned int IndexBits = 20;
		constexpr unsigned int IndexMask = (1u << IndexBits) - 1;
		constexpr unsigned int MaxGeneration = (1u << (32 - IndexBits)) - 1;

		inline EntityId Make(unsigned int index, unsigned int generation)
		{
			return (generation << IndexBits) | (index & IndexMask);
		}

		inline unsigned int Index(EntityId id) { return id & IndexMask; }
		inline unsigned int Generation(EntityId id) { return id >> IndexBits; }

	}

}
