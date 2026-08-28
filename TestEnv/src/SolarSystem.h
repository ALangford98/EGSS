#pragma once

// A solar system: a star, eight planets, and moons around four of them --
// **in one continuous space you can fly across.**
//
// **Nothing here contains an orbit.** The integrator knows one law --
// `a = -GM r / |r|^3` -- and everything else is a consequence: the paths are
// ellipses because an inverse-square force makes ellipses, and the periods come
// out matching Kepler's third law because they must. That is the whole reason
// to integrate gravity rather than to drive each planet around a circle with a
// sine and a cosine, which would look identical and prove nothing.
//
// **Units are astronomical, and chosen so the check is free.** Distance in AU,
// time in years, and `GM` for the Sun taken as `4*pi^2 AU^3/yr^2` -- which is
// not a fudge, it is what `GM` *is* in those units, since the Earth orbits at
// 1 AU in 1 year. A circular orbit at radius `a` then has period exactly
// `a^1.5` years, so the panel can put the measured period beside the predicted
// one and the difference is the integrator's error and nothing else.
//
// ---
//
// **The scale map, and why there is only one of it now.**
//
// The first version of this demo had two spaces and a key that teleported
// between them: an orbital view at 500 m to the AU where the Earth was 8.5 m
// across, and a surface view where it was 360 m across and made of voxels.
// That is the standard way out of the problem, and it has one fatal property --
// you cannot fly from one to the other, because they are not the same place.
//
// One space needs one rule, and the rule here is a single exponent:
//
//     drawn length = 360 m * (true length / Earth's radius) ^ p,  p = 1/2
//
// Applied to **everything** -- body radii and orbital distances alike -- with
// Earth's radius as the fixed point. So the Earth is 360 m across (which is
// what makes it a place you can stand on), its orbit is 55.2 km (not the
// 8,453 km true scale would demand), the Sun is 3.76 km across, and Neptune
// is 302 km out. The whole system is 302 km in radius, which at the speeds
// below is minutes across.
//
// Square root rather than a log or a hand-tuned table because it is one
// parameter, it is monotone (nothing overtakes anything), and it is *uniform*:
// every length obeys it, so no orbit has to be special-cased to keep its moon
// outside its planet. Check the tightest case in the system -- Phobos really
// does orbit at 2.77 Mars radii -- and it comes out at 1.66 drawn radii,
// still clear of the surface. The startup log prints that column for every
// body, because "does everything still fit" is the one thing this map can get
// wrong.
//
// What it costs is stated rather than hidden: true ratios are square-rooted,
// so Neptune is 8.8x further out than Mercury here where really it is 77.7x.
// The `p` slider goes to 1.0 if you want to see how empty it really is.
//
// **Frames, so a planet does not fly out from under you.** The ship's position
// is stored relative to *one body* -- whichever dominates where you are, by
// `distance / radius` -- and re-based when that changes. Near Earth you are in
// Earth's frame and its orbital motion is not your problem; out between the
// planets you are in the Sun's. The handover is exact: the offset is added to
// the position in the same step it is subtracted from the frame, so the scene
// position does not move by a millimetre. That invariant is measured on the
// first switch and logged.
//
// Moons orbit their planet and ignore the star. That is a simplification and
// a visible one -- a real moon's path around the Sun is a wavy line, never a
// loop -- but the alternative is a full N-body integration where the Sun's pull
// on the Moon is *twice* the Earth's, which needs a much smaller step to stay
// stable and buys nothing you can see at this scale.

#include <Egss.h>
#include <imgui.h>

#include <chrono>

#include "Demo.h"
#include "Vegetation.h"
#include "VoxelPlanet.h"
#include "SurfaceWater.h"
#include "HorizonMesh.h"
#include "PocketDimension.h"
#include "Climate.h"

// **Sampling a sphere map across the seam it necessarily has.**
//
// Every one of these shaders builds its texture coordinate with `atan`, so `u`
// runs 0..1 round the equator and wraps from 1 straight back to 0 along one
// meridian. The value is right on both sides of that line; the *derivative* is
// not. The hardware picks a mip level from the screen-space derivative of the
// coordinate, and across the wrap that derivative is a whole texture wide --
// so it selects the very top of the chain, where a texel is the average of the
// entire map.
//
// From orbit that drew a two-pixel bright blue line down the middle of the
// planet. At the top of the mip chain the wet mask averages to roughly the
// ocean fraction, so along the seam the shell decided there was sea and
// painted it over whatever land the meridian happened to cross.
//
// No sane sampling of a sphere moves half a texture in one pixel, so a
// derivative that claims to has wrapped, and subtracting the nearest whole
// turn recovers the true one. `textureGrad` is then given the gradient the
// surface actually has and picks the level it would have picked anywhere else
// along that meridian.
static const char* s_SphereSample = R"(
	vec4 SampleSphere(sampler2D map, vec2 uv)
	{
		vec2 dx = dFdx(uv);
		vec2 dy = dFdy(uv);

		dx.x -= round(dx.x);
		dy.x -= round(dy.x);

		return textureGrad(map, uv, dx, dy);
	}
)";

// `#version` has to be the first thing in a shader, so the shared function
// goes in immediately after it rather than at the front.
static std::string WithSphereSample(std::string source)
{
	const std::string version = "#version 330 core";

	size_t at = source.find(version);
	EGSS_CORE_ASSERT(at != std::string::npos, "shader has no #version line");

	return source.insert(at + version.size(), std::string("\n") + s_SphereSample);
}

class SolarSystem : public DemoLayer
{
public:
	SolarSystem()
		: DemoLayer("SolarSystem"), m_Camera(50.0f, 16.0f / 9.0f, 0.5f, 900000.0f)
	{
		RegisterParam("OrbitalYearsPerSecond", &m_OrbitalYearsPerSecond);
		RegisterParam("SecondsPerDay", &m_SecondsPerDay);

		// Reaches the simulation -- it decides how much ground a press
		// removes -- so a replay has to see it move.
		RegisterParam("DigRadius", &m_DigRadius);

		// Both reach the physics step, same reason.
		RegisterParam("BuoyancyStrength", &m_BuoyancyStrength);
		RegisterParam("WaterDrag", &m_WaterDrag);
	}

	// --- The system ---------------------------------------------------------

	struct BodyDescription
	{
		const char* Name;
		int Parent;             // -1 for the star, else an index into this table
		double SemiMajorAu;     // from its parent
		double RadiusKm;
		double MassSuns;        // in solar masses, so GM = GM_sun * this
		double RotationHours;   // sidereal day, always positive -- see AxialTiltDegrees

		// Real obliquity, in degrees, measured the IAU way: past 90 is a
		// retrograde spin rather than a negative period. Venus at 177.4 and
		// Uranus at 97.77 are the two that cross that line, and doing it this
		// way means SpinRate never needs a sign -- see the note on SpinAxis.
		// Zero for bodies with no meaningful pole to speak of (the Sun, whose
		// surface is never drawn) or whose real tilt is negligible (Mercury's
		// 0.034 rounds to it) or not yet modelled: every moon here is tidally
		// locked to an orbit that is still in the ecliptic rather than its
		// planet's equator, so a moon's own obliquity has nowhere correct to
		// point until that changes -- see the roadmap.
		float AxialTiltDegrees;
		glm::vec3 Colour;

		// Atmosphere: its depth as a fraction of the body's radius, a density
		// multiplier on top of the global one, and the colour it scatters.
		//
		// **The two multiply, and what matters is their product.** Optical
		// depth straight up is `0.25 * AirDensity * Fraction * Density` -- the
		// quarter is the scale height as a share of the shell. Earth's real
		// Rayleigh optical depth is about 0.1 and this uses 0.3, which is the
		// exaggeration that has always been here. Shrinking the shell from
		// 4.5% of the radius to its real 1.57% without dividing the density
		// out again took the horizontal path to twenty optical depths, and
		// the sky came out **brown**: at that thickness every horizon is a
		// sunset, because all the blue has been scattered out of it.
		// Zero depth means airless -- Mercury, the Moon, the small moons --
		// and those get no shell at all rather than a transparent one.
		//
		// **The gas giants' numbers are not measured atmosphere depths, and
		// should not be read as any.** Jupiter's visible air is about 1,000 km
		// over a 69,911 km radius, which is 1.4% -- no deeper in proportion
		// than Earth's. What is actually true about a gas giant is that it has
		// *no surface*: pressure rises until hydrogen stops being a gas, and
		// there is no altitude at which you land. This demo builds every body
		// out of voxels, so there is a surface down there whether it belongs
		// or not, and the honest way to draw a planet with no ground is air
		// deep enough and thick enough that you never reach it. Straight down
		// through Jupiter's shell is 5.9 optical depths, which leaves 0.3% of
		// whatever is underneath showing.
		float AtmosphereFraction;
		float AtmosphereDensity;

		// How much multiply-scattered light the air gives back. Zero for a
		// thin atmosphere, where single scattering is the whole story; one for
		// air deep enough that light bounces around inside it before leaving,
		// which is every gas giant and Venus. See the note in the fragment
		// shader for what it is standing in for.
		float AtmosphereGlow;
		glm::vec3 Scatter;

		// Rings: inner and outer edge in units of the body's own radius, and
		// the colour of the material. Zero inner means no rings. Saturn's
		// 1.24 to 2.27 is the C ring's inner edge to the A ring's outer one,
		// which is the span you can actually see; Uranus's are narrow, dark
		// and were not found until 1977 for that reason.
		//
		// **The plane is the body's own equator, from `AxialTiltDegrees`, not
		// a tilt of their own.** That used to be two numbers agreeing by
		// hand -- Saturn's ring tilt and Saturn's (absent) axial tilt were
		// both 26.73 but only one of them did anything -- which is exactly
		// the state the old comment here said would have to change once a
		// body got both rings and a real tilt.
		float RingInner;
		float RingOuter;
		glm::vec3 RingColour;

		// **Bond albedo: the fraction of all incident sunlight reflected back
		// to space**, which is the one that belongs in an energy budget. Not
		// the geometric albedo (how bright the disc looks face-on) and not
		// `Colour`, which was picked to render well -- its luminance puts
		// Earth at 0.46 and would make the equilibrium temperature 17 K too
		// cold. Real measured values: Venus reflects three quarters of what
		// reaches it at 0.76 and still has the hottest surface in the system,
		// which is the greenhouse doing all of the work.
		float BondAlbedo;
	};

