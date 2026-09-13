// TEMPORARY -- delete after verifying NetServer accepts a connection and a
// real NetClient can connect to it and exchange messages both ways.
#pragma once
#include <GS.h>
#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>

namespace NetServerClientTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	// A bare, hand-built ConnectRequest packet -- exercises NetServer's
	// accept path directly, without a real NetClient yet.
	inline void RunAcceptsAConnection()
	{
		GS::NetServer server;
		Check(server.Host(0), "server hosts on an ephemeral port");
		Check(server.IsHosting(), "IsHosting is true after Host");

		bool connected = false;
		GS::ClientId connectedId = GS::InvalidClient;
		server.OnClientConnected([&](GS::ClientId id) { connected = true; connectedId = id; });

		GS::Socket clientSocket;
		clientSocket.Bind(0);

		GS::ByteStream request;
		GS::Protocol::WriteHeader(request, { 0, 0, 0 });
		request.WriteU8((uint8_t)GS::Reliability::Reliable);
		request.WriteU32(GS::Net::kMsgConnectRequest);
		request.WriteU16(2);
		request.WriteU16(GS::Protocol::kProtocolVersion);

		GS::NetAddress serverAddress{ 0x7F000001u, server.GetLocalPort() };
		clientSocket.SendTo(serverAddress, request.Data(), request.Size());

		for (int i = 0; i < 200 && !connected; i++)
			server.Update(1.0f / 60.0f);

		Check(connected, "server's OnClientConnected fired");
		Check(connectedId != GS::InvalidClient, "a real ClientId was assigned");
		Check(server.GetConnectedClients().size() == 1, "one client is tracked as connected");

		server.Shutdown();
	}

	inline void RunClientConnectsToServer()
	{
		GS::NetServer server;
		Check(server.Host(0), "server hosts");

		GS::NetClient client;
		bool clientConnected = false;
		client.OnConnected([&] { clientConnected = true; });

		Check(client.Connect({ 0x7F000001u, server.GetLocalPort() }), "client starts connecting");

		bool messageArrivedAtServer = false;
		server.OnMessage([&](GS::ClientId, uint32_t typeId, GS::ByteStream&)
		{
			if (typeId == 9999) messageArrivedAtServer = true;
		});

		for (int i = 0; i < 200 && !clientConnected; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}

		Check(clientConnected, "client's OnConnected fired");
		Check(client.GetClientId() != GS::InvalidClient, "client learned its own ClientId");
		Check(client.GetRTT() >= 0.0f, "client has a real (possibly zero) RTT reading");

		uint8_t payload[1] = { 0 };
		client.Send(9999, payload, 1, GS::Reliability::Reliable);

		for (int i = 0; i < 200 && !messageArrivedAtServer; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}

		Check(messageArrivedAtServer, "a client->server message reaches NetServer::OnMessage");

		client.Disconnect();
		server.Shutdown();
	}

	inline void Run() {
		RunAcceptsAConnection();
		RunClientConnectsToServer();
		GS_TRACE("NetServerClientTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
