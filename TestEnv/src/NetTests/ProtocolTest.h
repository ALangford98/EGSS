// TEMPORARY -- delete after verifying sequence wraparound comparison and
// header round-tripping.
#pragma once
#include <GS.h>
#include <GS/Network/Protocol.h>
#include <GS/Network/ByteStream.h>

namespace ProtocolTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		using namespace GS::Protocol;

		Check(IsMoreRecent(5, 3), "5 is more recent than 3");
		Check(!IsMoreRecent(3, 5), "3 is not more recent than 5");
		Check(IsMoreRecent(0, 65535), "0 is more recent than 65535 (wraparound)");
		Check(!IsMoreRecent(65535, 0), "65535 is not more recent than 0 (wraparound)");
		Check(!IsMoreRecent(10, 10), "a sequence is not more recent than itself");

		PacketHeader header{ 42, 41, 0b101 };
		GS::ByteStream out;
		WriteHeader(out, header);
		GS::ByteStream in(out.Data(), out.Size());
		PacketHeader read = ReadHeader(in);
		Check(read.Sequence == 42 && read.Ack == 41 && read.AckBits == 0b101,
			"header round-trips through a ByteStream");

		GS_TRACE("ProtocolTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
