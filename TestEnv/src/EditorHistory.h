#pragma once

// A small command-history stack for the editor's own actions -- placing an
// entity, deleting one, editing a field in the Inspector. Not a
// general-purpose ECS transaction system: it covers what the editor itself
// does, and nothing a future system (mesh authoring, scripting) should
// build its own transactions on without a reason to widen this.
//
// Every command exposes SelectionAfter() rather than reaching into
// EditorSceneView to set the selection itself -- that would require this
// file to include EditorSceneView.h, and EditorSceneView.h needs to include
// *this* file for its Inspector edits to push commands. Returning the
// answer and letting the caller apply it keeps the dependency one-way.

#include <GS.h>
#include <optional>
#include <unordered_map>

#include "EditorProject.h"

// Forward-declared so EditorCommand::SelectionAfter() can resolve through
// it -- defined for real in the EditorHistory namespace below, alongside
// the map it reads.
namespace EditorHistory {
	inline std::unordered_map<GS::EntityId, GS::EntityId> s_Remap;

	inline GS::EntityId Resolve(GS::EntityId id)
	{
		auto it = s_Remap.find(id);
		while (it != s_Remap.end())
		{
			id = it->second;
			it = s_Remap.find(id);
		}
		return id;
	}
}

class EditorCommand
{
public:
	virtual ~EditorCommand() = default;
	virtual void Undo() = 0;
	virtual void Redo() = 0;

	// Whichever entity this command concerns, if it currently exists --
	// GS::InvalidEntity otherwise. Implemented once here rather than by
	// each derived command, because "is my entity valid right now" is the
	// same question after every Undo/Redo regardless of what kind of
	// command it was: a Place command's entity stops existing after Undo,
	// a Delete command's entity starts existing again after Undo, and an
	// Edit command's entity was never destroyed either way.
	GS::EntityId SelectionAfter() const
	{
		GS::EntityId current = EditorHistory::Resolve(m_Entity);
		return g_EditorScene.IsValid(current) ? current : GS::InvalidEntity;
	}
protected:
	GS::EntityId m_Entity = GS::InvalidEntity;
};

// Creates an entity with the given components. Undo destroys it; Redo
// creates it again. A fresh CreateEntity call every time rather than
// reusing an id -- GS::Scene's ids are index+generation, and a destroyed
// slot's next occupant always gets a bumped generation, so "the same
// entity" after a Redo is a different id from before the matching Undo.
class PlaceEntityCommand : public EditorCommand
{
public:
	PlaceEntityCommand(const std::string& name, const GS::TransformComponent& transform,
		const std::optional<GS::MeshComponent>& mesh, const std::optional<GS::CameraComponent>& camera,
		const std::optional<GS::LightComponent>& light = std::nullopt)
		: m_Name(name), m_Transform(transform), m_Mesh(mesh), m_Camera(camera), m_Light(light)
	{
	}

	void Redo() override
	{
		GS::Entity entity = g_EditorScene.CreateEntity(m_Name);
		*entity.Get<GS::TransformComponent>() = m_Transform;
		if (m_Mesh)
			entity.Add<GS::MeshComponent>(*m_Mesh);
		if (m_Camera)
			entity.Add<GS::CameraComponent>(*m_Camera);
		if (m_Light)
			entity.Add<GS::LightComponent>(*m_Light);
		m_Entity = entity.GetId();
	}

	void Undo() override
	{
		GS::EntityId current = EditorHistory::Resolve(m_Entity);
		if (g_EditorScene.IsValid(current))
			g_EditorScene.DestroyEntity(current);
	}
private:
	std::string m_Name;
	GS::TransformComponent m_Transform;
	std::optional<GS::MeshComponent> m_Mesh;
	std::optional<GS::CameraComponent> m_Camera;
	std::optional<GS::LightComponent> m_Light;
};

