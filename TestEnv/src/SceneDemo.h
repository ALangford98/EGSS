#pragma once

// Entities, components, and the systems that walk them.
//
// The point of a scene layer is that a thing has *identity*. Before this, the
// physics demo held bodies in one array and drew them from that same array;
// the lighting demo held obstacles the same way. Nothing could be a physical
// object and a sprite and a light at once without a struct that knew about all
// three.
//
// Here a "system" is just a loop that asks the scene for one component store
// and walks it. There is no scheduler and no registration -- a component is
// any struct.

#include <GS.h>
#include <imgui.h>

#include <glm/gtc/constants.hpp>

#include "Demo.h"

class SceneDemo : public DemoLayer
{
public:
	SceneDemo()
		: DemoLayer("SceneDemo"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
	}

	void OnDemoAttach() override
	{
		// Colour to look at, an integer attachment to pick from, and depth.
		// The picking attachment is the whole reason this demo renders
		// off-screen rather than straight to the window.
		GS::FramebufferSpecification spec;
		spec.Width = 1280;
		spec.Height = 720;
		spec.Attachments = {
			GS::FramebufferTextureFormat::RGBA8,
			GS::FramebufferTextureFormat::RED_INTEGER,
			GS::FramebufferTextureFormat::DEPTH24STENCIL8
		};
		m_Framebuffer.reset(GS::Framebuffer::Create(spec));

		// A camera covering exactly clip space, for blitting the result back
		// to the window as one textured quad.
		m_BlitCamera.SetProjection(-1.0f, 1.0f, -1.0f, 1.0f);

		BuildScene();
	}

	void BuildScene()
	{
		m_Scene.Clear();
		m_Selected = GS::InvalidEntity;

		m_Scene.GetPhysics().Gravity = { 0.0f, -6.0f };

		// Static geometry. These entities own a body too -- the difference is
		// only which side drives which.
		AddWall("Floor", { 0.0f, -0.80f }, { 1.45f, 0.05f });
		AddWall("Ceiling", { 0.0f, 0.80f }, { 1.45f, 0.05f });
		AddWall("Left wall", { -1.40f, 0.0f }, { 0.05f, 0.85f });
		AddWall("Right wall", { 1.40f, 0.0f }, { 0.05f, 0.85f });
		AddWall("Ledge", { -0.45f, -0.30f }, { 0.35f, 0.04f });

		for (int i = 0; i < 6; i++)
			SpawnBox();
	}

	GS::Entity AddWall(const std::string& name, const glm::vec2& position, const glm::vec2& halfExtents)
	{
		GS::Entity entity = m_Scene.CreateEntity(name);

		auto* transform = entity.Get<GS::TransformComponent>();
		transform->Position = glm::vec3(position, 0.0f);
		transform->Scale = glm::vec3(halfExtents * 2.0f, 1.0f);

		entity.Add<GS::SpriteComponent>({ { 0.32f, 0.34f, 0.40f, 1.0f }, nullptr, 1.0f });

		unsigned int body = m_Scene.GetPhysics().AddBody(
			GS::RigidBody2D::MakeStaticBox(position, halfExtents));

		// The transform drives this body, not the other way round -- so moving
		// a wall in the panel actually moves the collider.
		entity.Add<GS::RigidBody2DComponent>({ body, false });

		return entity;
	}

