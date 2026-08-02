#pragma once

// A lit, textured cube -- the smallest thing that exercises the whole 3D path.
//
// Worth noticing how little of this is new. The vertex buffer, index buffer,
// buffer layout, shader, and texture are the same types the 2D renderer uses;
// only the camera and the shader differ. What is *not* used is Renderer2D:
// its batching is quad-specific, so meshes go through Renderer::Submit
// instead, one draw call each.
//
// Things marked TRY: are deliberate places to experiment.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Demo.h"

class Cube3D : public DemoLayer
{
public:
	Cube3D()
		: DemoLayer("Cube3D"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f)
	{
	}

	void OnDemoAttach() override
	{
		m_Camera.SetPosition({ 0.0f, 1.6f, 6.0f });
		m_Camera.SetRotation(-90.0f, -12.0f);

		BuildCube();
		BuildShader();
		BuildTexture();
		BuildAudio();
	}

	// -- attach runs for every demo whichever one is showing, which is how
	// these hums ended up playing under the other demos.
	void OnDemoActivated() override { StartEmitters(); }

	void OnDemoDeactivated() override
	{
		for (Egss::VoiceHandle& emitter : m_Emitters)
		{
			Egss::AudioEngine::Stop(emitter);
			emitter = Egss::InvalidVoice;
		}
	}

	void BuildAudio()
	{
		m_HumClip = MakeHum(196.0f, 2.0f);
		m_ChimeClip = MakeHum(587.0f, 2.0f);

		m_EmitterPositions[0] = { -1.6f, 0.0f, 0.0f };
		m_EmitterPositions[1] = { 1.6f, 0.0f, -1.6f };

		// Deliberately not started here. OnAttach runs for every pushed layer
		// whichever demo is selected, so starting a loop here plays it forever
		// underneath whatever else you switch to. OnUpdate starts them once
		// this demo is actually running.
	}

	// A looping clip has to be seamless or the loop point clicks. Using a
	// whole number of cycles in the buffer means the end lines up with the
	// start exactly.
	static std::shared_ptr<Egss::AudioClip> MakeHum(float frequency, float seconds)
	{
		const unsigned int rate = Egss::AudioEngine::GetSampleRate();
		unsigned int frames = (unsigned int)(seconds * rate);

		float cycles = std::round(frequency * seconds);
		float exactFrequency = cycles / seconds;
		frames = (unsigned int)std::round(cycles * rate / exactFrequency);

		std::vector<float> samples(frames);
		for (unsigned int i = 0; i < frames; i++)
		{
			float t = (float)i / (float)rate;
			// A little second harmonic, so it is not a pure sine.
			samples[i] = (std::sin(glm::two_pi<float>() * exactFrequency * t) * 0.6f
				+ std::sin(glm::two_pi<float>() * exactFrequency * 2.0f * t) * 0.2f) * 0.5f;
		}

		return Egss::AudioClip::CreateFromSamples(std::move(samples), 1);
	}

	void StartEmitters()
	{
		for (int i = 0; i < 2; i++)
		{
			Egss::AudioEngine::Stop(m_Emitters[i]);

			Egss::Audio3DParams params;
			params.Position = m_EmitterPositions[i];
			params.Volume = 0.5f;
			params.Loop = true;
			params.MinDistance = m_EmitterMinDistance;
			params.MaxDistance = m_EmitterMaxDistance;
			params.DopplerFactor = m_DopplerFactor;

			m_Emitters[i] = Egss::AudioEngine::PlayAt(
				i == 0 ? m_HumClip : m_ChimeClip, params);
		}
	}

	// ---------------------------------------------------------------------
	// Geometry
	//
	// 24 vertices for 6 faces rather than 8 shared corners: a cube corner has
	// three different normals and three different texture coordinates
	// depending on which face you are on, and a vertex can only carry one of
	// each. Sharing is possible only where every attribute matches.
	// ---------------------------------------------------------------------
	void BuildCube()
	{
		struct Vertex
		{
			glm::vec3 Position;
			glm::vec3 Normal;
			glm::vec2 TexCoord;
		};

		const glm::vec3 faceNormals[6] = {
			{  0,  0,  1 }, {  0,  0, -1 },
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 }
		};

