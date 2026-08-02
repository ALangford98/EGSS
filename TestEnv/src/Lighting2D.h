#pragma once

#include <Egss.h>
#include <imgui.h>

// glm::two_pi -- do not rely on another header pulling this in.
#include <glm/gtc/constants.hpp>

#include "Demo.h"

// A 2D map lit by a single light source, with obstacles casting shadows.
//
// The physics world is used purely as a spatial database for Raycast -- Step()
// is never called, so nothing moves, collides or sleeps.
//
// The light is a *visibility polygon*: rays are cast at the corners of every
// obstacle, sorted by angle, and the gaps between neighbouring hits are filled
// with triangles. See docs/LIGHTING_EXERCISE.md.
// How the light is driven. Cycled with M.
enum class LightControl
{
    Fixed = 0,   // sliders in the panel
    Keyboard,    // arrow keys
    Mouse,       // follows the cursor

    Count
};

inline const char* s_LightControlNames[] = { "Fixed (sliders)", "Keyboard (arrows)", "Mouse" };

// One light. Position is worked out from the orbit for ring lights, or driven
// by input for the interactive one.
struct Light
{
	glm::vec2 Position = { 0.0f, 0.0f };
	glm::vec4 Color = { 1.0f, 0.92f, 0.70f, 0.85f };
	float Radius = 1.8f;
};

class Lighting2D : public DemoLayer
{
public:
	Lighting2D()
		: DemoLayer("Lighting2D"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
	}

	void OnDemoAttach() override
	{
		// Two is the default because opposing poles give the clearest picture:
		// one obstacle, two shadows going opposite ways.
		SetRingLightCount(2);
		BuildScene();

	}


	void BuildScene()
	{
		m_World.Clear();

		// Floor, ceiling and two walls -- a closed box, so the orbiting lights
		// always have a surface to fall on.
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 0.0f, -0.84f }, { 1.5f, 0.05f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 0.0f,  0.84f }, { 1.5f, 0.05f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ -1.45f, 0.0f }, { 0.05f, 0.9f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({  1.45f, 0.0f }, { 0.05f, 0.9f }));

		// One circle in the middle. With lights orbiting around it, its shadow
		// sweeps the room -- the clearest way to watch tangent rays working.
		m_World.AddBody(Egss::RigidBody2D::MakeStaticCircle({ 0.0f, 0.0f }, 0.22f));

		// Four steps climbing away from the bottom-left corner...
		for (int i = 0; i < 4; i++)
		{
			m_World.AddBody(Egss::RigidBody2D::MakeStaticBox(
				{ -1.15f + i * 0.20f, -0.70f + i * 0.13f }, { 0.09f, 0.035f }));
		}

