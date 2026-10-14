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

// Everything an entity is, as data: what Delete keeps to bring it back,
// what Duplicate copies, what Place creates. One list of components, here,
// rather than one in each command -- three copies had to be kept in step,
// and a component missing from one came back from an Undo without it.
struct EntitySnapshot
{
	std::string Name;
	GS::TransformComponent Transform;
	std::optional<GS::MeshComponent> Mesh;
	std::optional<GS::CameraComponent> Camera;
	std::optional<GS::ScriptComponent> Script;
	std::optional<GS::LightComponent> Light;
	std::optional<GS::PhysicsComponent> Physics;
	std::optional<GS::GroupComponent> Group;
	std::optional<GS::TerrainComponent> Terrain;
	std::optional<GS::PlayerComponent> Player;
	std::optional<GS::NpcComponent> Npc;

	static EntitySnapshot Capture(GS::EntityId entity)
	{
		EntitySnapshot s;
		if (auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity)) s.Name = tag->Name;
		if (auto* c = g_EditorScene.GetComponent<GS::TransformComponent>(entity)) s.Transform = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::MeshComponent>(entity)) s.Mesh = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::CameraComponent>(entity)) s.Camera = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::ScriptComponent>(entity)) s.Script = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::LightComponent>(entity)) s.Light = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::PhysicsComponent>(entity)) s.Physics = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::GroupComponent>(entity)) s.Group = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::TerrainComponent>(entity)) s.Terrain = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::PlayerComponent>(entity)) s.Player = *c;
		if (auto* c = g_EditorScene.GetComponent<GS::NpcComponent>(entity)) s.Npc = *c;
		return s;
	}

	// Makes an existing entity hold exactly these components (tag and
	// transform aside): each present one set, each absent one removed. In
	// place, so the entity keeps its id and its place in the scene.
	void ApplyTo(GS::EntityId entity) const
	{
		if (!g_EditorScene.IsValid(entity))
			return;
		if (auto* c = g_EditorScene.GetComponent<GS::TransformComponent>(entity)) *c = Transform;
		Set(entity, Mesh);
		Set(entity, Camera);
		Set(entity, Script);
		Set(entity, Light);
		Set(entity, Physics);
		Set(entity, Group);
		Set(entity, Terrain);
		Set(entity, Player);
		Set(entity, Npc);
	}

	template<typename Component>
	static void Set(GS::EntityId entity, const std::optional<Component>& value)
	{
		Component* current = g_EditorScene.GetComponent<Component>(entity);
		if (value && current)
			*current = *value;
		else if (value)
			g_EditorScene.AddComponent<Component>(entity, *value);
		else if (current)
			g_EditorScene.RemoveComponent<Component>(entity);
	}

	GS::EntityId Create(const std::string& name) const
	{
		GS::Entity entity = g_EditorScene.CreateEntity(name);
		*entity.Get<GS::TransformComponent>() = Transform;
		if (Mesh) entity.Add<GS::MeshComponent>(*Mesh);
		if (Camera) entity.Add<GS::CameraComponent>(*Camera);
		if (Script) entity.Add<GS::ScriptComponent>(*Script);
		if (Light) entity.Add<GS::LightComponent>(*Light);
		if (Physics) entity.Add<GS::PhysicsComponent>(*Physics);
		if (Group) entity.Add<GS::GroupComponent>(*Group);
		if (Terrain) entity.Add<GS::TerrainComponent>(*Terrain);
		if (Player) entity.Add<GS::PlayerComponent>(*Player);
		if (Npc) entity.Add<GS::NpcComponent>(*Npc);
		return entity.GetId();
	}
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
	{
		m_Snapshot.Name = name;
		m_Snapshot.Transform = transform;
		m_Snapshot.Mesh = mesh;
		m_Snapshot.Camera = camera;
		m_Snapshot.Light = light;
	}

	// Any combination of components -- a player is a mesh, a player and a
	// physics body; a terrain is a mesh, its terrain and a static body.
	explicit PlaceEntityCommand(EntitySnapshot snapshot) : m_Snapshot(std::move(snapshot)) {}

	void Redo() override { m_Entity = m_Snapshot.Create(m_Snapshot.Name); }

	void Undo() override
	{
		GS::EntityId current = EditorHistory::Resolve(m_Entity);
		if (g_EditorScene.IsValid(current))
			g_EditorScene.DestroyEntity(current);
	}
private:
	EntitySnapshot m_Snapshot;
};

// The inverse of Place: captures an existing entity's data at construction,
// destroys it on Redo, recreates it from the captured data on Undo.
class DeleteEntityCommand : public EditorCommand
{
public:
	explicit DeleteEntityCommand(GS::EntityId entity) : m_Snapshot(EntitySnapshot::Capture(entity))
	{
		m_Entity = entity;
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
		m_Entity = m_Snapshot.Create(m_Snapshot.Name);

		// Anything else on the stack that captured oldEntity (an
		// EditFieldCommand pushed before this Delete, or the original
		// PlaceEntityCommand) needs to keep finding this entity, which
		// GS::Scene's generation bump would otherwise make impossible.
		EditorHistory::s_Remap[oldEntity] = m_Entity;
	}
private:
	EntitySnapshot m_Snapshot;
};

