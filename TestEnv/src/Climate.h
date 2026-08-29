#pragma once

// **The weather, derived rather than authored.**
//
// One temperature and one wind for any point on any body, computed from the
// numbers the system already has: how far the body is from the star, how much
// light it reflects, how much air it has, which way is up, and where the sun
// is in its sky right now. Nothing here is a designer's dial. That is the
// point -- a wind you can lean into means very little if someone typed the
// number, and a great deal if it came out of the same orbit that decides when
// the sun rises.
//
// The chain is short enough to follow end to end:
//
//   1. The star's output spread over a sphere gives the flux arriving here.
//   2. What is not reflected is absorbed; what is absorbed is re-radiated;
//      setting those equal gives a temperature.
//   3. Air is transparent to sunlight and not to heat, so it raises that
//      temperature. One optical depth, one closed form.
//   4. Ground higher up is colder, at a rate that is g/c_p and nothing else.
//   5. Air moves from where there is more of it to where there is less, and
//      the planet turns underneath it, which is what makes the bands.
//
// The one place to be careful reading this: steps 1-3 are physics with no
// fitted constants except the greenhouse's, and step 5 is a *description* of
// the general circulation rather than a derivation of it. Solving for the
// Hadley cells is a general-circulation model. Stating where they are, and
// letting everything else follow, is a fair trade at this scale and is
// flagged as such where it happens.

#include <Egss.h>

#include <glm/glm.hpp>

#include <cmath>

namespace Climate {

	// --- Constants, all measured ---------------------------------------------

	// Stefan-Boltzmann, W m^-2 K^-4.
	static constexpr float s_Sigma = 5.670374e-8f;

	// The Sun's luminosity, W. With the AU below this reproduces the solar
	// constant to a part in a thousand, which is the check that they are both
	// right: 3.828e26 / (4 pi (1.495979e11)^2) = 1361.0 W/m^2.
	static constexpr float s_SunWatts = 3.828e26f;
	static constexpr float s_AuMetres = 1.495979e11f;

	// Specific heat of air at constant pressure, J kg^-1 K^-1. Earth's value,
	// used for every body -- see `LapseRate` for what that costs.
	static constexpr float s_AirHeat = 1005.0f;

	// Specific gas constant for dry air, J kg^-1 K^-1, for the scale height.
	static constexpr float s_AirGas = 287.05f;

	// Sea-level pressure on Earth, Pa. Every other body is scaled off its air
	// column relative to Earth's, so this is the unit rather than a constant.
	static constexpr float s_EarthPressure = 101325.0f;

	// Earth's air column as this table measures it: AtmosphereFraction times
	// AtmosphereDensity, 0.0157 * 3.0. Pressures and optical depths are quoted
	// relative to it, so it appears here rather than being spelled out at
	// three call sites.
	static constexpr float s_EarthColumn = 0.0157f * 3.0f;

	// **Grey infrared optical depth per unit of that column.**
	//
	// A grey atmosphere -- transparent to sunlight, one absorption coefficient
	// for everything coming back up -- gives `T_surface = T_eq (1 + 3 tau/4)^(1/4)`,
	// which is the standard two-stream result and the only fitted number in
	// the temperature. Earth's 288 K over its 254 K equilibrium needs
	// tau = 0.871; the textbook figure for the real atmosphere is about 0.84,
	// so this is not far from being measured either.
	//
	// **It gets Venus badly wrong and that is expected.** Venus's column here
	// is 0.9225, so tau comes out 17, and the grey model then says 442 K
	// against a real 737 K. A grey atmosphere is the wrong model for an
	// optically thick CO2 one -- the real thing is nowhere near grey, and no
	// single coefficient fixes it. Earth is the body being stood on; Venus is
	// listed as a known approximation rather than tuned around.
	static constexpr float s_GreyDepth = 0.871f / s_EarthColumn;

	// --- What a place is -----------------------------------------------------

	// Everything the model needs about one point, so the physics below has no
	// opinion about planets, scene graphs or draw scales. Filled by the demo.
	struct Site
	{
		// Distance from the star, in AU. A moon's is its planet's -- Io is not
		// measurably closer to the Sun than Jupiter.
		float StarDistanceAu = 1.0f;

		// Bond albedo, 0..1.
		float Albedo = 0.306f;

		// `AtmosphereFraction * AtmosphereDensity`, the same product the
		// shaders use for optical depth. Zero is airless.
		float AirColumn = s_EarthColumn;

		// Surface gravity, m/s^2, from the body's own GM and radius.
		float Gravity = 9.81f;

		// Cosine of the solar zenith angle: `dot(up, toSun)`. Negative is
		// night, and is used as such rather than clamped away early.
		float CosZenith = 1.0f;

