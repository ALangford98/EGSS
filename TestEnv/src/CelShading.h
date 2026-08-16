#pragma once

#include <Egss.h>
#include "Demo.h"

#include <glm/gtc/matrix_transform.hpp>

// Cel shading: two mechanisms, and neither is a filter over a finished image.
//
//   1. **Quantise the lighting.** A lit surface's brightness is a continuous
//      function of the angle to the light; snapping it to a few discrete
//      levels is what produces flat blocks of colour with hard edges between
//      them. That is one line of arithmetic in the fragment shader.
//
//   2. **Draw an outline by inflating the mesh.** Render the model a second
//      time, pushed outward along its normals and with the *front* faces
//      culled, so only the far side of the inflated copy is drawn. It sticks
//      out past the real silhouette by the amount of the push, and nowhere
//      else, because everywhere else the real model is nearer and wins the
//      depth test.
//
// Both are cheap and both are exactly measurable, which is why this demo can
// state what it should look like rather than only showing it.
class CelShading : public DemoLayer
{
public:
	CelShading()
		: DemoLayer("CelShading")
	{
		// The spin reaches the simulation, so it is recorded. The cel
		// parameters below it are deliberately *not* registered: they change
		// the picture rather than the run, and a replay that forced them back
		// would stop the sliders from being usable while a recording is being
		// watched.
		RegisterParam("Spin rate", &m_SpinRate);
	}

	void OnDemoAttach() override
	{
		m_Sphere.reset(Egss::Mesh::CreateSphere(1.0f, 48, 24));
		m_Ground.reset(Egss::Mesh::CreatePlane(14.0f));

		// Loaded rather than generated, so the demo says something about real
		// assets. Both are smooth-normalled, which matters -- see the note on
		// the outline pass.
		m_Torus.reset(Egss::Mesh::Load("assets/models/torus.obj"));
		m_Icosahedron.reset(Egss::Mesh::Load("assets/models/icosahedron.obj"));

		BuildShaders();
	}

	// Anything that moves lives here, not in OnDemoUpdate. A demo that spins
	// from wall-clock time cannot reproduce itself run to run, let alone under
	// replay.
	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		m_Spin += m_SpinRate * step;
		if (m_Spin > 360.0f)
			m_Spin -= 360.0f;
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		(void)ts;

		Egss::RenderCommand::SetClearColor(m_Background);
		Egss::RenderCommand::Clear();

		PositionCamera();
		Egss::Renderer::BeginScene(m_Camera);

		glm::vec3 lightDirection = glm::normalize(glm::vec3(
			std::cos(glm::radians(m_LightAngle)) * -0.8f,
			-m_LightHeight,
			std::sin(glm::radians(m_LightAngle)) * -0.8f));

		m_Scene->Set("u_LightDirection", lightDirection);
		m_Scene->Set("u_CameraPosition", m_Camera.GetPosition());
		m_Scene->Set("u_Ambient", m_Ambient);
		m_Scene->Set("u_Bands", m_Bands);
		m_Scene->Set("u_Quantise", m_Quantise ? 1.0f : 0.0f);
		m_Scene->Set("u_SpecularSize", m_SpecularSize);
		m_Scene->Set("u_SpecularStrength", m_SpecularStrength);
		m_Scene->Set("u_RimWidth", m_RimWidth);
		m_Scene->Set("u_RimStrength", m_RimStrength);

		Egss::Window& window = Egss::Application::Get().GetWindow();
		glm::vec3 viewport((float)window.GetWidth(), (float)window.GetHeight(), 0.0f);
		m_OutlineScene->Set("u_Viewport", viewport);
		m_OutlineScene->Set("u_OutlinePixels", m_OutlinePixels);
		m_OutlineScene->Set("u_OutlineColour", m_OutlineColour);

		DrawObject(m_Sphere, glm::vec3(-3.2f, 1.0f, 0.0f), m_Spin,
			glm::vec4(0.90f, 0.35f, 0.30f, 1.0f));
		DrawObject(m_Torus, glm::vec3(0.0f, 1.1f, 0.0f), m_Spin * 0.7f,
			glm::vec4(0.35f, 0.62f, 0.90f, 1.0f));
		DrawObject(m_Icosahedron, glm::vec3(3.2f, 1.1f, 0.0f), -m_Spin * 0.5f,
			glm::vec4(0.55f, 0.85f, 0.45f, 1.0f));

