#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"
#include "GS/Network/Socket.h"
#include "GS/Network/NetConnection.h"

namespace GS {

	using ClientId = uint16_t;
	constexpr ClientId ServerOwned = 0;
	constexpr ClientId InvalidClient = 0xFFFF;

	namespace Net {
		// Reserved, built-in message type IDs -- everything a game registers
		// through Net::RegisterHandler gets a hashed ID instead, so these low,
		// hand-picked values can never collide with one.
		constexpr uint32_t kMsgConnectRequest = 1;
		constexpr uint32_t kMsgConnectAccepted = 2;
		constexpr uint32_t kMsgConnectRejected = 3;
	}

	class GS_API NetServer
	{
	public:
		using ConnectedCallback = std::function<void(ClientId)>;
		using DisconnectedCallback = std::function<void(ClientId)>;
		using MessageCallback = std::function<void(ClientId from, uint32_t typeId, ByteStream& payload)>;

		bool Host(uint16_t port, int maxClients = 8);
		void Shutdown();

		// Pumps incoming packets, runs timeout checks, flushes every
		// connection. Call once per fixed step.
		void Update(float deltaSeconds);

		void Broadcast(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);
		void SendTo(ClientId client, uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);

		void OnClientConnected(ConnectedCallback cb) { m_OnConnected = std::move(cb); }
		void OnClientDisconnected(DisconnectedCallback cb) { m_OnDisconnected = std::move(cb); }
		void OnMessage(MessageCallback cb) { m_OnMessage = std::move(cb); }

		bool IsHosting() const { return m_Hosting; }
		uint16_t GetLocalPort() const { return m_Socket.LocalPort(); }
		NetConnection* GetConnection(ClientId client);
		std::vector<ClientId> GetConnectedClients() const;

	private:
		static uint64_t AddressKey(const NetAddress& address) { return ((uint64_t)address.IP << 16) | address.Port; }

		void HandleRawPacket(const uint8_t* data, size_t size, const NetAddress& from);

		Socket m_Socket;
		bool m_Hosting = false;
		int m_MaxClients = 8;
		ClientId m_NextClientId = 1; // 0 is ServerOwned -- never assigned to a real client

		std::unordered_map<ClientId, std::unique_ptr<NetConnection>> m_Clients;
		std::unordered_map<uint64_t, ClientId> m_AddressToClient;

		ConnectedCallback m_OnConnected;
		DisconnectedCallback m_OnDisconnected;
		MessageCallback m_OnMessage;
	};

}
