#include "egsspch.h"
#include "Egss/Physics/Sat3D.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

namespace Egss {

	void Obb3D::Corners(glm::vec3 out[8]) const
	{
		glm::vec3 x = Axis(0) * HalfExtents.x;
		glm::vec3 y = Axis(1) * HalfExtents.y;
		glm::vec3 z = Axis(2) * HalfExtents.z;

		out[0] = Centre - x - y - z;
		out[1] = Centre + x - y - z;
		out[2] = Centre + x + y - z;
		out[3] = Centre - x + y - z;
		out[4] = Centre - x - y + z;
		out[5] = Centre + x - y + z;
		out[6] = Centre + x + y + z;
		out[7] = Centre - x + y + z;
	}

	float Sat3D::ProjectedRadius(const Obb3D& box, const glm::vec3& axis)
	{
		// The absolute values are the whole trick, exactly as in 2D: a box
		// reaches the same distance along an axis and against it, so the sign
		// of each dot product is irrelevant -- and keeping the signs would let
		// the terms cancel and report a box with no width.
		return box.HalfExtents.x * std::fabs(glm::dot(box.Axis(0), axis))
			+ box.HalfExtents.y * std::fabs(glm::dot(box.Axis(1), axis))
			+ box.HalfExtents.z * std::fabs(glm::dot(box.Axis(2), axis));
	}

	bool Sat3D::Contains(const Obb3D& box, const glm::vec3& point, float tolerance)
	{
		glm::vec3 offset = point - box.Centre;

		for (int i = 0; i < 3; i++)
		{
			if (std::fabs(glm::dot(offset, box.Axis(i))) > box.HalfExtents[i] + tolerance)
				return false;
		}

		return true;
	}

	namespace {

		// An edge-edge axis has to beat a face axis by more than this before it
		// is believed.
		//
		// Two boxes resting face to face have edge-edge axes that are very
		// nearly as good as the face axis, and floating point regularly hands
		// one of them a marginally smaller overlap. Taking it produces a
		// contact normal skewed a few degrees off the surface, which a solver
		// turns into a resting box that slowly slides. Preferring faces on a
		// near-tie is the standard fix and it is a tolerance, not a fudge:
		// when the answer really is edge-edge it wins by a wide margin.
		const float s_EdgeAxisBias = 1.005f;

		// Below this, a cross product of two nearly parallel axes is not a
		// direction at all -- normalising it amplifies pure rounding.
		const float s_ParallelEpsilon = 1e-6f;

		struct Candidate
		{
			glm::vec3 Axis;
			// The real overlap, which is the penetration depth, and the
			// biased figure used only to decide which axis wins. Keeping them
			// apart matters: reporting the biased one would make every
			// edge-edge contact half a percent deeper than it is, and the
			// position solver would push the pair apart by that much too far,
			// every step, forever.
			float Overlap;
			float Comparable;
			bool IsEdgeAxis;
			int FaceOwner;   // 0 for A's face, 1 for B's, -1 for edge-edge
		};

