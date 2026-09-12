#pragma once

// Live timings for the frame just gone, plus a button to capture a Chrome
// trace -- now behind an "Advanced" tab. "Overview" is the tab that opens
// first: FPS, memory, draw calls and cached-mesh size in plain terms, for
// a reader who has no reason to know what "Renderer2D::Flush" means. The
// detailed scope table is still here, just not the first thing shown --
// it's genuinely useful for tracking down what got slow, which "FPS: 62"
// alone can't answer.
//
// This exists because VSync makes the frame counter useless as a measure of
// cost: the swap blocks until the display is ready, so a frame doing 2ms of
// work and one doing 12ms both report 16.7ms. The only way to tell them apart
// is to time the scopes inside the frame, which is what the Advanced tab shows.

#include <GS.h>
#include <imgui.h>

#include "Demo.h"

#include <cstdio>
#include <fstream>
#include <string>

// Linux-only (reads /proc/self/status) -- this repo only targets Linux
// today (see gs.py), so there is no other platform's convention to match
// yet. Returns 0 (shown as "unavailable") rather than guessing if the file
// or the line isn't there, e.g. running under something that doesn't
// provide /proc.
inline size_t ReadProcessRSSBytes()
{
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line))
	{
		if (line.rfind("VmRSS:", 0) != 0)
			continue;

		unsigned long long kb = 0;
		if (std::sscanf(line.c_str(), "VmRSS: %llu kB", &kb) == 1)
			return (size_t)kb * 1024;
		break;
	}
	return 0;
}

class ProfilerPanel : public GS::Layer
{
public:
	ProfilerPanel()
		: Layer("ProfilerPanel")
	{
	}

	void OnImGuiRender() override
	{
		ImGui::SetNextWindowPos(ImVec2(980.0f, 20.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(280.0f, 340.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Profiler");

		const auto& entries = GS::Instrumentor::GetLastFrame();

		// "Frame" wraps everything else, so it is the reference the other
		// rows are measured against -- and the only honest source for FPS,
		// unlike Instrumentor::GetLastFrameMicros() (documented as summing
		// every scope at every nesting level, not "time in the frame").
		double frameMicros = 0.0;
		for (const GS::ProfileEntry& entry : entries)
		{
			if (entry.Name == "Frame")
			{
				frameMicros = entry.TotalMicros;
				break;
			}
		}

		double swapMicros = 0.0;
		for (const GS::ProfileEntry& entry : entries)
		{
			if (entry.Name.rfind("Window::OnUpdate", 0) == 0)
			{
				swapMicros = entry.TotalMicros;
				break;
			}
		}

		PushFrameTime(frameMicros / 1000.0);

		if (ImGui::BeginTabBar("ProfilerTabs"))
		{
			if (ImGui::BeginTabItem("Overview"))
			{
				DrawOverview();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Advanced"))
			{
				DrawAdvanced(entries, frameMicros, swapMicros);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::End();
	}

private:
	static constexpr int kHistorySize = 60;

	void PushFrameTime(double frameMs)
	{
		m_FrameTimeHistory[m_HistoryIndex] = frameMs;
		m_HistoryIndex = (m_HistoryIndex + 1) % kHistorySize;
		if (m_HistoryCount < kHistorySize)
			m_HistoryCount++;
	}

	double AverageFrameMs() const
	{
		if (m_HistoryCount == 0)
			return 0.0;
		double sum = 0.0;
		for (int i = 0; i < m_HistoryCount; i++)
			sum += m_FrameTimeHistory[i];
		return sum / m_HistoryCount;
	}

	// Plain terms, no jargon: how fast, how much memory, how much is being
	// drawn, how much geometry is loaded. Everything here already existed
	// somewhere in the engine (Renderer/Renderer2D's own draw-call
	// counters, MeshCache's own per-mesh vertex/triangle counts) -- none of
	// it needed new instrumentation except the memory read and MeshCache::
	// All() (a thin enumeration, not new tracking). No texture or audio
	// number is shown: neither has a cache or registry anywhere in the
	// engine to enumerate (checked, not assumed) -- that would be new
	// tracking infrastructure, not a display change, and isn't built here.
	void DrawOverview()
	{
		double avgMs = AverageFrameMs();
		double fps = avgMs > 0.0 ? 1000.0 / avgMs : 0.0;
		ImGui::Text("FPS: %.0f", fps);
		ImGui::TextDisabled("%.2f ms/frame, averaged over %d frames", avgMs, m_HistoryCount);

		ImGui::Separator();

		size_t rssBytes = ReadProcessRSSBytes();
		if (rssBytes > 0)
			ImGui::Text("Memory: %.1f MB", rssBytes / (1024.0 * 1024.0));
		else
			ImGui::TextDisabled("Memory: unavailable (no /proc/self/status)");

		ImGui::Separator();

		// Reset right after reading, not before -- everything this frame
		// submitted (every demo's own OnUpdate, and EditorSceneView's own
		// mesh submissions, all of which run before this ImGui pass) has
		// already landed in GetStats() by the time this runs. Several demos
		// already call ResetStats() themselves at the top of their own
		// OnUpdate (redundant with this, not conflicting with it -- resetting
		// an already-zeroed counter changes nothing); this is what gives every
		// other case, not just those demos, a real per-frame number instead
		// of one that only ever grows.
		GS::Renderer::Statistics stats3D = GS::Renderer::GetStats();
		GS::Renderer2D::Statistics stats2D = GS::Renderer2D::GetStats();
		ImGui::Text("Draw calls: %u", stats3D.DrawCalls + stats2D.DrawCalls);
		ImGui::Text("Triangles: %u", stats3D.TriangleCount + stats2D.TriangleCount);
		GS::Renderer::ResetStats();
		GS::Renderer2D::ResetStats();

		ImGui::Separator();

		std::vector<std::shared_ptr<GS::Mesh>> meshes = GS::MeshCache::All();
		size_t totalBytes = 0;
		for (const std::shared_ptr<GS::Mesh>& mesh : meshes)
			totalBytes += mesh->GetVertexCount() * sizeof(GS::MeshVertex)
				+ mesh->GetTriangleCount() * 3 * sizeof(unsigned int);
		ImGui::Text("Meshes cached: %zu (%.2f MB)", meshes.size(), totalBytes / (1024.0 * 1024.0));
	}

	void DrawAdvanced(const std::vector<GS::ProfileEntry>& entries, double frameMicros, double swapMicros)
	{
		ImGui::Text("Frame: %.2f ms", frameMicros / 1000.0);

		// The swap is a wait rather than work -- but do not read the remainder
		// as pure CPU cost. With VSync the driver is free to block wherever it
		// likes, and in optimised builds it often stalls inside a GL call in
		// OnUpdate instead of at the swap. Measured here: the swap fell from
		// 5.6ms (Debug) to 0.4ms (Release) while OnUpdate doubled. Trust the
		// scopes that do no GL -- physics, in particular -- and treat anything
		// that touches the driver as work plus an unknown wait.
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

			for (const GS::ProfileEntry& entry : entries)
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
		if (GS::Instrumentor::IsSessionActive())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Capturing...");
			if (ImGui::Button("Stop capture"))
				GS_PROFILE_END_SESSION();
		}
		else
		{
			if (ImGui::Button("Capture trace"))
				GS_PROFILE_BEGIN_SESSION("GS", "profile.json");
			ImGui::TextDisabled("writes profile.json next to the exe");
		}
	}

	double m_FrameTimeHistory[kHistorySize] = {};
	int m_HistoryIndex = 0;
	int m_HistoryCount = 0;
};
