#include "gspch.h"
#include "GS/Network/NetClient.h"
#include "GS/Network/NetMessage.h"

namespace GS {

	bool NetClient::Connect(const NetAddress& server)
	{
		if (!m_Socket.Bind(0))
			return false;

		m_Connection = std::make_unique<NetConnection>(&m_Socket, server);
		m_Connecting = true;
		m_ClientId = InvalidClient;
		m_TimeSinceConnectRequest = 1000.0f; // force an immediate first send below

		return true;
	}

	void NetClient::Disconnect()
	{
		m_Connection.reset();
		m_Connecting = false;
		m_ClientId = InvalidClient;
		m_Socket.Close();
	}

	void NetClient::Update(float deltaSeconds)
	{
		if (!m_Connection)
			return;

		if (m_Connecting)
		{
			// Not yet a NetConnection-tracked reliable message -- there is no
			// confirmed peer sequence state until ConnectAccepted arrives, so
			// this is resent by hand at a fixed interval instead.
			m_TimeSinceConnectRequest += deltaSeconds;
			if (m_TimeSinceConnectRequest >= NetConnection::kResendInterval)
			{
				m_TimeSinceConnectRequest = 0.0f;
				ByteStream request;
				Protocol::WriteHeader(request, { 0, 0, 0 });
				request.WriteU8((uint8_t)Reliability::Reliable);
				request.WriteU32(Net::kMsgConnectRequest);
				request.WriteU16(2);
				request.WriteU16(Protocol::kProtocolVersion);
				m_Socket.SendTo(m_Connection->GetRemoteAddress(), request.Data(), request.Size());
			}
		}

		m_Connection->Update(deltaSeconds);

		uint8_t buffer[Protocol::kMaxPacketSize];
		NetAddress from;
		int received;
		while ((received = m_Socket.RecvFrom(buffer, sizeof(buffer), from)) > 0)
		{
			if (m_Connecting)
			{
				// Peek the message type without going through NetConnection --
				// there is no established sequence state to accept a raw
				// ConnectAccepted/Rejected through yet.
				ByteStream in(buffer, (size_t)received);
				Protocol::ReadHeader(in);
				if (in.AtEnd()) continue;
				in.ReadU8();
				uint32_t typeId = in.ReadU32();
				uint16_t length = in.ReadU16();

				if (typeId == Net::kMsgConnectAccepted && length == 2)
				{
					std::memcpy(&m_ClientId, in.Data() + in.ReadOffset(), 2);
					m_Connecting = false;
					if (m_OnConnected) m_OnConnected();
				}
				else if (typeId == Net::kMsgConnectRejected)
				{
					Disconnect();
				}
				continue;
			}

			m_Connection->ReceivePacket(buffer, (size_t)received, [&](uint32_t typeId, ByteStream& payload)
			{
				if (m_OnMessage) m_OnMessage(typeId, payload);
				Net::DispatchToHandler(GS::ServerOwned, typeId, payload);
			});
		}

		if (!m_Connecting && m_Connection->IsTimedOut())
		{
			bool wasConnected = IsConnected();
			Disconnect();
			if (wasConnected && m_OnDisconnected) m_OnDisconnected();
			return;
		}

		if (!m_Connecting)
			m_Connection->Flush(deltaSeconds);
	}

	void NetClient::Send(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		if (m_Connection && !m_Connecting)
			m_Connection->QueueMessage(typeId, payload, size, reliability);
	}

}
