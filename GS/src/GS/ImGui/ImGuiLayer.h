#pragma once

#include "GS/Layer.h"

#include "GS/Events/ApplicationEvent.h"
#include "GS/Events/KeyEvent.h"
#include "GS/Events/MouseEvent.h"

#include <string>

struct ImFont;
struct ImDrawList;

namespace GS {

	// Pushed as an overlay by Application, so it sits above every game layer
	// and sees input first.
	class GS_API ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer();

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& e) override;

		// Bracket the per-layer OnImGuiRender calls.
		void Begin();
		void End();

		// When set, ImGui consumes mouse and keyboard input that lands on its
		// windows instead of letting it fall through to the game.
		void BlockEvents(bool block) { m_BlockEvents = block; }

		// A game that draws to the whole window wants this off; an editor
		// wants it on.
		void EnableDockspace(bool enable) { m_DockspaceEnabled = enable; }

		// Lets a panel be dragged clean out of the window and become its own
		// OS window. Must be set before OnAttach -- the flag is read once when
		// the ImGui context is created, and the backend builds or skips its
		// platform interface on the strength of it.
		//
		// Off by default. Every extra viewport is a real window with its own
		// GL context, which is a cost a game that never undocks a panel should
		// not pay; and see the changelog for what it does to the mouse
		// coordinate space.
		void EnableViewports(bool enable) { m_ViewportsEnabled = enable; }
		bool ViewportsEnabled() const { return m_ViewportsEnabled; }

		// A real font instead of ImGui's tiny compiled-in default
		// (ProggyClean) -- static, not an instance setter, because
		// ImGuiLayer is constructed and attached inside Application's own
		// constructor, before the owning app's constructor body (the
		// earliest an instance method could be called) ever runs. The one
		// place early enough is GS::CreateApplication() itself, before
		// `new <App>()`. Empty path (the default) changes nothing -- every
		// other engine user keeps today's font.
		static void SetFontPath(const std::string& path, float sizePixels = 16.0f)
		{
			s_FontPath = path;
			s_FontSizePixels = sizePixels;
		}

		// No imgui.ini read or written. A shipped game has no panels whose
		// layout is worth remembering, and must not leave files in whatever
		// folder it was launched from. Same timing rule as SetFontPath.
		static void DisableIniFile() { s_NoIniFile = true; }

		// Whether a UI text field has the keyboard right now -- so a running
		// game's key polling can stand aside while someone types in the
		// editor. Here rather than callers asking ImGui themselves: ImGui
		// lives inside the engine library, and on Windows a DLL exports only
		// what it marks, so code outside it (the runtime, scripts) cannot
		// link against ImGui directly. False when there is no UI context.
		static bool WantsKeyboard();
		// The same for the mouse: true while it is over a panel, so a click
		// on a button is not also a tap in the game under it.
		static bool WantsMouse();

		// Draws one ImDrawList, built by the caller, into whatever target is
		// bound -- a framebuffer that becomes a texture: a game screen on a
		// monitor. ImGui's own renderer, so the same font, the same
		// anti-aliasing and the same look as on screen. (0, 0) is the top left
		// and (width, height) the bottom right, so the texture comes out with
		// its top at v = 1. Call inside a frame: glyphs the list asked for are
		// uploaded by this call.
		static void RenderDrawList(ImDrawList* list, float width, float height);
		// Uploads whatever any font atlas has pending -- one created this
		// frame, glyphs baked this frame -- so draw lists built since can be
		// rendered this frame. RenderDrawList calls it; a game that draws
		// text over its view calls it before the frame ends.
		static void UploadTextures();

		// Runtime font switching, unlike the static SetFontPath above (which
		// only works pre-OnAttach) -- called any time after the ImGui
		// context exists. Queues the request; Begin() applies it at the
		// start of the next frame, since rebuilding io.Fonts mid-frame
		// would leave already-drawn widgets pointing at freed glyphs.
		void RequestFontReload(const std::string& controlsPath, float controlsSize,
			const std::string& editorPath, float editorSize,
			const std::string& terminalPath, float terminalSize)
		{
			m_PendingControlsPath = controlsPath; m_PendingControlsSize = controlsSize;
			m_PendingEditorPath = editorPath; m_PendingEditorSize = editorSize;
			m_PendingTerminalPath = terminalPath; m_PendingTerminalSize = terminalSize;
			m_FontReloadPending = true;
		}

		ImFont* GetControlsFont() const { return m_ControlsFont; }
		ImFont* GetEditorFont() const { return m_EditorFont; }
		ImFont* GetTerminalFont() const { return m_TerminalFont; }
	private:
		bool m_BlockEvents = true;
		bool m_DockspaceEnabled = true;
		bool m_ViewportsEnabled = false;

		static std::string s_FontPath;
		static float s_FontSizePixels;
		static bool s_NoIniFile;

		bool m_FontReloadPending = false;
		std::string m_PendingControlsPath, m_PendingEditorPath, m_PendingTerminalPath;
		float m_PendingControlsSize = 16.0f, m_PendingEditorSize = 16.0f, m_PendingTerminalSize = 16.0f;
		ImFont* m_ControlsFont = nullptr;
		ImFont* m_EditorFont = nullptr;
		ImFont* m_TerminalFont = nullptr;

		void ApplyPendingFontReload();
	};

}