		// The ground gets no outline. An inflated plane is a slightly larger
		// plane, so its "outline" would be a frame around the whole floor --
		// correct behaviour from the mechanism, and not what anyone wants.
		DrawGround();

		Egss::Renderer::EndScene();
	}

	void OnDemoImGui() override;

private:
	// One object: outline pass, then the shaded pass over the top.
	void DrawObject(const std::shared_ptr<Egss::Mesh>& mesh, const glm::vec3& where,
		float spin, const glm::vec4& colour)
	{
		if (!mesh)
			return;

		glm::mat4 transform = glm::rotate(glm::translate(glm::mat4(1.0f), where),
			glm::radians(spin), glm::vec3(0.0f, 1.0f, 0.0f));

		// A pure rotation and a translation, so the inverse transpose is the
		// rotation itself. It is computed rather than assumed because the next
		// person to add a scale here should not have to notice.
		glm::mat4 normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(transform))));

		if (m_Outlines && m_OutlinePixels > 0.0f)
		{
			// **Cull the front faces**, leaving the far side of the inflated
			// copy. Drawing the near side instead would simply cover the model
			// in flat black -- the inflated hull encloses it.
			Egss::RenderCommand::SetCullFace(Egss::CullFace::Front);
			m_Outline->Set("u_NormalMatrix", normalMatrix);
			Egss::Renderer::Submit(m_Outline, mesh, transform);

			// **Back to None, which is the engine's default -- not to Back.**
			// Cull state is global and outlives the demo that set it: leaving
			// it on Back here left every demo selected afterwards culling back
			// faces, which made OpenWorld's single-sided water plane vanish the
			// moment the camera went under it. Cube3D already restored to None;
			// this did not.
			Egss::RenderCommand::SetCullFace(Egss::CullFace::None);
		}

		m_Object->Set("u_NormalMatrix", normalMatrix);
		m_Object->Set("u_BaseColour", colour);
		Egss::Renderer::Submit(m_Object, mesh, transform);
	}

	void DrawGround()
	{
		if (!m_Ground)
			return;

		m_Object->Set("u_NormalMatrix", glm::mat4(1.0f));
		m_Object->Set("u_BaseColour", glm::vec4(0.30f, 0.31f, 0.38f, 1.0f));
		Egss::Renderer::Submit(m_Object, m_Ground, glm::mat4(1.0f));
	}

	void PositionCamera()
	{
		float angle = glm::radians(m_Orbit);
		float distance = 11.0f;

		m_Camera.SetPosition(glm::vec3(
			std::sin(angle) * distance, m_CameraHeight, std::cos(angle) * distance));

		// Yaw 0 looks along +x, so the heading that points back at the origin
		// from an orbit position is -(orbit) - 90 degrees.
		m_Camera.SetRotation(-m_Orbit - 90.0f, -m_CameraPitch);
	}

	void BuildShaders();

	Egss::PerspectiveCamera m_Camera{ 50.0f, 1280.0f / 720.0f, 0.1f, 200.0f };

	std::shared_ptr<Egss::Mesh> m_Sphere, m_Torus, m_Icosahedron, m_Ground;

	std::shared_ptr<Egss::Shader> m_Shader, m_OutlineShader;
	std::shared_ptr<Egss::Material> m_Scene, m_Object;
	std::shared_ptr<Egss::Material> m_OutlineScene, m_Outline;

	// --- Cel parameters ---
	int m_Bands = 4;
	bool m_Quantise = true;
	bool m_Outlines = true;
	float m_OutlinePixels = 4.0f;
	glm::vec4 m_OutlineColour{ 0.05f, 0.05f, 0.08f, 1.0f };

	float m_Ambient = 0.18f;
	float m_SpecularSize = 0.55f;
	float m_SpecularStrength = 0.7f;
	float m_RimWidth = 0.35f;
	float m_RimStrength = 0.0f;

	float m_LightAngle = 40.0f;
	float m_LightHeight = 0.85f;

	glm::vec4 m_Background{ 0.55f, 0.68f, 0.80f, 1.0f };

	// --- Scene ---
	float m_Spin = 0.0f;
	float m_SpinRate = 18.0f;
	float m_Orbit = 0.0f;
	float m_CameraHeight = 4.0f;
	float m_CameraPitch = 14.0f;
};

