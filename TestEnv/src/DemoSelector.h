#pragma once

// A small always-visible panel for choosing which demo runs.
//
// It is its own layer so that exactly one place owns the switching. When the
// demo layers each handled the switch key themselves they fought over it --
// every layer sees the same event, so one would select the other demo and the
// second would immediately select it back. Centralising it removes the
// problem rather than working around it.
//
// Pushed last in TestApp.cpp, so it sits on top of the stack and sees events
// before the demos do.

#include <Egss.h>
#include <imgui.h>

#include "DemoRegistry.h"

class DemoSelector : public Egss::Layer
{
public:
	DemoSelector()
		: Layer("DemoSelector")
	{
	}

	void OnImGuiRender() override
	{
		// Only sets a position the first time; after that ImGui remembers
		// wherever the user dragged it, including if they docked it.
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Demos");

		int current = g_ActiveDemo;

		// Combo writes through the int, and returns true only on the frame the
		// selection actually changed.
		ImGui::SetNextItemWidth(-1.0f);
		// Names come straight from the registry, so a new demo appears here
		// with no edit at all.
		if (ImGui::Combo("##demo", &current, [](void*, int i) { return s_Demos[i].Name; },
			nullptr, s_DemoCount))
			SetDemo(current);

		ImGui::Spacing();

		// Buttons as well as the dropdown: one click instead of two, and it
		// makes the available demos visible without opening anything.
		// Wrapped rather than one long row: with four demos a fixed-width
		// button per column ran off the edge of the panel.
		const int perRow = 3;

		for (int i = 0; i < s_DemoCount; i++)
		{
			if (i % perRow != 0)
				ImGui::SameLine();

			bool active = i == g_ActiveDemo;

			// Highlight the live one so the panel reads at a glance.
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));

			if (ImGui::Button(s_Demos[i].ShortName, ImVec2(90.0f, 0.0f)))
				SetDemo(i);

			if (active)
				ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextDisabled("F1 cycles. F2 screenshots. Each demo has its own panel.");

		ImGui::End();
	}

	void OnEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::KeyPressedEvent>([](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			// F2: a screenshot of the frame about to be drawn. Named by frame
			// number rather than by a counter of its own, so two shots taken
			// in the same run can never collide and the filename says when it
			// was taken.
			if (e.GetKeyCode() == EGSS_KEY_F2)
			{
				Egss::Application& app = Egss::Application::Get();

				std::string path = "screenshots/egss-"
					+ std::to_string(app.GetFrameCount()) + ".png";

				app.CaptureFrame(path);
				return true;
			}

			// F1 rather than Tab: ImGui uses Tab to cycle widget focus, which
			// turns a slider into a text field and then swallows the keyboard.
			if (e.GetKeyCode() != EGSS_KEY_F1)
				return false;

			g_ActiveDemo = (g_ActiveDemo + 1) % s_DemoCount;

			// Handled, so it stops here and no demo layer sees it.
			return true;
		});
	}
private:
	void SetDemo(DemoId demo)
	{
		g_ActiveDemo = demo;
	}

};
