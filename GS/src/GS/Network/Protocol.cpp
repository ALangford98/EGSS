#include "gspch.h"
#include "GS/Network/Protocol.h"

namespace GS::Protocol {

	void WriteHeader(ByteStream& out, const PacketHeader& header)
	{
		out.WriteU16(header.Sequence);
		out.WriteU16(header.Ack);
		out.WriteU32(header.AckBits);
	}

	PacketHeader ReadHeader(ByteStream& in)
	{
		PacketHeader header;
		header.Sequence = in.ReadU16();
		header.Ack = in.ReadU16();
		header.AckBits = in.ReadU32();
		return header;
	}

	bool IsMoreRecent(uint16_t a, uint16_t b)
	{
		return (a != b) &&
			(((a > b) && (uint16_t)(a - b) <= 32768) ||
			 ((a < b) && (uint16_t)(b - a) > 32768));
	}

}