inline void CelShading::BuildShaders()
{
	std::string vertexSrc = R"(
		#version 330 core
		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;
		uniform mat4 u_NormalMatrix;

		out vec3 v_WorldPosition;
		out vec3 v_Normal;

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);
			v_WorldPosition = world.xyz;
			v_Normal = mat3(u_NormalMatrix) * a_Normal;
			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string fragmentSrc = R"(
		#version 330 core
		layout(location = 0) out vec4 color;

		in vec3 v_WorldPosition;
		in vec3 v_Normal;

		uniform vec4 u_BaseColour;
		uniform vec3 u_LightDirection;
		uniform vec3 u_CameraPosition;
		uniform float u_Ambient;

		uniform int u_Bands;
		uniform float u_Quantise;
		uniform float u_SpecularSize;
		uniform float u_SpecularStrength;
		uniform float u_RimWidth;
		uniform float u_RimStrength;

		void main()
		{
			vec3 n = normalize(v_Normal);
			vec3 l = normalize(-u_LightDirection);
			vec3 v = normalize(u_CameraPosition - v_WorldPosition);
			vec3 h = normalize(l + v);

			float d = max(dot(n, l), 0.0);

			// The whole of cel shading, in three lines.
			//
			// `floor(d * bands)` gives an integer level in 0..bands, since d
			// reaches exactly 1 at the point facing the light; clamping to
			// bands-1 folds that single brightest point into the top band and
			// leaves exactly `bands` distinct levels.
			//
			// Then divide by **bands - 1**, not by bands. Dividing by bands is
			// the obvious thing and it is wrong: the top level would come out
			// at (bands-1)/bands, so a 4-band model would never be brighter
			// than 0.75 and the whole image sits under a haze. Dividing by
			// bands-1 makes the levels span 0..1 inclusive.
			float steps = float(max(u_Bands, 2));
			float level = min(floor(d * steps), steps - 1.0);
			float quantised = level / (steps - 1.0);

			// u_Quantise is 0 or 1 rather than a branch, so the panel's
			// toggle shows the smooth original and the banded version through
			// the identical code path -- the comparison is honest that way.
			float shade = mix(d, quantised, u_Quantise);

			// The highlight gets the same treatment: a hard-edged blob rather
			// than a falloff. step() *is* the cel version of a specular term.
			float specular = 0.0;
			if (u_SpecularStrength > 0.0)
			{
				float blinn = pow(max(dot(n, h), 0.0), 32.0);
				specular = step(1.0 - u_SpecularSize, blinn) * u_SpecularStrength;
			}

			// Rim light: bright where the surface turns away from the viewer.
			// Also stepped, and gated on the surface being lit at all, so an
			// object does not get a halo on its dark side.
			float rim = 0.0;
			if (u_RimStrength > 0.0)
				rim = step(1.0 - u_RimWidth, 1.0 - max(dot(n, v), 0.0))
					* u_RimStrength * step(0.1, d);

			vec3 lit = u_BaseColour.rgb * (u_Ambient + shade * (1.0 - u_Ambient));
			color = vec4(lit + vec3(specular) + vec3(rim), u_BaseColour.a);
		}
	)";

	m_Shader.reset(Egss::Shader::Create("Cel", vertexSrc, fragmentSrc));
	m_Scene = Egss::Material::Create(m_Shader);
	m_Object = Egss::Material::CreateInstance(m_Scene);

	// --- The outline hull ---------------------------------------------------
	//
	// The push happens in **clip space**, not in world space, and that is the
	// difference between an outline that stays the same weight and one that
	// thins out as the object recedes. Moving the vertex by `k` in NDC moves it
	// by `k * width/2` pixels; multiplying by clip.w first cancels the
	// perspective divide that is about to happen, so the offset survives it
	// unchanged. Hence a thickness given directly in **pixels**:
	//
	//     offset_ndc = pixels * 2 / viewport
	//
	// which is a claim in units that can be checked against a screenshot, and
	// was: a 4 px setting measures 4 px of rim on a captured frame.
	//
	// A world-space push -- `position + normal * w` -- is the version usually
	// written first. It is not wrong, it just means something else: a constant
	// size in metres, which halves on screen every time the distance doubles.
	std::string outlineVertexSrc = R"(
		#version 330 core
		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;
		uniform mat4 u_NormalMatrix;
		uniform vec3 u_Viewport;
		uniform float u_OutlinePixels;

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);
			vec4 clip = u_ViewProjection * world;

			// The normal as a direction (w = 0), carried into clip space so it
			// can be flattened to the screen plane.
			vec3 worldNormal = mat3(u_NormalMatrix) * a_Normal;
			vec4 clipNormal = u_ViewProjection * vec4(worldNormal, 0.0);

			vec2 screenDirection = clipNormal.xy;
			float length2 = dot(screenDirection, screenDirection);

			// A normal pointing straight at or away from the camera projects to
			// nothing, and normalising it would be a divide by zero. Those
			// vertices are in the middle of the silhouette rather than on it,
			// so leaving them unmoved costs nothing.
			if (length2 > 1e-12)
			{
				screenDirection *= inversesqrt(length2);
				clip.xy += screenDirection * (u_OutlinePixels * 2.0 / u_Viewport.xy) * clip.w;
			}

			gl_Position = clip;
		}
	)";

	std::string outlineFragmentSrc = R"(
		#version 330 core
		layout(location = 0) out vec4 color;
		uniform vec4 u_OutlineColour;
		void main() { color = u_OutlineColour; }
	)";

	m_OutlineShader.reset(Egss::Shader::Create("CelOutline", outlineVertexSrc, outlineFragmentSrc));
	m_OutlineScene = Egss::Material::Create(m_OutlineShader);
	m_Outline = Egss::Material::CreateInstance(m_OutlineScene);
}