	GS::Entity SpawnBox()
	{
		float x = std::sin((float)m_SpawnCounter * 12.9898f) * 1.1f;
		float size = 0.06f + 0.04f * std::abs(std::cos((float)m_SpawnCounter * 4.1f));
		m_SpawnCounter++;

		GS::Entity entity = m_Scene.CreateEntity("Box " + std::to_string(m_SpawnCounter));

		auto* transform = entity.Get<GS::TransformComponent>();
		transform->Position = { x, 0.65f, 0.0f };
		transform->Scale = { size * 2.0f, size * 2.0f, 1.0f };

		// Hue spread so individual entities are tellable apart.
		float t = (float)m_SpawnCounter * 0.31f;
		entity.Add<GS::SpriteComponent>({ {
			0.55f + 0.45f * std::cos(glm::two_pi<float>() * t),
			0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.33f)),
			0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.66f)),
			1.0f }, nullptr, 1.0f });

		GS::RigidBody2D body = GS::RigidBody2D::MakeBox({ x, 0.65f }, { size, size }, 1.0f);
		body.Restitution = 0.2f;
		body.Friction = 0.5f;

		entity.Add<GS::RigidBody2DComponent>({ m_Scene.GetPhysics().AddBody(body), true });

		// Not every entity has every component: only some carry a light, and
		// the lighting system below simply never sees the rest.
		if (m_SpawnCounter % 3 == 0)
			entity.Add<GS::LightComponent>({ { 1.0f, 0.85f, 0.5f, 1.0f }, 0.55f, true });

		return entity;
	}

	void OnDemoFixedUpdate(GS::Timestep fixedStep) override
	{
		if (m_Paused)
			return;

		// One call: pushes transform-driven bodies in, steps, reads
		// physics-driven bodies back out.
		m_Scene.StepPhysics(fixedStep);
	}

	void OnDemoUpdate(GS::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		// Keep the target the same size as the window, so a framebuffer pixel
		// and a window pixel are the same thing and the mouse needs no
		// rebasing.
		GS::Window& window = GS::Application::Get().GetWindow();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		if (window.GetWidth() > 0 && window.GetHeight() > 0 &&
			(spec.Width != window.GetWidth() || spec.Height != window.GetHeight()))
			m_Framebuffer->Resize(window.GetWidth(), window.GetHeight());

		GS::Renderer2D::ResetStats();

		// --- Pass 1: the scene, into the framebuffer ---
		m_Framebuffer->Bind();

		GS::RenderCommand::SetClearColor({ 0.05f, 0.05f, 0.07f, 1.0f });
		GS::RenderCommand::Clear();

		// glClear only carries a float colour, so the integer attachment is
		// cleared separately. -1 is "nothing here".
		m_Framebuffer->ClearAttachment(1, -1);

		GS::Renderer2D::BeginScene(m_Camera);

		RenderSprites();
		RenderLights();
		RenderSelection();

		GS::Renderer2D::EndScene();

		// Read back before unbinding -- this is the pick.
		ReadHoveredEntity();

		m_Framebuffer->Unbind();

		// --- Pass 2: the result, to the window ---
		GS::RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
		GS::RenderCommand::Clear();

		// The wrapper is rebuilt whenever the framebuffer is recreated, since
		// a resize makes new textures and invalidates the old handle.
		unsigned int handle = m_Framebuffer->GetColorAttachmentRendererID(0);
		if (!m_ColorAttachment || m_ColorHandle != handle)
		{
			m_ColorAttachment.reset(GS::Texture2D::CreateFromHandle(handle,
				m_Framebuffer->GetSpecification().Width,
				m_Framebuffer->GetSpecification().Height));
			m_ColorHandle = handle;
		}

		GS::Renderer2D::BeginScene(m_BlitCamera);
		GS::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(2.0f), m_ColorAttachment);
		GS::Renderer2D::EndScene();
	}

	// What is under the cursor, straight out of the integer attachment.
	//
	// Pixel-exact and free of geometry maths: whatever the renderer decided to
	// draw there is what gets picked, including rotated, overlapping or
	// irregular shapes that a bounding-box test would get wrong.
	void ReadHoveredEntity()
	{
		m_Hovered = GS::InvalidEntity;

		if (ImGui::GetIO().WantCaptureMouse)
			return;

		auto [mouseX, mouseY] = GS::Input::GetMousePosition();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		// Flip y: window coordinates count down, GL counts up.
		int x = (int)mouseX;
		int y = (int)((float)spec.Height - mouseY);

		if (x < 0 || y < 0 || x >= (int)spec.Width || y >= (int)spec.Height)
			return;

		int slot = m_Framebuffer->ReadPixel(1, x, y);
		if (slot < 0)
			return;

		// The buffer holds the entity's slot, not its handle -- see
		// Scene::EntityAtIndex for why.
		m_Hovered = m_Scene.EntityAtIndex((unsigned int)slot);
	}

	// A system: walk every sprite, look up its transform, draw it.
	//
	// Note what it does *not* need to know -- whether the entity has a body, a
	// light, or anything else. Adding a component to an entity never touches
	// the systems that do not care about it.
	void RenderSprites()
	{
		auto& sprites = m_Scene.View<GS::SpriteComponent>();

		for (size_t i = 0; i < sprites.Size(); i++)
		{
			GS::EntityId entity = sprites.Owner(i);
			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(entity);
			if (!transform)
				continue;

			const GS::SpriteComponent& sprite = sprites.Components()[i];

			// The last argument is what makes picking work: the renderer
			// writes it into the integer attachment for every pixel it covers.
			GS::Renderer2D::DrawQuad(transform->Position,
				glm::vec2(transform->Scale), sprite.Color,
				(int)GS::EntityIds::Index(entity));
		}
	}

	// A second system over the same entities, reading a different component.
	void RenderLights()
	{
		auto& lights = m_Scene.View<GS::LightComponent>();

		for (size_t i = 0; i < lights.Size(); i++)
		{
			const GS::LightComponent& light = lights.Components()[i];
			if (!light.Enabled)
				continue;

			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(lights.Owner(i));
			if (!transform)
				continue;

			// A ring, so a lit entity is obvious without a full lighting pass.
			const int segments = 20;
			glm::vec2 centre(transform->Position);
			glm::vec2 previous = centre + glm::vec2(light.Radius, 0.0f);

			for (int s = 1; s <= segments; s++)
			{
				float angle = (float)s / (float)segments * glm::two_pi<float>();
				glm::vec2 next = centre + glm::vec2(std::cos(angle), std::sin(angle)) * light.Radius;
				GS::Renderer2D::DrawLine(previous, next, light.Color * 0.5f);
				previous = next;
			}
		}
	}

	void RenderSelection()
	{
		if (m_Scene.IsValid(m_Hovered) && m_Hovered != m_Selected)
		{
			if (auto* transform = m_Scene.GetComponent<GS::TransformComponent>(m_Hovered))
				GS::Renderer2D::DrawRect(transform->Position,
					glm::vec2(transform->Scale) * 1.08f, glm::vec4(0.6f, 0.8f, 1.0f, 0.7f));
		}

		if (!m_Scene.IsValid(m_Selected))
			return;

		auto* transform = m_Scene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		GS::Renderer2D::DrawRect(transform->Position, glm::vec2(transform->Scale) * 1.15f,
			glm::vec4(1.0f, 0.9f, 0.3f, 1.0f));
	}

	glm::vec2 ScreenToWorld(const std::pair<float, float>& mouse) const
	{
		GS::Window& window = GS::Application::Get().GetWindow();
		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();

		float x = (mouse.first / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.second / height) * 2.0f;

		return glm::vec2(glm::inverse(m_Camera.GetViewProjectionMatrix()) * glm::vec4(x, y, 0.0f, 1.0f));
	}

	void OnDemoEvent(GS::Event& e) override
	{
		GS::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<GS::WindowResizeEvent>([this](GS::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
			{
				float aspect = (float)e.GetWidth() / (float)e.GetHeight();
				m_Camera.SetProjection(-aspect * 0.9f, aspect * 0.9f, -0.9f, 0.9f);
			}
			return false;
		});

		dispatcher.Dispatch<GS::MouseButtonPressedEvent>([this](GS::MouseButtonPressedEvent& e)
		{
			if (e.GetMouseButton() != GS_MOUSE_BUTTON_LEFT || ImGui::GetIO().WantCaptureMouse)
				return false;

			// No geometry test: the answer was read out of the framebuffer
			// during the render pass.
			m_Selected = m_Hovered;
			return false;
		});

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			if (e.GetKeyCode() == GS_KEY_SPACE)
				SpawnBox();
			if (e.GetKeyCode() == GS_KEY_P)
				m_Paused = !m_Paused;
			if (e.GetKeyCode() == GS_KEY_R)
				BuildScene();
			if (e.GetKeyCode() == GS_KEY_DELETE && m_Scene.IsValid(m_Selected))
			{
				m_Scene.DestroyEntity(m_Selected);
				m_Selected = GS::InvalidEntity;
			}

			return false;
		});
	}

	void OnDemoImGui() override
	{
		auto stats = GS::Renderer2D::GetStats();

		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Scene");

		ImGui::Text("Click an entity to select. Space spawns, Delete removes.");
		ImGui::Text("P pauses, R rebuilds.");

		ImGui::Separator();
		ImGui::Text("Entities: %zu", m_Scene.GetEntityCount());
		ImGui::Text("Sprites: %zu   Bodies: %zu   Lights: %zu",
			m_Scene.View<GS::SpriteComponent>().Size(),
			m_Scene.View<GS::RigidBody2DComponent>().Size(),
			m_Scene.View<GS::LightComponent>().Size());
		ImGui::Text("Frame: %.2f ms   Draw calls: %u", m_FrameTime, stats.DrawCalls);

		if (auto* tag = m_Scene.GetComponent<GS::TagComponent>(m_Hovered))
			ImGui::Text("Hovered: %s", tag->Name.c_str());
		else
			ImGui::TextDisabled("Hovered: -");

		// --- The list ---
		ImGui::SeparatorText("Hierarchy");

		if (ImGui::BeginChild("entities", ImVec2(0, 140), ImGuiChildFlags_Borders))
		{
			for (GS::EntityId entity : m_Scene.GetEntities())
			{
				auto* tag = m_Scene.GetComponent<GS::TagComponent>(entity);

				ImGui::PushID((int)entity);
				if (ImGui::Selectable(tag ? tag->Name.c_str() : "?", entity == m_Selected))
					m_Selected = entity;
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		// --- The inspector ---
		ImGui::SeparatorText("Inspector");

		if (!m_Scene.IsValid(m_Selected))
		{
			ImGui::TextDisabled("Nothing selected.");
			ImGui::End();
			return;
		}

		GS::Entity entity = m_Scene.Wrap(m_Selected);

		if (auto* tag = entity.Get<GS::TagComponent>())
			ImGui::Text("%s  (id %u)", tag->Name.c_str(), m_Selected);

		if (auto* transform = entity.Get<GS::TransformComponent>())
		{
			ImGui::DragFloat3("Position", &transform->Position.x, 0.01f);
			ImGui::DragFloat3("Scale", &transform->Scale.x, 0.01f, 0.02f, 4.0f);
		}

		if (auto* sprite = entity.Get<GS::SpriteComponent>())
			ImGui::ColorEdit4("Sprite", &sprite->Color.x);

		if (auto* light = entity.Get<GS::LightComponent>())
		{
			ImGui::Checkbox("Light enabled", &light->Enabled);
			ImGui::SliderFloat("Light radius", &light->Radius, 0.1f, 2.0f);
			ImGui::ColorEdit4("Light colour", &light->Color.x);
		}
		else if (ImGui::Button("Add light"))
		{
			entity.Add<GS::LightComponent>({ { 1.0f, 0.85f, 0.5f, 1.0f }, 0.55f, true });
		}

		if (auto* body = entity.Get<GS::RigidBody2DComponent>())
		{
			ImGui::Checkbox("Driven by physics", &body->DrivenByPhysics);
			ImGui::TextDisabled("body %u", body->Body);
		}

		ImGui::End();
	}

private:
	GS::OrthographicCamera m_Camera;
	GS::Scene m_Scene;

	std::shared_ptr<GS::Framebuffer> m_Framebuffer;
	std::shared_ptr<GS::Texture2D> m_ColorAttachment;
	unsigned int m_ColorHandle = 0;
	GS::OrthographicCamera m_BlitCamera{ -1.0f, 1.0f, -1.0f, 1.0f };

	GS::EntityId m_Selected = GS::InvalidEntity;
	GS::EntityId m_Hovered = GS::InvalidEntity;
	int m_SpawnCounter = 0;
	bool m_Paused = false;
	float m_FrameTime = 0.0f;
};
