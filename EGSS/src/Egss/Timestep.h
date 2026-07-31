#pragma once

namespace Egss {

	// Seconds between frames, wrapped so the unit is explicit at call sites.
	// Implicitly converts to float, so `position += speed * ts` reads naturally.
	class Timestep
	{
	public:
		Timestep(float time = 0.0f)
			: m_Time(time) {}

		operator float() const { return m_Time; }

		float GetSeconds() const { return m_Time; }
		float GetMilliseconds() const { return m_Time * 1000.0f; }
	private:
		float m_Time;
	};

}