		// Latitude in degrees, signed, from the body's own spin axis.
		float LatitudeDegrees = 0.0f;

		// Metres above the body's sea level.
		float Altitude = 0.0f;

		// 0 arid, 1 saturated -- the hydrology's own field. Water is the
		// largest heat store on a surface, so this damps the day/night swing
		// and slackens the lapse rate.
		float Moisture = 0.5f;

		// Sidereal day in hours, for the Coriolis parameter.
		float RotationHours = 23.934f;

		// Obliquity in degrees. It is what decides how much less sun a pole
		// gets than an equator: at zero the poles get none at all, and the
		// annual mean there rises as sin of it.
		float ObliquityDegrees = 23.4392911f;
	};

	// What the model says about it.
	struct Weather
	{
		float SolarConstant = 0.0f;   // W/m^2 at the top of the air
		float Insolation = 0.0f;      // W/m^2 absorbed here, now
		float Equilibrium = 0.0f;     // K, airless whole-body average
		float Greenhouse = 0.0f;      // K the air adds
		float Temperature = 0.0f;     // K, here and now, at this altitude
		float LapseRate = 0.0f;       // K/km
		float Pressure = 0.0f;        // Pa
		float ScaleHeight = 0.0f;     // m
		float AirDensity = 0.0f;      // kg/m^3

		// Wind in the local tangent frame: x east, y north, in m/s. The demo
		// turns this into a world vector with the same east/north it used to
		// fill in the latitude.
		glm::vec2 Wind = glm::vec2(0.0f);
		float WindSpeed = 0.0f;
	};

	// --- The model -----------------------------------------------------------

	// Flux arriving at the top of the atmosphere, W/m^2. 1361 at 1 AU.
	inline float SolarConstant(float distanceAu)
	{
		float metres = glm::max(distanceAu, 1e-3f) * s_AuMetres;

		return s_SunWatts / (4.0f * glm::pi<float>() * metres * metres);
	}

	// **How much of the day's heat gets spread around before it is radiated.**
	//
	// A body with no air and no ocean radiates each patch of ground at the
	// temperature that patch's own sunlight supports: the Moon's subsolar
	// point is 390 K and its night side is 100 K, six hundred kilometres
	// apart. Air and water move heat sideways faster than the ground can
	// radiate it, so Earth's day/night swing is tens of degrees rather than
	// hundreds.
	//
	// Returned as the share of the absorbed flux that is pooled over the whole
	// sphere before being re-radiated; the rest stays where it landed. The
	// floor of 0.02 is not arbitrary -- it is what puts the airless night side
	// at 100 K, which is the measured lunar value, and stands in for the
	// regolith's own heat capacity.
	inline float Redistribution(float airColumn, float moisture)
	{
		float air = 1.0f - std::exp(-3.0f * airColumn / s_EarthColumn);

		// Water is the other half of it, and on a wet world the larger half.
		float pooled = air + (1.0f - air) * 0.55f * glm::clamp(moisture, 0.0f, 1.0f);

		return glm::clamp(glm::max(pooled, 0.02f), 0.0f, 0.995f);
	}

	// **The dry adiabat, slackened by water.**
	//
	// A parcel of air lifted without exchanging heat cools at exactly `g/c_p`
	// -- 9.76 K/km on Earth -- because the work it does expanding comes out of
	// its own internal energy. That is a derivation, not a measurement. The
	// *observed* lapse rate is 6.5 K/km, and the difference is entirely
	// condensation: rising air reaches saturation, water gives its latent heat
	// back, and the cooling slows. So this runs from the dry adiabat at zero
	// moisture to 6.5 K/km at saturation, and both ends are real numbers
	// rather than a fit.
	//
	// `c_p` is Earth's for every body, which is wrong for Mars's CO2 by about
	// 30% -- worth knowing before quoting a Martian lapse rate, and of no
	// consequence for anything anyone stands on here.
	inline float LapseRate(float gravity, float moisture)
	{
		float dry = gravity / s_AirHeat * 1000.0f;   // K per km

		return dry * (1.0f - 0.33f * glm::clamp(moisture, 0.0f, 1.0f));
	}