		// Each face as four corners, wound counter-clockwise seen from
		// outside. Consistent winding is what makes back-face culling safe to
		// switch on later.
		const glm::vec3 faceCorners[6][4] = {
			{ {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f} }, // +Z
			{ { 0.5f,-0.5f,-0.5f}, {-0.5f,-0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f} }, // -Z
			{ { 0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f}, { 0.5f, 0.5f, 0.5f} }, // +X
			{ {-0.5f,-0.5f,-0.5f}, {-0.5f,-0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f,-0.5f} }, // -X
			{ {-0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f} }, // +Y
			{ {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f, 0.5f}, {-0.5f,-0.5f, 0.5f} }  // -Y
		};

		const glm::vec2 uvs[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

		std::vector<Vertex> vertices;
		std::vector<unsigned int> indices;
		vertices.reserve(24);
		indices.reserve(36);

		for (int face = 0; face < 6; face++)
		{
			unsigned int base = (unsigned int)vertices.size();

			for (int corner = 0; corner < 4; corner++)
				vertices.push_back({ faceCorners[face][corner], faceNormals[face], uvs[corner] });

			// Two triangles per quad, same pattern Renderer2D uses.
			indices.insert(indices.end(), {
				base + 0, base + 1, base + 2,
				base + 2, base + 3, base + 0
			});
		}

		m_VertexArray.reset(Egss::VertexArray::Create());

		std::shared_ptr<Egss::VertexBuffer> vb;
		vb.reset(Egss::VertexBuffer::Create((float*)vertices.data(),
			(unsigned int)(vertices.size() * sizeof(Vertex))));

		// The layout is the only thing telling GL how to read those bytes.
		// Order and types must match the Vertex struct exactly.
		vb->SetLayout({
			{ Egss::ShaderDataType::Float3, "a_Position" },
			{ Egss::ShaderDataType::Float3, "a_Normal"   },
			{ Egss::ShaderDataType::Float2, "a_TexCoord" }
		});
		m_VertexArray->AddVertexBuffer(vb);

		std::shared_ptr<Egss::IndexBuffer> ib;
		ib.reset(Egss::IndexBuffer::Create(indices.data(), (unsigned int)indices.size()));
		m_VertexArray->SetIndexBuffer(ib);
	}

	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			// Both are set for you by Renderer::Submit.
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;
			out vec2 v_TexCoord;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				v_WorldPosition = world.xyz;
				// mat3() drops the translation, which a direction must not
				// have. This is only correct while the scale is uniform; with
				// non-uniform scale you need the inverse-transpose instead.
				v_Normal        = mat3(u_Transform) * a_Normal;
				v_TexCoord      = a_TexCoord;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_WorldPosition;
			in vec3 v_Normal;
			in vec2 v_TexCoord;

			uniform sampler2D u_Texture;
			uniform vec4 u_Color;

			// A point light, not a directional one. A directional light has no
			// position, so there would be nothing for a gizmo to drag.
			uniform vec3 u_LightPosition;
			uniform vec3 u_LightColor;
			uniform float u_LightRange;
			uniform vec3 u_CameraPosition;
			uniform float u_AmbientStrength;