// Adding or removing components on an entity, as one undo step: the whole
// set before and after, applied in place. What Add Terrain, Make Player and
// their Remove buttons push.
class ReplaceEntityCommand : public EditorCommand
{
public:
	ReplaceEntityCommand(GS::EntityId entity, EntitySnapshot before, EntitySnapshot after)
		: m_Before(std::move(before)), m_After(std::move(after))
	{
		m_Entity = entity;
	}

	void Redo() override { m_After.ApplyTo(EditorHistory::Resolve(m_Entity)); }
	void Undo() override { m_Before.ApplyTo(EditorHistory::Resolve(m_Entity)); }
private:
	EntitySnapshot m_Before, m_After;
};

// The context menu's Duplicate: a snapshot of the source, created as a new
// entity. Shaped like PlaceEntityCommand rather than DeleteEntityCommand's
// Undo -- this command *adds*, so Redo creates and Undo destroys, with no
// s_Remap bookkeeping needed (nothing can reference this entity before it
// has ever been created).
class DuplicateEntityCommand : public EditorCommand
{
public:
	explicit DuplicateEntityCommand(GS::EntityId source) : m_Snapshot(EntitySnapshot::Capture(source))
	{
		m_Snapshot.Name += " Copy";
	}

	void Redo() override { m_Entity = m_Snapshot.Create(m_Snapshot.Name); }

	void Undo() override
	{
		GS::EntityId current = EditorHistory::Resolve(m_Entity);
		if (g_EditorScene.IsValid(current))
			g_EditorScene.DestroyEntity(current);
	}
private:
	EntitySnapshot m_Snapshot;
};

// The multi-select gizmo's equivalent of EditFieldCommand<TransformComponent,
// glm::vec3>, generalized to N entities so one Ctrl+Z reverses a whole
// group-drag gesture at once rather than one entity at a time. Does not
// override SelectionAfter() -- a multi-move never changes what's selected,
// unlike Place/Delete/Duplicate, so the base class's "no entity of my own"
// default (returns GS::InvalidEntity) is correct as-is.
class MultiMoveCommand : public EditorCommand
{
public:
	MultiMoveCommand(std::vector<GS::EntityId> entities, std::vector<glm::vec3> before, std::vector<glm::vec3> after)
		: m_Entities(std::move(entities)), m_Before(std::move(before)), m_After(std::move(after))
	{
	}

	void Redo() override { Apply(m_After); }
	void Undo() override { Apply(m_Before); }
private:
	void Apply(const std::vector<glm::vec3>& positions)
	{
		for (size_t i = 0; i < m_Entities.size(); i++)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Entities[i]))
				transform->Position = positions[i];
	}

	std::vector<GS::EntityId> m_Entities;
	std::vector<glm::vec3> m_Before;
	std::vector<glm::vec3> m_After;
};

// A rotate or scale gizmo drag: whole transforms, before and after, for
// every entity it moved -- a group rotation changes positions as well as
// rotations, so one field is not enough. One gesture, one Ctrl+Z. A
// single-entity drag keeps that entity selected across Undo/Redo
// (m_Entity, via the base class); a group one leaves selection alone, as
// MultiMoveCommand does.
class SetTransformsCommand : public EditorCommand
{
public:
	SetTransformsCommand(std::vector<GS::EntityId> entities, std::vector<GS::TransformComponent> before,
		std::vector<GS::TransformComponent> after)
		: m_Entities(std::move(entities)), m_Before(std::move(before)), m_After(std::move(after))
	{
		if (m_Entities.size() == 1)
			m_Entity = m_Entities[0];
	}

	void Redo() override { Apply(m_After); }
	void Undo() override { Apply(m_Before); }
private:
	void Apply(const std::vector<GS::TransformComponent>& transforms)
	{
		for (size_t i = 0; i < m_Entities.size(); i++)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(EditorHistory::Resolve(m_Entities[i])))
				*transform = transforms[i];
	}

	std::vector<GS::EntityId> m_Entities;
	std::vector<GS::TransformComponent> m_Before;
	std::vector<GS::TransformComponent> m_After;
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

	// The cursor value at the last successful save. Dirty is "the cursor
	// has moved since then" -- exact, not a coarse boolean: undoing back
	// to precisely the point a save happened reads as clean again, because
	// position (not just "has anything happened") is what's compared.
	inline size_t s_CleanCursor = 0;

	inline bool IsDirty() { return s_Cursor != s_CleanCursor; }
	inline void MarkClean() { s_CleanCursor = s_Cursor; }

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
		s_CleanCursor = 0;
		s_Remap.clear();
	}

}
