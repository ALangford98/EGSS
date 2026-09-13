#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"
#include "GS/Network/NetServer.h"
#include "GS/Network/NetClient.h"

namespace GS::Net {

	using MessageHandler = std::function<void(ClientId from, ByteStream& payload)>;

	GS_API uint32_t HashMessageName(const std::string& name);

	GS_API void RegisterHandler(const std::string& name, MessageHandler handler);

	// Internal: called by NetServer/NetClient for any message type that
	// isn't one of the reserved built-in IDs (connect/spawn/despawn/
	// snapshot). Not part of the public API a game calls.
	GS_API void DispatchToHandler(ClientId from, uint32_t typeId, ByteStream& payload);

	GS_API void SendRPC(NetServer& server, ClientId to, const std::string& name, ByteStream& payload, Reliability reliability);
	GS_API void SendRPC(NetServer& server, const std::string& name, ByteStream& payload, Reliability reliability); // broadcast
	GS_API void SendRPC(NetClient& client, const std::string& name, ByteStream& payload, Reliability reliability);

}