	// The three-cell circulation, as a zonal and meridional surface wind.
	//
	// **This is described, not derived.** Sunlight lands hardest at the
	// equator, that air rises, moves poleward aloft, and comes down around
	// 30 degrees -- the Hadley cell -- with a weaker Ferrel cell to 60 and a
	// polar cell beyond. Deriving those from the heating is a general
	// circulation model; asserting where they sit and letting the rest follow
	// is what this does.
	//
	// One expression covers all three. With `b = |latitude| / 30`, the
	// surface flow is `sin(pi b)`, which is zero at 0, 30, 60 and 90 -- the
	// cell boundaries, for free -- and alternates sign between them. So the
	// trades come out easterly, the mid-latitudes westerly and the poles
	// easterly again, which is the pattern on the wall chart, out of one sine
	// and no cases.
	//
	// The deflection itself is Coriolis: air moving toward the equator is
	// moving toward ground that is turning faster than it is, and falls
	// behind, which is what makes an equatorward flow an easterly. That is why
	// the zonal component scales with the spin and vanishes on a body that
	// does not turn.
	inline glm::vec2 Prevailing(float latitudeDegrees, float rotationHours,
		float speed)
	{
		float b = std::abs(latitudeDegrees) / 30.0f;
		float s = std::sin(glm::pi<float>() * b);

		// Relative to Earth's day; a slow rotator has a weak deflection and a
		// nearly meridional flow. Tidally locked moons at 400 hours get almost
		// no zonal wind at all, which is right.
		float spin = 23.934f / glm::max(rotationHours, 0.1f);
		float turn = glm::clamp(spin, 0.0f, 3.0f);

		float east = -speed * s * glm::clamp(turn, 0.0f, 1.5f);

		// Surface flow is equatorward in the Hadley and polar cells and
		// poleward in the Ferrel one -- the same sine, signed by hemisphere.
		float north = -speed * 0.3f * s
			* (latitudeDegrees >= 0.0f ? 1.0f : -1.0f);

		return glm::vec2(east, north);
	}

	// **The share of the beam a latitude gets, averaged over a year.**
	//
	// Sunlight does not land evenly: the equator takes it near vertical all
	// year and a pole takes it at a grazing angle for half of one and not at
	// all for the other. The standard way to write that down is two terms of a
	// Legendre expansion -- `Q(lat) = (S/4)[1 + s2 P2(sin lat)]` -- and it is
	// standard because the second term carries almost all of it.
	//
	// `s2` is not a fitted constant here. The annual mean at a pole is exactly
	// `S sin(obliquity) / pi`, which is a clean integral, and setting the
	// expansion equal to it at `P2(1) = 1` gives `s2 = 4 sin(e)/pi - 1`.
	// Earth's obliquity puts that at -0.494 against a published -0.482, which
	// is the two-term expansion being a two-term expansion. It has the right
	// limit at the other end too: a body with no tilt gets `s2 = -1` and a
	// pole that receives nothing at all, which is correct.
	inline float MeanCosZenith(float latitudeDegrees, float obliquityDegrees)
	{
		float tilt = glm::radians(glm::clamp(obliquityDegrees, 0.0f, 90.0f));

		float s2 = 4.0f * std::sin(tilt) / glm::pi<float>() - 1.0f;

		float sinLat = std::sin(glm::radians(latitudeDegrees));
		float p2 = 0.5f * (3.0f * sinLat * sinLat - 1.0f);

		return glm::max(0.25f * (1.0f + s2 * p2), 0.0f);
	}

	// **How much heat gets carried from the equator to the poles**, which is a
	// different question from how much gets carried from noon to midnight and
	// was originally answered by the same number. That was wrong, and visibly:
	// with `Redistribution` at 0.97 for Earth -- correct, its day/night swing
	// really is small -- pooling the latitudes by the same factor left 1.6 K
	// between the equator and the pole against a real 44.
	//
	// The two are different physics. Evening out a day is thermal inertia:
	// ground and water that are still warm at dawn. Evening out a hemisphere
	// is *transport*, air and ocean actually moving heat thousands of
	// kilometres, and it is far less complete -- Earth's poles are cold, which
	// is the whole reason they have ice on them.
	//
	// 0.4 for a wet, thick atmosphere: it puts Earth's equator at 26.1 C
	// against a measured 26, and its pole at -9 C against an Arctic -16. The
	// equator is the one worth trusting; a pole is where an ice sheet's own
	// albedo takes over and this model has no such feedback.
	inline float Transport(float airColumn, float moisture)
	{
		float air = 1.0f - std::exp(-2.0f * airColumn / s_EarthColumn);

		return glm::clamp(0.42f * air
			* (0.6f + 0.4f * glm::clamp(moisture, 0.0f, 1.0f)), 0.0f, 0.9f);
	}

