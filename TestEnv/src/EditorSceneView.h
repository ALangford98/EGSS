#pragma once

// The editor's own view of g_EditorScene: a free-fly camera, framebuffer
// picking, a translate gizmo, an outliner and an inspector.
//
// Every piece of this already existed, proven, in Cube3D.h -- this is that
// code with m_Scene replaced by the shared g_EditorScene and the
// audio/acoustics it was tangled up with left behind. Nothing here is a new
// idea; it is Cube3D's editing half, promoted out of one demo into the shell.
//
// Active exactly when no demo is: g_ActiveDemo == InvalidDemo. A demo forced
// active by --demo owns the whole window instead, unchanged from before this
// existed.

#include <GS.h>
#include <imgui.h>

#include "Demo.h"
#include "EditorProject.h"

// Forward-declared so it can be set from OnAttach() below: a member function
// body is a complete-class context for the class's *own* members, but that
// does not reach forward to a namespace-scope global declared later in the
// file -- so the global has to come first, as EditorShell.h's g_EditorShell
// does.
class EditorSceneView;
inline EditorSceneView* g_EditorSceneView = nullptr;

class EditorSceneView : public GS::Layer
{
public:
	EditorSceneView()
		: Layer("EditorSceneView"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 1000.0f)
	{
	}

	void OnAttach() override
	{
		g_EditorSceneView = this;

		m_Camera.SetPosition({ 0.0f, 1.6f, 6.0f });
		m_Camera.SetRotation(-90.0f, -12.0f);

		BuildTarget();
		BuildShader();
	}

	bool IsActive() const { return g_ActiveDemo == InvalidDemo; }

	void Select(GS::EntityId entity) { m_Selected = entity; }
	GS::EntityId GetSelected() const { return m_Selected; }

	void OnUpdate(GS::Timestep ts) override
	{
		if (!IsActive())
			return;

		if (g_Viewport.Valid())
			GS::RenderCommand::SetViewport((unsigned int)g_Viewport.X,
				(unsigned int)g_Viewport.Y, (unsigned int)g_Viewport.Width,
				(unsigned int)g_Viewport.Height);

		MoveCamera(ts);
		ResizeTarget();
		UpdateGizmo();

		m_Framebuffer->Bind();

		GS::RenderCommand::SetClearColor({ 0.10f, 0.11f, 0.13f, 1.0f });
		GS::RenderCommand::Clear();
		m_Framebuffer->ClearAttachment(1, -1);

		GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		RenderMeshes();
		GS::RenderCommand::SetCullFace(GS::CullFace::None);

		GS::Renderer2D::BeginScene(m_Camera);
		DrawSelectionBox();
		if (m_ShowGizmo)
			DrawGizmo();
		GS::Renderer2D::EndScene();

		ReadHoveredEntity();

		m_Framebuffer->Unbind();

		BlitToWindow();

		if (g_Viewport.Valid())
		{
			GS::Window& window = GS::Application::Get().GetWindow();
			GS::RenderCommand::SetViewport(0, 0, window.GetWidth(), window.GetHeight());
		}
	}

