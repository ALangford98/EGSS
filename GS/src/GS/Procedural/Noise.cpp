#include "gspch.h"
#include "GS/Procedural/Noise.h"

namespace GS::Noise {

	namespace {
		// Wraps into [0, period) for negative inputs too -- `%` alone would
		// leave -1 at -1, and the cell left of the origin would hash
		// differently from the cell one period to its right.
		inline int WrapInt(int value, int period)
		{
			int r = value % period;
			return r < 0 ? r + period : r;
		}

		inline float Fade(float t)
		{
			return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
		}

		// 256 unit vectors evenly round the circle. Enough directions that no
		// axis bias shows through a gradient map (a set of 4 or 8 does), and
		// a table rather than cos/sin of a hashed angle: that was 24 trig
		// calls per 6-octave sample and most of the Noise node's 94 ms.
		struct GradientTable
		{
			glm::vec2 Entries[256];
			GradientTable()
			{
				for (int i = 0; i < 256; i++)
				{
					float angle = (float)i * (6.28318530718f / 256.0f);
					Entries[i] = glm::vec2(std::cos(angle), std::sin(angle));
				}
			}
		};

		inline const glm::vec2& Gradient(int x, int y, uint32_t seed)
		{
			static const GradientTable table;
			return table.Entries[Hash(x, y, seed) >> 24];
		}
	}

	uint32_t Hash(uint32_t x)
	{
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}

	uint32_t Hash(int x, int y, uint32_t seed)
	{
		// Chained rather than combined arithmetically, so (x, y) and (y, x)
		// -- or any other pair with the same sum -- land on unrelated values.
		return Hash((uint32_t)x ^ Hash((uint32_t)y ^ Hash(seed)));
	}

	float HashToUnit(uint32_t h)
	{
		return (float)(h >> 8) * (1.0f / 16777216.0f);
	}

	float Perlin(glm::vec2 p, glm::ivec2 period, uint32_t seed)
	{
		glm::vec2 cell = glm::floor(p);
		glm::vec2 f = p - cell;
		int x0 = (int)cell.x, y0 = (int)cell.y;

		int wx0 = WrapInt(x0, period.x), wx1 = WrapInt(x0 + 1, period.x);
		int wy0 = WrapInt(y0, period.y), wy1 = WrapInt(y0 + 1, period.y);

		float n00 = glm::dot(Gradient(wx0, wy0, seed), f);
		float n10 = glm::dot(Gradient(wx1, wy0, seed), f - glm::vec2(1.0f, 0.0f));
		float n01 = glm::dot(Gradient(wx0, wy1, seed), f - glm::vec2(0.0f, 1.0f));
		float n11 = glm::dot(Gradient(wx1, wy1, seed), f - glm::vec2(1.0f, 1.0f));

		float u = Fade(f.x), v = Fade(f.y);
		return glm::mix(glm::mix(n00, n10, u), glm::mix(n01, n11, u), v);
	}

	float Fbm(glm::vec2 p, glm::ivec2 period, int octaves, int lacunarity,
		float gain, uint32_t seed)
	{
		float sum = 0.0f, amplitude = 1.0f;
		int scale = 1;
		for (int i = 0; i < octaves; i++)
		{
			sum += amplitude * Perlin(p * (float)scale, period * scale, seed + (uint32_t)i);
			amplitude *= gain;
			scale *= lacunarity;
		}
		return sum;
	}

	VoronoiResult Voronoi(glm::vec2 p, glm::ivec2 period, float jitter, uint32_t seed)
	{
		glm::vec2 cell = glm::floor(p);
		int cx = (int)cell.x, cy = (int)cell.y;

		// Squared distances in the search, one sqrt each for F1 and F2 at
		// the end -- not 25.
		VoronoiResult result;
		float f1 = 1e30f, f2 = 1e30f;

		// 5x5, not 3x3. With jitter up to 1 the nearest point is always within
		// the 3x3 block, but the *second* nearest can sit one ring further out
		// -- a 3x3 search then returns a wrong F2, and F2 - F1 (the crack
		// pattern) shows faint straight seams along cell boundaries.
		for (int dy = -2; dy <= 2; dy++)
			for (int dx = -2; dx <= 2; dx++)
			{
				int x = cx + dx, y = cy + dy;
				int wx = WrapInt(x, period.x), wy = WrapInt(y, period.y);
				uint32_t h = Hash(wx, wy, seed);
				glm::vec2 offset(HashToUnit(h) - 0.5f, HashToUnit(Hash(h)) - 0.5f);
				// The point is placed relative to the *unwrapped* cell, so its
				// distance to p is real; only its hash comes from the wrapped one.
				glm::vec2 point = glm::vec2((float)x, (float)y) + 0.5f + offset * jitter;
				glm::vec2 delta = point - p;
				float d = glm::dot(delta, delta);
				if (d < f1)
				{
					f2 = f1;
					f1 = d;
					result.CellId = h;
				}
				else if (d < f2)
				{
					f2 = d;
				}
			}
		result.F1 = std::sqrt(f1);
		result.F2 = std::sqrt(f2);
		return result;
	}

}