	// **Wind goes to zero at the ground, and the profile is logarithmic.**
	//
	// A wind speed is always quoted at a reference height -- ten metres, by
	// meteorological convention -- and the air below that is slower, because
	// the ground is not moving and viscosity has to get from one to the other.
	// The shape of that is the log wind profile,
	// `v(z) = v_ref ln(z/z0) / ln(z_ref/z0)`, which comes out of the
	// turbulent boundary layer and is what every wind measurement in the world
	// is corrected with.
	//
	// `z0` is the roughness length, a real measured property of a surface:
	// about 0.0002 m over open water, 0.03 m over grass, 0.1 m over scattered
	// obstacles, and a metre or more over forest. 0.1 here.
	//
	// This matters because without it a boulder lying on the ground gets the
	// full ten-metre wind, and a light breeze rolls it away. With it, a rock
	// whose centre is 0.9 m up sits in `ln(9)/ln(100)` = 48% of the reference
	// wind and therefore 23% of the force -- and a person's feet are in
	// slower air than their head, which is also true.
	inline float WindFraction(float height, float roughness = 0.1f)
	{
		float z0 = glm::max(roughness, 1e-4f);
		float z = glm::max(height, z0);

		return glm::clamp(std::log(z / z0) / std::log(10.0f / z0), 0.0f, 1.5f);
	}

	// Temperature at a site, in kelvin, and everything on the way to it.
	inline Weather At(const Site& site)
	{
		Weather out;

		out.SolarConstant = SolarConstant(site.StarDistanceAu);

		float absorbed = out.SolarConstant
			* (1.0f - glm::clamp(site.Albedo, 0.0f, 1.0f));

		// **Equilibrium: in equals out.** A sphere intercepts sunlight over
		// its cross-section, pi r^2, and radiates over its whole area,
		// 4 pi r^2, so the average absorbed flux is a quarter of the beam.
		// Setting that equal to sigma T^4 is the whole derivation, and for
		// Earth it gives 254 K -- the number that says the atmosphere is
		// worth 34 degrees.
		out.Equilibrium = std::pow(absorbed * 0.25f / s_Sigma, 0.25f);

		// **Where the heat actually is, in two steps that are two different
		// mechanisms.**
		//
		// First the day: thermal inertia carries some of noon's heat into the
		// night, so what this patch of ground radiates is somewhere between
		// the sun on it right now and the average over its own day at its own
		// latitude. Then the hemisphere: air and ocean carry some of the
		// tropics' heat to the poles, so the latitude's own share is pulled
		// part of the way toward the planet's mean. Doing them with one factor
		// left Earth 1.6 K warmer at the equator than at the pole.
		float pooled = Redistribution(site.AirColumn, site.Moisture);
		float here = glm::max(site.CosZenith, 0.0f);

		out.Insolation = absorbed * here;

		float daily = MeanCosZenith(site.LatitudeDegrees,
			site.ObliquityDegrees);

		float overDay = (1.0f - pooled) * here + pooled * daily;

		float moved = Transport(site.AirColumn, site.Moisture);

		float local = absorbed * ((1.0f - moved) * overDay + moved * 0.25f);

		float surface = std::pow(glm::max(local, 1e-6f) / s_Sigma, 0.25f);

		// The grey-atmosphere greenhouse, applied as a ratio so it scales the
		// day and the night together rather than adding a fixed offset to a
		// night that has not got it.
		float tau = s_GreyDepth * site.AirColumn;
		float warmed = surface * std::pow(1.0f + 0.75f * tau, 0.25f);

		out.Greenhouse = warmed - surface;

		out.LapseRate = site.AirColumn > 0.0f
			? LapseRate(site.Gravity, site.Moisture) : 0.0f;

		out.Temperature = warmed - out.LapseRate * site.Altitude * 0.001f;

		// **Pressure, and the scale height that is the only thing setting it.**
		// Hydrostatic balance against the ideal gas law gives p = p0 exp(-h/H)
		// with H = R T / g. Earth at 288 K works out to 8.4 km, against a
		// measured 8.5 -- which is the check that the constants above are the
		// ones they claim to be.
		float sea = s_EarthPressure * (site.AirColumn / s_EarthColumn);

		out.ScaleHeight = s_AirGas * glm::max(warmed, 1.0f)
			/ glm::max(site.Gravity, 0.01f);

		out.Pressure = sea * std::exp(-glm::max(site.Altitude, 0.0f)
			/ glm::max(out.ScaleHeight, 1.0f));

		out.AirDensity = out.Pressure
			/ (s_AirGas * glm::max(out.Temperature, 1.0f));

		// **Wind.** Airless bodies have none: there is no fluid to move, and a
		// pressure gradient across a vacuum is not a wind.
		if (site.AirColumn <= 0.0f)
			return out;

		// The general circulation, scaled by how hard the star is driving it.
		// A body absorbing a quarter of Earth's flux has a quarter of the
		// energy going into its cells, and the wind that gets built goes
		// roughly as the square root of the energy.
		float driving = std::sqrt(
			glm::max(absorbed, 0.0f) / (1361.0f * (1.0f - 0.306f)));

		out.Wind = Prevailing(site.LatitudeDegrees, site.RotationHours,
			9.0f * driving);

		out.WindSpeed = glm::length(out.Wind);

		return out;
	}

}
