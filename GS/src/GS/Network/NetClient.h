#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"
#include "GS/Network/Socket.h"
#include "GS/Network/NetConnection.h"
#include "GS/Network/NetServer.h"

namespace GS {

	class GS_API NetClient
	{
	public:
		using ConnectedCallback = std::function<void()>;
		using DisconnectedCallback = std::function<void()>;
		using MessageCallback = std::function<void(uint32_t typeId, ByteStream& payload)>;

		bool Connect(const NetAddress& server);
		void Disconnect();

		void Update(float deltaSeconds);

		void Send(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);

		void OnConnected(ConnectedCallback cb) { m_OnConnected = std::move(cb); }
		void OnDisconnected(DisconnectedCallback cb) { m_OnDisconnected = std::move(cb); }
		void OnMessage(MessageCallback cb) { m_OnMessage = std::move(cb); }

		bool IsConnected() const { return m_ClientId != InvalidClient; }
		ClientId GetClientId() const { return m_ClientId; }
		float GetRTT() const { return m_Connection ? m_Connection->GetRTT() : 0.0f; }

	private:
		Socket m_Socket;
		std::unique_ptr<NetConnection> m_Connection;
		ClientId m_ClientId = InvalidClient;
		float m_TimeSinceConnectRequest = 0.0f;
		bool m_Connecting = false;

		ConnectedCallback m_OnConnected;
		DisconnectedCallback m_OnDisconnected;
		MessageCallback m_OnMessage;
	};

}
