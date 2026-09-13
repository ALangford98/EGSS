#include "gspch.h"
#include "GS/Network/NetReplication.h"

namespace GS::Net {

	static uint32_t s_NextNetworkId = 1;

	// Per-scene-pointer client-side bookkeeping. Keyed by Scene* rather than
	// held as a member of NetClient, since NetClient has no reason to know
	// Scene exists -- that separation is the point of this file existing.
	static std::unordered_map<Scene*, std::unordered_map<uint32_t, EntityId>> s_ClientNetworkIdToEntity;

	// Server-side: entities time out their SendRate independently.
	static std::unordered_map<uint32_t, float> s_TimeSinceLastSnapshot;

	Entity SpawnNetworkedEntity(NetServer& server, Scene& scene, ClientId owner, const glm::vec3& position)
	{
		Entity entity = scene.CreateEntity("NetworkedEntity");
		entity.Get<TransformComponent>()->Position = position;

		uint32_t networkId = s_NextNetworkId++;
		entity.Add<NetworkIdentity>({ networkId, owner });
		entity.Add<NetworkTransform>({});

		ByteStream spawn;
		spawn.WriteU32(networkId);
		spawn.WriteU16(owner);
		spawn.WriteVec3(position);
		server.Broadcast(kMsgSpawnEntity, spawn.Data(), spawn.Size(), Reliability::Reliable);

		return entity;
	}

	void DespawnNetworkedEntity(NetServer& server, Scene& scene, Entity entity)
	{
		auto* identity = entity.Get<NetworkIdentity>();
		if (!identity)
			return;

		ByteStream despawn;
		despawn.WriteU32(identity->NetworkId);
		server.Broadcast(kMsgDespawnEntity, despawn.Data(), despawn.Size(), Reliability::Reliable);

		s_TimeSinceLastSnapshot.erase(identity->NetworkId);
		scene.DestroyEntity(entity.GetId());
	}

	void ServerBroadcastSnapshots(NetServer& server, Scene& scene, float deltaSeconds)
	{
		auto& identities = scene.View<NetworkIdentity>();
		for (size_t i = 0; i < identities.Size(); i++)
		{
			EntityId owner = identities.Owner(i);
			auto* networkTransform = scene.GetComponent<NetworkTransform>(owner);
			if (!networkTransform)
				continue;

			uint32_t networkId = identities.Components()[i].NetworkId;
			float& elapsed = s_TimeSinceLastSnapshot[networkId];
			elapsed += deltaSeconds;

			float interval = 1.0f / networkTransform->SendRate;
			if (elapsed < interval)
				continue;
			elapsed = 0.0f;

			auto* transform = scene.GetComponent<TransformComponent>(owner);
			ByteStream snapshot;
			snapshot.WriteU32(networkId);
			snapshot.WriteVec3(transform->Position);
			snapshot.WriteVec3(transform->Rotation);
			server.Broadcast(kMsgTransformSnapshot, snapshot.Data(), snapshot.Size(), Reliability::Unreliable);
		}
	}

	void ClientBindScene(NetClient& client, Scene& scene)
	{
		auto& map = s_ClientNetworkIdToEntity[&scene];

		client.OnMessage([&scene, &map](uint32_t typeId, ByteStream& payload)
		{
			if (typeId == kMsgSpawnEntity)
			{
				uint32_t networkId = payload.ReadU32();
				ClientId owner = payload.ReadU16();
				glm::vec3 position = payload.ReadVec3();

				Entity entity = scene.CreateEntity("NetworkedEntity");
				entity.Get<TransformComponent>()->Position = position;
				entity.Add<NetworkIdentity>({ networkId, owner });
				entity.Add<NetworkTransform>({});
				map[networkId] = entity.GetId();
			}
			else if (typeId == kMsgDespawnEntity)
			{
				uint32_t networkId = payload.ReadU32();
				auto it = map.find(networkId);
				if (it != map.end())
				{
					scene.DestroyEntity(it->second);
					map.erase(it);
				}
			}
			else if (typeId == kMsgTransformSnapshot)
			{
				uint32_t networkId = payload.ReadU32();
				glm::vec3 position = payload.ReadVec3();
				glm::vec3 rotation = payload.ReadVec3();

				auto it = map.find(networkId);
				if (it != map.end())
				{
					// No interpolation smoothing here -- NetworkTransform::
					// Interpolate is wired up visually in NetworkDemo, where
					// there is a render frame rate to interpolate across.
					if (auto* transform = scene.GetComponent<TransformComponent>(it->second))
					{
						transform->Position = position;
						transform->Rotation = rotation;
					}
				}
			}
		});
	}

}