// The inverse of Place: captures an existing entity's data at construction,
// destroys it on Redo, recreates it from the captured data on Undo.
class DeleteEntityCommand : public EditorCommand
{
public:
	explicit DeleteEntityCommand(GS::EntityId entity)
	{
		m_Entity = entity;

		if (auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity))
			m_Name = tag->Name;
		if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
			m_Transform = *transform;
		if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity))
			m_Mesh = *mesh;
		if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(entity))
			m_Camera = *camera;
		if (auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(entity))
			m_Script = *script;
		if (auto* light = g_EditorScene.GetComponent<GS::LightComponent>(entity))
			m_Light = *light;
	}

	void Redo() override
	{
		GS::EntityId current = EditorHistory::Resolve(m_Entity);
		if (g_EditorScene.IsValid(current))
			g_EditorScene.DestroyEntity(current);
	}

	void Undo() override
	{
		GS::EntityId oldEntity = m_Entity;

		GS::Entity entity = g_EditorScene.CreateEntity(m_Name);
		*entity.Get<GS::TransformComponent>() = m_Transform;
		if (m_Mesh)
			entity.Add<GS::MeshComponent>(*m_Mesh);
		if (m_Camera)
			entity.Add<GS::CameraComponent>(*m_Camera);
		if (m_Script)
			entity.Add<GS::ScriptComponent>(*m_Script);
		if (m_Light)
			entity.Add<GS::LightComponent>(*m_Light);
		m_Entity = entity.GetId();

		// Anything else on the stack that captured oldEntity (an
		// EditFieldCommand pushed before this Delete, or the original
		// PlaceEntityCommand) needs to keep finding this entity, which
		// GS::Scene's generation bump would otherwise make impossible.
		EditorHistory::s_Remap[oldEntity] = m_Entity;
	}
private:
	std::string m_Name;
	GS::TransformComponent m_Transform;
	std::optional<GS::MeshComponent> m_Mesh;
	std::optional<GS::CameraComponent> m_Camera;
	std::optional<GS::ScriptComponent> m_Script;
	std::optional<GS::LightComponent> m_Light;
};

// One field of one component, changed once. `Component` and `Field` are
// deduced from the pointer-to-member passed in -- e.g.
// EditFieldCommand<GS::TransformComponent, glm::vec3>(entity,
// &GS::TransformComponent::Position, before, after). The component pointer
// is re-resolved through GetComponent on every Undo/Redo rather than
// captured once: ComponentStore's removal swaps the last element into a
// freed slot, which can silently move another entity's component, so a
// pointer captured at edit time and used later is exactly the kind of
// stale reference that pattern warns against.
template<typename Component, typename Field>
class EditFieldCommand : public EditorCommand
{
public:
	using MemberPtr = Field Component::*;

	EditFieldCommand(GS::EntityId entity, MemberPtr member, const Field& oldValue, const Field& newValue)
		: m_Member(member), m_Old(oldValue), m_New(newValue)
	{
		m_Entity = entity;
	}

	void Redo() override { Apply(m_New); }
	void Undo() override { Apply(m_Old); }
private:
	void Apply(const Field& value)
	{
		GS::EntityId current = EditorHistory::Resolve(m_Entity);
		if (auto* component = g_EditorScene.GetComponent<Component>(current))
			(*component).*m_Member = value;
	}

	MemberPtr m_Member;
	Field m_Old, m_New;
};

namespace EditorHistory {

	inline std::vector<std::unique_ptr<EditorCommand>> s_Commands;
	inline size_t s_Cursor = 0;

	inline bool CanUndo() { return s_Cursor > 0; }
	inline bool CanRedo() { return s_Cursor < s_Commands.size(); }

	// Performs the command (its Redo()) and adds it to the stack, discarding
	// anything after the cursor -- a new action after undoing replaces the
	// redo branch rather than keeping it around, the standard shape of an
	// undo stack. Returns what selection should become.
	inline GS::EntityId Push(std::unique_ptr<EditorCommand> command)
	{
		s_Commands.resize(s_Cursor);

		command->Redo();
		GS::EntityId selection = command->SelectionAfter();

		s_Commands.push_back(std::move(command));
		s_Cursor = s_Commands.size();

		return selection;
	}

	inline GS::EntityId Undo()
	{
		if (!CanUndo())
			return GS::InvalidEntity;

		s_Cursor--;
		s_Commands[s_Cursor]->Undo();
		return s_Commands[s_Cursor]->SelectionAfter();
	}

	inline GS::EntityId Redo()
	{
		if (!CanRedo())
			return GS::InvalidEntity;

		s_Commands[s_Cursor]->Redo();
		GS::EntityId selection = s_Commands[s_Cursor]->SelectionAfter();
		s_Cursor++;
		return selection;
	}

	// Called whenever the scene itself is swapped out from under the
	// history (New Project, Open Project, Open Demo) -- undoing across a
	// scene swap has no sensible meaning, the same reasoning that already
	// motivated clearing selection on those exact same actions.
	inline void Clear()
	{
		s_Commands.clear();
		s_Cursor = 0;
		s_Remap.clear();
	}

}