	// Real numbers, because they cost nothing and the check at the bottom of
	// the panel is only interesting if they are. Masses are in solar masses:
	// Jupiter is about a thousandth of the Sun, Earth three millionths.
	//
	// The moons are the ones you can pick out in a small telescope, which is
	// also the set whose periods are worth checking: Io at 1.77 days against
	// Callisto at 16.7 is a factor of nine from a factor of five in radius, and
	// nine is what `a^1.5` gives.
	static const std::vector<BodyDescription>& Table()
	{
		static const std::vector<BodyDescription> table =
		{
			{ "Sun",      -1, 0.0,       696000.0, 1.0, 609.0,     0.0f,     { 1.00f, 0.86f, 0.42f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.000f },

			{ "Mercury",   0, 0.387,       2440.0, 1.660e-7, 1407.6, 0.034f,   { 0.62f, 0.58f, 0.54f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.088f },
			{ "Venus",     0, 0.723,       6052.0, 2.448e-6, 5832.5, 177.4f,   { 0.92f, 0.80f, 0.55f }, 0.0410f, 22.5f, 1.0f, { 0.85f, 0.62f, 0.25f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.760f },
			{ "Earth",     0, 1.000,       6371.0, 3.003e-6, 23.934, 23.4392911f, { 0.28f, 0.48f, 0.85f }, 0.0157f, 3.0f, 0.0f, { 0.22f, 0.45f, 1.00f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.306f },
			{ "Mars",      0, 1.524,       3390.0, 3.227e-7, 24.623, 25.19f,   { 0.80f, 0.38f, 0.24f }, 0.0150f, 0.5f, 0.0f, { 0.80f, 0.45f, 0.30f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.250f },
			{ "Jupiter",   0, 5.203,      69911.0, 9.545e-4, 9.925,  3.13f,    { 0.80f, 0.68f, 0.52f }, 0.0700f, 11.0f, 1.0f, { 0.75f, 0.62f, 0.45f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.503f },
			{ "Saturn",    0, 9.537,      58232.0, 2.858e-4, 10.656, 26.73f,   { 0.88f, 0.80f, 0.60f }, 0.0800f, 9.6f, 1.0f, { 0.80f, 0.72f, 0.50f }, 1.24f, 2.27f, { 0.94f, 0.88f, 0.76f }, 0.342f },
			{ "Uranus",    0, 19.191,     25362.0, 4.366e-5, 17.24,  97.77f,   { 0.60f, 0.85f, 0.88f }, 0.0700f, 11.0f, 1.0f, { 0.40f, 0.80f, 0.85f }, 1.60f, 2.01f, { 0.34f, 0.34f, 0.36f }, 0.300f },
			{ "Neptune",   0, 30.070,     24622.0, 5.151e-5, 16.11,  28.32f,   { 0.30f, 0.44f, 0.86f }, 0.0700f, 11.0f, 1.0f, { 0.25f, 0.42f, 0.95f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.290f },

			{ "Moon",      3, 0.002570,    1737.0, 3.694e-8, 655.7, 0.0f,     { 0.72f, 0.71f, 0.68f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.110f },
			{ "Phobos",    4, 0.0000627,     11.3, 5.0e-15, 7.65,  0.0f,     { 0.55f, 0.50f, 0.46f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.071f },
			{ "Io",        5, 0.002819,    1822.0, 4.490e-8, 42.46, 0.0f,     { 0.88f, 0.82f, 0.45f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.630f },
			{ "Europa",    5, 0.004486,    1561.0, 2.413e-8, 85.2,  0.0f,     { 0.80f, 0.78f, 0.72f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.680f },
			{ "Ganymede",  5, 0.007155,    2634.0, 7.450e-8, 171.7, 0.0f,     { 0.66f, 0.62f, 0.58f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.430f },
			{ "Callisto",  5, 0.012585,    2410.0, 5.410e-8, 400.5, 0.0f,     { 0.48f, 0.45f, 0.44f }, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.220f },
			{ "Titan",     6, 0.008168,    2575.0, 6.766e-8, 382.7, 0.0f,     { 0.85f, 0.65f, 0.30f }, 0.2300f, 2.7f, 0.8f, { 0.90f, 0.60f, 0.25f }, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.265f },
		};

		return table;
	}

	void OnDemoAttach() override
	{
		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		for (size_t i = 1; i + 1 < arguments.size(); i++)
			if (arguments[i] == "--years-per-second")
				m_OrbitalYearsPerSecond = (float)std::atof(arguments[i + 1].c_str());

		// Terrain level of detail off, for measuring what it is worth. A
		// checkbox does the same thing, but a checkbox cannot be A/B'd from a
		// shell and this number is the whole reason the feature exists.
		for (size_t i = 1; i < arguments.size(); i++)
			if (arguments[i] == "--no-terrain-lod")
				m_Lod = false;

		for (size_t i = 1; i + 1 < arguments.size(); i++)
			if (arguments[i] == "--earth-radius")
				m_EarthDrawn = glm::max(1000.0f,
					(float)std::atof(arguments[i + 1].c_str()));

		// **Finer than a body needs, because three spheres have to agree.**
		// The planet's stand-in sphere, the sea and the atmosphere shell are
		// all drawn from this one mesh at radii within a few metres of each
		// other, so wherever their facets cross, one shows through the other.
		// At 48x24 that put polygonal patches of missing sea across the far
		// ocean. 128x64 is 16k triangles a sphere and the crossings fall below
		// a pixel.
		m_Sphere.reset(Egss::Mesh::CreateSphere(1.0f, 128, 64));

		BuildShader();
		BuildRings();
		BuildStars();
		BuildTrees();
		BuildLander();
		m_Pocket.Build(m_Material);
		Reset();

		// **Placed here rather than in `OnDemoActivated`.** Activation is an
		// edge detected in `OnUpdate`, which runs *after* the frame's fixed
		// steps -- so a demo that positions itself there has already taken one
		// step from wherever its members were default-constructed. Here that
		// was the origin, which is the middle of the Earth: the first step
		// streamed a planet's worth of chunks around a camera buried inside
		// it, and only then moved the camera out.
		//
		// Four radii out, not eight: the Moon's orbit is 7.8 Earth radii here,
		// so eight put the ship level with it and the frame changed hands
		// every time the Moon came round.
		GoTo(3, 4.0);

		if (!PlaceFromCommandLine())
			LandAtDefaultSite();
	}

	// **The default landing site, and why it is a constant.**
	//
	// The demo used to open four Earth radii out, looking at a blue marble,
	// and getting to the ground meant flying there -- which is the point of
	// the demo but not of opening it. It now opens standing beside the lander,
	// because that is where the game this is a prototype for spends ninety per
	// cent of its time.
	//
	// The direction is hard-coded rather than searched for, and that is the
	// load-bearing part: a *default* site has to be the same site every run,
	// or its terrain cannot be precomputed and the first four hundred frames
	// go back to being a slideshow. It was chosen by `SurveyLandingSites`,
	// which scored forty thousand directions on the Fibonacci spiral for dry
	// ground, standable slope, relief close enough to see, high ground within
	// six kilometres, and water within walking distance. This one measured:
	//
	//     21 m above sea level, slope 0.103 where the lander stands,
	//     233 m of relief inside 400 m, 187 m of high ground within 6 km,
	//     and the shore 150 m away.
	//
	// A shore at the foot of a mountain, which is about as much as one place
	// can be asked to show.
	static glm::vec3 DefaultSite()
	{
		return glm::vec3(-0.660266340f, -0.232047454f, 0.714284480f);
	}

	// The planet-fixed heading from a site toward the highest ground within a
	// couple of kilometres. Sixteen probes, which is sixteen evaluations of
	// the relief -- and it is asked twice, once to point the camera and once
	// to decide what time of day puts the sun behind it.
	glm::vec3 BestOutlook(const VoxelPlanet& planet, const glm::vec3& site) const
	{
		glm::vec3 east, north;
		TangentFrame(site, east, north);

		float best = -1e30f;
		glm::vec3 heading = north;

		for (int i = 0; i < 16; i++)
		{
			float angle = (float)i * 0.39269908f;

			glm::vec3 out = east * std::cos(angle) + north * std::sin(angle);

			glm::vec3 probe = glm::normalize(site
				+ out * (2000.0f / glm::max(planet.Get().Radius, 1.0f)));

			float height = planet.Relief(probe);

			if (height > best)
			{
				best = height;
				heading = out;
			}
		}

		return heading;
	}

	void LandAtDefaultSite()
	{
		// By name, not by index: the table is edited more often than this is.
		size_t index = 0;

		for (size_t i = 1; i < m_Bodies.size(); i++)
			if (m_Bodies[i].Name == "Earth")
				index = i;

		if (index == 0)
			return;

		SetTimeOfDay(index, DefaultSite(),
			BestOutlook(PlanetFor(index), DefaultSite()));

		GoToSite(index, DefaultSite());
		Land();
	}

	// **Winds the clock to a time of day that shows the place.**
	//
	// The site is a fixed direction in the planet's own frame, and at
	// `m_Time = 0` the spin is the identity -- so whether the default site
	// opens in daylight is decided by an epoch chosen for the orbits, which is
	// to say by accident. It opened at dawn.
	//
	// Winding the clock is the physical answer rather than a graphical one:
	// the sun is where the sun is, and this is what time it is when you
	// arrive. The cost is a fraction of a day of orbital motion, and a day
	// here is 1/365 of a year.
	//
	// **And the sun goes behind you, not in front.** Facing the high ground
	// with the sun beyond it means facing the one face of it the sun does not
	// reach: correctly shaded, and at five per cent of a dark green -- there
	// is no exposure curve between the framebuffer and the screen -- a
	// **silhouette**. The first version of this wound the clock to two hours
	// before local noon and produced a black mountain against a blue sky. The
	// mountain was right. Nobody could see it.
	//
	// Scanned rather than solved. The elevation alone has a closed form, but
	// "high, and behind that heading" does not, and a hundred and twenty
	// samples of a cheap expression is not worth being clever about.
	void SetTimeOfDay(size_t index, const glm::vec3& site, const glm::vec3& heading)
	{
		double rate = SpinRate(index);

		if (std::abs(rate) < 1e-12)
			return;

		glm::dvec3 toStar = BodyScene(0) - BodyScene(index);
		double length = glm::length(toStar);

		if (length < 1e-6)
			return;

		glm::dvec3 sunward = toStar / length;

		glm::dvec3 up = glm::dvec3(glm::normalize(site));
		glm::dvec3 look = glm::dvec3(glm::normalize(heading));

		double turn = 2.0 * 3.14159265358979323846;

		double bestScore = -1e30;
		double bestAngle = 0.0;

		for (int i = 0; i < 120; i++)
		{
			double angle = turn * (double)i / 120.0;

			// The spin takes planet-fixed to scene, so the sun in the
			// planet's own frame is the scene sun taken the other way.
			glm::dvec3 sun = RotateAboutAxis(sunward, SpinAxis(index), -angle);

			double elevation = glm::dot(sun, up);

			if (elevation < 0.25)
				continue;

			// Behind the shoulder: the sun's own azimuth against the heading,
			// with the vertical component taken out of both.
			glm::dvec3 flat = sun - up * elevation;
			double size = glm::length(flat);

			double behind = size > 1e-6 ? -glm::dot(flat / size, look) : 0.0;

			// A sun straight overhead lights every slope equally, which is the
			// one lighting that hides the shape of the ground; 0.62 is about
			// 38 degrees up, where a hillside still has a lit and a shaded
			// side but nothing is in the dark.
			double height = 1.0 - std::abs(elevation - 0.62) * 1.6;

			double score = behind * 1.0 + height;

			if (score > bestScore)
			{
				bestScore = score;
				bestAngle = angle;
			}
		}

		m_Time = bestAngle / rate;
	}

	// Puts the ship twenty metres over one exact direction in the planet's own
	// frame, facing whatever is most worth looking at.
	void GoToSite(size_t index, const glm::vec3& fixed)
	{
		VoxelPlanet& planet = PlanetFor(index);

		glm::vec3 site = glm::normalize(fixed);

		double surface = (double)planet.Get().Radius + (double)planet.Relief(site);

		glm::dvec3 direction = glm::normalize(ToScene(index, glm::dvec3(site)));

		m_Frame = index;
		m_Local = direction * (surface + 0.5);

		glm::vec3 vertical = glm::vec3(direction);

		glm::vec3 east, north;
		TangentFrame(vertical, east, north);

		// **Facing the high ground**, which is the difference between opening
		// with a mountain in front of you and opening with one behind you.
		glm::vec3 heading = glm::vec3(ToScene(index,
			glm::dvec3(BestOutlook(planet, site))));

		m_Up = vertical;
		m_Forward = glm::normalize(heading - vertical * glm::dot(heading, vertical));

		Reorthogonalise();

		m_YearsPerSecond = TargetYearsPerSecond();
	}

	// **Does everything still fit?**
	//
	// The scale map is one expression, and the one way it can be wrong is by
	// putting a moon inside its planet or a planet inside the star. The last
	// column is that test: the orbit in units of the parent's drawn radius,
	// which must be greater than one plus the moon's own. Phobos is the tight
	// one at 1.66, because it really is a tight orbit -- 2.77 Mars radii.
	void ReportScale() const
	{
		EGSS_TRACE("Solar system: p = {0:.3f} (orbits), q = {1:.3f} (bodies), "
			"Earth {2:.1f} m, 1 AU = {3:.0f} m",
			m_Compression, m_BodyScale, DrawnRadius(3),
			DrawnLength(s_AuKm, (double)m_Compression));

		for (size_t i = 0; i < m_Bodies.size(); i++)
		{
			if (m_Bodies[i].Parent < 0)
			{
				EGSS_TRACE("  {0:<9} r = {1:8.1f} m", m_Bodies[i].Name, DrawnRadius(i));
				continue;
			}

			size_t parent = (size_t)m_Bodies[i].Parent;
			double orbit = DrawnLength(m_Bodies[i].SemiMajorAu * s_AuKm, OrbitExponent(i));
			double clearance = orbit / (DrawnRadius(parent) + DrawnRadius(i));

			EGSS_TRACE("  {0:<9} r = {1:8.1f} m, a = {2:9.1f} m, {3:.2f}x clear of {4}",
				m_Bodies[i].Name, DrawnRadius(i), orbit, clearance, m_Bodies[parent].Name);
		}
	}

	// `--land Earth` (or `--goto Earth`) puts the ship somewhere specific at
	// startup, which is how a capture reaches one -- an unattended run has
	// nobody to fly it.
	bool PlaceFromCommandLine()
	{
		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		// A replay places itself: the recording drives the ship from wherever
		// the run it came from started, and landing first would put it
		// somewhere the recorded input was never taken at.
		if (Egss::Input::IsPlayingBack())
			return true;

		// `--orbit` is how you get the old opening back: four radii out,
		// looking at the planet.
		for (const std::string& argument : arguments)
			if (argument == "--orbit")
				return true;

		for (size_t i = 1; i + 1 < arguments.size(); i++)
		{
			bool land = arguments[i] == "--land";

			if (!land && arguments[i] != "--goto")
				continue;

			const std::string& wanted = arguments[i + 1];

			// From zero, so `--goto Sun` finds the Sun. It used to start at
			// one -- the star is not somewhere you land, so the loop that
			// serves both flags skipped it -- and `--goto Sun` reported that
			// the Sun matches no body. `Land` refuses the star on its own and
			// says why, which is the better place for that to be decided.
			for (size_t body = 0; body < m_Bodies.size(); body++)
			{
				if (m_Bodies[body].Name != wanted)
					continue;

				// Approached from the star's side, so the landing site is lit
				// rather than in the middle of its night.
				GoTo(body, land ? 1.06 : 6.0, land ? 0.0 : 55.0, land);

				if (land)
					Land();

				return true;
			}

			EGSS_WARN("{0} {1} matches no body", arguments[i], wanted);
		}

		return false;
	}

	// Put the ship `radii` of the body's own drawn radius out from its centre,
	// on the sunward side, looking at it. A teleport, and said to be one: it
	// is how the panel's buttons work and how a capture gets framed. Flying
	// there is the point of the demo, but not of a screenshot.
	//
	// `landing` says the arrival is meant to be walked away from, which asks
	// two things of it that a flyby does not care about: that there is dry
	// land underneath, and that the height clears the ground that is actually
	// there rather than a multiple of the mean radius.
	void GoTo(size_t index, double radii, double tiltDegrees = 55.0, bool landing = false)
	{
		glm::dvec3 toStar = BodyScene(0) - BodyScene(index);
		double length = glm::length(toStar);

		glm::dvec3 sunward = length > 1e-6
			? toStar / length : glm::dvec3(0.0, 0.0, 1.0);

		// Round from the sunward point toward the terminator, because a planet
		// lit dead-on is a flat disc and a planet lit from the side is a
		// sphere. Zero tilt is the landing approach, where standing on the lit
		// half is the whole point.
		glm::vec3 east, north;
		TangentFrame(glm::vec3(sunward), east, north);

		double tilt = glm::radians(tiltDegrees);
		glm::dvec3 direction = glm::normalize(
			sunward * std::cos(tilt) + glm::dvec3(north) * std::sin(tilt));

		if (landing)
			direction = LandingApproach(index, direction, radii);

		m_Frame = index;
		m_Local = direction * (DrawnRadius(index) * radii);

		glm::vec3 vertical = glm::normalize(glm::vec3(direction));

		// **The body is always straight down**, which is the one direction a
		// levelled camera cannot look: forward along the local vertical is
		// parallel to up, and levelling -- which pulls up back toward that
		// same vertical -- then tumbles the view.
		//
		// Far enough out the levelling is off (`UpdateFlight` fades it by four
		// radii), so there it is safe to look straight down and +Y serves as
		// up. Closer in, pitch down to the limb instead: it is `asin(1/radii)`
		// off the vertical, so from just above the surface that is three
		// degrees -- the horizon, which is what a landing approach wants.
		if (radii >= 4.0)
		{
			m_Forward = -vertical;
			m_Up = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		else
		{
			float limb = glm::degrees(std::asin(
				glm::clamp(1.0f / (float)radii, 0.0f, 1.0f)));

			float pitch = glm::radians(-glm::max(90.0f - limb - 8.0f, 0.0f));

			TangentFrame(vertical, east, north);

			m_Up = vertical;
			m_Forward = glm::normalize(north * std::cos(pitch) + vertical * std::sin(pitch));
		}

		Reorthogonalise();

		// A teleport resets the clock too, rather than letting it ease in from
		// whatever the last place wanted: the blend takes about a third of a
		// second, and at the orbital rate a third of a second is several turns
		// of the planet you have just arrived at.
		m_YearsPerSecond = TargetYearsPerSecond();
	}

	// **Where a lander should actually arrive**, given where it was aiming.
	//
	// Two corrections, and the order matters only in that the second needs the
	// first's answer.
	//
	// *Dry land.* Earth is 29.2% land, so the sunward point the approach aims
	// at is sea seven times in ten -- and the sea is not a surface here, it is
	// a shell drawn over the ground, so landing under it puts the eye 1.5 m
	// below the water with the seabed underfoot. `NearestLand` moves the site
	// the smallest angle that fixes it, which keeps the landing on the lit
	// half that the zero tilt was chosen for.
	//
	// *Height.* `radii` is a multiple of the *mean* radius, and relief here is
	// 8.5% of it: 1.06 radii is 10 m over a valley on Earth and 21 m *inside*
	// a ridge on Jupiter. So the arrival radius is the ground at the site plus
	// a fixed drop -- 20 m whatever the body, since it is a distance to fall
	// and not a fraction of anything.
	//
	// **The search runs in planet-fixed coordinates**, which is where the
	// terrain is; the direction goes in through `ToFixed` and comes back out
	// through `ToScene`. Skipping either would land on the right patch of
	// ground at t = 0, where the spin is the identity, and somewhere else at
	// every other moment -- and a day here is sixty seconds.
	glm::dvec3 LandingApproach(size_t index, const glm::dvec3& aimed, double& radii)
	{
		VoxelPlanet& planet = PlanetFor(index);

		glm::vec3 wanted = glm::vec3(ToFixed(index, aimed));

		// **Ten metres of dry ground, and the number was measured rather than
		// picked.** Two things bound it from opposite sides.
		//
		// Above, the shoreline: relief runs for five octaves, so the finest
		// thing in it is 2.6 m across and the coast wanders by about a metre
		// at that scale. Two independent probe sets -- 225 points on rings
		// against a 397-point sunflower -- agreed about every one of 812
		// directions at 3 m, on 98.8% at 10 m and on 92.7% at 28 m. Past ten
		// metres the search is asserting more than it can see.
		//
		// Below, how much planet is left to land on: 8.4% of Earth's surface
		// is 10 m clear of water against 1.3% at 28 m, and a rarer site is a
		// site further from where the lander was pointed. At ten the landing
		// moves 9.4 degrees off the approach on average and 27.5 at worst,
		// which is still the lit half and still the hemisphere you aimed at.
		glm::vec3 site = planet.NearestLand(wanted, 10.0f);

		double surface = (double)planet.Get().Radius + (double)planet.Relief(site);

		// **Replaces the caller's multiple, and does not take the larger.**
		// `max` was right while a multiple of the radius and a fixed drop were
		// the same order of thing: 1.06 radii of a 360 m planet is 21 m up.
		// At Earth's own radius it is **382 km**, so `--land Earth` put the
		// ship in orbit and the capture came back a picture of the horizon.
		// A landing arrives at a height, not at a fraction.
		radii = (surface + 20.0) / DrawnRadius(index);

		return glm::normalize(ToScene(index, glm::dvec3(site)));
	}

	// Said once, and only if this demo is the one being looked at: `OnAttach`
	// runs for every layer, so anything logged there is logged whichever demo
	// you asked for.
	void OnDemoActivated() override
	{
		if (m_Reported)
			return;

		ReportScale();
		m_Reported = true;
	}

	// --- The scale map ------------------------------------------------------

	// **Two exponents, and the split is between systems rather than between
	// kinds of length.**
	//
	// One exponent was the original design and it had a cost that showed:
	// `p = 1/2` applied to radii as well as orbits made Jupiter 3.3 times
	// Earth where it is really 11, so the gas giants read as another Earth
	// with a different colour. Raising `p` on its own is not available -- it
	// is what keeps the system 302 km wide instead of 8,453.
	//
	// So heliocentric distance keeps `p`, and everything *inside* a body's own
	// system -- its radius, its moons' radii, its moons' orbits -- goes
	// through `q`. Within one system the map is still a single uniform power,
	// which is the property the original design was built on: nothing
	// overtakes anything and no moon needs special-casing to stay outside its
	// planet, because the clearance `a^q / (R^q + r^q)` is monotone in every
	// one of its arguments.
	//
	// `q = 3/4` is where two constraints meet. Below it the giants shrink back
	// toward Earth; above it the Sun -- which grows as `109.3^q` while
	// Mercury's orbit is fixed at `9088^p` -- eats the inner planets, and it
	// swallows Mercury outright at `q = 0.97`. Three quarters puts Jupiter at
	// 6.0 Earths and leaves Mercury 2.8 solar radii clear.
	//
	// What it costs, stated rather than hidden: a moon's orbit now grows with
	// its planet's system rather than with the Sun's, so the Jovian system is
	// 25 km across where Jupiter's own orbit is 126 km. Really that ratio is
	// 0.24%, and here it is 20%. The moons are a *place* at this scale, which
	// is the trade -- and both exponents are sliders if you want to see it.
	double DrawnLength(double km, double exponent) const
	{
		if (km <= 0.0)
			return 0.0;

		return (double)m_EarthDrawn * std::pow(km / s_EarthRadiusKm, exponent);
	}

	// Radii are local geometry, whoever the body orbits.
	double DrawnRadius(size_t index) const
	{
		// A floor, because Phobos is 11.3 km across and lands at 3 m: small
		// enough to fly past without seeing, but not so small that the voxel
		// lattice under it has nothing to say.
		return std::max(12.0, DrawnLength(m_Bodies[index].RadiusKm, (double)m_BodyScale));
	}

	// **Whether a body's own disc reaches a pixel, anywhere reasonable.**
	//
	// Every body gets the same 128x64 sphere for its stand-in mesh, sixteen
	// moons and planets included, whether it fills the horizon or is a point
	// the star field would have drawn just as well -- nothing before this
	// asked how big a body actually is on screen before paying for it.
	//
	// `radius / distance` is the tangent of the angular radius, which is the
	// angular radius itself for anything this small: real scale puts every
	// non-local body here below a hundredth of a degree. 4e-4 is under half a
	// pixel at 1080p and this demo's narrower field of view (55 degrees), so
	// nothing culled by it could have shown as more than a single aliased
	// dot -- which the procedural star field behind it already draws.
	static constexpr double s_MinAngularSize = 4.0e-4;

	bool WorthDrawing(size_t index, const glm::dvec3& centre, float scale) const
	{
		double distance = glm::length(centre);

		if (distance < 1.0)
			return true;

		return DrawnRadius(index) * (double)scale / distance > s_MinAngularSize;
	}

	// Which exponent an orbit is drawn with: a planet's is a distance across
	// the solar system, a moon's is a distance across its planet's.
	double OrbitExponent(size_t index) const
	{
		return m_Bodies[index].Parent > 0
			? (double)m_BodyScale : (double)m_Compression;
	}

	// An offset in AU, compressed along its own direction. Applied per level of
	// the hierarchy, so a moon's offset is compressed as a moon-sized distance
	// and its planet's as a planet-sized one, which is what keeps Phobos above
	// Mars while Neptune is still 302 km out.
	glm::dvec3 DrawnOffset(const glm::dvec3& au, double exponent) const
	{
		double length = glm::length(au);

		if (length <= 0.0)
			return glm::dvec3(0.0);

		return au * (DrawnLength(length * s_AuKm, exponent) / length);
	}

	glm::dvec3 BodyScene(size_t index) const
	{
		glm::dvec3 at(0.0);

		// Walked up the hierarchy rather than stored, so a moon is exactly its
		// planet's position plus its own and cannot drift away from it.
		for (int i = (int)index; i >= 0; i = m_Bodies[(size_t)i].Parent)
			at += DrawnOffset(m_Bodies[(size_t)i].Position, OrbitExponent((size_t)i));

		return at;
	}

	glm::dvec3 ShipScene() const { return BodyScene(m_Frame) + m_Local; }

	// Which body the ship's position is measured from.
	size_t FrameBody() const { return m_Frame; }
	bool Walking() const { return m_Walking; }

	// --- Drawing a system that is 10^12 metres across ------------------------
	//
	// **A distant body is drawn nearer and smaller, at the same angular size.**
	//
	// At true scale Neptune is 4.5e12 m away and the ground under your boots
	// is 0.15 m. No depth buffer spans that, and a far plane that reaches
	// Neptune leaves the terrain in one depth value. So everything except the
	// body you are actually at is moved in along its own direction and scaled
	// by the same factor: the direction is unchanged, the ratio of radius to
	// distance is unchanged, and therefore *the picture is unchanged* -- what
	// changes is only which depth bucket it lands in.
	//
	// The map is logarithmic past `s_NearRange` and the identity below it, so
	// it is continuous, and monotone -- which is the property that matters,
	// because it means a body in front of another still comes out in front and
	// ordinary depth testing keeps working between them.
	//
	// The body whose terrain is streamed is never compressed: its chunks are
	// drawn at their true positions and a scaled sphere underneath them would
	// not line up. Everything else may be, and once terrain is evicted -- which
	// is the same test -- nothing is left to disagree with.
	static constexpr double s_NearRange = 2.0e5;

	double CompressedDistance(double distance) const
	{
		if (distance <= s_NearRange)
			return distance;

		return s_NearRange * (1.0 + std::log(distance / s_NearRange));
	}

	// True whenever this body's own geometry is being drawn around the camera,
	// which is exactly the condition `StreamTerrain` uses to decide whether to
	// keep its chunks.
	bool NearField(size_t index) const
	{
		if (index == 0 || index != (m_Walking ? (size_t)m_Ground : m_Frame))
			return false;

		if (m_Walking)
			return true;

		double radius = DrawnRadius(index);

		return glm::length(m_Local) - radius <= radius * 0.5 + (double)m_LoadRadius;
	}

	// Where to draw a body and what to multiply its own lengths by. One is
	// useless without the other: scaling the offset without the radius moves a
	// planet, and scaling both keeps it exactly where it looked.
	// **The centre comes back in double.** Standing on a planet, the body's
	// centre is a radius away -- 250 km today and 6,371 km at 1:1 -- so a
	// float centre is quantised to metres before anything is added to it. The
	// terrain's chunks are placed by adding their own offset to this, and that
	// sum has to be done at full precision before it is cast; a float here
	// would put the whole shell back on a 0.76 m grid however carefully the
	// chunk offsets were computed. Callers that only want to place a sphere
	// cast it themselves.
	float BodyPlacement(size_t index, const glm::dvec3& origin, glm::dvec3& outCentre) const
	{
		glm::dvec3 offset = BodyScene(index) - origin;

		double distance = glm::length(offset);

		if (NearField(index) || distance <= s_NearRange || distance <= 0.0)
		{
			outCentre = offset;
			return 1.0f;
		}

		double scale = CompressedDistance(distance) / distance;

		outCentre = offset * scale;

		return (float)scale;
	}

	// --- Spin ---------------------------------------------------------------
	//
	// **The planet turns; the sunlight does not.**
	//
	// The first version had this the other way round -- terrain generated in
	// planet-fixed coordinates, and day and night produced by rotating the
	// light direction backwards. That is correct as far as the surface can
	// tell, and it was invisible while the surface was its own scene. In one
	// space it is not: the lit hemisphere is then whichever way the *spin*
	// points, not whichever way the Sun is, so a planet seen from four radii
	// out with the Sun behind the camera showed its night side.
	//
	// So the spin rotates the things that are planet-fixed -- the terrain, the
	// loose rocks, and the standing player -- on their way into scene
	// coordinates, and the light is simply the direction the star is in. The
	// ship's own position stays *inertial*: co-rotating with the frame would
	// mean hovering geostationary over a planet, which sounds harmless until
	// the clock speeds up on the way out and the frame spins seven times a
	// second.
	//
	// A standing player is planet-fixed, so the conversion carries them round
	// once a day and the Sun crosses their sky without being moved at all.
	//
	// Radians of spin per year. The year is 8,766 hours. RotationHours is
	// always positive now -- see the note on AxialTiltDegrees for what used
	// to be a sign here.
	double SpinRate(size_t index) const
	{
		double hours = m_Bodies[index].RotationHours;

		if (std::abs(hours) < 1e-6)
			return 0.0;

		return (365.25 * 24.0) / hours * 2.0 * 3.14159265358979323846;
	}

	double SpinAngle(size_t index) const
	{
		return m_Time * SpinRate(index);
	}

	// **A body's own north, tilted out of the ecliptic pole by its real
	// obliquity.** Every body here tilts about the same reference axis --
	// global +X, tipping +Y toward +Z -- rather than each having its own
	// right ascension of pole. That is a real simplification (Jupiter's pole
	// and Earth's do not actually point the same way relative to their own
	// orbits), but it is the one direction this demo already checks against
	// real data: `SkyDirection` rotates the star catalogue from equatorial
	// into ecliptic coordinates about this exact axis, by Earth's exact
	// obliquity, verified to 0.003 degrees against published declinations.
	// Reusing it means Earth's ground agrees with Earth's own sky.
	//
	// A tilt past 90 degrees points the axis through the equator and out the
	// other side, in +Z as well as -Y -- which is what turns a positive spin
	// into an apparent retrograde for Uranus (97.77) and Venus (177.4)
	// without `SpinRate` ever carrying a sign.
	glm::dvec3 SpinAxis(size_t index) const
	{
		double tilt = glm::radians((double)m_Bodies[index].AxialTiltDegrees);

		return glm::dvec3(0.0, std::cos(tilt), std::sin(tilt));
	}

	// Prograde is +x toward +z, the same sense the orbits start in, about
	// whichever axis is passed in. Reduces exactly to the old Y-only rotation
	// when axis is +Y, which is what every untilted body still uses.
	static glm::dvec3 RotateAboutAxis(const glm::dvec3& v, const glm::dvec3& axis, double angle)
	{
		double c = std::cos(angle), s = std::sin(angle);

		return v * c - glm::cross(axis, v) * s + axis * (glm::dot(axis, v) * (1.0 - c));
	}

	glm::dvec3 ToScene(size_t index, const glm::dvec3& fixed) const
	{
		return RotateAboutAxis(fixed, SpinAxis(index), SpinAngle(index));
	}

	glm::dvec3 ToFixed(size_t index, const glm::dvec3& scene) const
	{
		return RotateAboutAxis(scene, SpinAxis(index), -SpinAngle(index));
	}

	// Where the camera is in the planet's own frame: what streaming centres
	// on, and what the forest measures its instance offsets from.
	glm::dvec3 TerrainFocus(size_t index) const
	{
		return ToFixed(index, m_Local);
	}

	// **The same rotation as a matrix, built by hand.**
	//
	// `glm::rotate(m, angle, axis)` is not `RotateAboutAxis(v, axis, angle)`:
	// glm's matrix takes +x toward *-z* about +Y, and this one takes it
	// toward +z. Both are perfectly ordinary rotations; they just differ in
	// which way the angle counts, and mixing them once meant the terrain was
	// drawn through the inverse of the spin the camera was placed by -- the
	// ground under your feet slid round the planet at *twice* the rate of the
	// day, invisible at t = 0 and 62 degrees out after seven seconds. Nothing
	// catches that by looking: the horizon sphere is smooth and symmetric.
	//
	// This is Rodrigues' rotation formula as a matrix, in the same sign
	// convention as `RotateAboutAxis` above -- the self-test checks the two
	// against each other over a spread of axes and angles rather than just
	// deriving one from the other on paper. At axis = +Y it is the original
	// hand-built matrix, term for term.
	glm::mat4 SpinMatrix(size_t index) const
	{
		return RotationMatrix(glm::vec3(SpinAxis(index)), SpinAngle(index));
	}

	// The same rotation, about the same axis, at whatever angle is asked
	// for -- factored out of SpinMatrix so the cloud layer can drift at its
	// own rate around the body's real spin axis without a second copy of
	// Rodrigues' formula. See SpinMatrix's own note for the sign convention.
	static glm::mat4 RotationMatrix(const glm::vec3& n, double angle)
	{
		float c = (float)std::cos(angle), s = (float)std::sin(angle);

		glm::vec3 col0(c + (1.0f - c) * n.x * n.x,
			-s * n.z + (1.0f - c) * n.x * n.y,
			 s * n.y + (1.0f - c) * n.x * n.z);
		glm::vec3 col1(s * n.z + (1.0f - c) * n.x * n.y,
			c + (1.0f - c) * n.y * n.y,
			-s * n.x + (1.0f - c) * n.y * n.z);
		glm::vec3 col2(-s * n.y + (1.0f - c) * n.x * n.z,
			 s * n.x + (1.0f - c) * n.y * n.z,
			 c + (1.0f - c) * n.z * n.z);

		return glm::mat4(
			glm::vec4(col0, 0.0f),
			glm::vec4(col1, 0.0f),
			glm::vec4(col2, 0.0f),
			glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	}

	// The cloud shell's own rotation: the same axis the body itself spins
	// about, but at its own drift rate, so a cloud band does not stay glued
	// to the coastline under it.
	glm::mat4 CloudMatrix(size_t index) const
	{
		return RotationMatrix(glm::vec3(SpinAxis(index)), SpinAngle(index) + m_CloudDrift);
	}

	// --- Planets ------------------------------------------------------------

	// **Voxel size is set by how many chunks the index can name, not by
	// memory.** The chunk store is sparse now, so a body's lattice costs
	// nothing until something is written to it -- what is finite is the chunk
	// *key*, which packs three axes into 21 bits each and so tops out at
	// 2,097,152 chunks a side.
	//
	// `R / 4e6` keeps every body inside a million: Earth lands at 1.59 m, which
	// is a size you can walk on, and Jupiter at 17.5 m, which nobody stands on
	// anyway. The floor is what a stride wants; the ceiling is arbitrary and
	// has never been reached.
	float VoxelSizeFor(size_t index) const
	{
		return glm::clamp((float)DrawnRadius(index) / 4.0e6f, 1.5f, 32.0f);
	}

	VoxelPlanet& PlanetFor(size_t index)
	{
		auto it = m_Planets.find(index);
		if (it != m_Planets.end())
			return it->second;

		float radius = (float)DrawnRadius(index);

		VoxelPlanet::Settings settings;
		settings.Radius = radius;
		settings.VoxelSize = VoxelSizeFor(index);
		// **Relief as a fraction of the radius, and the real one is small.**
		// It was 8.5%, which on a 360 m planet is 31 m of hills and on a real
		// one is 541 km -- sixty Everests. Earth's whole range, Everest to the
		// Marianas, is 0.31% of its radius; a quarter of a percent here gives
		// 15.9 km of relief, which is that with the trench filled in.
		settings.Amplitude = radius * 0.0025f;
		settings.FeatureSize = radius * 0.195f;

		// Enough octaves to reach about 50 m of wavelength, and no more: past
		// that the amplitude is under a centimetre and the sampling coordinate
		// is where a float stops resolving a noise cell.
		settings.Octaves = glm::clamp(
			(int)std::ceil(std::log2(glm::max(settings.FeatureSize / 50.0f, 2.0f))),
			4, 16);

		// The local layer. A 1/f fractal anchored at a continent has nothing
		// underfoot on a body this size -- see the note on `Roughness`.
		settings.Roughness = glm::min(radius * 3.0e-5f, 25.0f);
		settings.RoughnessSize = 320.0f;
		settings.Seed = (unsigned int)(index * 7919 + 13);
		settings.LowColour = m_Bodies[index].Colour * 0.55f;
		settings.HighColour = glm::mix(m_Bodies[index].Colour, glm::vec3(1.0f), 0.35f);

		// **One planet in the system is alive, and it is the third one.**
		// Everything that makes Earth look like Earth from orbit comes from
		// these two lines plus the number beside them: a sea covering 70.8% of
		// it, and a surface coloured by where the water, the altitude and the
		// latitude put you rather than by height alone.
		if (m_Bodies[index].Name == "Earth")
		{
			settings.HasOcean = true;
			settings.Vegetated = true;
			settings.LandFraction = 0.292f;

			// Continents rather than ridge filaments -- see the note on
			// `ContinentShare`. Roughly a planet-radius across, which is a
			// handful of landmasses.
			settings.ContinentShare = 0.62f;
			settings.ContinentSize = radius * 0.45f;
			settings.PlantsPerChunk = 14;

			// Finer ridges than the other bodies get, because the continents
			// are already carrying the large shapes here -- without this the
			// ground you walk on is a lawn.
			settings.FeatureSize = radius * 0.115f;

			settings.Octaves = glm::clamp(
				(int)std::ceil(std::log2(glm::max(settings.FeatureSize / 50.0f, 2.0f))),
				4, 16);

			// **The landscape you stand in, which the planetary spectrum
			// cannot provide.** See `Settings::Landscape`. Four kilometres
			// between ridgelines, 700 m of rise where the uplift is full,
			// sixty kilometres between one range and the next, and 18% of the
			// amplitude kept out on the plains so they roll rather than lie
			// flat. Measured over two dozen sites: 49 m of relief inside 400 m
			// on average against the 8.5 m this had before, 133 m in the
			// ranges, 18 m on the flats.
			settings.Landscape = 700.0f;
			settings.LandscapeSize = 4200.0f;
			settings.LandscapeOctaves = 6;
			settings.UpliftSize = 60000.0f;
			settings.LandscapeFloor = 0.18f;

			// The finest landscape octave is 131 m; without this there is a
			// gap between it and the 80 m the roughness layer reaches.
			settings.Roughness = 22.0f;
			settings.RoughnessSize = 300.0f;
		}

		EGSS_TRACE("{0}: voxel planet, radius {1:.0f} m, relief {2:.0f} m, voxel {3:.2f} m",
			m_Bodies[index].Name, settings.Radius, settings.Amplitude, settings.VoxelSize);

		VoxelPlanet& planet = m_Planets[index];
		planet.Create(settings);

		// **One file of edits a body, and only edits go in it.** The world is
		// procedural, so nothing is lost by regenerating it -- what would be
		// lost is the hole somebody dug, and that is the only thing stored.
		// The name is the body's, so digging on Mars does not overwrite what
		// was dug on Earth.
		planet.OpenEdits("planet-" + m_Bodies[index].Name + ".edits");

		// And the landing-site cache, which is the *terrain* rather than the
		// changes to it -- see `VoxelPlanet::OpenSiteCache`. Per body, like the
		// edits, so preparing a site on Mars does not evict the one on Earth.
		planet.OpenSiteCache("planet-" + m_Bodies[index].Name + ".site");

		return planet;
	}

	// --- Frames -------------------------------------------------------------

	// **Whose gravity well are you in?** Not the nearest body by metres --
	// that would hand you to Phobos while you were still closer to Mars in any
	// sense that matters -- but the smallest `distance / radius`, which is
	// scale-free and picks the body that fills the most of your sky.
	size_t DominantBody() const
	{
		glm::dvec3 ship = ShipScene();

		size_t best = 0;
		double bestRatio = 1e30;

		for (size_t i = 0; i < m_Bodies.size(); i++)
		{
			double ratio = glm::length(BodyScene(i) - ship) / DrawnRadius(i);

			if (ratio < bestRatio)
			{
				bestRatio = ratio;
				best = i;
			}
		}

		return best;
	}

	// **The handover has to be exact, or flight is not continuous.** The offset
	// between the two frames is added to the local position in the same
	// statement it stops being applied by `BodyScene`, so the ship's scene
	// position is unchanged to the last bit a double holds. Measured the first
	// time it happens rather than asserted, because a silent teleport of a few
	// metres is exactly the kind of thing that reads as a physics bug later.
	void UpdateFrame()
	{
		// Standing on a planet means being in that planet's frame by
		// definition -- the physics world's coordinates are its own.
		if (m_Walking)
			return;

		size_t candidate = DominantBody();

		if (candidate == m_Frame)
			return;

		glm::dvec3 before = ShipScene();
		double ratio = glm::length(BodyScene(candidate) - before) / DrawnRadius(candidate);
		double current = glm::length(BodyScene(m_Frame) - before) / DrawnRadius(m_Frame);

		// Hysteresis, or the boundary between two wells chatters every step.
		if (ratio > current * 0.8)
			return;

		m_Local += BodyScene(m_Frame) - BodyScene(candidate);
		m_Frame = candidate;

		double residual = glm::length(ShipScene() - before);
		m_HandoverResidual = std::max(m_HandoverResidual, residual);

		EGSS_TRACE("frame -> {0} ({1:.2f}x its radius), moved {2:.3e} m",
			m_Bodies[candidate].Name, ratio, residual);
	}

	// --- Flying -------------------------------------------------------------

	void SetMouseLook(bool on)
	{
		m_MouseLook = on;
		m_HasMouse = false;

		if (!Egss::Input::IsPlayingBack())
			Egss::Application::Get().GetWindow().SetCursorCaptured(on);
	}

	// How far to turn this step, in degrees: positive yaw is to the right,
	// positive pitch is up. Shared by both modes, so the view does not jump
	// when you step out of the ship.
	//
	// **Tab toggles, and the mouse does nothing until it does.** The first
	// version read raw cursor deltas every step with no gate, which made the
	// demo depend on where the pointer happened to be -- and that is not a
	// small thing here, because it broke step determinism: two identical
	// `--lockstep --land Earth` runs captured at step 300 produced different
	// frames, one of them looking at the sky and one at the ground. The
	// desktop cursor drifting during an unattended run was steering the
	// camera. Same convention as `FirstPersonController`, for the same reason.
	void LookDelta(float dt, float& yaw, float& pitch)
	{
		bool toggle = Egss::Input::IsKeyPressed(EGSS_KEY_TAB);

		if (toggle && !m_WasToggling)
			SetMouseLook(!m_MouseLook);
		else if (m_MouseLook && Egss::Input::IsKeyPressed(EGSS_KEY_ESCAPE))
			SetMouseLook(false);

		m_WasToggling = toggle;

		std::pair<float, float> mouse = Egss::Input::GetMousePosition();

		yaw = 0.0f;
		pitch = 0.0f;

		if (m_MouseLook)
		{
			// The step that turns mouse-look on has no previous position to
			// subtract, and capturing the cursor moves it -- so skip one.
			if (m_HasMouse)
			{
				yaw = (mouse.first - m_LastMouse.first) * m_LookSpeed;

				// Screen y grows downward, so pushing the mouse away is a
				// negative delta and has to raise the pitch.
				pitch = -(mouse.second - m_LastMouse.second) * m_LookSpeed;
			}

			m_HasMouse = true;
		}

		m_LastMouse = mouse;

		float rate = m_LookRate * dt;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  yaw -= rate;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) yaw += rate;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    pitch += rate;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  pitch -= rate;
	}

	static glm::vec3 RotateAbout(const glm::vec3& v, const glm::vec3& axis, float radians)
	{
		return glm::angleAxis(radians, glm::normalize(axis)) * v;
	}

	void Reorthogonalise()
	{
		m_Forward = glm::normalize(m_Forward);

		// Degenerate only if the two are parallel, which the pitch clamp below
		// prevents in flight and the tangent frame prevents on the ground --
		// but a teleport can set them up that way, so it is handled here once.
		if (std::abs(glm::dot(m_Forward, m_Up)) > 0.999f)
		{
			glm::vec3 reference = std::abs(m_Forward.y) < 0.9f
				? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

			m_Up = glm::normalize(reference - m_Forward * glm::dot(m_Forward, reference));
			return;
		}

		m_Up = glm::normalize(m_Up - m_Forward * glm::dot(m_Forward, m_Up));
	}

	// Height above the nearest body's *ground*, over every body rather than
	// just the frame's -- it is what sets the flight speed, and passing close
	// to a moon should slow you down whoever's frame you are in. It also sets
	// the near plane, which is where getting it wrong shows.
	//
	// **The mean radius is not the ground, and the difference is the relief.**
	// This used to return the height above `DrawnRadius`, and the near plane
	// is 0.4 of it. On a 360 m planet whose hills are 15 m that put the near
	// plane 3.5 m out while standing -- a sliver of ground missing at the very
	// bottom of the frame, which nobody ever noticed. At 250 km the hills are
	// 380 m, so the same expression put the near plane **76 metres** out and
	// clipped away everything within seventy-six metres of the camera: the
	// grass under your boots, the trees you were standing among, all of it,
	// leaving a hard horizontal line across the frame with the stand-in sphere
	// and the insides of the chunk meshes showing through underneath.
	//
	// On foot the answer is simply the eye height. The ground is right there;
	// nothing about the mean radius enters into it.
	double AltitudeAboveAnything() const
	{
		if (m_Walking)
			return (double)m_EyeHeight;

		glm::dvec3 ship = ShipScene();
		double best = 1e30;

		for (size_t i = 0; i < m_Bodies.size(); i++)
		{
			double surface = DrawnRadius(i);

			// The tallest thing that body can put in the way, which is what
			// "above the ground" has to mean when you are not on it.
			auto it = m_Planets.find(i);

			if (it != m_Planets.end())
				surface += it->second.ReliefReach();

			best = std::min(best, glm::length(BodyScene(i) - ship) - surface);
		}

		return std::max(best, 1.0);
	}

	// **A basis, not a yaw and a pitch.**
	//
	// Yaw/pitch is measured against a fixed world up, and there is no such
	// thing here: level flight over Earth's south pole is upside down in +Y
	// terms. So the camera carries `forward` and `up` as world vectors, the
	// mouse rotates forward about them, and `up` is drawn toward the local
	// vertical near a body and toward +Y far from one. That levelling is what
	// makes entering an atmosphere feel like entering an atmosphere: the
	// horizon rolls into place instead of the world tipping over.
	//
	// It also makes landing continuous, because the surface controller ends up
	// holding the same two vectors.
	void UpdateFlight(float dt)
	{
		float yaw, pitch;
		LookDelta(dt, yaw, pitch);

		// Rotating about up by a negative angle turns to the right: the matrix
		// about +Y takes +x toward -z, and +z is the camera's right.
		m_Forward = RotateAbout(m_Forward, m_Up, glm::radians(-yaw));

		glm::vec3 right = glm::normalize(glm::cross(m_Forward, m_Up));
		glm::vec3 pitched = RotateAbout(m_Forward, right, glm::radians(pitch));

		// Stop just short of straight up, where the next frame's levelling
		// would flip the whole view over.
		if (std::abs(glm::dot(glm::normalize(pitched), m_Up)) < 0.996f)
			m_Forward = glm::normalize(pitched);

		// Levelling, weighted by how much of the sky the frame body fills.
		// Four radii out it is off entirely and +Y takes over, which is what
		// deep space wants -- there is no local vertical out there.
		double distance = glm::length(m_Local);
		double radius = DrawnRadius(m_Frame);

		float weight = (float)glm::clamp(
			(4.0 * radius - distance) / (3.0 * radius), 0.0, 1.0);

		glm::vec3 vertical = distance > 1e-6
			? glm::vec3(m_Local / distance) : m_Up;

		glm::vec3 wanted = glm::normalize(
			glm::mix(glm::vec3(0.0f, 1.0f, 0.0f), vertical, weight));

		m_Up = glm::normalize(glm::mix(m_Up, wanted,
			glm::clamp(m_LevelRate * dt, 0.0f, 1.0f)));

		Reorthogonalise();

		// **Speed proportional to altitude**, which is the only way one control
		// serves both a 300 km crossing and a landing approach. On the ground
		// it floors at walking pace; at Neptune's distance from anything it is
		// tens of kilometres a second.
		right = glm::normalize(glm::cross(m_Forward, m_Up));

		glm::dvec3 move(0.0);

		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) move += glm::dvec3(m_Forward);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) move -= glm::dvec3(m_Forward);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) move += glm::dvec3(right);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) move -= glm::dvec3(right);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_SPACE)) move += glm::dvec3(m_Up);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_CONTROL)) move -= glm::dvec3(m_Up);

		double speed = glm::clamp(m_SpeedPerMetre * AltitudeAboveAnything(),
			(double)m_MinSpeed, (double)m_MaxSpeed);

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_SHIFT))
			speed *= 5.0;

		m_FlightSpeed = speed;

		if (glm::dot(move, move) > 0.0)
			m_Local += glm::normalize(move) * (speed * (double)dt);
	}

	// --- Landing and taking off ---------------------------------------------
	//
	// **No teleport left in either direction.** Landing builds a physics world
	// around wherever the ship already is and drops a capsule into it; taking
	// off deletes the capsule and hands the same position and the same basis
	// back to the flight controller. Step out at 300 m and you fall 300 m.

	// **Metres the ground under the ship stands above that planet's sea**,
	// or false where there is no sea to be over.
	//
	// Landing is where you already are and not where it would be nice to be --
	// that is the whole reason there is no teleport left in it -- so the only
	// thing to be done about a player hovering over an ocean is to say so
	// before they press the key. The sea is a shell drawn over the ground
	// rather than a surface you stand on, so what happens otherwise is that
	// the eye ends up under it, and a screenshot of that looks like a broken
	// water shader rather than like drowning.
	bool GroundUnderShip(double& aboveSea) const
	{
		auto it = m_Planets.find(m_Frame);

		if (m_Frame == 0 || it == m_Planets.end() || !it->second.Get().HasOcean)
			return false;

		double distance = glm::length(m_Local);

		if (distance < 1e-6)
			return false;

		const VoxelPlanet::Settings& settings = it->second.Get();

		glm::vec3 direction = glm::vec3(ToFixed(m_Frame, m_Local / distance));

		aboveSea = (double)settings.Radius + (double)it->second.Relief(direction)
			- (double)settings.OceanRadius;

		return true;
	}

	void Land()
	{
		if (m_Frame == 0)
		{
			EGSS_WARN("nothing to land on out here");
			return;
		}

		VoxelPlanet& planet = PlanetFor(m_Frame);

		double distance = glm::length(m_Local);

		if (distance > (double)planet.Get().Radius * 2.5)
		{
			EGSS_WARN("{0} is {1:.0f} m away -- fly closer first",
				m_Bodies[m_Frame].Name, distance - planet.Get().Radius);
			return;
		}

		double aboveSea = 0.0;

		// Not refused: a seabed is a place, and somebody may want to look at
		// one. Only said, because it is not what the key was pressed for.
		if (GroundUnderShip(aboveSea) && aboveSea < 0.0)
			EGSS_WARN("landing under {0:.0f} m of water", -aboveSea);

		m_Walking = true;
		m_Ground = (int)m_Frame;

		// Everything below the takeoff line is in the planet's own turning
		// frame, so the ship's inertial position and heading come across once
		// here and go back once in `TakeOff`.
		glm::dvec3 fixed = ToFixed(m_Frame, m_Local);
		glm::vec3 forward = glm::vec3(ToFixed(m_Frame, glm::dvec3(m_Forward)));

		BuildSurfaceWorld(m_Frame, planet, fixed, forward);

		// **The ground arrives before the first frame, not during the first
		// four hundred.**
		PrefillSite(planet);

		// **And the rest of the world.** The chunks stop at the load radius;
		// this carries the same relief from there to the horizon. Built with
		// the site, because it is a disc around the site -- walking off it is
		// what rebuilds it, the same way the water works.
		BuildHorizon(planet);

		// The local water is built from the site, like the physics world, and
		// spans the streamed region -- past that there is no ground for it to
		// stand on and the planet-wide sea takes over.
		//
		// After the prefill, and that ordering is worth a quarter of a minute.
		// Water here is a consequence of the shape of the ground, so every
		// time materially more ground arrives the answer changes and it is
		// rebuilt -- which while the region was streaming in meant fifteen
		// full rebuilds, measured at **24.6 s of a 32 s landing in Debug**,
		// against 8.5 s for all the streaming that provoked them. Built once,
		// on ground that is already finished, it is built once.
		RebuildWater(planet);

		// The basis is already right -- flight was levelled to the local
		// vertical on the way in. All that is needed is to say it in the
		// tangent frame the walk uses, so the first step does not swing.
		glm::vec3 up = glm::vec3(glm::normalize(fixed));
		glm::vec3 east, north;
		TangentFrame(up, east, north);

		m_SurfacePitch = glm::degrees(std::asin(
			glm::clamp(glm::dot(forward, up), -1.0f, 1.0f)));

		glm::vec3 heading = forward - up * glm::dot(forward, up);

		m_SurfaceYaw = glm::dot(heading, heading) > 1e-8f
			? glm::degrees(std::atan2(glm::dot(heading, east), glm::dot(heading, north)))
			: 0.0f;

		m_Grounded = false;
	}

	// How far from the hull you can be and still climb in.
	static constexpr float s_BoardingReach = 6.0f;

	float DistanceToShip() const
	{
		if (!m_HasShip)
			return 1e30f;

		return glm::length(m_World.GetBody(m_Ship).Position
			- m_World.GetBody(m_Player).Position);
	}

	void TakeOff()
	{
		// **You leave in the ship, so you have to be at the ship.** This is
		// the whole of the loop the demo was missing: land, get out, walk,
		// dig, and then find your way back to the one object that can take you
		// off again.
		if (m_HasShip && DistanceToShip() > s_BoardingReach)
		{
			EGSS_WARN("the ship is {0:.0f} m away -- walk back to it",
				DistanceToShip());
			return;
		}

		m_Walking = false;

		// Where the *ship* is, not where the feet are: you are in it now.
		glm::dvec3 hull = SiteFixed(m_HasShip
			? m_World.GetBody(m_Ship).Position
			: m_World.GetBody(m_Player).Position);

		glm::dvec3 eye = hull + glm::normalize(hull) * (double)m_EyeHeight;

		m_Local = ToScene((size_t)m_Ground, eye);

		m_World.Clear();
		m_WaterMesh.reset();
		m_HasShip = false;
		m_Ground = -1;
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& key)
		{
			if (key.GetRepeatCount() > 0 || key.GetKeyCode() != EGSS_KEY_L)
				return false;

			m_Walking ? TakeOff() : Land();

			return true;
		});
	}

	// --- Surface physics ----------------------------------------------------

	// **Gravity as a force per body, not a world vector.**
	//
	// `PhysicsWorld3D::Gravity` is one direction for the whole world, which is
	// right for a room and wrong for a planet: on a sphere, down is a different
	// direction for every body and its strength falls off with height. So the
	// world's own gravity is switched off and each body is pulled toward the
	// centre every step instead.
	//
	// `GM/r^2`, not a constant: at the surface it is the planet's real surface
	// gravity, and 100 m up on a 360 m planet it is 40% weaker. That is what
	// makes a thrown rock arc the way it does here rather than the way it would
	// on a flat world with the same g.
	// **Rebuilt as the ground arrives.** A landing happens before the region
	// has streamed, so the first water is built over a field that is mostly
	// empty and falls back to the generator for it. Every time materially more
	// terrain exists, the question is worth asking again -- and once the
	// region is full this stops firing by itself.
	void RebuildWater(VoxelPlanet& planet)
	{

		m_Water.Build(planet, m_SiteLattice, m_SiteFixed,
			m_LoadRadius * 0.95f * (planet.Get().VoxelSize / 1.5f));

		m_WaterChunks = planet.MeshedChunks();

		m_Water.Report();

		RebuildWaterMesh();
	}

	// **Everything inside the load radius, before anything is drawn.**
	//
	// `StreamAround` is budgeted because a frame has 16.7 ms in it and
	// generating a chunk costs a fraction of that; arriving is the one moment
	// where there is no frame to protect, because nothing has been shown yet.
	// So it is called with a budget nothing can exhaust, repeatedly, until a
	// pass changes nothing.
	//
	// **Repeatedly, because one pass cannot finish.** A chunk is only meshed
	// once its seven high neighbours are filled -- the mesher reads one plane
	// into each of them, and meshing against a chunk that does not exist yet
	// closes the surface against empty space and stands a wall up out of the
	// ground. So the meshes of the outermost filled shell always lag their
	// fills by a pass, and the loop is what lets them catch up.
	void PrefillSite(VoxelPlanet& planet)
	{
		if (m_Ground < 0)
			return;

		float scale = planet.Get().VoxelSize / 1.5f;

		planet.SetLod(m_Lod, m_LodNear * scale, m_LodFar * scale, 16.0f * scale);

		glm::dvec3 focus = TerrainFocus((size_t)m_Ground);

		auto before = std::chrono::high_resolution_clock::now();

		// Reading and writing the cache is only on while this runs: see the
		// note on `OpenSiteCache` for why a walk is not cached.
		planet.SetPrefilling(true);

		int passes = 0;

		for (; passes < 64; passes++)
		{
			size_t filled = planet.FilledChunks();
			size_t meshed = planet.MeshedChunks();

			planet.StreamAround(focus, m_LoadRadius * scale, 1.0e9f);

			if (planet.FilledChunks() == filled && planet.MeshedChunks() == meshed)
				break;
		}

		// **And what the height-field fill costs, measured rather than
		// assumed.** `FillChunkFast` interpolates the relief over a chunk
		// instead of evaluating it per voxel, which is worth a factor of ten
		// on the whole landing -- and is an approximation, so the error goes
		// in the log beside the timing every time a site is prepared.
		planet.ReportReliefError(focus);

		// And what the whole fill path costs end to end, against the one
		// line of arithmetic that says where the ground is. The patch
		// error above is one contributor to this number; sampling the
		// density at a float lattice position used to be the other, and
		// at `--earth-radius 6371000` it was much the larger of the two.
		planet.ReportSurfaceError(focus, m_LoadRadius * scale * 0.8f);

		planet.SetPrefilling(false);

		EGSS_TRACE("  site cache: {0} chunks read, {1} written",
			planet.CacheHits(), planet.CacheWrites());

		EGSS_TRACE("Landing site: {0} chunks filled, {1} meshed, {2} passes, {3:.0f} ms",
			planet.FilledChunks(), planet.MeshedChunks(), passes + 1,
			std::chrono::duration<double, std::milli>(
				std::chrono::high_resolution_clock::now() - before).count());
	}

	void BuildHorizon(const VoxelPlanet& planet)
	{
		if (m_Ground < 0)
			return;

		float scale = planet.Get().VoxelSize / 1.5f;

		// Inside the streamed edge, so there is no gap between the two: the
		// overlap is where the droop keeps the chunks in front.
		float inner = m_LoadRadius * scale * 0.85f;

		// **Fourteen kilometres, and the number is a horizon.** Ground at
		// height `h` clears the horizon from `sqrt(2 R h)` away, so the 316 m
		// the relief reaches on this planet is visible from 12.6 km. Past that
		// there is nothing left to draw that is not below the curve.
		float outer = glm::min(
			std::sqrt(2.0f * planet.Get().Radius * glm::max(planet.ReliefReach(), 1.0f))
				* 1.1f,
			planet.Get().Radius * 0.25f);

		m_Horizon.Build(planet, m_HorizonSite = TerrainFocus((size_t)m_Ground),
			inner, glm::max(outer, inner * 4.0f));

		// Said once a landing, not once every hundred metres of walking: the
		// numbers are a property of the planet and the load radius, and
		// repeating them down the log buries everything else.
		if (!m_HorizonReported)
		{
			m_Horizon.Report();
			m_HorizonReported = true;
		}
	}

	void RebuildWaterMesh()
	{
		Egss::MeshData surface;
		m_Water.BuildMesh(surface);

		m_WaterMesh = surface.Indices.empty()
			? nullptr
			: std::make_shared<Egss::Mesh>(surface, "SurfaceWater");
	}

	// A spade, reaching from the eye along the view.
	//
	// Everything is in the landing site's frame -- the same one the physics is
	// in -- so the ray, the hit and the edit are all small numbers about a
	// lattice point, and none of it depends on how large the planet is.
	void Dig(VoxelPlanet& planet, bool add)
	{
		const Egss::RigidBody3D& player = m_World.GetBody(m_Player);

		glm::vec3 eye = player.Position
			+ glm::vec3(glm::normalize(SiteFixed(player.Position))) * m_EyeHeight;

		glm::vec3 forward = glm::vec3(ToFixed((size_t)m_Ground, glm::dvec3(m_Forward)));

		glm::vec3 point, normal;

		if (!planet.RayToSurface(eye, forward, m_SiteLattice, m_DigReach, point, normal))
			return;

		// Stepped out along the normal when adding, so a new blob sits *on*
		// the surface rather than half inside it.
		glm::vec3 at = add ? point + normal * (m_DigRadius * 0.5f) : point;

		if (planet.Dig(at, m_SiteLattice, m_DigRadius, add) == 0)
			return;

		m_Edits++;

		// **And then ask the water what that changed.** Everything about where
		// water can be is a consequence of the shape of the ground, so the
		// only thing a dig has to do is say the ground moved.
		if (m_Water.Touch(planet, m_SiteLattice, at, m_DigRadius))
			RebuildWaterMesh();
	}

	// The landing-site frame, both ways. `SiteLocal` takes a place in the
	// planet's own frame into the physics world's; `SiteFixed` brings one
	// back. Both do the arithmetic in double and hand back the small half.
	glm::vec3 SiteLocal(const glm::dvec3& fixed) const
	{
		return glm::vec3(fixed - m_SiteFixed);
	}

	glm::dvec3 SiteFixed(const glm::vec3& local) const
	{
		return m_SiteFixed + glm::dvec3(local);
	}

	// --- Weather ------------------------------------------------------------
	//
	// **A moon is as far from the star as its planet is.** Io's own orbit is
	// 0.0028 AU across, which is inside the width of the line on any plot of
	// where Jupiter is. Walking up to the body that orbits the star directly
	// is both simpler and more nearly true than summing the chain.
	double StarDistanceAu(size_t index) const
	{
		size_t at = index;

		while (m_Bodies[at].Parent > 0)
			at = (size_t)m_Bodies[at].Parent;

		return m_Bodies[at].SemiMajorAu;
	}

	// Fills in everything `Climate::At` needs about one point on one body.
	// `sceneDirection` is the outward unit normal in scene coordinates;
	// `altitude` is metres above that body's sea level.
	Climate::Site SiteWeather(size_t index, const glm::dvec3& sceneDirection,
		float altitude) const
	{
		Climate::Site site;

		const Body& body = m_Bodies[index];

		site.StarDistanceAu = (float)StarDistanceAu(index);
		site.Albedo = body.BondAlbedo;
		site.AirColumn = body.AtmosphereFraction * body.AtmosphereDensity;
		site.Gravity = (float)RealSurfaceGravity(index);
		site.RotationHours = (float)body.RotationHours;
		site.Altitude = altitude;

		glm::dvec3 up = glm::normalize(sceneDirection);

		// Both of these are in scene coordinates already, so the axial tilt
		// and the time of day are in them without either being mentioned
		// here: the sun moves because the body turns, and the seasons happen
		// because the axis it turns about is not the orbit's.
		site.CosZenith = (float)glm::dot(up, glm::dvec3(SunDirection(index)));

		site.LatitudeDegrees = glm::degrees((float)std::asin(
			glm::clamp(glm::dot(up, SpinAxis(index)), -1.0, 1.0)));

		// Moisture is a property of the ground, so it is asked for in the
		// frame the ground is fixed in rather than the one it is drawn in.
		auto it = m_Planets.find(index);

		site.Moisture = it != m_Planets.end()
			? it->second.MoistureAt(glm::vec3(ToFixed(index, up)))
			: 0.0f;

		return site;
	}

	// **Every line of this is derived, and the panel says which.** A number
	// on a HUD that someone typed into a slider teaches nothing; the same
	// number with the flux it came from beside it is the model showing its
	// working. So the insolation is printed next to the temperature it
	// produced, and the equilibrium next to the greenhouse that is the
	// difference between it and the ground.
	void WeatherPanel()
	{
		if (!m_HasWeather)
		{
			ImGui::TextDisabled("no weather here -- vacuum");
			return;
		}

		const Climate::Weather& w = m_Weather;
		const Climate::Site& site = m_WeatherSite;

		// Kelvin is the physics and Celsius is what a person reads.
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f),
			"%.1f C  (%.1f K)", w.Temperature - 273.15f, w.Temperature);

