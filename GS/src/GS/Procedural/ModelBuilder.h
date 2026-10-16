#pragma once

// Hard-surface models built in code: rounded boxes, shapes turned on a lathe,
// tubes swept along a path, extruded profiles -- the operations a crash-test
// dummy, a cannon and a laboratory are made of. Everything a model is lives
// in the function that builds it, readable, with nothing downloaded.
//
// A model is one MeshData with one submesh per material name: geometry is
// filed under whichever Material() was last named, and Build() lays the
// groups out one after another. What a material name *looks* like is the
// renderer's business (GS/Procedural/Prefabs.h has the palette), not the
// builder's.
//
// Every operation is placed by a transform stack, so a part is written in its
// own frame -- a bolt at the origin -- and put where it goes by Push().

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace GS {

	class GS_API ModelBuilder
	{
	public:
		ModelBuilder();

		// What the following geometry is made of.
		void Material(const std::string& name);

		// The transform stack: Push multiplies onto the current transform,
		// Pop restores the one before.
		void Push(const glm::mat4& transform);
		void Pop();
		static glm::mat4 At(const glm::vec3& position);
		static glm::mat4 At(const glm::vec3& position, const glm::vec3& eulerDegrees);

		// A box of half-extents `half` whose edges and corners are rounded
		// by `radius`, `segments` steps per quarter turn. Flat faces are
		// single quads; the rounding is where the vertices go.
		void RoundedBox(const glm::vec3& half, float radius, int segments = 3);

		// A profile of (radius, y) points, bottom to top, turned about the y
		// axis. Where the profile turns by more than `creaseDegrees` the
		// shading breaks -- a chamfer reads as a chamfer, a curve as a curve.
		// `sweepDegrees` short of 360 leaves it open.
		void Lathe(const std::vector<glm::vec2>& profile, int segments, float creaseDegrees = 35.0f,
			float sweepDegrees = 360.0f);

		// Along y, centred, closed both ends; `chamfer` bevels the rims.
		void Cylinder(float radius, float halfHeight, int segments, float chamfer = 0.0f);
		void Sphere(float radius, int segments = 20, int rings = 12);
		// In the xz plane about y.
		void Torus(float major, float minor, int majorSegments, int minorSegments);
		// A circle swept along a path, its frames carried along by parallel
		// transport so it does not twist; closed at the ends when `caps`.
		void Tube(const std::vector<glm::vec3>& path, float radius, int segments, bool caps = true);
		// A polygon (x, y, counter-clockwise), star-shaped about its centroid,
		// extruded from z = -depth/2 to +depth/2 and capped both ends.
		void Extrude(const std::vector<glm::vec2>& outline, float depth);
		// A flat pie slice in the xy plane facing +z, from angle `from` to
		// `to` in degrees -- quarters of a calibration target.
		void Sector(float radius, float fromDegrees, float toDegrees, int segments);
		// A flat rectangle in the xy plane facing +z, UVs 0..1 (v up).
		void Quad(const glm::vec2& half);

		MeshData Build() const;

	private:
		struct Group
		{
			std::string Name;
			std::vector<MeshVertex> Vertices;
			std::vector<unsigned int> Indices;
		};

		Group& Current();
		unsigned int Vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& uv);
		unsigned int Vertex(const glm::vec3& position, const glm::vec3& normal);   // box-projected UV
		void Triangle(unsigned int a, unsigned int b, unsigned int c);
		void QuadIndices(unsigned int a, unsigned int b, unsigned int c, unsigned int d);

		std::vector<Group> m_Groups;
		int m_Current = 0;
		std::vector<glm::mat4> m_Stack;
	};

}
