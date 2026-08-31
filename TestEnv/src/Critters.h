#pragma once

#include <Egss.h>

#include <glm/glm.hpp>

#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// **Four kinds of animal, and only three rules between them.**
//
// The point of this header is that a flock of birds and a school of fish are
// the *same mechanism* in a different fluid, and that a swarm of midges and a
// beetle crossing a path are each one line of physics with a name. Nothing
// here is an animation: every one of them is a state that gets integrated, so
// each has a number that can be checked against a formula this file does not
// compute.
//
//   * **Birds and fish are boids.** Reynolds' three rules -- keep apart, match
//     heading, close up -- and nothing else. The two differ in speed, in how
//     far they see, and in what confines them.
//
//   * **Midges are an Ornstein-Uhlenbeck process.** A lek swarm really is a
//     cloud of insects each doing a damped random walk about a marker on the
//     ground, which is a spring, a drag and a noise. Its stationary spread is
//     `sigma^2 / (2 gamma k)` per axis, which nothing in the step computes and
//     which the swarm can be measured against.
//
//   * **Beetles are an active Brownian particle.** Constant speed, heading
//     diffusing. Over times long against `1 / D_r` the mean square
//     displacement grows as `4 D t` with `D = v^2 / (2 D_r)` -- the standard
//     result for a run in two dimensions, and the check that says the walk is
//     a walk rather than a wander with a period in it.
//
// **Everything steps on the fixed timestep.** Nothing in here reads a clock.
// ---------------------------------------------------------------------------
namespace Life
{
	// **One stream per animal, advanced in place.** A shared generator would
	// make each animal's path depend on how many others there were and on the
	// order they were stepped in; a per-animal xorshift keeps a run
	// reproducible whatever else is going on. Same reasoning as the scatter
	// keys in `Vegetation.h`, arrived at from the other end.
	inline unsigned int Next(unsigned int& state)
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;

