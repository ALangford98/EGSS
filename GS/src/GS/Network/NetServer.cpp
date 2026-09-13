#include "gspch.h"
#include "GS/Network/NetServer.h"
#include "GS/Network/NetMessage.h"

namespace GS {

	bool NetServer::Host(uint16_t port, int maxClients)
	{
		m_MaxClients = maxClients;
		m_Hosting = m_Socket.Bind(port);
		return m_Hosting;
	}

	void NetServer::Shutdown()
	{
		m_Socket.Close();
		m_Clients.clear();
		m_AddressToClient.clear();
		m_Hosting = false;
	}

	void NetServer::Update(float deltaSeconds)
	{
		if (!m_Hosting)
			return;

		uint8_t buffer[Protocol::kMaxPacketSize];
		NetAddress from;
		int received;
		while ((received = m_Socket.RecvFrom(buffer, sizeof(buffer), from)) > 0)
			HandleRawPacket(buffer, (size_t)received, from);

		std::vector<ClientId> timedOut;
		for (auto& [id, connection] : m_Clients)
		{
			connection->Update(deltaSeconds);
			if (connection->IsTimedOut())
				timedOut.push_back(id);
		}

		for (ClientId id : timedOut)
		{
			NetAddress address = m_Clients[id]->GetRemoteAddress();
			m_AddressToClient.erase(AddressKey(address));
			m_Clients.erase(id);
			if (m_OnDisconnected) m_OnDisconnected(id);
		}

		for (auto& [id, connection] : m_Clients)
			connection->Flush(deltaSeconds);
	}

	void NetServer::HandleRawPacket(const uint8_t* data, size_t size, const NetAddress& from)
	{
		uint64_t key = AddressKey(from);
		auto it = m_AddressToClient.find(key);

		if (it != m_AddressToClient.end())
		{
			NetConnection* connection = m_Clients.at(it->second).get();
			ClientId id = it->second;
			connection->ReceivePacket(data, size, [&](uint32_t typeId, ByteStream& payload)
			{
				if (m_OnMessage) m_OnMessage(id, typeId, payload);
				Net::DispatchToHandler(id, typeId, payload);
			});
			return;
		}

		// Unknown address: the only thing it's allowed to say is a connect
		// request. Everything else from a stranger is silently ignored.
		ByteStream in(data, size);
		Protocol::ReadHeader(in); // sequencing is meaningless before a connection exists
		if (in.AtEnd())
			return;

		in.ReadU8(); // reliability tag
		uint32_t typeId = in.ReadU32();
		uint16_t length = in.ReadU16();
		if (typeId != Net::kMsgConnectRequest || length != 2)
			return;

		uint16_t version = in.ReadU16();

		if (version != Protocol::kProtocolVersion || (int)m_Clients.size() >= m_MaxClients)
		{
			// Rejected -- sent raw, since no NetConnection exists to carry it
			// reliably, and there is nothing to retry against a peer that
			// isn't going to become a client.
			ByteStream reject;
			Protocol::WriteHeader(reject, { 0, 0, 0 });
			reject.WriteU8((uint8_t)Reliability::Unreliable);
			reject.WriteU32(Net::kMsgConnectRejected);
			reject.WriteU16(0);
			m_Socket.SendTo(from, reject.Data(), reject.Size());
			return;
		}

		ClientId newId = m_NextClientId++;
		auto connection = std::make_unique<NetConnection>(&m_Socket, from);

		uint8_t acceptPayload[2];
		std::memcpy(acceptPayload, &newId, sizeof(newId));
		connection->QueueMessage(Net::kMsgConnectAccepted, acceptPayload, sizeof(acceptPayload), Reliability::Reliable);

		m_AddressToClient[key] = newId;
		m_Clients[newId] = std::move(connection);

		if (m_OnConnected) m_OnConnected(newId);
	}

	void NetServer::Broadcast(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		for (auto& [id, connection] : m_Clients)
			connection->QueueMessage(typeId, payload, size, reliability);
	}

	void NetServer::SendTo(ClientId client, uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		auto it = m_Clients.find(client);
		if (it != m_Clients.end())
			it->second->QueueMessage(typeId, payload, size, reliability);
	}

	NetConnection* NetServer::GetConnection(ClientId client)
	{
		auto it = m_Clients.find(client);
		return it != m_Clients.end() ? it->second.get() : nullptr;
	}

	std::vector<ClientId> NetServer::GetConnectedClients() const
	{
		std::vector<ClientId> ids;
		ids.reserve(m_Clients.size());
		for (auto& [id, connection] : m_Clients)
			ids.push_back(id);
		return ids;
	}

}
