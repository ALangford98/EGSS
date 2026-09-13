#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Scene/Scene.h"
#include "GS/Network/NetServer.h"
#include "GS/Network/NetClient.h"

namespace GS::Net {

	constexpr uint32_t kMsgSpawnEntity = 4;
	constexpr uint32_t kMsgDespawnEntity = 5;
	constexpr uint32_t kMsgTransformSnapshot = 6;

	GS_API Entity SpawnNetworkedEntity(NetServer& server, Scene& scene, ClientId owner, const glm::vec3& position);
	GS_API void DespawnNetworkedEntity(NetServer& server, Scene& scene, Entity entity);

	// Sends a targeted SpawnEntity for every already-networked entity to one
	// specific client -- call this from the server's OnClientConnected.
	// Without it, a client that joins after an entity was spawned never
	// learns it exists: Broadcast only reaches connections that exist at
	// the moment it's called, and a spawn is normally broadcast once, at
	// the moment the entity is created.
	GS_API void CatchUpNewClient(NetServer& server, Scene& scene, ClientId newClient);

	// Server-side: call once per fixed step. Walks every NetworkIdentity +
	// NetworkTransform entity whose SendRate interval has elapsed and
	// broadcasts its current TransformComponent.
	GS_API void ServerBroadcastSnapshots(NetServer& server, Scene& scene, float deltaSeconds);

	// Client-side: call once, right after constructing the NetClient, before
	// the first Connect(). Wires Spawn/Despawn/Snapshot handling so incoming
	// replication messages create/destroy/update entities in `scene`.
	GS_API void ClientBindScene(NetClient& client, Scene& scene);

}
