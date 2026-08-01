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

class Lighting2D : public Egss::Layer
{
public:
	Lighting2D()
		: Layer("Lighting2D"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
	}

	void OnAttach() override
	{
		BuildScene();
	}


	void BuildScene()
	{
		m_World.Clear();

		// Static geometry: floor and two walls.
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 0.0f, -0.82f }, { 1.5f, 0.06f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ -1.45f, 0.0f }, { 0.06f, 0.9f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 1.45f, 0.0f }, { 0.06f, 0.9f }));

		// shadows, since the steps cast overlapping ones.
		for (int i = 0; i < 16; i++)
		{
			m_World.AddBody(Egss::RigidBody2D::MakeStaticBox(
				{ -1.1f + i * 0.16f, -0.72f + i * 0.09f }, { 0.08f, 0.03f }));
		}

		// Circles, deliberately with no shadow logic of their own.
		//
		// Raycast already handles circles, so rays *do* stop on them. But the
		// polygon only aims rays at box corners, and a circle has none, so its
		// shadow is decided entirely by whichever ring rays happen to graze
		// it. Expect coarse edges that jump about as the light moves, and a
		// shadow that narrows or vanishes as "Ring rays" is lowered.
		//
		// The proper fix is to aim two rays at each circle's *tangent* points
		// -- its silhouette as seen from the light -- which is exactly what
		// corners do for a box.
		// m_World.AddBody(Egss::RigidBody2D::MakeStaticCircle({ 0.60f, 0.30f }, 0.18f));
		// m_World.AddBody(Egss::RigidBody2D::MakeStaticCircle({ -0.70f, 0.35f }, 0.13f));
		// m_World.AddBody(Egss::RigidBody2D::MakeStaticCircle({ 0.05f, 0.45f }, 0.10f));
	}

	void OnUpdate(Egss::Timestep ts) override
	{
		if (g_ActiveDemo != Demo::Lighting2D)
			return;

		m_FrameTime = ts.GetMilliseconds();

		UpdateLightControl(ts);

		Egss::Renderer2D::ResetStats();
		Egss::RenderCommand::SetClearColor({ 0.06f, 0.06f, 0.08f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer2D::BeginScene(m_Camera);

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const Egss::RigidBody2D& body = bodies[i];
			glm::vec2 position = body.Position;

			// never run. One flat colour will do -- and for a lighting demo
			// the interesting question is how bright a wall is, not what
			// state its rigid body is in.
			glm::vec4 color;
			if (body.Type == Egss::BodyType::Static)
				color = { 0.30f, 0.32f, 0.38f, 1.0f };
			else if (!body.Awake)
				color = { 0.35f, 0.40f, 0.45f, 1.0f };
			else
				color = { 0.45f, 0.70f, 0.95f, 1.0f };

			if (body.Shape == Egss::ColliderShape::Box)
			{
				Egss::Renderer2D::DrawQuad(position, body.HalfExtents * 2.0f, color);
			}
			else
			{
				// Renderer2D has no circle fill, so a quad stands in. The
				// debug outline shows the true shape the rays actually hit --
				// which is worth having on while you look at this.
				Egss::Renderer2D::DrawQuad(position,
					{ body.Radius * 2.0f, body.Radius * 2.0f }, color * 0.7f);
			}
		}

		if (m_ShowColliders)
			DrawDebug();

		if (m_ShowLight)
			DrawLight();

		Egss::Renderer2D::EndScene();
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
				m_LightPosition += glm::normalize(move) * m_LightSpeed * (float)ts;
		}
		else if (m_Control == LightControl::Mouse)
		{
			// ImGui polls the same mouse. Without this check, dragging a
			// slider would also drag the light -- the engine's ImGuiLayer
			// blocks mouse *events* from reaching layers, but Input:: reads
			// the hardware directly and bypasses that entirely.
			if (!ImGui::GetIO().WantCaptureMouse)
				m_LightPosition = ScreenToWorld(Egss::Input::GetMousePosition());
		}

		// Keep it inside the walls whichever way it was moved.
		m_LightPosition = glm::clamp(m_LightPosition, glm::vec2(-1.35f), glm::vec2(1.35f));
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
			return m_LightPosition;

		float x = (mouse.first / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.second / height) * 2.0f;

		glm::vec4 world = glm::inverse(m_Camera.GetViewProjectionMatrix()) * glm::vec4(x, y, 0.0f, 1.0f);
		return glm::vec2(world);
	}

	// The light, as a visibility polygon.
	//
	// A regular fan of rays wastes most of them on empty space and still
	// misses corners, so edges shimmer. Casting *at the corners* instead is
	// exact: between any two neighbouring corner-rays the boundary is a
	// straight line, which is precisely what a triangle can represent.
	void DrawLight()
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
			// A circle has no corners, and its HalfExtents is left at the
			// default -- reading it here would invent four corners in the
			// wrong place. Skipping is what "no circle-specific logic" means:
			// circles still block rays, they just never attract one.
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
				glm::vec2 toCorner = corner - m_LightPosition;

				// Outside the light's reach, so it cannot cast a shadow.
				if (glm::dot(toCorner, toCorner) > m_LightRadius * m_LightRadius)
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
		for (int i = 0; i < m_LightRingRays; i++)
			m_Angles.push_back((float)i / (float)m_LightRingRays * glm::two_pi<float>() - glm::pi<float>());

		// 3. Sorted, so neighbouring entries are neighbouring directions and
		//    consecutive hits can simply be joined up.
		std::sort(m_Angles.begin(), m_Angles.end());

		// 4. Cast one ray per angle and keep where it landed.
		m_Hits.clear();
		m_Hits.reserve(m_Angles.size());

		for (float angle : m_Angles)
		{
			glm::vec2 direction = { std::cos(angle), std::sin(angle) };
			Egss::RaycastHit hit = m_World.Raycast(m_LightPosition, direction, m_LightRadius);

			m_Hits.push_back(hit.Hit ? hit.Point : m_LightPosition + direction * m_LightRadius);
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
		glm::vec4 centreColor = m_LightColor;

		for (size_t i = 0; i < m_Hits.size(); i++)
		{
			const glm::vec2& a = m_Hits[i];
			const glm::vec2& b = m_Hits[(i + 1) % m_Hits.size()];

			// Per-corner colour: full brightness at the light, faded at the
			// rim. The hardware interpolates between them, so the falloff
			// costs nothing.
			glm::vec4 colorA = m_LightColor * Falloff(a);
			glm::vec4 colorB = m_LightColor * Falloff(b);
			colorA.a = m_LightColor.a * Falloff(a);
			colorB.a = m_LightColor.a * Falloff(b);

			Egss::Renderer2D::DrawTriangle(
				glm::vec3(m_LightPosition, z), glm::vec3(a, z), glm::vec3(b, z),
				centreColor, colorA, colorB);
		}

		// The rays themselves, for while you are still working on it.
		if (m_ShowRays)
		{
			for (const glm::vec2& hit : m_Hits)
				Egss::Renderer2D::DrawLine(glm::vec3(m_LightPosition, z + 0.1f),
					glm::vec3(hit, z + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 0.25f));
		}
	}

	// 1 at the light, 0 at its maximum reach.
	float Falloff(const glm::vec2& point) const
	{
		float distance = glm::length(point - m_LightPosition);
		return std::max(0.0f, 1.0f - distance / m_LightRadius);
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

	void OnEvent(Egss::Event& e) override
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
			if (e.GetRepeatCount() > 0 || g_ActiveDemo != Demo::Lighting2D)
				return false;

			if (e.GetKeyCode() == EGSS_KEY_R)
				BuildScene();

			// An edge, so it belongs here rather than in the polled path.
			if (e.GetKeyCode() == EGSS_KEY_M)
				m_Control = (LightControl)(((int)m_Control + 1) % (int)LightControl::Count);

			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (g_ActiveDemo != Demo::Lighting2D)
			return;

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
		ImGui::TextDisabled("M cycles. Arrows move, or the cursor.");

		ImGui::Separator();
		ImGui::Checkbox("Show colliders", &m_ShowColliders);
		ImGui::Checkbox("Show rays", &m_ShowRays);

		// Disabled while something else is driving it, so the sliders cannot
		// silently fight the input you are actually using.
		ImGui::BeginDisabled(m_Control != LightControl::Fixed);
		ImGui::SliderFloat2("Light position", &m_LightPosition.x, -1.35f, 1.35f);
		ImGui::EndDisabled();

		ImGui::SliderFloat("Light radius", &m_LightRadius, 0.2f, 3.5f);
		ImGui::SliderInt("Ring rays", &m_LightRingRays, 4, 128);
		ImGui::SliderFloat("Move speed", &m_LightSpeed, 0.2f, 5.0f);
		ImGui::ColorEdit4("Light colour", &m_LightColor.x);

		ImGui::TextDisabled("%zu rays -> %zu triangles", m_Angles.size(), m_Hits.size());

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

	bool m_ShowLight = true;
	glm::vec2 m_LightPosition = { -0.2f, 0.45f };
	float m_LightRadius = 1.8f;
	int m_LightRingRays = 32;
	glm::vec4 m_LightColor = { 1.0f, 0.92f, 0.70f, 0.85f };

	// Reused every frame rather than reallocated -- this runs per frame and
	// the sizes barely change.
	std::vector<float> m_Angles;
	std::vector<glm::vec2> m_Hits;

	float m_FrameTime = 0.0f;
};
