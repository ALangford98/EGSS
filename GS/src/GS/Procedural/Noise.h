#pragma once

#include "GS/Core.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace GS::Noise {

	// Everything here tiles, because the material graph's whole output must:
	// a texture that repeats across a floor shows its seam at every repeat if
	// any node in the chain did not. Tiling is done by wrapping the *integer
	// lattice* coordinates by `period` before anything is hashed, so the value
	// one whole period away is not approximately equal -- it is computed from
	// the very same corner hashes, and is bit-identical.
	//
	// Nothing here uses rand() or a clock: the same inputs give the same
	// output on every run, which is what lets an exported PNG be hash-compared.

	// Integer avalanche hash (the "lowbias32" constants). Every input bit
	// flips about half the output bits, so neighbouring lattice corners get
	// unrelated gradients.
	GS_API uint32_t Hash(uint32_t x);
	GS_API uint32_t Hash(int x, int y, uint32_t seed);

	// [0, 1), from the top 24 bits -- exactly what a float's mantissa holds.
	GS_API float HashToUnit(uint32_t h);

	// Gradient (Perlin) noise. `p` is in lattice units, `period` in whole
	// lattice cells (>= 1 per axis). Unit-length random gradients and the
	// quintic fade 6t^5 - 15t^4 + 10t^3, whose first and second derivatives
	// vanish at the cell edge, so neither the value nor its slope creases
	// there. Exactly 0 at every lattice point; bounded by sqrt(2)/2.
	GS_API float Perlin(glm::vec2 p, glm::ivec2 period, uint32_t seed);

	// Fractional Brownian motion: sum over i < octaves of
	// gain^i * Perlin(p * lac^i, period * lac^i, seed + i). The lacunarity is
	// an integer so every octave's period is a whole multiple of the first,
	// which keeps every octave -- and so the sum -- tiling. Unnormalised: the
	// caller divides by sum gain^i if it wants a fixed range.
	GS_API float Fbm(glm::vec2 p, glm::ivec2 period, int octaves, int lacunarity,
		float gain, uint32_t seed);

	struct VoronoiResult
	{
		float F1 = 0.0f;       // distance to the nearest feature point
		float F2 = 0.0f;       // distance to the second nearest
		uint32_t CellId = 0;   // hash of the nearest point's (wrapped) cell
	};

	// Cellular noise: one feature point per lattice cell, at the cell centre
	// plus (hash - 0.5) * jitter per axis. Jitter 0 is a regular grid, jitter
	// 1 lets the point be anywhere in its cell. Distances are in lattice units.
	GS_API VoronoiResult Voronoi(glm::vec2 p, glm::ivec2 period, float jitter, uint32_t seed);

}
