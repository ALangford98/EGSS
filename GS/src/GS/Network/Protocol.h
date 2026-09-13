#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"
#include "GS/Network/ByteStream.h"

namespace GS::Protocol {

	constexpr uint16_t kProtocolVersion = 1;
	// Comfortably under a common 1500-byte MTU minus IP/UDP headers -- avoids
	// IP fragmentation. A single message bigger than this is not supported;
	// nothing this project sends comes close.
	constexpr size_t kMaxPacketSize = 1200;

	struct PacketHeader
	{
		uint16_t Sequence = 0;
		uint16_t Ack = 0;
		uint32_t AckBits = 0;
	};

	GS_API void WriteHeader(ByteStream& out, const PacketHeader& header);
	GS_API PacketHeader ReadHeader(ByteStream& in);

	// True if `a` is more recent than `b`, in the presence of uint16
	// wraparound -- the standard TCP-style comparison: two sequences within
	// half the numeric range of each other are ordered normally; further
	// apart than that is treated as a wrap.
	GS_API bool IsMoreRecent(uint16_t a, uint16_t b);

}
