#include <Egss.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

class ExampleLayer : public Egss::Layer
{
public:
	ExampleLayer()
		: Layer("Example"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
		// ---- Flat-colour triangle -------------------------------------
		m_TriangleVA.reset(Egss::VertexArray::Create());

		float triangleVertices[3 * 7] = {
			-0.5f, -0.5f, 0.0f,  0.9f, 0.2f, 0.3f, 1.0f,
			 0.5f, -0.5f, 0.0f,  0.3f, 0.8f, 0.4f, 1.0f,
			 0.0f,  0.5f, 0.0f,  0.3f, 0.4f, 0.9f, 1.0f
		};

		std::shared_ptr<Egss::VertexBuffer> triangleVB;
		triangleVB.reset(Egss::VertexBuffer::Create(triangleVertices, sizeof(triangleVertices)));
		triangleVB->SetLayout({
			{ Egss::ShaderDataType::Float3, "a_Position" },
			{ Egss::ShaderDataType::Float4, "a_Color" }
		});
		m_TriangleVA->AddVertexBuffer(triangleVB);

		unsigned int triangleIndices[3] = { 0, 1, 2 };
		std::shared_ptr<Egss::IndexBuffer> triangleIB;
		triangleIB.reset(Egss::IndexBuffer::Create(triangleIndices, 3));
		m_TriangleVA->SetIndexBuffer(triangleIB);

		// ---- Textured quad --------------------------------------------
		m_QuadVA.reset(Egss::VertexArray::Create());

		// Position (3) + texture coordinate (2).
		float quadVertices[4 * 5] = {
			-0.75f, -0.75f, 0.0f,  0.0f, 0.0f,
			 0.75f, -0.75f, 0.0f,  1.0f, 0.0f,
			 0.75f,  0.75f, 0.0f,  1.0f, 1.0f,
			-0.75f,  0.75f, 0.0f,  0.0f, 1.0f
		};

		std::shared_ptr<Egss::VertexBuffer> quadVB;
		quadVB.reset(Egss::VertexBuffer::Create(quadVertices, sizeof(quadVertices)));
		quadVB->SetLayout({
			{ Egss::ShaderDataType::Float3, "a_Position" },
			{ Egss::ShaderDataType::Float2, "a_TexCoord" }
		});
		m_QuadVA->AddVertexBuffer(quadVB);

		// Two triangles sharing an edge -- the reason index buffers exist.
		unsigned int quadIndices[6] = { 0, 1, 2, 2, 3, 0 };
		std::shared_ptr<Egss::IndexBuffer> quadIB;
		quadIB.reset(Egss::IndexBuffer::Create(quadIndices, 6));
		m_QuadVA->SetIndexBuffer(quadIB);

		// ---- Shaders ---------------------------------------------------
		std::string colorVertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec4 a_Color;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			out vec4 v_Color;
			void main()
			{
				v_Color = a_Color;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string colorFragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			in vec4 v_Color;
			void main() { color = v_Color; }
		)";

		m_ColorShader.reset(Egss::Shader::Create("FlatColor", colorVertexSrc, colorFragmentSrc));

		std::string textureVertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec2 a_TexCoord;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			out vec2 v_TexCoord;
			void main()
			{
				v_TexCoord = a_TexCoord;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string textureFragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			in vec2 v_TexCoord;
			uniform sampler2D u_Texture;
			uniform vec4 u_Tint;
			void main() { color = texture(u_Texture, v_TexCoord) * u_Tint; }
		)";

		m_TextureShader.reset(Egss::Shader::Create("Texture", textureVertexSrc, textureFragmentSrc));

		// ---- Texture ---------------------------------------------------
		// Generated rather than loaded from disk, so the sandbox has no asset
		// dependency. Texture2D::Create(path) handles real image files.
		const unsigned int size = 8;
		unsigned int pixels[size * size];
		for (unsigned int y = 0; y < size; y++)
		{
			for (unsigned int x = 0; x < size; x++)
			{
				bool light = ((x + y) % 2) == 0;
				// RGBA, little-endian byte order.
				pixels[y * size + x] = light ? 0xffcccccc : 0xff4444aa;
			}
		}

		m_Texture.reset(Egss::Texture2D::Create(size, size));
		m_Texture->SetData(pixels, sizeof(pixels));

		m_TextureShader->Bind();
		m_TextureShader->SetInt("u_Texture", 0);
	}

	void OnUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		// Camera movement, scaled by delta time so it's framerate independent.
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))
			m_CameraPosition.x -= m_CameraMoveSpeed * ts;
		else if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT))
			m_CameraPosition.x += m_CameraMoveSpeed * ts;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))
			m_CameraPosition.y -= m_CameraMoveSpeed * ts;
		else if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))
			m_CameraPosition.y += m_CameraMoveSpeed * ts;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_A))
			m_CameraRotation += m_CameraRotationSpeed * ts;
		else if (Egss::Input::IsKeyPressed(EGSS_KEY_D))
			m_CameraRotation -= m_CameraRotationSpeed * ts;

		m_Camera.SetPosition(m_CameraPosition);
		m_Camera.SetRotation(m_CameraRotation);

		Egss::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::BeginScene(m_Camera);

		m_TextureShader->Bind();
		m_TextureShader->SetFloat4("u_Tint", m_Tint);
		m_Texture->Bind(0);
		Egss::Renderer::Submit(m_TextureShader, m_QuadVA);

		// Nudged forward so it sits in front of the quad.
		glm::mat4 triangleTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, 0.1f });
		Egss::Renderer::Submit(m_ColorShader, m_TriangleVA, triangleTransform);

		Egss::Renderer::EndScene();
	}

	void OnImGuiRender() override
	{
		ImGui::Begin("Debug");
		ImGui::Text("Frame time: %.2f ms (%.0f fps)", m_FrameTime, m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Separator();
		ImGui::Text("Camera");
		ImGui::DragFloat3("Position", &m_CameraPosition.x, 0.01f);
		ImGui::DragFloat("Rotation", &m_CameraRotation, 1.0f);
		ImGui::Separator();
		ImGui::ColorEdit4("Quad tint", &m_Tint.x);
		ImGui::End();
	}

	void OnEvent(Egss::Event& event) override
	{
		if (event.GetEventType() == Egss::EventType::KeyPressed)
		{
			auto& e = (Egss::KeyPressedEvent&)event;
			EGSS_TRACE("{0}", e.ToString());
		}
	}
private:
	std::shared_ptr<Egss::VertexArray> m_TriangleVA, m_QuadVA;
	std::shared_ptr<Egss::Shader> m_ColorShader, m_TextureShader;
	std::shared_ptr<Egss::Texture2D> m_Texture;

	Egss::OrthographicCamera m_Camera;
	glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
	float m_CameraRotation = 0.0f;

	float m_CameraMoveSpeed = 2.0f;
	float m_CameraRotationSpeed = 90.0f;

	glm::vec4 m_Tint = { 1.0f, 1.0f, 1.0f, 1.0f };
	float m_FrameTime = 0.0f;
};

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{
		PushLayer(new ExampleLayer());
	}
	~TestEnv()
	{

	}
};



Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}
