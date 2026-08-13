#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Scene/Scene.h"
#include "Egss/Audio/Acoustics.h"

#include <glm/glm.hpp>

namespace Egss {

	// One path from source to listener in three dimensions.
	struct EGSS_API ReflectionPath3D
	{
		float Delay = 0.0f;          // seconds after the direct sound
		float Gain = 0.0f;           // linear amplitude
		glm::vec3 Direction = { 0.0f, 0.0f, 0.0f };   // unit, arriving *at* the listener
		int Bounces = 0;
		float PathLength = 0.0f;
	};

	struct EGSS_API AcousticsResult3D : public AcousticsResultBase
	{
		// Early reflections, soonest first.
		std::vector<ReflectionPath3D> Reflections;
	};

	// One traced ray, for drawing. Not produced unless asked for.
	struct EGSS_API TracedRay3D
	{
		std::vector<glm::vec3> Points;   // source, then each bounce
		float FinalEnergy = 0.0f;
		bool Escaped = false;
	};

	// The same tracer as `Acoustics2D`, in three dimensions, sharing everything
	// that is not about the dimension (see `Acoustics.h`).
	//
	// **This traces the scene, not a physics world.** `Raycast3D` is what makes
	// it possible and `Raycast3D` casts against entities that have a mesh and a
	// transform, so the geometry the sound bounces off is the geometry you can
	// see. That is a better arrangement than it sounds: a room built for the eye
	// is a room the ear agrees with, and there is no second set of collision
	// boxes to keep in step.
	//
	// Two consequences of casting against meshes rather than colliders:
	//  - A surface is the mesh's **bounds**, not its triangles, and a rotated
	//    mesh is tested by transforming the ray into its space. So a sphere
	//    sounds like the cube around it. For rooms made of walls -- which is
	//    what a traced room is -- the bounds *are* the geometry.
	//  - `PerBodyAbsorption` and `PerBodyScattering` are indexed by `EntityId`
	//    here, not by a physics body handle. Same shape, different namespace;
	//    `InvalidEntity` is 0, so index 0 is never read.
	//
	// Build a room out of **wall slabs, not one big hollow box.** A ray starting
	// inside a box gets a hit at distance zero, because that is the only answer
	// `AgainstAabb` can honestly give -- so a source inside a single box room
	// never gets anywhere.
	//
	// What 3D fixes, and it is the reason this exists: a room's floor and
	// ceiling are half its reflecting area, and in 2D they do not exist. The
	// mean free path in 2D tends to pi * Area / Perimeter; in 3D it tends to
	// 4 * Volume / Surface. For a 12 x 4 x 8 m room that is 7.54 m against
	// 4.36 m, and the tracer lands on each to within half a percent. The shorter
	// path means more bounces per second, so more absorption per second, so a
	// shorter tail: measured on that room at absorption 0.2, the 2D footprint
	// gives RT60 1.397 s against 3D's 0.828 s -- **2D over-predicts by 1.69x**.
	//
	// Against Eyring the traced RT60 runs high, and by more as the room gets
	// deader: +2.8% at absorption 0.10, +5.1% at 0.20, +8.9% at 0.35. The sign
	// is expected and is not a defect in the tracer -- Eyring assumes a
	// perfectly mixed field, and a bounce here is heard by the listener
	// regardless of which way it was heading (the diffuse-rain approximation
	// `Acoustics2D` also documents), which finds a little more late energy than
	// a real specular room would deliver. Treat 10% as the accuracy of this
	// method rather than as a bug to chase.
	//
	// The approximations `Acoustics2D` lists all still apply: a bounce is
	// detected at the listener regardless of which way it was heading (diffuse
	// rain), scattering is per bounce rather than per band, and three bands is
	// not a curve.
	class EGSS_API Acoustics3D
	{
	public:
		static AcousticsResult3D Trace(const Scene& scene,
			const glm::vec3& source, const glm::vec3& listener,
			const AcousticsSettings& settings = AcousticsSettings(),
			std::vector<TracedRay3D>* debugRays = nullptr);

		// Mean free path for a convex room, from its volume and surface area:
		// the figure a traced `MeanFreePath` should converge on, and the whole
		// reason a 3D trace decays faster than a 2D one.
		//
		// Here so a caller can check the tracer against it. The tracer does not
		// use it -- a formula the implementation contains is not a check on the
		// implementation.
		static float MeanFreePathFor(float volume, float surfaceArea)
		{
			return surfaceArea > 0.0f ? 4.0f * volume / surfaceArea : 0.0f;
		}
	};

}
