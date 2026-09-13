// TEMPORARY -- delete after verifying spawn/despawn bookkeeping and
// transform-snapshot convergence between a real NetServer and NetClient.
#pragma once
#include <GS.h>
#include <GS/Network/NetReplication.h>

namespace NetReplicationTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::NetServer server;
		server.Host(0);
		GS::Scene serverScene;

		GS::NetClient client;
		GS::Scene clientScene;
		GS::Net::ClientBindScene(client, clientScene);

		client.Connect({ 0x7F000001u, server.GetLocalPort() });

		bool connected = false;
		client.OnConnected([&] { connected = true; });
		auto pump = [&](int steps) {
			for (int i = 0; i < steps; i++) { server.Update(1.0f / 60.0f); client.Update(1.0f / 60.0f); }
		};
		for (int i = 0; i < 200 && !connected; i++) { server.Update(1.0f / 60.0f); client.Update(1.0f / 60.0f); }
		Check(connected, "client connected (setup)");

		GS::Entity spawned = GS::Net::SpawnNetworkedEntity(server, serverScene, GS::ServerOwned, { 1.0f, 2.0f, 3.0f });
		Check(spawned.IsValid(), "server-side entity was created");
		uint32_t networkId = spawned.Get<GS::NetworkIdentity>()->NetworkId;
		Check(networkId != 0, "a real NetworkId was assigned");

		pump(30);

		GS::EntityId clientEntity = GS::InvalidEntity;
		for (GS::EntityId id : clientScene.GetEntities())
		{
			auto* identity = clientScene.GetComponent<GS::NetworkIdentity>(id);
			if (identity && identity->NetworkId == networkId)
				clientEntity = id;
		}
		Check(clientEntity != GS::InvalidEntity, "spawn replicated: client created a matching entity");

		if (clientEntity != GS::InvalidEntity)
		{
			auto* clientTransform = clientScene.GetComponent<GS::TransformComponent>(clientEntity);
			glm::vec3 delta = clientTransform->Position - glm::vec3(1.0f, 2.0f, 3.0f);
			Check(glm::length(delta) < 0.01f, "initial position replicated exactly");
		}

		spawned.Get<GS::TransformComponent>()->Position = { 9.0f, 9.0f, 9.0f };
		for (int i = 0; i < 60; i++)
		{
			GS::Net::ServerBroadcastSnapshots(server, serverScene, 1.0f / 60.0f);
			pump(1);
		}

		if (clientEntity != GS::InvalidEntity)
		{
			auto* clientTransform = clientScene.GetComponent<GS::TransformComponent>(clientEntity);
			glm::vec3 delta = clientTransform->Position - glm::vec3(9.0f, 9.0f, 9.0f);
			Check(glm::length(delta) < 0.5f, "moved position converges on the client (delta=" + std::to_string(glm::length(delta)) + ")");
		}

		GS::Net::DespawnNetworkedEntity(server, serverScene, spawned);
		pump(30);

		bool stillPresent = false;
		for (GS::EntityId id : clientScene.GetEntities())
		{
			auto* identity = clientScene.GetComponent<GS::NetworkIdentity>(id);
			if (identity && identity->NetworkId == networkId)
				stillPresent = true;
		}
		Check(!stillPresent, "despawn replicated: client's entity is gone");

		client.Disconnect();
		server.Shutdown();
		GS_TRACE("NetReplicationTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