		// The face of `box` most nearly facing `normal`, as its four corners in
		// winding order, plus the face's outward normal.
		void MostFacingFace(const Obb3D& box, const glm::vec3& normal,
			glm::vec3 out[4], glm::vec3& outNormal)
		{
			int bestAxis = 0;
			float bestDot = 0.0f;
			float sign = 1.0f;

			for (int i = 0; i < 3; i++)
			{
				float d = glm::dot(box.Axis(i), normal);
				if (std::fabs(d) > std::fabs(bestDot))
				{
					bestDot = d;
					bestAxis = i;
				}
			}

			sign = bestDot < 0.0f ? -1.0f : 1.0f;

			// The two axes that are not the face normal span the face.
			int u = (bestAxis + 1) % 3;
			int v = (bestAxis + 2) % 3;

			glm::vec3 centre = box.Centre + box.Axis(bestAxis) * (box.HalfExtents[bestAxis] * sign);
			glm::vec3 uEdge = box.Axis(u) * box.HalfExtents[u];

			// Wound counter-clockwise about the face's *outward* normal, which
			// is why `sign` appears here as well as in `centre`.
			//
			// Without it the corners come out in the same (u, v) order whichever
			// way the face points, so a face along a negative axis is wound
			// backwards relative to its own normal. The caller builds its clip
			// planes as cross(edge, outwardNormal), so every one of them then
			// points inward and Sutherland-Hodgman keeps the *outside* of the
			// reference face: the polygon clips to nothing, and the "they must
			// meet at a corner" fallback fabricates a single point.
			//
			// A stack hid this. Body A is the lower box and its +y face wins the
			// tie, so the four-point manifold is correct -- until a hair of tilt
			// lets the upper box's axis win instead, the reference becomes a
			// *bottom* face, and the manifold silently drops to one point. One
			// point cannot hold a box level, so it tips, and which box wins that
			// near-tie depends on rounding: four boxes stood or fell chaotically
			// with the iteration counts and no trend.
			glm::vec3 vEdge = box.Axis(v) * (box.HalfExtents[v] * sign);

			out[0] = centre - uEdge - vEdge;
			out[1] = centre + uEdge - vEdge;
			out[2] = centre + uEdge + vEdge;
			out[3] = centre - uEdge + vEdge;

			outNormal = box.Axis(bestAxis) * sign;
		}

		// Sutherland-Hodgman: clip a polygon against the half-space
		// dot(point, normal) <= limit.
		void ClipToPlane(std::vector<glm::vec3>& polygon, const glm::vec3& normal, float limit)
		{
			if (polygon.empty())
				return;

			std::vector<glm::vec3> result;
			result.reserve(polygon.size() + 4);

			for (size_t i = 0; i < polygon.size(); i++)
			{
				const glm::vec3& current = polygon[i];
				const glm::vec3& next = polygon[(i + 1) % polygon.size()];

				float distanceCurrent = glm::dot(current, normal) - limit;
				float distanceNext = glm::dot(next, normal) - limit;

				if (distanceCurrent <= 0.0f)
					result.push_back(current);

				// Crossing the plane: keep where it crosses. The sign test is
				// on the product rather than on each side, so a vertex exactly
				// on the plane does not generate a duplicate.
				if ((distanceCurrent > 0.0f) != (distanceNext > 0.0f))
				{
					float t = distanceCurrent / (distanceCurrent - distanceNext);
					result.push_back(current + (next - current) * t);
				}
			}

			polygon.swap(result);
		}

	}