inline void CelShading::OnDemoImGui()
{
	ImGui::Begin("Cel shading");

	ImGui::TextWrapped("Quantised lighting plus an inverted-hull outline.");
	ImGui::Separator();

	ImGui::Checkbox("Quantise", &m_Quantise);
	ImGui::SameLine();
	ImGui::TextDisabled("(off = the same shader, unbanded)");

	ImGui::SliderInt("Bands", &m_Bands, 2, 8);
	ImGui::SliderFloat("Ambient", &m_Ambient, 0.0f, 0.6f);

	ImGui::Separator();
	ImGui::Checkbox("Outlines", &m_Outlines);
	ImGui::SliderFloat("Outline (pixels)", &m_OutlinePixels, 0.0f, 12.0f, "%.1f px");
	ImGui::ColorEdit3("Outline colour", &m_OutlineColour.x);
	ImGui::TextDisabled("Width is in pixels and stays put as the camera moves.");

	ImGui::Separator();
	ImGui::SliderFloat("Specular size", &m_SpecularSize, 0.0f, 1.0f);
	ImGui::SliderFloat("Specular strength", &m_SpecularStrength, 0.0f, 1.0f);
	ImGui::SliderFloat("Rim width", &m_RimWidth, 0.0f, 1.0f);
	ImGui::SliderFloat("Rim strength", &m_RimStrength, 0.0f, 1.0f);

	ImGui::Separator();
	ImGui::SliderFloat("Light angle", &m_LightAngle, 0.0f, 360.0f);
	ImGui::SliderFloat("Light height", &m_LightHeight, 0.05f, 2.0f);
	ImGui::SliderFloat("Orbit", &m_Orbit, -180.0f, 180.0f);
	ImGui::SliderFloat("Spin rate", &m_SpinRate, 0.0f, 90.0f);

	ImGui::Separator();
	ImGui::TextWrapped(
		"The icosahedron's outline breaks at its corners. That is the "
		"mechanism being honest: an inflated hull needs one shared normal per "
		"vertex, and a flat-shaded mesh has a separate vertex per face, so the "
		"inflated faces come apart at the seams.");

	ImGui::End();
}
