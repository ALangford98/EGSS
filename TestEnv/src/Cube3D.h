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

class Cube3D : public Egss::Layer
{
public:
	Cube3D()
		: Layer("Cube3D"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f)
	{
	}

	void OnAttach() override
	{
		m_Camera.SetPosition({ 0.0f, 1.6f, 6.0f });
		m_Camera.SetRotation(-90.0f, -12.0f);

		BuildCube();
		BuildShader();
		BuildTexture();
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

			uniform vec3 u_LightDirection;   // direction the light travels
			uniform vec3 u_LightColor;
			uniform vec3 u_CameraPosition;
			uniform float u_AmbientStrength;

			void main()
			{
				vec3 normal  = normalize(v_Normal);
				vec3 toLight = normalize(-u_LightDirection);

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
				vec3 lit     = base * (u_AmbientStrength + diffuse) * u_LightColor
				             + specular * u_LightColor * 0.35;

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

	void OnUpdate(Egss::Timestep ts) override
	{
		if (g_ActiveDemo != Demo::Cube3D)
			return;

		m_FrameTime = ts.GetMilliseconds();

		MoveCamera(ts);

		if (m_Spinning)
			m_Rotation += ts * 35.0f;

		Egss::RenderCommand::SetClearColor({ 0.06f, 0.07f, 0.09f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::BeginScene(m_Camera);

		// Uniforms that aren't per-object are set once, with the shader bound.
		// Submit binds it again and adds u_ViewProjection and u_Transform, but
		// uniform values belong to the program and survive a rebind.
		m_Shader->Bind();
		m_Shader->SetFloat3("u_LightDirection", glm::normalize(m_LightDirection));
		m_Shader->SetFloat3("u_LightColor", m_LightColor);
		m_Shader->SetFloat3("u_CameraPosition", m_Camera.GetPosition());
		m_Shader->SetFloat("u_AmbientStrength", m_Ambient);
		m_Shader->SetInt("u_Texture", 0);

		m_Texture->Bind(0);

		// A 3x3x1 grid, to make it obvious these are separate draw calls --
		// unlike Renderer2D, nothing here is batched.
		for (int y = 0; y < m_GridSize; y++)
		{
			for (int x = 0; x < m_GridSize; x++)
			{
				glm::vec3 position = {
					(x - (m_GridSize - 1) * 0.5f) * 1.6f,
					(y - (m_GridSize - 1) * 0.5f) * 1.6f,
					0.0f
				};

				// Transforms compose right-to-left: scale, then rotate, then
				// translate. Swapping any two changes the result.
				glm::mat4 transform =
					glm::translate(glm::mat4(1.0f), position) *
					glm::rotate(glm::mat4(1.0f), glm::radians(m_Rotation), glm::vec3(0.4f, 1.0f, 0.2f)) *
					glm::scale(glm::mat4(1.0f), glm::vec3(m_Scale));

				m_Shader->SetFloat4("u_Color", m_Tint);

				// TRY: give each cube its own tint from x and y.
				Egss::Renderer::Submit(m_Shader, m_VertexArray, transform);
			}
		}

		Egss::Renderer::EndScene();
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

		m_Camera.SetPosition(position);
		m_Camera.SetRotation(yaw, pitch);
	}

	void OnEvent(Egss::Event& e) override
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
			if (g_ActiveDemo != Demo::Cube3D)
				return false;

			if (e.GetKeyCode() == EGSS_KEY_SPACE)
				m_Spinning = !m_Spinning;

			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (g_ActiveDemo != Demo::Cube3D)
			return;

		// Clear of the Demos panel on first run; ImGui remembers it after.
		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Cube3D");

		ImGui::Text("WASD   move      Q/E  up / down");
		ImGui::Text("Arrows look      Space  pause spin");

		ImGui::Separator();
		glm::vec3 p = m_Camera.GetPosition();
		ImGui::Text("Camera  %.2f, %.2f, %.2f", p.x, p.y, p.z);
		ImGui::Text("Yaw %.0f  Pitch %.0f", m_Camera.GetYaw(), m_Camera.GetPitch());
		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Text("Draw calls: %d  (one per cube)", m_GridSize * m_GridSize);

		ImGui::Separator();
		ImGui::SliderInt("Grid", &m_GridSize, 1, 8);
		ImGui::SliderFloat("Scale", &m_Scale, 0.2f, 1.5f);
		ImGui::SliderFloat("Ambient", &m_Ambient, 0.0f, 1.0f);
		ImGui::SliderFloat3("Light dir", &m_LightDirection.x, -1.0f, 1.0f);
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

	int m_GridSize = 3;
	float m_Scale = 0.8f;

	glm::vec3 m_LightDirection = { -0.4f, -0.8f, -0.45f };
	glm::vec3 m_LightColor = { 1.0f, 0.96f, 0.9f };
	float m_Ambient = 0.18f;
	glm::vec4 m_Tint = { 1.0f, 1.0f, 1.0f, 1.0f };

	float m_FrameTime = 0.0f;
};
