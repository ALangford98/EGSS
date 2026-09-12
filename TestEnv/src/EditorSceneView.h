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
#include <cstring>
#include <filesystem>

#include "Demo.h"
#include "EditableMesh.h"
#include "EditorHistory.h"
#include "EditorProject.h"
#include "FileBrowserPopup.h"
#include "PlayMode.h"

// Forward-declared so it can be set from OnAttach() below: a member function
// body is a complete-class context for the class's *own* members, but that
// does not reach forward to a namespace-scope global declared later in the
// file -- so the global has to come first, as EditorShell.h's g_EditorShell
// does.
class EditorSceneView;
inline EditorSceneView* g_EditorSceneView = nullptr;

// Rotation-only, matching TransformComponent::GetTransform()'s own X-then-Y-
// then-Z order, against a local forward of (0,0,-1) -- zero rotation faces
// -Z, the same convention the editor fly-camera's default yaw already
// assumes. Shared by DrawCameraRays (an indicator ray) and ActiveCamera (an
// active CameraComponent's actual view direction while Playing) so the two
// never drift apart on what "which way is this camera facing" means.
inline glm::vec3 ForwardFromRotation(const glm::vec3& rotationDegrees)
{
	glm::mat4 rotation =
		glm::rotate(glm::mat4(1.0f), glm::radians(rotationDegrees.x), glm::vec3(1, 0, 0))
		* glm::rotate(glm::mat4(1.0f), glm::radians(rotationDegrees.y), glm::vec3(0, 1, 0))
		* glm::rotate(glm::mat4(1.0f), glm::radians(rotationDegrees.z), glm::vec3(0, 0, 1));
	return glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
}

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
		SyncMeshEditWorldPointBeforeGizmo();
		UpdateGizmo();
		SyncMeshEditWorldPointAfterGizmo();
		RebuildMeshEditPreviewIfNeeded();

		m_Framebuffer->Bind();

		GS::RenderCommand::SetClearColor({ 0.10f, 0.11f, 0.13f, 1.0f });
		GS::RenderCommand::Clear();
		m_Framebuffer->ClearAttachment(1, -1);

		GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		RenderMeshes();
		GS::RenderCommand::SetCullFace(GS::CullFace::None);

		GS::Renderer2D::BeginScene(ActiveCamera());
		DrawSelectionBox();
		DrawCameraRays();
		DrawLightGizmos();
		DrawMeshEditOverlay();
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
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0 && !m_MeshEditActive)
				m_Selected = m_Hovered;
			return false;
		});

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;
			if (e.GetKeyCode() == GS_KEY_DELETE && g_EditorScene.IsValid(m_Selected))
				m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(m_Selected));
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
			ImGui::BeginDisabled(m_MeshEditActive);
			if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
				m_Selected = entity;
			ImGui::EndDisabled();
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

		// The editable ID a script references this entity by --
		// GS::TagComponent::Name, already what scene.findByTag(name) looks
		// up (Breakout's recreation already relies on exactly this: "tag
		// Paddle"/"tag Ball" in scene.txt, scene.findByTag("Paddle") in
		// ball.gss). Not a second, parallel ID system -- one already
		// existed, serialized and resolvable, and simply had no Inspector
		// field to edit it from; every entity kept whatever default name
		// (PlaceEntityCommand's default: "Cube", "Light", ...) it was given
		// at creation, with no way to rename it afterward short of hand-
		// editing scene.txt. No uniqueness is enforced here, same as
		// before this change -- scene.findByTag already just returns the
		// first match, so two entities sharing a name means whichever a
		// script asks for, it gets the earlier one silently. Disclosed,
		// not fixed: enforcing it is a bigger, separate feature nobody
		// asked for.
		if (auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(m_Selected))
		{
			char nameBuf[256];
			strncpy(nameBuf, tag->Name.c_str(), sizeof(nameBuf) - 1);
			nameBuf[sizeof(nameBuf) - 1] = '\0';

			ImGui::PushItemWidth(160.0f);
			if (ImGui::InputText("ID##entityname", nameBuf, sizeof(nameBuf)))
				tag->Name = nameBuf;
			ImGui::PopItemWidth();
			if (ImGui::IsItemActivated())
				m_EditBeforeString = tag->Name;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TagComponent, std::string>>(
					m_Selected, &GS::TagComponent::Name, m_EditBeforeString, tag->Name));

			ImGui::SameLine();
			if (ImGui::SmallButton("Copy"))
				ImGui::SetClipboardText(tag->Name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("scene.findByTag(\"%s\")", tag->Name.c_str());

			// Checked, not assumed: Scene::Load always Clear()s first and
			// recreates entities in file order from scratch, so the handle
			// (slot index + generation) only reproduces identically across
			// a Save/Load round trip when nothing was created or destroyed
			// in between -- which describes most real editing sessions
			// (undo/redo, scene.spawn/destroy during Play, a deleted-then-
			// replaced entity). The ID above is what stays put regardless.
			ImGui::TextDisabled("entity handle %u -- can change after a Save/Load if anything was created or destroyed first", m_Selected);
		}

		if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
		{
			ImGui::DragFloat3("Position", &transform->Position.x, 0.01f);
			if (ImGui::IsItemActivated())
				m_EditBeforeVec3 = transform->Position;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
					m_Selected, &GS::TransformComponent::Position, m_EditBeforeVec3, transform->Position));

			ImGui::DragFloat3("Rotation", &transform->Rotation.x, 0.5f);
			if (ImGui::IsItemActivated())
				m_EditBeforeVec3 = transform->Rotation;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
					m_Selected, &GS::TransformComponent::Rotation, m_EditBeforeVec3, transform->Rotation));

			ImGui::DragFloat3("Scale", &transform->Scale.x, 0.01f, 0.02f, 20.0f);
			if (ImGui::IsItemActivated())
				m_EditBeforeVec3 = transform->Scale;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
					m_Selected, &GS::TransformComponent::Scale, m_EditBeforeVec3, transform->Scale));
		}

		if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected))
		{
			ImGui::ColorEdit4("Mesh colour", &mesh->Color.x);
			if (ImGui::IsItemActivated())
				m_EditBeforeVec4 = mesh->Color;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, glm::vec4>>(
					m_Selected, &GS::MeshComponent::Color, m_EditBeforeVec4, mesh->Color));

			ImGui::Checkbox("Visible", &mesh->Visible);
			// Not negated. A Checkbox activates on the mouse-*down* frame and
			// only toggles on release -- ImGui::Checkbox's `pressed` comes
			// from ButtonBehavior's default PressedOnClickRelease -- so the
			// value is still the pre-click one here. Measured against the
			// vendored ImGui 1.92.9b with a null-backend probe: activation on
			// frame N with value 1, deactivated-after-edit on frame N+1 with
			// value 0. Negating would store 0 and make Undo a no-op, which is
			// the bug this capture exists to fix.
			if (ImGui::IsItemActivated())
				m_EditBeforeBool = mesh->Visible;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, bool>>(
					m_Selected, &GS::MeshComponent::Visible, m_EditBeforeBool, mesh->Visible));

			ImGui::TextDisabled("Source: %s",
				mesh->SourcePath.empty() ? "(none)" : mesh->SourcePath.c_str());

			if (ImGui::Button("Edit Mesh") && !m_MeshEditActive && !PlayMode::IsPlaying())
			{
				GS::MeshData data;
				if (mesh->SourcePath.rfind("primitive:cube", 0) == 0)
					data = GS::Mesh::CreateCubeData();
				else if (mesh->SourcePath.rfind("primitive:plane", 0) == 0)
					data = GS::Mesh::CreatePlaneData();
				else if (mesh->SourcePath.rfind("primitive:sphere", 0) == 0)
					data = GS::Mesh::CreateSphereData();
				else if (mesh->SourcePath.rfind("primitive:cylinder", 0) == 0)
					data = GS::Mesh::CreateCylinderData();
				else
				{
					std::string error;
					GS::Mesh::LoadData(mesh->SourcePath, data, error);
				}

				m_MeshEditSession = EditableMesh::FromMeshData(data);
				m_MeshEditSelectedPoint = -1;
				m_MeshEditActive = true;
			}

			if (m_MeshEditActive)
			{
				ImGui::SeparatorText("Mesh Edit");

				bool hasSelection = m_MeshEditSelectedPoint >= 0;

				if (!hasSelection) ImGui::BeginDisabled();
				if (ImGui::Button("Delete Point") && m_MeshEditSession.CanDelete(m_MeshEditSelectedPoint))
				{
					m_MeshEditSession.PushUndo();
					m_MeshEditSession.DeletePoint(m_MeshEditSelectedPoint);
					m_MeshEditSelectedPoint = -1;
					m_MeshEditPreviewDirty = true;
				}
				if (!hasSelection) ImGui::EndDisabled();

				if (ImGui::Button("Undo##meshedit") && m_MeshEditSession.CanUndo())
				{
					m_MeshEditSession.Undo();
					m_MeshEditPreviewDirty = true;
				}
				ImGui::SameLine();
				if (ImGui::Button("Redo##meshedit") && m_MeshEditSession.CanRedo())
				{
					m_MeshEditSession.Redo();
					m_MeshEditPreviewDirty = true;
				}

				if (ImGui::Button("Done"))
				{
					GS::MeshData finalData = m_MeshEditSession.Rebuild();

					// TagComponent::Name for the export filename -- fetched
					// independently rather than assumed in scope: the ID
					// field's own `if (auto* tag = ...)` block (further up
					// this same Inspector) is a separate `if`, already
					// closed by here. A missing tag (shouldn't happen --
					// every entity gets one at creation) falls back to a
					// generic name rather than crashing on a null dereference.
					auto* entityTag = g_EditorScene.GetComponent<GS::TagComponent>(m_Selected);
					std::string exportName = entityTag ? entityTag->Name : "Mesh";
					std::string exportPath = "assets/" + exportName + ".obj";

					// Collision suffix -- default entity names are literally
					// "Cube"/"Sphere"/etc. (PlaceEntityCommand's own
					// defaults), so two never-renamed entities colliding on
					// the same export path is a real, likely case, not a
					// hypothetical one. Skipped when this entity's own
					// SourcePath already *is* the target path -- re-editing
					// the same entity a second time should overwrite its own
					// previous export, not spawn Cube1.obj, Cube2.obj, ...
					// forever.
					if (mesh->SourcePath != exportPath && std::filesystem::exists(exportPath))
					{
						int suffix = 1;
						std::string candidate;
						do
						{
							candidate = "assets/" + exportName + std::to_string(suffix) + ".obj";
							suffix++;
						} while (std::filesystem::exists(candidate) && candidate != mesh->SourcePath);
						exportPath = candidate;
					}

					std::string error;
					if (GS::ObjWriter::Save(exportPath, finalData, error))
					{
						std::string oldPath = mesh->SourcePath;
						mesh->SourcePath = exportPath;

						// Not MeshCache::Get(exportPath) -- MeshCache caches
						// "one mesh per path, ever, for the run" with no
						// invalidation, so re-editing this same entity a
						// second time (same exportPath, per the collision-
						// suffix skip above) would hit the FIRST edit's
						// stale cache entry and silently revert the visible
						// geometry to it, even though the file on disk was
						// just correctly overwritten. Building directly from
						// finalData -- what was actually just saved --
						// sidesteps the cache entirely for this entity.
						mesh->Geometry = std::make_shared<GS::Mesh>(finalData, exportName);

						EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, std::string>>(
							m_Selected, &GS::MeshComponent::SourcePath, oldPath, exportPath));
					}

					m_MeshEditActive = false;
					m_MeshEditSelectedPoint = -1;
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
				{
					auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
					if (transform)
						mesh->Geometry = GS::MeshCache::Get(mesh->SourcePath);   // discard the live preview -- reload the entity's real, unchanged mesh

					m_MeshEditActive = false;
					m_MeshEditSelectedPoint = -1;
				}
			}
		}

		if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(m_Selected))
		{
			ImGui::SliderFloat("Fov", &camera->Fov, 10.0f, 120.0f);
			if (ImGui::IsItemActivated())
				m_EditBeforeFloat = camera->Fov;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::CameraComponent, float>>(
					m_Selected, &GS::CameraComponent::Fov, m_EditBeforeFloat, camera->Fov));

			ImGui::Checkbox("Active camera", &camera->Active);
			if (ImGui::IsItemActivated())   // not negated -- see "Visible" above
				m_EditBeforeBool = camera->Active;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::CameraComponent, bool>>(
					m_Selected, &GS::CameraComponent::Active, m_EditBeforeBool, camera->Active));
		}

		if (auto* light = g_EditorScene.GetComponent<GS::LightComponent>(m_Selected))
		{
			ImGui::ColorEdit4("Light colour", &light->Color.x);
			if (ImGui::IsItemActivated())
				m_EditBeforeVec4 = light->Color;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::LightComponent, glm::vec4>>(
					m_Selected, &GS::LightComponent::Color, m_EditBeforeVec4, light->Color));

			ImGui::DragFloat("Radius", &light->Radius, 0.05f, 0.0f, 100.0f);
			if (ImGui::IsItemActivated())
				m_EditBeforeFloat = light->Radius;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::LightComponent, float>>(
					m_Selected, &GS::LightComponent::Radius, m_EditBeforeFloat, light->Radius));

			ImGui::Checkbox("Enabled", &light->Enabled);
			if (ImGui::IsItemActivated())   // not negated -- see "Visible" above
				m_EditBeforeBool = light->Enabled;
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::LightComponent, bool>>(
					m_Selected, &GS::LightComponent::Enabled, m_EditBeforeBool, light->Enabled));
		}

		ImGui::SeparatorText("Script");
		if (auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(m_Selected))
		{
			char pathBuf[512];
			strncpy(pathBuf, script->ScriptPath.c_str(), sizeof(pathBuf) - 1);
			pathBuf[sizeof(pathBuf) - 1] = '\0';
			if (ImGui::InputText("##scriptpath", pathBuf, sizeof(pathBuf)))
				script->ScriptPath = pathBuf;
			ImGui::SameLine();
			// Rooted at the open project's folder (falls back to the
			// working directory itself if none is open) -- that's a real
			// path only relative to the working directory, per how
			// ScriptEngine/PlayMode actually open it (a plain
			// std::ifstream against ScriptPath, no project-folder prefix),
			// which is why the result is converted back to that below
			// rather than kept as the absolute path the browser picked it
			// with.
			if (ImGui::Button("Browse...##script"))
				m_ScriptBrowser.Open("Choose Script", FileBrowserPopup::Mode::PickFile,
					g_EditorProjectPath.empty() ? "." : g_EditorProjectPath, ".gss");
			if (ImGui::Button("Remove Script"))
				g_EditorScene.RemoveComponent<GS::ScriptComponent>(m_Selected);
		}
		else
		{
			if (ImGui::Button("Add Script"))
				g_EditorScene.AddComponent<GS::ScriptComponent>(m_Selected, GS::ScriptComponent{});
		}

		if (m_ScriptBrowser.HasResult())
		{
			std::string picked = m_ScriptBrowser.TakeResult();
			if (auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(m_Selected))
			{
				std::error_code ec;
				std::filesystem::path relative = std::filesystem::relative(picked, std::filesystem::current_path(ec), ec);
				script->ScriptPath = (ec || relative.empty()) ? picked : relative.generic_string();
			}
		}
		m_ScriptBrowser.Draw();

		if (ImGui::Button("Delete"))
			m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(m_Selected));

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

	// Cube3D's own point-light math (attenuation, Lambert diffuse, Blinn-
	// Phong specular -- see its BuildShader for the derivation of each term)
	// summed over however many enabled GS::LightComponent entities the scene
	// actually has, rather than Cube3D's one hardcoded light -- a scene
	// *places* lights, the view doesn't own one permanently. u_AmbientStrength
	// is higher than Cube3D's default (0.10): Cube3D always has exactly one
	// light: an editor scene can have zero, and 10% brightness with nothing
	// placed would read as "broken," not "unlit." No u_Texture -- nothing in
	// EditorSceneView's own placement flow (PlaceEntityCommand's primitives)
	// sets MaterialsFromFile, so every submesh here is flat-colored; Cube3D's
	// gltf/obj-loaded path is the only one that needs a sampler.
	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			out vec3 v_WorldPosition;
			out vec3 v_Normal;
			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				v_WorldPosition = world.xyz;
				v_Normal = mat3(u_Transform) * a_Normal;
				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			layout(location = 1) out int entityID;
			in vec3 v_WorldPosition;
			in vec3 v_Normal;
			uniform vec4 u_Color;
			uniform int u_EntityID;

			const int MAX_LIGHTS = 8;
			uniform int u_LightCount;
			uniform vec3 u_LightPositions[MAX_LIGHTS];
			uniform vec3 u_LightColors[MAX_LIGHTS];
			uniform float u_LightRanges[MAX_LIGHTS];
			uniform vec3 u_CameraPosition;
			uniform float u_AmbientStrength;

			void main()
			{
				vec3 normal = normalize(v_Normal);
				vec3 toEye  = normalize(u_CameraPosition - v_WorldPosition);
				vec3 base   = u_Color.rgb;

				vec3 lit = base * u_AmbientStrength;
				for (int i = 0; i < u_LightCount; i++)
				{
					vec3 lightVector    = u_LightPositions[i] - v_WorldPosition;
					float lightDistance = length(lightVector);
					vec3 toLight        = lightVector / max(lightDistance, 0.0001);

					float attenuation = 1.0 / (1.0 + 0.08 * lightDistance * lightDistance);
					attenuation *= clamp(1.0 - lightDistance / max(u_LightRanges[i], 0.0001), 0.0, 1.0);

					float diffuse = max(dot(normal, toLight), 0.0);

					vec3 halfway   = normalize(toLight + toEye);
					float specular = pow(max(dot(normal, halfway), 0.0), 48.0);

					lit += base * diffuse * u_LightColors[i] * attenuation
					     + specular * u_LightColors[i] * attenuation * 0.35;
				}

				color = vec4(lit, u_Color.a);
				entityID = u_EntityID;
			}
		)";

		m_Shader.reset(GS::Shader::Create("EditorSceneView", vertexSrc, fragmentSrc));
		m_SceneMaterial = GS::Material::Create(m_Shader);
	}

	// Scene-wide (base-material) uniforms every submesh inherits: which
	// enabled lights exist, where the camera is (for specular), and the
	// ambient floor. Capped at 8 -- an editor scene has never needed more,
	// and there's no light-culling here to make a higher cap free.
	void UploadLights()
	{
		constexpr int kMaxLights = 8;
		auto& lights = g_EditorScene.View<GS::LightComponent>();

		int count = 0;
		for (size_t i = 0; i < lights.Size() && count < kMaxLights; i++)
		{
			GS::LightComponent& light = lights.Components()[i];
			if (!light.Enabled)
				continue;

			GS::EntityId owner = lights.Owner(i);
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(owner);
			if (!transform)
				continue;

			std::string index = std::to_string(count);
			m_SceneMaterial->Set("u_LightPositions[" + index + "]", transform->Position);
			m_SceneMaterial->Set("u_LightColors[" + index + "]", glm::vec3(light.Color));
			m_SceneMaterial->Set("u_LightRanges[" + index + "]", light.Radius);
			count++;
		}

		m_SceneMaterial->Set("u_LightCount", count);
		m_SceneMaterial->Set("u_CameraPosition", ActiveCamera().GetPosition());
		m_SceneMaterial->Set("u_AmbientStrength", m_AmbientStrength);
	}

	// While Playing, an entity's `CameraComponent::Active` should be what
	// the viewport shows -- otherwise watching a recreated demo means
	// manually flying the editor's own camera into place every time Play
	// starts. Falls back to the free-fly camera whenever nothing qualifies
	// (not Playing, or no active CameraComponent in the scene), which is
	// also what every demo without a camera entity gets today.
	//
	// m_PlayCamera is a separate object rather than repointing m_Camera
	// itself so that Stop() leaves the free-fly camera exactly where the
	// user left it -- it is never written to while an active CameraComponent
	// is driving the view.
	GS::PerspectiveCamera& ActiveCamera()
	{
		if (PlayMode::IsPlaying())
		{
			auto& cameras = g_EditorScene.View<GS::CameraComponent>();
			for (size_t i = 0; i < cameras.Size(); i++)
			{
				GS::CameraComponent& camera = cameras.Components()[i];
				if (!camera.Active)
					continue;

				GS::EntityId owner = cameras.Owner(i);
				auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(owner);
				if (!transform)
					continue;

				glm::vec3 forward = ForwardFromRotation(transform->Rotation);

				m_PlayCamera.SetProjection(camera.Fov, m_Camera.GetAspectRatio(), camera.NearClip, camera.FarClip);
				m_PlayCamera.SetPosition(transform->Position);
				m_PlayCamera.SetOrientation(forward, glm::vec3(0.0f, 1.0f, 0.0f));
				return m_PlayCamera;
			}
		}

		return m_Camera;
	}

	void RenderMeshes()
	{
		GS::Renderer::BeginScene(ActiveCamera());
		UploadLights();

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

	// A CameraComponent has no mesh, so it renders as nothing at all --
	// placed and then invisible. This is the whole fix: one line per camera
	// entity, from its position along the direction it faces, so "which way
	// is this camera looking" is answerable without opening the Inspector.
	void DrawCameraRays()
	{
		auto& cameras = g_EditorScene.View<GS::CameraComponent>();

		for (size_t i = 0; i < cameras.Size(); i++)
		{
			GS::EntityId entity = cameras.Owner(i);
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			if (!transform)
				continue;

			// Rotation only -- translating a direction makes no sense, and
			// baking in Scale would make the ray's length track the entity's
			// scale instead of staying a fixed, readable size.
			glm::vec3 forward = ForwardFromRotation(transform->Rotation);

			glm::vec3 origin = transform->Position;
			glm::vec3 tip = origin + forward * 0.75f;

			glm::vec4 color(0.9f, 0.9f, 0.95f, 1.0f);
			GS::Renderer2D::DrawLine(origin, tip, color);
		}
	}

	// A placed Light entity has no mesh (PlaceLight's own comment: "nothing
	// to resolve through MeshCache"), and until now had no gizmo either --
	// it was genuinely invisible in the viewport once deselected. A small
	// axis-aligned "plus" through its position, tinted by its own colour
	// (dimmed if disabled), is enough to find it -- same reasoning and the
	// same Renderer2D::DrawLine mechanism DrawCameraRays already uses.
	void DrawLightGizmos()
	{
		auto& lights = g_EditorScene.View<GS::LightComponent>();

		for (size_t i = 0; i < lights.Size(); i++)
		{
			GS::LightComponent& light = lights.Components()[i];
			GS::EntityId entity = lights.Owner(i);
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			if (!transform)
				continue;

			glm::vec3 p = transform->Position;
			glm::vec4 color = light.Enabled ? light.Color : glm::vec4(0.4f, 0.4f, 0.4f, 1.0f);
			constexpr float s = 0.15f;
			GS::Renderer2D::DrawLine(p - glm::vec3(s, 0.0f, 0.0f), p + glm::vec3(s, 0.0f, 0.0f), color);
			GS::Renderer2D::DrawLine(p - glm::vec3(0.0f, s, 0.0f), p + glm::vec3(0.0f, s, 0.0f), color);
			GS::Renderer2D::DrawLine(p - glm::vec3(0.0f, 0.0f, s), p + glm::vec3(0.0f, 0.0f, s), color);
		}
	}

	// One small cross per control point of the in-progress edit session, in
	// the selected entity's local-to-world space -- the session itself
	// (m_MeshEditSession) stores points in local space, so every point is
	// transformed here rather than baked once, keeping this correct even if
	// the entity's transform changes while a session is open. The selected
	// point is tinted differently so a click's result is visible immediately.
	void DrawMeshEditOverlay()
	{
		if (!m_MeshEditActive)
			return;

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		glm::mat4 toWorld = transform->GetTransform();

		for (int i = 0; i < m_MeshEditSession.PointCount(); i++)
		{
			glm::vec3 worldPos = glm::vec3(toWorld * glm::vec4(m_MeshEditSession.Point(i).Position, 1.0f));
			glm::vec4 color = (i == m_MeshEditSelectedPoint)
				? glm::vec4(1.0f, 1.0f, 0.4f, 1.0f)
				: glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);

			constexpr float s = 0.03f;
			GS::Renderer2D::DrawLine(worldPos - glm::vec3(s, 0, 0), worldPos + glm::vec3(s, 0, 0), color);
			GS::Renderer2D::DrawLine(worldPos - glm::vec3(0, s, 0), worldPos + glm::vec3(0, s, 0), color);
			GS::Renderer2D::DrawLine(worldPos - glm::vec3(0, 0, s), worldPos + glm::vec3(0, 0, s), color);
		}
	}

	// Before UpdateGizmo(): make the world-space mirror agree with the real,
	// object-space point -- unless a drag is already in progress, in which
	// case UpdateGizmo is the one actively moving the mirror this frame and
	// this must not stomp that with a stale value computed before the drag.
	void SyncMeshEditWorldPointBeforeGizmo()
	{
		if (!m_MeshEditActive || m_MeshEditSelectedPoint < 0 || m_DragAxis >= 0)
			return;

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		glm::vec3 objectSpace = m_MeshEditSession.Point(m_MeshEditSelectedPoint).Position;
		m_MeshEditSessionWorldPoint = glm::vec3(transform->GetTransform() * glm::vec4(objectSpace, 1.0f));
	}

	// After UpdateGizmo(): if it just moved the mirror (a drag was in
	// progress when it ran), write that new world position back into
	// EditableMesh's own object-space storage via the inverse transform.
	void SyncMeshEditWorldPointAfterGizmo()
	{
		if (!m_MeshEditActive || m_MeshEditSelectedPoint < 0 || m_DragAxis < 0)
			return;

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return;

		glm::vec3 newObjectSpace = glm::vec3(glm::inverse(transform->GetTransform()) * glm::vec4(m_MeshEditSessionWorldPoint, 1.0f));
		m_MeshEditSession.MovePoint(m_MeshEditSelectedPoint, newObjectSpace);
		m_MeshEditPreviewDirty = true;
	}

	void RebuildMeshEditPreviewIfNeeded()
	{
		if (!m_MeshEditActive || !m_MeshEditPreviewDirty)
			return;

		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected);
		if (mesh)
			mesh->Geometry.reset(new GS::Mesh(m_MeshEditSession.Rebuild(), "MeshEditPreview"));

		m_MeshEditPreviewDirty = false;
	}

	// Returns null when nothing is selected, which is why every caller checks
	// -- there is no light to fall back to dragging here, unlike Cube3D's own
	// version of this function.
	glm::vec3* GizmoPosition()
	{
		if (m_MeshEditActive && m_MeshEditSelectedPoint >= 0)
			return &m_MeshEditSessionWorldPoint;   // see SyncMeshEditWorldPoint{Before,After}Gizmo -- kept in sync each frame, since EditableMesh stores object space and the gizmo drags in world space

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

	// Nearest edit-session point to a screen-space click, in the same
	// on-screen-pixel-distance terms as the gizmo's own AxisScreenDistance --
	// `const` because, like WorldToScreen, it only reads state (the mouse
	// click itself is handled by Task 9's caller).
	int PickMeshEditPoint(const glm::vec2& mouse) const
	{
		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		if (!transform)
			return -1;

		glm::mat4 toWorld = transform->GetTransform();

		int best = -1;
		float bestDistance = 12.0f;   // pixels -- generous enough to click a small on-screen cross without needing pixel precision
		for (int i = 0; i < m_MeshEditSession.PointCount(); i++)
		{
			glm::vec3 worldPos = glm::vec3(toWorld * glm::vec4(m_MeshEditSession.Point(i).Position, 1.0f));
			glm::vec2 screen;
			if (!WorldToScreen(worldPos, screen))
				continue;

			float distance = glm::length(mouse - screen);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = i;
			}
		}
		return best;
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

		if (m_MeshEditActive && justPressed && m_HoverAxis < 0)
		{
			int picked = PickMeshEditPoint(mouse);
			if (picked >= 0)
			{
				m_MeshEditSession.PushUndo();
				m_MeshEditSelectedPoint = picked;
			}
		}

		if (!down)
		{
			// The drag just ended (m_DragAxis was set) -- push one command
			// for the whole gesture here, not per-frame while dragging, so
			// Undo puts the object back where it was grabbed in a single
			// step rather than replaying every intermediate frame.
			//
			// Excluded while a mesh-edit session is active: GizmoPosition()
			// then returns &m_MeshEditSessionWorldPoint, not &transform->
			// Position, so m_DragStartPosition holds the *point's* world
			// coordinate and transform->Position (the entity's own, untouched
			// origin) is a different value entirely -- comparing them here
			// would push a bogus EditFieldCommand onto the global
			// EditorHistory stack whose "old value" is not any real prior
			// position of this entity. The mesh-edit session has its own
			// undo stack (EditableMesh::PushUndo/Undo) for exactly this
			// gesture; this global one must stay out of it.
			if (m_DragAxis >= 0 && !m_MeshEditActive)
			{
				if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
					if (transform->Position != m_DragStartPosition)
						EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
							m_Selected, &GS::TransformComponent::Position, m_DragStartPosition, transform->Position));
			}

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
	// Reassigned every frame by ActiveCamera() while an active CameraComponent
	// drives the view; its construction values here are never observed.
	GS::PerspectiveCamera m_PlayCamera{ 45.0f, 16.0f / 9.0f, 0.1f, 1000.0f };
	GS::OrthographicCamera m_BlitCamera{ -1.0f, 1.0f, -1.0f, 1.0f };

	std::shared_ptr<GS::Framebuffer> m_Framebuffer;
	std::shared_ptr<GS::Texture2D> m_ColorAttachment;
	unsigned int m_ColorHandle = 0;

	std::shared_ptr<GS::Shader> m_Shader;
	std::shared_ptr<GS::Material> m_SceneMaterial;
	// See BuildShader's comment: higher than Cube3D's 0.10 default because a
	// scene here can have zero placed lights, where Cube3D always has one.
	float m_AmbientStrength = 0.25f;

	FileBrowserPopup m_ScriptBrowser;

	GS::EntityId m_Selected = GS::InvalidEntity;
	GS::EntityId m_Hovered = GS::InvalidEntity;

	bool m_ShowGizmo = true;
	int m_DragAxis = -1;
	// Captured once, on the frame a widget gesture begins (IsItemActivated),
	// and used against the post-gesture value on IsItemDeactivatedAfterEdit
	// -- a per-frame local re-read every frame (the previous approach)
	// always equals the just-changed value by the release frame, since this
	// whole function re-runs every frame regardless of whether a drag is in
	// progress. Only one ImGui item can be "active" at a time, so one member
	// per value type safely covers every field of that type below.
	glm::vec3 m_EditBeforeVec3{ 0.0f };
	glm::vec4 m_EditBeforeVec4{ 0.0f };
	bool m_EditBeforeBool = false;
	float m_EditBeforeFloat = 0.0f;
	std::string m_EditBeforeString;

	bool m_MeshEditActive = false;
	EditableMesh m_MeshEditSession;
	int m_MeshEditSelectedPoint = -1;
	glm::vec3 m_MeshEditSessionWorldPoint{ 0.0f };
	bool m_MeshEditPreviewDirty = false;

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
