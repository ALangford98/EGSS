#pragma once

// Live timings for the frame just gone, plus a button to capture a Chrome
// trace.
//
// This exists because VSync makes the frame counter useless as a measure of
// cost: the swap blocks until the display is ready, so a frame doing 2ms of
// work and one doing 12ms both report 16.7ms. The only way to tell them apart
// is to time the scopes inside the frame, which is what this shows.

#include <Egss.h>
#include <imgui.h>

#include "Demo.h"

class ProfilerPanel : public Egss::Layer
{
public:
	ProfilerPanel()
		: Layer("ProfilerPanel")
	{
	}

	void OnImGuiRender() override
	{
		ImGui::SetNextWindowPos(ImVec2(980.0f, 20.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(280.0f, 300.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Profiler");

		const auto& entries = Egss::Instrumentor::GetLastFrame();

		// "Frame" wraps everything else, so it is the reference the other
		// rows are measured against.
		double frameMicros = 0.0;
		for (const Egss::ProfileEntry& entry : entries)
		{
			if (entry.Name == "Frame")
			{
				frameMicros = entry.TotalMicros;
				break;
			}
		}

		ImGui::Text("Frame: %.2f ms", frameMicros / 1000.0);

		// The swap is a wait rather than work -- but do not read the remainder
		// as pure CPU cost. With VSync the driver is free to block wherever it
		// likes, and in optimised builds it often stalls inside a GL call in
		// OnUpdate instead of at the swap. Measured here: the swap fell from
		// 5.6ms (Debug) to 0.4ms (Release) while OnUpdate doubled. Trust the
		// scopes that do no GL -- physics, in particular -- and treat anything
		// that touches the driver as work plus an unknown wait.
		double swapMicros = 0.0;
		for (const Egss::ProfileEntry& entry : entries)
		{
			if (entry.Name.rfind("Window::OnUpdate", 0) == 0)
			{
				swapMicros = entry.TotalMicros;
				break;
			}
		}

		ImGui::Text("Swap:  %.2f ms", swapMicros / 1000.0);
		ImGui::TextDisabled("VSync can also stall inside GL calls");

		ImGui::Separator();

		if (ImGui::BeginTable("timings", 3,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Scope");
			ImGui::TableSetupColumn("ms");
			ImGui::TableSetupColumn("n");
			ImGui::TableHeadersRow();

			for (const Egss::ProfileEntry& entry : entries)
			{
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(entry.Name.c_str());

				ImGui::TableNextColumn();
				ImGui::Text("%.3f", entry.TotalMicros / 1000.0);

				ImGui::TableNextColumn();
				ImGui::Text("%u", entry.Calls);
			}

			ImGui::EndTable();
		}

		ImGui::Separator();

		// A trace answers a different question: not "what is slow now" but
		// "what happened during those three bad seconds". Load the file into
		// chrome://tracing or ui.perfetto.dev.
		if (Egss::Instrumentor::IsSessionActive())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Capturing...");
			if (ImGui::Button("Stop capture"))
				EGSS_PROFILE_END_SESSION();
		}
		else
		{
			if (ImGui::Button("Capture trace"))
				EGSS_PROFILE_BEGIN_SESSION("EGSS", "profile.json");
			ImGui::TextDisabled("writes profile.json next to the exe");
		}

		ImGui::End();
	}
};
