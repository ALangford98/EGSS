#include "egsspch.h"
#include "Egss/Physics/Sat2D.h"

namespace Egss {

	void ObbBox2D::Corners(glm::vec2 out[4]) const
	{
		glm::vec2 x = AxisX() * HalfExtents.x;
		glm::vec2 y = AxisY() * HalfExtents.y;

		out[0] = Centre - x - y;
		out[1] = Centre + x - y;
		out[2] = Centre + x + y;
		out[3] = Centre - x + y;
	}

	float Sat2D::ProjectedRadius(const ObbBox2D& box, const glm::vec2& axis)
	{
		// The absolute values are the whole trick. A box reaches the same
		// distance along an axis and against it, so the sign of each dot
		// product is irrelevant -- and keeping the signs would let the two
		// terms cancel and report a box with no width.
		return box.HalfExtents.x * std::fabs(glm::dot(box.AxisX(), axis))
			+ box.HalfExtents.y * std::fabs(glm::dot(box.AxisY(), axis));
	}

	namespace {

		// The edge of `box` most nearly facing `normal`: the one whose outward
		// normal is closest to it. Returns its two endpoints.
		void IncidentEdge(const ObbBox2D& box, const glm::vec2& normal,
			glm::vec2& start, glm::vec2& end)
		{
			glm::vec2 corners[4];
			box.Corners(corners);

			// Outward normal of edge i, which runs from corner i to i+1.
			const glm::vec2 axes[4] = {
				-box.AxisY(), box.AxisX(), box.AxisY(), -box.AxisX()
			};

			int best = 0;
			float bestDot = glm::dot(axes[0], normal);
			for (int i = 1; i < 4; i++)
			{
				float d = glm::dot(axes[i], normal);
				if (d > bestDot)
				{
					bestDot = d;
					best = i;
				}
			}

			start = corners[best];
			end = corners[(best + 1) % 4];
		}

		// Clips a segment against a half-plane: keeps the part of it on the
		// side where dot(point, normal) <= limit. Returns how many endpoints
		// survived, which is 0, 1 or 2 -- and when it is 1 the segment crossed
		// the plane, so the crossing point replaces the endpoint that left.
		int ClipSegment(glm::vec2& start, glm::vec2& end,
			const glm::vec2& normal, float limit)
		{
			float dStart = glm::dot(start, normal) - limit;
			float dEnd = glm::dot(end, normal) - limit;

			glm::vec2 keptStart = start, keptEnd = end;
			int kept = 0;

			if (dStart <= 0.0f) { keptStart = start; kept++; }
			if (dEnd <= 0.0f)
			{
				if (kept == 0) keptStart = end;
				else keptEnd = end;
				kept++;
			}

			// One in, one out: find where the segment crosses and keep that.
			if (kept == 1 && (dStart > 0.0f) != (dEnd > 0.0f))
			{
				float t = dStart / (dStart - dEnd);
				keptEnd = start + (end - start) * t;
				kept = 2;
			}

			start = keptStart;
			end = keptEnd;
			return kept;
		}

	}

	Manifold2D Sat2D::BoxBox(const ObbBox2D& a, const ObbBox2D& b)
	{
		Manifold2D manifold;

		// Only four candidate axes, because a box's shadow can only be narrowed
		// by one of its own faces. A general convex pair would need every edge
		// normal of both.
		const glm::vec2 axes[4] = { a.AxisX(), a.AxisY(), b.AxisX(), b.AxisY() };

		glm::vec2 between = b.Centre - a.Centre;

		float leastOverlap = std::numeric_limits<float>::max();
		glm::vec2 bestAxis(0.0f);
		bool referenceIsA = true;

		for (int i = 0; i < 4; i++)
		{
			glm::vec2 axis = axes[i];

			float overlap = ProjectedRadius(a, axis) + ProjectedRadius(b, axis)
				- std::fabs(glm::dot(between, axis));

			// A gap on any axis is proof they are apart, and there is nothing
			// left to work out.
			if (overlap <= 0.0f)
				return manifold;

			if (overlap < leastOverlap)
			{
				leastOverlap = overlap;
				// Orient it from A towards B, so the caller never has to work
				// out which way "apart" is.
				bestAxis = (glm::dot(between, axis) < 0.0f) ? -axis : axis;
				referenceIsA = (i < 2);
			}
		}

		manifold.Touching = true;
		manifold.Normal = bestAxis;
		manifold.Depth = leastOverlap;

		// The face that won owns the contact; the other box supplies the edge
		// being pressed into it.
		const ObbBox2D& reference = referenceIsA ? a : b;
		const ObbBox2D& incident = referenceIsA ? b : a;

		// The normal points A -> B, so from the incident box's point of view the
		// pressure comes the other way.
		glm::vec2 intoIncident = referenceIsA ? bestAxis : -bestAxis;

		glm::vec2 start, end;
		IncidentEdge(incident, -intoIncident, start, end);

		// Trim that edge to the width of the reference face, so a corner
		// hanging off the end does not become a contact point that is not
		// actually over anything.
		glm::vec2 side = { -intoIncident.y, intoIncident.x };
		float centreAlongSide = glm::dot(reference.Centre, side);
		float halfWidth = ProjectedRadius(reference, side);

		if (ClipSegment(start, end, side, centreAlongSide + halfWidth) == 0)
			return manifold;
		if (ClipSegment(start, end, -side, -(centreAlongSide - halfWidth)) == 0)
			return manifold;

		// Keep only the points actually inside the reference face. A clipped
		// endpoint can sit outside it when the boxes meet at a shallow angle,
		// and a contact point that is not penetrating would be solved as if it
		// were.
		float faceOffset = glm::dot(reference.Centre, intoIncident)
			+ ProjectedRadius(reference, intoIncident);

		const glm::vec2 candidates[2] = { start, end };
		for (const glm::vec2& point : candidates)
		{
			float depth = faceOffset - glm::dot(point, intoIncident);
			if (depth < 0.0f)
				continue;

			manifold.Points[manifold.PointCount] = point;
			manifold.Depths[manifold.PointCount] = depth;
			manifold.PointCount++;
		}

		// Overlapping on every axis but with no point surviving the clip means
		// the two touch exactly at a corner, where the deepest point is the
		// corner itself. Report the axis result rather than nothing -- a caller
		// told "touching, no points" cannot do anything with it.
		if (manifold.PointCount == 0)
		{
			manifold.Points[0] = start;
			manifold.Depths[0] = leastOverlap;
			manifold.PointCount = 1;
		}

		return manifold;
	}

}
