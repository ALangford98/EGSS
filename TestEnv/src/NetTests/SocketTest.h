// TEMPORARY -- delete after verifying two real UDP sockets on loopback can
// send and receive.
#pragma once
#include <GS.h>
#include <GS/Network/Socket.h>
#include <cstring>

namespace SocketTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::NetAddress parsed;
		Check(GS::ParseAddress("127.0.0.1:9001", parsed), "ParseAddress accepts host:port");
		Check(parsed.Port == 9001, "ParseAddress reads the port");

		GS::Socket a, b;
		Check(a.Bind(0), "socket a binds an ephemeral port");
		Check(b.Bind(0), "socket b binds an ephemeral port");
		Check(a.LocalPort() != 0 && b.LocalPort() != 0, "both got a real port");

		GS::NetAddress toB{ parsed.IP, b.LocalPort() };
		const char* msg = "ping";
		Check(a.SendTo(toB, (const uint8_t*)msg, 4), "SendTo succeeds");

		uint8_t buffer[64];
		GS::NetAddress from;
		int received = 0;
		for (int attempt = 0; attempt < 1000 && received == 0; attempt++)
			received = b.RecvFrom(buffer, sizeof(buffer), from);

		Check(received == 4, "b receives exactly what a sent");
		Check(std::memcmp(buffer, "ping", 4) == 0, "payload bytes match");
		Check(from.Port == a.LocalPort(), "sender address round-trips");

		int nothing = b.RecvFrom(buffer, sizeof(buffer), from);
		Check(nothing == 0, "RecvFrom returns 0 when nothing is waiting");

		GS_TRACE("SocketTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
