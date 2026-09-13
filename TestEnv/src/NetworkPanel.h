#pragma once

// The editor's "Network" panel: host a session or join one, and see who's
// connected. See docs/superpowers/specs/2026-09-13-multiplayer-foundation-design.md.

#include <GS.h>
#include <imgui.h>

#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>

// One process-wide server and client, matching how the editor already
// keeps one g_EditorScene rather than threading state through callers. A
// given editor process is either hosting or joined, never both.
inline GS::NetServer g_EditorNetServer;
inline GS::NetClient g_EditorNetClient;

class NetworkPanel : public GS::Layer
{
public:
	NetworkPanel() : Layer("NetworkPanel") {}

	void OnUpdate(GS::Timestep ts) override
	{
		(void)ts;
		float fixedStep = GS::Application::Get().GetFixedTimestep();
		if (g_EditorNetServer.IsHosting())
			g_EditorNetServer.Update(fixedStep);
		if (g_EditorNetClient.IsConnected())
			g_EditorNetClient.Update(fixedStep);
	}

	void OnImGuiRender() override
	{
		if (GS::Application::Get().IsUIHidden())
			return;

		ImGui::Begin("Network");

		if (g_EditorNetServer.IsHosting())
		{
			ImGui::Text("Hosting on port %u", g_EditorNetServer.GetLocalPort());
			if (ImGui::Button("Stop Hosting"))
				g_EditorNetServer.Shutdown();

			ImGui::Separator();
			ImGui::Text("Connected clients:");
			if (ImGui::BeginTable("clients", 2, ImGuiTableFlags_Borders))
			{
				ImGui::TableSetupColumn("Client");
				ImGui::TableSetupColumn("RTT (ms)");
				ImGui::TableHeadersRow();
				for (GS::ClientId id : g_EditorNetServer.GetConnectedClients())
				{
					GS::NetConnection* connection = g_EditorNetServer.GetConnection(id);
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%u", id);
					ImGui::TableNextColumn(); ImGui::Text("%.0f", connection ? connection->GetRTT() * 1000.0f : 0.0f);
				}
				ImGui::EndTable();
			}
		}
		else if (g_EditorNetClient.IsConnected())
		{
			ImGui::Text("Connected as client %u", g_EditorNetClient.GetClientId());
			ImGui::Text("RTT: %.0f ms", g_EditorNetClient.GetRTT() * 1000.0f);
			if (ImGui::Button("Disconnect"))
				g_EditorNetClient.Disconnect();
		}
		else
		{
			ImGui::InputInt("Port", &m_HostPort);
			if (ImGui::Button("Host"))
				g_EditorNetServer.Host((uint16_t)m_HostPort);

			ImGui::Separator();

			ImGui::InputText("Address:Port", m_JoinAddress, sizeof(m_JoinAddress));
			if (ImGui::Button("Join"))
			{
				GS::NetAddress address;
				if (GS::ParseAddress(m_JoinAddress, address))
					g_EditorNetClient.Connect(address);
				else
					GS_WARN("Network panel: '{0}' is not a valid address:port", m_JoinAddress);
			}
		}

		ImGui::End();
	}

private:
	int m_HostPort = 7777;
	char m_JoinAddress[64] = "127.0.0.1:7777";
};