		bool day = site.CosZenith > 0.0f;

		ImGui::Text("  sun %.0f deg %s the horizon, %s",
			glm::degrees(std::asin(glm::clamp(site.CosZenith, -1.0f, 1.0f))),
			day ? "above" : "below", day ? "day" : "night");

		ImGui::Text("  %.0f W/m^2 in, %.0f W/m^2 arriving at the top",
			w.Insolation, w.SolarConstant);

		ImGui::Text("  equilibrium %.1f K, greenhouse +%.1f K",
			w.Equilibrium, w.Greenhouse);

		ImGui::Text("  lapse %.2f K/km over %.0f m of altitude",
			w.LapseRate, site.Altitude);

		ImGui::Text("  %.1f kPa, scale height %.0f m, air %.3f kg/m^3",
			w.Pressure * 0.001f, w.ScaleHeight, w.AirDensity);

		// Which way the wind is going, said as a bearing, because "north-east
		// at 4 m/s" is a sentence and a vector is not.
		if (w.WindSpeed < 0.05f)
		{
			ImGui::Text("  calm");
		}
		else
		{
			static const char* points[8] = {
				"N", "NE", "E", "SE", "S", "SW", "W", "NW" };

			// Bearing of the direction the wind is blowing *towards*,
			// clockwise from north. `Wind.x` is east and `Wind.y` is north.
			float bearing = glm::degrees(std::atan2(w.Wind.x, w.Wind.y));

			if (bearing < 0.0f)
				bearing += 360.0f;

			int point = ((int)std::lround(bearing / 45.0f)) % 8;

			ImGui::Text("  wind %.1f m/s toward %s (%.0f deg), lat %.1f",
				w.WindSpeed, points[point], bearing, site.LatitudeDegrees);
		}

