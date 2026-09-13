// TEMPORARY -- delete after verifying ByteStream round-trips every type it
// supports, in order, in one buffer.
#pragma once
#include <GS.h>
#include <GS/Network/ByteStream.h>

namespace ByteStreamTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::ByteStream out;
		out.WriteU8(200);
		out.WriteU16(60000);
		out.WriteU32(4000000000u);
		out.WriteFloat(3.5f);
		out.WriteString("hello");
		out.WriteVec3({ 1.0f, -2.5f, 0.0f });

		GS::ByteStream in(out.Data(), out.Size());
		Check(in.ReadU8() == 200, "u8 round-trips");
		Check(in.ReadU16() == 60000, "u16 round-trips");
		Check(in.ReadU32() == 4000000000u, "u32 round-trips");
		Check(in.ReadFloat() == 3.5f, "float round-trips");
		Check(in.ReadString() == "hello", "string round-trips");
		glm::vec3 v = in.ReadVec3();
		Check(v.x == 1.0f && v.y == -2.5f && v.z == 0.0f, "vec3 round-trips");
		Check(in.AtEnd(), "read cursor consumes exactly what was written");

		GS::ByteStream skip;
		skip.WriteU32(111);
		skip.WriteU32(222);
		GS::ByteStream skipIn(skip.Data(), skip.Size());
		skipIn.Skip(4);
		Check(skipIn.ReadU32() == 222, "Skip advances past a field's bytes");

		GS_TRACE("ByteStreamTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