			void main()
			{
				vec3 normal  = normalize(v_Normal);

				vec3 lightVector = u_LightPosition - v_WorldPosition;
				float lightDistance = length(lightVector);
				vec3 toLight = lightVector / max(lightDistance, 0.0001);

				// Inverse-square, which is how light actually falls off, times
				// a linear fade so it reaches zero at the stated range instead
				// of trailing off forever.
				float attenuation = 1.0 / (1.0 + 0.08 * lightDistance * lightDistance);
				attenuation *= clamp(1.0 - lightDistance / u_LightRange, 0.0, 1.0);

				// Lambert: how square-on the surface is to the light. Faces
				// turned away give a negative dot, hence the clamp.
				float diffuse = max(dot(normal, toLight), 0.0);

				// Blinn-Phong specular, off the half-vector between the light
				// and the eye. Cheaper than reflecting, and better behaved at
				// grazing angles.
				vec3 toEye    = normalize(u_CameraPosition - v_WorldPosition);
				vec3 halfway  = normalize(toLight + toEye);
				float specular = pow(max(dot(normal, halfway), 0.0), 48.0);

				vec3 base    = texture(u_Texture, v_TexCoord).rgb * u_Color.rgb;
				vec3 lit     = base * u_AmbientStrength
				             + base * diffuse * u_LightColor * attenuation
				             + specular * u_LightColor * attenuation * 0.35;

				color = vec4(lit, 1.0);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("Cube3D", vertexSrc, fragmentSrc));
	}

	// A checkerboard, so the cube's faces and their orientation are readable
	// without any asset files. Texture2D::Create("path.png") for real art.
	void BuildTexture()
	{
		constexpr unsigned int size = 16;
		std::vector<unsigned int> pixels(size * size);

		for (unsigned int y = 0; y < size; y++)
		{
			for (unsigned int x = 0; x < size; x++)
			{
				bool light = ((x / 2) + (y / 2)) % 2 == 0;
				unsigned int shade = light ? 0xffe8e8e8 : 0xff6a6a72;
				pixels[y * size + x] = shade;
			}
		}

		m_Texture.reset(Egss::Texture2D::Create(size, size));
		m_Texture->SetData(pixels.data(), (unsigned int)(pixels.size() * sizeof(unsigned int)));
	}

	// Presentation only -- no OnFixedUpdate. Nothing here is simulated: the
	// camera is feel, and the spin is decoration, so both want the real frame
	// time rather than a fixed step. Not everything needs the simulation loop.

	// Presentation only -- no OnFixedUpdate. Nothing here is simulated: the
	// camera is feel, and the spin is decoration, so both want the real frame
	// time rather than a fixed step. Not everything needs the simulation loop.
	void OnDemoUpdate(Egss::Timestep ts) override
	{

		m_FrameTime = ts.GetMilliseconds();

		glm::vec3 previousCameraPosition = m_Camera.GetPosition();
		MoveCamera(ts);

		// The listener rides the camera. PerspectiveCamera already hands back
		// position, forward and up in exactly the form a listener wants.
		Egss::AudioListener listener;
		listener.Position = m_Camera.GetPosition();
		listener.Forward = m_Camera.GetForward();
		listener.Up = m_Camera.GetUp();
		// Velocity is only needed for Doppler, and is just how far the camera
		// moved this frame over how long the frame took.
		listener.Velocity = ts > 0.0f
			? (m_Camera.GetPosition() - previousCameraPosition) / (float)ts
			: glm::vec3(0.0f);

		Egss::AudioEngine::SetListener(listener);

		if (m_Spinning)
			m_Rotation += ts * 35.0f;

		UpdateGizmo();

		Egss::RenderCommand::SetClearColor({ 0.06f, 0.07f, 0.09f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::BeginScene(m_Camera);

		// Uniforms that aren't per-object are set once, with the shader bound.
		// Submit binds it again and adds u_ViewProjection and u_Transform, but
		// uniform values belong to the program and survive a rebind.
		m_Shader->Bind();
		m_Shader->SetFloat3("u_LightPosition", m_LightPosition);
		m_Shader->SetFloat3("u_LightColor", m_LightColor);
		m_Shader->SetFloat("u_LightRange", m_LightRange);
		m_Shader->SetFloat3("u_CameraPosition", m_Camera.GetPosition());
		m_Shader->SetFloat("u_AmbientStrength", m_Ambient);
		m_Shader->SetInt("u_Texture", 0);

		m_Texture->Bind(0);

		// One cube at the origin by default. The grid is still available, but
		// a single object is what the gizmo is for.
		for (int y = 0; y < m_GridSize; y++)
		{
			for (int x = 0; x < m_GridSize; x++)
			{
				glm::vec3 offset = {
					(x - (m_GridSize - 1) * 0.5f) * 1.6f,
					(y - (m_GridSize - 1) * 0.5f) * 1.6f,
					0.0f
				};

				// Transforms compose right-to-left: scale, then rotate, then
				// translate. Swapping any two changes the result.
				glm::mat4 transform =
					glm::translate(glm::mat4(1.0f), m_CubePosition + offset) *
					glm::rotate(glm::mat4(1.0f), glm::radians(m_CubeRotation.x), glm::vec3(1, 0, 0)) *
					glm::rotate(glm::mat4(1.0f), glm::radians(m_CubeRotation.y), glm::vec3(0, 1, 0)) *
					glm::rotate(glm::mat4(1.0f), glm::radians(m_CubeRotation.z + m_Rotation), glm::vec3(0, 0, 1)) *
					glm::scale(glm::mat4(1.0f), m_CubeScale);

				m_Shader->SetFloat4("u_Color", m_Tint);
				Egss::Renderer::Submit(m_Shader, m_VertexArray, transform);
			}
		}

		Egss::Renderer::EndScene();

		// Debug lines under a perspective camera. Renderer2D::BeginScene takes
		// any Camera, so the line batch works here exactly as it does in 2D --
		// the "2D" in the name is about the primitives, not the projection.
		if (m_ShowGrid || m_ShowEmitters)
		{
			Egss::Renderer2D::BeginScene(m_Camera);

			if (m_ShowGrid)
				DrawGrid();

			if (m_ShowEmitters)
				DrawEmitters();

			if (m_ShowGizmo)
				DrawGizmo();

			// A marker where the light is, so it can be seen and grabbed even
			// when it sits outside the lit geometry.
			DrawLightMarker();

			Egss::Renderer2D::EndScene();
		}
	}

	// A box round each emitter, brightened by how loud it currently is, so
	// what you hear and what you see agree.
	// A small three-axis star at the light's position.
	void DrawLightMarker()
	{
		glm::vec4 color(m_LightColor.x, m_LightColor.y, m_LightColor.z, 1.0f);
		const float size = 0.18f;

		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 offset(0.0f);
			offset[axis] = size;
			Egss::Renderer2D::DrawLine(m_LightPosition - offset, m_LightPosition + offset, color);
		}
	}

	void DrawEmitters()
	{
		for (int i = 0; i < 2; i++)
		{
			Egss::VoiceDebug debug;
			bool live = Egss::AudioEngine::GetVoiceDebug(m_Emitters[i], debug);

			float brightness = live ? 0.25f + debug.Gain * 0.75f : 0.15f;
			glm::vec4 color = i == 0
				? glm::vec4(0.35f, 0.85f, 1.0f, 1.0f) * brightness
				: glm::vec4(1.0f, 0.75f, 0.35f, 1.0f) * brightness;
			color.a = 1.0f;

			const glm::vec3& p = m_EmitterPositions[i];
			const float r = 0.22f;

			// A wireframe cube, drawn from the lines we already have.
			glm::vec3 corners[8] = {
				{ p.x - r, p.y - r, p.z - r }, { p.x + r, p.y - r, p.z - r },
				{ p.x + r, p.y + r, p.z - r }, { p.x - r, p.y + r, p.z - r },
				{ p.x - r, p.y - r, p.z + r }, { p.x + r, p.y - r, p.z + r },
				{ p.x + r, p.y + r, p.z + r }, { p.x - r, p.y + r, p.z + r }
			};

			for (int e = 0; e < 4; e++)
			{
				Egss::Renderer2D::DrawLine(corners[e], corners[(e + 1) % 4], color);
				Egss::Renderer2D::DrawLine(corners[4 + e], corners[4 + (e + 1) % 4], color);
				Egss::Renderer2D::DrawLine(corners[e], corners[4 + e], color);
			}
		}
	}

	// A ground plane and the world axes. Both are the sort of thing you want
	// permanently available once a scene stops being a single object at the
	// origin.
	void DrawGrid()
	{
		const int half = 6;
		const float y = -0.9f;
		const glm::vec4 gridColor = { 0.25f, 0.27f, 0.32f, 1.0f };

		for (int i = -half; i <= half; i++)
		{
			Egss::Renderer2D::DrawLine({ (float)i, y, (float)-half }, { (float)i, y, (float)half }, gridColor);
			Egss::Renderer2D::DrawLine({ (float)-half, y, (float)i }, { (float)half, y, (float)i }, gridColor);
		}

		// X red, Y green, Z blue -- the usual convention, and the fastest way
		// to work out which way a scene is facing.
		Egss::Renderer2D::DrawLine({ 0, y, 0 }, { 2.0f, y, 0 }, { 0.9f, 0.25f, 0.25f, 1.0f });
		Egss::Renderer2D::DrawLine({ 0, y, 0 }, { 0, y + 2.0f, 0 }, { 0.3f, 0.85f, 0.3f, 1.0f });
		Egss::Renderer2D::DrawLine({ 0, y, 0 }, { 0, y, 2.0f }, { 0.35f, 0.5f, 0.95f, 1.0f });
	}


	// ---------------------------------------------------------------------
	// The gizmo
	//
	// Three axis handles at the selected object's origin -- X red, Y green,
	// Z blue, the convention every 3D tool uses. Dragging one slides the
	// object along that axis only.
	//
	// The whole thing rests on two bits of maths:
	//
	//   * a **ray through the cursor**, because a mouse position is a line in
	//     3D, not a point;
	//   * the **closest point between that ray and the axis line**, which is
	//     what turns "where the mouse is" into "how far along X".
	//
	// Grabbing stores where on the axis you took hold, so the object does not
	// jump to the cursor -- it keeps the offset, exactly like a real tool.
	// ---------------------------------------------------------------------

	// A mouse position is a line through the scene. This returns its origin
	// and direction in world space.
	void ScreenRay(const glm::vec2& mouse, glm::vec3& outOrigin, glm::vec3& outDirection) const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();
		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();

		// Pixels -> clip space, flipping y because window coordinates count
		// downwards and clip space counts up.
		float x = (mouse.x / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.y / height) * 2.0f;

		glm::mat4 inverse = glm::inverse(m_Camera.GetViewProjectionMatrix());

		// The near and far plane points that project to this pixel. The
		// perspective divide is what makes them different.
		glm::vec4 nearPoint = inverse * glm::vec4(x, y, -1.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);

		nearPoint /= nearPoint.w;
		farPoint /= farPoint.w;

		outOrigin = glm::vec3(nearPoint);
		outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
	}

	// World position -> pixels. Returns false behind the camera, where the
	// projection would happily produce a plausible-looking wrong answer.
	bool WorldToScreen(const glm::vec3& world, glm::vec2& outScreen) const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();

		glm::vec4 clip = m_Camera.GetViewProjectionMatrix() * glm::vec4(world, 1.0f);
		if (clip.w <= 0.0001f)
			return false;

		glm::vec3 ndc = glm::vec3(clip) / clip.w;

		outScreen = {
			(ndc.x * 0.5f + 0.5f) * (float)window.GetWidth(),
			(1.0f - (ndc.y * 0.5f + 0.5f)) * (float)window.GetHeight()
		};
		return true;
	}

	// How far along `axis` the point nearest the cursor ray sits. This is the
	// heart of axis dragging: it collapses a 3D pick down to one number.
	static bool ClosestPointOnAxis(const glm::vec3& axisOrigin, const glm::vec3& axisDirection,
		const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float& outT)
	{
		glm::vec3 between = axisOrigin - rayOrigin;

		float a = glm::dot(axisDirection, axisDirection);
		float b = glm::dot(axisDirection, rayDirection);
		float c = glm::dot(rayDirection, rayDirection);
		float d = glm::dot(axisDirection, between);
		float e = glm::dot(rayDirection, between);

		float denominator = a * c - b * b;

		// Looking straight down the axis: every point on it projects to the
		// same pixel, so there is no meaningful answer.
		if (std::abs(denominator) < 0.00001f)
			return false;

		outT = (b * e - c * d) / denominator;
		return true;
	}

	glm::vec3& SelectedPosition()
	{
		return m_Selected == 0 ? m_CubePosition : m_LightPosition;
	}

	// Distance in pixels from the cursor to an axis handle, for picking.
	float AxisScreenDistance(int axis, const glm::vec2& mouse) const
	{
		glm::vec3 origin = m_Selected == 0 ? m_CubePosition : m_LightPosition;
		glm::vec3 direction(0.0f);
		direction[axis] = 1.0f;

		glm::vec2 a, b;
		if (!WorldToScreen(origin, a) || !WorldToScreen(origin + direction * m_GizmoLength, b))
			return std::numeric_limits<float>::max();

        // Distance from the cursor to the line segment a-b.
		glm::vec2 segment = b - a;
		float lengthSquared = glm::dot(segment, segment);
		if (lengthSquared < 0.0001f)
			return glm::length(mouse - a);

		float t = glm::clamp(glm::dot(mouse - a, segment) / lengthSquared, 0.0f, 1.0f);
		return glm::length(mouse - (a + segment * t));
	}

	void UpdateGizmo()
	{
		glm::vec2 mouse = { Egss::Input::GetMousePosition().first,
							Egss::Input::GetMousePosition().second };

		bool pressed = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT)
			&& !ImGui::GetIO().WantCaptureMouse;

		if (!pressed)
		{
			m_DragAxis = -1;
			m_HoverAxis = -1;

			// Highlight whichever handle the cursor is over, so it is obvious
			// what a click would grab.
			for (int axis = 0; axis < 3; axis++)
			{
				if (AxisScreenDistance(axis, mouse) < m_GizmoPickPixels)
				{
					m_HoverAxis = axis;
					break;
				}
			}
			return;
		}

		glm::vec3 rayOrigin, rayDirection;
		ScreenRay(mouse, rayOrigin, rayDirection);

		// --- Grab ---
		if (m_DragAxis < 0)
		{
			if (m_HoverAxis < 0)
				return;

			glm::vec3 axisDirection(0.0f);
			axisDirection[m_HoverAxis] = 1.0f;

			float t;
			if (!ClosestPointOnAxis(SelectedPosition(), axisDirection, rayOrigin, rayDirection, t))
				return;

			m_DragAxis = m_HoverAxis;
			m_DragStartT = t;
			m_DragStartPosition = SelectedPosition();
			return;
		}

		// --- Drag ---
		glm::vec3 axisDirection(0.0f);
		axisDirection[m_DragAxis] = 1.0f;

		float t;
		if (!ClosestPointOnAxis(m_DragStartPosition, axisDirection, rayOrigin, rayDirection, t))
			return;

		// Relative to where it was grabbed, so the object does not snap its
		// origin to the cursor.
		SelectedPosition() = m_DragStartPosition + axisDirection * (t - m_DragStartT);
	}

	void DrawGizmo()
	{
		glm::vec3 origin = m_Selected == 0 ? m_CubePosition : m_LightPosition;

		const glm::vec4 axisColors[3] = {
			{ 1.0f, 0.25f, 0.25f, 1.0f },   // X red
			{ 0.30f, 1.0f, 0.35f, 1.0f },   // Y green
			{ 0.35f, 0.55f, 1.0f, 1.0f }    // Z blue
		};

		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 direction(0.0f);
			direction[axis] = 1.0f;

			glm::vec4 color = axisColors[axis];
			if (axis == m_DragAxis || (m_DragAxis < 0 && axis == m_HoverAxis))
				color = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);   // highlighted

			glm::vec3 tip = origin + direction * m_GizmoLength;
			Egss::Renderer2D::DrawLine(origin, tip, color);

			// A little cross at the tip, so the end of the handle is visible
			// even when the line is nearly edge-on to the camera.
			glm::vec3 a(0.0f), b(0.0f);
			a[(axis + 1) % 3] = 0.06f;
			b[(axis + 2) % 3] = 0.06f;

			Egss::Renderer2D::DrawLine(tip - a, tip + a, color);
			Egss::Renderer2D::DrawLine(tip - b, tip + b, color);
		}
	}

	// Fly camera: WASD along the ground, Q/E straight up and down, arrows to
	// look. Keyboard-only on purpose -- mouse look would need cursor capture,
	// which the engine doesn't have yet.
	void MoveCamera(Egss::Timestep ts)
	{
		glm::vec3 position = m_Camera.GetPosition();
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		float move = m_MoveSpeed * ts;
		float look = m_LookSpeed * ts;

		// Forward and right come from the camera's own orientation, so "left"
		// always means left of where you are facing.
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) position += m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) position += m_Camera.GetRight() * move;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) position.y += move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) position.y -= move;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  yaw -= look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) yaw += look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    pitch += look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  pitch -= look;

		// --- Middle-drag to look ---
		// Mouse *delta*, not position: how far it moved since last frame, not
		// where it is. Kept every frame whether or not the button is held, or
		// the first frame of a drag would jump by however far the cursor had
		// travelled since it was last looked at.
		glm::vec2 mouse = { Egss::Input::GetMousePosition().first,
							Egss::Input::GetMousePosition().second };
		glm::vec2 delta = mouse - m_PreviousMouse;
		m_PreviousMouse = mouse;

		bool looking = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_MIDDLE)
			&& !ImGui::GetIO().WantCaptureMouse;

		if (looking)
		{
			// Degrees per pixel, so it feels the same at any frame rate --
			// deliberately *not* scaled by the timestep, because the mouse has
			// already moved a real distance.
			yaw += delta.x * m_MouseLookSensitivity;
			pitch -= delta.y * m_MouseLookSensitivity;
		}

		m_Camera.SetPosition(position);
		m_Camera.SetRotation(yaw, pitch);
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			// Only the projection cares about the window's shape. Get this
			// wrong in 3D and everything looks subtly stretched.
			if (e.GetHeight() > 0)
				m_Camera.SetAspectRatio((float)e.GetWidth() / (float)e.GetHeight());

			return false;
		});

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			// Switching demos is DemoSelector's job, not this layer's -- it
			// sits above this one and consumes F1 before it gets here.

			if (e.GetKeyCode() == EGSS_KEY_SPACE)
				m_Spinning = !m_Spinning;

			return false;
		});
	}

	void OnDemoImGui() override
	{

		// Clear of the Demos panel on first run; ImGui remembers it after.
		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Cube3D");

		ImGui::Text("WASD   move      Q/E  up / down");
		ImGui::Text("Arrows look      Middle-drag  mouse look");
		ImGui::Text("Space  pause spin");

		ImGui::SeparatorText("Transform");

		// Which object the gizmo is attached to.
		const char* targets[] = { "Cube", "Light" };
		ImGui::Combo("Gizmo target", &m_Selected, targets, 2);
		ImGui::Checkbox("Show gizmo", &m_ShowGizmo);
		ImGui::SameLine();
		ImGui::TextDisabled(m_DragAxis >= 0 ? "dragging %c" : "drag an axis",
			"XYZ"[m_DragAxis < 0 ? 0 : m_DragAxis]);

		if (m_Selected == 0)
		{
			ImGui::DragFloat3("Position", &m_CubePosition.x, 0.01f);
			ImGui::DragFloat3("Rotation", &m_CubeRotation.x, 0.5f);
			ImGui::DragFloat3("Scale", &m_CubeScale.x, 0.01f, 0.05f, 5.0f);

			if (ImGui::Button("Reset transform"))
			{
				m_CubePosition = glm::vec3(0.0f);
				m_CubeRotation = glm::vec3(0.0f);
				m_CubeScale = glm::vec3(1.0f);
			}
		}
		else
		{
			ImGui::DragFloat3("Position", &m_LightPosition.x, 0.01f);
			ImGui::SliderFloat("Range", &m_LightRange, 1.0f, 40.0f);
			ImGui::ColorEdit3("Colour", &m_LightColor.x);
		}

		ImGui::SliderFloat("Gizmo length", &m_GizmoLength, 0.3f, 3.0f);
		ImGui::SliderFloat("Look sensitivity", &m_MouseLookSensitivity, 0.02f, 0.6f);

		ImGui::Separator();
		glm::vec3 p = m_Camera.GetPosition();
		ImGui::Text("Camera  %.2f, %.2f, %.2f", p.x, p.y, p.z);
		ImGui::Text("Yaw %.0f  Pitch %.0f", m_Camera.GetYaw(), m_Camera.GetPitch());
		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Text("Draw calls: %d  (one per cube, plus one for the grid)",
			m_GridSize * m_GridSize + (m_ShowGrid ? 1 : 0));
		ImGui::Checkbox("Show grid", &m_ShowGrid);

		ImGui::Separator();
		ImGui::Text("Audio: %s   %u voices", Egss::AudioEngine::GetBackendName(),
			Egss::AudioEngine::GetActiveVoiceCount());
		ImGui::Checkbox("Show emitters", &m_ShowEmitters);

		for (int i = 0; i < 2; i++)
		{
			Egss::VoiceDebug debug;
			if (Egss::AudioEngine::GetVoiceDebug(m_Emitters[i], debug))
			{
				ImGui::Text("emitter %d: %.2fm  gain %.2f  pan %+.2f  doppler %.3f",
					i, debug.Distance, debug.Gain, debug.Pan, debug.PitchScale);
			}
			else
			{
				ImGui::TextDisabled("emitter %d: not playing", i);
			}
		}

		if (ImGui::SliderFloat("Emitter range", &m_EmitterMaxDistance, 1.0f, 40.0f))
			StartEmitters();
		if (ImGui::SliderFloat("Doppler", &m_DopplerFactor, 0.0f, 8.0f))
			StartEmitters();
		if (ImGui::Button("Restart emitters"))
			StartEmitters();

		ImGui::Separator();
		ImGui::SliderInt("Grid", &m_GridSize, 1, 8);
		ImGui::SliderFloat("Ambient", &m_Ambient, 0.0f, 1.0f);
		ImGui::ColorEdit3("Light colour", &m_LightColor.x);
		ImGui::ColorEdit4("Tint", &m_Tint.x);

		ImGui::End();
	}