		return state;
	}

	inline float Unit(unsigned int& state)
	{
		return (float)(Next(state) & 0xFFFFFFu) / 16777216.0f;
	}

	inline float Signed(unsigned int& state)
	{
		return Unit(state) * 2.0f - 1.0f;
	}

	// A draw with **variance one**, which is what the noise terms below are
	// written against. Uniform on [-sqrt(3), sqrt(3)] has variance
	// `(2 sqrt 3)^2 / 12 = 1`, so it can stand in for a normal wherever only
	// the second moment matters -- which, for a linear system, is everywhere.
	inline float Wiggle(unsigned int& state)
	{
		return Signed(state) * 1.7320508f;
	}

	struct Critter
	{
		glm::vec3 At = glm::vec3(0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f);

		// Where the swarm this one belongs to is anchored. Only the midges use
		// it; for the rest it is where they were born, which is a useful thing
		// to be able to look at.
		glm::vec3 Home = glm::vec3(0.0f);

		// Wingbeat or gait, advanced by speed rather than by time -- a bird
		// gliding does not flap and a beetle standing still does not step.
		float Phase = 0.0f;

		float Size = 1.0f;

		unsigned int Seed = 1u;
	};

	// --- Boids ---------------------------------------------------------------

	struct Flock
	{
		float Speed = 7.0f;         // what it cruises at
		float Turn = 3.0f;          // how hard it can steer, m/s^2 per unit
		float Separation = 3.0f;    // closer than this and it pulls away
		float Neighbour = 12.0f;    // further than this and it is not seen
		float Apart = 1.6f;         // weights on the three rules
		float Together = 0.35f;
		float Match = 0.55f;
	};

	// **Reynolds' three rules, and the fourth thing that is not a rule.**
	//
	// Separation, cohesion and alignment are computed over the neighbours
	// inside `Neighbour` and summed as accelerations; `confine` is whatever
	// keeps the flock in its own fluid, and is a caller's business because a
	// lake and a sky are not confined the same way.
	//
	// The steering is capped rather than the velocity being set outright: a
	// boid that snaps to its desired heading is a boid with no inertia, and
	// what a flock looks like is entirely inertia.
	inline void StepFlock(std::vector<Critter>& boids, const Flock& rule,
		float dt, const std::function<glm::vec3(const Critter&)>& confine)
	{
		// Read the positions before any of them move. Stepping in place makes
		// the last boid in the list steer against a flock that has already
		// moved and the first against one that has not, which is a bias with
		// no meaning -- and it makes the result depend on the list order.
		std::vector<glm::vec3> was(boids.size());

		for (size_t i = 0; i < boids.size(); i++)
			was[i] = boids[i].At;

		float near2 = rule.Separation * rule.Separation;
		float far2 = rule.Neighbour * rule.Neighbour;

		for (size_t i = 0; i < boids.size(); i++)
		{
			Critter& boid = boids[i];

			glm::vec3 away(0.0f), middle(0.0f), heading(0.0f);
			int seen = 0;

			for (size_t j = 0; j < boids.size(); j++)
			{
				if (j == i)
					continue;

				glm::vec3 gap = was[j] - was[i];

				float range2 = glm::dot(gap, gap);

				if (range2 > far2 || range2 < 1e-6f)
					continue;

				// **Separation goes as one over the distance**, so a boid that
				// is nearly touching pulls away far harder than one at the
				// edge of the bubble. A constant push makes a flock breathe.
				if (range2 < near2)
					away -= gap / range2;

				middle += was[j];
				heading += boids[j].Velocity;

				seen++;
			}

			glm::vec3 steer = away * rule.Apart;

			if (seen > 0)
			{
				middle /= (float)seen;
				heading /= (float)seen;

				steer += (middle - was[i]) * rule.Together;
				steer += (heading - boid.Velocity) * rule.Match;
			}

			steer += confine(boid);

			float effort = glm::length(steer);

			if (effort > rule.Turn)
				steer *= rule.Turn / effort;

			boid.Velocity += steer * dt;

			// Cruising speed, not a speed limit: an animal that has to keep
			// station in a flock speeds up as readily as it slows down.
			float speed = glm::length(boid.Velocity);

			if (speed > 1e-4f)
				boid.Velocity *= glm::mix(1.0f, rule.Speed / speed,
					glm::min(1.0f, 3.0f * dt));

			boid.At += boid.Velocity * dt;

			boid.Phase += speed * dt;
		}
	}

	// --- A lek swarm ----------------------------------------------------------

	struct Swarm
	{
		float Stiffness = 9.0f;   // k, the pull back to the marker
		float Damping = 2.2f;     // gamma
		float Noise = 2.0f;       // sigma
	};

	// **Midges over a marker, which is a spring with a drag and a kick.**
	//
	//     dv = (-k x - gamma v) dt + sigma dW
	//
	// The stationary spread of that is `sigma^2 / (2 gamma k)` per axis, and
	// the swarm's radius is therefore a thing the parameters *predict* rather
	// than a thing a radius constant is set to. That is the whole reason to
	// write it this way round: a hand-set radius says nothing, and this can be
	// measured against a formula the step never evaluates.
	//
	// Real lek swarms do this. The insects are not going anywhere; they are
	// holding station over a mark on the ground, and each one's excursion from
	// it is a damped random walk.
	inline void StepSwarm(std::vector<Critter>& swarm, const Swarm& rule,
		float dt)
	{
		float kick = rule.Noise * std::sqrt(dt);

		for (Critter& one : swarm)
		{
			glm::vec3 offset = one.At - one.Home;

			glm::vec3 noise(Wiggle(one.Seed), Wiggle(one.Seed),
				Wiggle(one.Seed));

			one.Velocity += (-rule.Stiffness * offset
				- rule.Damping * one.Velocity) * dt + noise * kick;

			one.At += one.Velocity * dt;

			one.Phase += 40.0f * dt;
		}
	}

	// --- A walk on the ground -------------------------------------------------

	struct Crawl
	{
		float Speed = 0.35f;      // v
		float Turning = 1.4f;     // D_r, the heading's diffusion
		float Homing = 0.06f;     // a slow pull back, or they leave the map
		float Range = 14.0f;      // how far from home before the pull bites
	};

	// **An active Brownian particle, which is what a beetle is.**
	//
	// Constant speed, heading diffusing. Over times long against `1 / D_r` the
	// mean square displacement grows as `4 D t` in two dimensions, with
	// `D = v^2 / (2 D_r)` -- a result this step does not compute and can
	// therefore be checked against.
	//
	// The heading increment has variance `2 D_r dt`, so a variance-one draw is
	// scaled by its square root. Getting that scaling wrong is the classic way
	// to write a random walk whose diffusion depends on the timestep.
	inline void StepCrawl(std::vector<Critter>& crawlers, const Crawl& rule,
		float dt, const std::function<float(float, float)>& ground)
	{
		float turn = std::sqrt(2.0f * rule.Turning * dt);

		for (Critter& one : crawlers)
		{
			// The heading lives in the velocity, which is kept unit-length in
			// the plane -- there is no third degree of freedom in a walk.
			glm::vec2 facing(one.Velocity.x, one.Velocity.z);

			if (glm::dot(facing, facing) < 1e-8f)
				facing = glm::vec2(1.0f, 0.0f);

			facing = glm::normalize(facing);

			float angle = std::atan2(facing.y, facing.x)
				+ Wiggle(one.Seed) * turn;

			facing = glm::vec2(std::cos(angle), std::sin(angle));

			// A slow bias home, so a walk that is genuinely diffusive does not
			// diffuse off the edge of the world. Weak enough to leave the
			// short-time statistics alone, which is what the check measures.
			glm::vec2 back(one.Home.x - one.At.x, one.Home.z - one.At.z);

			float out = glm::length(back);

			if (out > rule.Range)
				facing = glm::normalize(facing
					+ back / out * rule.Homing * (out - rule.Range));

			one.Velocity = glm::vec3(facing.x, 0.0f, facing.y) * rule.Speed;

			one.At += one.Velocity * dt;
			one.At.y = ground(one.At.x, one.At.z);

			one.Phase += 9.0f * dt;
		}
	}
}