		ImGui::Text("  moisture %.2f, albedo %.3f, air column %.4f",
			site.Moisture, site.Albedo, site.AirColumn);
	}

	// The weather where the camera is, recomputed every fixed step so a
	// replay sees the same numbers. Airless bodies and deep space both come
	// back as the default `Weather`, which is all zeros and no wind.
	void StepWeather()
	{
		m_Weather = Climate::Weather();
		m_HasWeather = false;

		size_t index = m_Walking ? (size_t)m_Ground : m_Frame;

		if (index == 0 || index >= m_Bodies.size() || m_Pocket.InPocket())
			return;

		glm::dvec3 fixedAt = m_Walking
			? SiteFixed(m_World.GetBody(m_Player).Position)
			: ToFixed(index, m_Local);

		double distance = glm::length(fixedAt);

		if (distance < 1.0)
			return;

		glm::dvec3 up = fixedAt / distance;

		auto it = m_Planets.find(index);

		double sea = it != m_Planets.end()
			? (double)it->second.Get().OceanRadius
			: DrawnRadius(index);

		m_WeatherSite = SiteWeather(index, ToScene(index, up),
			(float)(distance - sea));

		m_Weather = Climate::At(m_WeatherSite);
		m_HasWeather = true;

	}

	void ApplyGravity()
	{
		// A pocket dimension is not on any planet, so it does not get a
		// radial pull toward one -- a fixed "down", applied to the player
		// only. Everything else in the room is a static collider and does
		// not want gravity at all.
		if (m_Pocket.InPocket())
		{
			Egss::RigidBody3D& player = m_World.GetBody(m_Player);
			float mass = 1.0f / player.InverseMass;

			m_World.ApplyForce(m_Player, -m_Pocket.Up() * 9.81f * mass);
			return;
		}

		double gm = LocalGm((size_t)m_Ground);

		for (Egss::RigidBody3D& body : m_World.GetBodies())
		{
			if (body.Type != Egss::BodyType::Dynamic || body.InverseMass <= 0.0f)
				continue;

			// **Down is toward the planet's centre, which is not the origin of
			// the frame this body is in.** The site's own offset has to come
			// back before the direction means anything -- and in double,
			// because it is a planet radius long.
			glm::dvec3 fixed = SiteFixed(body.Position);
			double distance = glm::length(fixed);

			if (distance < 1e-3)
				continue;

			glm::vec3 toCentre = glm::vec3(-fixed / distance);

			float acceleration = (float)(gm / (distance * distance));
			float mass = 1.0f / body.InverseMass;

			m_World.ApplyForce(BodyHandleOf(body), toCentre * acceleration * mass);
		}
	}

	// **The one place water pushes back.** Until now the local water sheet
	// was purely a picture: the player's collider reads the terrain's SDF,
	// which knows nothing about the sheet drawn over it, so walking into the
	// sea did nothing but hide your feet. This is what makes it water rather
	// than a tinted pane of glass -- a spring toward the surface, and drag
	// that opposes whichever way you are already moving through it.
	void ApplyBuoyancy()
	{
		m_Submersion = 0.0f;
		m_EyeUnderwater = false;

		if (m_Ground < 0 || !m_Water.Valid())
			return;

		Egss::RigidBody3D& player = m_World.GetBody(m_Player);

		float level;
		if (!m_Water.LevelNear(player.Position, level))
			return;

		glm::dvec3 fixed = SiteFixed(player.Position);
		double distance = glm::length(fixed);

		if (distance < 1e-3)
			return;

		glm::vec3 up = glm::vec3(fixed / distance);

		// How far the water stands above the capsule's own feet, as a
		// fraction of its full height -- 0 dry, 1 covered to the top.
		float depth = (float)((double)level - (distance - (double)m_PlayerHalfHeight));

		m_Submersion = glm::clamp(depth / (2.0f * m_PlayerHalfHeight), 0.0f, 1.0f);
		m_EyeUnderwater = (double)level > distance + (double)m_EyeHeight;

		if (m_Submersion <= 0.0f)
			return;

		float mass = 1.0f / player.InverseMass;

		m_World.ApplyForce(BodyHandleOf(player),
			up * (m_Submersion * mass * 9.81f * m_BuoyancyStrength)
			- player.Velocity * (mass * m_WaterDrag * m_Submersion));
	}

	Egss::PhysicsWorld3D::BodyHandle BodyHandleOf(const Egss::RigidBody3D& body) const
	{
		const std::vector<Egss::RigidBody3D>& bodies = m_World.GetBodies();

		return (Egss::PhysicsWorld3D::BodyHandle)(&body - bodies.data());
	}

	// A stable pair of tangents at a point on a sphere. The reference axis is
	// whichever world axis is least aligned with up, so the frame never
	// degenerates as you walk over a pole.
	static void TangentFrame(const glm::vec3& up, glm::vec3& east, glm::vec3& north)
	{
		glm::vec3 reference = std::abs(up.y) < 0.9f
			? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

		east = glm::normalize(glm::cross(reference, up));
		north = glm::cross(up, east);
	}

	// **The surface world is not in the planet's frame; it is in the landing
	// site's.**
	//
	// A body's position is a float, and on a planet drawn at its own radius a
	// float carries half a metre -- so a player standing in the planet's frame
	// would move in half-metre jumps and the field it is standing on would be
	// sampled at the same point for a metre's walking. Everything here is
	// therefore measured from the lattice point nearest where the ship came
	// down, which is what `RigidBody3D::VoxelOrigin` exists to say. Positions
	// stay in the hundreds of metres, which is a millimetre of precision.
	//
	// A lattice *point*, not just any point, because the collider's field
	// lookup adds it back as an integer.
	void BuildSurfaceWorld(size_t index, VoxelPlanet& planet, const glm::dvec3& at,
		const glm::vec3& facing)
	{
		m_World.Clear();

		m_SiteLattice = planet.LatticeNear(at);
		m_SiteFixed = planet.LatticePosition(m_SiteLattice);

		// No world gravity at all: every pull here is radial and applied per
		// body. Leaving the default -9.81 Y in place would add a second,
		// invisible gravity pointing at the planet's north pole.
		m_World.Gravity = glm::vec3(0.0f);

		Egss::RigidBody3D ground =
			Egss::RigidBody3D::MakeSdf(glm::vec3(0.0f), planet.Field(), m_SiteLattice);
		ground.Friction = 0.8f;
		ground.Restitution = 0.0f;

		m_World.AddBody(ground);

		glm::vec3 up = glm::vec3(glm::normalize(at));

		// **The ship is a body, and it stays where it came down.**
		//
		// It used to be nothing at all: `L` toggled between a physics capsule
		// and a camera, and the vehicle existed only as the fact that you
		// could leave. Making it an object is what turns landing into a place
		// you have to get back to -- and it costs almost nothing, because the
		// player was already a rigid body and gravity here is applied per body
		// rather than as a world vector.
		//
		// A capsule with the hull drawn round it. A box would be the honest
		// shape and the narrowphase only tests a box's corners against a
		// distance field, so one resting on rough ground sinks a corner or
		// jitters; a capsule is sampled along its segment. Angular damping
		// near one keeps it from slowly toppling on a slope, which is a
		// lander with legs behaving like a lander with legs rather than a
		// physical claim about its inertia.
		Egss::RigidBody3D ship = Egss::RigidBody3D::MakeCapsule(
			SiteLocal(at) + up * 1.4f, 1.3f, 1.1f, 4200.0f);

		ship.Friction = 0.9f;
		ship.Restitution = 0.0f;
		ship.LinearDamping = 0.0f;
		ship.AngularDamping = 0.9f;
		ship.Orientation = UprightAt(up);

		m_Ship = m_World.AddBody(ship);
		m_HasShip = true;

		// **Out of the back of it, facing it.** Beside it works and puts the
		// thing you have to walk back to off the edge of the screen the moment
		// you arrive, which is a poor way to introduce it. The heading is the
		// one the ship came in on, flattened into the tangent plane, so
		// stepping out backward along it leaves the hull dead ahead.
		glm::vec3 along = facing - up * glm::dot(facing, up);

		along = glm::length(along) > 1e-3f
			? glm::normalize(along) : glm::vec3(0.0f);

		Egss::RigidBody3D player = Egss::RigidBody3D::MakeCapsule(
			SiteLocal(at) - up * m_EyeHeight - along * 8.0f, 0.4f, 0.9f, 78.0f);

		player.Friction = 0.6f;
		player.Restitution = 0.0f;
		player.Orientation = UprightAt(up);

		m_Player = m_World.AddBody(player);

		// **The portal, ahead and off to one side of the walk-out line.** A
		// pure lateral offset (tried first) put the doorway ~68 degrees off
		// the player's default forward -- outside even a 65-97 degree
		// frustum, so "visible on arrival" was never true. The player faces
		// back toward the ship (+along), so the offset needs an `along`
		// component too, biased enough that the doorway lands inside that
		// cone while the `lateral` component still keeps it visibly off the
		// direct line to the hull and clear of the 11-27 m rock annulus.
		glm::vec3 lateral = glm::length(along) > 1e-3f
			? glm::normalize(glm::cross(along, up)) : glm::vec3(1.0f, 0.0f, 0.0f);

		m_Pocket.Place(m_World, SiteLocal(at) + lateral * 6.0f + along * 4.0f, lateral, up);

		// A handful of rocks to watch fall, dropped around the landing site --
		// gravity you cannot see acting on anything is gravity you cannot check.
		unsigned int random = 90210u;
		auto next = [&random]()
		{
			random ^= random << 13; random ^= random >> 17; random ^= random << 5;
			return (float)(random >> 8) * (1.0f / 16777216.0f);
		};

		glm::vec3 east, north;
		TangentFrame(up, east, north);

		float surface = planet.SurfaceRadius(up);

		for (int i = 0; i < 12; i++)
		{
			// **An annulus, not a square.** They used to be scattered over
			// forty metres centred on the touchdown point, which is also
			// where the ship stands and where the player steps out -- so once
			// the player stopped starting *at* the landing point, one of them
			// was 2.4 m from the eye and filled a quarter of the screen with
			// boulder. Nothing was wrong with it; it was simply in the way.
			float turn = next() * 6.2831853f;
			float away = 11.0f + next() * 16.0f;

			glm::vec3 offset = (east * std::cos(turn) + north * std::sin(turn))
				* away;

			// The direction is a unit vector and survives anything; the radius
			// it is multiplied by is the planet's, and does not. Composed in
			// double and handed to the body in the site's frame.
			glm::vec3 where = glm::vec3(glm::normalize(
				glm::dvec3(up) * (double)surface + glm::dvec3(offset)));

			double ground2 = (double)planet.SurfaceRadius(where);

			Egss::RigidBody3D rock = Egss::RigidBody3D::MakeSphere(
				SiteLocal(glm::dvec3(where)
					* (ground2 + 18.0 + (double)next() * 25.0)),
				0.6f + next() * 0.7f, 40.0f);

			rock.Friction = 0.7f;
			rock.Restitution = 0.12f;

			// **No drag: this is a vacuum.** `LinearDamping` defaults to 0.01,
			// a percent of velocity a second, which is a reasonable stand-in
			// for air in a room and wrong here. It is invisible in a short fall
			// and ruinous over an orbit -- with the default left on, a test
			// orbit at 600 m spiralled into the ground and a launch above
			// escape velocity fell back.
			rock.LinearDamping = 0.0f;
			rock.AngularDamping = 0.0f;

			m_World.AddBody(rock);
		}
	}

	// The rotation that takes the body's own +Y onto local up, so a capsule
	// stands on the ground instead of lying across it.
	static glm::quat UprightAt(const glm::vec3& up)
	{
		glm::vec3 from(0.0f, 1.0f, 0.0f);
		float dot = glm::clamp(glm::dot(from, up), -1.0f, 1.0f);

		if (dot > 0.9999f)
			return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		if (dot < -0.9999f)
			return glm::angleAxis(3.14159265f, glm::vec3(1.0f, 0.0f, 0.0f));

		glm::vec3 axis = glm::normalize(glm::cross(from, up));

		return glm::angleAxis(std::acos(dot), axis);
	}

	// Walking on a sphere.
	//
	// **Nothing here uses a world up.** The local up is `normalize(position)`,
	// forward and right are built in the tangent plane at wherever you are
	// standing, and the camera is given that basis explicitly -- a yaw/pitch
	// camera measured against +Y renders the ground up the side of the screen
	// as soon as you leave the north pole, which is exactly what the first
	// version did.
	void UpdateSurface(float dt)
	{
		VoxelPlanet& planet = m_Planets[(size_t)m_Ground];

		float yaw, pitch;
		LookDelta(dt, yaw, pitch);

		// **Plus, not minus.** Yaw is measured from north toward east, and
		// east is the camera's right -- so turning right is a positive yaw.
		// The first version subtracted, which inverted the mouse on the
		// ground while flight (which rotates a basis rather than an angle)
		// had it the right way round.
		m_SurfaceYaw += yaw;
		m_SurfacePitch = glm::clamp(m_SurfacePitch + pitch, -85.0f, 85.0f);

		// All of this is in the planet's own frame, which is the frame the
		// terrain and the physics are in. It reaches scene coordinates once,
		// at the bottom, through the spin. Inside the pocket dimension there
		// is no planet to be radial toward, so "up" is whatever fixed
		// direction the room was placed with instead.
		glm::vec3 up = m_Pocket.InPocket() ? m_Pocket.Up() : glm::vec3(glm::normalize(
			SiteFixed(m_World.GetBody(m_Player).Position)));
		glm::vec3 east, north;
		TangentFrame(up, east, north);

		float bearing = glm::radians(m_SurfaceYaw);
		glm::vec3 heading = north * std::cos(bearing) + east * std::sin(bearing);

		glm::vec3 move(0.0f);

		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) move += heading;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) move -= heading;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) move += glm::cross(heading, up);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) move -= glm::cross(heading, up);

		float speed = Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_SHIFT)
			? m_WalkSpeed * 2.5f : m_WalkSpeed;

		Egss::RigidBody3D& body = m_World.GetBody(m_Player);

		// **Steer the tangential velocity, leave the radial one alone.** The
		// player is a rigid body: falling, landing and being knocked about are
		// the solver's business, and walking is only a claim about the
		// component of velocity across the ground. Overwriting the whole vector
		// would cancel gravity every step and the player would never fall.
		glm::vec3 velocity = body.Velocity;
		float radial = glm::dot(velocity, up);
		glm::vec3 tangential = velocity - up * radial;

		glm::vec3 wanted = glm::dot(move, move) > 0.0f
			? glm::normalize(move) * speed : glm::vec3(0.0f);

		// Chased rather than snapped to, so the ground still has a say: a slope
		// that is too steep to climb still slows you down.
		float responsiveness = m_Grounded ? 12.0f : 2.0f;
		tangential += (wanted - tangential) * glm::clamp(responsiveness * dt, 0.0f, 1.0f);

		if (m_Grounded && Egss::Input::IsKeyPressed(EGSS_KEY_SPACE) && m_JumpCooldown <= 0.0f)
		{
			// Straight up is radially out, which on a planet is a different
			// direction at every point.
			radial = m_JumpSpeed;
			m_JumpCooldown = 0.35f;
		}

		m_JumpCooldown = std::max(0.0f, m_JumpCooldown - dt);

		body.Velocity = tangential + up * radial;
		body.Awake = true;

		// Upright against the local vertical. Set rather than solved: a capsule
		// left to the solver tips over on a slope and then rolls to the equator,
		// because on a sphere there is always a downhill.
		body.Orientation = UprightAt(up);
		body.AngularVelocity = glm::vec3(0.0f);

		// **And the ship, for the same reason and with more justification.**
		// A capsule left to the solver tips over on a slope and then rolls to
		// the equator, because on a sphere there is always a downhill -- which
		// is what the lander did, ending up on its side with its legs out
		// sideways. A vehicle that stands on legs stays on its legs. It is
		// still a dynamic body, so digging the ground out from under it drops
		// it; it just does not lie down.
		if (m_HasShip)
		{
			Egss::RigidBody3D& hull = m_World.GetBody(m_Ship);

			hull.Orientation = UprightAt(
				glm::vec3(glm::normalize(SiteFixed(hull.Position))));
			hull.AngularVelocity = glm::vec3(0.0f);
		}

		// **Digging happens on the fixed step, not in an event handler.** The
		// mouse is in the replay stream and events are not, so a session spent
		// digging records and replays as itself.
		bool cut = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT);
		bool fill = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_RIGHT);

		// One edit a press rather than one a step, or holding the button
		// hollows out the hillside in a second.
		if (cut && !m_WasCutting)
			Dig(planet, false);
		else if (fill && !m_WasFilling)
			Dig(planet, true);

		m_WasCutting = cut;
		m_WasFilling = fill;

		// **Half as much ground again, not an eighth more.**
		//
		// A rebuild is a Priority-Flood over sixteen thousand columns and
		// costs about 1.6 s in Debug, so the threshold is what decides whether
		// walking hitches. An eighth was right when arriving was the only time
		// the count moved in large steps; now that the site is prefilled, the
		// only thing that moves it is walking off the edge of it, and half
		// again is the point at which the answer has genuinely changed.
		// Both of these follow the player's distance from where they were
		// last built -- which the pocket dimension's own 2000 m offset would
		// otherwise read as "walked off the edge" every single step. The room
		// has neither water nor a horizon of its own to rebuild.
		if (!m_Pocket.InPocket())
		{
			if (m_Water.Valid()
				&& planet.MeshedChunks() > m_WaterChunks + m_WaterChunks / 2 + 64)
				RebuildWater(planet);

			// **And the horizon follows you.** It is a disc about the place it was
			// built, with a hole in the middle sized to sit just inside the
			// streamed region -- so walking off it opens a gap on one side and
			// puts the mesh over the chunks on the other. A hundred metres is
			// under a third of the hole's radius and costs 18,432 evaluations of
			// the relief, which is 8 ms here and is not in a frame's way often.
			if (m_Horizon.Valid()
				&& glm::length(TerrainFocus((size_t)m_Ground) - m_HorizonSite) > 100.0)
				BuildHorizon(planet);
		}

		ApplyGravity();
		ApplyBuoyancy();
		m_World.Step(dt);

		m_Pocket.UpdateCrossing(m_World.GetBody(m_Player));

		glm::dvec3 feet = SiteFixed(m_World.GetBody(m_Player).Position);

		// Grounded is measured against the terrain the physics is using, not a
		// contact flag, so it means the same thing as the ground query the
		// camera and the spawner use. Inside the pocket dimension "the
		// terrain" is a flat floor at a fixed height, not a radius the voxel
		// field can answer -- and "direction" is the room's own fixed up
		// rather than a fresh radial direction, for the same reason the
		// movement basis above used `m_Pocket.Up()` instead of recomputing
		// one: there is nothing here for a direction to be radial *toward*.
		glm::vec3 direction;

		if (m_Pocket.InPocket())
		{
			direction = m_Pocket.Up();
			m_Grounded = m_Pocket.HeightAboveFloor(m_World.GetBody(m_Player).Position)
				< m_PlayerHalfHeight + 0.35f;
		}
		else
		{
			direction = glm::vec3(glm::normalize(feet));
			float ground = planet.SurfaceRadius(direction);

			m_Grounded = (glm::length(feet) - (double)ground)
				< (double)(m_PlayerHalfHeight + 0.35f);
		}

		glm::vec3 forward = glm::normalize(
			heading * std::cos(glm::radians(m_SurfacePitch))
			+ direction * std::sin(glm::radians(m_SurfacePitch)));

		// Out into scene coordinates. Standing still on a turning planet is
		// motion out here, which is exactly why the Sun crosses the sky
		// without the light direction ever being touched.
		size_t index = (size_t)m_Ground;

		// **Kept in the planet's frame, and converted once, at the end of the
		// step.** Converting here used the spin angle as it stood *before* the
		// clock advanced, and everything else in the planet's frame is drawn
		// with the angle as it stands after -- so the camera sat one step of
		// rotation away from the body it belongs to. At 250 km with an hour to
		// the day the surface moves 436 m/s, and a sixtieth of that is 7.3 m:
		// the camera was seven metres from the player, and the ground was
		// seven metres from where the player was standing on it.
		//
		// Invisible until now, because *everything* in that frame moved
		// together and the player is not drawn. The lander is the first object
		// whose position on screen could be predicted from the physics, and it
		// came out in the wrong place by exactly one step of spin.
		m_EyeFixed = feet + glm::dvec3(direction) * (double)m_EyeHeight;
		m_UpFixed = direction;
		m_ForwardFixed = forward;
	}

	// --- The step -----------------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		if (m_Walking)
			UpdateSurface(step);
		else
			UpdateFlight(step);

		UpdateFrame();
		EnsurePlanets();

		StreamTerrain();

		// After the frame is chosen and before anything reads the weather.
		StepWeather();

		// **The clock has to follow the camera, not the other way round.**
		// A day on Earth is 1/365 of a year, so at a time scale that makes
		// orbits visible it passes in a fiftieth of a second and the surface
		// strobes between noon and midnight. Close to a body the rate comes
		// from `SecondsPerDay`; far from one it goes back to the orbital rate.
		//
		// Approached geometrically rather than switched, because a hard change
		// of time scale mid-flight looks like a bug in the integrator.
		double target = TargetYearsPerSecond();

		double blend = glm::clamp(3.0 * (double)step, 0.0, 1.0);
		m_YearsPerSecond = std::exp(std::log(std::max(m_YearsPerSecond, 1e-9))
			* (1.0 - blend) + std::log(std::max(target, 1e-9)) * blend);

		// Years of simulated time per second of wall clock, on a fixed step, so
		// the same run always integrates the same trajectory.
		double dt = (double)step * m_YearsPerSecond;

		// Substepping keeps the inner moons stable without slowing the whole
		// system down: Io goes round Jupiter in 0.0048 years, so one step of a
		// hundredth of a year would be twice its entire period. The count comes
		// from the shortest period in the table, not from a number that looked
		// safe.
		int substeps = glm::clamp((int)std::ceil(dt / m_MaxStep), 1, 512);
		double h = dt / (double)substeps;

		// **Periods are tracked per substep, not per frame.** Sampling the
		// angle once a frame is sampling at 1/60 s whatever the time scale, and
		// Io goes round Jupiter in half of one such step at a modest speed --
		// so it was measured aliased, and reported a period 3,164% long. The
		// substep is chosen from the shortest orbit in the system, which is
		// exactly the rate this needs.
		for (int i = 0; i < substeps; i++)
		{
			Integrate(h);
			m_Time += h;
			TrackPeriods(h);
		}

		// Once a call, not once a substep: this is a slow visual drift, not
		// an orbit, so it carries none of the aliasing risk TrackPeriods is
		// guarding against above.
		m_CloudDrift += dt * m_CloudDriftRate;

		CarryWithTheAir(dt);

		// The clock has moved; the surface camera is quoted in the planet's
		// own frame, so it reaches scene coordinates now rather than before.
		if (m_Walking && m_Ground >= 0)
		{
			size_t ground = (size_t)m_Ground;

			m_Local = ToScene(ground, m_EyeFixed);
			m_Up = glm::vec3(ToScene(ground, glm::dvec3(m_UpFixed)));
			m_Forward = glm::vec3(ToScene(ground, glm::dvec3(m_ForwardFixed)));
		}
	}

	// **A craft inside an atmosphere turns with the atmosphere.**
	//
	// The ship's position is held in the frame body's *inertial* frame, which
	// is right for a spacecraft: an orbit does not care which way the planet
	// under it is pointing. It is wrong for anything in air. Hovering over a
	// coastline, the coastline slid out from under the ship at a full turn a
	// day, which nothing that flies does -- a balloon over a city is over the
	// city an hour later.
	//
	// **This was not worth having until the day got longer.** At the sixty
	// seconds to the day this demo started with, the frame turned six degrees
	// a step: carrying the ship round at that rate is a fairground ride, not a
	// hover, and the note that used to be here rejecting co-rotation was right
	// at the time. At an hour to the day it is a tenth of a degree a step and
	// it reads as air.
	//
	// Weighted by how much air there is to be carried by, on the same
	// exponential the scattering uses -- full at the surface, a fiftieth at the
	// top of the shell -- so leaving the atmosphere hands you back to the
	// inertial frame without a boundary to cross. That weighting also disposes
	// of the old objection about the clock: the rate only speeds up outside six
	// radii, and there is no air out there to hold on to.
	// **How much of the planet's turn a craft at this radius is carried by.**
	//
	// Named and separate because it is the whole model, and because the number
	// that matters -- how fast the ground slides under a hover -- is
	// `(1 - AirCoupling) * spin rate * radius`, which is a thing to measure
	// rather than a thing to hope about.
	double AirCoupling(size_t index, double height) const
	{
		const Body& body = m_Bodies[index];

		if (body.AtmosphereFraction <= 0.0f)
			return 0.0;

		double radius = DrawnRadius(index);
		double top = radius * (1.0 + (double)body.AtmosphereFraction * (double)m_AirScale);
		double thickness = std::max(top - radius, 1e-6);

		double t = (height - radius) / thickness;

		if (t >= 1.0)
			return 0.0;
		if (t <= s_AirGripTop)
			return 1.0;

		// **Air does not co-rotate a little; it co-rotates.** This used to be
		// the same exponential the scattering uses -- full at the surface and
		// a fiftieth at the top of the shell -- on the theory that thin air
		// grips less. That is not what an atmosphere does: the whole of it
		// turns with the planet, near enough rigidly, and a balloon at 10 km
		// is over the same city as one at 100 m. The exponential meant a
		// *seventy metre* hover kept only 93% of the turn, which at 250 km and
		// an hour to the day is 30 m/s of ground sliding past -- the thing
		// that prompted this, reported twice.
		//
		// So: carried completely through the bulk of the shell, and released
		// over the top quarter of it, where a craft is leaving the air anyway.
		// The taper is only there so that crossing out is not a step.
		double u = (t - s_AirGripTop) / (1.0 - s_AirGripTop);

		return 1.0 - u * u * (3.0 - 2.0 * u);
	}

	void CarryWithTheAir(double years)
	{
		if (m_Walking || m_Frame == 0 || years <= 0.0)
			return;

		double share = AirCoupling(m_Frame, glm::length(m_Local));

		if (share <= 0.0)
			return;

		// Straight from the rate rather than by differencing two angles: the
		// absolute spin angle runs to tens of radians over a session and the
		// increment is a millionth of one, which is a subtraction worth not
		// doing.
		double turned = SpinRate(m_Frame) * years * share;
		glm::dvec3 axis = SpinAxis(m_Frame);

		m_Local = RotateAboutAxis(m_Local, axis, turned);

		// The view goes with it, or the ground slides sideways under a ship
		// that is holding station over it -- which is the bug, moved.
		m_Forward = glm::vec3(RotateAboutAxis(glm::dvec3(m_Forward), axis, turned));
		m_Up = glm::vec3(RotateAboutAxis(glm::dvec3(m_Up), axis, turned));
	}

	// Close to a body the clock comes from `SecondsPerDay`; far from one it is
	// the orbital rate. Six radii is where it changes hands, which is also
	// about where a planet stops filling the view.
	double TargetYearsPerSecond() const
	{
		if (m_Frame == 0 || glm::length(m_Local) > DrawnRadius(m_Frame) * 6.0)
			return m_OrbitalYearsPerSecond;

		double dayInYears = std::abs(m_Bodies[m_Frame].RotationHours) / (365.25 * 24.0);

		return dayInYears / (double)m_SecondsPerDay;
	}

	// **A planet is generated when it stops being a dot.** Sixty of its own
	// radii is about twelve pixels across at this field of view, which is
	// where the difference between a flat disc and a mapped one starts to
	// show -- and comfortably before the terrain streams, so the two costs do
	// not land in the same step. One a step, because building the colour map
	// is half a million evaluations of the same generator the ground is made
	// of.
	void EnsurePlanets()
	{
		glm::dvec3 ship = ShipScene();

		for (size_t i = 1; i < m_Bodies.size(); i++)
		{
			if (m_Planets.count(i))
				continue;

			if (glm::length(BodyScene(i) - ship) < DrawnRadius(i) * 60.0)
			{
				PlanetFor(i);
				return;
			}
		}
	}

	// Terrain exists only where somebody can see it: within half a radius of
	// the surface. Past that the drawn sphere is the planet, which is what it
	// was from orbit all along.
	void StreamTerrain()
	{
		size_t index = m_Walking ? (size_t)m_Ground : m_Frame;

		if (index == 0)
			return;

		double radius = DrawnRadius(index);
		double altitude = glm::length(m_Local) - radius;

		if (!m_Walking && altitude > radius * 0.5 + (double)m_LoadRadius)
		{
			// Evicting on the way out rather than keeping every planet's
			// chunks resident: a meshed shell is tens of megabytes and there
			// are sixteen bodies.
			auto it = m_Planets.find(index);

			if (it != m_Planets.end() && it->second.MeshedChunks() > 0)
				it->second.EvictBeyond(glm::vec3(0.0f), 0.0f);

			return;
		}

		VoxelPlanet& planet = PlanetFor(index);

		// Budgeted, like OpenWorld's streaming: meshing a chunk is marching
		// tetrahedra over 4,096 cells, and doing every newly qualifying chunk
		// in one step is exactly the spike a budget exists to prevent.
		float scale = planet.Get().VoxelSize / 1.5f;
		glm::dvec3 focus = TerrainFocus(index);

		// **The bands scale with the voxel, like the load radius does.** They
		// are quoted for Earth's 1.5 m voxels; Jupiter's are 17.5 m, and a
		// stride-2 lattice there is 35 m of ground -- the same fraction of the
		// same shell, which is what makes one pair of sliders serve sixteen
		// bodies.
		planet.SetLod(m_Lod, m_LodNear * scale, m_LodFar * scale, 16.0f * scale);

		// **A budget in milliseconds, except where a millisecond is not
		// allowed to matter.**
		//
		// Generating a chunk is 4,096 evaluations of thirteen octaves of
		// noise, and a landing needs about nine hundred of them. At a fixed
		// twelve a step that is 21 ms of the 16.7 ms frame in Release and
		// **150 ms in Debug**, which is not a slow frame, it is a freeze --
		// and the tell is that the GPU falls to idle while it happens, because
		// nothing is being submitted to it.
		//
		// So the count is derived from what the last step actually cost. It is
		// proportional and damped: aim at the target, and never move by more
		// than a factor of two in a step, so one expensive chunk does not
		// collapse the rate and one cheap one does not spike it.
		//
		// **Not under lockstep.** The ground collider reads the *field*, so
		// how much has streamed is part of the simulation, not just of the
		// picture -- a budget that depends on the wall clock would make a
		// replay depend on the machine. Lockstep keeps the fixed count, which
		// is what every capture and every recording runs under anyway.
		bool timed = !Egss::Application::Get().IsLockstep()
			&& !Egss::Input::IsPlayingBack();

		// **The allowance is a fraction, and it is banked.**
		//
		// One chunk is the atom of this work and it costs about four
		// milliseconds here, so an integer budget has a floor of four
		// milliseconds -- on a slower machine, or in Debug where it is seven
		// times that, no integer can hold the frame. Carrying the unspent
		// fraction from step to step lets the answer be "a chunk every third
		// step", which is what a machine that cannot afford one a step should
		// do: fill in more slowly, at frame rate, instead of stopping.
		float budget = (float)m_ChunksPerStep;

		if (timed)
		{
			m_StreamCredit = glm::min(m_StreamCredit + m_StreamAllowance, 8.0f);

			budget = std::floor(m_StreamCredit);

			if (budget < 1.0f)
			{
				m_StreamMs = 0.0;
				return;
			}

			m_StreamCredit -= budget;
		}

		auto before = std::chrono::high_resolution_clock::now();

		planet.StreamAround(focus, m_LoadRadius * scale, budget);

		m_StreamMs = std::chrono::duration<double, std::milli>(
			std::chrono::high_resolution_clock::now() - before).count();

		if (timed)
		{
			// Proportional and damped: aim at the target, and never move by
			// more than a factor of two in a step, so one expensive chunk does
			// not collapse the rate and one cheap one does not spike it.
			double want = (double)budget
				* (double)m_StreamTargetMs / glm::max(m_StreamMs, 0.05);

			double next = glm::clamp(want, budget * 0.5, budget * 2.0 + 1.0);

			m_StreamAllowance = glm::clamp((float)next, 0.02f, 96.0f);

		}
		planet.EvictBeyond(focus, m_LoadRadius * scale * 1.6f);

		// **And hand the voxels back further out.** Dropping a mesh leaves the
		// chunk it came from filled, which on a 360 m planet was every chunk
		// there is and cost nothing. At 250 km a walk accumulates every chunk
		// it has ever passed. Three times the load radius, so a chunk that
		// goes out of sight costs one re-mesh to come back and only one that
		// goes well out of range costs a re-fill.
		planet.ReleaseBeyond(focus, m_LoadRadius * scale * 3.0f);
	}

	// --- Drawing ------------------------------------------------------------

	void OnDemoUpdate(Egss::Timestep) override
	{
		// Reset is the caller's job here, and the panel reads the total back
		// at the end of the frame -- without this it counts every frame since
		// the demo started, which is exactly the shape of a plausible-looking
		// number that is not measuring what you think.
		Egss::Renderer::ResetStats();

		Egss::RenderCommand::SetClearColor({ 0.01f, 0.01f, 0.02f, 1.0f });
		Egss::RenderCommand::Clear();

		// **A floating origin.** Neptune is 302 km out, where a float's spacing
		// is 3.6 cm -- so a vertex transformed through a world-space matrix out
		// there lands on a 3.6 cm lattice, and two chunks that should meet do
		// not. Everything is therefore drawn relative to the camera, with the
		// subtraction done in double before the cast. The camera itself sits at
		// the origin and only its orientation is a matrix.
		glm::dvec3 origin = ShipScene();

		// **The near plane follows the altitude.** Depth precision at distance
		// z goes as z^2 / (near * 2^24), so a near plane of 15 cm -- which
		// standing on a planet needs -- leaves two bodies 10 km apart at 300 km
		// range indistinguishable in depth. Out where that matters the near
		// plane can be hundreds of metres, because nothing is close.
		// **The near plane follows the altitude; the far plane holds the
		// proxies.** Depth precision at distance z goes as `z^2 / (near *
		// 2^24)`, so standing on the ground with a 0.15 m near plane gives
		// 6 cm at the 400 m edge of the streamed terrain and ten metres at
		// five kilometres -- where there is nothing but the horizon sphere,
		// eight kilometres below the surface it stands in for. The far plane
		// only has to reach the compressed sky, which tops out under 4e6 m.
		float nearClip = (float)glm::clamp(AltitudeAboveAnything() * 0.4, 0.15, 5000.0);

		m_Camera.SetProjection(m_Walking ? 65.0f : 55.0f, Aspect(), nearClip, 1.4e7f);
		m_Camera.SetPosition(glm::vec3(0.0f));
		m_Camera.SetOrientation(m_Forward, m_Up);

		// Its own Framebuffer, so it has to be a separate pass -- one cannot
		// bind inside another. Before the main scene's own BeginScene, not
		// after, since this one has to finish and hand the window's viewport
		// back before that scene starts drawing into it.
		if (m_Walking)
			m_Pocket.RenderRoomToTexture();

		Egss::Renderer::BeginScene(m_Camera);

		// Before anything that writes depth, so everything else is in front of
		// it by construction rather than by comparison.
		DrawStars();

		size_t terrain = m_Walking ? (size_t)m_Ground : m_Frame;

		for (size_t i = 0; i < m_Bodies.size(); i++)
		{
			glm::dvec3 centre;
			float scale = BodyPlacement(i, origin, centre);

			if (i != terrain && i != 0 && !WorthDrawing(i, centre, scale))
				continue;

			DrawBody(i, centre, i == terrain, scale);
		}

		DrawRocks(origin);
		DrawShip(origin);

		if (m_Walking && m_Ground >= 0)
		{
			size_t index = (size_t)m_Ground;

			glm::dvec3 groundCentre;
			BodyPlacement(index, origin, groundCentre);

			glm::mat4 spin = SpinMatrix(index);
			glm::vec3 right = glm::vec3(spin * glm::vec4(m_Pocket.Right(), 0.0f));
			glm::vec3 up2 = glm::vec3(spin * glm::vec4(m_Pocket.Up(), 0.0f));
			glm::vec3 forward = glm::vec3(spin * glm::vec4(m_Pocket.Forward(), 0.0f));

			if (m_Pocket.InPocket())
			{
				glm::vec3 at = glm::vec3(groundCentre
					+ ToScene(index, SiteFixed(m_Pocket.RoomLocal())));

				m_Pocket.DrawInterior(at, right, up2, forward);
			}
			else
			{
				glm::vec3 at = glm::vec3(groundCentre
					+ ToScene(index, SiteFixed(m_Pocket.PortalLocal())));

				m_Pocket.DrawWindow(at, right, up2, forward);
			}
		}

		// After the bodies, so the far half of a ring is occluded by the planet
		// it goes round; before the air, so a ring seen through an atmosphere
		// is veiled by it.
		for (size_t i = 1; i < m_Bodies.size(); i++)
		{
			glm::dvec3 centre;
			float scale = BodyPlacement(i, origin, centre);

			if (i != terrain && !WorthDrawing(i, centre, scale))
				continue;

			DrawRings(i, glm::vec3(centre), scale);
		}

		// Then the water, then the air. Both are blended and neither writes
		// depth, so the order they go down in is the order they are read in,
		// and sea has to be under sky.
		for (size_t i = 1; i < m_Bodies.size(); i++)
		{
			glm::dvec3 centre;
			float scale = BodyPlacement(i, origin, centre);

			if (i != terrain && !WorthDrawing(i, centre, scale))
				continue;

			DrawOcean(i, glm::vec3(centre), scale);
		}

		// After the solid bodies, so the depth buffer already holds them and a
		// planet occludes the air behind it.
		for (size_t i = 1; i < m_Bodies.size(); i++)
		{
			glm::dvec3 centre;
			float scale = BodyPlacement(i, origin, centre);

			if (i != terrain && !WorthDrawing(i, centre, scale))
				continue;

			DrawAtmosphere(i, glm::vec3(centre), (float)DrawnRadius(i) * scale);
		}

		// After the air, for the same reason the air goes after the ground:
		// a thin shell nearer the surface than the scattering atmosphere is,
		// and blended, so it wants whatever is behind it already drawn.
		for (size_t i = 1; i < m_Bodies.size(); i++)
		{
			glm::dvec3 centre;
			float scale = BodyPlacement(i, origin, centre);

			if (i != terrain && !WorthDrawing(i, centre, scale))
				continue;

			DrawClouds(i, glm::vec3(centre), (float)DrawnRadius(i) * scale);
		}

		Egss::Renderer::EndScene();

		// **The tint, which is the other half of "no buoyancy and no tint".**
		// A flat colour over the whole screen, the same blit-quad trick
		// Cube3D uses to show its own framebuffer -- there is no post-process
		// pass to hook this into, and a screen-space effect does not need
		// one. Drawn after the 3D scene rather than mixed into its shaders,
		// so it survives `--hide-ui` (which this is not part of; it is the
		// picture, not a panel) and shows up in a capture the same way
		// standing at the surface does.
		if (m_EyeUnderwater)
		{
			Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
			Egss::RenderCommand::SetDepthWrite(false);

			Egss::Renderer2D::BeginScene(m_TintCamera);
			Egss::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(2.0f),
				glm::vec4(0.04f, 0.20f, 0.34f, 0.55f));
			Egss::Renderer2D::EndScene();

			Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
			Egss::RenderCommand::SetDepthWrite(true);
		}

		m_Stats = Egss::Renderer::GetStats();
	}

	// One body: its sphere, and its terrain if any has been meshed.
	//
	// **The sphere is drawn even when the terrain is.** Streamed chunks reach
	// `m_LoadRadius` and stop, and past that there was nothing but black -- a
	// world you are standing on ending a hundred metres away at a hard edge.
	// Shrinking the sphere to `R - amplitude/2` puts it *below* the floor of
	// the relief, so wherever real terrain exists it is strictly in front and
	// the depth test hides the sphere completely. Where terrain has not
	// streamed, the sphere is the horizon -- the right shape, the right colour,
	// and curving away exactly as the ground would.
	//
	// A skirt or a fog would hide the edge; this shows what is actually there,
	// which is a planet.
	void DrawBody(size_t index, const glm::dvec3& centre, bool isNear, float scale)
	{
		float radius = (float)DrawnRadius(index) * scale;

		if (index == 0)
		{
			// The star lights everything and is lit by nothing, so it is drawn
			// as its own colour at full brightness -- an emissive surface, and
			// the one body where a lighting calculation would be wrong.
			auto material = Egss::Material::CreateInstance(m_Material);
			material->Set("u_Color", glm::vec4(m_Bodies[0].Colour, 1.0f));
			material->Set("u_Emissive", 1.0f);
			material->Set("u_Sky", glm::vec3(0.0f));
			material->Set("u_Up", glm::vec3(0.0f, 1.0f, 0.0f));
			material->Set("u_LightPosition", glm::vec3(centre));
			material->Set("u_LightColor", m_SunLight * m_StarBrightness);

			Egss::Renderer::Submit(material, m_Sphere,
				glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(centre)),
					glm::vec3(radius)));

			return;
		}

		auto it = m_Planets.find(index);
		bool generated = it != m_Planets.end();
		bool meshed = isNear && generated && it->second.MeshedChunks() > 0;

		// **How far below the mean radius the stand-in sphere has to sit**, and
		// it is the generator's own answer rather than half the amplitude:
		// the relief is skewed, so the deepest valley is further down than the
		// tallest peak is up. Half the amplitude left the sphere *inside* the
		// valley floors, which is a smooth surface pushing up through terrain.
		//
		// Scaled with the radius, or a compressed planet keeps full-size hills.
		float relief = (generated ? it->second.ReliefReach() * 2.0f
			: (float)DrawnRadius(index) * 0.005f) * scale;

		// **Inset as soon as the planet exists, not only once it is meshed.**
		// The sphere has to sit below the lowest valley so real terrain hides
		// it -- and below the sea, or the ocean shell would be inside the ball
		// it is meant to cover and no planet would have water until you were
		// close enough to mesh one.
		float drawn = generated ? radius - relief * 0.5f - 0.5f : radius;

		auto material = Egss::Material::CreateInstance(m_TerrainMaterial);
		material->Set("u_LightDirection", SunDirection(index));
		material->Set("u_LightColor", m_SunLight * m_StarBrightness);
		material->Set("u_Sky", SkyLight(index));
		material->Set("u_LowColour", glm::vec4(m_Bodies[index].Colour * 0.55f, 1.0f));
		material->Set("u_HighColour",
			glm::vec4(glm::mix(m_Bodies[index].Colour, glm::vec3(1.0f), 0.35f), 1.0f));
		material->Set("u_Radius", radius);
		material->Set("u_Relief", relief);
		material->Set("u_Origin", glm::vec3(centre));
		material->Set("u_Unspin", glm::transpose(SpinMatrix(index)));
		material->Set("u_HazeDensity", m_Bodies[index].AtmosphereDensity * m_HazeScale);

		// The same scale height the shell marches with -- a quarter of the
		// shell's thickness -- because the two are describing one atmosphere
		// and a fragment sits under both of them. Drifting them apart would
		// put a step in the haze exactly where the shell takes over, which is
		// the horizon, which is the one place it would be seen.
		material->Set("u_AirScaleHeight",
			(float)(radius * (double)m_Bodies[index].AtmosphereFraction
				* (double)m_AirScale * 0.25));

		// A planet with no sea gets a waterline below its deepest valley, so
		// every point on it is "land" and the altitude ramp is all that runs.
		SetBiome(material, index, generated
			? it->second.Get() : VoxelPlanet::Settings(), scale);

		// **The sphere reads the map; the chunks do not.** Same material
		// otherwise, and the flag is the only thing that differs -- which is
		// why it is set twice rather than once.
		material->Set("u_HasMap", generated ? 1.0f : 0.0f);

		if (generated)
			material->SetTexture("u_Map", it->second.Map(), 0);

		// **The sphere has no reference point and does not need one.** It is a
		// body-sized ball, so an offset from anything on it is body-sized too
		// and the identity in the shader would be computing `e.e` at 1e14.
		// When it is drawn at all you are far enough away that half a metre of
		// shading height is a fraction of a pixel. See `u_ReferenceAltitude`.
		material->Set("u_HasReference", 0.0f);

		Egss::Renderer::Submit(material, m_Sphere,
			glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(centre)),
				glm::vec3(drawn)));

		if (!meshed)
			return;

		material->Set("u_HasMap", 0.0f);
		material->Set("u_HasReference", 1.0f);

		// The waterline this body's chunks are measured against, in double, so
		// that each chunk's reference altitude is the small number it should
		// be rather than a difference taken on the GPU.
		//
		// **Unscaled, and it is allowed to be**: `meshed` implies `isNear`,
		// and `BodyPlacement` returns a scale of exactly 1 for the body you
		// are standing on. Everything else the shader compares this against --
		// `u_SeaDepth`, `u_Beach` -- carries the scale, so if a compressed
		// body ever grew chunks this would need it too.
		double sea = SeaRadiusOf(index, it->second.Get());

		// **A transform per chunk, and the sum inside it done in double.**
		//
		// A chunk's mesh is measured from the chunk's own lattice origin, so
		// placing it is `centre + spin * origin` -- and both of those are a
		// planet radius long while the answer is a few hundred metres. That
		// subtraction is the entire reason the terrain is not on a 0.76 m
		// grid at 1:1, and it has to happen before the cast, not after.
		//
		// The rotation stays a float matrix because a rotation's entries are
		// all in [-1, 1] and lose nothing.
		glm::mat4 spin = SpinMatrix(index);

		for (const auto& [key, chunk] : it->second.Chunks())
		{
			glm::dvec3 turned = ToScene(index, chunk.Origin);
			glm::vec3 placed = glm::vec3(centre + turned);

			// **The chunk's own origin is the reference.** It is already the
			// point every vertex in this mesh is measured from, so the offset
			// the shader forms is exactly `a_Position` up to the spin -- a few
			// tens of metres, and exact as a float. The radius is
			// rotation-invariant, so it comes from the unturned original.
			SetReference(material, placed, turned,
				glm::length(chunk.Origin), sea);

			Egss::Renderer::Submit(material, chunk.MeshPtr,
				glm::translate(glm::mat4(1.0f), placed) * spin);
		}

		// **And the world past the chunks.** The same material with the same
		// flag: it is ground, coloured by the same rule, and the only thing
		// that makes it different is that it was displaced on the CPU instead
		// of marched. Drawn after the chunks so the depth buffer already holds
		// them where they overlap -- the droop makes that decision, but there
		// is no reason to make the driver work for it.
		if (m_Horizon.Valid() && m_Walking && (size_t)m_Ground == index)
		{
			glm::dvec3 turned = ToScene(index, m_Horizon.Site());
			glm::vec3 placed = glm::vec3(centre + turned);

			// The same treatment, and the reason the shader's identity is
			// written without a small-`e` assumption: this mesh reaches
			// fourteen kilometres from its site, where a Taylor expansion
			// would be carrying a 15 m second-order term.
			SetReference(material, placed, turned,
				glm::length(m_Horizon.Site()), sea);

			Egss::Renderer::Submit(material, m_Horizon.Mesh(),
				glm::translate(glm::mat4(1.0f), placed) * spin);
		}

		DrawPlants(index, it->second, centre, spin);
	}

	// **What the sky over this body is worth as a light.**
	//
	// The air shader already knows how much atmosphere there is and what
	// colour it scatters; this is the same two numbers read as an ambient
	// term. A quarter of the sunlight is what a clear sky returns on Earth at
	// a moderate sun angle -- the real figure runs about 15% of the horizontal
	// illuminance and rises toward the horizon -- and it is multiplied by the
	// sun's own elevation, because a sky at night is not a light.
	//
	// Zero atmosphere gives zero, which is the Moon, and is correct: shadows
	// there really are black.
	glm::vec3 SkyLight(size_t index) const
	{
		if (index >= m_Bodies.size())
			return glm::vec3(0.0f);

		const Body& body = m_Bodies[index];

		float air = glm::clamp(body.AtmosphereDensity * m_AirDensity / 60.0f,
			0.0f, 1.0f);

		if (air <= 0.0f)
			return glm::vec3(0.0f);

		// How high the sun is over the site, which is what decides whether the
		// dome is lit. `m_Up` is the local vertical wherever the camera is; off
		// the surface it is the flight frame's, which is the same direction.
		float elevation = glm::clamp(
			glm::dot(SunDirection(index), m_Up), 0.0f, 1.0f);

		// **`Scatter` is an extinction ratio, not the colour of the sky.**
		// (0.22, 0.45, 1.00) is how much more strongly blue is scattered than
		// red, which is why the sky is blue and the sun is yellow at noon; it
		// is not what the dome *looks* like, which is a pale blue with plenty
		// of red in it. Using the ratio directly gave a shaded slope 20% of
		// the red it should have and turned every hillside teal.
		glm::vec3 spectrum = body.Scatter
			/ glm::max(glm::max(body.Scatter.x,
				glm::max(body.Scatter.y, body.Scatter.z)), 1e-4f);

		glm::vec3 tint = glm::mix(glm::vec3(1.0f), spectrum, 0.6f);

		// **A shaded slope at about a third of a sunlit one.** The physical
		// figure for a clear sky is nearer a fifth of the direct beam on a
		// horizontal surface, and a fifth is what this started at -- but the
		// frame is written to an 8-bit buffer with no exposure curve on it, so
		// a fifth of a dark green is four units out of 255 and reads as black
		// whatever the arithmetic says. A third is what makes the shape of a
		// hillside legible on the screen it is actually shown on.
		return m_SunLight * m_StarBrightness * tint * (0.55f * air * elevation);
	}

	// **What one draw needs so the shader never forms a planet-sized float.**
	//
	// `placed` is the reference in camera-relative coordinates, which is what
	// the vertex stage subtracts; `turned` is the same point measured from the
	// planet's centre in the scene's orientation, which is what gives the
	// radial direction; `radius` is its distance from the centre, taken from
	// the *unturned* original because a spin is a rotation and cannot change
	// it. `altitude` is the only one that had to be a subtraction, and it
	// happens here in double.
	static void SetReference(const std::shared_ptr<Egss::Material>& material,
		const glm::vec3& placed, const glm::dvec3& turned, double radius,
		double sea)
	{
		material->Set("u_Reference", placed);
		material->Set("u_ReferenceNormal",
			glm::vec3(turned / glm::max(radius, 1e-9)));
		material->Set("u_ReferenceRadius", (float)radius);
		material->Set("u_ReferenceAltitude", (float)(radius - sea));
	}

	// The palette and the waterline, which the terrain and the sea both need
	// and neither owns.
	void SetBiome(const std::shared_ptr<Egss::Material>& material, size_t index,
		const VoxelPlanet::Settings& settings, float scale = 1.0f) const
	{
		double sea = SeaRadiusOf(index, settings);

		material->Set("u_Vegetated", settings.Vegetated ? 1.0f : 0.0f);
		material->Set("u_SeaRadius", (float)((double)scale * sea));

		// The one the shader actually shades from. See `SeaRadiusOf`.
		material->Set("u_SeaDepth",
			(float)((double)scale * (sea - DrawnRadius(index))));

		material->Set("u_Shallow", settings.Shallow);
		material->Set("u_Deep", settings.Deep);
		material->Set("u_Sand", settings.Sand);
		material->Set("u_Tropical", settings.Tropical);
		material->Set("u_Temperate", settings.Temperate);
		material->Set("u_Tundra", settings.Tundra);
		material->Set("u_Rock", settings.Rock);
		material->Set("u_Snow", settings.Snow);
		material->Set("u_Desert", settings.Desert);
		material->Set("u_Steppe", settings.Steppe);

		// Only a body with a sea has a drainage pass, and therefore a climate.
		material->Set("u_HasClimate", settings.HasOcean ? 1.0f : 0.0f);

		// Three voxels of shore: enough to read as a beach from a distance and
		// not so much that it becomes the landscape.
		material->Set("u_Beach", scale * 3.0f * glm::max(settings.VoxelSize, 0.1f));
	}

	static float ReliefOf(const VoxelPlanet::Settings& settings, float radius)
	{
		return settings.Amplitude > 0.0f ? settings.Amplitude : radius * 0.085f;
	}

	// **The waterline, in double, because everything that wants it wants the
	// difference from the radius rather than the number itself.** Both are
	// about 6.4e6 at 1:1 and the answer is a few hundred metres, so whoever
	// takes that subtraction decides how well the shore is known -- and the
	// GPU, in float, knew it to half a metre. Taken here instead, and the
	// small result is what crosses.
	double SeaRadiusOf(size_t index, const VoxelPlanet::Settings& settings) const
	{
		double radius = DrawnRadius(index);

		return settings.HasOcean ? (double)settings.OceanRadius
			: radius - (double)ReliefOf(settings, (float)radius);
	}

	// --- Trees ---------------------------------------------------------------
	//
	// Three shapes from the shared generator, drawn wherever the planet's
	// chunks put them. **Shallower and coarser than OpenWorld's**, which are
	// looked at from a metre away in a forest of forty: here there are a few
	// hundred in view at once on a world you can walk round in a couple of
	// minutes, and a branching factor of three at depth four is 121 tapered
	// prisms and 81 foliage blobs *each*.
	//
	// Depth three at five sides is 40 and 27, which is about a third of the
	// triangles for something you mostly see against the sky.
	// The same tree at three levels of detail. `lod` 0 is the one you stand
	// under; 2 is the one thirteen pixels tall on the horizon.
	//
	// **Only the tessellation changes between 0 and 1, not the structure.**
	// A tree's shape is its branching, and the branching is a function of the
	// seed -- so dropping `Sides` from five to three and coarsening the leaf
	// blobs leaves the silhouette where it was and the switch is invisible.
	// Level 2 does change the structure (a shallower tree has 9 tips instead
	// of 27), which is why its leaf clusters are grown to cover the crown the
	// missing generation used to fill: 27/9 the count wants (27/9)^(1/3) the
	// radius.
	static Veg::TreeParams TreeShape(int lod)
	{
		Veg::TreeParams params;
		params.Depth = lod < 2 ? 3 : 2;
		params.Sides = lod < 1 ? 5 : 3;
		params.Length = 2.1f;
		params.Radius = 0.16f;
		params.LeafSegments = lod < 1 ? 5 : 4;
		params.LeafRings = lod < 1 ? 3 : 2;

		if (lod >= 2)
			params.LeafRadius *= 1.44f;

		return params;
	}

	// **A lander, from three primitives and no asset file.**
	//
	// A tapered hexagonal hull, three legs and a nozzle: about two hundred
	// triangles, which is all a shape needs to be recognisable as the thing
	// you arrived in and have to get back to. Built here rather than loaded
	// because the demo's whole point is that nothing is a black box, and a
	// hull described by eight numbers can be argued with.
	void BuildLander()
	{
		Egss::MeshData data;

		const int sides = 6;
		const float pi = 3.14159265358979323846f;

		auto ring = [&](float radius, float height)
		{
			int first = (int)data.Vertices.size();

			for (int i = 0; i < sides; i++)
			{
				float a = (float)i / (float)sides * 2.0f * pi;

				Egss::MeshVertex point;
				point.Position = glm::vec3(std::cos(a) * radius, height,
					std::sin(a) * radius);
				point.Normal = glm::normalize(
					glm::vec3(std::cos(a), 0.35f, std::sin(a)));
				point.TexCoord = glm::vec2(0.0f);

				data.Vertices.push_back(point);
			}

			return first;
		};

		auto skirt = [&](int lower, int upper)
		{
			for (int i = 0; i < sides; i++)
			{
				unsigned int a = (unsigned int)(lower + i);
				unsigned int b = (unsigned int)(lower + (i + 1) % sides);
				unsigned int c = (unsigned int)(upper + i);
				unsigned int d = (unsigned int)(upper + (i + 1) % sides);

				data.Indices.insert(data.Indices.end(), { a, c, b, b, c, d });
			}
		};

		// Waist, shoulder, crown -- the hull swells and then tapers, which is
		// what makes it read as a capsule rather than a bin.
		int base = ring(1.15f, 0.35f);
		int waist = ring(1.55f, 1.30f);
		int shoulder = ring(1.30f, 2.45f);
		int crown = ring(0.55f, 3.15f);

		skirt(base, waist);
		skirt(waist, shoulder);
		skirt(shoulder, crown);

		// A cap, fanned from a centre vertex.
		{
			Egss::MeshVertex top;
			top.Position = glm::vec3(0.0f, 3.35f, 0.0f);
			top.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			top.TexCoord = glm::vec2(0.0f);

			unsigned int centre = (unsigned int)data.Vertices.size();
			data.Vertices.push_back(top);

			for (int i = 0; i < sides; i++)
				data.Indices.insert(data.Indices.end(), {
					centre, (unsigned int)(crown + i),
					(unsigned int)(crown + (i + 1) % sides) });
		}

		// The nozzle, a short cone under the base.
		{
			int mouth = ring(0.75f, -0.55f);

			skirt(mouth, base);
		}

		// Three legs, splayed. A box each, built as a skewed prism from two
		// quads -- enough to hold the hull off the ground and to say which way
		// up it is.
		for (int leg = 0; leg < 3; leg++)
		{
			float a = (float)leg / 3.0f * 2.0f * pi + 0.5f;
			glm::vec3 out(std::cos(a), 0.0f, std::sin(a));
			glm::vec3 side = glm::normalize(glm::cross(out, glm::vec3(0, 1, 0)));

			glm::vec3 hip = out * 1.05f + glm::vec3(0.0f, 1.05f, 0.0f);
			glm::vec3 foot = out * 2.15f + glm::vec3(0.0f, -0.55f, 0.0f);

			int first = (int)data.Vertices.size();

			for (int i = 0; i < 4; i++)
			{
				glm::vec3 where = (i < 2 ? hip : foot)
					+ side * ((i % 2) ? 0.16f : -0.16f);

				Egss::MeshVertex point;
				point.Position = where;
				point.Normal = glm::normalize(out + glm::vec3(0.0f, 0.5f, 0.0f));
				point.TexCoord = glm::vec2(0.0f);

				data.Vertices.push_back(point);
			}

			unsigned int f = (unsigned int)first;

			data.Indices.insert(data.Indices.end(), {
				f, f + 2, f + 1, f + 1, f + 2, f + 3,
				f, f + 1, f + 2, f + 1, f + 3, f + 2 });
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes.push_back(all);
		data.RecalculateBounds();

		m_Lander.reset(new Egss::Mesh(data, "Lander"));

		EGSS_TRACE("Lander: {0} vertices, {1} triangles",
			data.Vertices.size(), data.Indices.size() / 3);
	}

	void BuildTrees()
	{
		size_t triangles = 0;

		for (int i = 0; i < s_TreeShapes; i++)
		{
			for (int lod = 0; lod < s_TreeLods; lod++)
			{
			Egss::MeshData bark, leaves;
			Veg::MakeTreeMesh(521u + (unsigned int)i * 97u, TreeShape(lod),
				bark, leaves);

			m_TreeBark[i][lod].reset(new Egss::Mesh(bark, "PlanetTree"));
			m_TreeLeaves[i][lod].reset(new Egss::Mesh(leaves, "PlanetTreeLeaves"));

			// **One buffer of transforms, attached to both meshes of the
			// shape.** A trunk and its leaves are separate geometry with
			// separate materials but they stand in the same place, so they
			// share the instance data -- a `VertexBuffer` can belong to any
			// number of vertex arrays.
			//
			// Allocated once at full size rather than grown, because growing
			// it would mean adding it to the vertex arrays a second time and
			// the attribute locations are assigned in the order buffers are
			// added: the new one would land at 7 and the shader would still be
			// reading 3.
			m_TreeInstances[i][lod].reset(
				Egss::VertexBuffer::Create(s_MaxTreesPerShape * sizeof(glm::mat4)));

			// The divisor is what makes it per-instance, and it has to be set
			// before the buffer joins a vertex array -- that is when the
			// attribute pointers are declared.
			m_TreeInstances[i][lod]->SetLayout(
				Egss::BufferLayout({ { Egss::ShaderDataType::Mat4, "a_Model" } }, 1));

			m_TreeBark[i][lod]->SetInstanceBuffer(m_TreeInstances[i][lod]);
			m_TreeLeaves[i][lod]->SetInstanceBuffer(m_TreeInstances[i][lod]);

			triangles += (bark.Indices.size() + leaves.Indices.size()) / 3;
			}
		}

		EGSS_TRACE("Planet trees: {0} shapes x {1} levels, {2} triangles each "
			"on average", s_TreeShapes, s_TreeLods,
			triangles / (s_TreeShapes * s_TreeLods));
	}

	// One planet's trees, in the frame the chunks are already drawn in.
	void DrawPlants(size_t index, const VoxelPlanet& planet,
		const glm::dvec3& centre, const glm::mat4& spin)
	{
		if (planet.Get().PlantsPerChunk <= 0)
			return;

		// **One origin for the whole forest, and it follows the player.**
		//
		// Chunk meshes each carry their own origin because they are separate
		// draws; the trees are one buffer per shape and level, so they share
		// a uniform and therefore have to share an origin. The camera's own
		// planet-fixed position is the obvious one -- every tree that is drawn
		// is within the load radius of it, so every instance translation is a
		// few hundred metres at most, whatever the planet's radius.
		glm::dvec3 localOrigin = TerrainFocus(index);

		glm::mat4 frame = glm::translate(glm::mat4(1.0f),
			glm::vec3(centre + ToScene(index, localOrigin))) * spin;

		auto bark = Egss::Material::CreateInstance(m_TreeMaterial);
		bark->Set("u_Color", glm::vec4(0.30f, 0.22f, 0.15f, 1.0f));
		bark->Set("u_Emissive", 0.0f);
		bark->Set("u_Sky", SkyLight(index));
		bark->Set("u_Up", m_Up);
		bark->Set("u_LightColor", m_SunLight * m_StarBrightness);

		auto leaves = Egss::Material::CreateInstance(m_TreeMaterial);
		leaves->Set("u_Color", glm::vec4(0.16f, 0.34f, 0.13f, 1.0f));
		leaves->Set("u_Emissive", 0.0f);
		leaves->Set("u_Sky", SkyLight(index));
		leaves->Set("u_Up", m_Up);
		leaves->Set("u_LightColor", m_SunLight * m_StarBrightness);

		// **A directional light faked as a very distant point.** The shader
		// this shares with the rocks and the star takes a position and
		// normalises the difference, so a light four kilometres away along the
		// sun direction is parallel to a thousandth of a degree across a
		// planet this size.
		glm::vec3 lamp = SunDirection(index) * 40000.0f;
		bark->Set("u_LightPosition", lamp);
		leaves->Set("u_LightPosition", lamp);



		// **Gathered by shape, then drawn six times.**
		//
		// It used to be two `Submit`s a tree -- bark and leaves -- which on a
		// landed frame was 3,556 trees and **7,112 draw calls**, next to 1,174
		// for the terrain itself. That is what a surface frame's 16.65 ms was
		// spent on, and why an integrated GPU sat at 60% without being the
		// thing that was slow: the cost is submission, not shading.
		//
		// Three shapes times two materials is six, whatever the forest does.
		for (int shape = 0; shape < s_TreeShapes; shape++)
			for (int lod = 0; lod < s_TreeLods; lod++)
				m_TreeBatch[shape][lod].clear();

		// **Nothing behind you, tested a chunk at a time.**
		//
		// There was no culling of any kind here -- every tree in every
		// resident chunk was submitted, which while it was two draw calls each
		// was not the thing worth fixing. Now that they are one buffer, the
		// buffer may as well hold only what can be seen: 11,010 trees at 1,210
		// triangles apiece is 13.3 million triangles a frame, and well over
		// half of them are behind the camera.
		//
		// Per chunk rather than per tree, which is 963 tests instead of 11,010
		// for the same answer -- the trees in a chunk are within 24 m of each
		// other, so whatever the chunk is, they are. A cone rather than a
		// frustum, at 75 degrees against a half-angle of about 50, widened by
		// the chunk's own reach and a canopy so nothing whose leaves are in
		// view is dropped for having its trunk outside.
		const float chunkReach = 30.0f;

		for (const auto& [key, chunk] : planet.Chunks())
		{
			glm::vec3 towards = glm::vec3(centre + ToScene(index, chunk.Centre));

			float away = glm::length(towards);

			if (away > 60.0f && glm::dot(towards / away, m_Forward)
				< 0.26f - chunkReach / away)
				continue;

			for (const VoxelPlanet::Plant& plant : chunk.Plants)
			{
				// **Level of detail per tree, not per chunk.** The cull above
				// is per chunk because the answer is the same for everything
				// in one; this is not, because a 24 m chunk straddling a band
				// would flip thirty trees at once and that pops. It costs one
				// matrix-vector product a tree, which is the same product the
				// instance matrix below needs anyway.
				//
				// Divided by the tree's own scale, because what decides how
				// much geometry a tree is worth is how large it is on screen,
				// and these range 0.6 to 1.15 -- distance alone would give the
				// smallest tree in a stand more triangles than the largest.
				// The plant's offset from the forest's origin, in double
				// because both terms are a planet radius long and the answer
				// is metres. Everything after this is small.
				glm::vec3 local = glm::vec3(
					chunk.Origin + glm::dvec3(plant.Position) - localOrigin);

				// `localOrigin` *is* the camera, so this offset is already the
				// distance to it -- and the spin is a rotation, which does not
				// change a length.
				float apart = glm::length(local) / plant.Scale;

				int lod = apart < s_TreeLodNear ? 0 : (apart < s_TreeLodFar ? 1 : 2);

				// **Nothing grows on the landing pad.** A tree through the
				// hull is the first thing you see on opening the demo, and
				// the trees are scattered from the chunk's own hash, which
				// knows nothing about where a lander came down.
				//
				// **Across the ground, not through the air.** `m_SiteFixed` is
				// the lattice point nearest where the ship *arrived*, which is
				// twenty metres up -- so a straight distance to it put a tree
				// standing on the pad at exactly the clearing radius and let
				// it through. The first version of this had a twenty-metre
				// clearing and a tree through the hull, which looks like the
				// test not running rather than the test measuring the wrong
				// triangle. A clearing is a radius on the ground; the radial
				// component comes out.
				if (m_Walking && (size_t)m_Ground == index)
				{
					glm::dvec3 offset = chunk.Origin + glm::dvec3(plant.Position)
						- m_SiteFixed;

					glm::dvec3 vertical = glm::normalize(m_SiteFixed);

					double across = glm::length(
						offset - vertical * glm::dot(offset, vertical));

					if (across < (double)s_LandingClearing)
						continue;
				}

				std::vector<glm::mat4>& batch = m_TreeBatch[plant.Shape][lod];

				if (batch.size() >= s_MaxTreesPerShape)
					continue;

				// **The planet's frame stays a uniform.** Only what differs
				// between trees goes in the buffer, so the big translation is
				// applied once on the GPU instead of being multiplied into
				// every tree's matrix on the CPU.
				batch.push_back(
					glm::translate(glm::mat4(1.0f), local)
					* glm::mat4_cast(UprightAt(plant.Up))
					* glm::rotate(glm::mat4(1.0f), plant.Yaw, glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::scale(glm::mat4(1.0f), glm::vec3(plant.Scale)));
			}
		}

		int drawn = 0;

		for (int shape = 0; shape < s_TreeShapes; shape++)
		for (int lod = 0; lod < s_TreeLods; lod++)
		{
			const std::vector<glm::mat4>& batch = m_TreeBatch[shape][lod];

			if (batch.empty())
				continue;

			m_TreeInstances[shape][lod]->SetData(batch.data(),
				(unsigned int)(batch.size() * sizeof(glm::mat4)));

			// `frame` is the planet's placement and spin: what every tree on
			// this body has in common, and the only thing left in a uniform.
			Egss::Renderer::SubmitInstanced(bark, m_TreeBark[shape][lod],
				(unsigned int)batch.size(), frame);
			Egss::Renderer::SubmitInstanced(leaves, m_TreeLeaves[shape][lod],
				(unsigned int)batch.size(), frame);

			drawn += (int)batch.size();
		}

		m_PlantsDrawn = drawn;
	}

	// The sea. Drawn after every opaque thing in the frame, because it is
	// blended and writes no depth -- ground in front of it has to be in the
	// depth buffer already or the water goes over the top of it.
	// --- The sky -------------------------------------------------------------
	//
	// **Real stars, in their real places.**
	//
	// The bright ones are a table of J2000 right ascension and declination, so
	// Orion is Orion and the Plough points at Polaris. Nothing about the sky is
	// procedural except the faint background behind it -- a made-up star is a
	// star nobody can check, and the whole reason to type coordinates in is
	// that the angle between two of them is a number this code does not
	// contain.
	//
	// **What is not real is where the planets are against it.** The bodies
	// start at longitudes this demo chose, not at an epoch, so the constellation
	// behind Jupiter means nothing. The sky is right relative to itself.
	struct StarDescription
	{
		const char* Name;
		double RaHours;        // J2000
		double DecDegrees;
		float Magnitude;       // apparent visual
		float ColourIndex;     // B-V, which is what fixes the colour below
	};

	static const std::vector<StarDescription>& Stars()
	{
		// The brightest two dozen, plus the ones that make four constellations
		// readable rather than merely present: Orion including its belt, the
		// Plough, Cassiopeia's W, and the Southern Cross.
		static const std::vector<StarDescription> table =
		{
			{ "Sirius",     6.75248, -16.7161, -1.46f,  0.00f },
			{ "Canopus",    6.39920, -52.6957, -0.74f,  0.15f },
			{ "Rigil Kent",14.66014, -60.8340, -0.27f,  0.71f },
			{ "Arcturus",  14.26103,  19.1825, -0.05f,  1.23f },
			{ "Vega",      18.61564,  38.7837,  0.03f,  0.00f },
			{ "Capella",    5.27815,  45.9980,  0.08f,  0.80f },
			{ "Rigel",      5.24230,  -8.2016,  0.13f, -0.03f },
			{ "Procyon",    7.65503,   5.2250,  0.34f,  0.42f },
			{ "Achernar",   1.62857, -57.2367,  0.46f, -0.16f },
			{ "Betelgeuse", 5.91953,   7.4070,  0.50f,  1.85f },
			{ "Hadar",     14.06373, -60.3730,  0.61f, -0.23f },
			{ "Altair",    19.84639,   8.8683,  0.77f,  0.22f },
			{ "Acrux",     12.44331, -63.0991,  0.77f, -0.24f },
			{ "Aldebaran",  4.59867,  16.5093,  0.85f,  1.54f },
			{ "Spica",     13.41989, -11.1613,  1.04f, -0.23f },
			{ "Antares",   16.49013, -26.4320,  1.09f,  1.83f },
			{ "Pollux",     7.75534,  28.0262,  1.14f,  1.00f },
			{ "Fomalhaut", 22.96084, -29.6222,  1.16f,  0.09f },
			{ "Deneb",     20.69053,  45.2803,  1.25f,  0.09f },
			{ "Mimosa",    12.79537, -59.6888,  1.25f, -0.24f },
			{ "Regulus",   10.13953,  11.9672,  1.35f, -0.11f },
			{ "Castor",     7.57667,  31.8883,  1.58f,  0.03f },
			{ "Gacrux",    12.51944, -57.1133,  1.63f,  1.59f },
			{ "Bellatrix",  5.41885,   6.3497,  1.64f, -0.22f },
			{ "Elnath",     5.43819,  28.6075,  1.65f, -0.13f },
			{ "Alnilam",    5.60356,  -1.2019,  1.69f, -0.18f },
			{ "Alnitak",    5.67931,  -1.9426,  1.77f, -0.20f },
			{ "Alioth",    12.90049,  55.9598,  1.77f, -0.02f },
			{ "Dubhe",     11.06213,  61.7511,  1.79f,  1.07f },
			{ "Alkaid",    13.79235,  49.3133,  1.86f, -0.19f },
			{ "Alphard",    9.45979,  -8.6586,  1.98f,  1.44f },
			{ "Polaris",    2.53030,  89.2641,  1.98f,  0.60f },
			{ "Saiph",      5.79594,  -9.6696,  2.07f, -0.17f },
			{ "Denebola",  11.81766,  14.5720,  2.14f,  0.09f },
			{ "Mintaka",    5.53344,  -0.2991,  2.23f, -0.18f },
			{ "Mizar",     13.39873,  54.9254,  2.23f,  0.06f },
			{ "Schedar",    0.67511,  56.5373,  2.24f,  1.17f },
			{ "Caph",       0.15297,  59.1498,  2.28f,  0.38f },
			{ "Merak",     11.03069,  56.3825,  2.37f,  0.03f },
			{ "Phecda",    11.89717,  53.6948,  2.44f,  0.04f },
			{ "Gamma Cas",  0.94515,  60.7167,  2.47f, -0.15f },
			{ "Ruchbah",    1.43022,  60.2353,  2.68f,  0.13f },
			{ "Megrez",    12.25707,  57.0326,  3.31f,  0.08f },
			{ "Segin",      1.90661,  63.6701,  3.37f, -0.15f },
		};

		return table;
	}

	// **Equatorial to the plane the planets are actually in.**
	//
	// Right ascension and declination are measured from Earth's equator, and
	// everything else in this demo is measured from the ecliptic -- the two are
	// 23.44 degrees apart, which is Earth's axial tilt, and it is the one place
	// in the demo where that number is used for anything. Skip the rotation and
	// the whole sky is tilted by it: Polaris comes out at the pole of the solar
	// system, where the ecliptic pole is really 23.44 degrees away from it.
	//
	// Then into this demo's axes, which are not glm's: +Y is the north ecliptic
	// pole, and longitude increases from +x toward +z because that is the
	// direction the orbits run. See the note on `SpinMatrix` for the same trap
	// caught the hard way.
	static glm::dvec3 SkyDirection(double raHours, double decDegrees)
	{
		const double pi = 3.14159265358979323846;

		double ra = raHours * (pi / 12.0);
		double dec = decDegrees * (pi / 180.0);

		// Equatorial: +z toward the north celestial pole, +x at the vernal
		// equinox, which is the axis the two systems share.
		double x = std::cos(dec) * std::cos(ra);
		double y = std::cos(dec) * std::sin(ra);
		double z = std::sin(dec);

		const double obliquity = 23.4392911 * (pi / 180.0);

		double c = std::cos(obliquity), s = std::sin(obliquity);

		// About the shared +x axis, which leaves the equinox alone.
		double yEcliptic = y * c + z * s;
		double zEcliptic = -y * s + z * c;

		return glm::dvec3(x, zEcliptic, yEcliptic);
	}

	// Colour from B-V, which is what a colour index is for: how much bluer a
	// star is in B than in V, so negative is hot and positive is cool. The
	// breakpoints are Vega at 0.00 (white by definition -- the scale is built
	// on it), the Sun at 0.65, and Betelgeuse at 1.85.
	static glm::vec3 StarColour(float bv)
	{
		if (bv < 0.0f)
			return glm::mix(glm::vec3(0.66f, 0.76f, 1.00f),
				glm::vec3(1.00f, 1.00f, 1.00f), glm::clamp(bv / -0.35f, 0.0f, 1.0f));

		if (bv < 0.65f)
			return glm::mix(glm::vec3(1.00f, 1.00f, 1.00f),
				glm::vec3(1.00f, 0.96f, 0.86f), bv / 0.65f);

		return glm::mix(glm::vec3(1.00f, 0.96f, 0.86f),
			glm::vec3(1.00f, 0.76f, 0.55f), glm::clamp((bv - 0.65f) / 1.2f, 0.0f, 1.0f));
	}

	// **Two layers, because they are two different problems.**
	//
	// The catalogue is 44 real stars and wants to be exactly where it says it
	// is, which is geometry. The background is thousands of anonymous faint
	// ones and wants to be even, resolution-independent and free, which is a
	// function of the view direction. So the first is a mesh and the second is
	// a shader on the sphere behind it.
	void BuildStars()
	{
		// --- The catalogue, as billboarded quads ---------------------------
		//
		// **A star has no size, and its size on screen is entirely the point
		// spread of whatever looked at it.** So every quad here is the same
		// angular size and only the brightness differs -- and the visible disc
		// still grows with brightness, because a brighter Gaussian crosses the
		// eye's threshold further out. That is what makes Sirius look bigger
		// than Megrez without a single per-star size being stored.
		Egss::MeshData data;

		const std::vector<StarDescription>& stars = Stars();

		for (const StarDescription& star : stars)
		{
			glm::vec3 direction = glm::vec3(SkyDirection(star.RaHours, star.DecDegrees));

			// Magnitude is a logarithm of flux with 2.512 per step and the
			// bright end negative, so this is the flux, normalised to put
			// Sirius at one.
			float flux = std::pow(2.512f, -(star.Magnitude - stars[0].Magnitude));

			// **Compressed, and the exponent is the whole argument.** The flux
			// range across this table is 86:1 and an 8-bit display's is not,
			// so it has to be squashed; the question is by how much, and both
			// ends of the answer are visible in a capture.
			//
			// A cube root gives 4.4:1, at which the faintest catalogue star
			// and the brightest anonymous one behind it are the same dot --
			// **Orion was not findable in a frame pointed straight at it.** A
			// square root gives 9.3:1 and puts Orion's belt at 0.235, only
			// 1.5 times the field's brightest. Two fifths gives 5.9:1, the
			// belt at 0.322 and Megrez -- the faintest star in the table -- at
			// 0.169, which is the number the background is then scaled to sit
			// under.
			float brightness = std::pow(flux, 0.4f);

			// **Sirius is allowed to clip, on purpose.** Every photograph of
			// the sky ever taken has a saturated core on its brightest star,
			// and holding the top of the range below white to avoid it would
			// mean putting magnitude 2 down at 0.28 -- which is where the
			// Plough became invisible. Gained so that the middle of the table
			// lands around two thirds and the top three or four run over.
			glm::vec3 colour = StarColour(star.ColourIndex) * (brightness * 2.2f);

			for (int corner = 0; corner < 4; corner++)
			{
				glm::vec2 offset((corner == 1 || corner == 2) ? 1.0f : -1.0f,
					(corner >= 2) ? 1.0f : -1.0f);

				data.Vertices.push_back({ direction, colour, offset });
			}
		}

		for (size_t i = 0; i < stars.size(); i++)
		{
			unsigned int at = (unsigned int)i * 4;

			data.Indices.insert(data.Indices.end(),
				{ at, at + 1, at + 2, at, at + 2, at + 3 });
		}

		data.Submeshes.push_back({ "", -1, 0, (unsigned int)data.Indices.size() });

		m_Stars.reset(new Egss::Mesh(data, "Stars"));

		std::string brightVertex = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Direction;
			layout(location = 1) in vec3 a_Colour;
			layout(location = 2) in vec2 a_Corner;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform vec3 u_Right;
			uniform vec3 u_Up;
			uniform float u_Distance;
			uniform float u_Size;

			out vec3 v_Colour;
			out vec2 v_Corner;

			void main()
			{
				v_Colour = a_Colour;
				v_Corner = a_Corner;

				// Billboarded against the camera's own basis rather than by a
				// matrix trick, because the camera here has no fixed up: it is
				// a basis carried around by the flight controller.
				vec3 at = a_Direction * u_Distance
					+ (u_Right * a_Corner.x + u_Up * a_Corner.y) * u_Distance * u_Size;

				gl_Position = u_ViewProjection * u_Transform * vec4(at, 1.0);
			}
		)";

		std::string brightFragment = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_Colour;
			in vec2 v_Corner;

			uniform float u_Fade;

			void main()
			{
				// A Gaussian point spread. The 0.34 is the width in units of
				// the quad's half-size, chosen so the brightest star's visible
				// disc is a few pixels and the faintest is one.
				float r = length(v_Corner);

				float spread = exp(-(r * r) / (0.34 * 0.34));

				vec3 result = v_Colour * spread * u_Fade;

				// Additive: stars overlap by being in front of each other, not
				// by hiding each other, and the sky behind them is black.
				color = vec4(result, 1.0);
			}
		)";

		m_StarShader.reset(Egss::Shader::Create("Stars", brightVertex, brightFragment));
		m_StarMaterial = Egss::Material::Create(m_StarShader);

		// --- The background, on the sphere ---------------------------------

		std::string fieldVertex = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_Direction;

			void main()
			{
				v_Direction = a_Position;

				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string fieldFragment = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_Direction;

			uniform float u_Fade;
			uniform vec3 u_GalacticPole;

			// **An integer hash, because `fract(sin(dot(...)) * 43758.5)` fails
			// exactly where a starfield needs it.**
			//
			// That idiom works while its argument is small. Here the argument
			// is a cell index times a few hundred plus a layer salt, which
			// reaches eighteen thousand -- and a float carries about a
			// thousandth of absolute precision there, while `sin` needs the
			// phase to a millionth to decorrelate neighbours. The result was a
			// sky of **grey rectangles**: whole cells sharing a hash with
			// their neighbours because the phase could not tell them apart.
			// Same family as the `Hash2D` trap in the handover, arrived at
			// from the other direction.
			//
			// The integer version has no such range: every bit of the input
			// reaches every bit of the output, at any magnitude.
			float hash(ivec3 cell, uint salt)
			{
				// Shifted positive first: the conversion to unsigned is
				// defined either way, but a negative index and its positive
				// twin must not collide.
				uvec3 key = uvec3(cell + ivec3(4096));

				uint h = key.x * 374761393u + key.y * 668265263u
					+ key.z * 2246822519u + salt * 3266489917u;

				h = (h ^ (h >> 13)) * 1274126177u;
				h ^= h >> 16;

				return float(h & 0xFFFFFFu) / float(0xFFFFFFu);
			}

			// **A cube face, not a latitude and a longitude.** Cells cut from
			// `atan` and `asin` bunch at the poles, and a starfield built that
			// way has two obvious clumps in it. Dividing the two smaller
			// components by the largest is the cube-map parameterisation, whose
			// worst distortion is the 1.5x between a face's middle and its
			// corner -- invisible in a field of points.
			vec3 face(vec3 direction, out vec2 uv)
			{
				vec3 a = abs(direction);

				if (a.x >= a.y && a.x >= a.z)
				{
					uv = direction.yz / a.x;
					return vec3(direction.x > 0.0 ? 1.0 : 2.0, 0.0, 0.0);
				}

				if (a.y >= a.z)
				{
					uv = direction.xz / a.y;
					return vec3(direction.y > 0.0 ? 3.0 : 4.0, 0.0, 0.0);
				}

				uv = direction.xy / a.z;
				return vec3(direction.z > 0.0 ? 5.0 : 6.0, 0.0, 0.0);
			}

			// Smooth value noise in three dimensions, on the direction rather
			// than on anything the cube faces know about, so it has no seams.
			// Eight corners and a smoothstep, which is the cheapest thing that
			// is continuous.
			float noise(vec3 at, uint salt)
			{
				vec3 base = floor(at);
				vec3 f = at - base;

				f = f * f * (3.0 - 2.0 * f);

				ivec3 b = ivec3(base);

				float x00 = mix(hash(b + ivec3(0, 0, 0), salt),
					hash(b + ivec3(1, 0, 0), salt), f.x);
				float x10 = mix(hash(b + ivec3(0, 1, 0), salt),
					hash(b + ivec3(1, 1, 0), salt), f.x);
				float x01 = mix(hash(b + ivec3(0, 0, 1), salt),
					hash(b + ivec3(1, 0, 1), salt), f.x);
				float x11 = mix(hash(b + ivec3(0, 1, 1), salt),
					hash(b + ivec3(1, 1, 1), salt), f.x);

				return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
			}

			// One layer of the field: a grid over the face, one star per cell
			// that passes the threshold, jittered inside it.
			vec3 layer(vec2 uv, int which, float density, float share)
			{
				vec2 scaled = uv * density;

				// **The derivative is taken here, before anything branches.**
				//
				// GLSL leaves `fwidth` undefined in non-uniform control flow,
				// and a cell with no star in it returning early *is* non-
				// uniform: a 2x2 quad straddling a cell boundary then has
				// helper invocations that took the other path, and the
				// derivative they contribute is whatever the driver had lying
				// around. It cost run-to-run reproducibility -- twelve faint
				// pixels, always the same twelve, flickering between two
				// values from one run of the same binary to the next, on a
				// scene that was byte-identical with the sky switched off.
				//
				// Hoisting it above the branch is the whole fix. Everything
				// below may diverge as much as it likes.
				float pixel = max(fwidth(scaled.x), 1.0e-6);

				vec2 cell = floor(scaled);
				vec2 f = scaled - cell;

				ivec3 key = ivec3(int(cell.x), int(cell.y), which);

				if (hash(key, 0u) < 1.0 - share)
					return vec3(0.0);

				// Kept to the middle of the cell so a star is never cut in half
				// by a boundary this shader does not look across.
				vec2 at = vec2(0.25 + 0.5 * hash(key, 1u),
					0.25 + 0.5 * hash(key, 2u));

				// **Sized in pixels, not in cells.** `pixel` above is how far
				// the cell coordinate moves between neighbouring fragments, so
				// this is a star a fixed couple of pixels across at any field
				// of view -- which is what a point source looks like, and it
				// stops the whole sky swelling when the camera zooms.
				//
				// The 1.5 is a floor and not a taste: below about one pixel of
				// sigma the Gaussian is narrower than the sampling grid, and
				// what gets drawn is not a small star but an aliased one --
				// the first version used 0.8 and produced a sky of **dashes**,
				// each one a point source caught by a scanline.
				//
				// Capped as well as floored: where the projection stretches --
				// a cube face's corner, or a grazing view -- the derivative
				// grows, and a sigma approaching a whole cell lights the cell
				// rather than a point in it.
				float sigma = clamp(pixel * 1.5, 1.0e-5, 0.2);

				float r = length(f - at);

				float spread = exp(-(r * r) / (sigma * sigma));

				// Faint ones far outnumber bright ones, which a cube does well
				// enough and a uniform does not -- a linear brightness gives a
				// flat grey haze rather than a scattering of points.
				float magnitude = hash(key, 3u);

				float brightness = magnitude * magnitude * magnitude;

				// A little colour, on the same warm-to-cool axis the catalogue
				// uses, but weaker: faint stars are near the eye's colour
				// threshold and read as white.
				float tint = hash(key, 4u);

				vec3 colour = mix(vec3(0.82f, 0.88f, 1.0), vec3(1.0, 0.90f, 0.78f), tint);

				return colour * (brightness * spread);
			}

			void main()
			{
				vec3 direction = normalize(v_Direction);

				vec2 uv;
				int which = int(face(direction, uv).x);

				// **About 5,000 stars over the whole sphere**, which is what
				// the naked eye gets on a good night: `cells^2 * share * 6
				// faces` for each layer. The first draft ran two layers at 90
				// and 200 cells, which is 49,000, and the sky came out as a
				// wall of noise with no black in it.
				// **Faint, because these are the ones the catalogue leaves
				// out.** Left at full brightness the field's best cells came
				// out as bright as Sirius, and the forty-four stars that are
				// really there vanished into four thousand that are not --
				// Orion was not findable in a capture pointed straight at it.
				// 0.15 puts the whole field under the faintest catalogue star
				// -- Megrez at magnitude 3.31 draws at 0.169 -- which is where
				// it belongs, since the field *is* the stars the catalogue
				// stops before. Most of it is far below that: the cube of a
				// uniform has a median of an eighth, so the typical star here
				// is 0.019 and Orion's belt is seventeen times it.
				vec3 result = 0.15 * (layer(uv, which, 24.0, 0.30)
					+ layer(uv, which + 8, 55.0, 0.22));

				// **The Milky Way**, which is a band because the galaxy is a
				// disc and we are inside it. The pole is a real direction --
				// RA 12h51m, Dec +27.13 -- so the band lies where it lies, and
				// crosses Cassiopeia and Crux the way it should.
				float latitude = asin(clamp(dot(direction, u_GalacticPole), -1.0, 1.0));

				float band = exp(-(latitude * latitude) / (0.28 * 0.28));

				// Broken up, or it is an airbrushed stripe. Two octaves of the
				// same cheap hash, quantised coarsely enough to read as clouds.
				// Broken up, or it is an airbrushed stripe. Smooth 3D value
				// noise on the direction itself rather than on the face
				// coordinate: quantised per-cell hashes drew the band as
				// **grey squares**, and anything indexed by the face has a
				// seam down every cube edge the band crosses.
				float mottle = 0.55
					+ 0.30 * noise(direction * 7.0, 5u)
					+ 0.15 * noise(direction * 19.0, 6u);

				result += vec3(0.055, 0.056, 0.068) * band * mottle;

				color = vec4(result * u_Fade, 1.0);
			}
		)";

		m_FieldShader.reset(Egss::Shader::Create("Starfield", fieldVertex, fieldFragment));
		m_FieldMaterial = Egss::Material::Create(m_FieldShader);
	}

	// **How much of the sky is drowned out by daylight.**
	//
	// Stars are not hidden in the day by air blocking them -- Rayleigh
	// extinction at the zenith is about a tenth of a magnitude, which is
	// nothing. They are hidden by being *outshone*: the daytime sky is some
	// thousands of times brighter than the brightest of them, and a display
	// with no headroom above white cannot say that. So the fade is explicit,
	// and it is driven by the two things that decide it -- how much air is
	// above you, and how far the star is above the horizon.
	//
	// The window is chosen from what it is standing in for: full daylight with
	// the star more than three degrees up, full darkness below about eleven,
	// which is astronomical twilight to within a couple of degrees.
	float SkyFade() const
	{
		size_t index = m_Walking ? (size_t)m_Ground : m_Frame;

		if (index == 0 || index >= m_Bodies.size())
			return 1.0f;

		const Body& body = m_Bodies[index];

		if (body.AtmosphereFraction <= 0.0f)
			return 1.0f;

		double radius = DrawnRadius(index);
		double top = radius * (1.0 + (double)body.AtmosphereFraction * (double)m_AirScale);
		double height = glm::length(m_Local);

		if (height >= top || height < 1e-6)
			return 1.0f;

		double thickness = std::max(top - radius, 1e-6);
		double air = std::exp(-std::max(height - radius, 0.0) / (thickness * 0.25));

		glm::vec3 up = glm::vec3(m_Local / height);

		float elevation = glm::dot(up, SunDirection(index));

		float day = glm::smoothstep(-0.20f, 0.05f, elevation);

		// **Saturating, because the sky does not have to be at full brightness
		// to win.** The first version returned `1 - air * day`, which reads
		// like the right shape and is not: an exponential with a scale height
		// a quarter of the shell is down to 0.21 six metres up, so a player
		// standing on the ground in the middle of the afternoon kept a fifth
		// of the starfield -- **stars, in a blue daytime sky**.
		//
		// The physical error is in what `air` measures. The density at the eye
		// is not what outshines a star; the column of lit air above it is, and
		// a fiftieth of that column is still thousands of times brighter than
		// anything in the catalogue. So the threshold is small and the rolloff
		// is sharp: full daylight kills the sky whether there is a whole
		// atmosphere overhead or a twentieth of one, and it takes real
		// altitude or a real sunset to get the stars back.
		float sky = (float)air * day;

		return 1.0f - glm::smoothstep(0.0f, 0.05f, sky);
	}

	// Drawn first, before anything that writes depth. The sphere is centred on
	// the camera and writes no depth of its own, so everything else lands in
	// front of it whatever its distance -- which is what "at infinity" has to
	// mean when the far plane is 900 km.
	void DrawStars()
	{
		float fade = SkyFade();

		if (fade <= 0.001f)
			return;

		// Comfortably inside the far plane. The number does not matter beyond
		// that: nothing is ever drawn behind it and it never moves relative to
		// the camera, so no parallax can reveal it.
		const float distance = 600000.0f;

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Additive);
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		auto field = Egss::Material::CreateInstance(m_FieldMaterial);
		field->Set("u_Fade", fade);
		field->Set("u_GalacticPole",
			glm::vec3(SkyDirection(12.85694, 27.1283)));

		Egss::Renderer::Submit(field, m_Sphere,
			glm::scale(glm::mat4(1.0f), glm::vec3(distance)));

		glm::vec3 right = glm::normalize(glm::cross(m_Forward, m_Up));

		auto bright = Egss::Material::CreateInstance(m_StarMaterial);
		bright->Set("u_Distance", distance);
		bright->Set("u_Fade", fade);
		bright->Set("u_Right", right);
		bright->Set("u_Up", m_Up);

		// Half a degree of quad, which is the room the point spread needs at
		// the bright end. The star inside it is a couple of pixels.
		// **Nearly a degree of quad, which is far more than a star subtends
		// and exactly what a star looks like.** A point source is drawn by
		// whatever spread the eye or the lens gives it, and at half a degree
		// the Plough -- seven stars between magnitude 1.8 and 3.3 -- was a
		// capture pointed straight at it with nothing findable in the frame.
		// The pattern was exactly right; it was two pixels wide.
		bright->Set("u_Size", glm::radians(0.85f));

		Egss::Renderer::Submit(bright, m_Stars, glm::mat4(1.0f));

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	// --- Rings ---------------------------------------------------------------
	//
	// **One annulus mesh, and the radii are a uniform.**
	//
	// Vertices carry a direction on the unit circle and a parameter in
	// `TexCoord.x` saying which edge they belong to; the vertex shader puts
	// them at `mix(inner, outer, t)`. So Saturn and Uranus share one mesh with
	// nothing scaled non-uniformly, and the fragment shader gets the radius in
	// units of planet radii, which is the only coordinate the band structure
	// is naturally described in.
	//
	// **The plane is the body's own equator, from `SpinAxis`.** This used to
	// be `y = 0` for every body -- nothing had an axial tilt, so Saturn's
	// rings sat in the ecliptic rather than 26.7 degrees out of it, edge-on
	// from another planet where really they are what a small telescope shows
	// first. It waited on the terrain generator, the surface gravity, the
	// walking frame and the spin all agreeing about which way is up -- which
	// is what real axial tilt is, below.
	void BuildRings()
	{
		Egss::MeshData data;

		const int segments = 256;
		const float pi = 3.14159265358979323846f;

		for (int i = 0; i <= segments; i++)
		{
			float angle = 2.0f * pi * (float)i / (float)segments;
			glm::vec3 direction(std::cos(angle), 0.0f, std::sin(angle));

			// The position is the *direction*; the vertex shader turns it into
			// a radius. Normal is up for both edges -- a ring is flat.
			data.Vertices.push_back({ direction, glm::vec3(0.0f, 1.0f, 0.0f),
				glm::vec2(0.0f, (float)i / (float)segments) });
			data.Vertices.push_back({ direction, glm::vec3(0.0f, 1.0f, 0.0f),
				glm::vec2(1.0f, (float)i / (float)segments) });
		}

		for (int i = 0; i < segments; i++)
		{
			unsigned int at = (unsigned int)i * 2;

			data.Indices.insert(data.Indices.end(),
				{ at, at + 1, at + 3, at, at + 3, at + 2 });
		}

		data.Submeshes.push_back({ "", -1, 0, (unsigned int)data.Indices.size() });

		m_Ring.reset(new Egss::Mesh(data, "Ring"));

		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Direction;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform vec3 u_Centre;
			uniform float u_Inner;
			uniform float u_Outer;

			out vec3 v_World;
			out float v_Radius;

			void main()
			{
				v_Radius = mix(u_Inner, u_Outer, a_TexCoord.x);
				v_World = u_Centre + mat3(u_Transform) * (a_Direction * v_Radius);

				gl_Position = u_ViewProjection * vec4(v_World, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_World;
			in float v_Radius;

			uniform vec3 u_Centre;
			uniform vec3 u_Normal;
			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec3 u_Colour;
			uniform float u_PlanetRadius;
			uniform float u_Inner;
			uniform float u_Outer;

			float hash(float x)
			{
				return fract(sin(x * 127.1) * 43758.5453);
			}

			// Value noise in one dimension: the bands are a radial function and
			// nothing else, which is what makes them look like orbits rather
			// than like a texture.
			float bands(float x)
			{
				float low = floor(x);
				float f = fract(x);

				f = f * f * (3.0 - 2.0 * f);

				return mix(hash(low), hash(low + 1.0), f);
			}

			void main()
			{
				// Radius in planet radii, which is the unit every published
				// number about a ring system is quoted in.
				float u = v_Radius / u_PlanetRadius;

				float opacity = 0.35 * bands(u * 9.0)
					+ 0.35 * bands(u * 27.0)
					+ 0.30 * bands(u * 71.0);

				// **The Cassini division**, at 1.95 to 2.02 Saturn radii -- the
				// one gap anybody can name, and the reason a ring reads as a
				// ring system rather than as a disc. Smoothed rather than cut,
				// because its edges are not sharp either.
				opacity *= 1.0 - 0.92 * (smoothstep(1.93, 1.96, u)
					- smoothstep(2.00, 2.04, u));

				// Fade at both edges so the annulus does not end on a hard
				// circle where the geometry does.
				float span = u_Outer - u_Inner;
				opacity *= smoothstep(0.0, 0.06, (v_Radius - u_Inner) / span);
				opacity *= 1.0 - smoothstep(0.90, 1.0, (v_Radius - u_Inner) / span);

				vec3 normal = u_Normal;

				// The ring is flat, so which side the star is on and which side
				// the eye is on are one number each.
				float sunSide = dot(normal, u_LightDirection);
				float viewSide = dot(normal, normalize(-v_World));

				float lit = abs(sunSide);

				// **Seen from the shadowed side a ring is a negative of
				// itself**: the thin parts pass light and the thick parts stop
				// it, so the Cassini division is *bright* from underneath.
				// That is one line here and it is the whole difference between
				// a ring and a grey band.
				float through = sunSide * viewSide < 0.0
					? mix(1.0, 0.12, opacity) * 0.6 : 1.0;

				// The planet's shadow, cast along the direction of the star.
				// A point is in it when the planet is between it and the star:
				// on the far side along the light, and inside the cylinder.
				vec3 at = v_World - u_Centre;
				float along = dot(at, u_LightDirection);
				float across = length(at - u_LightDirection * along);

				float shadow = along < 0.0
					? smoothstep(u_PlanetRadius * 0.97, u_PlanetRadius * 1.03, across)
					: 1.0;

				// **The opposition surge.** Ring particles throw light back the
				// way it came far more strongly than a diffuse surface does --
				// each one hides its own shadow when you look along the
				// sunbeam -- and Saturn's rings brighten sharply within a
				// degree or two of opposition because of it. Here it is worth
				// having for a second reason: a ring tilted 26.73 degrees
				// receives `sin(26.73) = 0.45` of normal incidence at best, so
				// without it the rings sit at a third of the brightness of the
				// planet they are lit by.
				float back = max(dot(normalize(-v_World), u_LightDirection), 0.0);
				float surge = 1.0 + 0.8 * pow(back, 3.0);

				vec3 result = u_Colour * u_LightColor * lit * surge * through * shadow;

				color = vec4(result, clamp(opacity, 0.0, 1.0));
			}
		)";

		m_RingShader.reset(Egss::Shader::Create("Ring", vertexSrc, fragmentSrc));
		m_RingMaterial = Egss::Material::Create(m_RingShader);
	}

	void DrawRings(size_t index, const glm::vec3& centre, float scale)
	{
		const Body& body = m_Bodies[index];

		if (body.RingInner <= 0.0f)
			return;

		float radius = (float)DrawnRadius(index) * scale;

		auto material = Egss::Material::CreateInstance(m_RingMaterial);
		// The ring's own basis: +Y taken to the spin axis itself -- a ring is
		// the body's equatorial plane, not a tilt of its own -- and the other
		// two axes anywhere consistent, since an annulus has no preferred
		// azimuth. Crossed with +X rather than +Z to build `across`: the axis
		// never has an x-component (see `SpinAxis`), so +X is never close to
		// parallel to it, which +Z is at Uranus's 97.77-degree tilt.
		glm::vec3 normal = glm::vec3(SpinAxis(index));
		glm::vec3 across = glm::normalize(glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), normal));

		// Passed as the mesh's own transform rather than as a uniform of its
		// own -- it is exactly what a model matrix is for, and `Renderer` sets
		// `u_Transform` on every submission whether a shader asks or not.
		glm::mat4 basis(1.0f);
		basis[0] = glm::vec4(across, 0.0f);
		basis[1] = glm::vec4(normal, 0.0f);
		basis[2] = glm::vec4(glm::cross(across, normal), 0.0f);

		material->Set("u_Normal", normal);
		material->Set("u_Centre", centre);
		material->Set("u_LightDirection", SunDirection(index));
		material->Set("u_LightColor", m_SunLight * m_StarBrightness);
		material->Set("u_Colour", body.RingColour);
		material->Set("u_PlanetRadius", radius);
		material->Set("u_Inner", radius * body.RingInner);
		material->Set("u_Outer", radius * body.RingOuter);

		// Alpha, no depth write, no culling: a ring is thin enough to see
		// through, thin enough to have no back, and the far half of it has to
		// be occluded by the planet -- which depth *testing* does, since the
		// bodies were drawn first and wrote depth.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		Egss::Renderer::Submit(material, m_Ring, basis);

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	void DrawOcean(size_t index, const glm::vec3& centre, float scale)
	{
		auto it = m_Planets.find(index);

		if (it == m_Planets.end() || !it->second.Get().HasOcean)
			return;

		const VoxelPlanet::Settings& settings = it->second.Get();

		// The same water, twice: once as a sphere for everything out to the
		// horizon and beyond, and once as a sheet at the local level over the
		// ground you are standing on. Only the geometry and the cut-out
		// differ, so the uniforms are written once.
		auto dress = [&](const std::shared_ptr<Egss::Material>& water)
		{
			water->SetTexture("u_Map", it->second.Map(), 0);
			water->Set("u_Unspin", glm::transpose(SpinMatrix(index)));
			water->Set("u_Radius", settings.Radius * scale);

			// **The relief the map was baked with, not the body's amplitude.**
			// The shell decodes the map's red channel to find how deep the
			// water over it is, and a decode has to use the same scale the
			// encode did -- which is the terrain shader's `u_Relief`, and that
			// is `ReliefReach() * 2`. This said `Amplitude` while nothing read
			// it, and would have been quietly wrong by the ratio between them
			// the moment something did.
			water->Set("u_Relief", it->second.ReliefReach() * 2.0f * scale);
			water->Set("u_SeaRadius", settings.OceanRadius * scale);
			water->Set("u_SeaDepth",
				(float)((double)scale * (SeaRadiusOf(index, settings)
					- DrawnRadius(index))));
			water->Set("u_LightDirection", SunDirection(index));
			water->Set("u_LightColor", m_SunLight * m_StarBrightness);
			water->Set("u_Origin", centre);
			water->Set("u_Eye", -centre);
			water->Set("u_Shallow", settings.Shallow);
			water->Set("u_Deep", settings.Deep);
			water->Set("u_Time", (float)m_Time * 8766.0f);
			water->Set("u_WaveScale", 1.6f);

			// Metres of water that attenuate the return path by 1/e. Clear
			// lake water is a few; the open sea is more, but the sea here is
			// deep enough everywhere that it saturates either way.
			water->Set("u_Clarity", 3.5f);
			water->Set("u_HasDepth", 0.0f);
			water->Set("u_NearCentre", glm::vec3(0.0f, 1.0f, 0.0f));
			water->Set("u_NearCos", 2.0f);
		};

		auto material = Egss::Material::CreateInstance(m_WaterMaterial);
		dress(material);

		// No depth write, so two bits of sea do not occlude each other, and no
		// culling because the camera can be under it.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		bool local = m_WaterMesh && m_Walking && (size_t)m_Ground == index;

		// **The sphere stands back where the local surface is standing.** Both
		// are blended and neither writes depth, so two surfaces over the same
		// water blend twice and read as a darker disc round the player.
		if (local)
		{
			material->Set("u_NearCentre",
				glm::vec3(glm::normalize(m_Water.Site())));

			// **`DrawnReach`, not `Reach`.** The mesh leaves a margin off each
			// edge and the cone did not, so the shell stood back over a disc
			// 10% wider than anything that replaced it -- a ring of missing
			// sea at the far edge of the local sheet. See `DrawnReach`.
			material->Set("u_NearCos", std::cos(std::atan(
				m_Water.DrawnReach() / glm::max(settings.Radius, 1.0f))));
		}

		Egss::Renderer::Submit(material, m_Sphere,
			glm::scale(glm::translate(glm::mat4(1.0f), centre),
				glm::vec3(settings.OceanRadius * scale)));

		if (local)
		{
			auto nearby = Egss::Material::CreateInstance(m_WaterMaterial);
			dress(nearby);

			glm::dvec3 exact = BodyScene(index) - ShipScene();

			nearby->Set("u_HasDepth", 1.0f);

			Egss::Renderer::Submit(nearby, m_WaterMesh,
				glm::translate(glm::mat4(1.0f),
					glm::vec3(exact + ToScene(index, m_Water.Site())))
				* SpinMatrix(index));
		}

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	// The lander, where the physics has it. Drawn from the site's frame like
	// the rocks, and turned by the body's own orientation -- so a ship that
	// came down on a slope leans, and one that has been dug out from under
	// leans further.
	void DrawShip(const glm::dvec3& origin)
	{
		if (!m_HasShip || m_Ground < 0 || !m_Lander)
			return;

		size_t index = (size_t)m_Ground;

		const Egss::RigidBody3D& body = m_World.GetBody(m_Ship);

		auto hull = Egss::Material::CreateInstance(m_Material);
		hull->Set("u_Color", glm::vec4(0.72f, 0.74f, 0.78f, 1.0f));
		hull->Set("u_Emissive", 0.0f);
		hull->Set("u_LightPosition", SunDirection(index) * 40000.0f);
		hull->Set("u_LightColor", m_SunLight * m_StarBrightness);
		hull->Set("u_Sky", SkyLight(index));
		hull->Set("u_Up", m_Up);

		glm::dvec3 centre = BodyScene(index) - origin;

		glm::vec3 at = glm::vec3(centre
			+ ToScene(index, SiteFixed(body.Position)));

		// The hull's own origin is its base, and the capsule's is its middle,
		// so it is dropped by the half height it was given.
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), at)
			* SpinMatrix(index)
			* glm::mat4_cast(body.Orientation)
			* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.4f, 0.0f));

		Egss::Renderer::Submit(hull, m_Lander, transform);
	}

	// The loose bodies. Gravity acting on nothing you can see is gravity you
	// cannot check by looking. They live in the planet's own coordinates, so
	// they get the planet's offset like the chunks do.
	void DrawRocks(const glm::dvec3& origin)
	{
		if (m_Ground < 0)
			return;

		size_t index = (size_t)m_Ground;

		// **The bodies are in the landing site's frame, not the planet's.**
		// `SiteFixed` puts them back before the spin does, and the sum with
		// the body's centre stays in double until the last step -- the same
		// reason the chunk transforms do.
		glm::dvec3 centre = BodyScene(index) - origin;
		const std::vector<Egss::RigidBody3D>& bodies = m_World.GetBodies();

		for (const Egss::RigidBody3D& body : bodies)
		{
			if (body.Shape != Egss::ColliderShape3D::Sphere)
				continue;

			auto rock = Egss::Material::CreateInstance(m_Material);
			rock->Set("u_Color", glm::vec4(0.62f, 0.58f, 0.52f, 1.0f));
			rock->Set("u_Emissive", 0.0f);
			rock->Set("u_LightPosition", SunDirection(index) * 4000.0f);
			rock->Set("u_LightColor", m_SunLight * m_StarBrightness);
			rock->Set("u_Sky", SkyLight(index));
			rock->Set("u_Up", m_Up);

			glm::vec3 at = glm::vec3(centre
				+ ToScene(index, SiteFixed(body.Position)));

			glm::mat4 transform = glm::scale(
				glm::translate(glm::mat4(1.0f), at), glm::vec3(body.Radius));

			Egss::Renderer::Submit(rock, m_Sphere, transform);
		}
	}

	// Which way the star is from a body, in scene coordinates. **Directional,
	// not positional**: the Sun is 55 km away and a planet is 360 m across, so
	// a point light would light it from a lamp hung just above it.
	//
	// Nothing about the spin appears here any more -- see `SpinAngle`.
	glm::vec3 SunDirection(size_t index) const
	{
		glm::dvec3 toStar = BodyScene(0) - BodyScene(index);
		double length = glm::length(toStar);

		if (length <= 1e-4)
			return glm::vec3(0.0f, 1.0f, 0.0f);

		return glm::vec3(toStar / length);
	}