private:
	Egss::PerspectiveCamera m_Camera;

	std::shared_ptr<Egss::VertexArray> m_VertexArray;
	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Texture2D> m_Texture;

	float m_MoveSpeed = 3.0f;
	float m_LookSpeed = 70.0f;

	float m_Rotation = 0.0f;
	bool m_Spinning = true;

	std::shared_ptr<Egss::AudioClip> m_HumClip;
	std::shared_ptr<Egss::AudioClip> m_ChimeClip;
	Egss::VoiceHandle m_Emitters[2] = { Egss::InvalidVoice, Egss::InvalidVoice };
	glm::vec3 m_EmitterPositions[2];
	float m_EmitterMinDistance = 1.0f;
	float m_EmitterMaxDistance = 12.0f;
	float m_DopplerFactor = 2.0f;
	bool m_ShowEmitters = true;

	bool m_ShowGrid = true;
	int m_GridSize = 1;

	// The light is an object in the scene now, so it has a position the gizmo
	// can drag.
	glm::vec3 m_LightPosition = { 1.6f, 1.6f, 1.8f };
	glm::vec3 m_LightColor = { 1.0f, 0.96f, 0.9f };
	float m_LightRange = 12.0f;
	float m_Ambient = 0.10f;

	// 0 = the cube, 1 = the light. Which one the gizmo is attached to.
	int m_Selected = 0;
	bool m_ShowGizmo = true;

	glm::vec2 m_PreviousMouse = { 0.0f, 0.0f };
	float m_MouseLookSensitivity = 0.18f;

	int m_HoverAxis = -1;
	int m_DragAxis = -1;
	float m_DragStartT = 0.0f;
	glm::vec3 m_DragStartPosition = { 0.0f, 0.0f, 0.0f };
	float m_GizmoLength = 1.0f;
	float m_GizmoPickPixels = 12.0f;

	// The cube's transform, which the gizmo and the panel both edit.
	glm::vec3 m_CubePosition = { 0.0f, 0.0f, 0.0f };
	glm::vec3 m_CubeRotation = { 0.0f, 0.0f, 0.0f };
	glm::vec3 m_CubeScale = { 1.0f, 1.0f, 1.0f };
	glm::vec4 m_Tint = { 1.0f, 1.0f, 1.0f, 1.0f };

	float m_FrameTime = 0.0f;
};
