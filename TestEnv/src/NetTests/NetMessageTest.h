// TEMPORARY -- delete after verifying RPC registration/dispatch and that
// the message-name hash never collides with the reserved built-in IDs.
#pragma once
#include <GS.h>
#include <GS/Network/NetMessage.h>
#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>

namespace NetMessageTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		Check(GS::Net::HashMessageName("BallBounced") != GS::Net::HashMessageName("Score"),
			"different names hash differently");
		Check(GS::Net::HashMessageName("BallBounced") > 100,
			"hashed IDs stay clear of the low reserved built-in range (1-3)");

		GS::NetServer server;
		server.Host(0);
		GS::NetClient client;
		client.Connect({ 0x7F000001u, server.GetLocalPort() });

		bool connected = false;
		client.OnConnected([&] { connected = true; });
		for (int i = 0; i < 200 && !connected; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}
		Check(connected, "client connected (setup for the RPC check below)");

		bool serverGotIt = false;
		int receivedValue = 0;
		GS::Net::RegisterHandler("TestEvent", [&](GS::ClientId, GS::ByteStream& payload)
		{
			serverGotIt = true;
			receivedValue = payload.ReadU32();
		});

		GS::ByteStream payload;
		payload.WriteU32(4242);
		GS::Net::SendRPC(client, "TestEvent", payload, GS::Reliability::Reliable);

		for (int i = 0; i < 200 && !serverGotIt; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}

		Check(serverGotIt, "server's registered handler fired for a client-sent RPC");
		Check(receivedValue == 4242, "the RPC payload arrived intact");

		client.Disconnect();
		server.Shutdown();
		GS_TRACE("NetMessageTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
