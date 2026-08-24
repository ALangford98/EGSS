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

		// Point the camera with an explicit basis instead of yaw and pitch.
		//
		// **This is what a spherical world needs.** Yaw and pitch are measured
		// against a fixed world up of +Y, which is right for a flat world and
		// meaningless on a planet: standing on the equator of a sphere whose
		// axis is +Y, local up is horizontal in world terms, and a yaw/pitch
		// camera renders the ground up the side of the screen. Passing the
		// local up directly is the only way to keep the horizon level.
		//
		// `up` need not be perpendicular to `forward`; it is orthogonalised
		// against it, the way a look-at does.
		void SetOrientation(const glm::vec3& forward, const glm::vec3& up);

		// True once SetOrientation has been called, until SetRotation is used
		// again -- the two are alternatives, and mixing them silently would
		// leave the camera pointing wherever the last one said.
		bool HasExplicitOrientation() const { return m_ExplicitBasis; }

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

		bool m_ExplicitBasis = false;
		glm::vec3 m_Forward = { 0.0f, 0.0f, -1.0f };
		glm::vec3 m_Up = { 0.0f, 1.0f, 0.0f };
		glm::vec3 m_Right = { 1.0f, 0.0f, 0.0f };
	};

}
