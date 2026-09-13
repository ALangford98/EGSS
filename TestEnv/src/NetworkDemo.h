#pragma once

// Proves the whole networking stack end to end: run as two real processes,
//
//   ./TestEnv --demo NetworkDemo --host 7777
//   ./TestEnv --demo NetworkDemo --join 127.0.0.1:7777
//
// Each process gets a coloured square it moves with WASD; NetworkIdentity +
// NetworkTransform replicate position, and Space broadcasts a "Flash" RPC
// both processes render, proving the RPC path alongside replication. 2D
// (Renderer2D, straight to the window) rather than 3D -- the point is the
// networking, and a flat coloured square proves replication exactly as well
// as a lit mesh would, with far less machinery to get right.

#include <GS.h>
#include <imgui.h>

#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>
#include <GS/Network/NetReplication.h>
#include <GS/Network/NetMessage.h>

#include "Demo.h"

class NetworkDemo : public DemoLayer
{
public:
	NetworkDemo() : DemoLayer("NetworkDemo"), m_Camera(-4.0f, 4.0f, -2.25f, 2.25f) {}

	void OnDemoAttach() override
	{
		const std::vector<std::string>& args = GS::Application::GetCommandLine();
		for (size_t i = 1; i + 1 < args.size(); i++)
		{
			if (args[i] == "--host")
				m_IsHost = true, m_HostPort = (uint16_t)std::atoi(args[i + 1].c_str());
			else if (args[i] == "--join")
				m_IsClient = true, m_JoinAddress = args[i + 1];
			else if (args[i] == "--net-capture")
				m_NetCapturePath = args[i + 1];
		}

		if (m_IsHost)
		{
			m_Server.Host(m_HostPort);
			m_Server.OnClientConnected([this](GS::ClientId client)
			{
				GS::Net::CatchUpNewClient(m_Server, m_Scene, client);
			});
			m_LocalEntity = GS::Net::SpawnNetworkedEntity(m_Server, m_Scene, GS::ServerOwned, { -2.0f, 0.0f, 0.0f });
			m_LocalEntity.Add<GS::SpriteComponent>({ { 0.9f, 0.3f, 0.3f, 1.0f }, nullptr, 1.0f });
			GS::Net::RegisterHandler("Flash", [this](GS::ClientId, GS::ByteStream&) { m_FlashTimer = 0.2f; });
		}
		else if (m_IsClient)
		{
			GS::Net::ClientBindScene(m_Client, m_Scene);
			GS::NetAddress address;
			if (GS::ParseAddress(m_JoinAddress, address))
				m_Client.Connect(address);
			GS::Net::RegisterHandler("Flash", [this](GS::ClientId, GS::ByteStream&) { m_FlashTimer = 0.2f; });
		}
	}

