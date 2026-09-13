#include "gspch.h"
#include "GS/Network/NetMessage.h"

namespace GS::Net {

	static std::unordered_map<uint32_t, MessageHandler> s_Handlers;

	uint32_t HashMessageName(const std::string& name)
	{
		// FNV-1a. Simple, no dependency, plenty of distribution for the
		// handful of message names one game defines.
		uint32_t hash = 2166136261u;
		for (char c : name)
		{
			hash ^= (uint8_t)c;
			hash *= 16777619u;
		}
		// Never collide with the reserved built-in IDs (1-3 connect, 4-6
		// replication) -- push every hashed ID above a safe floor.
		return hash < 1000 ? hash + 1000 : hash;
	}

	void RegisterHandler(const std::string& name, MessageHandler handler)
	{
		s_Handlers[HashMessageName(name)] = std::move(handler);
	}

	void DispatchToHandler(ClientId from, uint32_t typeId, ByteStream& payload)
	{
		auto it = s_Handlers.find(typeId);
		if (it != s_Handlers.end())
			it->second(from, payload);
	}

	void SendRPC(NetServer& server, ClientId to, const std::string& name, ByteStream& payload, Reliability reliability)
	{
		server.SendTo(to, HashMessageName(name), payload.Data(), payload.Size(), reliability);
	}

	void SendRPC(NetServer& server, const std::string& name, ByteStream& payload, Reliability reliability)
	{
		server.Broadcast(HashMessageName(name), payload.Data(), payload.Size(), reliability);
	}

	void SendRPC(NetClient& client, const std::string& name, ByteStream& payload, Reliability reliability)
	{
		client.Send(HashMessageName(name), payload.Data(), payload.Size(), reliability);
	}

}
