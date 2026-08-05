#pragma once

#include "egsspch.h"
#include "Core.h"

namespace Egss {

	// Everything the hardware was doing at one instant, in a form that can be
	// written to a file and handed back later.
	//
	// Keys are a bitset indexed by GLFW keycode rather than a list of what is
	// down, so two snapshots compare in one memcmp -- which is what makes
	// "only write a record when something changed" cheap enough to be the
	// default.
	struct EGSS_API InputSnapshot
	{
		// GLFW_KEY_LAST is 348. Rounded up so the bitset is whole bytes.
		static constexpr int MaxKeys = 352;
		static constexpr int KeyBytes = MaxKeys / 8;
		static constexpr int MaxMouseButtons = 8;

		unsigned char Keys[KeyBytes] = {};
		unsigned char MouseButtons = 0;
		float MouseX = 0.0f;
		float MouseY = 0.0f;

		bool GetKey(int code) const
		{
			if (code < 0 || code >= MaxKeys)
				return false;

			return (Keys[code >> 3] & (1u << (code & 7))) != 0;
		}

		void SetKey(int code, bool down)
		{
			if (code < 0 || code >= MaxKeys)
				return;

			unsigned char bit = (unsigned char)(1u << (code & 7));
			if (down)
				Keys[code >> 3] |= bit;
			else
				Keys[code >> 3] &= (unsigned char)~bit;
		}

		bool GetMouseButton(int button) const
		{
			if (button < 0 || button >= MaxMouseButtons)
				return false;

			return (MouseButtons & (1u << button)) != 0;
		}

		void SetMouseButton(int button, bool down)
		{
			if (button < 0 || button >= MaxMouseButtons)
				return;

			unsigned char bit = (unsigned char)(1u << button);
			if (down)
				MouseButtons |= bit;
			else
				MouseButtons &= (unsigned char)~bit;
		}

		bool operator==(const InputSnapshot& other) const
		{
			return std::memcmp(Keys, other.Keys, KeyBytes) == 0
				&& MouseButtons == other.MouseButtons
				&& MouseX == other.MouseX
				&& MouseY == other.MouseY;
		}

		bool operator!=(const InputSnapshot& other) const { return !(*this == other); }
	};

	// Polling counterpart to the event system. Events tell you when something
	// changed; this tells you the current state, which is what continuous
	// movement wants.
	class EGSS_API Input
	{
	public:
		virtual ~Input() = default;

		inline static bool IsKeyPressed(int keycode)
		{
			return s_Playback ? s_Playback->GetKey(keycode) : s_Instance->IsKeyPressedImpl(keycode);
		}

		inline static bool IsMouseButtonPressed(int button)
		{
			return s_Playback ? s_Playback->GetMouseButton(button) : s_Instance->IsMouseButtonPressedImpl(button);
		}

		inline static std::pair<float, float> GetMousePosition()
		{
			return s_Playback ? std::pair<float, float>{ s_Playback->MouseX, s_Playback->MouseY }
							  : s_Instance->GetMousePositionImpl();
		}

		inline static float GetMouseX() { return GetMousePosition().first; }
		inline static float GetMouseY() { return GetMousePosition().second; }

		// --- Replay ----------------------------------------------------------

		// Answer every query from this snapshot instead of the hardware. The
		// pointer is borrowed, not copied -- Replay owns the storage and
		// rewrites it in place each step. Null restores live input.
		inline static void SetPlaybackSnapshot(const InputSnapshot* snapshot) { s_Playback = snapshot; }
		inline static bool IsPlayingBack() { return s_Playback != nullptr; }

		// What the input *currently reads as*, which during playback is the
		// recorded state and otherwise is the hardware.
		//
		// Deliberately not "read the hardware regardless". Sampling the
		// effective state is what lets a recording made while replaying come
		// back byte-identical to the file it was replaying, and that round
		// trip is the strongest check there is that the two halves agree.
		static InputSnapshot CaptureSnapshot();
	protected:
		virtual bool IsKeyPressedImpl(int keycode) = 0;
		virtual bool IsMouseButtonPressedImpl(int button) = 0;
		virtual std::pair<float, float> GetMousePositionImpl() = 0;
		virtual float GetMouseXImpl() = 0;
		virtual float GetMouseYImpl() = 0;
	private:
		static Input* s_Instance;
		static const InputSnapshot* s_Playback;
	};

}
