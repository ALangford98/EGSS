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

#include "Demo.h"

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

		int current = (int)g_ActiveDemo;

		// Combo writes through the int, and returns true only on the frame the
		// selection actually changed.
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::Combo("##demo", &current, s_DemoNames, (int)Demo::Count))
			SetDemo((Demo)current);

		ImGui::Spacing();

		// Buttons as well as the dropdown: one click instead of two, and it
		// makes the available demos visible without opening anything.
		for (int i = 0; i < (int)Demo::Count; i++)
		{
			if (i > 0)
				ImGui::SameLine();

			bool active = i == (int)g_ActiveDemo;

			// Highlight the live one so the panel reads at a glance.
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));

			if (ImGui::Button(ShortName(i), ImVec2(90.0f, 0.0f)))
				SetDemo((Demo)i);

			if (active)
				ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextDisabled("F1 cycles. Each demo has its own panel.");

		ImGui::End();
	}

	void OnEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::KeyPressedEvent>([](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			// F1 rather than Tab: ImGui uses Tab to cycle widget focus, which
			// turns a slider into a text field and then swallows the keyboard.
			if (e.GetKeyCode() != EGSS_KEY_F1)
				return false;

			g_ActiveDemo = (Demo)(((int)g_ActiveDemo + 1) % (int)Demo::Count);

			// Handled, so it stops here and no demo layer sees it.
			return true;
		});
	}
private:
	void SetDemo(Demo demo)
	{
		g_ActiveDemo = demo;
	}

	// The dropdown entries carry a description; the buttons need to be short.
	static const char* ShortName(int index)
	{
		switch ((Demo)index)
		{
			case Demo::Breakout: return "Breakout";
			case Demo::Cube3D:   return "Cube3D";
			case Demo::Physics2D: return "Physics";
			case Demo::Lighting2D: return "Lighting2D";
			default: break;
		}
		return "?";
	}
};