		// ...and four descending from the top-right, so both halves of the
		// orbit have something to cast against.
		for (int i = 0; i < 4; i++)
		{
			m_World.AddBody(Egss::RigidBody2D::MakeStaticBox(
				{ 1.15f - i * 0.20f, 0.70f - i * 0.13f }, { 0.09f, 0.035f }));
		}
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{

		m_FrameTime = ts.GetMilliseconds();

		UpdateLightControl(ts);
		UpdateOrbit(ts);

		// One list, used by both the polygon pass and the surface shading, so
		// the two can never disagree about what is lighting the scene.
		m_ActiveLights.clear();
		for (const Light& light : m_RingLights)
			m_ActiveLights.push_back(light);
		if (m_ShowInteractive)
			m_ActiveLights.push_back(m_Interactive);

		Egss::Renderer2D::ResetStats();
		Egss::RenderCommand::SetClearColor({ 0.06f, 0.06f, 0.08f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer2D::BeginScene(m_Camera);

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const Egss::RigidBody2D& body = bodies[i];
			glm::vec2 position = body.Position;

			// Lit by raycast rather than drawn flat. In darkness a surface is
			// the background colour, so it simply isn't there; a light has to
			// reach it before it exists.
			glm::vec4 color = ShadeSurface((unsigned int)i, body);

			if (body.Shape == Egss::ColliderShape::Box)
			{
				Egss::Renderer2D::DrawQuad(position, body.HalfExtents * 2.0f, color);
			}
			else
			{
				// A real circle now, not a stand-in quad -- it joins the same
				// triangle batch as the light, so it costs no extra draw call.
				Egss::Renderer2D::DrawCircle(position, body.Radius, color, 32);
			}
		}

		if (m_ShowColliders)
			DrawDebug();

		// End the map pass before touching the blend mode: blending is global
		// state, and anything still batched would be drawn with whatever mode
		// is set at flush time rather than the one it was submitted under.
		Egss::Renderer2D::EndScene();

		if (m_ShowLight)
		{
			// Additive, so two lights overlapping are brighter than either --
			// with alpha blending the nearer one would simply hide the other.
			Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Additive);

			// And depth testing OFF, which matters just as much. Every light
			// polygon sits at the same z, and the depth test rejects anything
			// at equal depth after the first -- so the second light was being
			// discarded precisely where it overlapped the first. Additive
			// blending cannot help if the fragments never reach it.
			Egss::RenderCommand::SetDepthTest(false);

			Egss::Renderer2D::BeginScene(m_Camera);

			EGSS_PROFILE_SCOPE("Lighting::LightPass");
			m_TotalRays = 0;

			for (const Light& light : m_ActiveLights)
				DrawLight(light);

			Egss::Renderer2D::EndScene();

			Egss::RenderCommand::SetDepthTest(true);
			Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		}
	}

	// The ring lights ride a circle around the scene. Their positions are
	// derived every frame rather than stored, so changing the count or the
	// orbit radius needs no bookkeeping -- the next frame simply recomputes.
	void UpdateOrbit(Egss::Timestep ts)
	{
		if (!m_Paused)
			m_OrbitAngle += m_OrbitSpeed * (float)ts;

		// Evenly spaced. Two lights land on opposing poles, which is the
		// default because it is the clearest way to see two shadow sets from
		// the same obstacle.
		for (size_t i = 0; i < m_RingLights.size(); i++)
		{
			float angle = m_OrbitAngle + (float)i / (float)m_RingLights.size() * glm::two_pi<float>();
			m_RingLights[i].Position = m_OrbitCentre + glm::vec2(std::cos(angle), std::sin(angle)) * m_OrbitRadius;
		}
	}

