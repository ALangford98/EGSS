#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>

namespace Egss {

	// An oriented box: a centre, half extents along its *own* axes, and how far
	// those axes are turned from the world's.
	//
	// Deliberately not `RigidBody2D`. This file is geometry and nothing else --
	// no mass, no velocity, no solver -- which is what lets it be checked
	// against hand-worked answers with no simulation running, and what will let
	// the solver be checked separately against it later.
	struct EGSS_API ObbBox2D
	{
		glm::vec2 Centre = { 0.0f, 0.0f };
		glm::vec2 HalfExtents = { 0.5f, 0.5f };
		float Rotation = 0.0f;   // radians

		// The box's own x and y axes in world space. y is x turned a quarter
		// turn, so one sin/cos pair does both.
		glm::vec2 AxisX() const { return { std::cos(Rotation), std::sin(Rotation) }; }
		glm::vec2 AxisY() const { return { -std::sin(Rotation), std::cos(Rotation) }; }

		// The four corners, counter-clockwise from (-x, -y) in local space.
		void Corners(glm::vec2 out[4]) const;
	};

	// Where two shapes touch, and how much they overlap there.
	//
	// Up to two points, which is the number a 2D box-box contact needs: one is
	// enough to stop penetration but not to stop a resting box from rocking,
	// because a single point can only ever resist rotation about itself. Face
	// contacts give two, corner contacts give one.
	struct EGSS_API Manifold2D
	{
		bool Touching = false;

		// Points from A towards B, so pushing B along it separates them.
		glm::vec2 Normal = { 0.0f, 0.0f };
		// How deep the overlap is along that normal.
		float Depth = 0.0f;

		int PointCount = 0;
		glm::vec2 Points[2] = {};
		// Per point, since clipping can leave one corner deeper than the other
		// on a box resting at a slight angle.
		float Depths[2] = { 0.0f, 0.0f };
	};

	// The separating axis test for oriented boxes.
	//
	// The idea in one line: two convex shapes are apart if and only if some
	// direction exists along which their shadows do not overlap. For boxes only
	// four directions can be that axis -- the two face normals of each -- so the
	// whole test is four projections, and the *smallest* overlap among them is
	// the shortest way out.
	//
	// Nothing here is specific to rigid bodies, and nothing here moves
	// anything. Turning a manifold into an impulse is the solver's job, and is
	// deliberately not yet written: this half can be wrong in ways that are
	// obvious against arithmetic and invisible once a solver is smearing them
	// across sixteen iterations.
	class EGSS_API Sat2D
	{
	public:
		static Manifold2D BoxBox(const ObbBox2D& a, const ObbBox2D& b);

		// How far a box reaches along `axis`, measured from its centre. The
		// support function every SAT projection is built from.
		static float ProjectedRadius(const ObbBox2D& box, const glm::vec2& axis);
	};

}