	void OnEvent(GS::Event& e) override
	{
		if (!IsActive())
			return;

		GS::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<GS::WindowResizeEvent>([this](GS::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
				m_Camera.SetAspectRatio((float)e.GetWidth() / (float)e.GetHeight());
			return false;
		});

		dispatcher.Dispatch<GS::MouseButtonPressedEvent>([this](GS::MouseButtonPressedEvent& e)
		{
			// A click on a hovered gizmo handle must grab it, not change the
			// selection -- checking m_HoverAxis rather than m_DragAxis is what
			// distinguishes the two, since m_DragAxis is not set until
			// UpdateGizmo sees the button on the *next* poll.
			if (e.GetMouseButton() == GS_MOUSE_BUTTON_LEFT
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0)
				m_Selected = m_Hovered;
			return false;
		});

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;
			if (e.GetKeyCode() == GS_KEY_DELETE && g_EditorScene.IsValid(m_Selected))
			{
				g_EditorScene.DestroyEntity(m_Selected);
				m_Selected = GS::InvalidEntity;
			}
			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (!IsActive())
			return;

		ImGui::Begin("Outliner");

		ImGui::Text("Entities: %zu", g_EditorScene.GetEntityCount());
		ImGui::BeginChild("hierarchy", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		for (GS::EntityId entity : g_EditorScene.GetEntities())
		{
			auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
			if (!tag)
				continue;

			ImGui::PushID((int)entity);
			if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
				m_Selected = entity;
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::End();

		ImGui::Begin("Inspector");

		if (!g_EditorScene.IsValid(m_Selected))
		{
			ImGui::TextDisabled("Nothing selected.");
			ImGui::End();
			return;
		}

		if (auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(m_Selected))
			ImGui::Text("%s  (id %u)", tag->Name.c_str(), m_Selected);

		if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
		{
			ImGui::DragFloat3("Position", &transform->Position.x, 0.01f);
			ImGui::DragFloat3("Rotation", &transform->Rotation.x, 0.5f);
			ImGui::DragFloat3("Scale", &transform->Scale.x, 0.01f, 0.02f, 20.0f);
		}

		if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected))
		{
			ImGui::ColorEdit4("Mesh colour", &mesh->Color.x);
			ImGui::Checkbox("Visible", &mesh->Visible);
			ImGui::TextDisabled("Source: %s",
				mesh->SourcePath.empty() ? "(none)" : mesh->SourcePath.c_str());
		}

		if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(m_Selected))
		{
			ImGui::SliderFloat("Fov", &camera->Fov, 10.0f, 120.0f);
			ImGui::Checkbox("Active camera", &camera->Active);
		}

		if (ImGui::Button("Delete"))
		{
			g_EditorScene.DestroyEntity(m_Selected);
			m_Selected = GS::InvalidEntity;
		}

		ImGui::End();
	}