	void OnDemoFixedUpdate(GS::Timestep step) override
	{
		float dt = step.GetSeconds();

		if (m_IsHost)
		{
			glm::vec3& position = m_LocalEntity.Get<GS::TransformComponent>()->Position;
			if (GS::Input::IsKeyPressed(GS_KEY_A)) position.x -= 3.0f * dt;
			if (GS::Input::IsKeyPressed(GS_KEY_D)) position.x += 3.0f * dt;
			if (GS::Input::IsKeyPressed(GS_KEY_W)) position.y += 3.0f * dt;
			if (GS::Input::IsKeyPressed(GS_KEY_S)) position.y -= 3.0f * dt;

			if (GS::Input::IsKeyPressed(GS_KEY_SPACE) && m_SpaceWasUp)
			{
				GS::ByteStream empty;
				GS::Net::SendRPC(m_Server, "Flash", empty, GS::Reliability::Reliable);
				m_FlashTimer = 0.2f;
			}
			m_SpaceWasUp = !GS::Input::IsKeyPressed(GS_KEY_SPACE);

			m_Server.Update(dt);
			GS::Net::ServerBroadcastSnapshots(m_Server, m_Scene, dt);

			// Capture half a second after the real event of interest (a
			// client connected) rather than at a fixed frame/step count --
			// two independently-started processes share no clock, so "wait
			// N frames" cannot be relied on to line up with "the handshake
			// finished," and the delay lets the spawn+snapshot messages
			// that follow a connection actually arrive and settle first.
			if (!m_Server.GetConnectedClients().empty())
				m_TimeSinceReadyToCapture += dt;
			if (!m_NetCapturePath.empty() && !m_HasCaptured && m_TimeSinceReadyToCapture >= 0.5f)
			{
				GS::Application::Get().CaptureFrame(m_NetCapturePath);
				m_HasCaptured = true;
			}
		}
		else if (m_IsClient)
		{
			m_Client.Update(dt);

			if (m_Client.IsConnected() && GS::Input::IsKeyPressed(GS_KEY_SPACE) && m_SpaceWasUp)
			{
				GS::ByteStream empty;
				GS::Net::SendRPC(m_Client, "Flash", empty, GS::Reliability::Reliable);
				m_FlashTimer = 0.2f;
			}
			m_SpaceWasUp = !GS::Input::IsKeyPressed(GS_KEY_SPACE);

			if (m_Client.IsConnected())
				m_TimeSinceReadyToCapture += dt;
			if (!m_NetCapturePath.empty() && !m_HasCaptured && m_TimeSinceReadyToCapture >= 0.5f)
			{
				GS::Application::Get().CaptureFrame(m_NetCapturePath);
				m_HasCaptured = true;
			}
		}

		if (m_FlashTimer > 0.0f)
			m_FlashTimer -= dt;
	}

	void OnDemoUpdate(GS::Timestep ts) override
	{
		(void)ts;

		GS::RenderCommand::SetClearColor({ 0.08f, 0.08f, 0.10f, 1.0f });
		GS::RenderCommand::Clear();

		GS::Renderer2D::BeginScene(m_Camera);

		auto& sprites = m_Scene.View<GS::SpriteComponent>();
		for (size_t i = 0; i < sprites.Size(); i++)
		{
			GS::EntityId owner = sprites.Owner(i);
			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(owner);
			GS::Renderer2D::DrawQuad(transform->Position, { 0.5f, 0.5f }, sprites.Components()[i].Color);
		}

		// Client-side entities (created by ClientBindScene on Spawn) never
		// get a SpriteComponent of their own -- draw every NetworkIdentity
		// entity that doesn't have one, so a joining client still sees the
		// host's square.
		auto& identities = m_Scene.View<GS::NetworkIdentity>();
		for (size_t i = 0; i < identities.Size(); i++)
		{
			GS::EntityId owner = identities.Owner(i);
			if (m_Scene.HasComponent<GS::SpriteComponent>(owner))
				continue;
			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(owner);
			GS::Renderer2D::DrawQuad(transform->Position, { 0.5f, 0.5f }, { 0.3f, 0.6f, 0.9f, 1.0f });
		}

		GS::Renderer2D::EndScene();
	}

	void OnDemoImGui() override
	{
		ImGui::Begin("NetworkDemo");
		if (m_IsHost)
			ImGui::Text("Hosting on port %u", m_HostPort);
		else if (m_IsClient)
			ImGui::Text("Client -- connected: %s", m_Client.IsConnected() ? "yes" : "no");
		else
			ImGui::Text("Neither --host nor --join was given");

		if (m_FlashTimer > 0.0f)
			ImGui::TextColored({ 1, 1, 0, 1 }, "FLASH");
		ImGui::End();
	}

private:
	GS::OrthographicCamera m_Camera;
	GS::Scene m_Scene;

	bool m_IsHost = false;
	bool m_IsClient = false;
	uint16_t m_HostPort = 7777;
	std::string m_JoinAddress;

	GS::NetServer m_Server;
	GS::NetClient m_Client;
	GS::Entity m_LocalEntity;

	bool m_SpaceWasUp = true;
	float m_FlashTimer = 0.0f;

	std::string m_NetCapturePath;
	bool m_HasCaptured = false;
	float m_TimeSinceReadyToCapture = 0.0f;
};
