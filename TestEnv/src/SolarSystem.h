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

#include "Demo.h"
#include "Vegetation.h"
#include "VoxelPlanet.h"

class SolarSystem : public DemoLayer
{
public:
	SolarSystem()
		: DemoLayer("SolarSystem"), m_Camera(50.0f, 16.0f / 9.0f, 0.5f, 900000.0f)
	{
		RegisterParam("OrbitalYearsPerSecond", &m_OrbitalYearsPerSecond);
		RegisterParam("SecondsPerDay", &m_SecondsPerDay);
	}

	// --- The system ---------------------------------------------------------

	struct BodyDescription
	{
		const char* Name;
		int Parent;             // -1 for the star, else an index into this table
		double SemiMajorAu;     // from its parent
		double RadiusKm;
		double MassSuns;        // in solar masses, so GM = GM_sun * this
		double RotationHours;   // sidereal day; negative for a retrograde spin
		glm::vec3 Colour;

		// Atmosphere: its depth as a fraction of the body's radius, and the
		// colour it scatters. Zero depth means airless -- Mercury, the Moon,
		// the small moons -- and those get no shell at all rather than a
		// transparent one.
		float AtmosphereFraction;
		glm::vec3 Scatter;
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
			{ "Sun",      -1, 0.0,       696000.0, 1.0, 609.0,         { 1.00f, 0.86f, 0.42f }, 0.0f, { 0.0f, 0.0f, 0.0f } },

