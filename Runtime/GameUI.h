#pragma once

// The game UI kit: screens of widgets a game shows over itself (a HUD) or in
// its world (a monitor, a sign), described in .gsui files and driven by
// values the game publishes and commands it understands.
//
// The load-bearing idea is that UI never changes the game itself. A button
// or a slider *submits a game-console command* -- "fire", "speed 22" --
// which runs on the next fixed step like anything typed into the terminal,
// and what the UI shows is read from named values the game sets
// (Context::Set("launcher.speed", 22)). So a screen is pure data: it can be
// written, reloaded and swapped without the game knowing, the terminal and
// the HUD can never disagree about what the cannon is set to, and a script
// drives the same game through the same words (ui.command).
//
// A .gsui file is JSON:
//
//   { "theme": { "accent": "#ff8a1c", ... },
//     "root": { "type": "panel", "anchor": "bottom-left", "pos": [24, 24],
//               "size": [380, 0], "layout": "vertical", "children": [
//       { "type": "label", "text": "Speed {launcher.speed} m/s" },
//       { "type": "slider", "bind": "launcher.speed", "min": 1, "max": 60,
//         "command": "speed" },
//       { "type": "button", "text": "FIRE", "command": "fire" } ] } }
//
// Sizes are points on a 1080-line screen, scaled to the real one, so a HUD
// keeps its proportions in any window. Widgets: panel, label, button,
// slider, list, bar, crosshair, prompt, spacer. Each takes "visible": a
// value name that hides it while empty, "0" or "false".
//
// Screens are looked for at ui/<name>.gsui among the project's assets; a
// game may also register one built in (RegisterBuiltin), which is what a
// project without the file gets.
//
// Drawing is into an ImDrawList: the screen's foreground list for a HUD, or
// a private one rendered into a texture (Texture below) for a screen in the
// world. Everything here runs per frame, outside the simulation.