private:
	// --- Units --------------------------------------------------------------
	//
	// GM of the Sun in AU^3/yr^2. Not a measured constant here but a definition
	// of the units: an orbit at 1 AU takes 1 year, and `GM = 4*pi^2*a^3/T^2`.
	static constexpr double s_GmSun = 4.0 * 3.14159265358979323846 * 3.14159265358979323846;

	static constexpr double s_AuKm = 149597870.0;
	static constexpr double s_EarthRadiusKm = 6371.0;

	// **250 km, with both exponents at one -- so every ratio in the system is
	// the true one and only the absolute size is not.**
	//
	// It was 360 m. That made the atmosphere 16 m deep, twice the height of a
	// tree; it put Venus in the sky as a disc the size of a moon; and escape
	// velocity was 84 m/s. Those are not three problems, they are one: a body
	// too small for anything about it to feel like a body.
	//
	// With `p = q = 1` the map is the identity, so the Sun is its real half a
	// degree across, Venus is a point of light, the Moon's orbit is 60 Earth
	// radii and Phobos clears Mars by the 2.76 it actually clears it by. What
	// is not real is the metre: everything is 1/25.5 of true size.
	//
	// **The limit is a float, and it is exact.** The chunk meshes, the plant
	// positions and the physics bodies of the surface all live in planet-fixed
	// coordinates whose magnitude is the radius, and a float carries 24 bits --
	// so the spacing of representable positions at the surface is `R / 2^23`.
	// At Earth's own radius that is **0.76 m**, which is half a voxel: the
	// meshes come out as rubble and the physics with them, and a capture from
	// the ground shows the smooth stand-in sphere and nothing else, because
	// nothing else survived. 250 km puts it at 30 mm, which is under the
	// collider's own slop and a fiftieth of a voxel.
	//
	// Lifting it means carrying the surface in doubles or about a local origin
	// that follows the player -- through `VoxelField3D`, the mesher, the SDF
	// collider and the plant placement. That is the next thing this wants, and
	// it is on the roadmap with these numbers.
	// **How large Earth is drawn, and therefore everything else.** Every other
	// body is a ratio off this, so it is the one number that sets the scale of
	// the system.
	//
	// 250 km by default rather than 6,371: a planet is only worth the radius
	// you can afford to stream across, and at 1:1 the streamed 400 m is a flat
	// disc under a horizon 71 km away. The *representation* now goes all the
	// way -- see the 2026-08-25 changelog entry on the local origin, and
	// `--earth-radius 6371000`, which stands up, streams, and reports escape
	// velocity within 0.02% of the real 11,186 m/s.
	float m_EarthDrawn = 250000.0f;

	struct Body
	{
		std::string Name;
		int Parent = -1;
		double Gm = 0.0;          // GM of *this* body, for whatever orbits it
		double RotationHours = 24.0;
		float AxialTiltDegrees = 0.0f;
		float AtmosphereFraction = 0.0f;
		float AtmosphereDensity = 1.0f;
		float AtmosphereGlow = 0.0f;
		float BondAlbedo = 0.3f;
		glm::vec3 Scatter = glm::vec3(0.0f);
		float RingInner = 0.0f;
		float RingOuter = 0.0f;
		glm::vec3 RingColour = glm::vec3(1.0f);
		double RadiusKm = 0.0;
		glm::vec3 Colour = glm::vec3(1.0f);

		// Position and velocity relative to the parent, in AU and AU/yr. Kept
		// relative rather than absolute so a moon's orbit is a clean two-body
		// problem and does not inherit its planet's error.
		glm::dvec3 Position = glm::dvec3(0.0);
		glm::dvec3 Velocity = glm::dvec3(0.0);

		double SemiMajorAu = 0.0;

		// Period measurement: the angle it started at, and the times it has
		// come back to it.
		double StartAngle = 0.0;
		double LastAngle = 0.0;
		double Swept = 0.0;
		bool Seen = false;
		bool Aliased = false;
		double LastCrossing = -1.0;
		double MeasuredPeriod = 0.0;
		int Revolutions = 0;
	};

	void Reset()
	{
		const std::vector<BodyDescription>& table = Table();

		m_Bodies.clear();
		m_Time = 0.0;

		for (size_t i = 0; i < table.size(); i++)
		{
			const BodyDescription& description = table[i];

			Body body;
			body.Name = description.Name;
			body.Parent = description.Parent;
			body.Gm = s_GmSun * description.MassSuns;
			body.RadiusKm = description.RadiusKm;
			body.RotationHours = description.RotationHours;
			body.AxialTiltDegrees = description.AxialTiltDegrees;
			body.AtmosphereFraction = description.AtmosphereFraction;
			body.AtmosphereDensity = description.AtmosphereDensity;
			body.BondAlbedo = description.BondAlbedo;
			body.AtmosphereGlow = description.AtmosphereGlow;
			body.RingInner = description.RingInner;
			body.RingOuter = description.RingOuter;
			body.RingColour = description.RingColour;
			body.Scatter = description.Scatter;
			body.Colour = description.Colour;
			body.SemiMajorAu = description.SemiMajorAu;

			if (description.Parent >= 0)
			{
				double a = description.SemiMajorAu;
				double gm = m_Bodies[(size_t)description.Parent].Gm;

				// Started on the +x axis moving +z, which makes every orbit
				// circular and prograde. Real eccentricities are small enough
				// that circles look right, and a circle makes the period check
				// exact rather than approximate.
				body.Position = { a, 0.0, 0.0 };
				body.Velocity = { 0.0, 0.0, std::sqrt(gm / a) };

				body.StartAngle = std::atan2(body.Position.z, body.Position.x);
			}

			m_Bodies.push_back(body);
		}

		// The shortest orbit in the system decides the substep: a step longer
		// than a fraction of a period does not integrate an orbit, it invents
		// a polygon.
		double shortest = 1.0;

		for (const Body& body : m_Bodies)
		{
			if (body.Parent < 0)
				continue;

			double gm = m_Bodies[(size_t)body.Parent].Gm;
			double period = 2.0 * 3.14159265358979323846
				* std::sqrt(body.SemiMajorAu * body.SemiMajorAu * body.SemiMajorAu / gm);

			shortest = std::min(shortest, period);
		}

		m_ShortestPeriod = shortest;
		m_MaxStep = shortest / 256.0;
	}

	// Semi-implicit Euler, which is symplectic: it does not conserve energy
	// exactly but its error oscillates instead of accumulating, so an orbit
	// stays an orbit over thousands of revolutions. Plain Euler spirals
	// outward visibly within one.
	void Integrate(double h)
	{
		for (Body& body : m_Bodies)
		{
			if (body.Parent < 0)
				continue;

			double gm = m_Bodies[(size_t)body.Parent].Gm;

			glm::dvec3 r = body.Position;
			double distance = glm::length(r);

			if (distance <= 0.0)
				continue;

			glm::dvec3 acceleration = -r * (gm / (distance * distance * distance));

			body.Velocity += acceleration * h;
			body.Position += body.Velocity * h;
		}
	}

	// A revolution is 2*pi of swept angle, and the time of the crossing is
	// interpolated inside the step it happened in.
	//
	// **Both of those matter, and neither did in the first version.** Watching
	// for atan2's jump from +pi to -pi needs the sample to land in the last few
	// degrees before pi, which at a high time scale it usually does not -- the
	// Moon at one year a second moves 80 degrees a step and the marker is
	// simply missed. And taking the crossing to be at the end of whichever step
	// noticed it quantises every period to the step: at 1/60 yr a step, every
	// measured period came out an exact multiple of 0.01667, and Mercury -- the
	// fastest, at 14 steps an orbit -- read 3.1% short for that reason alone.
	//
	// Accumulating the swept angle fixes the first (it only needs two samples
	// per orbit), and interpolating fixes the second, which leaves the residual
	// being the integrator's own error rather than the sampling's.
	void TrackPeriods(double dt)
	{
		const double twoPi = 2.0 * 3.14159265358979323846;

		for (Body& body : m_Bodies)
		{
			if (body.Parent < 0)
				continue;

			double angle = std::atan2(body.Position.z, body.Position.x);

			if (!body.Seen)
			{
				body.Seen = true;
				body.LastAngle = angle;
				continue;
			}

			// The step's own turn, wrapped into (-pi, pi] so a lap boundary is
			// not read as a sudden reversal.
			double delta = angle - body.LastAngle;

			while (delta > 3.14159265358979323846) delta -= twoPi;
			while (delta < -3.14159265358979323846) delta += twoPi;

			body.LastAngle = angle;

			// Two samples an orbit is the floor for measuring one at all, and
			// past it the reading is not merely inaccurate but meaningless --
			// an aliased angle can report any period at all. Better to say
			// nothing: the panel shows a dash.
			if (std::abs(delta) >= 3.14159265358979323846)
			{
				body.Aliased = true;
				continue;
			}

			double before = body.Swept;
			body.Swept += delta;

			double target = twoPi * (double)(body.Revolutions + 1);

			if (before >= target || body.Swept < target || delta <= 0.0)
				continue;

			// Where in this step the lap actually closed.
			double fraction = (target - before) / delta;
			double crossing = m_Time - dt + fraction * dt;

			if (body.LastCrossing >= 0.0)
				body.MeasuredPeriod = crossing - body.LastCrossing;

			body.LastCrossing = crossing;
			body.Revolutions++;

			// Said out loud the first time a full period is measured, so a
			// headless run reports the check that the panel shows.
			if (body.Revolutions == 2 && !body.Aliased)
			{
				double predicted = PredictedPeriod(body);

				EGSS_TRACE("{0}: a = {1:.5f} AU, measured {2:.6f} yr, "
					"Kepler {3:.6f} yr, {4:+.4f}%",
					body.Name, body.SemiMajorAu, body.MeasuredPeriod, predicted,
					100.0 * (body.MeasuredPeriod - predicted) / predicted);
			}
		}
	}

	// **Surface gravity, in m/s^2, derived from the same table.**
	//
	// `g = GM / R^2` with GM in SI and R in metres. Nothing new is tabulated:
	// the mass is already there in solar masses and the radius in kilometres,
	// so this is another consequence of the data rather than another input to
	// disagree with it. Earth comes out at 9.82, Mars at 3.71, the Moon at
	// 1.62 -- which is the check, since none of those numbers is written down
	// anywhere here.
	double RealSurfaceGravity(size_t index) const
	{
		// GM of the Sun in SI, which is the one constant this needs that the
		// AU/year units did not.
		const double gmSun = 1.32712440018e20;

		// The Body carries GM in AU^3/yr^2; the mass in solar masses lives on
		// the description it was built from, and dividing by the Sun's own GM
		// recovers it without a second copy of the number.
		double massSuns = m_Bodies[index].Gm / s_GmSun;

		double gm = gmSun * massSuns;
		double radius = m_Bodies[index].RadiusKm * 1000.0;

		return radius > 0.0 ? gm / (radius * radius) : 0.0;
	}

	// The same gravity, on a planet a few hundred metres across instead of a
	// few thousand kilometres. `GM_local = g * R_local^2` keeps the *surface*
	// acceleration real while the body is toy-sized -- so a jump feels like a
	// jump, and the horizon is close.
	double LocalGm(size_t index) const
	{
		double radius = DrawnRadius(index);

		return RealSurfaceGravity(index) * radius * radius;
	}

	// Kepler's third law for this body, from the parent it actually orbits.
	// Nothing in the integrator knows this expression.
	double PredictedPeriod(const Body& body) const
	{
		if (body.Parent < 0)
			return 0.0;

		double gm = m_Bodies[(size_t)body.Parent].Gm;
		double a = body.SemiMajorAu;

		return 2.0 * 3.14159265358979323846 * std::sqrt(a * a * a / gm);
	}

	float Aspect() const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();
		float height = (float)window.GetHeight();

		return height > 0.0f ? (float)window.GetWidth() / height : 16.0f / 9.0f;
	}

	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				v_WorldPosition = world.xyz;
				v_Normal = mat3(u_Transform) * a_Normal;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_WorldPosition;
			in vec3 v_Normal;

			uniform vec4 u_Color;
			uniform vec3 u_LightPosition;
			uniform vec3 u_LightColor;
			uniform vec3 u_Sky;
			uniform vec3 u_Up;
			uniform float u_Emissive;

			void main()
			{
				vec3 normal = normalize(v_Normal);
				vec3 toLight = u_LightPosition - v_WorldPosition;

				// **No distance attenuation.** A point light falling off with
				// distance is right for a lamp in a room and wrong for a star:
				// sunlight at Neptune is a thirtieth of what it is here in
				// *irradiance*, but a planet is lit by a beam, not by a bulb it
				// is inside, and dividing by r^2 across 30 AU makes the outer
				// planets pure black. The terminator is the thing worth having,
				// so the direction is used and the distance is not.
				float diffuse = max(dot(normal, normalize(toLight)), 0.0);

				// **The same skylight the ground gets.** See the note in the
				// terrain shader: this was `0.06 + 0.94 * diffuse`, and six
				// per cent of a leaf colour is black. A forest on a slope
				// turned away from the sun came out as a single dark mass with
				// the trees indistinguishable from the hill they stood on --
				// which is what a wood looks like at night and not what one
				// looks like at ten in the morning.
				//
				// `u_Up` is one direction for everything drawn through this
				// shader, which is exact enough: the whole forest is inside
				// the load radius, and four hundred metres of a 250 km planet
				// is a tenth of a degree of vertical.
				float dome = 0.5 + 0.5 * dot(normal, u_Up);

				vec3 lit = u_Color.rgb * (u_Sky * dome + u_LightColor * diffuse);

				color = vec4(mix(lit, u_Color.rgb, u_Emissive), u_Color.a);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("SolarSystem", vertexSrc, fragmentSrc));

		// **The same lighting, fed a transform per copy instead of per draw.**
		//
		// Locations 0 to 2 are the mesh's own position, normal and texture
		// coordinate; a `mat4` attribute is four consecutive locations, so
		// `a_Model` occupies 3 through 6. `u_Transform` is still a uniform and
		// still carries the planet's placement and spin, because that is the
		// part every tree on a body shares -- only what differs goes down the
		// buffer.
		//
		// The fragment shader is the one above, unchanged. It reads a world
		// position and a normal and does not care where they came from.
		std::string treeVertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;
			layout(location = 3) in mat4 a_Model;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;

			void main()
			{
				mat4 model = u_Transform * a_Model;

				vec4 world = model * vec4(a_Position, 1.0);

				v_WorldPosition = world.xyz;
				v_Normal = mat3(model) * a_Normal;

				gl_Position = u_ViewProjection * world;
			}
		)";

		m_TreeShader.reset(
			Egss::Shader::Create("SolarSystemTrees", treeVertexSrc, fragmentSrc));

		m_TreeMaterial = Egss::Material::Create(m_TreeShader);
		m_Material = Egss::Material::Create(m_Shader);

		BuildTerrainShader();
		BuildWaterShader();
		BuildAtmosphereShader();
		BuildCloudShader();
	}

	// Terrain is coloured by **altitude and slope against the local up**, and on
	// a planet the local up is `normalize(position)` -- there is no global +Y to
	// measure a slope from. That one substitution is most of what makes a
	// spherical world different to write than a flat one.
	//
	// **The floating origin means the shader cannot use its own vertices for
	// that.** Positions arrive relative to the camera, not to the planet, so
	// the planet's centre is passed in and subtracted. Getting this wrong is
	// silent: the terrain still draws, coloured by distance from the camera.
	void BuildTerrainShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform vec3 u_Origin;

			// **A point on this draw's own ground, and the offset from it.**
			//
			// `v_Position` is planet-centred, which at 1:1 is about 6.4e6 --
			// and `float` has 24 bits, so it arrives on a 0.5 m grid and
			// `length(v_Position)` lands on one too. Every shading height in
			// this shader is that length minus a sea radius of the same size,
			// so the whole of what the terrain is made of is computed in the
			// bits that were thrown away.
			//
			// The fix is the one the CPU side already uses: keep the large
			// part off the GPU entirely. `u_Reference` is a point near this
			// draw in camera-relative coordinates -- a chunk's own origin --
			// so `world.xyz - u_Reference` is a difference of two small
			// numbers and is exact. The fragment stage rebuilds the height
			// from that offset plus a reference altitude the CPU computed in
			// double. See `u_ReferenceAltitude`.
			uniform vec3 u_Reference;

			out vec3 v_Position;
			out vec3 v_Offset;
			out vec3 v_Normal;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				// Back into the planet's own coordinates. The chunks arrive in
				// them already and the sphere does not (it is a unit sphere
				// scaled), so the subtraction has to happen after the
				// transform rather than before it.
				v_Position = world.xyz - u_Origin;
				v_Offset = world.xyz - u_Reference;
				v_Normal = mat3(u_Transform) * a_Normal;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_Position;
			in vec3 v_Offset;
			in vec3 v_Normal;

			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec3 u_Sky;
			uniform vec4 u_LowColour;
			uniform vec4 u_HighColour;
			uniform float u_Radius;
			uniform float u_Relief;
			uniform float u_SeaRadius;

			// **The waterline as a small number, because it never was one.**
			// `u_SeaRadius - u_Radius` is a few hundred metres taken as the
			// difference of two values near 6.4e6, so on the GPU it was a few
			// hundred metres known to half a metre. The CPU knows both in
			// double and can just send the answer.
			uniform float u_SeaDepth;

			// **The height, without ever forming a planet-sized number.**
			//
			// `u_ReferenceRadius` is `|C|` for a reference point `C` near this
			// draw, and `u_ReferenceAltitude` is `|C| - seaRadius` computed on
			// the CPU in double. What is left for the GPU is `|C + e| - |C|`
			// for the small offset `e = v_Offset`, and that is written as the
			// difference of the squares over the sum:
			//
			//     |C+e| - |C| = (2 |C| (n.e) + e.e) / (|C+e| + |C|)
			//
			// which is an identity, not an expansion -- there is no small-`e`
			// assumption in it, which matters because the horizon mesh's `e`
			// reaches fourteen kilometres. Every term is either small or only
			// needs relative precision: the numerator is about 1e7 with an ulp
			// of 1, the denominator about 1.3e7, so the quotient is good to
			// about 1e-7 m. The subtraction that used to destroy the answer
			// has been done in double before the value ever arrived.
			//
			// `u_HasReference` is 0 for the stand-in sphere, which has no
			// nearby point to measure from and is a body-sized ball seen from
			// space when it is drawn at all.
			uniform vec3 u_ReferenceNormal;
			uniform float u_ReferenceRadius;
			uniform float u_ReferenceAltitude;
			uniform float u_HasReference;

			// Camera distance, for the haze mix below -- v_Position is already
			// planet-centred, and u_Origin (also bound in the vertex stage) is
			// the planet's own centre relative to the camera, so their sum is
			// camera-relative again, which is what a distance wants.
			uniform vec3 u_Origin;
			uniform float u_HazeDensity;

			// The height at which the air thins by 1/e. The shell shader uses
			// the same number to march its density; see `u_AirScaleHeight`
			// where it is bound for why the two have to agree.
			uniform float u_AirScaleHeight;

			uniform sampler2D u_Map;
			uniform float u_HasMap;
			uniform float u_Vegetated;

			// Undoes the spin -- axis tilt included -- so the map can be
			// looked up in the frame it was baked in, and so latitude means
			// the same thing at every hour of the body's day. This used to
			// be one float, a shift along the map's azimuth, which was
			// exactly right while every body spun about +Y: rotating about
			// +Y is the one rotation an equirectangular longitude shift can
			// undo on its own, and it happens to leave latitude alone too.
			// A tilted axis needs the real inverse rotation, which is this
			// matrix's transpose.
			uniform mat4 u_Unspin;

			uniform vec3 u_Shallow;
			uniform vec3 u_Deep;
			uniform vec3 u_Sand;
			uniform vec3 u_Tropical;
			uniform vec3 u_Temperate;
			uniform vec3 u_Tundra;
			uniform vec3 u_Rock;
			uniform vec3 u_Snow;
			uniform vec3 u_Desert;
			uniform vec3 u_Steppe;

			// How far up the shore the sand reaches, in metres.
			uniform float u_Beach;

			// Whether this body has a climate to read out of the map's green
			// and blue. Only the one with a sea does.
			uniform float u_HasClimate;

			// **Two axes and an altitude, evaluated per pixel.**
			//
			// This used to be height and latitude, which is as much as there
			// was to know: with nothing but the map to read, latitude was the
			// only thing that varied across the surface. It gave banded
			// stripes, because stripes are what a function of latitude *is*.
			//
			// `moisture` and `warmth` come out of the drainage pass -- see
			// `VoxelPlanet::BuildHydrology`. Warmth is latitude with a lapse
			// rate on it, so it still bands, but moisture does not: it is
			// where the water in this particular landscape collects, which is
			// a function of the shape of the ground and of nothing else. That
			// is what stops a biome map looking like a filter over a globe.
			//
			// The order of the mixes is still the model. Sand loses to
			// whatever grows a metre inland, that loses to rock on a mountain,
			// rock loses to snow higher still, and cold comes last because an
			// ice cap is an ice cap whatever is under it.
			vec3 Biome(float height, float latitude, float moisture, float warmth)
			{
				float sea = u_SeaDepth;

				if (u_Vegetated < 0.5)
				{
					float t = clamp((height + sea) / max(u_Relief, 0.001) + 0.5, 0.0, 1.0);

					return mix(u_LowColour.rgb, u_HighColour.rgb, t);
				}

				// **Ground is ground, however low it is.**
				//
				// This returned ocean colour for anything below the
				// waterline, which is a statement about altitude and not
				// about water -- so the 3,425 km^2 across 435 basins that the
				// drainage pass correctly calls *dry* were painted sea, and
				// the planet from orbit was a uniform blue-grey ball with no
				// coastline anywhere on it. Whether there is water somewhere
				// is `SurfaceWater`'s answer and the wet mask's, and both of
				// them are drawn *as water*, in front of this. A seabed is
				// allowed to look like a seabed; what is over it decides what
				// colour it arrives as.

				float top = max(u_Relief * 0.5 - sea, 1.0);
				float f = clamp(height / top, 0.0, 1.0);

				// Without a climate map, fall back to the old latitude band --
				// which is exactly what a body with no water has to do.
				float wet = u_HasClimate > 0.5 ? moisture : 0.6;
				float warm = u_HasClimate > 0.5 ? warmth : 1.0 - latitude;

				// A Whittaker square, with the corners named. Dry and warm is
				// desert; wet and warm is forest; dry and cool is steppe; wet
				// and cool is the temperate green this planet used to be
				// everywhere between the tropics and the tundra.
				vec3 hot  = mix(u_Desert, u_Tropical,  smoothstep(0.30, 0.62, wet));
				vec3 mild = mix(u_Steppe, u_Temperate, smoothstep(0.34, 0.66, wet));

				vec3 green = mix(mild, hot, smoothstep(0.48, 0.78, warm));

				// **A beach is a few metres, not a percentage of the relief.**
				// This was `smoothstep(0.0, 0.05, height / top)`, which on a
				// planet with 625 m of relief put the sand 14 m up the hill --
				// so a tropical landing site read as pale coastal plain -- and
				// at 1:1, where the relief is 16 km, would have run it 400 m
				// up. It is a tide line: quoted in metres, from the size of
				// the voxels the ground is actually made of.
				vec3 colour = mix(u_Sand, green, smoothstep(0.0, u_Beach, height));

				colour = mix(colour, u_Rock, smoothstep(0.45, 0.75, f));
				colour = mix(colour, u_Snow, smoothstep(0.74, 0.93, f));

				// Cold last, and driven by warmth rather than by latitude, so
				// the tundra line bends round a highland instead of running
				// straight through it.
				colour = mix(colour, u_Tundra, smoothstep(0.30, 0.16, warm));
				colour = mix(colour, u_Snow, smoothstep(0.14, 0.05, warm));

				return colour;
			}

			void main()
			{
				vec3 normal = normalize(v_Normal);

				// Up is away from the centre, everywhere. `v_Position` is in
				// the planet's own coordinates, so the centre is the origin.
				vec3 up = normalize(v_Position);

				// The body-fixed direction, undoing the spin -- the frame the
				// map was baked in, and the frame a point's own latitude is
				// measured in. `up` itself still carries the day: it is what
				// lights a slope and reads a cliff as a cliff below.
				vec3 fixedUp = normalize(mat3(u_Unspin) * up);

				// **Meshed ground knows its own height exactly; the sphere has
				// to be told.** Reading the map for both would blur the coast
				// under your feet to the map's two metres a texel for no
				// reason -- the geometry in front of you is the answer.
				const float pi = 3.14159265;

				vec2 uv = vec2(atan(fixedUp.z, fixedUp.x) / (2.0 * pi) + 0.5,
					acos(clamp(fixedUp.y, -1.0, 1.0)) / pi);

				// **The climate is read from the map even when the height is
				// not.** Meshed ground knows its own height exactly and the
				// map's two metres a texel would only blur the coast under
				// your feet -- but moisture has no geometry to be read off,
				// so it comes from the map wherever you are standing. One
				// sample serves both.
				vec4 mapped = SampleSphere(u_Map, uv);

				float height;

				if (u_HasMap > 0.5)
					height = (mapped.r * 2.0 - 1.0) * u_Relief - u_SeaDepth;
				else if (u_HasReference > 0.5)
					height = u_ReferenceAltitude
						+ (2.0 * u_ReferenceRadius * dot(u_ReferenceNormal, v_Offset)
							+ dot(v_Offset, v_Offset))
						/ (u_ReferenceRadius + length(v_Position));
				else
					height = length(v_Position) - u_SeaRadius;


				vec3 base = Biome(height, abs(fixedUp.y), mapped.g, mapped.b);

				// Steep ground shows rock rather than the surface colour, which
				// is what makes a cliff read as a cliff.
				float flatness = clamp(dot(normal, up), 0.0, 1.0);
				base = mix(base * 0.55, base, smoothstep(0.35, 0.8, flatness));

				float diffuse = max(dot(normal, u_LightDirection), 0.0);

				// **The sky is a light source, and it was worth 5%.**
				//
				// This was `0.05 + 0.95 * diffuse`, where the 0.05 was a floor
				// to stop unlit ground reaching pure black. It produced
				// exactly what it was there to prevent: with the landscape
				// layer in, the demo opens facing a hillside that leans away
				// from the sun, and at five per cent of a dark green that is a
				// **silhouette**. The mountains were being drawn correctly and
				// could not be seen.
				//
				// On a body with air, a slope facing away from the sun is lit
				// by the whole dome above it -- which is why a shaded hillside
				// on Earth is blue-grey rather than black, and why the same
				// hillside on the Moon really is black. `u_Sky` carries the
				// atmosphere's own scattering colour and is zero for a body
				// with no atmosphere, so both come out of one expression.
				//
				// `0.5 + 0.5 * dot(normal, up)` is the share of the dome a
				// surface can see: one for flat ground, a half for a vertical
				// face, zero for an overhang. It is the geometric term, not a
				// tuning constant.
				float dome = 0.5 + 0.5 * dot(normal, up);

				vec3 lit = base * (u_Sky * dome + u_LightColor * diffuse);

				// **Haze, after lighting rather than before it.** This is air
				// between the eye and the ground, not a property of the
				// ground -- so it has to sit outside the `base * (...)`
				// lighting term above, or a shaded slope and a lit one would
				// haze by different amounts for no physical reason. Same
				// exp(-x) extinction shape the atmosphere shell already
				// raymarches, applied here as one term instead of a raymarch
				// because this is a single surface, not a volume. Zero
				// density -- an airless body -- leaves this the identity mix,
				// the same place u_Sky itself already goes to zero.
				float camDist = length(v_Position + u_Origin);

				// **How much air is on the path, not how long the path is.**
				//
				// This was `1.0 - exp(-camDist * u_HazeDensity)`, with the
				// full camera distance and nothing else. `u_HazeDensity` is
				// 9.9e-3 per metre for Earth here -- a half-hazed distance of
				// 70 m, which is the right order for standing in a landscape
				// and is what it was tuned against. From orbit `camDist` is
				// 750 km, the exponent is **7425**, and `haze` is 1.0 to the
				// bit: every land pixel on the disc came out exactly `u_Sky`.
				// The planet had no continents on it because the terrain was
				// never drawn, only the sky colour was -- and the mottling
				// that read as malformed ground was the atmosphere shell's
				// raymarch over a flat grey ball.
				//
				// The missing term is the air itself. Extinction is the
				// integral of density along the ray, and density falls off
				// exponentially with height; a path 750 km long that spends
				// all but four of those kilometres above the atmosphere
				// carries almost no air. Sampling the density at the midpoint
				// of the segment is the cheapest thing with the right limits:
				// on the ground both ends are at zero altitude, the factor is
				// 1, and the landed tuning is untouched to the last bit;
				// from orbit the midpoint is 375 km up, the factor underflows
				// to zero, and the ground is drawn as ground. Between the two
				// it falls off the way flying up out of the murk actually
				// looks.
				//
				// It is a near-field term and it stays one -- the honest
				// account of a long slant path is the shell's raymarch, which
				// is already drawn over the top of this.
				vec3 eye = -u_Origin;
				vec3 midway = 0.5 * (eye + v_Position);

				float midAltitude = max(length(midway) - u_SeaRadius, 0.0);
				float air = exp(-midAltitude / max(u_AirScaleHeight, 1.0));

				float haze = 1.0 - exp(-camDist * u_HazeDensity * air);
				lit = mix(lit, u_Sky, haze);

				color = vec4(lit, 1.0);
			}
		)";

		m_TerrainShader.reset(
			Egss::Shader::Create("PlanetSurface", vertexSrc,
				WithSphereSample(fragmentSrc)));

		m_TerrainMaterial = Egss::Material::Create(m_TerrainShader);
	}

	// **The sea is one sphere, and the map says where it is not.**
	//
	// Drawn after the terrain, so meshed ground standing above the waterline
	// occludes it by depth -- but only inside the streaming radius, and the
	// sphere goes all the way round. Past that there is no geometry to hide
	// behind and every continent would be underwater. So the shader reads the
	// same map the ground is coloured from, whose alpha is the height above
	// sea level ramped over a metre and a half, and discards where there is
	// land. That ramp is what antialiases the coastline; a hard test gives a
	// staircase you can count.
	//
	// Fresnel does two jobs at once: it is why a sea is a mirror at a grazing
	// angle and clear water straight down, and it is also exactly the alpha
	// that lets a sandbar show through near the shore without a second pass.
	void BuildWaterShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform vec3 u_Origin;

			out vec3 v_Position;
			out float v_Depth;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				v_Position = world.xyz - u_Origin;

				// Metres of water under this vertex, put there by
				// `SurfaceWater::BuildMesh`. The sphere carries none, and says
				// so with `u_HasDepth`.
				v_Depth = a_TexCoord.x;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_Position;
			in float v_Depth;

			uniform float u_HasDepth;

			uniform sampler2D u_Map;
			uniform mat4 u_Unspin;
			uniform float u_Radius;
			uniform float u_Relief;
			uniform float u_SeaRadius;

			// Sea radius less mean radius, taken in double on the CPU -- see
			// the terrain shader's own `u_SeaDepth` for why it is not a
			// subtraction done here.
			uniform float u_SeaDepth;

			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec3 u_Eye;            // camera, in the planet's frame
			uniform vec3 u_Shallow;
			uniform vec3 u_Deep;
			uniform float u_Time;
			uniform float u_WaveScale;
			uniform float u_Clarity;

			// The cone the local water surface covers. The sphere skips it, so
			// the two never blend over each other. Two means never.
			uniform vec3 u_NearCentre;
			uniform float u_NearCos;

			void main()
			{
				vec3 up = normalize(v_Position);

				if (dot(up, u_NearCentre) > u_NearCos)
					discard;

				// See the terrain shader's u_Unspin for why this is a matrix
				// rather than a shift along the map's azimuth.
				vec3 fixedUp = normalize(mat3(u_Unspin) * up);

				const float pi = 3.14159265;
				vec2 uv = vec2(atan(fixedUp.z, fixedUp.x) / (2.0 * pi) + 0.5,
					acos(clamp(fixedUp.y, -1.0, 1.0)) / pi);

				// **Where the map says there is no water, there is no water.**
				//
				// This used to re-derive the coastline from the height
				// channel -- discard wherever the ground is above sea level --
				// which can only express one idea: that water is everywhere
				// below a given radius. The drainage pass answers the question
				// the shader actually wants, which is whether water can *get*
				// here, and a basin ringed by land and floored below sea level
				// now correctly gets none.
				//
				// Half-open rather than half a metre inland: the mask is
				// bilinear across a texel, and taking anything above a
				// quarter as wet errs toward too much sea, which puts the
				// disagreement with the real geometry under the ground rather
				// than leaving a fringe of missing sea along every shore.
				// **Coverage, not a threshold** -- and the local sheet is not
				// asked at all.
				//
				// This was `if (mask < 0.25) discard`, which is a claim that
				// a texel is either sea or not. A texel is 1.5 km across and
				// a coastline runs through the middle of it, so at any real
				// distance it is *part* sea -- and once the mip chain landed
				// (2026-08-26) the averaged mask stopped dropping below 0.25
				// almost anywhere, so from orbit the shell covered the whole
				// disc and the planet had no land on it at all. That was
				// invisible only because the shell was 55% transparent and
				// the terrain under it was painting itself blue; take either
				// of those away and the continents vanish.
				//
				// Weighting the alpha by the mask is the same statement the
				// mip chain is already making: a half-wet texel is half a
				// pixel of sea. It antialiases the coastline for free instead
				// of stair-stepping it, and it costs nothing.
				//
				// The near mesh carries its own shoreline -- `SurfaceWater`
				// cut it against the terrain the mesher actually produced --
				// so gating that by a 1.5 km texel could only erode it.
				float wet = u_HasDepth > 0.5 ? 1.0 : SampleSphere(u_Map, uv).a;

				if (wet < 0.02)
					discard;

				vec3 view = normalize(u_Eye - v_Position);

				// Two crossed ripples, tilting the normal a degree or so. Not
				// a wave model -- there is no displacement and no wind -- but
				// enough that the specular breaks up instead of sitting on the
				// sphere as one perfect disc.
				vec3 tangent = normalize(cross(abs(up.y) < 0.9
					? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), up));
				vec3 bitangent = cross(up, tangent);

				float a = dot(v_Position, tangent) * u_WaveScale + u_Time * 1.7;
				float b = dot(v_Position, bitangent) * u_WaveScale * 1.31 - u_Time * 1.1;

				vec3 normal = normalize(up
					+ tangent * cos(a) * 0.03 + bitangent * cos(b) * 0.03);

				float facing = clamp(dot(normal, view), 0.0, 1.0);

				// Schlick, with water's 0.02 at normal incidence.
				float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);

				// **Water is coloured by how much of it there is.**
				//
				// This was `mix(u_Deep, u_Shallow, facing)` -- the view angle,
				// and only the view angle. That is a statement about how much
				// sky is being reflected and says nothing about what is
				// underneath, so a puddle two centimetres deep and a lake
				// forty metres deep came out the same colour and the whole
				// surface read as a blue sheet laid over the ground. It is the
				// single thing that makes water look painted on.
				//
				// Beer's law on the depth instead: light goes down through the
				// water, off the bottom, and back up, so the path is twice the
				// depth and what survives falls off exponentially. `u_Clarity`
				// is the depth at which that path has attenuated by `1/e`,
				// which for the clear water of a lake is a few metres.
				//
				// The view angle still has a job -- it is why a lake is a
				// mirror at a grazing angle -- but it is applied as Fresnel
				// below, where it belongs, rather than as a colour.
				// **The sphere has a depth too, and it comes off the map.**
				//
				// It had none: `u_HasDepth` was 0 for the shell and `sunk`
				// went straight to 1, so every ocean pixel on the planet was
				// the same flat `u_Deep` at a constant 0.55 alpha. Half the
				// terrain showed through it and the terrain was painting
				// itself blue underneath, which between them is the whole
				// reason the planet had no visible coastline from orbit.
				//
				// The map already carries the ground height in its red
				// channel -- the terrain shader decodes it the same way --
				// and the shell stands at sea level, so the depth under any
				// point of it is however far that ground is below zero.
				float depth = u_HasDepth > 0.5
					? max(v_Depth, 0.0)
					: max(u_SeaDepth
						- (SampleSphere(u_Map, uv).r * 2.0 - 1.0) * u_Relief, 0.0);

				float sunk = 1.0 - exp(-2.0 * depth / max(u_Clarity, 0.01));

				vec3 body = mix(u_Shallow, u_Deep, sunk);

				float diffuse = max(dot(normal, u_LightDirection), 0.0);

				// Blinn-Phong, tight enough to read as a sun glint rather than
				// a shine.
				// `half` is a reserved word in GLSL, which the compiler will
				// tell you in a way that does not mention that.
				vec3 midway = normalize(u_LightDirection + view);
				float glint = pow(max(dot(normal, midway), 0.0), 220.0);

				vec3 lit = body * (0.05 + 0.95 * diffuse) * u_LightColor
					+ u_LightColor * glint * fresnel * 12.0;

				// **And shallow water is see-through.** The same number
				// again: where the bottom is a hand's breadth down you are
				// looking at wet sand, not at water, and an opaque sheet
				// running right up the beach is the other half of why this
				// read as a texture. Fresnel still floors it, because even a
				// film is a mirror edge-on.
				// The same number again, and now the shell obeys it as well:
				// a shelf you can see the bottom of, an ocean you cannot.
				float alpha = clamp(mix(0.10, 0.92, sunk) + 0.55 * fresnel,
					0.0, 1.0) * clamp(wet, 0.0, 1.0);

				color = vec4(lit, alpha);
			}
		)";

		m_WaterShader.reset(Egss::Shader::Create("PlanetWater", vertexSrc,
			WithSphereSample(fragmentSrc)));
		m_WaterMaterial = Egss::Material::Create(m_WaterShader);
	}

	// **Single-scattering, marched along the view ray.**
	//
	// The shell is a sphere bigger than the planet, and every pixel of it is a
	// ray through a thin gas. What makes a sky blue is that scattering goes as
	// 1/lambda^4, so blue light is thrown sideways out of the beam far more
	// than red -- and what makes a sunset red is the same fact seen the other
	// way round, along a path so long that the blue has already been scattered
	// out before it arrives. Both fall out of integrating one expression, and
	// neither is drawn as a special case.
	//
	// Density falls off exponentially with height, so the integral has no
	// closed form and is marched: eight steps along the ray, four more toward
	// the sun at each. That is 32 samples a pixel, which is why the shell is
	// the only thing drawn this way.
	//
	// The same shader serves orbit and surface, which in one continuous space
	// is not a convenience but the requirement: flying down through the shell
	// has to turn the rim into a sky without a seam. From outside, the ray
	// enters and leaves and you see a rim of air; from inside, the near end of
	// the ray is the camera and you see a sky. The only difference is where the
	// segment starts, which the sphere intersection already knows.
	void BuildAtmosphereShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_World;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				v_World = world.xyz;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_World;

			uniform vec3 u_Centre;
			uniform vec3 u_CameraPosition;
			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec3 u_Scatter;
			uniform float u_PlanetRadius;
			uniform float u_AtmosphereRadius;
			uniform float u_Density;
			uniform float u_Glow;

			// Distances to a sphere's two intersections, or a miss.
			bool hitSphere(vec3 origin, vec3 direction, float radius,
			               out float near, out float far)
			{
				float b = dot(origin, direction);
				float c = dot(origin, origin) - radius * radius;
				float h = b * b - c;

				if (h < 0.0)
					return false;

				h = sqrt(h);
				near = -b - h;
				far  = -b + h;

				return true;
			}

			float density(vec3 at)
			{
				float height = length(at) - u_PlanetRadius;
				float thickness = u_AtmosphereRadius - u_PlanetRadius;

				// Scale height a quarter of the shell, so most of the air is
				// in the bottom of it, the way an atmosphere actually is.
				return exp(-max(height, 0.0) / (thickness * 0.25));
			}

			void main()
			{
				vec3 origin = u_CameraPosition - u_Centre;
				vec3 direction = normalize(v_World - u_CameraPosition);

				float near, far;
				if (!hitSphere(origin, direction, u_AtmosphereRadius, near, far))
					discard;

				// Inside the shell the segment starts at the camera, not at the
				// far side of a sphere behind it.
				near = max(near, 0.0);

				// **Stop where the light stops, which is not always the ground.**
				//
				// For a planet with a surface, that is the surface: air in
				// front of it is lit, air behind it is not there to be seen,
				// and the step in path length across the edge of the disc is
				// *correct* -- there is a hard edge there, and it is drawn.
				//
				// A gas giant has no surface, and stopping at the one the
				// voxel lattice happens to provide put a **hard circle across
				// Saturn**. Moving the stop below the surface only moved the
				// circle: what jumps is not the ground, it is the path length,
				// and eight fixed steps over a path that doubles at a boundary
				// are a different quadrature on either side of it. So a body
				// with no surface does not stop -- the ray crosses the whole
				// shell, `density` saturates below the drawn radius, and the
				// far half is buried under twenty-nine optical depths without
				// anything having to special-case it.
				if (u_Glow <= 0.0)
				{
					float groundNear, groundFar;

					if (hitSphere(origin, direction, u_PlanetRadius, groundNear, groundFar)
						&& groundNear > 0.0)
						far = min(far, groundNear);
				}

				if (far <= near)
					discard;

				// **Steps set by the scale height, not by a constant.** Eight
				// was enough for a shell 4.5% of Earth's radius and is not
				// enough for one that is 35% of Saturn's: what has to be
				// resolved is the scale height, which is a quarter of the
				// shell, and a step longer than half of that turns the limb
				// into bands. Two samples per scale height, capped so a ray
				// straight through a gas giant cannot cost more than 48.
				float thickness = u_AtmosphereRadius - u_PlanetRadius;

				int steps = int(clamp((far - near) / max(thickness * 0.125, 1e-4),
					8.0, 48.0));

				const int sunSteps = 4;

				float segment = (far - near) / float(steps);
				vec3 accumulated = vec3(0.0);
				float viewDepth = 0.0;

				// **Where to ask which way the star is.** The front of the drawn
				// sphere where the ray reaches it, and the closest approach to
				// the centre where it does not. The two agree at the limb, so
				// the terminator does not step across the edge of the disc.
				//
				// It has to be a point on a *shell*. Using the closest
				// approach everywhere is what the first version did, and for a
				// ray aimed at the middle of the planet that point is the
				// centre -- where `normalize` of very nearly zero swings
				// through every direction there is, and Saturn grew a bright
				// **cone** out of its terminator. Anchoring to the sphere
				// keeps `|deepest|` at or above the drawn radius everywhere.
				vec3 deepest;

				float surfaceNear, surfaceFar;

				if (hitSphere(origin, direction, u_PlanetRadius, surfaceNear, surfaceFar)
					&& surfaceNear > 0.0)
				{
					deepest = origin + direction * surfaceNear;
				}
				else
				{
					float approach = clamp(dot(-origin, direction), near, far);
					deepest = origin + direction * approach;
				}

				for (int i = 0; i < steps; i++)
				{
					vec3 at = origin + direction * (near + segment * (float(i) + 0.5));
					float local = density(at) * segment;

					viewDepth += local;

					// How much air the sunlight crossed to reach this point.
					float sunNear, sunFar;
					hitSphere(at, u_LightDirection, u_AtmosphereRadius, sunNear, sunFar);

					float sunSegment = max(sunFar, 0.0) / float(sunSteps);
					float sunDepth = 0.0;

					for (int j = 0; j < sunSteps; j++)
						sunDepth += density(at + u_LightDirection
							* (sunSegment * (float(j) + 0.5))) * sunSegment;

					// Out-scattering both ways: the light on the way in, and
					// what this sample sends toward the eye on the way out.
					// **This is where a sunset comes from** -- along a long
					// path the exponent is large, and it is largest for the
					// channel that scatters most, which is blue.
					vec3 transmittance =
						exp(-(sunDepth + viewDepth) * u_Scatter * u_Density);

					accumulated += transmittance * local;
				}

				// Rayleigh phase: scattering is strongest straight back toward
				// the light and toward the viewer, weakest at right angles.
				float cosAngle = dot(direction, u_LightDirection);
				float phase = 0.75 * (1.0 + cosAngle * cosAngle);

				vec3 result = accumulated * u_Scatter * u_Density * phase * u_LightColor;

				// **Multiple scattering, as one term rather than a second
				// integral -- because single scattering makes thick air
				// *dark*.**
				//
				// Every photon that bounces twice is dropped by the loop
				// above, and at Jupiter's optical depth almost all of them do:
				// the `exp(-(sunDepth + viewDepth))` inside kills the deep
				// samples, which are the ones carrying most of the mass. The
				// planet came out a **grey ball**, dimmer than the thin-aired
				// Earth beside it, which is backwards -- a gas giant is bright
				// precisely because its air is deep enough for light to bounce
				// around inside it before it leaves.
				//
				// Modelling that properly is a second scattering order at
				// least. What it is replaced with is one term with the two
				// properties that matter: it appears only where the air is
				// thick, because it is scaled by the same extinction that
				// fills the alpha channel, and it is lit by where the deepest
				// visible air sits relative to the star, so there is still a
				// terminator. The colour is the scattering colour with its
				// brightest channel at one, which is what the medium's albedo
				// would be.
				//
				// `u_Glow` is zero for Earth and Mars, so nothing about a thin
				// atmosphere changed when this was added.
				float lit = max(dot(normalize(deepest), u_LightDirection), 0.0);

				float brightest = max(max(u_Scatter.r, u_Scatter.g), u_Scatter.b);
				vec3 albedo = u_Scatter / max(brightest, 1e-4);

				// **And what the air hid on the way through.**
				//
				// The scattering integral above says how much light the air
				// sends to the eye. It says nothing about what the air stops,
				// and drawn additively it *cannot*: Jupiter came out as a hard
				// disc of ground with a bright halo round it, because 5.9
				// optical depths of hydrogen added a glow and then let every
				// bit of the surface through underneath.
				//
				// Alpha is the extinction along the same path -- one minus the
				// transmittance -- and the colour is already premultiplied by
				// its own coverage, because an integral of in-scattered light
				// is exactly that. `BlendMode::Premultiplied` composites the
				// two: `result + background * (1 - alpha)`. Where the air is
				// thin this is what additive already did; where it is thick
				// the planet stops being visible, which is the whole point of
				// a body with no surface.
				//
				// Grey rather than per-channel: the alpha channel is one
				// number, so a wavelength-dependent extinction cannot be
				// expressed here. The colour of what shows through is carried
				// by `result`, which is per-channel; the *amount* is the mean.
				float hidden = dot(u_Scatter * u_Density, vec3(1.0 / 3.0));
				float alpha = 1.0 - exp(-viewDepth * hidden);

				result += albedo * u_LightColor * lit * alpha * u_Glow;

				color = vec4(result, alpha);
			}
		)";

		m_AtmosphereShader.reset(
			Egss::Shader::Create("Atmosphere", vertexSrc, fragmentSrc));

		m_AtmosphereMaterial = Egss::Material::Create(m_AtmosphereShader);
	}

	// Draws one body's air, in camera-relative coordinates like everything
	// else -- so the camera is at the origin and the centre carries the offset.
	void DrawAtmosphere(size_t index, const glm::vec3& centre, float radius)
	{
		const Body& body = m_Bodies[index];

		if (body.AtmosphereFraction <= 0.0f)
			return;

		float outer = radius * (1.0f + body.AtmosphereFraction * m_AirScale);

		// Nothing to see from far enough away that the whole shell is under a
		// pixel, and 32 samples a pixel is worth skipping.
		if (glm::length(centre) > outer * 800.0f)
			return;

		auto material = Egss::Material::CreateInstance(m_AtmosphereMaterial);
		material->Set("u_Centre", centre);
		material->Set("u_CameraPosition", glm::vec3(0.0f));
		material->Set("u_LightDirection", SunDirection(index));
		material->Set("u_LightColor", m_SunLight * m_StarBrightness);
		material->Set("u_Scatter", body.Scatter);
		material->Set("u_PlanetRadius", radius);

		material->Set("u_AtmosphereRadius", outer);
		material->Set("u_Density",
			m_AirDensity * body.AtmosphereDensity / std::max(1.0f, radius));
		material->Set("u_Glow", body.AtmosphereGlow);

		// **Premultiplied, no depth write, no culling.** Premultiplied because
		// air both adds light and hides what is behind it, and additive can
		// only do the first -- see the note at the bottom of the fragment
		// shader for what that looked like on Jupiter. No depth write so one
		// shell does not occlude the next, and no culling because the camera
		// can be inside the shell, where only its back faces are visible.
		// Depth *testing* stays on, so terrain in front still occludes the sky.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Premultiplied);
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		Egss::Renderer::Submit(material, m_Sphere,
			glm::scale(glm::translate(glm::mat4(1.0f), centre), glm::vec3(outer)));

		// Put the pipeline back. Application::ResetState does this once a frame
		// anyway, but leaving it set means whatever draws next in *this* frame
		// inherits it -- which is the render-state leak of 2026-08-17.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	// **A coverage map on a shell, not a second scattering integral.** The
	// atmosphere above already answers "how does the air itself look"; this
	// only answers "is there a cloud between the eye and whatever is behind
	// it", which is one texture sample and a lighting term, not a raymarch.
	void BuildCloudShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_World;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				v_World = world.xyz;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_World;

			uniform vec3 u_Centre;
			uniform sampler2D u_CloudMap;
			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec3 u_Sky;

			void main()
			{
				// The mesh itself was rotated by the cloud layer's own drift
				// matrix before this fragment was produced, so reading the
				// map straight from the rotated position -- no unspin, unlike
				// the terrain -- is exactly what makes the pattern drift: a
				// fixed point on the map corresponds to a moving point on the
				// sphere as the drift angle grows.
				vec3 up = normalize(v_World - u_Centre);

				const float pi = 3.14159265;
				vec2 uv = vec2(atan(up.z, up.x) / (2.0 * pi) + 0.5,
					acos(clamp(up.y, -1.0, 1.0)) / pi);

				float coverage = SampleSphere(u_CloudMap, uv).a;

				if (coverage < 0.02)
					discard;

				float lit = max(dot(up, u_LightDirection), 0.0);

				// Sky-lit rather than pure black on the night side, the same
				// reason the terrain carries u_Sky.
				vec3 shade = u_LightColor * lit + u_Sky * 0.3;

				// Premultiplied: this is a surface that both reflects light
				// and hides what is behind it, same as the atmosphere shell.
				color = vec4(shade * coverage, coverage);
			}
		)";

		m_CloudShader.reset(Egss::Shader::Create("Clouds", vertexSrc,
			WithSphereSample(fragmentSrc)));
		m_CloudMaterial = Egss::Material::Create(m_CloudShader);
	}

	void DrawClouds(size_t index, const glm::vec3& centre, float radius)
	{
		const Body& body = m_Bodies[index];

		if (body.AtmosphereFraction <= 0.0f)
			return;

		auto it = m_Planets.find(index);

		if (it == m_Planets.end() || !it->second.CloudMap())
			return;

		float outer = radius * (1.0f + body.AtmosphereFraction * m_AirScale);

		// A third of the way up the shell: real clouds sit low in a real
		// atmosphere, and this is meant to read as a cloud deck rather than
		// a second, redundant atmosphere boundary.
		float shell = radius + (outer - radius) * 0.35f;

		// Same distance cull as the atmosphere shell it sits inside.
		if (glm::length(centre) > shell * 800.0f)
			return;

		auto material = Egss::Material::CreateInstance(m_CloudMaterial);
		material->Set("u_Centre", centre);
		material->SetTexture("u_CloudMap", it->second.CloudMap(), 0);
		material->Set("u_LightDirection", SunDirection(index));
		material->Set("u_LightColor", m_SunLight * m_StarBrightness);
		material->Set("u_Sky", SkyLight(index));

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Premultiplied);
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		Egss::Renderer::Submit(material, m_Sphere,
			glm::translate(glm::mat4(1.0f), centre) * CloudMatrix(index)
			* glm::scale(glm::mat4(1.0f), glm::vec3(shell)));

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	// **Labels, because a 302 km void has no landmarks.** Flying to Mars means
	// knowing which speck is Mars, and at 350 m across from 30 km away every
	// planet is one pixel. Projected through the same view-projection the
	// scene used, so a label sits exactly on its body.
	void DrawLabels()
	{
		if (m_Walking || !m_ShowLabels)
			return;

		ImDrawList* draw = ImGui::GetForegroundDrawList();
		ImGuiViewport* viewport = ImGui::GetMainViewport();

		glm::dvec3 origin = ShipScene();
		const glm::mat4& viewProjection = m_Camera.GetViewProjectionMatrix();

		for (size_t i = 0; i < m_Bodies.size(); i++)
		{
			glm::dvec3 offset = BodyScene(i) - origin;
			double distance = glm::length(offset);

			glm::vec4 clip = viewProjection * glm::vec4(glm::vec3(offset), 1.0f);

			if (clip.w <= 0.0f)
				continue;

			glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);

			if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
				continue;

			ImVec2 at(viewport->Pos.x + (ndc.x * 0.5f + 0.5f) * viewport->Size.x,
				viewport->Pos.y + (0.5f - ndc.y * 0.5f) * viewport->Size.y);

			const glm::vec3& colour = m_Bodies[i].Colour;
			ImU32 tint = ImGui::ColorConvertFloat4ToU32(
				ImVec4(colour.r, colour.g, colour.b, 0.85f));

			char text[64];
			std::snprintf(text, sizeof(text), "%s  %.1f km",
				m_Bodies[i].Name.c_str(), distance / 1000.0);

			draw->AddCircle(at, 6.0f, tint, 12, 1.5f);
			draw->AddText(ImVec2(at.x + 9.0f, at.y - 7.0f), tint, text);
		}
	}

	void OnDemoImGui() override
	{
		DrawLabels();

		ImGui::Begin("Solar system");

		ImGui::Text("t = %.3f years", m_Time);

		double altitude = glm::length(m_Local) - DrawnRadius(m_Frame);

		ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s %s, %.1f m up",
			m_Walking ? "on" : "near", m_Bodies[m_Frame].Name.c_str(), altitude);

		WeatherPanel();

		if (m_Walking)
		{
			VoxelPlanet& planet = m_Planets[(size_t)m_Ground];

			ImGui::Text("%zu chunks meshed, %zu triangles",
				planet.MeshedChunks(), planet.TriangleCount());

			int perStride[3];
			size_t trianglesPerStride[3];
			planet.LodCounts(perStride, trianglesPerStride);

			ImGui::Text("  stride 1: %d chunks, %zu tris", perStride[0], trianglesPerStride[0]);
			ImGui::Text("  stride 2: %d chunks, %zu tris", perStride[1], trianglesPerStride[1]);
			ImGui::Text("  stride 4: %d chunks, %zu tris", perStride[2], trianglesPerStride[2]);

			double gm = LocalGm((size_t)m_Ground);
			double here = glm::length(m_Local);
			double radius = DrawnRadius((size_t)m_Ground);

			ImGui::Text("surface gravity %.2f m/s^2 (real %.2f), here %.2f",
				gm / (radius * radius), RealSurfaceGravity((size_t)m_Ground),
				gm / (here * here));

			ImGui::Text("escape velocity %.1f m/s, orbit %.1f m/s",
				std::sqrt(2.0 * gm / here), std::sqrt(gm / here));

			ImGui::Text("%s", m_Submersion > 0.0f
				? (m_EyeUnderwater ? "underwater" : "wading")
				: m_Grounded ? "on the ground" : "falling");

			if (m_Submersion > 0.0f)
			{
				ImGui::SliderFloat("Buoyancy", &m_BuoyancyStrength, 0.5f, 4.0f,
					"%.2fx body weight");
				ImGui::SliderFloat("Water drag", &m_WaterDrag, 0.0f, 20.0f, "%.1f");
			}

			ImGui::SliderFloat("Dig radius", &m_DigRadius, 0.5f, 12.0f, "%.1f m");

			ImGui::Text("%d edits, %zu chunks changed  (mouse: left digs, "
				"right fills)", m_Edits, planet.EditedChunks());

			// What the climate says about where you are standing, which is
			// otherwise only inferable from the colour of the ground.
			if (planet.Water().Valid())
			{
				glm::vec3 here = glm::vec3(glm::normalize(
					SiteFixed(m_World.GetBody(m_Player).Position)));

				float wet = planet.SampleHydrology(planet.Water().Moisture, here);
				float warm = planet.SampleHydrology(planet.Water().Warmth, here);

				ImGui::Text("moisture %.2f, warmth %.2f  (%s)", wet, warm,
					warm < 0.22f ? "ice"
					: wet < 0.42f ? (warm > 0.5f ? "desert" : "steppe")
					: (warm > 0.5f ? "tropical" : "temperate"));
			}

			ImGui::Text("%d trees in view (%d / %d / %d by detail)",
				m_PlantsDrawn, (int)(m_TreeBatch[0][0].size() + m_TreeBatch[1][0].size()
					+ m_TreeBatch[2][0].size()),
				(int)(m_TreeBatch[0][1].size() + m_TreeBatch[1][1].size()
					+ m_TreeBatch[2][1].size()),
				(int)(m_TreeBatch[0][2].size() + m_TreeBatch[1][2].size()
					+ m_TreeBatch[2][2].size()));

			ImGui::Text("%u draws, %.2f M triangles", m_Stats.DrawCalls,
				m_Stats.TriangleCount / 1.0e6f);

			if (m_HasShip)
			{
				float away = DistanceToShip();

				if (away <= s_BoardingReach)
					ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
						"at the ship -- press L to board and lift off");
				else
					ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.5f, 1.0f),
						"the ship is %.0f m away", away);
			}

			if (ImGui::Button("Take off  (L)"))
				TakeOff();
		}
		else
		{
			ImGui::Text("%.0f m/s  (WASD, space/ctrl, shift to boost)", m_FlightSpeed);

			double aboveSea = 0.0;

			if (GroundUnderShip(aboveSea) && aboveSea < 0.0)
				ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
					"over water -- %.0f m of it, and nothing here swims", -aboveSea);

			if (ImGui::Button("Land  (L)"))
				Land();
		}

		ImGui::Separator();

		// Logarithmic, because the useful range spans seven decades: the
		// default is 7.6e-7 and a linear slider cannot represent it at all.
		ImGui::SliderFloat("Years per second", &m_OrbitalYearsPerSecond, 1.0e-7f, 2.0f,
			"%.3g", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Seconds per day", &m_SecondsPerDay, 4.0f, 7200.0f, "%.0f s",
			ImGuiSliderFlags_Logarithmic);

		// Said as durations rather than as rates, because "7.6e-7 yr/s" is not
		// a thing anybody can picture and "a year takes 365 h" is.
		ImGui::Text("a year takes %.4g h, and %s day %.0f s",
			1.0 / (glm::max(m_YearsPerSecond, 1e-12) * 3600.0),
			m_Frame == 0 ? "the Sun's" : m_Bodies[m_Frame].Name.c_str(),
			std::abs(m_Bodies[m_Frame].RotationHours) / (365.25 * 24.0)
				/ glm::max(m_YearsPerSecond, 1e-12));

		if (ImGui::SliderFloat("Orbits p", &m_Compression, 0.3f, 1.0f, "%.3f"))
			ReportScale();

		// Past about 0.97 the Sun reaches Mercury's orbit, which the clearance
		// column in the startup log is there to show you happening.
		if (ImGui::SliderFloat("Bodies q", &m_BodyScale, 0.4f, 1.0f, "%.3f"))
			ReportScale();

		ImGui::SliderFloat("Star brightness", &m_StarBrightness, 0.2f, 3.0f);
		ImGui::SliderFloat("Air depth", &m_AirScale, 0.2f, 6.0f, "%.1fx");
		ImGui::SliderFloat("Air density", &m_AirDensity, 1.0f, 120.0f, "%.0f");
		ImGui::SliderFloat("Haze", &m_HazeScale, 0.0f, 1.5e-3f, "%.6f");
		ImGui::SliderFloat("Load radius", &m_LoadRadius, 80.0f, 900.0f, "%.0f m");

		ImGui::Checkbox("Terrain LOD", &m_Lod);
		ImGui::SliderFloat("Stride 2 beyond", &m_LodNear, 24.0f, 400.0f, "%.0f m");
		ImGui::SliderFloat("Stride 4 beyond", &m_LodFar, 48.0f, 600.0f, "%.0f m");
		ImGui::SliderInt("Chunks per step", &m_ChunksPerStep, 1, 48);
		ImGui::SliderFloat("Stream budget", &m_StreamTargetMs, 0.5f, 12.0f, "%.1f ms");

		ImGui::TextDisabled("streaming %.2f ms, %.2f chunks a step%s", m_StreamMs,
			Egss::Application::Get().IsLockstep()
				? (float)m_ChunksPerStep : m_StreamAllowance,
			Egss::Application::Get().IsLockstep() ? " (lockstep: fixed)" : "");
		ImGui::Checkbox("Labels", &m_ShowLabels);

		ImGui::TextDisabled("Earth %.0f m across, 1 AU = %.1f km, system %.0f km wide",
			2.0 * DrawnRadius(3), DrawnLength(s_AuKm, (double)m_Compression) / 1000.0,
			2.0 * DrawnLength(30.07 * s_AuKm, (double)m_Compression) / 1000.0);

		if (m_HandoverResidual > 0.0)
			ImGui::TextDisabled("worst frame handover: %.3e m", m_HandoverResidual);

		ImGui::Separator();

		// **The check.** The integrator knows only an inverse-square force; the
		// right-hand column is Kepler's third law, which it has never been
		// told. A row that disagrees is either too few substeps or a bug.
		if (ImGui::BeginTable("bodies", 6,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("body");
			ImGui::TableSetupColumn("a (AU)");
			ImGui::TableSetupColumn("measured");
			ImGui::TableSetupColumn("Kepler");
			ImGui::TableSetupColumn("error");
			ImGui::TableSetupColumn("go");
			ImGui::TableHeadersRow();

			for (size_t i = 0; i < m_Bodies.size(); i++)
			{
				const Body& body = m_Bodies[i];

				if (body.Parent < 0)
					continue;

				double predicted = PredictedPeriod(body);

				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::TextColored(ImVec4(body.Colour.r, body.Colour.g, body.Colour.b, 1.0f),
					"%s", body.Name.c_str());

				ImGui::TableNextColumn();
				ImGui::Text("%.4f", body.SemiMajorAu);

				ImGui::TableNextColumn();
				if (body.Aliased)
					ImGui::TextDisabled("too fast");
				else if (body.Revolutions > 1)
					ImGui::Text("%.6f", body.MeasuredPeriod);
				else
					ImGui::TextDisabled("--");

				ImGui::TableNextColumn();
				ImGui::Text("%.5f", predicted);

				ImGui::TableNextColumn();
				if (!body.Aliased && body.Revolutions > 1 && predicted > 0.0)
					ImGui::Text("%+.4f%%",
						100.0 * (body.MeasuredPeriod - predicted) / predicted);
				else
					ImGui::TextDisabled("--");

				// A teleport, and labelled one -- for framing a capture, not
				// for getting about. Flying there is the demo.
				ImGui::TableNextColumn();
				ImGui::PushID((int)i);
				if (!m_Walking && ImGui::SmallButton("go"))
					GoTo(i, 6.0);
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::Separator();
		ImGui::Text("shortest orbit %.5f yr, substep %.6f yr",
			m_ShortestPeriod, m_MaxStep);

		if (ImGui::Button("Restart"))
		{
			Reset();
			GoTo(3, 4.0);
		}

		ImGui::End();
	}

	Egss::PerspectiveCamera m_Camera;

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;
	std::shared_ptr<Egss::Mesh> m_Sphere;
	std::shared_ptr<Egss::Mesh> m_Ring;
	std::shared_ptr<Egss::Mesh> m_Stars;
	std::shared_ptr<Egss::Shader> m_StarShader;
	std::shared_ptr<Egss::Material> m_StarMaterial;
	std::shared_ptr<Egss::Shader> m_FieldShader;
	std::shared_ptr<Egss::Material> m_FieldMaterial;
	std::shared_ptr<Egss::Shader> m_RingShader;
	std::shared_ptr<Egss::Material> m_RingMaterial;

	std::shared_ptr<Egss::Shader> m_AtmosphereShader;
	std::shared_ptr<Egss::Material> m_AtmosphereMaterial;

	std::shared_ptr<Egss::Shader> m_CloudShader;
	std::shared_ptr<Egss::Material> m_CloudMaterial;

	// **The light the Sun emits, which is not the colour the Sun looks.**
	//
	// The star is drawn warm because that is how it appears from inside an
	// atmosphere -- but the reason it appears warm is that the air scatters the
	// blue out on the way in, and the scattering shader computes exactly that.
	// Feeding it the apparent colour applies the reddening twice, which turned
	// Earth's sky green: scatter (0.22, 0.45, 1.00) times a light of
	// (1.00, 0.86, 0.42) is (0.22, 0.39, 0.42), and that is a cyan-green, not a
	// blue. Sunlight above the air is very nearly white.
	glm::vec3 m_SunLight = { 1.0f, 0.97f, 0.92f };

	float m_AirScale = 1.0f;
	float m_AirDensity = 26.0f;

	// Scales body.AtmosphereDensity into the terrain shader's per-metre haze
	// extinction. The half-hazed distance this puts Earth's own density
	// (3.0) at is under 100 m, well short of the real 949 m horizon at this
	// scale -- a formula sized to the horizon measured all but flat over the
	// few hundred metres a landed view actually shows, so it was retuned
	// against a capture instead: this is the value at which the foreground
	// stays clean and the treeline a few hundred metres out visibly greys
	// toward the sky.
	float m_HazeScale = 3.3e-3f;

	// The weather where the camera is. Recomputed once a fixed step rather
	// than per frame or per draw: several things read it -- the panel, and
	// shortly the wind on the ship and the trees -- and they all have to be
	// looking at the same instant, or a gust pushes the player and not the
	// grass beside them.
	Climate::Site m_WeatherSite;
	Climate::Weather m_Weather;
	bool m_HasWeather = false;

	std::shared_ptr<Egss::Shader> m_TerrainShader;
	std::shared_ptr<Egss::Material> m_TerrainMaterial;

	std::shared_ptr<Egss::Shader> m_WaterShader;
	std::shared_ptr<Egss::Material> m_WaterMaterial;

	static constexpr int s_TreeShapes = 3;

	// Three levels, switching at these distances over the tree's own scale.
	// 45 m is about a hundred pixels of tree at this field of view and 150 m
	// is about forty; past that a trunk is three pixels wide and five sides
	// of it are four more than anyone can see.
	static constexpr int s_TreeLods = 3;
	static constexpr float s_TreeLodNear = 45.0f;
	static constexpr float s_TreeLodFar = 150.0f;

	// One instance buffer per level, because a buffer belongs to the vertex
	// arrays it was added to and the levels are different vertex arrays. Nine
	// buffers of 16,384 matrices is 9.4 MB of VRAM that is mostly never
	// written -- cheap next to the alternative of re-declaring attributes.
	std::shared_ptr<Egss::Mesh> m_TreeBark[s_TreeShapes][s_TreeLods];
	std::shared_ptr<Egss::Mesh> m_TreeLeaves[s_TreeShapes][s_TreeLods];
	std::shared_ptr<Egss::VertexBuffer> m_TreeInstances[s_TreeShapes][s_TreeLods];
	std::vector<glm::mat4> m_TreeBatch[s_TreeShapes][s_TreeLods];

	std::shared_ptr<Egss::Shader> m_TreeShader;
	std::shared_ptr<Egss::Material> m_TreeMaterial;

	// Room for every tree the streaming radius can hold: 14 a chunk over a few
	// thousand chunks, with headroom. A megabyte of matrices a shape.
	static constexpr size_t s_MaxTreesPerShape = 16384;
	int m_PlantsDrawn = 0;

	// Last frame's totals, read back after EndScene. The panel is the only
	// consumer, but it is the number every performance question here has
	// started from.
	Egss::Renderer::Statistics m_Stats;

	// Generated on approach and kept: regenerating a planet is a density
	// evaluation for every voxel of its shell.
	std::unordered_map<size_t, VoxelPlanet> m_Planets;

	// --- Where the camera is ------------------------------------------------

	float m_Compression = 1.0f;

	// The body exponent. See `DrawnLength` for why it is three quarters and
	// what breaks on either side of that.
	float m_BodyScale = 1.0f;

	size_t m_Frame = 3;                       // whose frame m_Local is in
	glm::dvec3 m_Local = glm::dvec3(0.0);     // ship, relative to that body

	glm::vec3 m_Forward = { 0.0f, 0.0f, -1.0f };
	glm::vec3 m_Up = { 0.0f, 1.0f, 0.0f };

	bool m_Reported = false;
	double m_HandoverResidual = 0.0;
	double m_FlightSpeed = 0.0;

	double m_SpeedPerMetre = 0.6;
	float m_MinSpeed = 6.0f;
	float m_MaxSpeed = 3.0e7f;
	float m_LevelRate = 2.0f;
	bool m_ShowLabels = true;

	// --- On the ground ------------------------------------------------------

	bool m_Walking = false;
	int m_Ground = -1;                        // the planet the world belongs to

	float m_SurfaceYaw = 0.0f;
	float m_SurfacePitch = 0.0f;
	float m_EyeHeight = 1.2f;
	float m_WalkSpeed = 7.0f;
	float m_JumpSpeed = 9.0f;
	float m_JumpCooldown = 0.0f;
	float m_PlayerHalfHeight = 1.3f;   // capsule half height plus its radius
	bool m_Grounded = false;

	// How much of the capsule the local water covers, feet to head: 0 dry,
	// 1 submerged past the top. Kept between fixed steps because the render
	// frame that reads it may not coincide with the step that set it.
	float m_Submersion = 0.0f;
	bool m_EyeUnderwater = false;

	// Real buoyancy is Archimedes' constant -- weight of the water displaced
	// -- but a human floats near the surface either way, so a spring that
	// settles there stands in for it. At equilibrium (weight = buoyancy)
	// submersion sits at 1 / Strength, so 1.5 floats with roughly a third of
	// the capsule showing.
	float m_BuoyancyStrength = 1.5f;
	float m_WaterDrag = 4.0f;

	Egss::OrthographicCamera m_TintCamera{ -1.0f, 1.0f, -1.0f, 1.0f };

	Egss::PhysicsWorld3D m_World;
	Egss::PhysicsWorld3D::BodyHandle m_Player = 0;

	// The lattice point the surface physics world is centred on, and where it
	// is in the planet's own frame. Set when a landing builds the world.
	glm::ivec3 m_SiteLattice = glm::ivec3(0);
	glm::dvec3 m_SiteFixed = glm::dvec3(0.0);

	static constexpr float s_LandingClearing = 20.0f;

	SurfaceWater m_Water;
	HorizonMesh m_Horizon;
	glm::dvec3 m_HorizonSite { 0.0 };
	bool m_HorizonReported = false;
	std::shared_ptr<Egss::Mesh> m_WaterMesh;

	std::shared_ptr<Egss::Mesh> m_Lander;
	Egss::PhysicsWorld3D::BodyHandle m_Ship = 0;
	bool m_HasShip = false;

	PocketDimension m_Pocket;
	// The walking camera, in the planet's own frame. See UpdateSurface.
	glm::dvec3 m_EyeFixed = glm::dvec3(0.0);
	glm::vec3 m_UpFixed = glm::vec3(0.0f, 1.0f, 0.0f);
	glm::vec3 m_ForwardFixed = glm::vec3(0.0f, 0.0f, 1.0f);
	size_t m_WaterChunks = 0;

	float m_DigRadius = 2.5f;
	float m_DigReach = 12.0f;
	int m_Edits = 0;
	bool m_WasCutting = false;
	bool m_WasFilling = false;
	float m_LookSpeed = 0.12f;

	std::pair<float, float> m_LastMouse = { 0.0f, 0.0f };
	bool m_HasMouse = false;
	bool m_MouseLook = false;
	bool m_WasToggling = false;
	float m_LookRate = 90.0f;         // degrees a second, for the arrow keys

	// What the last step's streaming cost, and how many chunks that bought.
	double m_StreamMs = 0.0;
	float m_StreamAllowance = 1.0f;
	float m_StreamCredit = 0.0f;

	// Of a 16.7 ms frame. The rest of it has to draw the planet.
	float m_StreamTargetMs = 5.0f;

	// Where the air stops carrying a craft round with it, as a fraction of the
	// atmosphere's thickness. Below this the coupling is total.
	static constexpr double s_AirGripTop = 0.75;

	float m_LoadRadius = 400.0f;

	// Terrain level of detail, in metres from the camera at Earth's voxel
	// size. See the note where they are handed to the planet.
	bool m_Lod = true;
	float m_LodNear = 100.0f;
	float m_LodFar = 200.0f;
	int m_ChunksPerStep = 12;

	// --- The clocks ---------------------------------------------------------

	// **An hour to the day, and 365 hours to the year.**
	//
	// Both numbers are one number: near a body the clock is set so *that
	// body's* day takes `m_SecondsPerDay`, and far from one it runs at
	// `m_OrbitalYearsPerSecond`. Asking for a 3,600 s day on Earth implies
	// `(23.934 / 8766) / 3600 = 7.584e-7` yr/s, and 365 hours to the year is
	// `1 / (365 * 3600) = 7.610e-7`. They agree to a third of a percent, so
	// the handover at six radii is not a visible change of pace -- which was
	// not true of the 60 s day this started with, where the two differed by
	// four orders of magnitude and leaving a planet made the sky lurch.
	//
	// The cost is that nothing completes an orbit while you watch: Mercury's
	// 88 days is 88 hours of sitting there, so the Kepler column on the panel
	// stays empty at this rate. That is what the slider is for, and it is
	// logarithmic so the seven decades between "a year in a fortnight" and "a
	// year in half a second" are all reachable.
	float m_SecondsPerDay = 3600.0f;
	float m_OrbitalYearsPerSecond = 1.0f / (365.0f * 3600.0f);
	double m_YearsPerSecond = 1.0 / (365.0 * 3600.0);

	std::vector<Body> m_Bodies;

	double m_Time = 0.0;

	// Radians of drift a year, about the body's own spin axis -- independent
	// of the body's real rotation rate, so cloud bands slide over the
	// terrain instead of staying glued to whatever coastline is under them.
	// Arbitrary and purely visual; picked for one full drift in about nine
	// days of simulated time.
	double m_CloudDrift = 0.0;
	double m_CloudDriftRate = 2.0 * 3.14159265358979323846 * (365.25 / 9.0);

	double m_ShortestPeriod = 1.0;
	double m_MaxStep = 1.0 / 64.0;

	float m_StarBrightness = 1.0f;
};