private:
	void BuildTarget()
	{
		GS::Window& window = GS::Application::Get().GetWindow();

		GS::FramebufferSpecification spec;
		spec.Width = window.GetWidth() > 0 ? window.GetWidth() : 1280;
		spec.Height = window.GetHeight() > 0 ? window.GetHeight() : 720;
		spec.Attachments = {
			GS::FramebufferTextureFormat::RGBA8,
			GS::FramebufferTextureFormat::RED_INTEGER,
			GS::FramebufferTextureFormat::DEPTH24STENCIL8
		};

		m_Framebuffer.reset(GS::Framebuffer::Create(spec));
		m_BlitCamera.SetProjection(-1.0f, 1.0f, -1.0f, 1.0f);
	}

	// Same shader Cube3D built -- a lit, textured mesh with a picking output
	// -- minus the point light this view has no gizmo-draggable light for.
	// Ambient-only lighting is enough to tell shapes apart; a light entity is
	// exactly the kind of thing a scene *places* rather than the view owning
	// one permanently.
	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			out vec3 v_Normal;
			void main()
			{
				v_Normal = mat3(u_Transform) * a_Normal;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			layout(location = 1) out int entityID;
			in vec3 v_Normal;
			uniform vec4 u_Color;
			uniform int u_EntityID;
			void main()
			{
				float shade = 0.5 + 0.5 * max(dot(normalize(v_Normal), normalize(vec3(0.4, 1.0, 0.6))), 0.0);
				color = vec4(u_Color.rgb * shade, u_Color.a);
				entityID = u_EntityID;
			}
		)";

		m_Shader.reset(GS::Shader::Create("EditorSceneView", vertexSrc, fragmentSrc));
		m_SceneMaterial = GS::Material::Create(m_Shader);
	}

	void RenderMeshes()
	{
		GS::Renderer::BeginScene(m_Camera);

		auto& meshes = g_EditorScene.View<GS::MeshComponent>();

		for (size_t i = 0; i < meshes.Size(); i++)
		{
			GS::MeshComponent& mesh = meshes.Components()[i];
			if (!mesh.Visible || !mesh.Geometry)
				continue;

			GS::EntityId entity = meshes.Owner(i);
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			if (!transform)
				continue;

			const std::vector<GS::Submesh>& submeshes = mesh.Geometry->GetSubmeshes();
			if (mesh.Materials.size() < submeshes.size())
				mesh.Materials.resize(submeshes.size());

			for (size_t s = 0; s < submeshes.size(); s++)
			{
				if (!mesh.Materials[s])
					mesh.Materials[s] = GS::Material::CreateInstance(m_SceneMaterial);

				if (!mesh.MaterialsFromFile)
					mesh.Materials[s]->Set("u_Color", mesh.Color);

				mesh.Materials[s]->Set("u_EntityID", (int)GS::EntityIds::Index(entity));

				GS::Renderer::SubmitSubmesh(mesh.Materials[s], mesh.Geometry,
					(unsigned int)s, transform->GetTransform());
			}
		}

		GS::Renderer::EndScene();
	}

	void ResizeTarget()
	{
		GS::Window& window = GS::Application::Get().GetWindow();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		if (window.GetWidth() > 0 && window.GetHeight() > 0 &&
			(spec.Width != window.GetWidth() || spec.Height != window.GetHeight()))
			m_Framebuffer->Resize(window.GetWidth(), window.GetHeight());
	}

	void BlitToWindow()
	{
		unsigned int handle = m_Framebuffer->GetColorAttachmentRendererID(0);
		if (!m_ColorAttachment || m_ColorHandle != handle)
		{
			m_ColorAttachment.reset(GS::Texture2D::CreateFromHandle(handle,
				m_Framebuffer->GetSpecification().Width,
				m_Framebuffer->GetSpecification().Height));
			m_ColorHandle = handle;
		}

		GS::RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
		GS::RenderCommand::Clear();

		GS::Renderer2D::BeginScene(m_BlitCamera);
		GS::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(2.0f), m_ColorAttachment);
		GS::Renderer2D::EndScene();
	}

	void ReadHoveredEntity()
	{
		m_Hovered = GS::InvalidEntity;

		if (ImGui::GetIO().WantCaptureMouse || m_DragAxis >= 0)
			return;
		if (GS::Application::Get().IsUIHidden())
			return;

		auto [mouseX, mouseY] = GS::Input::GetMousePosition();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		int x = (int)mouseX;
		int y = (int)((float)spec.Height - mouseY);
		if (x < 0 || y < 0 || x >= (int)spec.Width || y >= (int)spec.Height)
			return;

		int slot = m_Framebuffer->ReadPixel(1, x, y);
		if (slot < 0)
			return;

		m_Hovered = g_EditorScene.EntityAtIndex((unsigned int)slot);
	}

	void DrawSelectionBox()
	{
		for (int pass = 0; pass < 2; pass++)
		{
			GS::EntityId entity = (pass == 0) ? m_Hovered : m_Selected;
			if (!g_EditorScene.IsValid(entity) || (pass == 0 && entity == m_Selected))
				continue;

			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
			if (!transform || !mesh || !mesh->Geometry)
				continue;

			glm::vec4 color = (pass == 0)
				? glm::vec4(0.45f, 0.85f, 1.0f, 1.0f)
				: glm::vec4(1.00f, 0.85f, 0.3f, 1.0f);

			glm::vec3 lo = mesh->Geometry->GetBoundsMin();
			glm::vec3 hi = mesh->Geometry->GetBoundsMax();
			glm::mat4 model = transform->GetTransform();

			glm::vec3 corner[8];
			for (int i = 0; i < 8; i++)
				corner[i] = glm::vec3(model * glm::vec4(
					(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z, 1.0f));

			static const int edges[12][2] = {
				{0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7}
			};
			for (auto& edge : edges)
				GS::Renderer2D::DrawLine(corner[edge[0]], corner[edge[1]], color);
		}
	}

	// Returns null when nothing is selected, which is why every caller checks
	// -- there is no light to fall back to dragging here, unlike Cube3D's own
	// version of this function.
	glm::vec3* GizmoPosition()
	{
		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		return transform ? &transform->Position : nullptr;
	}

	bool WorldToScreen(const glm::vec3& world, glm::vec2& outScreen) const
	{
		GS::Window& window = GS::Application::Get().GetWindow();

		glm::vec4 clip = m_Camera.GetViewProjectionMatrix() * glm::vec4(world, 1.0f);
		if (clip.w <= 0.0001f)
			return false;

		glm::vec3 ndc = glm::vec3(clip) / clip.w;
		outScreen = { (ndc.x * 0.5f + 0.5f) * (float)window.GetWidth(),
			(1.0f - (ndc.y * 0.5f + 0.5f)) * (float)window.GetHeight() };
		return true;
	}

	// Pixels -> a ray in world space: the near and far points that project to
	// this pixel, which the perspective divide makes different.
	void ScreenRay(const glm::vec2& mouse, glm::vec3& outOrigin, glm::vec3& outDirection) const
	{
		GS::Window& window = GS::Application::Get().GetWindow();
		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();

		float x = (mouse.x / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.y / height) * 2.0f;

		glm::mat4 inverse = glm::inverse(m_Camera.GetViewProjectionMatrix());

		glm::vec4 nearPoint = inverse * glm::vec4(x, y, -1.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);
		nearPoint /= nearPoint.w;
		farPoint /= farPoint.w;

		outOrigin = glm::vec3(nearPoint);
		outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
	}

	// How far along `axis` the point nearest the cursor ray sits -- the heart
	// of axis dragging, collapsing a 3D pick down to one number.
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
		if (std::abs(denominator) < 0.00001f)   // looking straight down the axis
			return false;

		outT = (b * e - c * d) / denominator;
		return true;
	}

	float AxisScreenDistance(int axis, const glm::vec2& mouse)
	{
		glm::vec3* origin = GizmoPosition();
		if (!origin)
			return std::numeric_limits<float>::max();

		glm::vec3 direction(0.0f);
		direction[axis] = 1.0f;

		glm::vec2 a, b;
		if (!WorldToScreen(*origin, a) || !WorldToScreen(*origin + direction * m_GizmoLength, b))
			return std::numeric_limits<float>::max();

		glm::vec2 segment = b - a;
		float lengthSquared = glm::dot(segment, segment);
		if (lengthSquared < 0.0001f)
			return glm::length(mouse - a);

		float t = glm::clamp(glm::dot(mouse - a, segment) / lengthSquared, 0.0f, 1.0f);
		return glm::length(mouse - (a + segment * t));
	}

	void UpdateGizmo()
	{
		glm::vec2 mouse = { GS::Input::GetMousePosition().first, GS::Input::GetMousePosition().second };

		bool down = GS::Input::IsMouseButtonPressed(GS_MOUSE_BUTTON_LEFT)
			&& !ImGui::GetIO().WantCaptureMouse;

		// A grab needs the button to go *down* while over a handle, not
		// merely to be held -- polling "is it held" would grab whatever the
		// cursor is near the instant a held button is first seen.
		bool justPressed = down && !m_MouseDownLastFrame;
		m_MouseDownLastFrame = down;

		if (!down)
		{
			m_DragAxis = -1;
			m_HoverAxis = -1;

			for (int axis = 0; axis < 3; axis++)
				if (AxisScreenDistance(axis, mouse) < m_GizmoPickPixels)
				{
					m_HoverAxis = axis;
					break;
				}
			return;
		}

		glm::vec3 rayOrigin, rayDirection;
		ScreenRay(mouse, rayOrigin, rayDirection);

		if (m_DragAxis < 0)
		{
			if (m_HoverAxis < 0 || !justPressed)
				return;

			glm::vec3* target = GizmoPosition();
			if (!target)
				return;

			glm::vec3 axisDirection(0.0f);
			axisDirection[m_HoverAxis] = 1.0f;

			float t;
			if (!ClosestPointOnAxis(*target, axisDirection, rayOrigin, rayDirection, t))
				return;

			m_DragAxis = m_HoverAxis;
			m_DragStartT = t;
			m_DragStartPosition = *target;
			return;
		}

		glm::vec3 axisDirection(0.0f);
		axisDirection[m_DragAxis] = 1.0f;

		float t;
		if (!ClosestPointOnAxis(m_DragStartPosition, axisDirection, rayOrigin, rayDirection, t))
			return;

		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		// Relative to where it was grabbed, so the object does not snap its
		// origin to the cursor.
		*target = m_DragStartPosition + axisDirection * (t - m_DragStartT);
	}

	void DrawGizmo()
	{
		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		glm::vec3 origin = *target;

		const glm::vec4 axisColors[3] = {
			{ 1.0f, 0.25f, 0.25f, 1.0f }, { 0.30f, 1.0f, 0.35f, 1.0f }, { 0.35f, 0.55f, 1.0f, 1.0f }
		};

		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 direction(0.0f);
			direction[axis] = 1.0f;

			glm::vec4 color = axisColors[axis];
			if (axis == m_DragAxis || (m_DragAxis < 0 && axis == m_HoverAxis))
				color = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);

			glm::vec3 tip = origin + direction * m_GizmoLength;
			GS::Renderer2D::DrawLine(origin, tip, color);

			// A little cross at the tip, so the end of the handle is visible
			// even when the line is nearly edge-on to the camera.
			glm::vec3 a(0.0f), b(0.0f);
			a[(axis + 1) % 3] = 0.06f;
			b[(axis + 2) % 3] = 0.06f;
			GS::Renderer2D::DrawLine(tip - a, tip + a, color);
			GS::Renderer2D::DrawLine(tip - b, tip + b, color);
		}
	}

	// Adapted from Cube3D's fly camera, not identical to it: arrow-key look
	// is dropped as redundant with middle-drag look, and the
	// WantCaptureKeyboard guard below is new -- this view sits beside the
	// Inspector's and Assets panel's text/numeric fields, and Cube3D never
	// had that problem because a demo doesn't share the window with editable
	// text.
	void MoveCamera(GS::Timestep ts)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		glm::vec3 position = m_Camera.GetPosition();
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		float move = m_MoveSpeed * ts;

		if (GS::Input::IsKeyPressed(GS_KEY_W)) position += m_Camera.GetForward() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_D)) position += m_Camera.GetRight() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_E)) position.y += move;
		if (GS::Input::IsKeyPressed(GS_KEY_Q)) position.y -= move;

		glm::vec2 mouse = { GS::Input::GetMousePosition().first, GS::Input::GetMousePosition().second };
		glm::vec2 delta = mouse - m_PreviousMouse;
		m_PreviousMouse = mouse;

		if (GS::Input::IsMouseButtonPressed(GS_MOUSE_BUTTON_MIDDLE) && !ImGui::GetIO().WantCaptureMouse)
		{
			yaw += delta.x * m_MouseLookSensitivity;
			pitch -= delta.y * m_MouseLookSensitivity;
		}

		m_Camera.SetPosition(position);
		m_Camera.SetRotation(yaw, pitch);
	}

private:
	GS::PerspectiveCamera m_Camera;
	GS::OrthographicCamera m_BlitCamera{ -1.0f, 1.0f, -1.0f, 1.0f };

	std::shared_ptr<GS::Framebuffer> m_Framebuffer;
	std::shared_ptr<GS::Texture2D> m_ColorAttachment;
	unsigned int m_ColorHandle = 0;

	std::shared_ptr<GS::Shader> m_Shader;
	std::shared_ptr<GS::Material> m_SceneMaterial;

	GS::EntityId m_Selected = GS::InvalidEntity;
	GS::EntityId m_Hovered = GS::InvalidEntity;

	bool m_ShowGizmo = true;
	int m_DragAxis = -1;
	int m_HoverAxis = -1;
	bool m_MouseDownLastFrame = false;
	float m_DragStartT = 0.0f;
	glm::vec3 m_DragStartPosition{ 0.0f };
	float m_GizmoLength = 1.0f;
	float m_GizmoPickPixels = 12.0f;

	glm::vec2 m_PreviousMouse{ 0.0f, 0.0f };
	float m_MoveSpeed = 3.0f;
	float m_MouseLookSensitivity = 0.18f;
};
