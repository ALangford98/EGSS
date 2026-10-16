#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Texture.h"
#include "GS/Renderer/Mesh.h"
#include "GS/Renderer/Material.h"
#include "GS/Physics/RigidBody2D.h"   // BodyType -- dimension-agnostic, reused as-is
#include "GS/Network/NetServer.h"     // ClientId/ServerOwned, for NetworkIdentity

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace GS {

	// The components the engine itself knows about. Game-specific ones live in
	// the game -- any struct can be a component, there is nothing to register.

	struct TagComponent
	{
		std::string Name;
	};

	// Where a thing is. Almost every other system reads this one, which is why
	// it is stored as parts rather than a matrix: a matrix is easy to build
	// from position/rotation/scale and painful to pull them back out of.
	struct TransformComponent
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		// Degrees, applied X then Y then Z.
		glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		glm::mat4 GetTransform() const
		{
			// Right to left: scale, then rotate, then translate.
			return glm::translate(glm::mat4(1.0f), Position)
				* glm::rotate(glm::mat4(1.0f), glm::radians(Rotation.x), glm::vec3(1, 0, 0))
				* glm::rotate(glm::mat4(1.0f), glm::radians(Rotation.y), glm::vec3(0, 1, 0))
				* glm::rotate(glm::mat4(1.0f), glm::radians(Rotation.z), glm::vec3(0, 0, 1))
				* glm::scale(glm::mat4(1.0f), Scale);
		}
	};

	struct SpriteComponent
	{
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::shared_ptr<Texture2D> Texture;
		float TilingFactor = 1.0f;
	};

	// Links an entity to a body in a PhysicsWorld2D.
	//
	// The body is *not* stored here. The physics world owns its bodies and
	// knows nothing about entities -- this is only the handle that joins the
	// two, which is what lets the physics world stay a standalone thing that
	// could be swapped for Box2D without the scene noticing.
	struct RigidBody2DComponent
	{
		unsigned int Body = ~0u;
		// When false the transform drives the body; otherwise the body drives
		// the transform. Static scenery usually wants the former.
		bool DrivenByPhysics = true;
	};

	// 3D geometry. The mesh is shared rather than owned: a hundred entities
	// pointing at one Mesh cost one copy of the geometry on the GPU. That is
	// the whole reason the transform lives on the entity and not in the mesh.
	struct MeshComponent
	{
		// Named Geometry, not Mesh -- a member cannot share its name with the
		// type it is declared from inside the same struct.
		std::shared_ptr<GS::Mesh> Geometry;

		// The cache key Geometry was resolved through -- "primitive:cube", or
		// a file path. Empty means this mesh was built ad hoc and has nothing
		// to reload from, which Scene::Save uses to skip it rather than write
		// a reference nothing can follow.
		std::string SourcePath;

		// One material per submesh, in the same order as Geometry's. A model
		// whose file switched material partway through needs one each, so this
		// is a vector rather than the single material it started as -- a mesh
		// with one submesh simply has one entry.
		//
		// May be shorter than the submesh count, or empty: the renderer fills
		// the gap. An entity with geometry and no material yet is a normal
		// thing to have while a scene is being built.
		std::vector<std::shared_ptr<GS::Material>> Materials;

		// Whether the materials above came from the model's own .mtl. When they
		// did, the renderer must leave their colour alone -- overwriting it
		// with Color would throw away the thing the file was loaded for. When
		// they did not, Color drives them, which is what lets the panel recolour
		// a primitive.
		bool MaterialsFromFile = false;

		// Kept alongside the materials rather than folded into them. Colour is
		// the one thing every scene wants per object, and requiring a material
		// for it would mean building one before anything could be seen at all.
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool Visible = true;

		// A procedural material (.gsmat) whose albedo textures this mesh,
		// multiplied by Color. Empty means none -- flat Color, as before this
		// existed. Only a path: the engine does not evaluate graphs per
		// entity; the editor resolves it through one shared texture per file
		// (Runtime/MaterialLibrary.h), the same relationship SourcePath
		// has with MeshCache.
		std::string MaterialPath;

		// How the surface takes light under an EnvironmentComponent's shading
		// (GGX): roughness 0 a mirror, 1 chalk; metallic 0 a dielectric (4%
		// reflectance, coloured diffuse), 1 a metal (coloured reflection, no
		// diffuse); emission adds Color times this, unlit -- a lamp's panel, a
		// screen. Ignored by the plain shader, so a scene without an
		// environment looks exactly as it did; saved only when not default.
		float Roughness = 0.5f;
		float Metallic = 0.0f;
		float Emission = 0.0f;
	};

	struct LightComponent
	{
		glm::vec4 Color = { 1.0f, 0.95f, 0.8f, 1.0f };
		float Radius = 2.0f;
		bool Enabled = true;
	};

	// A viewpoint an entity carries with it. Position and yaw/pitch are read
	// off TransformComponent -- there is no reason for a camera to have two
	// places to be -- so this is only the lens.
	struct CameraComponent
	{
		float Fov = 45.0f;
		float NearClip = 0.1f;
		float FarClip = 1000.0f;

		// At most one camera in a scene drives the runtime view. A second
		// entity with this set simply loses -- whichever is found first wins,
		// which is enough until something needs to switch cameras at runtime.
		bool Active = false;
	};

	// A TypeScript file's path, run against this entity while Play mode is
	// active (TestEnv/src/PlayMode.h). The engine itself only knows this is
	// a path -- same relationship MeshComponent has with SourcePath.
	struct ScriptComponent
	{
		std::string ScriptPath;
	};

	// A named tag shared by every entity that carries the same GroupName --
	// there is no separate group entity or registry. An entity belongs to
	// at most one group; joining a new one overwrites the old value, it
	// doesn't add to it. See docs/superpowers/specs/2026-09-12-entity-
	// grouping-design.md for why this is a plain string rather than a
	// group entity or a set.
	struct GroupComponent
	{
		std::string GroupName;
	};

	// Data only, for now -- nothing reads this yet. RigidBody2DComponent
	// links an entity to a body a PhysicsWorld2D already simulates; this has
	// no world behind it at all. It exists so the editor's Physics tab
	// (right-click an entity -> Advanced -> Physics) has real fields to hold
	// and a scene file to survive a Save/Load round trip in, ahead of the
	// editor scene actually running a physics world. Deliberately placed
	// scaffolding, not an oversight -- see editor_roadmap.md.
	struct PhysicsComponent
	{
		BodyType Type = BodyType::Dynamic;
		float Mass = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		// The velocity a dynamic body is given the moment Play starts, in
		// m/s, world axes -- a throw, a push, a launch, set up in the
		// editor. A velocity rather than a force or an impulse so that what
		// is typed is what happens whatever the mass: 5 along x moves it at
		// 5 m/s along x on the first step, a crate or a pebble.
		glm::vec3 LaunchVelocity{ 0.0f };
	};

	// Terrain on a primitive shape: the shape rebuilt at Resolution, its
	// surface pushed in and out by fractal noise -- a plane becomes hills, a
	// sphere a small planet, a cube a rock. The mesh is generated
	// (TerrainMesh), never stored: the scene file keeps these numbers and
	// every load rebuilds the same surface from them.
	//
	// Units are the shape's own, before the entity's scale: the primitives
	// are 1 across, so Height 0.1 on a plane scaled to 50 is 5 m of relief.
	// Ints and floats rather than enums and uint32_t so the editor's undo
	// (EditFieldCommand) takes every field as it is.
	struct TerrainComponent
	{
		int Resolution = 64;       // subdivisions across the shape: the vertex count
		float Height = 0.1f;       // displacement, in the shape's units
		float Frequency = 3.0f;    // features across the shape
		int Octaves = 5;           // layers of finer detail
		float Roughness = 0.5f;    // how much each finer layer keeps (the fbm gain)
		int Seed = 1;
		bool Ridged = false;       // sharp crests instead of rounded hills

		// What MeshComponent::Geometry was last built from. Not saved: the
		// editor compares it each frame and rebuilds when an edit or an
		// undo has changed the numbers.
		std::string BuiltKey;
	};

	// The character a player controls in Play and in an exported game:
	// WASD relative to the camera, Space to jump, Shift to sprint, and a
	// camera that follows (orbit with right-drag or the arrow keys). Run by
	// Runtime/CharacterController.h, which gives it an upright capsule body.
	struct PlayerComponent
	{
		float Speed = 5.0f;            // metres per second, walking
		float SprintMultiplier = 1.8f;
		float JumpHeight = 1.2f;       // metres: the jump speed is derived from it and gravity
		float CameraDistance = 6.0f;   // third person: how far behind
		float CameraHeight = 0.6f;     // above the character's centre: 1.5 m over the feet, about shoulder height
		int CameraMode = 0;            // 0 third person, 1 first person, 2 leave the scene's camera alone
		int Body = 0;                  // 0 upright capsule, 1 rolling ball, 2 humanoid (active ragdoll)
		int Look = 0;                  // a humanoid's: 0 mannequin (its colliders), 1 crash-test dummy
	};

	// A non-player character with a simple built-in behaviour, run by the
	// same controller as the player. Anything richer belongs in a script,
	// which an NPC can carry as well.
	struct NpcComponent
	{
		int Behaviour = 1;             // 0 idle, 1 wander, 2 follow the player, 3 flee the player
		float Speed = 2.5f;
		float WanderRadius = 6.0f;     // around where it started
		float DetectRadius = 10.0f;    // follow/flee: how near the player must be
		float StopDistance = 2.0f;     // follow: how close it comes
		int Body = 0;                  // as PlayerComponent::Body
		int Look = 0;                  // as PlayerComponent::Look
	};

	// Opt-in networking. An entity with both of these has its
	// TransformComponent auto-replicated by the server to every client;
	// anything else about it (score, health, "this ball bounced") travels
	// as a manual RPC instead -- see GS::Net::SendRPC.
	// Fires a physics object at a target when the game asks -- the F key, or
	// "fire" in the game console (Runtime/Launcher.h is the system). The
	// launcher's own transform is where things leave from.
	struct LauncherComponent
	{
		int Shape = 0;                 // 0 ball, 1 box
		glm::vec3 Size{ 0.3f };        // a ball's diameter is x; a box's edges
		float Mass = 5.0f;             // kg
		float Speed = 15.0f;           // m/s, leaving the launcher
		float Spin = 0.0f;             // rad/s, topspin about the sideways axis
		float AimHeight = 1.3f;        // m above the target's origin -- its feet; 1.3 is the chest
		bool Arc = true;               // aim high enough for gravity to bring it onto the target
		glm::vec4 Color{ 0.95f, 0.55f, 0.15f, 1.0f };
		std::string Target;            // the tag of what it aims at; empty: straight ahead

		// Aimed by hand -- a player operating it -- instead of at Target:
		// yaw about up from +z, pitch above level, in degrees. Not saved:
		// it is where somebody left the barrel, not part of the scene.
		bool Manual = false;
		float AimYaw = 0.0f;
		float AimPitch = 8.0f;
	};

	// A camera that flies while nothing else drives it: WASD, Space/Ctrl up
	// and down, Shift faster, the right mouse button held to look. Run by
	// GameSession on the fixed step, so it records and replays.
	struct FlyCameraComponent
	{
		float Speed = 6.0f;            // m/s; Shift triples it
		float LookSpeed = 0.2f;        // degrees per pixel of mouse travel
	};

	// Something the player can walk up to and use. Near enough (Range, from
	// the viewer -- the fly camera, or the body being controlled) and looked
	// at (within 30 degrees), the game shows "[E] Prompt" over it; E runs
	// Action, a game-console command -- "possess Dummy", "terminal",
	// "operate" -- so what pressing E does is data, in the same language the
	// terminal speaks.
	struct InteractableComponent
	{
		std::string Prompt = "Use";
		std::string Action;
		float Range = 2.5f;            // m
	};

	// A game UI screen (Runtime/GameUI.h, a .gsui file) drawn into a texture
	// and shown on this entity's mesh -- a monitor, a sign. "@terminal" is
	// the game console itself. The texture is the running game's, never
	// saved.
	struct ScreenComponent
	{
		std::string Screen;
		int Width = 1024, Height = 640; // texture pixels
		float Emission = 1.0f;         // how much it glows: a screen lights itself
		std::shared_ptr<Texture2D> Texture;
	};

	// Opt-in to the full renderer for a scene: a sun with shadows, a sky,
	// hemisphere ambient, fog, physically based shading and tone mapping.
	// Without one a scene draws exactly as it always has -- the older
	// captures stay byte-identical, which is the reason it is opt-in.
	struct EnvironmentComponent
	{
		float SunAzimuth = 35.0f;      // degrees about up, from +z towards +x: where the sun is
		float SunElevation = 50.0f;    // degrees above the horizon
		glm::vec3 SunColor{ 1.0f, 0.95f, 0.88f };
		float SunIntensity = 3.0f;
		glm::vec3 SkyColor{ 0.36f, 0.55f, 0.85f };      // zenith
		glm::vec3 HorizonColor{ 0.78f, 0.84f, 0.9f };
		glm::vec3 GroundColor{ 0.32f, 0.3f, 0.28f };    // what lights from below
		float AmbientIntensity = 0.6f;
		float FogDensity = 0.012f;     // per metre, at height 0
		float FogFalloff = 0.15f;      // per metre: thinner going up
		float Exposure = 1.0f;
		bool Shadows = true;
		float ShadowExtent = 30.0f;    // m: half-width of what the sun's shadow map covers
		float PointLightScale = 1.0f;  // the scene's point lights, scaled into this exposure
	};

	struct NetworkIdentity
	{
		uint32_t NetworkId = 0;   // assigned by the server when spawned
		ClientId Owner = ServerOwned;
	};

	struct NetworkTransform
	{
		float SendRate = 20.0f;   // Hz -- independent of the fixed-step rate
		bool Interpolate = true;
	};

}