	Manifold3D Sat3D::BoxBox(const Obb3D& a, const Obb3D& b)
	{
		Manifold3D manifold;

		glm::vec3 between = b.Centre - a.Centre;

		Candidate best;
		best.Overlap = std::numeric_limits<float>::max();
		best.Comparable = std::numeric_limits<float>::max();
		best.IsEdgeAxis = false;
		best.FaceOwner = -1;

		// One projection, kept in a lambda so the fifteen axes cannot drift
		// apart in how they are tested. Returns false the moment a gap is
		// found, which is proof the boxes are apart and ends the whole test.
		auto consider = [&](const glm::vec3& axis, bool isEdgeAxis, int faceOwner)
		{
			float overlap = ProjectedRadius(a, axis) + ProjectedRadius(b, axis)
				- std::fabs(glm::dot(between, axis));

			if (overlap <= 0.0f)
				return false;

			// Edge axes must clear the bias before displacing a face axis.
			float comparable = isEdgeAxis ? overlap * s_EdgeAxisBias : overlap;

			if (comparable < best.Comparable)
			{
				best.Comparable = comparable;
				best.Overlap = overlap;
				best.Axis = axis;
				best.IsEdgeAxis = isEdgeAxis;
				best.FaceOwner = faceOwner;
			}

			return true;
		};

		for (int i = 0; i < 3; i++)
		{
			if (!consider(a.Axis(i), false, 0))
				return manifold;
		}

		for (int i = 0; i < 3; i++)
		{
			if (!consider(b.Axis(i), false, 1))
				return manifold;
		}

		for (int i = 0; i < 3; i++)
		{
			for (int j = 0; j < 3; j++)
			{
				glm::vec3 cross = glm::cross(a.Axis(i), b.Axis(j));
				float lengthSquared = glm::dot(cross, cross);

				// Parallel axes give a zero-length cross, which is not a
				// direction. Testing it anyway is the classic way to break
				// this test: both shadows come out zero-width, the overlap
				// reads as zero, and two boxes plainly inside each other are
				// declared apart. Two axis-aligned boxes hit this on all nine.
				if (lengthSquared < s_ParallelEpsilon)
					continue;

				if (!consider(cross / std::sqrt(lengthSquared), true, -1))
					return manifold;
			}
		}

		// Nothing separated them, so they overlap.
		manifold.Touching = true;
		manifold.Depth = best.Overlap;

		// Orient from A towards B, so the caller never has to work out which
		// way "apart" is.
		manifold.Normal = glm::dot(between, best.Axis) < 0.0f ? -best.Axis : best.Axis;

		if (best.IsEdgeAxis)
		{
			// An edge-edge contact is a single point: the two edges cross at
			// one place. Approximated here as the midpoint of the overlap
			// along the normal, which is where the crossing sits to within the
			// penetration depth -- and the depth is small whenever this case
			// is the right answer.
			manifold.From = Manifold3D::Feature::EdgeEdge;

			glm::vec3 surfaceA = a.Centre + manifold.Normal * ProjectedRadius(a, manifold.Normal);
			glm::vec3 surfaceB = b.Centre - manifold.Normal * ProjectedRadius(b, manifold.Normal);

			manifold.Points[0] = (surfaceA + surfaceB) * 0.5f;
			manifold.Depths[0] = manifold.Depth;
			manifold.PointCount = 1;
			return manifold;
		}

		// --- Face contact ---------------------------------------------------
		// The face that won owns the contact; the other box supplies the face
		// pressed into it, which is clipped to the reference face's outline.
		bool referenceIsA = best.FaceOwner == 0;
		manifold.From = referenceIsA ? Manifold3D::Feature::FaceA : Manifold3D::Feature::FaceB;

		const Obb3D& reference = referenceIsA ? a : b;
		const Obb3D& incident = referenceIsA ? b : a;

		// The normal points A -> B, so from the incident box's side the
		// pressure comes the other way.
		glm::vec3 intoIncident = referenceIsA ? manifold.Normal : -manifold.Normal;

		glm::vec3 referenceFace[4], incidentFace[4];
		glm::vec3 referenceNormal, incidentNormal;

		MostFacingFace(reference, intoIncident, referenceFace, referenceNormal);
		MostFacingFace(incident, -intoIncident, incidentFace, incidentNormal);

		std::vector<glm::vec3> polygon(incidentFace, incidentFace + 4);

		// Clip against the reference face's four side planes. Unlike 2D, where
		// a face is a segment with two ends, a face here is a rectangle with
		// four edges -- so this is the same idea run twice more.
		for (int i = 0; i < 4; i++)
		{
			glm::vec3 edge = referenceFace[(i + 1) % 4] - referenceFace[i];
			glm::vec3 planeNormal = glm::normalize(glm::cross(edge, referenceNormal));

			ClipToPlane(polygon, planeNormal, glm::dot(referenceFace[i], planeNormal));

			if (polygon.empty())
				break;
		}

		// Keep only what is actually behind the reference face. A clipped
		// vertex can sit outside it when the boxes meet at a shallow angle,
		// and a point that is not penetrating would be solved as though it
		// were -- which holds a body up in mid-air.
		float faceOffset = glm::dot(referenceFace[0], referenceNormal);

		// Points closer together than this are the same point.
		//
		// Clipping a square against a square of the same size puts every
		// corner exactly on two of the clip planes, and each plane emits it --
		// so a flat stack of identical boxes came back with eight contact
		// points that were four corners listed twice. Duplicates are not
		// harmless: each one is solved as its own constraint, so the corners
		// get twice the share of the impulse that the honest points do, and a
		// stack leans on whichever corner rounded first. An octagon is a real
		// answer when the faces are turned against each other; this is not.
		const float mergeDistance = 0.001f;

		for (const glm::vec3& point : polygon)
		{
			if (manifold.PointCount >= 8)
				break;

			float depth = faceOffset - glm::dot(point, referenceNormal);
			if (depth < 0.0f)
				continue;

			bool duplicate = false;
			for (int i = 0; i < manifold.PointCount; i++)
			{
				glm::vec3 offset = manifold.Points[i] - point;
				if (glm::dot(offset, offset) < mergeDistance * mergeDistance)
				{
					duplicate = true;
					break;
				}
			}

			if (duplicate)
				continue;

			manifold.Points[manifold.PointCount] = point;
			manifold.Depths[manifold.PointCount] = depth;
			manifold.PointCount++;
		}

		// --- Reduce to at most four points ----------------------------------
		//
		// A face contact between two boxes is fully constrained by four
		// points, and clipping regularly produces more that carry no extra
		// information. On two identical boxes stacked flat, a tilt of a
		// hundredth of a degree makes each edge cross its own clip plane near
		// the middle and emit a vertex there: eight points, four of them
		// meaningless, sitting on the boundary and moving about as the tilt
		// changes. Each is solved as its own constraint, so the patch is held
		// by points that are partly noise.
		//
		// Kept: the deepest point, the one furthest from it, then the two that
		// most enlarge the polygon spanned so far. That is the standard choice
		// and it keeps the *outline* -- holding a box level needs the corners
		// of its footprint, not the middle of its edges.
		if (manifold.PointCount > 4)
		{
			glm::vec3 points[8];
			float depths[8];
			int total = manifold.PointCount;

			std::memcpy(points, manifold.Points, sizeof(glm::vec3) * total);
			std::memcpy(depths, manifold.Depths, sizeof(float) * total);

			bool used[8] = {};
			int kept[4] = {};

			// 1. The deepest, since that is the one most needing solving.
			int deepest = 0;
			for (int i = 1; i < total; i++)
			{
				if (depths[i] > depths[deepest])
					deepest = i;
			}

			kept[0] = deepest;
			used[deepest] = true;

			// 2. The furthest from it.
			int furthest = -1;
			float bestDistance = -1.0f;
			for (int i = 0; i < total; i++)
			{
				if (used[i])
					continue;

				float distance = glm::length2(points[i] - points[kept[0]]);
				if (distance > bestDistance)
				{
					bestDistance = distance;
					furthest = i;
				}
			}

			kept[1] = furthest;
			used[furthest] = true;

			// 3 and 4. Whichever adds the most area to the polygon so far.
			for (int slot = 2; slot < 4; slot++)
			{
				int best = -1;
				float bestArea = -1.0f;

				for (int i = 0; i < total; i++)
				{
					if (used[i])
						continue;

					float area = 0.0f;
					for (int j = 0; j < slot; j++)
					{
						int next = (j + 1) % slot;
						if (slot == 2 && j == 1)
							break;

						area += glm::length(glm::cross(
							points[kept[next]] - points[kept[j]],
							points[i] - points[kept[j]]));
					}

					if (area > bestArea)
					{
						bestArea = area;
						best = i;
					}
				}

				if (best < 0)
					break;

				kept[slot] = best;
				used[best] = true;
			}

			for (int i = 0; i < 4; i++)
			{
				manifold.Points[i] = points[kept[i]];
				manifold.Depths[i] = depths[kept[i]];
			}

			manifold.PointCount = 4;
		}

		// Overlapping on every axis but with nothing surviving the clip means
		// the two meet at a corner. Report the axis result rather than
		// nothing: a caller told "touching, no points" cannot do anything.
		if (manifold.PointCount == 0)
		{
			glm::vec3 surfaceA = a.Centre + manifold.Normal * ProjectedRadius(a, manifold.Normal);
			glm::vec3 surfaceB = b.Centre - manifold.Normal * ProjectedRadius(b, manifold.Normal);

			manifold.Points[0] = (surfaceA + surfaceB) * 0.5f;
			manifold.Depths[0] = manifold.Depth;
			manifold.PointCount = 1;
		}

		return manifold;
	}

}
