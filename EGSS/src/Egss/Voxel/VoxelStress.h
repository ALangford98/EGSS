#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Voxel/VoxelField3D.h"

#include <glm/glm.hpp>

namespace Egss {

	// Whether a piece of rock is strong enough to hold up what is above it.
	//
	// `VoxelIslands` answers connectivity: attached, or not. That is what
	// Teardown does, and it has a famous consequence -- a single voxel can hold
	// up a tower, because the tower is *connected*. This answers the other half:
	// a connection carries a load, a load produces a stress, and past a limit the
	// connection is not there any more.
	//
	// **The model, and why this one.** A full static solve -- every block's
	// forces summed to zero, as the Gustave library does -- is the honest
	// version and costs a linear system per edit. A single force per part, as
	// Red Faction used, is cheap and cannot tell a long overhang from a short
	// one. What is here sits between them and costs one pass:
	//
	//   1. Every solid voxel has a weight. Load is routed to the anchors along
	//      the connections, splitting evenly wherever there is a choice, so a
	//      wide neck shares what a narrow one carries alone.
	//
	//   2. Each connection also carries the **centre of mass** of everything
	//      routed through it, which is what makes length matter. Weight alone
	//      cannot tell a ledge sticking out ten metres from the same rock piled
	//      against the cliff.
	//
	//   3. Connections lying in the same plane are grouped into a **section**,
	//      and the section is judged as a whole -- direct stress from the load it
	//      carries, plus bending stress from the moment of that load about the
	//      section's own centroid.
	//
	// Step 3 is the one that makes it agree with beam theory. Judging each
	// connection on its own would have every face resist its own moment
	// independently, which makes a wide neck no stiffer in bending than a narrow
	// one -- badly wrong, and wrong in the direction that looks fine until
	// something wide snaps. Grouped, the arithmetic comes out as
	//
	//     sigma = W/A + M*r/I
	//
	// and for a uniform cantilever of thickness h that is `3*rho*g*L^2/h`, the
	// textbook root stress, which is the check this is measured against.
	//
	// **What it does not model**: sections are grouped by axis-aligned plane,
	// because a voxel grid's faces are axis-aligned; a neck that fails along a
	// diagonal is not seen. Nothing is dynamic -- no impulse from a falling
	// piece, no fatigue, no resonance. And compression and tension share one
	// limit, where real rock is roughly ten times stronger in compression.
	struct EGSS_API VoxelStressSettings
	{
		float Density = 1400.0f;      // kg per cubic metre; rock, near enough
		float Gravity = 9.81f;

		// The stress a section fails above, in pascals. Real sandstone is a few
		// megapascals in tension; this is deliberately far lower, because a
		// game wants rock that breaks when you undermine it rather than rock
		// that is correct.
		float Strength = 120000.0f;

		// Lattice y at or below which voxels are held up by the world.
		int AnchorHeight = 0;

		// Sections smaller than this are ignored entirely. **This used to be
		// load-bearing and is not any more**: it was 4, to hide lone connections
		// whose bending stress came out enormous because a single face has
		// almost no second moment. That was a symptom of attributing a moment to
		// a section that was only one of several parallel paths, and the cut test
		// in `Overloaded` fixes the cause -- so this is back to 1, meaning a
		// genuine single-voxel neck is judged on its merits again.
		int MinSectionLinks = 1;
	};

	// A cross-section, and how hard it is being pulled.
	struct EGSS_API VoxelSection
	{
		// The connections making up the section: for each, the voxel on the far
		// side of the plane -- the side whose weight is being carried.
		std::vector<glm::ivec3> Far;

		// Which axis the section's plane is perpendicular to, 0/1/2 for x/y/z.
		int Axis = 1;

		float Load = 0.0f;         // newtons carried through the section
		float Area = 0.0f;         // square metres of connection
		float Moment = 0.0f;       // newton metres about the section's centroid

		float DirectStress = 0.0f;
		float BendingStress = 0.0f;
		float Stress = 0.0f;       // the sum, and what is compared to Strength

		glm::vec3 Centroid = glm::vec3(0.0f);
	};

	class EGSS_API VoxelStress
	{
	public:
		// Every section whose stress exceeds the strength, worst first.
		static std::vector<VoxelSection> Overloaded(const VoxelField3D& field,
			const VoxelStressSettings& settings = VoxelStressSettings());

		// Breaks the worst overloaded section, if there is one, by carving the
		// layer of voxels on its far side. Returns true if anything broke.
		//
		// **A break costs a voxel layer.** The field stores material, not
		// connections, so the only way to separate two lumps is to remove the
		// rock between them. At half-metre voxels that is a half-metre seam,
		// which reads as the rock crumbling where it tore -- and is why this is
		// worth knowing rather than hiding.
		//
		// One section per call on purpose: breaking the worst one changes what
		// every other section carries, so the caller should re-run this until it
		// returns false (with a cap -- a structure can cascade a long way).
		static bool BreakWorst(VoxelField3D& field,
			const VoxelStressSettings& settings = VoxelStressSettings());

		// Breaks up to `maxSections` overloaded sections from **one** analysis,
		// worst first, skipping any that share a voxel with one already broken.
		// Returns how many gave way.
		//
		// `BreakWorst` in a loop is the same thing done honestly and costs an
		// analysis per break -- and an analysis is a pass over every solid voxel
		// in the field, which is 21 ms on the demo's map. Breaking six of them
		// that way is an eighth of a second for one dig.
		//
		// The approximation is that the sections are judged against a structure
		// none of them has broken yet, so a set that would have relieved each
		// other all fail together. Skipping overlaps keeps that from removing the
		// same rock twice; the caller should still loop, because the *next*
		// analysis is what sees the redistributed load and drives the cascade.
		static int Relieve(VoxelField3D& field,
			const VoxelStressSettings& settings = VoxelStressSettings(),
			int maxSections = 8);

		// The stress a uniform cantilever's root should be under, from beam
		// theory: `3 * rho * g * L^2 / h`, for length L and thickness h.
		//
		// Here so a caller can check the model against it. **The model does not
		// use this** -- a formula the implementation contains is not a check on
		// the implementation.
		static float CantileverRootStress(float length, float thickness,
			float density, float gravity = 9.81f)
		{
			return thickness > 0.0f
				? 3.0f * density * gravity * length * length / thickness
				: 0.0f;
		}
	};

}
