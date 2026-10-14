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
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_set>

#include "Demo.h"
#include "EditableMesh.h"
#include "EditorHistory.h"
#include "EditorProject.h"
#include "EntityGroups.h"
#include "FileBrowserPopup.h"
#include "GizmoMath.h"

#include <GS/Procedural/TerrainMesh.h>
#include <type_traits>
#include "MaterialEditorPanel.h"
#include "MaterialLibrary.h"
#include "SceneRenderer.h"
#include "PlayMode.h"

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

	// What the gizmo does: 1, 2, 3 choose, T cycles, and the menu bar shows
	// and sets it. Not W/E/R, the usual editor keys -- WASD and Q/E fly the
	// camera here.
	enum class GizmoMode { Move, Rotate, Scale };
	GizmoMode GetGizmoMode() const { return m_GizmoMode; }
	void SetGizmoMode(GizmoMode mode)
	{
		// Not mid-drag: the gesture would change meaning under the cursor.
		if (m_DragAxis < 0)
			m_GizmoMode = mode;
	}
	bool IsMeshEditing() const { return m_MeshEditActive; }

	void Select(GS::EntityId entity) { m_Selected = entity; }
	GS::EntityId Selected() const { return m_Selected; }

	// Where the Tools panel puts something new: the ground (y = 0) where
	// the camera is looking, when it is looking down at it within 30 m;
	// otherwise 6 m straight ahead.
	glm::vec3 PlacementPoint() const
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 forward = m_Camera.GetForward();
		if (forward.y < -0.05f)
		{
			float distance = -origin.y / forward.y;
			if (distance > 0.0f && distance < 30.0f)
				return origin + forward * distance;
		}
		return origin + forward * 6.0f;
	}
	GS::EntityId GetSelected() const { return m_Selected; }

	void OnUpdate(GS::Timestep ts) override
	{
		if (!IsActive())
			return;

		// First, unconditionally -- this must run even on a frame where the
		// Inspector's own Done/Cancel buttons never get a chance to draw
		// (the edited entity became invalid, or selection moved elsewhere),
		// which is exactly the scenario it exists to catch.
		ValidateMeshEditSession();

		if (g_Viewport.Valid())
			GS::RenderCommand::SetViewport((unsigned int)g_Viewport.X,
				(unsigned int)g_Viewport.Y, (unsigned int)g_Viewport.Width,
				(unsigned int)g_Viewport.Height);

		MoveCamera(ts);
		ResizeTarget();
		SyncTerrainMeshes();
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

		// Overlays are the editor's, not the game's: while playing, the view
		// shows what a player would see -- the same frame GSPlayer draws.
		if (!PlayMode::IsPlaying())
		{
			GS::Renderer2D::BeginScene(ActiveCamera());
			DrawSelectionBox();
			DrawCameraRays();
			DrawLightGizmos();
			DrawMeshEditOverlay();
			GS::Renderer2D::EndScene();

			// The gizmo on top of everything, in a batch of its own with no
			// depth test. Depth-tested, it sat at the selected object's
			// centre *inside* the object, and any object larger than the
			// handles hid it completely -- a capture of a 2 m box showed no
			// gizmo at all, in any mode, the old Move one included.
			if (m_ShowGizmo)
			{
				GS::RenderCommand::SetDepthTest(false);
				GS::Renderer2D::BeginScene(ActiveCamera());
				DrawGizmo();
				GS::Renderer2D::EndScene();
				GS::RenderCommand::SetDepthTest(true);
			}
		}

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
			{
				if (CtrlHeld())
				{
					if (g_EditorScene.IsValid(m_Hovered))
						ToggleMultiSelect(m_Hovered);
				}
				else
				{
					m_MultiSelection.clear();
					m_Selected = m_Hovered;
				}
			}

			// Right-click opens the same context menu the Outliner row does
			// (DrawEntityContextMenu) -- ImGui::OpenPopup can't be called
			// from here directly, since events can arrive outside the ImGui
			// frame; OnImGuiRender opens it next frame from this flag.
			if (e.GetMouseButton() == GS_MOUSE_BUTTON_RIGHT
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0 && !m_MeshEditActive
				&& g_EditorScene.IsValid(m_Hovered))
			{
				m_Selected = m_Hovered;
				m_ViewportContextEntity = m_Hovered;
				m_ViewportContextRequested = true;
			}
			return false;
		});

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;
			if (e.GetKeyCode() == GS_KEY_DELETE && g_EditorScene.IsValid(m_Selected))
				m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(m_Selected));
			else if (e.GetKeyCode() == GS_KEY_G && CtrlHeld() && m_MultiSelection.size() >= 2)
			{
				m_GroupPromptRequested = true;
				m_GroupPromptNeedsConfirm = EntityGroups::AnyAlreadyGrouped(m_MultiSelection);
			}
			// Gizmo modes: not while typing into a field, and not with Ctrl
			// (Ctrl+number is left free for something else).
			else if (!CtrlHeld() && !ImGui::GetIO().WantCaptureKeyboard)
			{
				if (e.GetKeyCode() == GS_KEY_1) SetGizmoMode(GizmoMode::Move);
				else if (e.GetKeyCode() == GS_KEY_2) SetGizmoMode(GizmoMode::Rotate);
				else if (e.GetKeyCode() == GS_KEY_3) SetGizmoMode(GizmoMode::Scale);
				else if (e.GetKeyCode() == GS_KEY_T)
					SetGizmoMode(m_GizmoMode == GizmoMode::Move ? GizmoMode::Rotate
						: m_GizmoMode == GizmoMode::Rotate ? GizmoMode::Scale : GizmoMode::Move);
			}
			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (!IsActive())
			return;

		DrawGizmoDragTooltip();

		// An entity Ctrl-selected earlier can be deleted from under the
		// multi-selection (Delete key, or another session action) --
		// pruned once per frame, the same defensive shape
		// ValidateMeshEditSession already applies to m_MeshEditEntity.
		m_MultiSelection.erase(
			std::remove_if(m_MultiSelection.begin(), m_MultiSelection.end(),
				[](GS::EntityId e) { return !g_EditorScene.IsValid(e); }),
			m_MultiSelection.end());

		// Duplicate/Delete destroy or create entities, which would invalidate
		// the Outliner's range-for below mid-iteration -- GetEntities()
		// returns a reference to Scene's own live-entity vector, not a copy.
		// Both are deferred to after every popup below has had its chance to
		// run this frame (Outliner row and viewport alike), the same way the
		// Inspector's own Delete button already runs in a separate
		// ImGui::Begin block that only starts once the Outliner loop is done.
		GS::EntityId pendingDuplicate = GS::InvalidEntity;
		GS::EntityId pendingDelete = GS::InvalidEntity;

		// The viewport has no ImGui item to hang BeginPopupContextItem off
		// of -- the 3D scene is a full-screen quad blit, not an ImGui::Image
		// -- so a right-click there (OnEvent, above) just raises this flag
		// and OpenPopup happens here, inside the ImGui frame.
		if (m_ViewportContextRequested)
		{
			ImGui::OpenPopup("viewport_entity_context");
			m_ViewportContextRequested = false;
		}
		if (ImGui::BeginPopup("viewport_entity_context"))
		{
			if (g_EditorScene.IsValid(m_ViewportContextEntity))
				DrawEntityContextMenu(m_ViewportContextEntity, pendingDuplicate, pendingDelete);
			else
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (m_GroupPromptRequested)
		{
			ImGui::OpenPopup(m_GroupPromptNeedsConfirm ? "Confirm Regroup" : "New Group");
			if (!m_GroupPromptNeedsConfirm)
				m_GroupNameBuf[0] = '\0';
			m_GroupPromptRequested = false;
		}

		if (ImGui::BeginPopupModal("Confirm Regroup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Some of these entities already belong to a group.");
			ImGui::Text("Move them into a new group?");
			if (ImGui::Button("Move Them"))
			{
				m_GroupNameBuf[0] = '\0';
				ImGui::CloseCurrentPopup();
				ImGui::OpenPopup("New Group");
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("New Group", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_GroupNameBuf, sizeof(m_GroupNameBuf));
			bool canCreate = m_GroupNameBuf[0] != '\0';
			if (!canCreate) ImGui::BeginDisabled();
			if (ImGui::Button("Create"))
			{
				EntityGroups::AssignGroup(m_MultiSelection, m_GroupNameBuf);
				ImGui::CloseCurrentPopup();
			}
			if (!canCreate) ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (m_GroupDialogRequested)
		{
			ImGui::OpenPopup("Manage Groups");
			m_GroupDialogRequested = false;
		}
		if (ImGui::BeginPopupModal("Manage Groups", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			DrawManageGroupsDialog();
			ImGui::Separator();
			if (ImGui::Button("Close"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		ImGui::Begin("Outliner");

		ImGui::Text("Entities: %zu", g_EditorScene.GetEntityCount());
		ImGui::BeginChild("hierarchy", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);

		auto groups = EntityGroups::GroupEntitiesByName();
		std::unordered_set<GS::EntityId> inAnyGroup;
		for (auto& [name, members] : groups)
			for (GS::EntityId member : members)
				inAnyGroup.insert(member);

		for (auto& [name, members] : groups)
		{
			ImGui::PushID(name.c_str());
			bool open = ImGui::TreeNode(name.c_str());

			if (ImGui::BeginPopupContextItem("group_context"))
			{
				if (ImGui::MenuItem("Select All"))
				{
					m_MultiSelection = members;
					if (!members.empty())
						m_Selected = members.back();
				}
				if (ImGui::MenuItem("Ungroup"))
					EntityGroups::DeleteGroup(name);
				ImGui::EndPopup();
			}

			if (open)
			{
				for (GS::EntityId entity : members)
					DrawOutlinerRow(entity, pendingDuplicate, pendingDelete);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (!inAnyGroup.count(entity))
				DrawOutlinerRow(entity, pendingDuplicate, pendingDelete);

		ImGui::EndChild();
		ImGui::End();

		if (g_EditorScene.IsValid(pendingDuplicate))
			m_Selected = EditorHistory::Push(std::make_unique<DuplicateEntityCommand>(pendingDuplicate));
		if (g_EditorScene.IsValid(pendingDelete))
			m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(pendingDelete));

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

			// Red when linked but unavailable, so a flat-coloured mesh that
			// should be textured says why rather than looking merely wrong.
			bool broken = !mesh->MaterialPath.empty() && !MaterialLibrary::Albedo(mesh->MaterialPath);
			if (broken)
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Material: %s (missing or invalid)", mesh->MaterialPath.c_str());
			else
				ImGui::TextDisabled("Material: %s%s", mesh->MaterialPath.empty() ? "(none)" : mesh->MaterialPath.c_str(),
					IsOutsideProject(mesh->MaterialPath) ? "  (outside the project)" : "");
			if (ImGui::Button("Browse...##material"))
				m_MaterialBrowser.Open("Choose Material", FileBrowserPopup::Mode::PickFile,
					g_EditorProjectPath.empty() ? "." : g_EditorProjectPath, ".gsmat");
			if (!mesh->MaterialPath.empty())
			{
				ImGui::SameLine();
				if (ImGui::Button("Edit##material") && g_MaterialEditor)
					g_MaterialEditor->Open(mesh->MaterialPath);
				ImGui::SameLine();
				if (ImGui::Button("Clear##material"))
				{
					std::string old = mesh->MaterialPath;
					mesh->MaterialPath.clear();
					EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, std::string>>(
						m_Selected, &GS::MeshComponent::MaterialPath, old, std::string()));
				}
			}

			if (ImGui::Button("Edit Mesh") && !m_MeshEditActive && !PlayMode::IsPlaying())
				StartMeshEdit(m_Selected, mesh);

			ImGui::SetNextItemWidth(80.0f);
			ImGui::InputInt("##UvExportResolution", &m_UvExportResolution);
			m_UvExportResolution = glm::clamp(m_UvExportResolution, 16, 8192);
			ImGui::SameLine();
			if (ImGui::Button("Export UV Template") && !m_MeshEditActive && !PlayMode::IsPlaying())
			{
				std::string error;
				if (!ExportUvTemplate(m_Selected, mesh, m_UvExportResolution, error))
					GS_ERROR("Export UV Template: failed for '{0}': {1}", mesh->SourcePath, error);
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
					// In the project's meshes/ folder, so the edit travels
					// with the project; re-editing overwrites its own file.
					std::string exportPath = NewAssetPath("meshes", exportName, ".obj", mesh->SourcePath);

					std::string error;
					if (GS::ObjWriter::Save(AssetDiskPath(exportPath), finalData, error))
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

						m_MeshEditActive = false;
						m_MeshEditSelectedPoint = -1;
						m_MeshEditEntity = GS::InvalidEntity;
					}
					else
					{
						// Session deliberately stays open on failure -- closing
						// it here would silently discard the edit with no way
						// to retry and nothing on screen to say anything went
						// wrong. Logged instead; the user can retry Done (e.g.
						// after freeing disk space) or Cancel out.
						GS_ERROR("Edit Mesh: Done failed to save '{0}': {1}", exportPath, error);
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
				{
					auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
					if (transform)
						mesh->Geometry = GS::MeshCache::Get(mesh->SourcePath);   // discard the live preview -- reload the entity's real, unchanged mesh

					m_MeshEditActive = false;
					m_MeshEditSelectedPoint = -1;
					m_MeshEditEntity = GS::InvalidEntity;
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

		DrawTerrainInspector();
		DrawCharacterInspector();

		ImGui::SeparatorText("Network");
		if (auto* identity = g_EditorScene.GetComponent<GS::NetworkIdentity>(m_Selected))
		{
			ImGui::Text("NetworkId: %u%s", identity->NetworkId,
				identity->NetworkId == 0 ? " (not yet assigned by a live server)" : "");
			ImGui::Text("Owner: %s", identity->Owner == GS::ServerOwned ? "Server" : std::to_string(identity->Owner).c_str());

			if (auto* networkTransform = g_EditorScene.GetComponent<GS::NetworkTransform>(m_Selected))
			{
				ImGui::DragFloat("Send Rate (Hz)", &networkTransform->SendRate, 1.0f, 1.0f, 60.0f);
				if (ImGui::IsItemActivated())
					m_EditBeforeFloat = networkTransform->SendRate;
				if (ImGui::IsItemDeactivatedAfterEdit())
					EditorHistory::Push(std::make_unique<EditFieldCommand<GS::NetworkTransform, float>>(
						m_Selected, &GS::NetworkTransform::SendRate, m_EditBeforeFloat, networkTransform->SendRate));

				ImGui::Checkbox("Interpolate", &networkTransform->Interpolate);
				if (ImGui::IsItemActivated())
					m_EditBeforeBool = networkTransform->Interpolate;
				if (ImGui::IsItemDeactivatedAfterEdit())
					EditorHistory::Push(std::make_unique<EditFieldCommand<GS::NetworkTransform, bool>>(
						m_Selected, &GS::NetworkTransform::Interpolate, m_EditBeforeBool, networkTransform->Interpolate));
			}

			if (ImGui::Button("Remove Network"))
			{
				g_EditorScene.RemoveComponent<GS::NetworkTransform>(m_Selected);
				g_EditorScene.RemoveComponent<GS::NetworkIdentity>(m_Selected);
			}
		}
		else
		{
			if (ImGui::Button("Add Network"))
			{
				g_EditorScene.AddComponent<GS::NetworkIdentity>(m_Selected, GS::NetworkIdentity{});
				g_EditorScene.AddComponent<GS::NetworkTransform>(m_Selected, GS::NetworkTransform{});
			}
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
			// Rooted at the open project's folder; the pick is stored
			// relative to it (StoreAssetPath), which is how the runtime
			// resolves it.
			if (ImGui::Button("Browse...##script"))
				m_ScriptBrowser.Open("Choose Script", FileBrowserPopup::Mode::PickFile,
					g_EditorProjectPath.empty() ? "." : g_EditorProjectPath, ".gss");
			if (IsOutsideProject(script->ScriptPath))
				ImGui::TextDisabled("Outside the project -- it will not move with it.");
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
				script->ScriptPath = StoreAssetPath(picked);
			}
		}
		m_ScriptBrowser.Draw();

		// Relative to the project when inside it, absolute otherwise -- the
		// form GS::Assets resolves in the editor and the player alike.
		if (m_MaterialBrowser.HasResult())
		{
			std::string picked = m_MaterialBrowser.TakeResult();
			if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected))
			{
				std::string old = mesh->MaterialPath;
				mesh->MaterialPath = StoreAssetPath(picked);
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, std::string>>(
					m_Selected, &GS::MeshComponent::MaterialPath, old, mesh->MaterialPath));
			}
		}
		m_MaterialBrowser.Draw();

		if (ImGui::Button("Delete"))
			m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(m_Selected));

		ImGui::End();
	}

private:
	// Shared by the Outliner row's BeginPopupContextItem and the viewport's
	// plain BeginPopup (right-clicking a 3D object) -- both just need this
	// run once they've confirmed their popup is open, on whichever entity
	// they're targeting. Duplicate/Delete are written into the caller's
	// per-frame locals rather than acted on immediately: both destroy/create
	// entities, and the Outliner call site is still mid-range-for over
	// Scene's live-entity vector when this runs.
	static bool CtrlHeld()
	{
		return GS::Input::IsKeyPressed(GS_KEY_LEFT_CONTROL) || GS::Input::IsKeyPressed(GS_KEY_RIGHT_CONTROL);
	}

	// Toggling rather than always-add is what makes Ctrl+click a real
	// multi-select (click a selected member again to drop it), not just an
	// accumulator. m_Selected always follows the last entity touched, so
	// the Inspector keeps showing something sensible even with several
	// entities in m_MultiSelection.
	void ToggleMultiSelect(GS::EntityId entity)
	{
		auto it = std::find(m_MultiSelection.begin(), m_MultiSelection.end(), entity);
		if (it != m_MultiSelection.end())
			m_MultiSelection.erase(it);
		else
			m_MultiSelection.push_back(entity);
		m_Selected = entity;
	}

	// One Outliner row -- extracted so both the grouped and ungrouped render
	// passes call the same code.
	void DrawOutlinerRow(GS::EntityId entity, GS::EntityId& pendingDuplicate, GS::EntityId& pendingDelete)
	{
		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
		if (!tag)
			return;

		ImGui::PushID((int)entity);
		ImGui::BeginDisabled(m_MeshEditActive);

		bool isSelected = m_MultiSelection.empty()
			? (entity == m_Selected)
			: (std::find(m_MultiSelection.begin(), m_MultiSelection.end(), entity) != m_MultiSelection.end());

		if (ImGui::Selectable(tag->Name.c_str(), isSelected))
		{
			if (CtrlHeld())
				ToggleMultiSelect(entity);
			else
			{
				m_MultiSelection.clear();
				m_Selected = entity;
			}
		}

		if (ImGui::BeginPopupContextItem("row_context"))
		{
			DrawEntityContextMenu(entity, pendingDuplicate, pendingDelete);
			ImGui::EndPopup();
		}

		ImGui::EndDisabled();
		ImGui::PopID();
	}

	// Opened via "Add to Group" (DrawEntityContextMenu) -- lists and acts on
	// every group in the scene, not just the entity that opened it, per the
	// "full management" scope from the design spec. Every action here runs
	// immediately with no confirmation: this dialog is already an explicit,
	// deliberate action a user opened on purpose, unlike Ctrl+G's fast
	// shortcut, which is why that one gets a confirm step and this doesn't.
	void DrawManageGroupsDialog()
	{
		auto groups = EntityGroups::GroupEntitiesByName();

		for (auto& [name, members] : groups)
		{
			ImGui::PushID(name.c_str());
			if (ImGui::CollapsingHeader(name.c_str()))
			{
				if (m_RenamingGroup == name)
				{
					ImGui::SetNextItemWidth(150.0f);
					if (ImGui::InputText("##rename_group", m_GroupRenameBuf, sizeof(m_GroupRenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
					{
						EntityGroups::RenameGroup(name, m_GroupRenameBuf);
						m_RenamingGroup.clear();
					}
				}
				else if (ImGui::Button("Rename"))
				{
					m_RenamingGroup = name;
					strncpy(m_GroupRenameBuf, name.c_str(), sizeof(m_GroupRenameBuf) - 1);
					m_GroupRenameBuf[sizeof(m_GroupRenameBuf) - 1] = '\0';
				}
				ImGui::SameLine();
				if (ImGui::Button("Delete Group"))
					EntityGroups::DeleteGroup(name);

				for (GS::EntityId member : members)
				{
					auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(member);
					ImGui::PushID((int)member);
					ImGui::BulletText("%s", tag ? tag->Name.c_str() : "?");
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove"))
						g_EditorScene.RemoveComponent<GS::GroupComponent>(member);
					ImGui::PopID();
				}

				ImGui::SetNextItemWidth(200.0f);
				if (ImGui::BeginCombo("##add_to_group", "Add entity..."))
				{
					for (GS::EntityId candidate : g_EditorScene.GetEntities())
					{
						if (std::find(members.begin(), members.end(), candidate) != members.end())
							continue;
						auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(candidate);
						if (!tag)
							continue;
						if (ImGui::Selectable(tag->Name.c_str()))
							g_EditorScene.AddComponent<GS::GroupComponent>(candidate, GS::GroupComponent{ name });
					}
					ImGui::EndCombo();
				}
			}
			ImGui::PopID();
		}

		ImGui::Separator();
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("##new_group_name", m_NewGroupBuf, sizeof(m_NewGroupBuf));
		ImGui::SameLine();
		bool canCreate = m_NewGroupBuf[0] != '\0' && g_EditorScene.IsValid(m_GroupDialogEntity);
		if (!canCreate) ImGui::BeginDisabled();
		if (ImGui::Button("Create New Group"))
		{
			g_EditorScene.AddComponent<GS::GroupComponent>(m_GroupDialogEntity, GS::GroupComponent{ m_NewGroupBuf });
			m_NewGroupBuf[0] = '\0';
		}
		if (!canCreate) ImGui::EndDisabled();
	}

	void DrawEntityContextMenu(GS::EntityId entity, GS::EntityId& pendingDuplicate, GS::EntityId& pendingDelete)
	{
		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
		if (!tag)
			return;

		// A right click (viewport or Outliner row) also selects, so Rename/
		// Edit Mesh below -- and the Inspector, drawn after both -- all agree
		// on which entity the menu is acting on.
		if (ImGui::IsWindowAppearing())
		{
			m_Selected = entity;
			strncpy(m_RenameBuf, tag->Name.c_str(), sizeof(m_RenameBuf) - 1);
			m_RenameBuf[sizeof(m_RenameBuf) - 1] = '\0';
		}

		auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(entity);
		if (!script)
		{
			if (ImGui::MenuItem("Add Script"))
				g_EditorScene.AddComponent<GS::ScriptComponent>(entity, GS::ScriptComponent{});
		}
		else if (ImGui::MenuItem("Remove Script"))
			g_EditorScene.RemoveComponent<GS::ScriptComponent>(entity);

		ImGui::Separator();
		ImGui::SetNextItemWidth(150.0f);
		if (ImGui::InputText("##rename_ctx", m_RenameBuf, sizeof(m_RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TagComponent, std::string>>(
				entity, &GS::TagComponent::Name, tag->Name, std::string(m_RenameBuf)));
			ImGui::CloseCurrentPopup();
		}

		ImGui::Separator();
		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
		if (ImGui::MenuItem("Edit Mesh", nullptr, false, mesh && !m_MeshEditActive && !PlayMode::IsPlaying()))
			StartMeshEdit(entity, mesh);
		if (ImGui::MenuItem("Duplicate"))
			pendingDuplicate = entity;
		if (ImGui::MenuItem("Delete"))
			pendingDelete = entity;
		if (ImGui::MenuItem("Add to Group"))
		{
			m_GroupDialogEntity = entity;
			m_GroupDialogRequested = true;
		}

		ImGui::Separator();
		// Deliberately just Physics for now -- a reserved home for later,
		// more technical per-entity data (see editor_roadmap.md item 1)
		// rather than a real settled set of categories, hence a tab bar
		// with one tab instead of a flat list.
		if (ImGui::BeginMenu("Advanced"))
		{
			if (ImGui::BeginTabBar("AdvancedTabs"))
			{
				if (ImGui::BeginTabItem("Physics"))
				{
					auto* physics = g_EditorScene.GetComponent<GS::PhysicsComponent>(entity);
					if (!physics)
					{
						if (ImGui::MenuItem("Add Physics"))
							g_EditorScene.AddComponent<GS::PhysicsComponent>(entity, GS::PhysicsComponent{});
					}
					else
					{
						const char* bodyTypes[] = { "Static", "Dynamic", "Kinematic" };
						int typeIdx = (int)physics->Type;
						ImGui::SetNextItemWidth(120.0f);
						if (ImGui::Combo("Body Type", &typeIdx, bodyTypes, 3))
							EditorHistory::Push(std::make_unique<EditFieldCommand<GS::PhysicsComponent, GS::BodyType>>(
								entity, &GS::PhysicsComponent::Type, physics->Type, (GS::BodyType)typeIdx));

						ImGui::SetNextItemWidth(120.0f);
						ImGui::DragFloat("Mass", &physics->Mass, 0.1f, 0.0f, 1000.0f);
						if (ImGui::IsItemActivated())
							m_EditBeforeFloat = physics->Mass;
						if (ImGui::IsItemDeactivatedAfterEdit())
							EditorHistory::Push(std::make_unique<EditFieldCommand<GS::PhysicsComponent, float>>(
								entity, &GS::PhysicsComponent::Mass, m_EditBeforeFloat, physics->Mass));

						ImGui::SetNextItemWidth(120.0f);
						ImGui::DragFloat("Friction", &physics->Friction, 0.01f, 0.0f, 1.0f);
						if (ImGui::IsItemActivated())
							m_EditBeforeFloat = physics->Friction;
						if (ImGui::IsItemDeactivatedAfterEdit())
							EditorHistory::Push(std::make_unique<EditFieldCommand<GS::PhysicsComponent, float>>(
								entity, &GS::PhysicsComponent::Friction, m_EditBeforeFloat, physics->Friction));

						ImGui::SetNextItemWidth(120.0f);
						ImGui::DragFloat("Restitution", &physics->Restitution, 0.01f, 0.0f, 1.0f);
						if (ImGui::IsItemActivated())
							m_EditBeforeFloat = physics->Restitution;
						if (ImGui::IsItemDeactivatedAfterEdit())
							EditorHistory::Push(std::make_unique<EditFieldCommand<GS::PhysicsComponent, float>>(
								entity, &GS::PhysicsComponent::Restitution, m_EditBeforeFloat, physics->Restitution));

						ImGui::Separator();
						if (ImGui::MenuItem("Remove Physics"))
							g_EditorScene.RemoveComponent<GS::PhysicsComponent>(entity);
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::EndMenu();
		}
	}

	// Shared by StartMeshEdit and ExportUvTemplate -- both need the same
	// primitive-vs-file loading branch for a MeshComponent's current
	// geometry.
	// A terrain's mesh is generated, not loaded: rebuilt whenever the
	// numbers it was built from (TerrainComponent::BuiltKey) are not the
	// numbers it has -- after an Inspector edit, an Undo or Redo of one, or
	// a Duplicate. Comparing a key a frame is far cheaper than rebuilding,
	// and it is what keeps every path that changes a field honest without
	// each having to remember. A shape terrain does not apply to (Edit
	// Mesh's baked .obj) keeps the geometry it has.
	void SyncTerrainMeshes()
	{
		auto& terrains = g_EditorScene.View<GS::TerrainComponent>();
		for (size_t i = 0; i < terrains.Size(); i++)
		{
			GS::TerrainComponent& terrain = terrains.Components()[i];
			auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(terrains.Owner(i));
			if (!mesh || !GS::TerrainMesh::Supports(mesh->SourcePath))
				continue;
			std::string key = GS::TerrainMesh::Key(mesh->SourcePath, terrain);
			if (key == terrain.BuiltKey)
				continue;
			mesh->Geometry = GS::TerrainMesh::Get(mesh->SourcePath, terrain);
			terrain.BuiltKey = key;
		}
	}

	// The "before" of an undoable Inspector edit, by the field's type.
	template<typename Field>
	Field& EditBefore()
	{
		if constexpr (std::is_same_v<Field, float>) return m_EditBeforeFloat;
		else if constexpr (std::is_same_v<Field, int>) return m_EditBeforeInt;
		else return m_EditBeforeBool;
	}

	// After a widget: one EditFieldCommand per completed edit, the same
	// IsItemActivated / IsItemDeactivatedAfterEdit pair every field above
	// spells out by hand.
	template<typename Component, typename Field>
	void TrackEdit(Field Component::*member, Field& value)
	{
		if (ImGui::IsItemActivated())
			EditBefore<Field>() = value;
		if (ImGui::IsItemDeactivatedAfterEdit())
			EditorHistory::Push(std::make_unique<EditFieldCommand<Component, Field>>(
				m_Selected, member, EditBefore<Field>(), value));
	}

	// Adding or removing a component as one undo step: the entity as it is,
	// then as it will be.
	template<typename Edit>
	void ReplaceSelected(Edit edit)
	{
		EntitySnapshot before = EntitySnapshot::Capture(m_Selected);
		EntitySnapshot after = before;
		edit(after);
		EditorHistory::Push(std::make_unique<ReplaceEntityCommand>(m_Selected, before, after));
	}

	void DrawTerrainInspector()
	{
		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected);
		if (!mesh || !GS::TerrainMesh::Supports(mesh->SourcePath))
			return;

		ImGui::SeparatorText("Terrain");
		auto* terrain = g_EditorScene.GetComponent<GS::TerrainComponent>(m_Selected);
		if (!terrain)
		{
			if (ImGui::Button("Add Terrain"))
				ReplaceSelected([&](EntitySnapshot& s)
				{
					GS::TerrainComponent t;
					t.Resolution = mesh->SourcePath == "primitive:plane" ? 128 : 48;
					s.Terrain = t;
				});
			ImGui::SameLine();
			ImGui::TextDisabled("Noise relief on this shape, at a vertex count you choose.");
			return;
		}

		ImGui::SliderInt("Resolution", &terrain->Resolution, 1, 512);
		TrackEdit(&GS::TerrainComponent::Resolution, terrain->Resolution);
		if (mesh->Geometry)
			ImGui::TextDisabled("%zu vertices, %zu triangles", mesh->Geometry->GetVertexCount(), mesh->Geometry->GetTriangleCount());
		ImGui::DragFloat("Height", &terrain->Height, 0.002f, -2.0f, 2.0f, "%.3f");
		TrackEdit(&GS::TerrainComponent::Height, terrain->Height);
		ImGui::DragFloat("Feature scale", &terrain->Frequency, 0.05f, 0.1f, 64.0f, "%.2f");
		TrackEdit(&GS::TerrainComponent::Frequency, terrain->Frequency);
		ImGui::SliderInt("Detail layers", &terrain->Octaves, 1, 10);
		TrackEdit(&GS::TerrainComponent::Octaves, terrain->Octaves);
		ImGui::SliderFloat("Roughness", &terrain->Roughness, 0.0f, 1.0f, "%.2f");
		TrackEdit(&GS::TerrainComponent::Roughness, terrain->Roughness);
		ImGui::DragInt("Seed", &terrain->Seed, 1.0f);
		TrackEdit(&GS::TerrainComponent::Seed, terrain->Seed);
		ImGui::Checkbox("Ridged", &terrain->Ridged);
		TrackEdit(&GS::TerrainComponent::Ridged, terrain->Ridged);

		// Whether it will be walkable, said where it is decided.
		if (auto* physics = g_EditorScene.GetComponent<GS::PhysicsComponent>(m_Selected))
		{
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
			bool heightfield = mesh->SourcePath == "primitive:plane" && physics->Type == GS::BodyType::Static
				&& transform && transform->Rotation == glm::vec3(0.0f)
				&& std::abs(transform->Scale.x - transform->Scale.z) <= 1e-4f * std::max(1.0f, transform->Scale.x);
			ImGui::TextDisabled(heightfield ? "Collides as its surface (a heightfield)."
				: "Collides as its bounding box -- only a static, unrotated plane with equal x and z scale collides as its surface.");
		}

		if (ImGui::Button("Remove Terrain"))
			ReplaceSelected([&](EntitySnapshot& s)
			{
				s.Terrain.reset();
				s.Mesh->Geometry = GS::MeshCache::Get(s.Mesh->SourcePath);
			});
	}

	template<typename Component>
	void BodyCombo(int& body, const char* id)
	{
		static const char* kinds[] = { "Upright capsule (walks)", "Ball (rolls)", "Humanoid (active ragdoll)" };
		static const char* tips[] = {
			"Moves at a set speed, never tips over. Uses the entity's own mesh to look like.",
			"A real sphere driven by torque: momentum, rolling down slopes, spinning as it goes.",
			"The ragdoll from the Ragdoll demo: balances, walks, staggers, falls, gets up. Drawn as a mannequin." };
		int value = body;
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::Combo((std::string("Body") + id).c_str(), &value, kinds, 3) && value != body)
			EditorHistory::Push(std::make_unique<EditFieldCommand<Component, int>>(m_Selected, &Component::Body, body, value));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tips[std::clamp(body, 0, 2)]);
	}

	void DrawCharacterInspector()
	{
		auto* player = g_EditorScene.GetComponent<GS::PlayerComponent>(m_Selected);
		auto* npc = g_EditorScene.GetComponent<GS::NpcComponent>(m_Selected);
		if (!player && !npc)
		{
			if (!g_EditorScene.HasComponent<GS::MeshComponent>(m_Selected))
				return;
			ImGui::SeparatorText("Character");
			// A sphere is made a ball -- it rolls; anything else walks
			// upright, keeping its own shape.
			auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected);
			int body = mesh->SourcePath == "primitive:sphere" ? 1 : 0;
			if (ImGui::Button("Make Player"))
				ReplaceSelected([body](EntitySnapshot& s) { GS::PlayerComponent p; p.Body = body; s.Player = p; });
			ImGui::SameLine();
			if (ImGui::Button("Make NPC"))
				ReplaceSelected([body](EntitySnapshot& s) { GS::NpcComponent n; n.Body = body; s.Npc = n; });
			return;
		}

		if (player)
		{
			ImGui::SeparatorText("Player");
			ImGui::TextDisabled("WASD move, Space jump, Shift sprint; right-drag or arrows orbit the camera.");
			BodyCombo<GS::PlayerComponent>(player->Body, "##playerbody");
			ImGui::DragFloat("Speed (m/s)", &player->Speed, 0.05f, 0.0f, 50.0f, "%.2f");
			TrackEdit(&GS::PlayerComponent::Speed, player->Speed);
			ImGui::DragFloat("Sprint x", &player->SprintMultiplier, 0.01f, 1.0f, 5.0f, "%.2f");
			TrackEdit(&GS::PlayerComponent::SprintMultiplier, player->SprintMultiplier);
			ImGui::DragFloat("Jump height (m)", &player->JumpHeight, 0.02f, 0.0f, 20.0f, "%.2f");
			TrackEdit(&GS::PlayerComponent::JumpHeight, player->JumpHeight);
			static const char* modes[] = { "Third person", "First person", "Scene camera (no follow)" };
			int mode = player->CameraMode;
			if (ImGui::Combo("Camera", &mode, modes, 3) && mode != player->CameraMode)
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::PlayerComponent, int>>(
					m_Selected, &GS::PlayerComponent::CameraMode, player->CameraMode, mode));
			if (player->CameraMode == 0)
			{
				ImGui::DragFloat("Camera distance", &player->CameraDistance, 0.05f, 0.5f, 100.0f, "%.2f");
				TrackEdit(&GS::PlayerComponent::CameraDistance, player->CameraDistance);
			}
			if (player->CameraMode != 2)
			{
				ImGui::DragFloat("Camera height", &player->CameraHeight, 0.02f, -5.0f, 20.0f, "%.2f");
				TrackEdit(&GS::PlayerComponent::CameraHeight, player->CameraHeight);
			}
			if (ImGui::Button("Remove Player"))
				ReplaceSelected([](EntitySnapshot& s) { s.Player.reset(); });
		}

		if (npc)
		{
			ImGui::SeparatorText("NPC");
			BodyCombo<GS::NpcComponent>(npc->Body, "##npcbody");
			static const char* behaviours[] = { "Idle", "Wander", "Follow the player", "Flee the player" };
			int behaviour = npc->Behaviour;
			if (ImGui::Combo("Behaviour", &behaviour, behaviours, 4) && behaviour != npc->Behaviour)
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::NpcComponent, int>>(
					m_Selected, &GS::NpcComponent::Behaviour, npc->Behaviour, behaviour));
			ImGui::DragFloat("Speed (m/s)##npc", &npc->Speed, 0.05f, 0.0f, 50.0f, "%.2f");
			TrackEdit(&GS::NpcComponent::Speed, npc->Speed);
			if (npc->Behaviour == 1)
			{
				ImGui::DragFloat("Wander radius", &npc->WanderRadius, 0.1f, 0.0f, 500.0f, "%.1f");
				TrackEdit(&GS::NpcComponent::WanderRadius, npc->WanderRadius);
			}
			if (npc->Behaviour >= 2)
			{
				ImGui::DragFloat("Notices within", &npc->DetectRadius, 0.1f, 0.0f, 500.0f, "%.1f");
				TrackEdit(&GS::NpcComponent::DetectRadius, npc->DetectRadius);
			}
			if (npc->Behaviour == 2)
			{
				ImGui::DragFloat("Stops at", &npc->StopDistance, 0.05f, 0.0f, 100.0f, "%.2f");
				TrackEdit(&GS::NpcComponent::StopDistance, npc->StopDistance);
			}
			if (ImGui::Button("Remove NPC"))
				ReplaceSelected([](EntitySnapshot& s) { s.Npc.reset(); });
		}
	}

	bool LoadMeshDataForExport(GS::MeshComponent* mesh, GS::MeshData& outData, std::string& error)
	{
		// A terrain's surface is the generated one: what is edited, or
		// unwrapped, is what is seen -- baked into the .obj this writes.
		if (auto* terrain = g_EditorScene.GetComponent<GS::TerrainComponent>(m_Selected);
			terrain && g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected) == mesh
			&& GS::TerrainMesh::Supports(mesh->SourcePath))
		{
			outData = GS::TerrainMesh::BuildData(mesh->SourcePath, *terrain);
			return true;
		}
		if (mesh->SourcePath.rfind("primitive:cube", 0) == 0)
			outData = GS::Mesh::CreateCubeData();
		else if (mesh->SourcePath.rfind("primitive:plane", 0) == 0)
			outData = GS::Mesh::CreatePlaneData();
		else if (mesh->SourcePath.rfind("primitive:sphere", 0) == 0)
			outData = GS::Mesh::CreateSphereData();
		else if (mesh->SourcePath.rfind("primitive:cylinder", 0) == 0)
			outData = GS::Mesh::CreateCylinderData();
		else if (mesh->SourcePath.rfind("primitive:capsule", 0) == 0)
			outData = GS::Mesh::CreateCapsuleData();
		else
			return GS::Mesh::LoadData(GS::Assets::Resolve(mesh->SourcePath), outData, error);
		return true;
	}

	// Shared by the Inspector's "Edit Mesh" button and the Outliner's
	// context-menu equivalent -- both need the same load-and-open logic on
	// whatever entity/mesh they were given, not necessarily m_Selected in
	// the context-menu case (right-clicking a row selects it first, but the
	// order that happens in relative to this call shouldn't matter).
	void StartMeshEdit(GS::EntityId entity, GS::MeshComponent* mesh)
	{
		GS::MeshData data;
		std::string error;
		bool loaded = LoadMeshDataForExport(mesh, data, error);
		if (!loaded)
			GS_ERROR("Edit Mesh: could not load '{0}': {1}", mesh->SourcePath, error);

		// A session with nothing in it has nothing to pick, drag, or
		// delete -- the only way out would be Done, which would then
		// write an empty .obj and replace this entity's real Geometry
		// with it. Better to leave the entity untouched and log why.
		if (loaded && !data.Vertices.empty())
		{
			m_MeshEditEntity = entity;
			m_MeshEditSession = EditableMesh::FromMeshData(data);
			m_MeshEditSelectedPoint = -1;
			m_MeshEditActive = true;
		}
	}

	// Shared by the Inspector's "Export UV Template" button and the
	// self-test below -- factored out so its branch/save/write logic can
	// be exercised without a live ImGui frame, the same reason
	// LoadMeshDataForExport itself was factored out above.
	bool ExportUvTemplate(GS::EntityId entity, GS::MeshComponent* mesh, int resolution, std::string& error)
	{
		GS::MeshData data;
		if (!LoadMeshDataForExport(mesh, data, error))
			return false;
		if (data.Vertices.empty())
		{
			error = "mesh has no vertices";
			return false;
		}

		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
		std::string exportName = tag ? tag->Name : "Mesh";

		GS::UvUnwrap::ChartAssignment chartIds;
		if (!GS::UvUnwrap::HasUsableUVs(data))
		{
			chartIds = GS::UvUnwrap::Unwrap(data);

			// Same collision-suffixed path Done (Edit Mesh) already
			// computes -- deliberately identical, not re-derived.
			std::string exportPath = NewAssetPath("meshes", exportName, ".obj", mesh->SourcePath);

			if (!GS::ObjWriter::Save(AssetDiskPath(exportPath), data, error))
				return false;

			std::string oldPath = mesh->SourcePath;
			mesh->SourcePath = exportPath;
			// Bypasses MeshCache -- same reason Edit Mesh's own Done does
			// (see its comment at this file's Done handler): MeshCache
			// caches one mesh per path forever, so re-exporting the same
			// path twice would otherwise show the first export's stale
			// geometry.
			mesh->Geometry = std::make_shared<GS::Mesh>(data, exportName);
			EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, std::string>>(
				entity, &GS::MeshComponent::SourcePath, oldPath, exportPath));
		}
		else
		{
			chartIds = GS::UvUnwrap::FindIslandsFromUVs(data);
		}

		return GS::UvTemplateWriter::Write(AssetDiskPath(NewAssetPath("textures", exportName + "_uv", ".png")),
			resolution, data, chartIds, error);
	}

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
		// Anti-aliased like the window the demos draw into: without it this
		// target -- and so the whole editor view -- had hard, stepped edges
		// the demos never showed.
		m_Antialiasing = EditorAntialiasing();
		spec.Samples = m_Antialiasing;

		m_Framebuffer.reset(GS::Framebuffer::Create(spec));
		m_BlitCamera.SetProjection(-1.0f, 1.0f, -1.0f, 1.0f);
	}

	void BuildShader()
	{
		m_SceneRenderer.Init();
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
		if (PlayMode::IsPlaying()
			&& SceneRenderer::FindActiveCamera(g_EditorScene, m_Camera.GetAspectRatio(), m_PlayCamera))
			return m_PlayCamera;
		return m_Camera;
	}

	void RenderMeshes()
	{
		m_SceneRenderer.Render(g_EditorScene, ActiveCamera());
	}

	void ResizeTarget()
	{
		GS::Window& window = GS::Application::Get().GetWindow();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		if (window.GetWidth() > 0 && window.GetHeight() > 0 &&
			(spec.Width != window.GetWidth() || spec.Height != window.GetHeight()))
			m_Framebuffer->Resize(window.GetWidth(), window.GetHeight());

		// The project's setting changes (View > Anti-aliasing, or another
		// project opening): rebuilt at the new level.
		unsigned int antialiasing = EditorAntialiasing();
		if (antialiasing != m_Antialiasing)
		{
			m_Antialiasing = antialiasing;
			m_Framebuffer->SetSamples(antialiasing);
		}
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
		auto drawBoxFor = [this](GS::EntityId entity, const glm::vec4& color)
		{
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
			if (!transform || !mesh || !mesh->Geometry)
				return;

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
		};

		const glm::vec4 hoverColor(0.45f, 0.85f, 1.0f, 1.0f);
		const glm::vec4 selectedColor(1.00f, 0.85f, 0.3f, 1.0f);

		if (g_EditorScene.IsValid(m_Hovered) && m_Hovered != m_Selected)
			drawBoxFor(m_Hovered, hoverColor);

		if (m_MultiSelection.size() > 1)
		{
			for (GS::EntityId entity : m_MultiSelection)
				drawBoxFor(entity, selectedColor);
		}
		else if (g_EditorScene.IsValid(m_Selected))
			drawBoxFor(m_Selected, selectedColor);
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

	// A placed Light entity has no mesh (ToolsPanel::PlaceLight: "nothing
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

	// Runs every frame regardless of what the Inspector draws this frame --
	// unlike the Done/Cancel buttons, this cannot be starved by the entity
	// becoming invalid (which is exactly the scenario it exists to catch).
	// Two ways a session can go stale without ever reaching Done/Cancel:
	// the edited entity is deleted (Delete key, Inspector "Delete", an Undo
	// past its own placement) while m_Selected still names it -- or m_Selected
	// is reassigned out from under the session by one of Select()'s several
	// other call sites (the Place* functions in ToolsPanel.h,
	// Undo/Redo and "Find Entity" in EditorMenuBar.h) while the entity being
	// edited is still perfectly valid. Either way, m_MeshEditSession no
	// longer corresponds to what m_Selected/the Outliner/the viewport click
	// guard now mean by "the active entity", so leaving m_MeshEditActive true
	// would either lock the editor out of ever selecting anything again (the
	// Outliner and viewport-click guards both key off that one flag) or let
	// RebuildMeshEditPreviewIfNeeded silently overwrite an unrelated entity's
	// Geometry with the old session's data.
	void ValidateMeshEditSession()
	{
		if (!m_MeshEditActive)
			return;

		bool entityGone = !g_EditorScene.IsValid(m_MeshEditEntity);
		bool selectionMovedAway = m_Selected != m_MeshEditEntity;

		if (!entityGone && !selectionMovedAway)
			return;

		// Best-effort restore: if the entity we were editing still exists
		// (just not what's selected anymore), put its real, on-disk mesh
		// back rather than leaving it showing the abandoned live preview --
		// same intent as the Cancel button, just targeting the tracked
		// entity instead of whatever m_Selected happens to be now.
		if (!entityGone)
		{
			if (auto* editedMesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_MeshEditEntity))
				editedMesh->Geometry = GS::MeshCache::Get(editedMesh->SourcePath);
		}

		m_MeshEditActive = false;
		m_MeshEditSelectedPoint = -1;
		m_MeshEditEntity = GS::InvalidEntity;
	}

	// Returns null when nothing is selected, which is why every caller checks
	// -- there is no light to fall back to dragging here, unlike Cube3D's own
	// version of this function.
	glm::vec3* GizmoPosition()
	{
		if (m_MeshEditActive && m_MeshEditSelectedPoint >= 0)
			return &m_MeshEditSessionWorldPoint;   // see SyncMeshEditWorldPoint{Before,After}Gizmo -- kept in sync each frame, since EditableMesh stores object space and the gizmo drags in world space

		if (m_MultiSelection.size() > 1)
		{
			std::vector<glm::vec3> positions;
			for (GS::EntityId entity : m_MultiSelection)
				if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
					positions.push_back(transform->Position);
			m_MultiSelectionPivot = EntityGroups::ComputeCentroid(positions);
			return &m_MultiSelectionPivot;
		}

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
	// on-screen-pixel-distance terms as the gizmo's own HandleScreenDistance --
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

	// --- The gizmo -----------------------------------------------------------
	// One set of handles, three modes (1/2/3, T cycles; the menu bar shows
	// which): Move drags along world axes, Rotate turns about them, Scale
	// stretches along the entity's own axes or, from the centre, uniformly.
	// Hold Ctrl to snap rotation to 15 degrees and scale to steps of 0.1.
	// A mesh-edit session moves single points, so it is Move only.
	//
	// Handle ids: 0, 1, 2 are the X, Y, Z axes; 3 is Scale's centre.

	GizmoMode EffectiveGizmoMode() const { return m_MeshEditActive ? GizmoMode::Move : m_GizmoMode; }

	// A fixed share of the distance to the camera, so the gizmo is the same
	// size on screen near or far. It was a fixed metre: a speck at fifty
	// metres, and wider than the view at half a metre.
	float GizmoLength()
	{
		glm::vec3* origin = GizmoPosition();
		if (!origin)
			return m_GizmoLength;
		return std::max(0.05f, glm::length(*origin - m_Camera.GetPosition()) * m_GizmoScreenShare);
	}

	static glm::mat3 RotationFromEuler(const glm::vec3& degrees) { return GizmoMath::RotationFromEuler(degrees); }

	// Which way a handle points in the world: the world axis, except Scale
	// on one entity, whose handles follow its own (rotated) axes -- scale is
	// applied before rotation, so that is the direction it stretches.
	glm::vec3 GizmoAxis(int axis)
	{
		glm::vec3 direction(0.0f);
		direction[axis] = 1.0f;
		if (EffectiveGizmoMode() == GizmoMode::Scale && m_MultiSelection.size() <= 1)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
				return glm::normalize(RotationFromEuler(transform->Rotation) * direction);
		return direction;
	}

	// A point on the ring around world axis `axis`, at angle `theta`.
	glm::vec3 RingPoint(const glm::vec3& origin, int axis, float radius, float theta) const
	{
		glm::vec3 u(0.0f), v(0.0f);
		u[(axis + 1) % 3] = 1.0f;
		v[(axis + 2) % 3] = 1.0f;
		return origin + radius * (std::cos(theta) * u + std::sin(theta) * v);
	}

	float SegmentScreenDistance(const glm::vec3& from, const glm::vec3& to, const glm::vec2& mouse) const
	{
		glm::vec2 a, b;
		if (!WorldToScreen(from, a) || !WorldToScreen(to, b))
			return std::numeric_limits<float>::max();
		glm::vec2 segment = b - a;
		float lengthSquared = glm::dot(segment, segment);
		if (lengthSquared < 0.0001f)
			return glm::length(mouse - a);
		float t = glm::clamp(glm::dot(mouse - a, segment) / lengthSquared, 0.0f, 1.0f);
		return glm::length(mouse - (a + segment * t));
	}

	float HandleScreenDistance(int handle, const glm::vec2& mouse)
	{
		glm::vec3* target = GizmoPosition();
		if (!target)
			return std::numeric_limits<float>::max();
		glm::vec3 origin = *target;
		float length = GizmoLength();

		switch (EffectiveGizmoMode())
		{
			case GizmoMode::Rotate:
			{
				if (handle > 2)
					return std::numeric_limits<float>::max();
				float best = std::numeric_limits<float>::max();
				for (int i = 0; i < s_RingSegments; i++)
				{
					float a = glm::two_pi<float>() * i / s_RingSegments;
					float b = glm::two_pi<float>() * (i + 1) / s_RingSegments;
					best = std::min(best, SegmentScreenDistance(RingPoint(origin, handle, length, a),
						RingPoint(origin, handle, length, b), mouse));
				}
				return best;
			}
			case GizmoMode::Scale:
				if (handle == 3)
				{
					glm::vec2 centre;
					return WorldToScreen(origin, centre) ? glm::length(mouse - centre) : std::numeric_limits<float>::max();
				}
				return SegmentScreenDistance(origin, origin + GizmoAxis(handle) * length, mouse);
			case GizmoMode::Move:
			default:
				if (handle > 2)
					return std::numeric_limits<float>::max();
				return SegmentScreenDistance(origin, origin + GizmoAxis(handle) * length, mouse);
		}
	}

	// The nearest handle within reach of the cursor, or -1. Scale's centre
	// is tried first: every axis line starts there too.
	int PickHandle(const glm::vec2& mouse)
	{
		if (EffectiveGizmoMode() == GizmoMode::Scale && HandleScreenDistance(3, mouse) < m_GizmoPickPixels)
			return 3;
		int best = -1;
		float bestDistance = m_GizmoPickPixels;
		for (int axis = 0; axis < 3; axis++)
		{
			float distance = HandleScreenDistance(axis, mouse);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = axis;
			}
		}
		return best;
	}

	static bool RayPlane(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
		const glm::vec3& planePoint, const glm::vec3& normal, glm::vec3& hit)
	{
		float facing = glm::dot(rayDirection, normal);
		if (std::abs(facing) < 0.0001f)
			return false;
		float t = glm::dot(planePoint - rayOrigin, normal) / facing;
		if (t < 0.0f)
			return false;
		hit = rayOrigin + rayDirection * t;
		return true;
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
			// One command for the whole gesture, pushed when it ends, so Undo
			// puts things back as they were grabbed in a single step.
			//
			// Not while a mesh-edit session is active: GizmoPosition() then
			// returns the *point's* world position, not the entity's, and
			// the session has its own undo stack for exactly this gesture.
			if (m_DragAxis >= 0 && !m_MeshEditActive)
				FinishGizmoDrag();
			m_DragAxis = -1;
			m_HoverAxis = PickHandle(mouse);
			return;
		}

		glm::vec3 rayOrigin, rayDirection;
		ScreenRay(mouse, rayOrigin, rayDirection);

		if (m_DragAxis < 0)
		{
			if (m_HoverAxis >= 0 && justPressed)
				BeginGizmoDrag(m_HoverAxis, mouse, rayOrigin, rayDirection);
			return;
		}

		switch (m_DragMode)
		{
			case GizmoMode::Move: DragMove(rayOrigin, rayDirection); break;
			case GizmoMode::Rotate: DragRotate(mouse, rayOrigin, rayDirection); break;
			case GizmoMode::Scale: DragScale(mouse, rayOrigin, rayDirection); break;
		}
	}

	void BeginGizmoDrag(int handle, const glm::vec2& mouse, const glm::vec3& rayOrigin, const glm::vec3& rayDirection)
	{
		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		m_DragMode = EffectiveGizmoMode();
		m_DragStartPosition = *target;
		m_DragStartMouse = mouse;
		m_DragValue = m_DragMode == GizmoMode::Scale ? 1.0f : 0.0f;

		// Whole transforms of everything the gesture can change -- what
		// Rotate and Scale compute from, and their undo's "before".
		m_DragStartTransforms.clear();
		if (!m_MeshEditActive)
		{
			if (m_MultiSelection.size() > 1)
			{
				for (GS::EntityId entity : m_MultiSelection)
					if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
						m_DragStartTransforms.push_back({ entity, *transform });
			}
			else if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
				m_DragStartTransforms.push_back({ m_Selected, *transform });
		}

		if (m_DragMode == GizmoMode::Move)
		{
			float t;
			if (!ClosestPointOnAxis(*target, GizmoAxis(handle), rayOrigin, rayDirection, t))
				return;
			m_DragStartT = t;
			if (m_MultiSelection.size() > 1)
			{
				m_MultiSelectionStartPositions.clear();
				for (GS::EntityId entity : m_MultiSelection)
					if (auto* memberTransform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
						m_MultiSelectionStartPositions.push_back({ entity, memberTransform->Position });
			}
		}
		else if (m_DragMode == GizmoMode::Rotate)
		{
			// Turned by where the cursor meets the ring's plane -- unless the
			// ring is seen edge-on, where that point runs off to infinity:
			// then sideways mouse movement turns it instead.
			glm::vec3 normal(0.0f), hit;
			normal[handle] = 1.0f;
			m_RotateByScreen = std::abs(glm::dot(rayDirection, normal)) < 0.08f
				|| !RayPlane(rayOrigin, rayDirection, *target, normal, hit)
				|| glm::length(hit - *target) < 0.0001f;
			if (!m_RotateByScreen)
				m_RotatePrevious = glm::normalize(hit - *target);
			m_RotateAngle = 0.0f;
		}
		else if (handle != 3)
		{
			m_DragScaleAxis = GizmoAxis(handle);
			float t;
			if (!ClosestPointOnAxis(*target, m_DragScaleAxis, rayOrigin, rayDirection, t))
				return;
			// Grabbed (nearly) at the centre, a ratio of distances would
			// explode: the handle's own length stands in.
			m_DragStartT = std::abs(t) < GizmoLength() * 0.1f ? GizmoLength() : t;
		}

		m_DragAxis = handle;
	}

	void DragMove(const glm::vec3& rayOrigin, const glm::vec3& rayDirection)
	{
		glm::vec3 axisDirection = GizmoAxis(m_DragAxis);

		float t;
		if (!ClosestPointOnAxis(m_DragStartPosition, axisDirection, rayOrigin, rayDirection, t))
			return;

		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		// Relative to where it was grabbed, so the object does not snap its
		// origin to the cursor.
		glm::vec3 newPivot = m_DragStartPosition + axisDirection * (t - m_DragStartT);
		*target = newPivot;
		m_DragValue = t - m_DragStartT;

		if (m_MultiSelection.size() > 1)
		{
			glm::vec3 delta = newPivot - m_DragStartPosition;
			for (auto& [entity, startPos] : m_MultiSelectionStartPositions)
				if (auto* memberTransform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
					memberTransform->Position = startPos + delta;
		}
	}

	void DragRotate(const glm::vec2& mouse, const glm::vec3& rayOrigin, const glm::vec3& rayDirection)
	{
		glm::vec3 normal(0.0f);
		normal[m_DragAxis] = 1.0f;

		if (m_RotateByScreen)
			m_RotateAngle = (mouse.x - m_DragStartMouse.x) * 0.5f;
		else
		{
			// Summed a frame at a time, so a drag can go round more than
			// once -- one atan2 against the start would wrap at 180.
			glm::vec3 hit;
			if (RayPlane(rayOrigin, rayDirection, m_DragStartPosition, normal, hit)
				&& glm::length(hit - m_DragStartPosition) > 0.0001f)
			{
				glm::vec3 current = glm::normalize(hit - m_DragStartPosition);
				m_RotateAngle += GizmoMath::SignedAngle(m_RotatePrevious, current, normal);
				m_RotatePrevious = current;
			}
		}

		float angle = CtrlHeld() ? std::round(m_RotateAngle / 15.0f) * 15.0f : m_RotateAngle;
		m_DragValue = angle;

		// About the pivot: a lone entity's own position (which so stays
		// put), or a group's centre, which its members orbit.
		for (auto& [entity, start] : m_DragStartTransforms)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
				*transform = GizmoMath::Rotated(start, m_DragStartPosition, m_DragAxis, angle);
	}

	void DragScale(const glm::vec2& mouse, const glm::vec3& rayOrigin, const glm::vec3& rayDirection)
	{
		float factor;
		if (m_DragAxis == 3)
		{
			// The centre: right or up grows, left or down shrinks.
			float delta = (mouse.x - m_DragStartMouse.x) - (mouse.y - m_DragStartMouse.y);
			factor = 1.0f + delta * 0.005f;
		}
		else
		{
			float t;
			if (!ClosestPointOnAxis(m_DragStartPosition, m_DragScaleAxis, rayOrigin, rayDirection, t))
				return;
			factor = t / m_DragStartT;
		}
		if (CtrlHeld())
			factor = std::round(factor * 10.0f) / 10.0f;
		factor = std::max(factor, 0.01f);
		m_DragValue = factor;

		// Each entity in place, along its own axes.
		for (auto& [entity, start] : m_DragStartTransforms)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
				*transform = GizmoMath::Scaled(start, m_DragAxis, factor);
	}

	static bool SameTransform(const GS::TransformComponent& a, const GS::TransformComponent& b)
	{
		return a.Position == b.Position && a.Rotation == b.Rotation && a.Scale == b.Scale;
	}

	void FinishGizmoDrag()
	{
		if (m_DragMode == GizmoMode::Move)
		{
			if (m_MultiSelection.size() > 1)
			{
				std::vector<GS::EntityId> entities;
				std::vector<glm::vec3> before, after;
				bool anyMoved = false;
				for (auto& [entity, startPos] : m_MultiSelectionStartPositions)
					if (auto* memberTransform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
					{
						entities.push_back(entity);
						before.push_back(startPos);
						after.push_back(memberTransform->Position);
						if (memberTransform->Position != startPos)
							anyMoved = true;
					}
				if (anyMoved)
					EditorHistory::Push(std::make_unique<MultiMoveCommand>(entities, before, after));
			}
			else if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
				if (transform->Position != m_DragStartPosition)
					EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
						m_Selected, &GS::TransformComponent::Position, m_DragStartPosition, transform->Position));
			return;
		}

		std::vector<GS::EntityId> entities;
		std::vector<GS::TransformComponent> before, after;
		bool changed = false;
		for (auto& [entity, start] : m_DragStartTransforms)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
			{
				entities.push_back(entity);
				before.push_back(start);
				after.push_back(*transform);
				changed = changed || !SameTransform(start, *transform);
			}
		if (changed)
			EditorHistory::Push(std::make_unique<SetTransformsCommand>(entities, before, after));
	}

	// What the drag has done so far, beside the cursor.
	void DrawGizmoDragTooltip()
	{
		if (m_DragAxis < 0 || GS::Application::Get().IsUIHidden())
			return;
		static const char* axes[] = { "X", "Y", "Z", "all" };
		const char* axis = axes[m_DragAxis];
		switch (m_DragMode)
		{
			case GizmoMode::Move: ImGui::SetTooltip("Move %s  %+.3f", axis, m_DragValue); break;
			case GizmoMode::Rotate: ImGui::SetTooltip("Rotate %s  %+.1f deg%s", axis, m_DragValue, CtrlHeld() ? "  (snap 15)" : ""); break;
			case GizmoMode::Scale: ImGui::SetTooltip("Scale %s  x%.3f%s", axis, m_DragValue, CtrlHeld() ? "  (snap 0.1)" : ""); break;
		}
	}

	void DrawGizmo()
	{
		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		glm::vec3 origin = *target;
		float length = GizmoLength();

		const glm::vec4 axisColors[3] = {
			{ 1.0f, 0.25f, 0.25f, 1.0f }, { 0.30f, 1.0f, 0.35f, 1.0f }, { 0.35f, 0.55f, 1.0f, 1.0f }
		};
		const glm::vec4 active(1.0f, 1.0f, 0.5f, 1.0f);
		auto colorOf = [&](int handle, const glm::vec4& normal)
		{
			return (handle == m_DragAxis || (m_DragAxis < 0 && handle == m_HoverAxis)) ? active : normal;
		};

		switch (EffectiveGizmoMode())
		{
			case GizmoMode::Move:
				for (int axis = 0; axis < 3; axis++)
				{
					glm::vec3 direction = GizmoAxis(axis);
					glm::vec4 color = colorOf(axis, axisColors[axis]);
					glm::vec3 tip = origin + direction * length;
					GS::Renderer2D::DrawLine(origin, tip, color);

					// A little cross at the tip, so the end of the handle is
					// visible even when the line is nearly edge-on.
					glm::vec3 a(0.0f), b(0.0f);
					a[(axis + 1) % 3] = 0.06f * length;
					b[(axis + 2) % 3] = 0.06f * length;
					GS::Renderer2D::DrawLine(tip - a, tip + a, color);
					GS::Renderer2D::DrawLine(tip - b, tip + b, color);
				}
				break;

			case GizmoMode::Rotate:
				for (int axis = 0; axis < 3; axis++)
				{
					glm::vec4 color = colorOf(axis, axisColors[axis]);
					for (int i = 0; i < s_RingSegments; i++)
					{
						float a = glm::two_pi<float>() * i / s_RingSegments;
						float b = glm::two_pi<float>() * (i + 1) / s_RingSegments;
						GS::Renderer2D::DrawLine(RingPoint(origin, axis, length, a), RingPoint(origin, axis, length, b), color);
					}
				}
				break;

			case GizmoMode::Scale:
			{
				// A small cube: the conventional mark of a scale handle.
				auto box = [](const glm::vec3& centre, float half, const glm::vec4& color)
				{
					for (int axis = 0; axis < 3; axis++)
					{
						int u = (axis + 1) % 3, v = (axis + 2) % 3;
						for (int corner = 0; corner < 4; corner++)
						{
							glm::vec3 offset(0.0f);
							offset[u] = (corner & 1) ? half : -half;
							offset[v] = (corner & 2) ? half : -half;
							glm::vec3 from = centre + offset, to = centre + offset;
							from[axis] -= half;
							to[axis] += half;
							GS::Renderer2D::DrawLine(from, to, color);
						}
					}
				};
				for (int axis = 0; axis < 3; axis++)
				{
					glm::vec4 color = colorOf(axis, axisColors[axis]);
					glm::vec3 tip = origin + GizmoAxis(axis) * length;
					GS::Renderer2D::DrawLine(origin, tip, color);
					box(tip, 0.05f * length, color);
				}
				box(origin, 0.07f * length, colorOf(3, glm::vec4(0.9f, 0.9f, 0.9f, 1.0f)));
				break;
			}
		}
	}

	// Adapted from Cube3D's fly camera, not identical to it: the
	// WantCaptureKeyboard guard below is new -- this view sits beside the
	// Inspector's and Assets panel's text/numeric fields, and Cube3D never
	// had that problem because a demo doesn't share the window with editable
	// text. Arrow-key look was originally dropped here as redundant with
	// middle-drag look, then re-added on request -- PerspectiveCamera::
	// SetRotation already clamps pitch, so holding Up/Down can't flip the
	// view over.
	void MoveCamera(GS::Timestep ts)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		glm::vec3 position = m_Camera.GetPosition();
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		float move = m_MoveSpeed * ts;
		float look = m_LookSpeed * ts;

		if (GS::Input::IsKeyPressed(GS_KEY_W)) position += m_Camera.GetForward() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_D)) position += m_Camera.GetRight() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_E)) position.y += move;
		if (GS::Input::IsKeyPressed(GS_KEY_Q)) position.y -= move;

		if (GS::Input::IsKeyPressed(GS_KEY_LEFT))  yaw -= look;
		if (GS::Input::IsKeyPressed(GS_KEY_RIGHT)) yaw += look;
		if (GS::Input::IsKeyPressed(GS_KEY_UP))    pitch += look;
		if (GS::Input::IsKeyPressed(GS_KEY_DOWN))  pitch -= look;

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

	SceneRenderer m_SceneRenderer;
	// See BuildShader's comment: higher than Cube3D's 0.10 default because a
	// scene here can have zero placed lights, where Cube3D always has one.

	FileBrowserPopup m_ScriptBrowser;
	FileBrowserPopup m_MaterialBrowser;

	GS::EntityId m_Selected = GS::InvalidEntity;
	GS::EntityId m_Hovered = GS::InvalidEntity;

	// Transient, session-only, and deliberately separate from GroupComponent
	// -- Ctrl+click builds a working set to act on right now (drag, or
	// Ctrl+G), nothing about it touches the scene file. See the design
	// spec's "Why this shape" section.
	std::vector<GS::EntityId> m_MultiSelection;
	glm::vec3 m_MultiSelectionPivot{ 0.0f };
	std::vector<std::pair<GS::EntityId, glm::vec3>> m_MultiSelectionStartPositions;

	// Backing buffer for the Outliner row context menu's inline rename
	// field -- seeded from the row's tag on popup-open (IsWindowAppearing),
	// same reasoning as m_EditBeforeString above but for a raw InputText
	// buffer rather than a captured "before" value.
	char m_RenameBuf[256] = {};

	// Set by OnEvent's right-click handler, consumed by OnImGuiRender --
	// ImGui::OpenPopup has to run inside the ImGui frame, which an input
	// event isn't guaranteed to be within.
	bool m_ViewportContextRequested = false;
	GS::EntityId m_ViewportContextEntity = GS::InvalidEntity;

	bool m_GroupPromptRequested = false;
	bool m_GroupPromptNeedsConfirm = false;
	char m_GroupNameBuf[256] = {};

	bool m_GroupDialogRequested = false;
	GS::EntityId m_GroupDialogEntity = GS::InvalidEntity;
	std::string m_RenamingGroup;   // empty = nothing being renamed right now
	char m_GroupRenameBuf[256] = {};
	char m_NewGroupBuf[256] = {};

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
	int m_EditBeforeInt = 0;
	std::string m_EditBeforeString;

	bool m_MeshEditActive = false;
	EditableMesh m_MeshEditSession;
	int m_MeshEditSelectedPoint = -1;
	glm::vec3 m_MeshEditSessionWorldPoint{ 0.0f };
	bool m_MeshEditPreviewDirty = false;
	// Which entity the open session belongs to -- m_Selected can move to a
	// different, still-valid entity out from under an open session (several
	// Select() call sites elsewhere have no reason to know one is open), and
	// this is what ValidateMeshEditSession checks against to catch that.
	GS::EntityId m_MeshEditEntity = GS::InvalidEntity;

	int m_UvExportResolution = 1024;

	int m_HoverAxis = -1;
	bool m_MouseDownLastFrame = false;
	float m_DragStartT = 0.0f;
	glm::vec3 m_DragStartPosition{ 0.0f };
	unsigned int m_Antialiasing = 1;     // samples asked of m_Framebuffer
	float m_GizmoLength = 1.0f;          // world units, when there is nothing to measure from
	float m_GizmoScreenShare = 0.15f;    // gizmo length / distance to the camera
	float m_GizmoPickPixels = 12.0f;
	static constexpr int s_RingSegments = 64;

	GizmoMode m_GizmoMode = GizmoMode::Move;
	GizmoMode m_DragMode = GizmoMode::Move;
	glm::vec2 m_DragStartMouse{ 0.0f };
	float m_DragValue = 0.0f;            // what the tooltip shows: distance, degrees or factor
	std::vector<std::pair<GS::EntityId, GS::TransformComponent>> m_DragStartTransforms;
	bool m_RotateByScreen = false;
	glm::vec3 m_RotatePrevious{ 1.0f, 0.0f, 0.0f };
	float m_RotateAngle = 0.0f;
	glm::vec3 m_DragScaleAxis{ 1.0f, 0.0f, 0.0f };

	glm::vec2 m_PreviousMouse{ 0.0f, 0.0f };
	float m_MoveSpeed = 3.0f;
	float m_LookSpeed = 70.0f;
	float m_MouseLookSensitivity = 0.18f;
};