	// Keeps the light list the size the GUI asked for, giving new entries a
	// colour spread around the hue circle so they are told apart at a glance.
	void SetRingLightCount(int count)
	{
		count = std::max(count, 0);

		while ((int)m_RingLights.size() > count)
			m_RingLights.pop_back();

		while ((int)m_RingLights.size() < count)
		{
			Light light;
			light.Radius = m_DefaultLightRadius;

			// Cheap hue spread: three offset cosines.
			float t = (float)m_RingLights.size() * 0.37f;
			light.Color = {
				0.55f + 0.45f * std::cos(glm::two_pi<float>() * t),
				0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.33f)),
				0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.66f)),
				0.85f
			};

			m_RingLights.push_back(light);
		}
	}

	// Moves the light by `delta`, stopping at walls and sliding along them.
	//
	// Both control modes go through this, which is why the mouse never needs a
	// special case: "follow the cursor" is just a delta towards it. The light
	// keeps up in open space and hugs the wall when the cursor crosses one,
	// rather than teleporting to the far side.
	//
	// Two passes: the first travels until it touches something, the second
	// spends what is left along the surface. Without the second, running into
	// a wall at an angle stops you dead instead of sliding.
	glm::vec2 MoveWithCollision(glm::vec2 from, glm::vec2 delta) const
	{
		if (!m_LightCollides)
			return from + delta;

		// A little gap kept between the light and the surface, so the next
		// frame's ray starts outside the wall rather than exactly on it.
		const float skin = 0.001f;

		for (int pass = 0; pass < 2; pass++)
		{
			float distance = glm::length(delta);
			if (distance < 0.00001f)
				break;

			glm::vec2 direction = delta / distance;

			// Cast far enough ahead that the light's own radius is accounted
			// for -- it is a circle, not a point.
			Egss::RaycastHit hit = m_World.Raycast(from, direction, distance + m_LightCollisionRadius);

			if (!hit.Hit || hit.Distance > distance + m_LightCollisionRadius)
			{
				from += delta;
				break;
			}

			// Up to the contact, never past it.
			float travel = std::max(0.0f, hit.Distance - m_LightCollisionRadius - skin);
			from += direction * travel;

			// Whatever is left, with the part heading into the surface removed
			// -- that remainder is the slide.
			delta = delta - direction * travel;
			delta -= hit.Normal * glm::dot(delta, hit.Normal);
		}

		// Anything that still overlaps -- a corner, or a start already inside
		// geometry -- gets pushed out here, so the light can never get stuck.
		return m_World.ResolveCircle(from, m_LightCollisionRadius);
	}

	// ---------------------------------------------------------------------
	// Input
	//
	// Two different kinds of input, and they want different mechanisms:
	//
	//   * **Held keys** are a *state* question -- "is left held right now" --
	//     so they are polled, every frame, and scaled by the timestep. Using
	//     an event here would move the light once per press.
	//   * **Switching mode** is an *edge* -- it happens once when the key goes
	//     down -- so it is an event, in OnEvent. Polling for it would flip the
	//     mode every frame the key was held.
	//
	// That split is the whole of input handling in this engine. See
	// docs/ENGINE.md, decision 2.
	// ---------------------------------------------------------------------
	void UpdateLightControl(Egss::Timestep ts)
	{
		if (m_Control == LightControl::Keyboard)
		{
			glm::vec2 move(0.0f);

			// Polled, not events: held keys are continuous movement.
			if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  move.x -= 1.0f;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) move.x += 1.0f;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  move.y -= 1.0f;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    move.y += 1.0f;

			// Normalise, or moving diagonally is 1.41x faster than straight.
			if (glm::dot(move, move) > 0.0f)
			{
				glm::vec2 delta = glm::normalize(move) * m_LightSpeed * (float)ts;
				m_Interactive.Position = MoveWithCollision(m_Interactive.Position, delta);
			}
		}
		else if (m_Control == LightControl::Mouse)
		{
			// ImGui polls the same mouse. Without this check, dragging a
			// slider would also drag the light -- the engine's ImGuiLayer
			// blocks mouse *events* from reaching layers, but Input:: reads
			// the hardware directly and bypasses that entirely.
			if (!ImGui::GetIO().WantCaptureMouse)
			{
				// A delta towards the cursor rather than a jump to it, so the
				// same sweep that blocks the keyboard blocks this too. In open
				// space it lands exactly on the cursor; against a wall it
				// stops at the surface instead of appearing on the far side.
				glm::vec2 target = ScreenToWorld(Egss::Input::GetMousePosition());
				m_Interactive.Position = MoveWithCollision(m_Interactive.Position,
					target - m_Interactive.Position);
			}
		}

		// Keep it inside the walls whichever way it was moved.
		m_Interactive.Position = glm::clamp(m_Interactive.Position, glm::vec2(-1.35f), glm::vec2(1.35f));
	}

	// Window pixels -> world units.
	//
	// Three steps, and each one is a place to get it wrong:
	//   1. pixels    -> 0..1 across the window
	//   2. 0..1      -> -1..+1 clip space, flipping y because window
	//                   coordinates count downwards and clip space counts up
	//   3. clip      -> world, via the inverse of the camera's view-projection
	glm::vec2 ScreenToWorld(const std::pair<float, float>& mouse) const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();

		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();
		if (width <= 0.0f || height <= 0.0f)
			return m_Interactive.Position;

		float x = (mouse.first / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.second / height) * 2.0f;

		glm::vec4 world = glm::inverse(m_Camera.GetViewProjectionMatrix()) * glm::vec4(x, y, 0.0f, 1.0f);
		return glm::vec2(world);
	}

	// How lit is this body?
	//
	// One ray per light, aimed at the body's centre. If the *first* thing that
	// ray meets is this body, its face is in view of that light; if something
	// else is in the way, it is in shadow. That is the same query the light
	// polygon makes, just asked per object instead of per direction.
	//
	// Aiming at the centre works even for a wall, whose centre is inside
	// itself: the ray hits its near face first, which is the face being lit.
	glm::vec4 ShadeSurface(unsigned int index, const Egss::RigidBody2D& body)
	{
		glm::vec3 accumulated(m_SurfaceAmbient);

		for (const Light& light : m_ActiveLights)
		{
			glm::vec2 toBody = body.Position - light.Position;
			float distance = glm::length(toBody);

			if (distance > light.Radius || distance < 0.0001f)
				continue;

			// A little past the centre, so a ray that only grazes the near
			// face still registers as reaching it.
			Egss::RaycastHit hit = m_World.Raycast(light.Position, toBody, distance + 0.05f);

			if (!hit.Hit || hit.Body != index)
				continue;

			float falloff = 1.0f - hit.Distance / light.Radius;
			accumulated += glm::vec3(light.Color) * (falloff * light.Color.a * m_SurfaceGain);
		}

		return glm::vec4(glm::min(accumulated, glm::vec3(1.0f)), 1.0f);
	}

	// The light, as a visibility polygon.
	//
	// A regular fan of rays wastes most of them on empty space and still
	// misses corners, so edges shimmer. Casting *at the corners* instead is
	// exact: between any two neighbouring corner-rays the boundary is a
	// straight line, which is precisely what a triangle can represent.
	void DrawLight(const Light& light)
	{
		m_Angles.clear();

		// 1. One angle per obstacle corner, plus a pair either side of it.
		//
		//    The offsets are the whole trick. A ray aimed exactly at a corner
		//    stops there. The two nudged rays slip *past* it and carry on to
		//    whatever is behind, and the gap between where they land is the
		//    shadow's edge. Remove the offsets and you get spikes and gaps.
		const float nudge = 0.0001f;

		for (const Egss::RigidBody2D& body : m_World.GetBodies())
		{
			// A circle has no corners, so its HalfExtents is meaningless --
			// reading it would invent four corners in the wrong place. What a
			// circle has instead is a *silhouette*: the two points where a ray
			// from the light just grazes it. Those are its corners, as far as
			// shadows are concerned.
			//
			//        light o- - - - - - - .
			//                \  half   . '   tangent
			//                 \  .  '
			//              base \'   ( )  circle
			//                    ` .
			//                        ` .   tangent
			//
			// base is the angle to the centre; half is the angle it subtends,
			// asin(radius / distance). Add and subtract it for the two edges.
			if (body.Shape == Egss::ColliderShape::Circle)
			{
				glm::vec2 toCentre = body.Position - light.Position;
				float distance = glm::length(toCentre);

				// Out of reach, or the light is inside it -- in which case
				// there is no silhouette, and asin would be out of domain.
				if (distance > light.Radius + body.Radius || distance <= body.Radius)
					continue;

				float base = std::atan2(toCentre.y, toCentre.x);
				float half = std::asin(body.Radius / distance);

				// Just *outside* the tangents, for the same reason the box
				// corners are nudged: a ray exactly on the tangent grazes the
				// circle and stops, and the shadow edge needs the ray that
				// slips past it.
				m_Angles.push_back(base - half - nudge);
				m_Angles.push_back(base + half + nudge);

				// And just inside, so the lit edge of the circle itself is
				// sampled rather than being cut off by the chord between the
				// two outer rays.
				m_Angles.push_back(base - half + nudge);
				m_Angles.push_back(base + half - nudge);

				continue;
			}

			if (body.Shape != Egss::ColliderShape::Box)
				continue;

			const glm::vec2 corners[4] = {
				body.Position + glm::vec2(-body.HalfExtents.x, -body.HalfExtents.y),
				body.Position + glm::vec2( body.HalfExtents.x, -body.HalfExtents.y),
				body.Position + glm::vec2( body.HalfExtents.x,  body.HalfExtents.y),
				body.Position + glm::vec2(-body.HalfExtents.x,  body.HalfExtents.y)
			};

			for (const glm::vec2& corner : corners)
			{
				glm::vec2 toCorner = corner - light.Position;

				// Outside the light's reach, so it cannot cast a shadow.
				if (glm::dot(toCorner, toCorner) > light.Radius * light.Radius)
					continue;

				float angle = std::atan2(toCorner.y, toCorner.x);
				m_Angles.push_back(angle - nudge);
				m_Angles.push_back(angle);
				m_Angles.push_back(angle + nudge);
			}
		}

		// 2. A coarse ring as well. Where nothing blocks the light its edge is
		//    a circle, and corner rays alone would cut straight across it --
		//    the unobstructed part would come out as a polygon with very few,
		//    very long sides.
		// Scaled to the light's size, not a fixed count. The rim is a polygon
		// of straight chords, and how far each one sags from the true circle
		// is R * (1 - cos(pi / N)) -- proportional to R. A fixed N therefore
		// looks fine on a small light and visibly square on a big one, which
		// is exactly the "square shadows at a distance" symptom.
		int ringRays = std::min(std::max((int)(light.Radius * m_RingDensity), 16), 160);

		for (int i = 0; i < ringRays; i++)
			m_Angles.push_back((float)i / (float)ringRays * glm::two_pi<float>() - glm::pi<float>());

		// 3. Sorted, so neighbouring entries are neighbouring directions and
		//    consecutive hits can simply be joined up.
		std::sort(m_Angles.begin(), m_Angles.end());

		// 4. Cast one ray per angle and keep where it landed.
		m_Hits.clear();
		m_Hits.reserve(m_Angles.size());

		for (float angle : m_Angles)
		{
			glm::vec2 direction = { std::cos(angle), std::sin(angle) };
			Egss::RaycastHit hit = m_World.Raycast(light.Position, direction, light.Radius);

			// Overshoot the hit slightly so the polygon laps onto the surface
			// it stopped at. Land exactly on the surface and the wall is never
			// lit -- the light stops at the boundary and the face stays flat
			// grey, which is what "the walls aren't illuminated" looks like.
			m_Hits.push_back(hit.Hit
				? hit.Point + direction * m_SurfaceSpill
				: light.Position + direction * light.Radius);
		}

		if (m_Hits.size() < 2)
			return;

		// 5. Fan out from the light: one triangle per neighbouring pair, and a
		//    final one wrapping the last back to the first to close the ring.
		//
		//    z sits above the map because depth testing is on and, with this
		//    projection, a higher z is *nearer*. At equal z the first thing
		//    drawn wins, so a light at z = 0 would simply not appear.
		const float z = 0.5f;

		for (size_t i = 0; i < m_Hits.size(); i++)
		{
			const glm::vec2& a = m_Hits[i];
			const glm::vec2& b = m_Hits[(i + 1) % m_Hits.size()];

			// Per-corner colour: full brightness at the light, faded at the
			// rim. The hardware interpolates between them, so the falloff
			// costs nothing.
			float fa = Falloff(light, a);
			float fb = Falloff(light, b);

			glm::vec4 colorA = light.Color * fa;
			glm::vec4 colorB = light.Color * fb;
			colorA.a = light.Color.a * fa;
			colorB.a = light.Color.a * fb;

			Egss::Renderer2D::DrawTriangle(
				glm::vec3(light.Position, z), glm::vec3(a, z), glm::vec3(b, z),
				light.Color, colorA, colorB);
		}

		m_TotalRays += (unsigned int)m_Hits.size();

		// The rays themselves, for while you are still working on it.
		if (m_ShowRays)
		{
			for (const glm::vec2& hit : m_Hits)
				Egss::Renderer2D::DrawLine(glm::vec3(light.Position, z + 0.1f),
					glm::vec3(hit, z + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 0.25f));
		}
	}

	// 1 at the light, 0 at its maximum reach.
	static float Falloff(const Light& light, const glm::vec2& point)
	{
		float distance = glm::length(point - light.Position);
		return std::max(0.0f, 1.0f - distance / light.Radius);
	}

	void DrawDebug()
	{
		const auto& bodies = m_World.GetBodies();

		for (const Egss::RigidBody2D& body : bodies)
		{
			glm::vec2 position = body.Position;
			glm::vec4 outline = { 0.2f, 0.9f, 0.5f, 1.0f };

			if (body.Shape == Egss::ColliderShape::Box)
				Egss::Renderer2D::DrawRect(position, body.HalfExtents * 2.0f, outline);
			else
				DrawCircleOutline(position, body.Radius, outline);
		}

		// The interactive light's collision circle -- it is a body of that
		// size to the mover, not a point, which is what stops it slipping
		// through the seam where two boxes meet.
		if (m_ShowInteractive && m_LightCollides)
		{
			DrawCircleOutline(m_Interactive.Position, m_LightCollisionRadius,
				glm::vec4(1.0f, 0.85f, 0.3f, 0.6f));
		}
	}

	// (keeping to draw the light's radius.)
	void DrawCircleOutline(const glm::vec2& centre, float radius, const glm::vec4& color)
	{
		const int segments = 16;
		glm::vec2 previous = centre + glm::vec2(radius, 0.0f);

		for (int i = 1; i <= segments; i++)
		{
			float angle = (float)i / (float)segments * glm::two_pi<float>();
			glm::vec2 next = centre + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
			Egss::Renderer2D::DrawLine(previous, next, color);
			previous = next;
		}
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
			{
				float aspect = (float)e.GetWidth() / (float)e.GetHeight();
				m_Camera.SetProjection(-aspect * 0.9f, aspect * 0.9f, -0.9f, 0.9f);
			}
			return false;
		});

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			if (e.GetKeyCode() == EGSS_KEY_R)
				BuildScene();

			// An edge, so it belongs here rather than in the polled path.
			if (e.GetKeyCode() == EGSS_KEY_M)
				m_Control = (LightControl)(((int)m_Control + 1) % (int)LightControl::Count);

			return false;
		});
	}

	void OnDemoImGui() override
	{

		auto stats = Egss::Renderer2D::GetStats();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Lighting2D");

		// ADD stats.TriangleCount once Stage 4 is drawing triangles -- that
		// number staying at 1 draw call as the polygon grows is the thing to
		// watch.
		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		// Watch the draw-call count stay flat as the polygon grows -- the
		// triangles are one batch however many you submit.
		ImGui::Text("Draw calls: %u   Quads: %u   Tris: %u   Lines: %u",
			stats.DrawCalls, stats.QuadCount, stats.TriangleCount, stats.LineCount);

		ImGui::Separator();

		int control = (int)m_Control;
		if (ImGui::Combo("Control", &control, s_LightControlNames, (int)LightControl::Count))
			m_Control = (LightControl)control;
		ImGui::TextDisabled("M cycles. Arrows move, or the cursor. P pauses the orbit.");

		ImGui::Separator();
		ImGui::Checkbox("Show colliders", &m_ShowColliders);
		ImGui::SameLine();
		ImGui::Checkbox("Show rays", &m_ShowRays);
		ImGui::Checkbox("Draw lights", &m_ShowLight);
		ImGui::SameLine();
		ImGui::Checkbox("Interactive light", &m_ShowInteractive);

		// --- The orbiting ring ---
		ImGui::SeparatorText("Ring");

		int count = (int)m_RingLights.size();
		if (ImGui::SliderInt("Light count", &count, 0, 8))
			SetRingLightCount(count);

		ImGui::SliderFloat("Orbit radius", &m_OrbitRadius, 0.0f, 1.4f);
		ImGui::SliderFloat("Orbit speed", &m_OrbitSpeed, -3.0f, 3.0f);
		ImGui::SliderFloat2("Orbit centre", &m_OrbitCentre.x, -1.0f, 1.0f);

		// Per light. Pushing the index as an ID is what stops every row's
		// widgets sharing state -- ImGui identifies controls by label, and
		// these labels are all identical.
		for (size_t i = 0; i < m_RingLights.size(); i++)
		{
			ImGui::PushID((int)i);

			ImGui::ColorEdit4("##colour", &m_RingLights[i].Color.x,
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##radius", &m_RingLights[i].Radius, 0.2f, 3.5f, "radius %.2f");

			ImGui::PopID();
		}

		// --- The one you drive ---
		if (m_ShowInteractive)
		{
			ImGui::SeparatorText("Interactive");

			ImGui::BeginDisabled(m_Control != LightControl::Fixed);
			ImGui::SliderFloat2("Position", &m_Interactive.Position.x, -1.35f, 1.35f);
			ImGui::EndDisabled();

			ImGui::SliderFloat("Radius", &m_Interactive.Radius, 0.2f, 3.5f);
			ImGui::ColorEdit4("Colour", &m_Interactive.Color.x);
			ImGui::SliderFloat("Move speed", &m_LightSpeed, 0.2f, 5.0f);
			ImGui::Checkbox("Collides with walls", &m_LightCollides);
			ImGui::SliderFloat("Light body radius", &m_LightCollisionRadius, 0.01f, 0.2f);
		}

		ImGui::SeparatorText("Quality");

		// Rays per unit of light radius, rather than a flat count: the rim's
		// error grows with radius, so a fixed number looks fine on a small
		// light and square on a big one.
		ImGui::SliderFloat("Ring density", &m_RingDensity, 4.0f, 80.0f, "%.0f rays / unit");
		ImGui::SliderFloat("Surface spill", &m_SurfaceSpill, 0.0f, 0.2f);
		ImGui::SliderFloat("Surface ambient", &m_SurfaceAmbient, 0.0f, 0.4f);
		ImGui::SliderFloat("Surface gain", &m_SurfaceGain, 0.0f, 2.0f);
		ImGui::TextDisabled("%u rays cast last frame, %zu lights",
			m_TotalRays, m_RingLights.size() + (m_ShowInteractive ? 1 : 0));

		ImGui::End();
	}
private:
	//the map, and the query structure the light casts into.
	Egss::OrthographicCamera m_Camera;
	Egss::PhysicsWorld2D m_World;

	bool m_ShowColliders = true;
	bool m_ShowRays = false;

	LightControl m_Control = LightControl::Mouse;
	float m_LightSpeed = 1.6f;

	// The light is a small circle for collision purposes, not a point -- a
	// point would slip through the seam where two boxes meet.
	bool m_LightCollides = true;
	float m_LightCollisionRadius = 0.05f;

	bool m_ShowLight = true;
	bool m_ShowInteractive = true;

	// Freezes the orbit. Was dead when nothing moved; the ring gives P a job.
	bool m_Paused = false;

	// The light you drive with the mouse or arrows.
	Light m_Interactive = { { -0.2f, 0.45f }, { 1.0f, 0.92f, 0.70f, 0.85f }, 1.8f };

	// The ring that orbits the scene. Positions are recomputed each frame from
	// the orbit, so only colour and radius are actually stored per light.
	std::vector<Light> m_RingLights;
	glm::vec2 m_OrbitCentre = { 0.0f, 0.0f };
	float m_OrbitRadius = 1.05f;
	float m_OrbitSpeed = 0.5f;
	float m_OrbitAngle = 0.0f;
	float m_DefaultLightRadius = 1.6f;

	// Rays per world unit of light radius. See DrawLight.
	float m_RingDensity = 24.0f;

	// How far past a hit the light polygon reaches, in world units, so
	// surfaces catch the light rather than the light stopping dead at them.
	float m_SurfaceSpill = 0.05f;

	// What a surface looks like with no light on it at all. Matching the clear
	// colour makes unlit geometry genuinely invisible rather than a dark
	// shape -- raise it if you want the room readable in the dark.
	float m_SurfaceAmbient = 0.06f;
	float m_SurfaceGain = 0.9f;

	// Rebuilt each frame: the ring plus the interactive light, if enabled.
	std::vector<Light> m_ActiveLights;
	unsigned int m_TotalRays = 0;

	// Reused every frame rather than reallocated -- this runs per frame and
	// the sizes barely change.
	std::vector<float> m_Angles;
	std::vector<glm::vec2> m_Hits;

	float m_FrameTime = 0.0f;
};
