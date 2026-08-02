#pragma once

#include "Egss/Core.h"
#include "Egss/Renderer/Camera.h"

#include <glm/glm.hpp>

namespace Egss {

	// 3D camera with a field of view, so distant things get smaller -- the one
	// difference from OrthographicCamera that matters.
	//
	// Orientation is yaw/pitch in degrees rather than a quaternion or a target
	// point: it is what a fly camera needs, it cannot gimbal-lock while pitch
	// stays clamped, and it keeps the maths readable. Roll is deliberately
	// absent; add it when something needs it.
	class EGSS_API PerspectiveCamera : public Camera
	{
	public:
		PerspectiveCamera(float fovDegrees, float aspectRatio,
			float nearClip = 0.1f, float farClip = 1000.0f);

		void SetProjection(float fovDegrees, float aspectRatio, float nearClip, float farClip);
		// Called on window resize; keeps the field of view and clip planes.
		void SetAspectRatio(float aspectRatio);

		const glm::vec3& GetPosition() const { return m_Position; }
		void SetPosition(const glm::vec3& position) { m_Position = position; RecalculateViewMatrix(); }

		// Vertical field of view in degrees. Framing a camera on an object
		// needs it: the distance that fits a sphere of radius r is
		// r / sin(fov / 2).
		float GetFov() const { return m_Fov; }
		float GetAspectRatio() const { return m_AspectRatio; }
		float GetNearClip() const { return m_NearClip; }
		float GetFarClip() const { return m_FarClip; }

		float GetYaw() const { return m_Yaw; }
		float GetPitch() const { return m_Pitch; }
		// Pitch is clamped to just inside straight up/down, where the view
		// matrix would otherwise be degenerate.
		void SetRotation(float yawDegrees, float pitchDegrees);

		// Unit vectors in world space, derived from yaw/pitch. Movement code
		// wants these rather than the matrix.
		glm::vec3 GetForward() const;
		glm::vec3 GetRight() const;
		glm::vec3 GetUp() const;
	private:
		void RecalculateViewMatrix();
	private:
		float m_Fov = 45.0f;
		float m_AspectRatio = 16.0f / 9.0f;
		float m_NearClip = 0.1f;
		float m_FarClip = 1000.0f;

		glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
		float m_Yaw = -90.0f;   // -90 looks down -Z, which is GL's default forward
		float m_Pitch = 0.0f;
	};

}