#include <GS.h>
#include <imgui.h>
#include <imgui_internal.h>   // ImGui::RegisterFontAtlas

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace GameUI
{
	struct Theme
	{
		ImU32 Panel = IM_COL32(12, 15, 20, 214);
		ImU32 PanelBorder = IM_COL32(255, 255, 255, 34);
		ImU32 Text = IM_COL32(236, 240, 244, 255);
		ImU32 Dim = IM_COL32(150, 160, 172, 255);
		ImU32 Accent = IM_COL32(255, 138, 28, 255);
		ImU32 Good = IM_COL32(120, 230, 150, 255);
		ImU32 Button = IM_COL32(40, 46, 56, 235);
		ImU32 ButtonHot = IM_COL32(62, 70, 84, 245);
		ImU32 Track = IM_COL32(255, 255, 255, 30);
		float Rounding = 8.0f;
		float FontSize = 19.0f;
	};

	struct Widget
	{
		std::string Type = "panel";
		std::string Id, Text, Command, Bind, Items, Visible, Style, Align;
		int Anchor = 0;                      // 0 top-left ... 8 bottom-right, row by row
		bool StretchX = false, StretchY = false;
		ImVec2 Pos{ 0.0f, 0.0f }, Size{ 0.0f, 0.0f };
		int Layout = 0;                      // 0 by anchor, 1 vertical, 2 horizontal
		float Padding = 0.0f, Gap = 8.0f, FontSize = 0.0f;
		float Min = 0.0f, Max = 1.0f;
		bool Log = false;                    // a slider over orders of magnitude (mass)
		int Rows = 6;                        // a list's visible rows
		std::string Format = "%.2f";
		std::vector<Widget> Children;

		// Laid out this frame, in pixels.
		ImVec2 Min_{ 0, 0 }, Max_{ 0, 0 };
	};

	struct Screen
	{
		std::string Name;
		Theme Style;
		Widget Root;
		bool Shown = false;
	};

	// What the pointer did this frame, in the drawing's own pixels.
	struct Pointer
	{
		ImVec2 Position{ -1e9f, -1e9f };
		bool Down = false, Clicked = false, Released = false;
		float Wheel = 0.0f;
	};

	inline ImU32 ParseColour(const std::string& text, ImU32 fallback)
	{
		// "#rrggbb" or "#rrggbbaa".
		if (text.size() != 7 && text.size() != 9)
			return fallback;
		unsigned int v = 0;
		if (std::sscanf(text.c_str() + 1, "%x", &v) != 1)
			return fallback;
		if (text.size() == 7)
			return IM_COL32((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
		return IM_COL32((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
	}

	inline ImU32 Fade(ImU32 colour, float alpha)
	{
		unsigned int a = (unsigned int)std::clamp((float)((colour >> IM_COL32_A_SHIFT) & 255) * alpha, 0.0f, 255.0f);
		return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
	}

	// ---- parsing --------------------------------------------------------

	inline int AnchorOf(const std::string& name)
	{
		static const char* names[9] = { "top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right" };
		for (int i = 0; i < 9; i++)
			if (name == names[i])
				return i;
		if (name == "centre")
			return 4;
		return 0;
	}

	inline void ParseWidget(const GS::JsonValue& json, Widget& w)
	{
		w.Type = json["type"].GetString();
		if (w.Type.empty())
			w.Type = "panel";
		w.Id = json["id"].GetString();
		w.Text = json["text"].GetString();
		w.Command = json["command"].GetString();
		w.Bind = json["bind"].GetString();
		w.Items = json["items"].GetString();
		w.Visible = json["visible"].GetString();
		w.Style = json["style"].GetString();
		w.Align = json["align"].GetString();
		w.Anchor = AnchorOf(json["anchor"].GetString());
		std::string stretch = json["stretch"].GetString();
		w.StretchX = stretch == "x" || stretch == "both";
		w.StretchY = stretch == "y" || stretch == "both";
		const GS::JsonValue& pos = json["pos"];
		if (pos.IsArray() && pos.Size() == 2)
			w.Pos = ImVec2((float)pos[0].GetDouble(), (float)pos[1].GetDouble());
		const GS::JsonValue& size = json["size"];
		if (size.IsArray() && size.Size() == 2)
			w.Size = ImVec2((float)size[0].GetDouble(), (float)size[1].GetDouble());
		std::string layout = json["layout"].GetString();
		w.Layout = layout == "vertical" ? 1 : layout == "horizontal" ? 2 : 0;
		w.Padding = (float)json["padding"].GetDouble(w.Type == "panel" ? 14.0 : 0.0);
		w.Gap = (float)json["gap"].GetDouble(8.0);
		w.FontSize = (float)json["fontSize"].GetDouble(0.0);
		w.Min = (float)json["min"].GetDouble(0.0);
		w.Max = (float)json["max"].GetDouble(1.0);
		w.Log = json["log"].GetBool(false);
		w.Rows = (int)json["rows"].GetDouble(6.0);
		if (json["format"].IsString())
			w.Format = json["format"].GetString();
		const GS::JsonValue& children = json["children"];
		for (size_t i = 0; children.IsArray() && i < children.Size(); i++)
		{
			Widget child;
			ParseWidget(children[i], child);
			w.Children.push_back(std::move(child));
		}
	}

	inline bool Parse(const std::string& text, Screen& out, std::string& error)
	{
		GS::JsonValue json;
		if (!GS::JsonValue::Parse(text, json, error))
			return false;
		if (!json.IsObject() || !json["root"].IsObject())
		{
			error = "a screen needs a \"root\" object";
			return false;
		}
		const GS::JsonValue& theme = json["theme"];
		if (theme.IsObject())
		{
			Theme& t = out.Style;
			t.Panel = ParseColour(theme["panel"].GetString(), t.Panel);
			t.PanelBorder = ParseColour(theme["border"].GetString(), t.PanelBorder);
			t.Text = ParseColour(theme["text"].GetString(), t.Text);
			t.Dim = ParseColour(theme["dim"].GetString(), t.Dim);
			t.Accent = ParseColour(theme["accent"].GetString(), t.Accent);
			t.Good = ParseColour(theme["good"].GetString(), t.Good);
			t.Button = ParseColour(theme["button"].GetString(), t.Button);
			t.ButtonHot = ParseColour(theme["buttonHot"].GetString(), t.ButtonHot);
			t.Track = ParseColour(theme["track"].GetString(), t.Track);
			t.Rounding = (float)theme["rounding"].GetDouble(t.Rounding);
			t.FontSize = (float)theme["fontSize"].GetDouble(t.FontSize);
		}
		ParseWidget(json["root"], out.Root);
		return true;
	}

	// ---- the game's font ---------------------------------------------------
	//
	// A game's text is set in the game's own font, not whatever the host
	// happens to use for its panels -- the editor's is a monospace one, a
	// shipped player's ImGui's built-in -- or a HUD would look different in
	// the editor's Play from the game it is, and the two frames that are
	// meant to be byte-identical would not be. DejaVu Sans: from the project's
	// fonts/ folder if it has one, else the engine's (exports carry it).
	// ImGui 1.92 bakes glyphs at whatever size is asked for, so one font
	// serves every size, crisp. Loaded on first use, inside a frame, and
	// again if the host rebuilt its atlas (a font change in the editor).
	// The fonts live in an atlas of the game's own, registered with the
	// context, rather than in the host's: where a glyph lands in an atlas
	// depends on what else is in it, a glyph's edge is filtered from its
	// neighbours, and the editor's atlas holds the editor's fonts -- so the
	// same text came out three pixels different in the editor's Play and in
	// GSPlayer, which are meant to agree byte for byte. Owned by the
	// context, so it goes when ImGui does.
	inline ImFontAtlas* Atlas()
	{
		static ImFontAtlas* atlas = nullptr;
		static ImGuiContext* owner = nullptr;
		if (!atlas || owner != ImGui::GetCurrentContext())
		{
			atlas = IM_NEW(ImFontAtlas)();
			owner = ImGui::GetCurrentContext();
			atlas->OwnerContext = owner;
			ImGui::RegisterFontAtlas(atlas);
		}
		return atlas;
	}

	// `mono` is DejaVu Sans Mono, for the terminal.
	inline ImFont* Font(bool mono = false)
	{
		static ImFont* fonts[2] = { nullptr, nullptr };
		static bool missing[2] = { false, false };
		static ImFontAtlas* loadedInto = nullptr;
		ImFontAtlas* atlas = Atlas();
		if (loadedInto != atlas)
		{
			fonts[0] = fonts[1] = nullptr;
			loadedInto = atlas;
		}
		ImFont*& font = fonts[mono ? 1 : 0];
		if (font)
			return font;
		if (missing[mono ? 1 : 0])
			return ImGui::GetFont();
		std::string file = mono ? "DejaVuSansMono.ttf" : "DejaVuSans.ttf";
		for (const std::string& path : { GS::Assets::Resolve("fonts/" + file), "assets/fonts/" + file })
		{
			std::ifstream probe(path, std::ios::binary);
			if (probe && (font = atlas->AddFontFromFileTTF(path.c_str(), 20.0f)) != nullptr)
				return font;
		}
		missing[mono ? 1 : 0] = true;
		GS_WARN("GameUI: no fonts/{0} -- the game's text falls back to the host's font", file);
		return ImGui::GetFont();
	}

	// Text in one of those fonts. Its atlas's texture is bound for the text
	// alone: shapes are drawn from the context atlas's white pixel.
	inline void AddText(ImDrawList* list, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text)
	{
		list->PushTexture(font->OwnerAtlas->TexRef);
		list->AddText(font, size, at, colour, text);
		list->PopTexture();
	}

	// ---- primitives, also used directly by the game ----------------------

	inline void Text(ImDrawList* list, float size, ImVec2 at, ImU32 colour, const std::string& text)
	{
		AddText(list, Font(), size, at, colour, text.c_str());
	}

	inline ImVec2 Measure(float size, const std::string& text)
	{
		return Font()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
	}

	// "[E] Take control": a key cap and its action, centred on `at`.
	inline void Prompt(ImDrawList* list, ImVec2 at, const std::string& key, const std::string& text, const Theme& theme, float scale)
	{
		float size = 22.0f * scale;
		ImVec2 keySize = Measure(size, key), textSize = Measure(size, text);
		float capW = std::max(keySize.x + 18.0f * scale, 34.0f * scale), capH = size + 12.0f * scale;
		float w = capW + 12.0f * scale + textSize.x + 22.0f * scale, h = capH + 14.0f * scale;
		ImVec2 lo(at.x - w * 0.5f, at.y - h * 0.5f), hi(at.x + w * 0.5f, at.y + h * 0.5f);
		list->AddRectFilled(ImVec2(lo.x + 3 * scale, lo.y + 4 * scale), ImVec2(hi.x + 3 * scale, hi.y + 4 * scale), IM_COL32(0, 0, 0, 90), h * 0.5f);
		list->AddRectFilled(lo, hi, theme.Panel, h * 0.5f);
		list->AddRect(lo, hi, theme.PanelBorder, h * 0.5f, 0, 1.0f);
		ImVec2 capLo(lo.x + 7.0f * scale, at.y - capH * 0.5f), capHi(capLo.x + capW, at.y + capH * 0.5f);
		list->AddRectFilled(capLo, capHi, theme.Accent, 7.0f * scale);
		list->AddRectFilled(ImVec2(capLo.x, capHi.y - 4.0f * scale), capHi, IM_COL32(0, 0, 0, 60), 7.0f * scale, ImDrawFlags_RoundCornersBottom);
		Text(list, size, ImVec2((capLo.x + capHi.x - keySize.x) * 0.5f, at.y - keySize.y * 0.5f - 1.0f * scale), IM_COL32(20, 14, 6, 255), key);
		Text(list, size, ImVec2(capHi.x + 12.0f * scale, at.y - textSize.y * 0.5f), theme.Text, text);
	}

	// A crosshair: four ticks, a ring and a dot.
	inline void Crosshair(ImDrawList* list, ImVec2 at, ImU32 colour, float scale, bool locked)
	{
		float r = 16.0f * scale, gap = 6.0f * scale, len = 12.0f * scale, t = 2.5f * scale;
		ImU32 shadow = IM_COL32(0, 0, 0, 140);
		for (int pass = 0; pass < 2; pass++)
		{
			ImU32 c = pass == 0 ? shadow : colour;
			float o = pass == 0 ? 1.5f * scale : 0.0f;
			list->AddCircle(ImVec2(at.x + o, at.y + o), r, c, 40, t * (locked ? 1.0f : 0.6f));
			list->AddLine(ImVec2(at.x - r - len + o, at.y + o), ImVec2(at.x - gap + o, at.y + o), c, t);
			list->AddLine(ImVec2(at.x + gap + o, at.y + o), ImVec2(at.x + r + len + o, at.y + o), c, t);
			list->AddLine(ImVec2(at.x + o, at.y - r - len + o), ImVec2(at.x + o, at.y - gap + o), c, t);
			list->AddLine(ImVec2(at.x + o, at.y + gap + o), ImVec2(at.x + o, at.y + r + len + o), c, t);
			list->AddCircleFilled(ImVec2(at.x + o, at.y + o), 2.2f * scale, c);
		}
	}

	// ---- the context: values, screens, drawing ---------------------------

	class Context
	{
	public:
		using Submit = std::function<void(const std::string&)>;

		void SetSubmit(Submit submit) { m_Submit = std::move(submit); }

		void Set(const std::string& name, const std::string& value) { m_Values[name] = value; }
		void Set(const std::string& name, float value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof buffer, "%g", value);
			m_Values[name] = buffer;
		}
		std::string Get(const std::string& name) const
		{
			auto found = m_Values.find(name);
			return found == m_Values.end() ? std::string() : found->second;
		}
		float Number(const std::string& name, float fallback = 0.0f) const
		{
			auto found = m_Values.find(name);
			if (found == m_Values.end())
				return fallback;
			char* end = nullptr;
			float value = std::strtof(found->second.c_str(), &end);
			return end == found->second.c_str() ? fallback : value;
		}
		void SetList(const std::string& name, std::vector<std::string> items) { m_Lists[name] = std::move(items); }

		void RegisterBuiltin(const std::string& name, const std::string& json) { m_Builtin[name] = json; }

		// The screen of that name: loaded from the project the first time it
		// is asked for, else the built-in one, else null.
		Screen* Find(const std::string& name)
		{
			auto found = m_Screens.find(name);
			if (found != m_Screens.end())
				return found->second.get();
			std::string text;
			std::ifstream file(GS::Assets::Resolve("ui/" + name + ".gsui"), std::ios::binary);
			if (file)
			{
				std::stringstream buffer;
				buffer << file.rdbuf();
				text = buffer.str();
			}
			else if (m_Builtin.count(name))
				text = m_Builtin[name];
			else
				return nullptr;
			auto screen = std::make_unique<Screen>();
			screen->Name = name;
			std::string error;
			if (!Parse(text, *screen, error))
			{
				GS_ERROR("GameUI: screen '{0}' did not parse: {1}", name, error);
				return nullptr;
			}
			Screen* raw = screen.get();
			m_Screens[name] = std::move(screen);
			m_Order.push_back(name);
			return raw;
		}

		bool Show(const std::string& name, bool shown = true)
		{
			Screen* screen = Find(name);
			if (screen)
				screen->Shown = shown;
			return screen != nullptr;
		}
		bool IsShown(const std::string& name) const
		{
			auto found = m_Screens.find(name);
			return found != m_Screens.end() && found->second->Shown;
		}

		// Whether the pointer was over a widget that takes it, last frame:
		// a click there is the UI's, not the game's.
		bool WantsPointer() const { return m_WantsPointer; }

		// Every shown screen into `list`, over the rectangle at `origin`.
		void Draw(ImDrawList* list, ImVec2 origin, ImVec2 size, const Pointer& pointer)
		{
			m_WantsPointer = false;
			for (const std::string& name : m_Order)
			{
				Screen& screen = *m_Screens[name];
				if (screen.Shown)
					DrawScreen(screen, list, origin, size, pointer);
			}
			if (!pointer.Down)
				m_Dragging.clear();
		}

		// One screen, shown or not -- a screen drawn into a texture.
		void DrawScreen(Screen& screen, ImDrawList* list, ImVec2 origin, ImVec2 size, const Pointer& pointer)
		{
			m_Scale = std::max(0.4f, size.y / 1080.0f);
			m_Theme = &screen.Style;
			m_List = list;
			m_Pointer = pointer;
			Widget& root = screen.Root;
			ImVec2 natural = Natural(root);
			Place(root, origin, size, natural);
			Render(root);
		}

		float Scale() const { return m_Scale; }

		// "{name}" replaced by the value of that name.
		std::string Expand(const std::string& text) const
		{
			std::string out;
			for (size_t i = 0; i < text.size(); i++)
			{
				if (text[i] == '{')
				{
					size_t close = text.find('}', i);
					if (close != std::string::npos)
					{
						out += Get(text.substr(i + 1, close - i - 1));
						i = close;
						continue;
					}
				}
				out += text[i];
			}
			return out;
		}

	private:
		bool Truthy(const std::string& name) const
		{
			if (name.empty())
				return true;
			bool negate = name[0] == '!';
			std::string value = Get(negate ? name.substr(1) : name);
			bool truthy = !value.empty() && value != "0" && value != "false";
			return negate ? !truthy : truthy;
		}

		float Font(const Widget& w) const
		{
			float size = w.FontSize > 0.0f ? w.FontSize : m_Theme->FontSize;
			if (w.Style == "title") size *= 1.35f;
			if (w.Style == "big") size *= 2.2f;
			if (w.Style == "small") size *= 0.82f;
			return size * m_Scale;
		}

		// The size a widget wants, in pixels.
		ImVec2 Natural(Widget& w)
		{
			float s = m_Scale;
			ImVec2 size(w.Size.x * s, w.Size.y * s);
			ImVec2 content(0.0f, 0.0f);
			if (w.Type == "label" || w.Type == "button")
			{
				ImVec2 text = Measure(Font(w), Expand(w.Text));
				content = w.Type == "button" ? ImVec2(text.x + 36.0f * s, text.y + 20.0f * s) : text;
			}
			else if (w.Type == "slider")
				content = ImVec2(280.0f * s, Font(w) + 30.0f * s);
			else if (w.Type == "bar")
				content = ImVec2(200.0f * s, 10.0f * s);
			else if (w.Type == "list")
				content = ImVec2(280.0f * s, w.Rows * (Font(w) + 16.0f * s));
			else if (w.Type == "spacer")
				content = ImVec2(0.0f, 0.0f);
			else if (w.Type == "prompt")
				content = ImVec2(0.0f, 0.0f);
			else
			{
				// A container: its children's sizes stacked, or the largest.
				float pad = w.Padding * s * 2.0f, gap = w.Gap * s;
				int shown = 0;
				for (Widget& child : w.Children)
				{
					if (!Truthy(child.Visible))
						continue;
					ImVec2 c = Natural(child);
					if (w.Layout == 1) { content.x = std::max(content.x, c.x); content.y += c.y + (shown ? gap : 0.0f); }
					else if (w.Layout == 2) { content.y = std::max(content.y, c.y); content.x += c.x + (shown ? gap : 0.0f); }
					else { content.x = std::max(content.x, c.x + child.Pos.x * s); content.y = std::max(content.y, c.y + child.Pos.y * s); }
					shown++;
				}
				content.x += pad;
				content.y += pad;
			}
			if (size.x <= 0.0f) size.x = content.x;
			if (size.y <= 0.0f) size.y = content.y;
			return size;
		}

		// Placed by its anchor inside `area` (its parent's content box).
		void Place(Widget& w, ImVec2 areaMin, ImVec2 areaSize, ImVec2 size)
		{
			float s = m_Scale;
			if (w.StretchX) size.x = areaSize.x - 2.0f * w.Pos.x * s;
			if (w.StretchY) size.y = areaSize.y - 2.0f * w.Pos.y * s;
			int col = w.Anchor % 3, row = w.Anchor / 3;
			float x = col == 0 ? areaMin.x + w.Pos.x * s
				: col == 1 ? areaMin.x + (areaSize.x - size.x) * 0.5f + w.Pos.x * s
				: areaMin.x + areaSize.x - size.x - w.Pos.x * s;
			float y = row == 0 ? areaMin.y + w.Pos.y * s
				: row == 1 ? areaMin.y + (areaSize.y - size.y) * 0.5f + w.Pos.y * s
				: areaMin.y + areaSize.y - size.y - w.Pos.y * s;
			Arrange(w, ImVec2(x, y), size);
		}

		void Arrange(Widget& w, ImVec2 at, ImVec2 size)
		{
			w.Min_ = at;
			w.Max_ = ImVec2(at.x + size.x, at.y + size.y);
			float pad = w.Padding * m_Scale, gap = w.Gap * m_Scale;
			ImVec2 inner(at.x + pad, at.y + pad), innerSize(size.x - 2.0f * pad, size.y - 2.0f * pad);
			float cursor = 0.0f;
			for (Widget& child : w.Children)
			{
				if (!Truthy(child.Visible))
					continue;
				ImVec2 c = Natural(child);
				if (w.Layout == 1)
				{
					// A stack fills its width, as a column of controls should.
					Arrange(child, ImVec2(inner.x, inner.y + cursor), ImVec2(innerSize.x, c.y));
					cursor += c.y + gap;
				}
				else if (w.Layout == 2)
				{
					Arrange(child, ImVec2(inner.x + cursor, inner.y), ImVec2(c.x, innerSize.y));
					cursor += c.x + gap;
				}
				else
					Place(child, inner, innerSize, c);
			}
		}

		bool Over(const Widget& w) const
		{
			const ImVec2& p = m_Pointer.Position;
			return p.x >= w.Min_.x && p.x < w.Max_.x && p.y >= w.Min_.y && p.y < w.Max_.y;
		}

		void Send(const std::string& line)
		{
			if (m_Submit && !line.empty())
				m_Submit(Expand(line));
		}

		static std::string Formatted(const char* format, float value)
		{
			char buffer[48];
			std::snprintf(buffer, sizeof buffer, format, value);
			return buffer;
		}

		void Render(Widget& w)
		{
			if (!Truthy(w.Visible))
				return;
			const Theme& t = *m_Theme;
			ImDrawList* list = m_List;
			float s = m_Scale;
			ImU32 textColour = w.Style == "dim" ? t.Dim : w.Style == "accent" ? t.Accent : w.Style == "good" ? t.Good : t.Text;

			if (w.Type == "panel")
			{
				if (w.Style != "clear")
				{
					float r = t.Rounding * s;
					list->AddRectFilled(ImVec2(w.Min_.x + 4 * s, w.Min_.y + 6 * s), ImVec2(w.Max_.x + 4 * s, w.Max_.y + 6 * s), IM_COL32(0, 0, 0, 70), r);
					list->AddRectFilled(w.Min_, w.Max_, t.Panel, r);
					list->AddRect(w.Min_, w.Max_, t.PanelBorder, r, 0, 1.0f);
					if (w.Style == "accent")
						list->AddRectFilled(w.Min_, ImVec2(w.Max_.x, w.Min_.y + 3.0f * s), t.Accent, r, ImDrawFlags_RoundCornersTop);
				}
				if (Over(w) && w.Style != "clear")
					m_WantsPointer = true;
			}
			else if (w.Type == "label")
			{
				std::string text = Expand(w.Text);
				ImVec2 size = Measure(Font(w), text);
				float x = w.Align == "right" ? w.Max_.x - size.x : w.Align == "center" ? (w.Min_.x + w.Max_.x - size.x) * 0.5f : w.Min_.x;
				Text(list, Font(w), ImVec2(x, w.Min_.y), textColour, text);
			}
			else if (w.Type == "button")
			{
				bool over = Over(w);
				bool on = !w.Bind.empty() && Truthy(w.Bind);
				ImU32 fill = on ? t.Accent : over ? t.ButtonHot : t.Button;
				if (w.Style == "accent" && !on)
					fill = over ? Fade(t.Accent, 0.95f) : Fade(t.Accent, 0.8f);
				float r = 6.0f * s;
				list->AddRectFilled(w.Min_, w.Max_, fill, r);
				if (over && m_Pointer.Down)
					list->AddRectFilled(w.Min_, w.Max_, IM_COL32(0, 0, 0, 50), r);
				list->AddRect(w.Min_, w.Max_, t.PanelBorder, r, 0, 1.0f);
				std::string text = Expand(w.Text);
				ImVec2 size = Measure(Font(w), text);
				ImU32 ink = (on || w.Style == "accent") ? IM_COL32(24, 16, 8, 255) : t.Text;
				Text(list, Font(w), ImVec2((w.Min_.x + w.Max_.x - size.x) * 0.5f, (w.Min_.y + w.Max_.y - size.y) * 0.5f), ink, text);
				if (over)
				{
					m_WantsPointer = true;
					if (m_Pointer.Clicked)
						Send(w.Command);
				}
			}
			else if (w.Type == "slider")
				RenderSlider(w, textColour);
			else if (w.Type == "bar")
			{
				float v = std::clamp((Number(w.Bind) - w.Min) / std::max(1e-6f, w.Max - w.Min), 0.0f, 1.0f);
				float r = (w.Max_.y - w.Min_.y) * 0.5f;
				list->AddRectFilled(w.Min_, w.Max_, t.Track, r);
				if (v > 0.0f)
					list->AddRectFilled(w.Min_, ImVec2(w.Min_.x + (w.Max_.x - w.Min_.x) * v, w.Max_.y), w.Style == "good" ? t.Good : t.Accent, r);
			}
			else if (w.Type == "list")
				RenderList(w);
			else if (w.Type == "crosshair")
			{
				ImVec2 at(Number(w.Bind + ".x", -1.0f), Number(w.Bind + ".y", -1.0f));
				if (at.x >= 0.0f)
					Crosshair(list, ImVec2(w.Min_.x + at.x * (w.Max_.x - w.Min_.x), w.Min_.y + at.y * (w.Max_.y - w.Min_.y)),
						Truthy(w.Bind + ".warn") ? IM_COL32(255, 90, 70, 255) : t.Accent, s, Truthy(w.Bind + ".hit"));
			}
			else if (w.Type == "prompt")
			{
				ImVec2 centre((w.Min_.x + w.Max_.x) * 0.5f, (w.Min_.y + w.Max_.y) * 0.5f);
				Prompt(list, centre, w.Style.empty() ? "E" : w.Style, Expand(w.Text), t, s);
			}

			for (Widget& child : w.Children)
				Render(child);
		}

		void RenderSlider(Widget& w, ImU32 textColour)
		{
			const Theme& t = *m_Theme;
			float s = m_Scale;
			ImDrawList* list = m_List;
			auto toUnit = [&](float v)
			{
				if (w.Log)
					return (std::log(std::max(v, 1e-6f)) - std::log(w.Min)) / (std::log(w.Max) - std::log(w.Min));
				return (v - w.Min) / std::max(1e-6f, w.Max - w.Min);
			};
			auto fromUnit = [&](float u)
			{
				if (w.Log)
					return std::exp(std::log(w.Min) + u * (std::log(w.Max) - std::log(w.Min)));
				return w.Min + u * (w.Max - w.Min);
			};

			std::string key = w.Bind;
			bool dragging = m_Dragging == key && m_Pointer.Down;
			float value = dragging ? m_DragValue : Number(w.Bind, w.Min);

			// The label and value above, the track below.
			float font = Font(w);
			Text(list, font, w.Min_, t.Dim, Expand(w.Text));
			std::string shown = Formatted(w.Format.c_str(), value);
			ImVec2 shownSize = Measure(font, shown);
			Text(list, font, ImVec2(w.Max_.x - shownSize.x, w.Min_.y), textColour, shown);

			float trackY = w.Max_.y - 11.0f * s;
			ImVec2 lo(w.Min_.x, trackY - 3.0f * s), hi(w.Max_.x, trackY + 3.0f * s);
			list->AddRectFilled(lo, hi, t.Track, 3.0f * s);
			float u = std::clamp(toUnit(value), 0.0f, 1.0f);
			float knobX = lo.x + (hi.x - lo.x) * u;
			list->AddRectFilled(lo, ImVec2(knobX, hi.y), t.Accent, 3.0f * s);
			bool over = m_Pointer.Position.x >= w.Min_.x - 8 * s && m_Pointer.Position.x <= w.Max_.x + 8 * s
				&& m_Pointer.Position.y >= trackY - 14 * s && m_Pointer.Position.y <= trackY + 14 * s;
			list->AddCircleFilled(ImVec2(knobX, trackY), (over || dragging ? 9.0f : 7.5f) * s, IM_COL32(245, 245, 245, 255), 24);
			list->AddCircle(ImVec2(knobX, trackY), (over || dragging ? 9.0f : 7.5f) * s, t.Accent, 24, 2.0f * s);

			if (over || dragging)
				m_WantsPointer = true;
			if (over && m_Pointer.Clicked)
			{
				m_Dragging = key;
				dragging = true;
			}
			if (dragging)
			{
				float nu = std::clamp((m_Pointer.Position.x - lo.x) / std::max(1.0f, hi.x - lo.x), 0.0f, 1.0f);
				float next = fromUnit(nu);
				// Sent while dragging only when it has moved by a visible
				// amount -- each one is a command on the next step -- and
				// always on release.
				if (std::abs(toUnit(next) - toUnit(m_DragValue)) > 0.004f || m_DragValue != next)
				{
					m_DragValue = next;
					Set(w.Bind, next);
					if (m_SentFrames++ % 3 == 0)
						Send(w.Command + " " + Formatted("%.4g", next));
				}
			}
			if (m_Dragging == key && m_Pointer.Released)
			{
				Send(w.Command + " " + Formatted("%.4g", m_DragValue));
				m_Dragging.clear();
			}
		}

		void RenderList(Widget& w)
		{
			const Theme& t = *m_Theme;
			float s = m_Scale;
			ImDrawList* list = m_List;
			const std::vector<std::string>& items = m_Lists[w.Items];
			const std::vector<std::string>& details = m_Lists[w.Items + ".detail"];
			std::string selected = Get(w.Bind);
			// -1 when the value is none of the items -- a launcher set up by
			// hand is no preset -- so nothing is shown chosen.
			int index = -1;
			for (size_t i = 0; i < items.size(); i++)
				if (items[i] == selected)
					index = (int)i;

			bool over = Over(w);
			if (over)
				m_WantsPointer = true;
			// The wheel moves the selection when over the list, or anywhere
			// when the list says so ("style": "wheel").
			if ((over || w.Style == "wheel") && m_Pointer.Wheel != 0.0f && !items.empty())
			{
				int next = index < 0 ? 0 : std::clamp(index + (m_Pointer.Wheel > 0.0f ? -1 : 1), 0, (int)items.size() - 1);
				if (next != index)
				{
					index = next;
					Set(w.Bind, items[(size_t)index]);
					Send(w.Command + " " + items[(size_t)index]);
				}
			}

			float font = Font(w);
			float row = font + 16.0f * s;
			int visible = std::max(1, w.Rows);
			int first = std::clamp(std::max(index, 0) - visible / 2, 0, std::max(0, (int)items.size() - visible));
			for (int k = 0; k < visible && first + k < (int)items.size(); k++)
			{
				int i = first + k;
				ImVec2 lo(w.Min_.x, w.Min_.y + k * row), hi(w.Max_.x, w.Min_.y + (k + 1) * row - 3.0f * s);
				bool here = m_Pointer.Position.x >= lo.x && m_Pointer.Position.x < hi.x && m_Pointer.Position.y >= lo.y && m_Pointer.Position.y < hi.y;
				bool chosen = i == index;
				if (chosen)
				{
					list->AddRectFilled(lo, hi, Fade(t.Accent, 0.22f), 5.0f * s);
					list->AddRectFilled(lo, ImVec2(lo.x + 4.0f * s, hi.y), t.Accent, 2.0f * s);
				}
				else if (here)
					list->AddRectFilled(lo, hi, IM_COL32(255, 255, 255, 18), 5.0f * s);
				Text(list, font, ImVec2(lo.x + 14.0f * s, lo.y + 7.0f * s), chosen ? t.Text : t.Dim, items[(size_t)i]);
				if (i < (int)details.size())
				{
					ImVec2 size = Measure(font * 0.82f, details[(size_t)i]);
					Text(list, font * 0.82f, ImVec2(hi.x - size.x - 10.0f * s, lo.y + 9.0f * s), t.Dim, details[(size_t)i]);
				}
				if (here && m_Pointer.Clicked && !chosen)
				{
					Set(w.Bind, items[(size_t)i]);
					Send(w.Command + " " + items[(size_t)i]);
				}
			}
			// Where in the list this window is.
			if ((int)items.size() > visible)
			{
				float h = w.Max_.y - w.Min_.y;
				float top = h * first / items.size(), height = h * visible / items.size();
				list->AddRectFilled(ImVec2(w.Max_.x - 3.0f * s, w.Min_.y + top), ImVec2(w.Max_.x, w.Min_.y + top + height), t.Track, 2.0f * s);
			}
		}

		std::map<std::string, std::string> m_Values;
		std::map<std::string, std::vector<std::string>> m_Lists;
		std::map<std::string, std::string> m_Builtin;
		std::map<std::string, std::unique_ptr<Screen>> m_Screens;
		std::vector<std::string> m_Order;
		Submit m_Submit;

		// This frame's drawing.
		float m_Scale = 1.0f;
		const Theme* m_Theme = nullptr;
		ImDrawList* m_List = nullptr;
		Pointer m_Pointer;
		bool m_WantsPointer = false;
		std::string m_Dragging;
		float m_DragValue = 0.0f;
		unsigned int m_SentFrames = 0;
	};

	// A screen in the world: a framebuffer an ImDrawList is drawn into, and
	// the texture that is its colour. Drawn inside the host's ImGui frame;
	// the scene shows it the frame after.
	class Texture
	{
	public:
		// Begins a frame's drawing at this size; Draw on the list, then End.
		ImDrawList* Begin(int width, int height)
		{
			if (!m_List)
				m_List = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
			if (!m_Target || m_Width != width || m_Height != height)
			{
				GS::FramebufferSpecification spec;
				spec.Width = (unsigned int)width;
				spec.Height = (unsigned int)height;
				spec.Attachments = { GS::FramebufferTextureFormat::RGBA8 };
				m_Target.reset(GS::Framebuffer::Create(spec));
				m_Width = width;
				m_Height = height;
				m_Texture.reset();
			}
			m_List->_ResetForNewFrame();
			m_List->Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;   // no atlas-textured lines; see GameModes::Frame
			m_List->PushClipRect(ImVec2(0, 0), ImVec2((float)width, (float)height));
			m_List->PushTexture(ImGui::GetIO().Fonts->TexRef);
			return m_List.get();
		}

		void End(const glm::vec4& clear)
		{
			GS::RenderCommand::PushTarget();
			m_Target->Bind();
			GS::RenderCommand::SetClearColor(clear);
			GS::RenderCommand::Clear();
			GS::ImGuiLayer::RenderDrawList(m_List.get(), (float)m_Width, (float)m_Height);
			GS::RenderCommand::PopTarget();
			unsigned int handle = m_Target->GetColorAttachmentRendererID(0);
			if (!m_Texture || m_Handle != handle)
			{
				m_Texture.reset(GS::Texture2D::CreateFromHandle(handle, (unsigned int)m_Width, (unsigned int)m_Height));
				m_Handle = handle;
			}
		}

		const std::shared_ptr<GS::Texture2D>& Get() const { return m_Texture; }

	private:
		std::unique_ptr<ImDrawList> m_List;
		std::unique_ptr<GS::Framebuffer> m_Target;
		std::shared_ptr<GS::Texture2D> m_Texture;
		unsigned int m_Handle = 0;
		int m_Width = 0, m_Height = 0;
	};
}
