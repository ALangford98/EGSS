#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Egss {

	// An oriented box in three dimensions: a centre, half extents along its
	// own axes, and how those axes are turned from the world's.
	//
	// Deliberately not `RigidBody3D`, for the same reason `ObbBox2D` is not
	// `RigidBody2D`: this file is geometry and nothing else -- no mass, no
	// velocity, no solver -- which is what lets it be checked against
	// hand-worked answers with no simulation running.
	struct EGSS_API Obb3D
	{
		glm::vec3 Centre = { 0.0f, 0.0f, 0.0f };
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
		glm::quat Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		// The box's own x, y and z axes in world space.
		glm::vec3 Axis(int index) const
		{
			glm::mat3 rotation = glm::mat3_cast(Orientation);
			return rotation[index];
		}

		// The eight corners, in the order (-x,-y,-z), (+x,-y,-z), (+x,+y,-z),
		// (-x,+y,-z) then the same four at +z.
		void Corners(glm::vec3 out[8]) const;
	};

	// Where two boxes touch, and how deeply.
	//
	// Up to eight points, which is what a face-on-face contact between two
	// boxes can produce once one rectangle is clipped against the other. Two
	// is enough in 2D because a face there is a line segment; in 3D it is a
	// polygon, and a box resting flat on another needs its whole footprint or
	// it rocks about whichever edge was kept.
	struct EGSS_API Manifold3D
	{
		bool Touching = false;

		// Points from A towards B, so pushing B along it separates them.
		glm::vec3 Normal = { 0.0f, 0.0f, 0.0f };
		float Depth = 0.0f;

		int PointCount = 0;
		glm::vec3 Points[8] = {};
		float Depths[8] = {};

		// Which kind of feature pair produced the contact. Worth knowing
		// because the two behave differently: a face contact carries a
		// polygon of points and can hold a body level, while an edge-edge
		// contact is a single point and cannot.
		enum class Feature
		{
			None = 0,
			FaceA,      // a face of A is the reference
			FaceB,
			EdgeEdge
		};

		Feature From = Feature::None;
	};

	// The separating axis test for oriented boxes in three dimensions.
	//
	// Two convex shapes are apart if and only if some direction exists along
	// which their shadows do not overlap. In 2D only four directions could be
	// that axis. In 3D there are **fifteen**: the three face normals of each
	// box, plus the nine cross products of one box's axis with the other's.
	//
	// Those nine are not an optimisation to be skipped. They are the only
	// axes that can separate two boxes crossing like the arms of an X, where
	// every face of each still overlaps every face of the other. Leave them
	// out and such a pair reports as touching, which reads as bodies sticking
	// to each other in mid-air.
	//
	// Nothing here is specific to rigid bodies and nothing here moves
	// anything. Turning a manifold into an impulse is the solver's job.
	class EGSS_API Sat3D
	{
	public:
		static Manifold3D BoxBox(const Obb3D& a, const Obb3D& b);

		// How far a box reaches along `axis`, measured from its centre. The
		// support function every projection is built from. `axis` should be
		// unit length for the result to be a distance.
		static float ProjectedRadius(const Obb3D& box, const glm::vec3& axis);

		// Whether a point is inside the box, within a tolerance. Used by the
		// tests, and cheap enough to be worth having.
		static bool Contains(const Obb3D& box, const glm::vec3& point, float tolerance = 0.0f);
	};

}
