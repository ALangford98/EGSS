// TEMPORARY -- delete after verifying NetConnection's reliable channel
// delivers exactly-once, in order, over a real loopback socket pair, with
// and without simulated loss, and that RTT measures a known injected delay.
#pragma once
#include <GS.h>
#include <GS/Network/NetConnection.h>
#include <GS/Network/Socket.h>

namespace NetConnectionTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	// Pumps two real, loopback-connected NetConnections against each other
	// for `steps` fixed steps of `dt` seconds, feeding whatever arrives on
	// each socket into the matching connection's ReceivePacket.
	inline void PumpBothWays(GS::Socket& socketA, GS::NetConnection& a,
		GS::Socket& socketB, GS::NetConnection& b, int steps, float dt,
		const GS::NetConnection::MessageCallback& onA,
		const GS::NetConnection::MessageCallback& onB)
	{
		for (int i = 0; i < steps; i++)
		{
			a.Update(dt);
			b.Update(dt);
			a.Flush(dt);
			b.Flush(dt);

			uint8_t buffer[2048];
			GS::NetAddress from;
			int received;
			while ((received = socketB.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				b.ReceivePacket(buffer, received, onB);
			while ((received = socketA.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				a.ReceivePacket(buffer, received, onA);
		}
	}

	inline void RunReliableDeliveryUnderLoss(float lossProbability)
	{
		GS::Socket socketA, socketB;
		socketA.Bind(0);
		socketB.Bind(0);

		GS::NetAddress toB{ 0x7F000001u, socketB.LocalPort() };
		GS::NetAddress toA{ 0x7F000001u, socketA.LocalPort() };

		GS::NetConnection a(&socketA, toB);
		GS::NetConnection b(&socketB, toA);
		a.SimulateLoss(lossProbability);
		b.SimulateLoss(lossProbability);

		std::vector<uint32_t> received;
		auto onB = [&](uint32_t typeId, GS::ByteStream&) { received.push_back(typeId); };
		auto onA = [](uint32_t, GS::ByteStream&) {};

		for (uint32_t i = 0; i < 10; i++)
		{
			uint8_t payload[1] = { 0 };
			a.QueueMessage(1000 + i, payload, 1, GS::Reliability::Reliable);
		}

		PumpBothWays(socketA, a, socketB, b, 40, 1.0f / 20.0f, onA, onB);

		Check(received.size() == 10, "all 10 reliable messages arrived exactly once (loss=" + std::to_string(lossProbability) + ")");
		bool inOrder = true;
		for (uint32_t i = 0; i < received.size(); i++)
			if (received[i] != 1000 + i)
				inOrder = false;
		Check(inOrder, "reliable messages arrived in the order sent");
	}

	inline void RunRTTAccuracy()
	{
		GS::Socket socketA, socketB;
		socketA.Bind(0);
		socketB.Bind(0);

		GS::NetAddress toB{ 0x7F000001u, socketB.LocalPort() };
		GS::NetAddress toA{ 0x7F000001u, socketA.LocalPort() };

		GS::NetConnection a(&socketA, toB);
		GS::NetConnection b(&socketB, toA);

		const float dt = 0.1f;
		bool pending = false;

		auto onA = [](uint32_t, GS::ByteStream&) {};
		auto onB = [&](uint32_t typeId, GS::ByteStream&) { (void)typeId; pending = true; };

		uint8_t payload[1] = { 0 };
		a.QueueMessage(2000, payload, 1, GS::Reliability::Reliable);

		for (int i = 0; i < 20; i++)
		{
			a.Update(dt);
			b.Update(dt);

			if (pending)
			{
				pending = false;
				b.QueueMessage(2001, payload, 1, GS::Reliability::Reliable);
			}

			a.Flush(dt);
			b.Flush(dt);

			uint8_t buffer[2048];
			GS::NetAddress from;
			int received;
			while ((received = socketB.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				b.ReceivePacket(buffer, received, onB);
			while ((received = socketA.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				a.ReceivePacket(buffer, received, onA);
		}

		float rtt = a.GetRTT();
		Check(rtt > 0.05f && rtt < 0.6f, "measured RTT (" + std::to_string(rtt) + "s) is in the expected ~0.2s range");
	}

	inline void Run() {
		RunReliableDeliveryUnderLoss(0.0f);
		RunReliableDeliveryUnderLoss(0.3f);
		RunRTTAccuracy();

		GS_TRACE("NetConnectionTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