			{ "Mercury",   0, 0.387,       2440.0, 1.660e-7, 1407.6,    { 0.62f, 0.58f, 0.54f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Venus",     0, 0.723,       6052.0, 2.448e-6, -5832.5,    { 0.92f, 0.80f, 0.55f }, 0.055f, { 0.85f, 0.62f, 0.25f } },
			{ "Earth",     0, 1.000,       6371.0, 3.003e-6, 23.934,    { 0.28f, 0.48f, 0.85f }, 0.045f, { 0.22f, 0.45f, 1.00f } },
			{ "Mars",      0, 1.524,       3390.0, 3.227e-7, 24.623,    { 0.80f, 0.38f, 0.24f }, 0.020f, { 0.80f, 0.45f, 0.30f } },
			{ "Jupiter",   0, 5.203,      69911.0, 9.545e-4, 9.925,    { 0.80f, 0.68f, 0.52f }, 0.035f, { 0.75f, 0.62f, 0.45f } },
			{ "Saturn",    0, 9.537,      58232.0, 2.858e-4, 10.656,    { 0.88f, 0.80f, 0.60f }, 0.038f, { 0.80f, 0.72f, 0.50f } },
			{ "Uranus",    0, 19.191,     25362.0, 4.366e-5, -17.24,    { 0.60f, 0.85f, 0.88f }, 0.040f, { 0.40f, 0.80f, 0.85f } },
			{ "Neptune",   0, 30.070,     24622.0, 5.151e-5, 16.11,    { 0.30f, 0.44f, 0.86f }, 0.040f, { 0.25f, 0.42f, 0.95f } },

			{ "Moon",      3, 0.002570,    1737.0, 3.694e-8, 655.7,    { 0.72f, 0.71f, 0.68f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Phobos",    4, 0.0000627,     11.3, 5.0e-15, 7.65,     { 0.55f, 0.50f, 0.46f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Io",        5, 0.002819,    1822.0, 4.490e-8, 42.46,    { 0.88f, 0.82f, 0.45f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Europa",    5, 0.004486,    1561.0, 2.413e-8, 85.2,    { 0.80f, 0.78f, 0.72f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Ganymede",  5, 0.007155,    2634.0, 7.450e-8, 171.7,    { 0.66f, 0.62f, 0.58f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Callisto",  5, 0.012585,    2410.0, 5.410e-8, 400.5,    { 0.48f, 0.45f, 0.44f }, 0.0f, { 0.0f, 0.0f, 0.0f } },
			{ "Titan",     6, 0.008168,    2575.0, 6.766e-8, 382.7,    { 0.85f, 0.65f, 0.30f }, 0.060f, { 0.90f, 0.60f, 0.25f } },
		};

		return table;
	}

	void OnDemoAttach() override
	{
		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		for (size_t i = 1; i + 1 < arguments.size(); i++)
			if (arguments[i] == "--years-per-second")
				m_OrbitalYearsPerSecond = (float)std::atof(arguments[i + 1].c_str());

		// **Finer than a body needs, because three spheres have to agree.**
		// The planet's stand-in sphere, the sea and the atmosphere shell are
		// all drawn from this one mesh at radii within a few metres of each
		// other, so wherever their facets cross, one shows through the other.
		// At 48x24 that put polygonal patches of missing sea across the far
		// ocean. 128x64 is 16k triangles a sphere and the crossings fall below
		// a pixel.
		m_Sphere.reset(Egss::Mesh::CreateSphere(1.0f, 128, 64));

		BuildShader();
		BuildTrees();
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
		PlaceFromCommandLine();
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
		EGSS_TRACE("Solar system: p = {0:.3f}, Earth {1:.1f} m, 1 AU = {2:.0f} m",
			m_Compression, DrawnRadius(3), DrawnLength(s_AuKm));

		for (size_t i = 0; i < m_Bodies.size(); i++)
		{
			if (m_Bodies[i].Parent < 0)
			{
				EGSS_TRACE("  {0:<9} r = {1:8.1f} m", m_Bodies[i].Name, DrawnRadius(i));
				continue;
			}

			size_t parent = (size_t)m_Bodies[i].Parent;
			double orbit = DrawnLength(m_Bodies[i].SemiMajorAu * s_AuKm);
			double clearance = orbit / (DrawnRadius(parent) + DrawnRadius(i));

			EGSS_TRACE("  {0:<9} r = {1:8.1f} m, a = {2:9.1f} m, {3:.2f}x clear of {4}",
				m_Bodies[i].Name, DrawnRadius(i), orbit, clearance, m_Bodies[parent].Name);
		}
	}

	// `--land Earth` (or `--goto Earth`) puts the ship somewhere specific at
	// startup, which is how a capture reaches one -- an unattended run has
	// nobody to fly it.
	void PlaceFromCommandLine()
	{
		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		for (size_t i = 1; i + 1 < arguments.size(); i++)
		{
			bool land = arguments[i] == "--land";

			if (!land && arguments[i] != "--goto")
				continue;

			const std::string& wanted = arguments[i + 1];

			for (size_t body = 1; body < m_Bodies.size(); body++)
			{
				if (m_Bodies[body].Name != wanted)
					continue;

				// Approached from the star's side, so the landing site is lit
				// rather than in the middle of its night.
				GoTo(body, land ? 1.06 : 6.0, land ? 0.0 : 55.0);

				if (land)
					Land();

				return;
			}

			EGSS_WARN("{0} {1} matches no body", arguments[i], wanted);
		}
	}

	// Put the ship `radii` of the body's own drawn radius out from its centre,
	// on the sunward side, looking at it. A teleport, and said to be one: it
	// is how the panel's buttons work and how a capture gets framed. Flying
	// there is the point of the demo, but not of a screenshot.
	void GoTo(size_t index, double radii, double tiltDegrees = 55.0)
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

	// One expression, and every length in the demo goes through it.
	double DrawnLength(double km) const
	{
		if (km <= 0.0)
			return 0.0;

		return (double)s_EarthDrawn * std::pow(km / s_EarthRadiusKm, (double)m_Compression);
	}

	double DrawnRadius(size_t index) const
	{
		// A floor, because Phobos is 11.3 km across and lands at 15 m: small
		// enough to fly past without seeing, but not so small that the voxel
		// lattice under it has nothing to say.
		return std::max(12.0, DrawnLength(m_Bodies[index].RadiusKm));
	}

	// An offset in AU, compressed along its own direction. Applied per level of
	// the hierarchy, so a moon's offset is compressed as a moon-sized distance
	// and its planet's as a planet-sized one, which is what keeps Phobos above
	// Mars while Neptune is still 302 km out.
	glm::dvec3 DrawnOffset(const glm::dvec3& au) const
	{
		double length = glm::length(au);

		if (length <= 0.0)
			return glm::dvec3(0.0);

		return au * (DrawnLength(length * s_AuKm) / length);
	}

	glm::dvec3 BodyScene(size_t index) const
	{
		glm::dvec3 at(0.0);

		// Walked up the hierarchy rather than stored, so a moon is exactly its
		// planet's position plus its own and cannot drift away from it.
		for (int i = (int)index; i >= 0; i = m_Bodies[(size_t)i].Parent)
			at += DrawnOffset(m_Bodies[(size_t)i].Position);

		return at;
	}

	glm::dvec3 ShipScene() const { return BodyScene(m_Frame) + m_Local; }

	// Which body the ship's position is measured from.
	size_t FrameBody() const { return m_Frame; }
	bool Walking() const { return m_Walking; }

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
	// Axis is +Y: no axial tilt, so no seasons. Venus and Uranus have negative
	// rotation periods in the table and turn the other way for free.
	double SpinAngle(size_t index) const
	{
		double hours = m_Bodies[index].RotationHours;

		if (std::abs(hours) < 1e-6)
			return 0.0;

		// Days elapsed, times two pi. The year is 8,766 hours.
		return m_Time * (365.25 * 24.0) / hours * 2.0 * 3.14159265358979323846;
	}

	// Prograde is +x toward +z, the same sense the orbits start in.
	static glm::dvec3 RotateY(const glm::dvec3& v, double angle)
	{
		double c = std::cos(angle), s = std::sin(angle);

		return glm::dvec3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
	}

	glm::dvec3 ToScene(size_t index, const glm::dvec3& fixed) const
	{
		return RotateY(fixed, SpinAngle(index));
	}

	glm::dvec3 ToFixed(size_t index, const glm::dvec3& scene) const
	{
		return RotateY(scene, -SpinAngle(index));
	}

	// **The same rotation as a matrix, built by hand.**
	//
	// `glm::rotate(m, angle, +Y)` is not `RotateY(v, angle)`: glm's matrix
	// takes +x toward *-z*, and this one takes it toward +z. Both are
	// perfectly ordinary right-handed rotations about +Y; they just differ in
	// which way the angle counts. Mixing them meant the terrain was drawn
	// through the inverse of the spin the camera was placed by, so the ground
	// under your feet slid round the planet at *twice* the rate of the day --
	// invisible at t = 0, where both are the identity, and 62 degrees out
	// after seven seconds.
	//
	// Nothing catches that by looking: the horizon sphere is smooth and
	// symmetric, so what you see is a planet with no visible terrain on it and
	// no reason given. It came out of asking why a tree was 421 m away on a
	// world 360 m across.
	glm::mat4 SpinMatrix(size_t index) const
	{
		double angle = SpinAngle(index);

		float c = (float)std::cos(angle), s = (float)std::sin(angle);

		return glm::mat4(
			   c, 0.0f,    s, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			  -s, 0.0f,    c, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f);
	}

	// --- Planets ------------------------------------------------------------

	// Voxels get bigger on bigger planets, which keeps the chunk count roughly
	// constant: the lattice spans the whole body, so at a fixed voxel size
	// Jupiter would cost eleven times Earth's memory for terrain no closer to
	// the eye. 1.5 m on Earth is the reference.
	float VoxelSizeFor(size_t index) const
	{
		return glm::clamp((float)DrawnRadius(index) / 240.0f, 0.5f, 6.0f);
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
		settings.Amplitude = radius * 0.085f;
		settings.FeatureSize = radius * 0.195f;
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
			settings.Octaves = 5;
		}

		EGSS_TRACE("{0}: voxel planet, radius {1:.0f} m, relief {2:.0f} m, voxel {3:.2f} m",
			m_Bodies[index].Name, settings.Radius, settings.Amplitude, settings.VoxelSize);

		VoxelPlanet& planet = m_Planets[index];
		planet.Create(settings);

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

	// Height above the nearest body's *drawn surface*, over every body rather
	// than just the frame's -- it is what sets the flight speed, and passing
	// close to a moon should slow you down whoever's frame you are in.
	double AltitudeAboveAnything() const
	{
		glm::dvec3 ship = ShipScene();
		double best = 1e30;

		for (size_t i = 0; i < m_Bodies.size(); i++)
			best = std::min(best, glm::length(BodyScene(i) - ship) - DrawnRadius(i));

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

		m_Walking = true;
		m_Ground = (int)m_Frame;

		// Everything below the takeoff line is in the planet's own turning
		// frame, so the ship's inertial position and heading come across once
		// here and go back once in `TakeOff`.
		glm::vec3 fixed = glm::vec3(ToFixed(m_Frame, m_Local));
		glm::vec3 forward = glm::vec3(ToFixed(m_Frame, glm::dvec3(m_Forward)));

		BuildSurfaceWorld(m_Frame, planet, fixed);

		// The basis is already right -- flight was levelled to the local
		// vertical on the way in. All that is needed is to say it in the
		// tangent frame the walk uses, so the first step does not swing.
		glm::vec3 up = glm::normalize(fixed);
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

	void TakeOff()
	{
		m_Walking = false;

		// Where the feet were is where the ship is, which is the whole point.
		glm::vec3 feet = m_World.GetBody(m_Player).Position;
		glm::dvec3 eye = glm::dvec3(feet + glm::normalize(feet) * m_EyeHeight);

		m_Local = ToScene((size_t)m_Ground, eye);

		m_World.Clear();
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
	void ApplyGravity()
	{
		double gm = LocalGm((size_t)m_Ground);

		for (Egss::RigidBody3D& body : m_World.GetBodies())
		{
			if (body.Type != Egss::BodyType::Dynamic || body.InverseMass <= 0.0f)
				continue;

			glm::vec3 toCentre = -body.Position;
			float distance = glm::length(toCentre);

			if (distance < 1e-3f)
				continue;

			float acceleration = (float)(gm / ((double)distance * (double)distance));
			float mass = 1.0f / body.InverseMass;

			m_World.ApplyForce(BodyHandleOf(body), (toCentre / distance) * acceleration * mass);
		}
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

	void BuildSurfaceWorld(size_t index, VoxelPlanet& planet, const glm::vec3& at)
	{
		m_World.Clear();

		// No world gravity at all: every pull here is radial and applied per
		// body. Leaving the default -9.81 Y in place would add a second,
		// invisible gravity pointing at the planet's north pole.
		m_World.Gravity = glm::vec3(0.0f);

		Egss::RigidBody3D ground =
			Egss::RigidBody3D::MakeSdf(glm::vec3(0.0f), planet.Field());
		ground.Friction = 0.8f;
		ground.Restitution = 0.0f;

		m_World.AddBody(ground);

		glm::vec3 up = glm::normalize(at);

		// The player starts exactly where the ship was, feet first. Above the
		// ground it falls; on it, it stands.
		Egss::RigidBody3D player = Egss::RigidBody3D::MakeCapsule(
			at - up * m_EyeHeight, 0.4f, 0.9f, 78.0f);

		player.Friction = 0.6f;
		player.Restitution = 0.0f;
		player.Orientation = UprightAt(up);

		m_Player = m_World.AddBody(player);

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
			glm::vec3 offset = east * (next() * 40.0f - 20.0f)
				+ north * (next() * 40.0f - 20.0f);

			glm::vec3 at = glm::normalize(up * surface + offset);
			float ground2 = planet.SurfaceRadius(at);

			Egss::RigidBody3D rock = Egss::RigidBody3D::MakeSphere(
				at * (ground2 + 18.0f + next() * 25.0f), 0.6f + next() * 0.7f, 40.0f);

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
		// at the bottom, through the spin.
		glm::vec3 up = glm::normalize(m_World.GetBody(m_Player).Position);
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

		ApplyGravity();
		m_World.Step(dt);

		glm::vec3 feet = m_World.GetBody(m_Player).Position;

		// Grounded is measured against the terrain the physics is using, not a
		// contact flag, so it means the same thing as the ground query the
		// camera and the spawner use.
		glm::vec3 direction = glm::normalize(feet);
		float ground = planet.SurfaceRadius(direction);

		m_Grounded = (glm::length(feet) - ground) < m_PlayerHalfHeight + 0.35f;

		glm::vec3 forward = glm::normalize(
			heading * std::cos(glm::radians(m_SurfacePitch))
			+ direction * std::sin(glm::radians(m_SurfacePitch)));

		// Out into scene coordinates. Standing still on a turning planet is
		// motion out here, which is exactly why the Sun crosses the sky
		// without the light direction ever being touched.
		size_t index = (size_t)m_Ground;

		m_Local = ToScene(index, glm::dvec3(feet + direction * m_EyeHeight));
		m_Up = glm::vec3(ToScene(index, glm::dvec3(direction)));
		m_Forward = glm::vec3(ToScene(index, glm::dvec3(forward)));
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
		glm::vec3 focus = glm::vec3(ToFixed(index, m_Local));

		planet.StreamAround(focus, m_LoadRadius * scale, m_ChunksPerStep);
		planet.EvictBeyond(focus, m_LoadRadius * scale * 1.6f);
	}

	// --- Drawing ------------------------------------------------------------

	void OnDemoUpdate(Egss::Timestep) override
	{
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
		float nearClip = (float)glm::clamp(AltitudeAboveAnything() * 0.4, 0.15, 400.0);

		m_Camera.SetProjection(m_Walking ? 65.0f : 55.0f, Aspect(), nearClip, 900000.0f);
		m_Camera.SetPosition(glm::vec3(0.0f));
		m_Camera.SetOrientation(m_Forward, m_Up);

		Egss::Renderer::BeginScene(m_Camera);

		size_t terrain = m_Walking ? (size_t)m_Ground : m_Frame;

		for (size_t i = 0; i < m_Bodies.size(); i++)
			DrawBody(i, glm::vec3(BodyScene(i) - origin), i == terrain);

		DrawRocks(origin);

		// Then the water, then the air. Both are blended and neither writes
		// depth, so the order they go down in is the order they are read in,
		// and sea has to be under sky.
		for (size_t i = 1; i < m_Bodies.size(); i++)
			DrawOcean(i, glm::vec3(BodyScene(i) - origin));

		// After the solid bodies, so the depth buffer already holds them and a
		// planet occludes the air behind it.
		for (size_t i = 1; i < m_Bodies.size(); i++)
			DrawAtmosphere(i, glm::vec3(BodyScene(i) - origin), (float)DrawnRadius(i));

		Egss::Renderer::EndScene();
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
	void DrawBody(size_t index, const glm::vec3& centre, bool isNear)
	{
		float radius = (float)DrawnRadius(index);

		if (index == 0)
		{
			// The star lights everything and is lit by nothing, so it is drawn
			// as its own colour at full brightness -- an emissive surface, and
			// the one body where a lighting calculation would be wrong.
			auto material = Egss::Material::CreateInstance(m_Material);
			material->Set("u_Color", glm::vec4(m_Bodies[0].Colour, 1.0f));
			material->Set("u_Emissive", 1.0f);
			material->Set("u_LightPosition", centre);
			material->Set("u_LightColor", m_SunLight * m_StarBrightness);

			Egss::Renderer::Submit(material, m_Sphere,
				glm::scale(glm::translate(glm::mat4(1.0f), centre), glm::vec3(radius)));

			return;
		}

		auto it = m_Planets.find(index);
		bool generated = it != m_Planets.end();
		bool meshed = isNear && generated && it->second.MeshedChunks() > 0;

		float relief = generated ? it->second.Get().Amplitude : radius * 0.085f;

		// **Inset as soon as the planet exists, not only once it is meshed.**
		// The sphere has to sit below the lowest valley so real terrain hides
		// it -- and below the sea, or the ocean shell would be inside the ball
		// it is meant to cover and no planet would have water until you were
		// close enough to mesh one.
		float drawn = generated ? radius - relief * 0.5f - 0.5f : radius;

		auto material = Egss::Material::CreateInstance(m_TerrainMaterial);
		material->Set("u_LightDirection", SunDirection(index));
		material->Set("u_LightColor", m_SunLight * m_StarBrightness);
		material->Set("u_LowColour", glm::vec4(m_Bodies[index].Colour * 0.55f, 1.0f));
		material->Set("u_HighColour",
			glm::vec4(glm::mix(m_Bodies[index].Colour, glm::vec3(1.0f), 0.35f), 1.0f));
		material->Set("u_Radius", radius);
		material->Set("u_Relief", relief);
		material->Set("u_Origin", centre);
		material->Set("u_Spin", MapSpin(index));

		// A planet with no sea gets a waterline below its deepest valley, so
		// every point on it is "land" and the altitude ramp is all that runs.
		SetBiome(material, index, generated
			? it->second.Get() : VoxelPlanet::Settings());

		// **The sphere reads the map; the chunks do not.** Same material
		// otherwise, and the flag is the only thing that differs -- which is
		// why it is set twice rather than once.
		material->Set("u_HasMap", generated ? 1.0f : 0.0f);

		if (generated)
			material->SetTexture("u_Map", it->second.Map(), 0);

		Egss::Renderer::Submit(material, m_Sphere,
			glm::scale(glm::translate(glm::mat4(1.0f), centre), glm::vec3(drawn)));

		if (!meshed)
			return;

		material->Set("u_HasMap", 0.0f);

		// The chunks are planet-fixed; the spin is what puts them in the sky
		// where the Sun says they should be.
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), centre) * SpinMatrix(index);

		for (const auto& [key, chunk] : it->second.Chunks())
			Egss::Renderer::Submit(material, chunk.MeshPtr, transform);

		DrawPlants(index, it->second, transform);
	}

	// The palette and the waterline, which the terrain and the sea both need
	// and neither owns.
	void SetBiome(const std::shared_ptr<Egss::Material>& material, size_t index,
		const VoxelPlanet::Settings& settings) const
	{
		float radius = (float)DrawnRadius(index);

		material->Set("u_Vegetated", settings.Vegetated ? 1.0f : 0.0f);
		material->Set("u_SeaRadius", settings.HasOcean
			? settings.OceanRadius : radius - ReliefOf(settings, radius));

		material->Set("u_Shallow", settings.Shallow);
		material->Set("u_Deep", settings.Deep);
		material->Set("u_Sand", settings.Sand);
		material->Set("u_Tropical", settings.Tropical);
		material->Set("u_Temperate", settings.Temperate);
		material->Set("u_Tundra", settings.Tundra);
		material->Set("u_Rock", settings.Rock);
		material->Set("u_Snow", settings.Snow);
	}

	static float ReliefOf(const VoxelPlanet::Settings& settings, float radius)
	{
		return settings.Amplitude > 0.0f ? settings.Amplitude : radius * 0.085f;
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
	void BuildTrees()
	{
		Veg::TreeParams params;
		params.Depth = 3;
		params.Sides = 5;
		params.Length = 2.1f;
		params.Radius = 0.16f;
		params.LeafSegments = 5;
		params.LeafRings = 3;

		size_t triangles = 0;

		for (int i = 0; i < s_TreeShapes; i++)
		{
			Egss::MeshData bark, leaves;
			Veg::MakeTreeMesh(521u + (unsigned int)i * 97u, params, bark, leaves);

			m_TreeBark[i].reset(new Egss::Mesh(bark, "PlanetTree"));
			m_TreeLeaves[i].reset(new Egss::Mesh(leaves, "PlanetTreeLeaves"));

			triangles += (bark.Indices.size() + leaves.Indices.size()) / 3;
		}

		EGSS_TRACE("Planet trees: {0} shapes, {1} triangles each on average",
			s_TreeShapes, triangles / s_TreeShapes);
	}

	// One planet's trees, in the frame the chunks are already drawn in.
	void DrawPlants(size_t index, const VoxelPlanet& planet, const glm::mat4& frame)
	{
		if (planet.Get().PlantsPerChunk <= 0)
			return;

		auto bark = Egss::Material::CreateInstance(m_Material);
		bark->Set("u_Color", glm::vec4(0.30f, 0.22f, 0.15f, 1.0f));
		bark->Set("u_Emissive", 0.0f);
		bark->Set("u_LightColor", m_SunLight * m_StarBrightness);

		auto leaves = Egss::Material::CreateInstance(m_Material);
		leaves->Set("u_Color", glm::vec4(0.16f, 0.34f, 0.13f, 1.0f));
		leaves->Set("u_Emissive", 0.0f);
		leaves->Set("u_LightColor", m_SunLight * m_StarBrightness);

		// **A directional light faked as a very distant point.** The shader
		// this shares with the rocks and the star takes a position and
		// normalises the difference, so a light four kilometres away along the
		// sun direction is parallel to a thousandth of a degree across a
		// planet this size.
		glm::vec3 lamp = SunDirection(index) * 40000.0f;
		bark->Set("u_LightPosition", lamp);
		leaves->Set("u_LightPosition", lamp);

		int drawn = 0;

		for (const auto& [key, chunk] : planet.Chunks())
		{
			for (const VoxelPlanet::Plant& plant : chunk.Plants)
			{
				glm::mat4 model = frame
					* glm::translate(glm::mat4(1.0f), plant.Position)
					* glm::mat4_cast(UprightAt(plant.Up))
					* glm::rotate(glm::mat4(1.0f), plant.Yaw, glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::scale(glm::mat4(1.0f), glm::vec3(plant.Scale));

				Egss::Renderer::Submit(bark, m_TreeBark[plant.Shape], model);
				Egss::Renderer::Submit(leaves, m_TreeLeaves[plant.Shape], model);

				drawn++;
			}
		}

		m_PlantsDrawn = drawn;
	}

	// Turns of the planet, wrapped, which is what the equirectangular lookup
	// wants: `u` is the azimuth over two pi, so the spin is a shift along it.
	float MapSpin(size_t index) const
	{
		double turns = SpinAngle(index) / (2.0 * 3.14159265358979323846);

		return (float)(turns - std::floor(turns));
	}

	// The sea. Drawn after every opaque thing in the frame, because it is
	// blended and writes no depth -- ground in front of it has to be in the
	// depth buffer already or the water goes over the top of it.
	void DrawOcean(size_t index, const glm::vec3& centre)
	{
		auto it = m_Planets.find(index);

		if (it == m_Planets.end() || !it->second.Get().HasOcean)
			return;

		const VoxelPlanet::Settings& settings = it->second.Get();

		auto material = Egss::Material::CreateInstance(m_WaterMaterial);
		material->SetTexture("u_Map", it->second.Map(), 0);
		material->Set("u_Spin", MapSpin(index));
		material->Set("u_Radius", settings.Radius);
		material->Set("u_Relief", settings.Amplitude);
		material->Set("u_SeaRadius", settings.OceanRadius);
		material->Set("u_LightDirection", SunDirection(index));
		material->Set("u_LightColor", m_SunLight * m_StarBrightness);
		material->Set("u_Origin", centre);
		material->Set("u_Eye", -centre);
		material->Set("u_Shallow", settings.Shallow);
		material->Set("u_Deep", settings.Deep);
		material->Set("u_Time", (float)m_Time * 8766.0f);
		material->Set("u_WaveScale", 1.6f);

		// No depth write, so two bits of sea do not occlude each other, and no
		// culling because the camera can be under it.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		Egss::Renderer::Submit(material, m_Sphere,
			glm::scale(glm::translate(glm::mat4(1.0f), centre),
				glm::vec3(settings.OceanRadius)));

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);
		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	// The loose bodies. Gravity acting on nothing you can see is gravity you
	// cannot check by looking. They live in the planet's own coordinates, so
	// they get the planet's offset like the chunks do.
	void DrawRocks(const glm::dvec3& origin)
	{
		if (m_Ground < 0)
			return;

		size_t index = (size_t)m_Ground;
		glm::vec3 offset = glm::vec3(BodyScene(index) - origin);
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

			glm::vec3 at = offset + glm::vec3(ToScene(index, glm::dvec3(body.Position)));

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

	// The one length the scale map leaves alone: Earth's radius, drawn at
	// 360 m. Big enough to walk on with 1.5 m voxels and a horizon that is
	// visibly close; small enough that the whole system fits in 302 km.
	static constexpr float s_EarthDrawn = 360.0f;

	struct Body
	{
		std::string Name;
		int Parent = -1;
		double Gm = 0.0;          // GM of *this* body, for whatever orbits it
		double RotationHours = 24.0;
		float AtmosphereFraction = 0.0f;
		glm::vec3 Scatter = glm::vec3(0.0f);
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
			body.AtmosphereFraction = description.AtmosphereFraction;
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

				// A little light on the night side, so a planet reads as a
				// sphere rather than as a lit crescent floating in nothing.
				vec3 lit = u_Color.rgb * (0.06 + 0.94 * diffuse) * u_LightColor;

				color = vec4(mix(lit, u_Color.rgb, u_Emissive), u_Color.a);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("SolarSystem", vertexSrc, fragmentSrc));
		m_Material = Egss::Material::Create(m_Shader);

		BuildTerrainShader();
		BuildWaterShader();
		BuildAtmosphereShader();
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

			out vec3 v_Position;
			out vec3 v_Normal;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				// Back into the planet's own coordinates. The chunks arrive in
				// them already and the sphere does not (it is a unit sphere
				// scaled), so the subtraction has to happen after the
				// transform rather than before it.
				v_Position = world.xyz - u_Origin;
				v_Normal = mat3(u_Transform) * a_Normal;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_Position;
			in vec3 v_Normal;

			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec4 u_LowColour;
			uniform vec4 u_HighColour;
			uniform float u_Radius;
			uniform float u_Relief;
			uniform float u_SeaRadius;

			uniform sampler2D u_Map;
			uniform float u_HasMap;
			uniform float u_Vegetated;

			// Turns of the planet, so the map can be looked up in the frame it
			// was baked in. Everything else here is in scene axes, and the
			// only thing the spin changes about an equirectangular map is the
			// azimuth -- so it is one subtraction rather than a matrix.
			uniform float u_Spin;

			uniform vec3 u_Shallow;
			uniform vec3 u_Deep;
			uniform vec3 u_Sand;
			uniform vec3 u_Tropical;
			uniform vec3 u_Temperate;
			uniform vec3 u_Tundra;
			uniform vec3 u_Rock;
			uniform vec3 u_Snow;

			// **One rule, evaluated per pixel.** `height` is metres above sea
			// level and `latitude` is zero at the equator and one at a pole --
			// nothing else decides what grows where, because with no axial
			// tilt there is nothing else to decide it.
			//
			// The order of the mixes is the model. Sand loses to grass a metre
			// inland, grass loses to rock on a mountain, rock loses to snow
			// higher still, and latitude comes last because an ice cap is an
			// ice cap whatever the altitude under it.
			vec3 Biome(float height, float latitude)
			{
				float sea = u_SeaRadius - u_Radius;

				if (u_Vegetated < 0.5)
				{
					float t = clamp((height + sea) / max(u_Relief, 0.001) + 0.5, 0.0, 1.0);

					return mix(u_LowColour.rgb, u_HighColour.rgb, t);
				}

				if (height <= 0.0)
					return mix(u_Shallow, u_Deep,
						clamp(-height / (u_Relief * 0.35), 0.0, 1.0));

				float top = max(u_Relief * 0.5 - sea, 1.0);
				float f = clamp(height / top, 0.0, 1.0);

				vec3 green = mix(u_Tropical, u_Temperate, smoothstep(0.10, 0.55, latitude));

				vec3 colour = mix(u_Sand, green, smoothstep(0.0, 0.05, f));

				colour = mix(colour, u_Rock, smoothstep(0.45, 0.75, f));
				colour = mix(colour, u_Snow, smoothstep(0.74, 0.93, f));
				colour = mix(colour, u_Tundra, smoothstep(0.58, 0.76, latitude));
				colour = mix(colour, u_Snow, smoothstep(0.82, 0.93, latitude));

				return colour;
			}

			void main()
			{
				vec3 normal = normalize(v_Normal);

				// Up is away from the centre, everywhere. `v_Position` is in
				// the planet's own coordinates, so the centre is the origin.
				vec3 up = normalize(v_Position);

				// **Meshed ground knows its own height exactly; the sphere has
				// to be told.** Reading the map for both would blur the coast
				// under your feet to the map's two metres a texel for no
				// reason -- the geometry in front of you is the answer.
				float height;

				if (u_HasMap > 0.5)
				{
					const float pi = 3.14159265;

					vec2 uv = vec2(atan(up.z, up.x) / (2.0 * pi) + 0.5 - u_Spin,
						acos(clamp(up.y, -1.0, 1.0)) / pi);

					float relief = (texture(u_Map, uv).r * 2.0 - 1.0) * u_Relief;

					height = u_Radius + relief - u_SeaRadius;
				}
				else
				{
					height = length(v_Position) - u_SeaRadius;
				}

				vec3 base = Biome(height, abs(up.y));

				// Steep ground shows rock rather than the surface colour, which
				// is what makes a cliff read as a cliff.
				float flatness = clamp(dot(normal, up), 0.0, 1.0);
				base = mix(base * 0.55, base, smoothstep(0.35, 0.8, flatness));

				float diffuse = max(dot(normal, u_LightDirection), 0.0);

				vec3 lit = base * (0.05 + 0.95 * diffuse) * u_LightColor;

				color = vec4(lit, 1.0);
			}
		)";

		m_TerrainShader.reset(
			Egss::Shader::Create("PlanetSurface", vertexSrc, fragmentSrc));

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

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform vec3 u_Origin;

			out vec3 v_Position;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				v_Position = world.xyz - u_Origin;
				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_Position;

			uniform sampler2D u_Map;
			uniform float u_Spin;
			uniform float u_Radius;
			uniform float u_Relief;
			uniform float u_SeaRadius;
			uniform vec3 u_LightDirection;
			uniform vec3 u_LightColor;
			uniform vec3 u_Eye;            // camera, in the planet's frame
			uniform vec3 u_Shallow;
			uniform vec3 u_Deep;
			uniform float u_Time;
			uniform float u_WaveScale;

			void main()
			{
				vec3 up = normalize(v_Position);

				const float pi = 3.14159265;
				vec2 uv = vec2(atan(up.z, up.x) / (2.0 * pi) + 0.5 - u_Spin,
					acos(clamp(up.y, -1.0, 1.0)) / pi);

				// **Where the map says there is ground above the waterline,
				// there is no sea.** Without this the sphere would drown every
				// continent past the streaming radius, where there is no
				// terrain in the depth buffer to hide behind.
				//
				// Biased half a metre *inland* on purpose: close up it is the
				// real geometry that occludes the water, and the map is a
				// two-metre-a-texel approximation of it. Erring toward too
				// much sea puts the disagreement under the ground rather than
				// leaving a fringe of missing sea along every shore.
				float relief = (texture(u_Map, uv).r * 2.0 - 1.0) * u_Relief;

				if (u_Radius + relief > u_SeaRadius + 0.5)
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

				// Straight down is deep water seen into; grazing is sky seen
				// off it. The depth tint uses the same two colours the map's
				// sea is painted with, so the surface and what is under it are
				// not two different oceans.
				vec3 body = mix(u_Deep, u_Shallow, facing);

				float diffuse = max(dot(normal, u_LightDirection), 0.0);

				// Blinn-Phong, tight enough to read as a sun glint rather than
				// a shine.
				// `half` is a reserved word in GLSL, which the compiler will
				// tell you in a way that does not mention that.
				vec3 midway = normalize(u_LightDirection + view);
				float glint = pow(max(dot(normal, midway), 0.0), 220.0);

				vec3 lit = body * (0.05 + 0.95 * diffuse) * u_LightColor
					+ u_LightColor * glint * fresnel * 12.0;

				color = vec4(lit, clamp(0.55 + 0.45 * fresnel, 0.0, 1.0));
			}
		)";

		m_WaterShader.reset(Egss::Shader::Create("PlanetWater", vertexSrc, fragmentSrc));
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

				// Stop at the ground: air in front of a planet is lit, air
				// behind it is not there to be seen.
				float groundNear, groundFar;
				if (hitSphere(origin, direction, u_PlanetRadius, groundNear, groundFar)
					&& groundNear > 0.0)
					far = min(far, groundNear);

				if (far <= near)
					discard;

				const int steps = 8;
				const int sunSteps = 4;

				float segment = (far - near) / float(steps);
				vec3 accumulated = vec3(0.0);
				float viewDepth = 0.0;

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

				color = vec4(result, 1.0);
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
		material->Set("u_Density", m_AirDensity / std::max(1.0f, radius));

		// **Additive, no depth write, no culling.** Additive because air adds
		// light rather than hiding what is behind it; no depth write so one
		// shell does not occlude the next; and no culling because the camera
		// can be inside the shell, where only its back faces are visible.
		// Depth *testing* stays on, so terrain in front still occludes the sky.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Additive);
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

		if (m_Walking)
		{
			VoxelPlanet& planet = m_Planets[(size_t)m_Ground];

			ImGui::Text("%zu chunks meshed, %zu triangles",
				planet.MeshedChunks(), planet.TriangleCount());

			double gm = LocalGm((size_t)m_Ground);
			double here = glm::length(m_Local);
			double radius = DrawnRadius((size_t)m_Ground);

			ImGui::Text("surface gravity %.2f m/s^2 (real %.2f), here %.2f",
				gm / (radius * radius), RealSurfaceGravity((size_t)m_Ground),
				gm / (here * here));

			ImGui::Text("escape velocity %.1f m/s, orbit %.1f m/s",
				std::sqrt(2.0 * gm / here), std::sqrt(gm / here));

			ImGui::Text("%s", m_Grounded ? "on the ground" : "falling");
		ImGui::Text("%d trees in view", m_PlantsDrawn);

			if (ImGui::Button("Take off  (L)"))
				TakeOff();
		}
		else
		{
			ImGui::Text("%.0f m/s  (WASD, space/ctrl, shift to boost)", m_FlightSpeed);

			if (ImGui::Button("Land  (L)"))
				Land();
		}

		ImGui::Separator();

		ImGui::SliderFloat("Years per second", &m_OrbitalYearsPerSecond, 0.0f, 2.0f, "%.4f");
		ImGui::SliderFloat("Seconds per day", &m_SecondsPerDay, 4.0f, 600.0f, "%.0f s");
		ImGui::Text("now running at %.5f yr/s", m_YearsPerSecond);

		if (ImGui::SliderFloat("Compression p", &m_Compression, 0.3f, 1.0f, "%.3f"))
			ReportScale();

		ImGui::SliderFloat("Star brightness", &m_StarBrightness, 0.2f, 3.0f);
		ImGui::SliderFloat("Air depth", &m_AirScale, 0.2f, 6.0f, "%.1fx");
		ImGui::SliderFloat("Air density", &m_AirDensity, 1.0f, 120.0f, "%.0f");
		ImGui::SliderFloat("Load radius", &m_LoadRadius, 40.0f, 260.0f, "%.0f m");
		ImGui::SliderInt("Chunks per step", &m_ChunksPerStep, 1, 12);
		ImGui::Checkbox("Labels", &m_ShowLabels);

		ImGui::TextDisabled("Earth %.0f m across, 1 AU = %.1f km, system %.0f km wide",
			2.0 * DrawnRadius(3), DrawnLength(s_AuKm) / 1000.0,
			2.0 * DrawnLength(30.07 * s_AuKm) / 1000.0);

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

	std::shared_ptr<Egss::Shader> m_AtmosphereShader;
	std::shared_ptr<Egss::Material> m_AtmosphereMaterial;

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

	std::shared_ptr<Egss::Shader> m_TerrainShader;
	std::shared_ptr<Egss::Material> m_TerrainMaterial;

	std::shared_ptr<Egss::Shader> m_WaterShader;
	std::shared_ptr<Egss::Material> m_WaterMaterial;

	static constexpr int s_TreeShapes = 3;
	std::shared_ptr<Egss::Mesh> m_TreeBark[s_TreeShapes];
	std::shared_ptr<Egss::Mesh> m_TreeLeaves[s_TreeShapes];
	int m_PlantsDrawn = 0;

	// Generated on approach and kept: regenerating a planet is a density
	// evaluation for every voxel of its shell.
	std::unordered_map<size_t, VoxelPlanet> m_Planets;

	// --- Where the camera is ------------------------------------------------

	float m_Compression = 0.5f;

	size_t m_Frame = 3;                       // whose frame m_Local is in
	glm::dvec3 m_Local = glm::dvec3(0.0);     // ship, relative to that body

	glm::vec3 m_Forward = { 0.0f, 0.0f, -1.0f };
	glm::vec3 m_Up = { 0.0f, 1.0f, 0.0f };

	bool m_Reported = false;
	double m_HandoverResidual = 0.0;
	double m_FlightSpeed = 0.0;

	double m_SpeedPerMetre = 0.6;
	float m_MinSpeed = 6.0f;
	float m_MaxSpeed = 40000.0f;
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

	Egss::PhysicsWorld3D m_World;
	Egss::PhysicsWorld3D::BodyHandle m_Player = 0;
	float m_LookSpeed = 0.12f;

	std::pair<float, float> m_LastMouse = { 0.0f, 0.0f };
	bool m_HasMouse = false;
	bool m_MouseLook = false;
	bool m_WasToggling = false;
	float m_LookRate = 90.0f;         // degrees a second, for the arrow keys

	float m_LoadRadius = 110.0f;
	int m_ChunksPerStep = 4;

	// --- The clocks ---------------------------------------------------------

	float m_SecondsPerDay = 60.0f;
	float m_OrbitalYearsPerSecond = 0.02f;
	double m_YearsPerSecond = 0.02;

	std::vector<Body> m_Bodies;

	double m_Time = 0.0;
	double m_ShortestPeriod = 1.0;
	double m_MaxStep = 1.0 / 64.0;

	float m_StarBrightness = 1.0f;
};
