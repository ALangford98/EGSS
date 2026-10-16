#pragma once
// How a scene looks when it runs: the lit shader (albedo, normal and
// roughness maps from linked procedural materials), per-mesh material
// binding, the scene's lights, and which camera a running game looks
// through. Extracted from EditorSceneView so the editor and GSPlayer draw a
// game with the same code -- which is what lets an editor Play frame and a
// player frame be compared byte for byte. See
// docs/superpowers/specs/2026-10-07-game-runtime-player-design.md.
#include <GS.h>

#include <glm/gtc/matrix_transform.hpp>

#include "MaterialLibrary.h"

#include <GS/Procedural/Prefabs.h>
#include <GS/Procedural/ModelBuilder.h>

#include <unordered_map>

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

// Something drawn that is not an entity's own mesh -- a humanoid's
// mannequin, one item per body. Lit like any mesh, and tagged with the
// entity it belongs to, so clicking it in the editor selects that entity.
struct ExtraDraw
{
	std::shared_ptr<GS::Mesh> Mesh;
	glm::mat4 Model{ 1.0f };
	glm::vec4 Color{ 1.0f };
	GS::EntityId Entity = GS::InvalidEntity;
	// Every submesh, each in its own material (a built-in model, whose
	// submeshes name their Prefabs surfaces) rather than submesh 0 in Color.
	bool AllSubmeshes = false;
	// Under an EnvironmentComponent: how it takes light, and whether it is
	// in the sun's shadow map. A trajectory's dots glow and cast nothing.
	float Roughness = 0.5f, Metallic = 0.0f, Emission = 0.0f;
	bool CastShadow = true;
};

// The rotation (degrees, as TransformComponent holds it) that looks along
// `direction`: ForwardFromRotation's inverse with no roll. That forward is
// (-sin b, cos b sin a, -cos b cos a) for rotation (a, b, 0), so
// b = asin(-dx) and a = atan2(dy, -dz).
inline glm::vec3 RotationLookingAlong(const glm::vec3& direction)
{
	glm::vec3 d = glm::normalize(direction);
	float b = std::asin(glm::clamp(-d.x, -1.0f, 1.0f));
	float a = std::atan2(d.y, -d.z);
	return glm::degrees(glm::vec3(a, b, 0.0f));
}

// What a submesh of a built-in model looks like (GS/Procedural/Prefabs),
// with the entity's colour for the parts that take it. Null for a submesh
// that names no built-in surface -- a model from a file.
inline const GS::Prefabs::Surface* SurfaceOf(const GS::Mesh& mesh, size_t submesh)
{
	const std::vector<GS::Submesh>& submeshes = mesh.GetSubmeshes();
	if (submesh >= submeshes.size() || submeshes[submesh].Material.empty())
		return nullptr;
	return GS::Prefabs::FindSurface(submeshes[submesh].Material);
}

inline glm::vec4 SurfaceColour(const GS::Prefabs::Surface& surface, const glm::vec4& tint)
{
	return surface.Tint ? surface.Color * tint : surface.Color;
}

// Whether an entity is drawn as a humanoid mannequin rather than its mesh.
inline bool IsHumanoidEntity(GS::Scene& scene, GS::EntityId entity)
{
	if (auto* player = scene.GetComponent<GS::PlayerComponent>(entity))
		return player->Body == 2;
	if (auto* npc = scene.GetComponent<GS::NpcComponent>(entity))
		return npc->Body == 2;
	return false;
}

class SceneRenderer
{
public:
	// The ambient floor every lit pixel gets. 0.25, not Cube3D's 0.10: a
	// scene may have no lights at all, and 10% reads as broken, not unlit.
	float AmbientStrength = 0.25f;

	// The scene's first enabled-and-Active CameraComponent with a transform,
	// written into `out` (projection at `aspect`, position, facing). False
	// when there is none -- the editor then keeps its own fly camera.
	static bool FindActiveCamera(GS::Scene& scene, float aspect, GS::PerspectiveCamera& out)
	{
		auto& cameras = scene.View<GS::CameraComponent>();
		for (size_t i = 0; i < cameras.Size(); i++)
		{
			GS::CameraComponent& camera = cameras.Components()[i];
			if (!camera.Active)
				continue;
			auto* transform = scene.GetComponent<GS::TransformComponent>(cameras.Owner(i));
			if (!transform)
				continue;
			out.SetProjection(camera.Fov, aspect, camera.NearClip, camera.FarClip);
			out.SetPosition(transform->Position);
			out.SetOrientation(ForwardFromRotation(transform->Rotation), glm::vec3(0.0f, 1.0f, 0.0f));
			return true;
		}
		return false;
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
	void Init()
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
			out vec2 v_TexCoord;
			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				v_WorldPosition = world.xyz;
				v_Normal = mat3(u_Transform) * a_Normal;
				v_TexCoord = a_TexCoord;
				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			layout(location = 1) out int entityID;
			in vec3 v_WorldPosition;
			in vec3 v_Normal;
			in vec2 v_TexCoord;
			uniform vec4 u_Color;
			uniform int u_EntityID;

			// A linked procedural material's albedo (MeshComponent::
			// MaterialPath). With u_HasAlbedoMap 0 the shader takes exactly
			// the arithmetic it had before this existed -- checked by a
			// byte-identical capture, not assumed.
			uniform sampler2D u_AlbedoMap;
			uniform int u_HasAlbedoMap;

			// The same material's tangent-space normal map (+Y along +v).
			// No vertex carries a tangent, so the frame is rebuilt per pixel
			// from how position and UV change across the screen -- Schueler's
			// cotangent frame. T and B come out along increasing u and v, so
			// mirrored UVs need nothing special; the cost is that they are
			// constant across a triangle, which a low-poly curved mesh can
			// show as a slight shift in bump direction at its edges.
			uniform sampler2D u_NormalMap;
			uniform int u_HasNormalMap;

			// And its roughness, kept in Blinn-Phong rather than moving to a
			// microfacet model (the owner's call: every unlinked object must
			// look as it did). Roughness r maps through the Beckmann
			// correspondence, alpha = r^2 and n = 2/alpha^2 - 2, which puts the
			// fixed 48 below at r ~ 0.447. Strength scales by (n + 8) / 56 --
			// Blinn-Phong's energy normalisation (n + 8) / 8pi, relative to
			// n = 48 -- so a smooth spot is a small bright highlight and a
			// rough one a broad dim one, and r ~ 0.447 is today's 0.35.
			uniform sampler2D u_RoughnessMap;
			uniform int u_HasRoughnessMap;

			vec3 PerturbNormal(vec3 N)
			{
				vec3 dp1 = dFdx(v_WorldPosition);
				vec3 dp2 = dFdy(v_WorldPosition);
				vec2 duv1 = dFdx(v_TexCoord);
				vec2 duv2 = dFdy(v_TexCoord);
				vec3 dp2perp = cross(dp2, N);
				vec3 dp1perp = cross(N, dp1);
				vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
				vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
				// One scale for both, so the frame is independent of how
				// many times the texture tiles, without skewing T against B.
				float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
				vec3 m = texture(u_NormalMap, v_TexCoord).xyz * 2.0 - 1.0;
				return normalize(mat3(T * invmax, B * invmax, N) * m);
			}

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
				if (u_HasNormalMap != 0)
					normal = PerturbNormal(normal);
				vec3 toEye  = normalize(u_CameraPosition - v_WorldPosition);
				vec3 base   = u_Color.rgb;
				float shininess = 48.0;
				float specularStrength = 0.35;
				if (u_HasRoughnessMap != 0)
				{
					float r = texture(u_RoughnessMap, v_TexCoord).r;
					float alpha = r * r;
					shininess = clamp(2.0 / max(alpha * alpha, 1e-6) - 2.0, 1.0, 2048.0);
					specularStrength = 0.35 * (shininess + 8.0) / 56.0;
				}
				if (u_HasAlbedoMap != 0)
					base *= texture(u_AlbedoMap, v_TexCoord).rgb;

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
					float specular = pow(max(dot(normal, halfway), 0.0), shininess);

					lit += base * diffuse * u_LightColors[i] * attenuation
					     + specular * u_LightColors[i] * attenuation * specularStrength;
				}

				color = vec4(lit, u_Color.a);
				entityID = u_EntityID;
			}
		)";

		m_Shader.reset(GS::Shader::Create("EditorSceneView", vertexSrc, fragmentSrc));
		m_SceneMaterial = GS::Material::Create(m_Shader);
		m_SceneMaterial->Set("u_HasAlbedoMap", 0);
		m_SceneMaterial->Set("u_HasNormalMap", 0);
		m_SceneMaterial->Set("u_HasRoughnessMap", 0);
	}

	// Scene-wide (base-material) uniforms every submesh inherits: which
	// enabled lights exist, where the camera is (for specular), and the
	// ambient floor. Capped at 8 -- an editor scene has never needed more,
	// and there's no light-culling here to make a higher cap free.
	void UploadLights(GS::Scene& scene, const glm::vec3& cameraPosition)
	{
		constexpr int kMaxLights = 8;
		auto& lights = scene.View<GS::LightComponent>();

		int count = 0;
		for (size_t i = 0; i < lights.Size() && count < kMaxLights; i++)
		{
			GS::LightComponent& light = lights.Components()[i];
			if (!light.Enabled)
				continue;

			GS::EntityId owner = lights.Owner(i);
			auto* transform = scene.GetComponent<GS::TransformComponent>(owner);
			if (!transform)
				continue;

			std::string index = std::to_string(count);
			m_SceneMaterial->Set("u_LightPositions[" + index + "]", transform->Position);
			m_SceneMaterial->Set("u_LightColors[" + index + "]", glm::vec3(light.Color));
			m_SceneMaterial->Set("u_LightRanges[" + index + "]", light.Radius);
			count++;
		}

		m_SceneMaterial->Set("u_LightCount", count);
		m_SceneMaterial->Set("u_CameraPosition", cameraPosition);
		m_SceneMaterial->Set("u_AmbientStrength", AmbientStrength);
	}

	void Render(GS::Scene& scene, GS::PerspectiveCamera& camera, const std::vector<ExtraDraw>* extra = nullptr)
	{
		// The full renderer, for a scene that asks for it. Everything below
		// this line is the plain one, unchanged, so a scene without an
		// environment draws byte for byte as it always has.
		auto& environments = scene.View<GS::EnvironmentComponent>();
		if (environments.Size() > 0)
		{
			RenderLit(scene, camera, extra, environments.Components()[0]);
			return;
		}

		GS::Renderer::BeginScene(camera);
		UploadLights(scene, camera.GetPosition());

		auto& meshes = scene.View<GS::MeshComponent>();

		for (size_t i = 0; i < meshes.Size(); i++)
		{
			GS::MeshComponent& mesh = meshes.Components()[i];
			if (!mesh.Visible || !mesh.Geometry)
				continue;

			GS::EntityId entity = meshes.Owner(i);
			auto* transform = scene.GetComponent<GS::TransformComponent>(entity);
			if (!transform)
				continue;
			// Its mannequin (in `extra`) is what a humanoid looks like; the
			// mesh it carries is only what it would be with another body.
			if (IsHumanoidEntity(scene, entity))
				continue;

			const std::vector<GS::Submesh>& submeshes = mesh.Geometry->GetSubmeshes();
			if (mesh.Materials.size() < submeshes.size())
				mesh.Materials.resize(submeshes.size());

			// Null for no link or a broken one -- either way, flat colour.
			std::shared_ptr<GS::Texture2D> albedo = mesh.MaterialPath.empty()
				? nullptr : MaterialLibrary::Albedo(mesh.MaterialPath);
			std::shared_ptr<GS::Texture2D> normalMap = mesh.MaterialPath.empty()
				? nullptr : MaterialLibrary::Normal(mesh.MaterialPath);
			std::shared_ptr<GS::Texture2D> roughnessMap = mesh.MaterialPath.empty()
				? nullptr : MaterialLibrary::Roughness(mesh.MaterialPath);

			for (size_t s = 0; s < submeshes.size(); s++)
			{
				if (!mesh.Materials[s])
					mesh.Materials[s] = GS::Material::CreateInstance(m_SceneMaterial);

				if (const GS::Prefabs::Surface* surface = SurfaceOf(*mesh.Geometry, s))
					mesh.Materials[s]->Set("u_Color", SurfaceColour(*surface, mesh.Color));
				else if (!mesh.MaterialsFromFile)
					mesh.Materials[s]->Set("u_Color", mesh.Color);

				mesh.Materials[s]->Set("u_EntityID", (int)GS::EntityIds::Index(entity));

				// Set every frame, both ways: an instance keeps whatever it
				// was last given, so a mesh unlinked since must be told 0.
				mesh.Materials[s]->Set("u_HasAlbedoMap", albedo ? 1 : 0);
				if (albedo)
					mesh.Materials[s]->SetTexture("u_AlbedoMap", albedo, 0);
				mesh.Materials[s]->Set("u_HasNormalMap", normalMap ? 1 : 0);
				if (normalMap)
					mesh.Materials[s]->SetTexture("u_NormalMap", normalMap, 1);
				mesh.Materials[s]->Set("u_HasRoughnessMap", roughnessMap ? 1 : 0);
				if (roughnessMap)
					mesh.Materials[s]->SetTexture("u_RoughnessMap", roughnessMap, 2);

				GS::Renderer::SubmitSubmesh(mesh.Materials[s], mesh.Geometry,
					(unsigned int)s, transform->GetTransform());
			}
		}

		if (extra)
		{
			// One material instance per item and submesh, kept and reused
			// frame to frame: the colour is per instance.
			if (m_ExtraMaterials.size() < extra->size())
				m_ExtraMaterials.resize(extra->size());
			for (size_t i = 0; i < extra->size(); i++)
			{
				const ExtraDraw& item = (*extra)[i];
				if (!item.Mesh)
					continue;
				size_t count = item.AllSubmeshes ? item.Mesh->GetSubmeshes().size() : 1;
				std::vector<std::shared_ptr<GS::Material>>& materials = m_ExtraMaterials[i];
				if (materials.size() < count)
					materials.resize(count);
				for (size_t sub = 0; sub < count; sub++)
				{
					if (!materials[sub])
						materials[sub] = GS::Material::CreateInstance(m_SceneMaterial);
					const std::shared_ptr<GS::Material>& material = materials[sub];
					const GS::Prefabs::Surface* surface = item.AllSubmeshes ? SurfaceOf(*item.Mesh, sub) : nullptr;
					material->Set("u_Color", surface ? SurfaceColour(*surface, item.Color) : item.Color);
					material->Set("u_EntityID", item.Entity == GS::InvalidEntity ? -1 : (int)GS::EntityIds::Index(item.Entity));
					material->Set("u_HasAlbedoMap", 0);
					material->Set("u_HasNormalMap", 0);
					material->Set("u_HasRoughnessMap", 0);
					GS::Renderer::SubmitSubmesh(material, item.Mesh, (unsigned int)sub, item.Model);
				}
			}
		}

		GS::Renderer::EndScene();
	}

	// ---- the full renderer (EnvironmentComponent) -------------------------
	//
	// A forward renderer, one pass per frame plus the sun's shadow map:
	//
	//   shadow  every caster's depth from the sun, orthographic, into a
	//           2048^2 map that follows the camera (snapped to whole texels,
	//           so it does not shimmer as the view moves)
	//   sky     a gradient and a sun disc, behind everything
	//   scene   GGX specular with Schlick's Fresnel and Smith's masking over
	//           Lambert diffuse; the sun shadowed with a 3x3 PCF; point
	//           lights unshadowed; a hemisphere for the ambient and the sky
	//           gradient for what metals reflect; exponential height fog;
	//           then exposure, the ACES fit and sRGB encoding -- in the same
	//           shader, since there is no HDR target to tone-map afterwards.
	//
	// Colours a scene names are sRGB and are linearised first; light is
	// added up in linear units. A point light's colour times pi is its
	// radiance at the old attenuation, so a light that lit a scene before
	// lights it about as brightly now.
	void InitLit()
	{
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform mat4 u_LightViewProjection;
			uniform float u_ShadowNormalOffset;
			out vec3 v_WorldPosition;
			out vec3 v_Normal;
			out vec2 v_TexCoord;
			out vec4 v_LightSpace;
			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				vec3 normal = normalize(mat3(u_Transform) * a_Normal);
				v_WorldPosition = world.xyz;
				v_Normal = normal;
				v_TexCoord = a_TexCoord;
				// Looked up a little off the surface, along its normal: the
				// usual cure for a surface shadowing itself in stripes.
				v_LightSpace = u_LightViewProjection * vec4(world.xyz + normal * u_ShadowNormalOffset, 1.0);
				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			layout(location = 1) out int entityID;
			in vec3 v_WorldPosition;
			in vec3 v_Normal;
			in vec2 v_TexCoord;
			in vec4 v_LightSpace;

			uniform vec4 u_Color;
			uniform float u_Roughness;
			uniform float u_Metallic;
			uniform float u_Emission;
			uniform int u_EntityID;

			uniform sampler2D u_AlbedoMap;
			uniform int u_HasAlbedoMap;
			uniform sampler2D u_NormalMap;
			uniform int u_HasNormalMap;
			uniform sampler2D u_RoughnessMap;
			uniform int u_HasRoughnessMap;
			uniform sampler2D u_ShadowMap;
			uniform int u_HasShadows;
			uniform float u_ShadowTexel;

			const int MAX_LIGHTS = 8;
			uniform int u_LightCount;
			uniform vec3 u_LightPositions[MAX_LIGHTS];
			uniform vec3 u_LightColors[MAX_LIGHTS];
			uniform float u_LightRanges[MAX_LIGHTS];
			uniform float u_PointScale;

			uniform vec3 u_CameraPosition;
			uniform vec3 u_SunDirection;
			uniform vec3 u_SunRadiance;
			uniform vec3 u_SkyColor;
			uniform vec3 u_HorizonColor;
			uniform vec3 u_GroundColor;
			uniform float u_Ambient;
			uniform float u_FogDensity;
			uniform float u_FogFalloff;
			uniform vec3 u_FogColor;
			uniform float u_Exposure;

			const float PI = 3.14159265;

			vec3 ToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

			vec3 PerturbNormal(vec3 N)
			{
				vec3 dp1 = dFdx(v_WorldPosition);
				vec3 dp2 = dFdy(v_WorldPosition);
				vec2 duv1 = dFdx(v_TexCoord);
				vec2 duv2 = dFdy(v_TexCoord);
				vec3 dp2perp = cross(dp2, N);
				vec3 dp1perp = cross(N, dp1);
				vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
				vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
				float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
				vec3 m = texture(u_NormalMap, v_TexCoord).xyz * 2.0 - 1.0;
				return normalize(mat3(T * invmax, B * invmax, N) * m);
			}

			// The sky as a function of direction: the gradient the sky pass
			// draws, which is also what a metal sees reflected.
			vec3 Sky(vec3 d)
			{
				float up = d.y;
				vec3 above = mix(u_HorizonColor, u_SkyColor, pow(clamp(up, 0.0, 1.0), 0.6));
				vec3 below = mix(u_HorizonColor, u_GroundColor, clamp(-up * 3.0, 0.0, 1.0));
				return up >= 0.0 ? above : below;
			}

			float Shadow(float ndotl)
			{
				if (u_HasShadows == 0)
					return 1.0;
				vec3 p = v_LightSpace.xyz / v_LightSpace.w * 0.5 + 0.5;
				if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0)
					return 1.0;
				// Steeper to the sun, more bias: a grazing surface's depth
				// changes fastest across one texel.
				float bias = 0.0006 + 0.0025 * (1.0 - ndotl);
				float lit = 0.0;
				for (int y = -1; y <= 1; y++)
					for (int x = -1; x <= 1; x++)
					{
						float depth = texture(u_ShadowMap, p.xy + vec2(x, y) * u_ShadowTexel).r;
						lit += p.z - bias > depth ? 0.0 : 1.0;
					}
				return lit / 9.0;
			}

			// GGX/Trowbridge-Reitz, Smith-Schlick (k = (r + 1)^2 / 8),
			// Schlick's Fresnel: the light reflected towards the eye from
			// light arriving along L, per unit irradiance.
			vec3 Brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float rough, float metal, vec3 F0)
			{
				vec3 H = normalize(V + L);
				float ndotl = max(dot(N, L), 0.0);
				float ndotv = max(dot(N, V), 1e-4);
				float ndoth = max(dot(N, H), 0.0);
				float vdoth = max(dot(V, H), 0.0);
				float a = rough * rough;
				float a2 = a * a;
				float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
				float D = a2 / (PI * d * d);
				float k = (rough + 1.0) * (rough + 1.0) / 8.0;
				float G = (ndotl / (ndotl * (1.0 - k) + k)) * (ndotv / (ndotv * (1.0 - k) + k));
				vec3 F = F0 + (1.0 - F0) * pow(1.0 - vdoth, 5.0);
				vec3 specular = D * G * F / max(4.0 * ndotl * ndotv, 1e-4);
				vec3 diffuse = (1.0 - F) * (1.0 - metal) * albedo / PI;
				return diffuse + specular;
			}

			vec3 Aces(vec3 x)
			{
				return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
			}

			void main()
			{
				vec3 N = normalize(v_Normal);
				if (!gl_FrontFacing)
					N = -N;
				if (u_HasNormalMap != 0)
					N = PerturbNormal(N);
				vec3 V = normalize(u_CameraPosition - v_WorldPosition);

				vec3 albedo = ToLinear(u_Color.rgb);
				if (u_HasAlbedoMap != 0)
					albedo *= ToLinear(texture(u_AlbedoMap, v_TexCoord).rgb);
				float rough = clamp(u_Roughness, 0.04, 1.0);
				if (u_HasRoughnessMap != 0)
					rough = clamp(texture(u_RoughnessMap, v_TexCoord).r, 0.04, 1.0);
				float metal = clamp(u_Metallic, 0.0, 1.0);
				vec3 F0 = mix(vec3(0.04), albedo, metal);

				// The sun.
				float sunDot = max(dot(N, u_SunDirection), 0.0);
				vec3 lit = Brdf(N, V, u_SunDirection, albedo, rough, metal, F0) * u_SunRadiance * sunDot * Shadow(sunDot);

				// The scene's point lights, with the plain shader's falloff.
				for (int i = 0; i < u_LightCount; i++)
				{
					vec3 lightVector = u_LightPositions[i] - v_WorldPosition;
					float distance = length(lightVector);
					vec3 L = lightVector / max(distance, 1e-4);
					float attenuation = 1.0 / (1.0 + 0.08 * distance * distance);
					attenuation *= clamp(1.0 - distance / max(u_LightRanges[i], 1e-4), 0.0, 1.0);
					float ndotl = max(dot(N, L), 0.0);
					lit += Brdf(N, V, L, albedo, rough, metal, F0) * u_LightColors[i] * (PI * u_PointScale) * attenuation * ndotl;
				}

				// Ambient: the hemisphere for diffuse, the sky for what a
				// smooth surface reflects -- blurred towards the hemisphere's
				// average as it gets rough.
				vec3 irradiance = mix(u_GroundColor, u_SkyColor, N.y * 0.5 + 0.5) * u_Ambient;
				float ndotv = max(dot(N, V), 0.0);
				vec3 Fa = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndotv, 5.0);
				vec3 reflected = mix(Sky(reflect(-V, N)) * u_Ambient, irradiance, rough * rough);
				lit += (1.0 - Fa) * (1.0 - metal) * albedo * irradiance + Fa * reflected * (1.0 - 0.6 * rough);

				// Something that glows is its own light: a screen or a lamp
				// is seen by what it emits, and the room's light on its
				// glass would only grey it. Below 1 the glow is added to the
				// lit surface -- an LED on a dark case.
				if (u_Emission >= 1.0)
					lit = albedo * u_Emission;
				else
					lit += albedo * u_Emission;

				// Height fog, integrated along the view ray: density falls
				// off as exp(-k y), so the fog met between eye and point is
				// (d / k) e^(-k y_eye) (1 - e^(-k dy)) / dy per unit length.
				vec3 ray = v_WorldPosition - u_CameraPosition;
				float distance = length(ray);
				float dy = ray.y;
				float k = max(u_FogFalloff, 1e-4);
				float amount = u_FogDensity * exp(-k * u_CameraPosition.y) * distance
					* (abs(k * dy) > 1e-4 ? (1.0 - exp(-k * dy)) / (k * dy) : 1.0);
				lit = mix(lit, u_FogColor, 1.0 - exp(-amount));

				vec3 mapped = Aces(lit * u_Exposure);
				color = vec4(pow(mapped, vec3(1.0 / 2.2)), u_Color.a);
				entityID = u_EntityID;
			}
		)";

		m_LitShader.reset(GS::Shader::Create("SceneLit", vertexSrc, fragmentSrc));
		m_LitMaterial = GS::Material::Create(m_LitShader);
		m_LitMaterial->Set("u_HasAlbedoMap", 0);
		m_LitMaterial->Set("u_HasNormalMap", 0);
		m_LitMaterial->Set("u_HasRoughnessMap", 0);
		m_LitMaterial->Set("u_HasShadows", 0);

		std::string depthVertex = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform mat4 u_LightViewProjection;
			void main()
			{
				gl_Position = u_LightViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";
		std::string depthFragment = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			void main() { color = vec4(1.0); }
		)";
		m_DepthShader.reset(GS::Shader::Create("SceneShadow", depthVertex, depthFragment));
		m_DepthMaterial = GS::Material::Create(m_DepthShader);

		std::string skyVertex = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform mat4 u_InverseViewProjection;
			out vec3 v_Far;
			void main()
			{
				// A quad over the whole view at the far plane; the ray
				// through each corner is where that corner looks.
				vec2 clip = a_Position.xy * 2.0;
				vec4 far = u_InverseViewProjection * vec4(clip, 1.0, 1.0);
				v_Far = far.xyz / far.w;
				gl_Position = vec4(clip, 0.99999, 1.0);
			}
		)";
		std::string skyFragment = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			layout(location = 1) out int entityID;
			in vec3 v_Far;
			uniform vec3 u_CameraPosition;
			uniform vec3 u_SunDirection;
			uniform vec3 u_SunRadiance;
			uniform vec3 u_SkyColor;
			uniform vec3 u_HorizonColor;
			uniform vec3 u_GroundColor;
			uniform float u_SkyBrightness;
			uniform float u_Exposure;
			vec3 Aces(vec3 x)
			{
				return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
			}
			void main()
			{
				vec3 d = normalize(v_Far - u_CameraPosition);
				float up = d.y;
				vec3 sky = up >= 0.0 ? mix(u_HorizonColor, u_SkyColor, pow(clamp(up, 0.0, 1.0), 0.6))
					: mix(u_HorizonColor, u_GroundColor, clamp(-up * 3.0, 0.0, 1.0));
				sky *= u_SkyBrightness;
				float s = max(dot(d, u_SunDirection), 0.0);
				sky += u_SunRadiance * (smoothstep(0.99985, 0.9999, s) * 40.0 + pow(s, 64.0) * 0.6 + pow(s, 8.0) * 0.08);
				color = vec4(pow(Aces(sky * u_Exposure), vec3(1.0 / 2.2)), 1.0);
				entityID = -1;
			}
		)";
		m_SkyShader.reset(GS::Shader::Create("SceneSky", skyVertex, skyFragment));
		m_SkyMaterial = GS::Material::Create(m_SkyShader);
		GS::ModelBuilder quad;
		quad.Quad({ 0.5f, 0.5f });
		m_SkyQuad = std::make_shared<GS::Mesh>(quad.Build(), "SkyQuad");
	}

	static glm::vec3 Linear(const glm::vec3& c) { return glm::pow(glm::max(c, glm::vec3(0.0f)), glm::vec3(2.2f)); }

	void RenderLit(GS::Scene& scene, GS::PerspectiveCamera& camera, const std::vector<ExtraDraw>* extra,
		const GS::EnvironmentComponent& env)
	{
		if (!m_LitShader)
			InitLit();

		float azimuth = glm::radians(env.SunAzimuth), elevation = glm::radians(env.SunElevation);
		glm::vec3 sun(std::sin(azimuth) * std::cos(elevation), std::sin(elevation), std::cos(azimuth) * std::cos(elevation));

		// Everything drawn, once, as (mesh, submesh, transform, appearance).
		struct Item
		{
			std::shared_ptr<GS::Mesh> Mesh;
			unsigned int Submesh;
			glm::mat4 Model;
			glm::vec4 Color;
			float Roughness, Metallic, Emission;
			bool CastShadow;
			int Entity;
			std::shared_ptr<GS::Texture2D> Albedo, Normal, Rough;
		};
		std::vector<Item> items;

		auto& meshes = scene.View<GS::MeshComponent>();
		for (size_t i = 0; i < meshes.Size(); i++)
		{
			GS::MeshComponent& mesh = meshes.Components()[i];
			if (!mesh.Visible || !mesh.Geometry)
				continue;
			GS::EntityId entity = meshes.Owner(i);
			auto* transform = scene.GetComponent<GS::TransformComponent>(entity);
			if (!transform || IsHumanoidEntity(scene, entity))
				continue;
			std::shared_ptr<GS::Texture2D> albedo = mesh.MaterialPath.empty() ? nullptr : MaterialLibrary::Albedo(mesh.MaterialPath);
			std::shared_ptr<GS::Texture2D> normal = mesh.MaterialPath.empty() ? nullptr : MaterialLibrary::Normal(mesh.MaterialPath);
			std::shared_ptr<GS::Texture2D> rough = mesh.MaterialPath.empty() ? nullptr : MaterialLibrary::Roughness(mesh.MaterialPath);
			float emission = mesh.Emission;
			// A screen shows its texture, glowing.
			if (auto* screen = scene.GetComponent<GS::ScreenComponent>(entity); screen && screen->Texture)
			{
				albedo = screen->Texture;
				emission = screen->Emission;
			}
			glm::mat4 model = transform->GetTransform();
			const std::vector<GS::Submesh>& submeshes = mesh.Geometry->GetSubmeshes();
			for (size_t sub = 0; sub < submeshes.size(); sub++)
			{
				Item item{ mesh.Geometry, (unsigned int)sub, model, mesh.Color, mesh.Roughness, mesh.Metallic, emission, true,
					(int)GS::EntityIds::Index(entity), albedo, normal, rough };
				if (const GS::Prefabs::Surface* surface = SurfaceOf(*mesh.Geometry, sub))
				{
					item.Color = SurfaceColour(*surface, mesh.Color);
					item.Roughness = surface->Roughness;
					item.Metallic = surface->Metallic;
					item.Emission = surface->Emission;
				}
				// Glowing things are their own light: they cast no shadow.
				item.CastShadow = item.Emission < 0.5f;
				items.push_back(item);
			}
		}
		if (extra)
			for (const ExtraDraw& draw : *extra)
			{
				if (!draw.Mesh)
					continue;
				size_t count = draw.AllSubmeshes ? draw.Mesh->GetSubmeshes().size() : 1;
				for (size_t sub = 0; sub < count; sub++)
				{
					Item item{ draw.Mesh, (unsigned int)sub, draw.Model, draw.Color, draw.Roughness, draw.Metallic, draw.Emission,
						draw.CastShadow, draw.Entity == GS::InvalidEntity ? -1 : (int)GS::EntityIds::Index(draw.Entity), nullptr, nullptr, nullptr };
					if (draw.AllSubmeshes)
						if (const GS::Prefabs::Surface* surface = SurfaceOf(*draw.Mesh, sub))
						{
							item.Color = SurfaceColour(*surface, draw.Color);
							item.Roughness = surface->Roughness;
							item.Metallic = surface->Metallic;
							item.Emission = surface->Emission;
						}
					items.push_back(item);
				}
			}

		GS::Renderer::BeginScene(camera);

		// The sun's shadow map, around where the camera is looking.
		glm::mat4 lightViewProjection(1.0f);
		bool shadows = env.Shadows && env.SunElevation > 0.0f;
		if (shadows)
		{
			const int size = 2048;
			if (!m_ShadowMap)
			{
				GS::FramebufferSpecification spec;
				spec.Width = size;
				spec.Height = size;
				spec.Attachments = { GS::FramebufferTextureFormat::DEPTH24STENCIL8 };
				m_ShadowMap.reset(GS::Framebuffer::Create(spec));
				m_ShadowTexture.reset(GS::Texture2D::CreateFromHandle(m_ShadowMap->GetDepthAttachmentRendererID(), size, size));
			}
			float extent = std::max(1.0f, env.ShadowExtent);
			glm::vec3 forward = camera.GetForward();
			glm::vec3 centre = camera.GetPosition() + forward * (extent * 0.5f);
			glm::vec3 up = std::abs(sun.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			glm::mat4 view = glm::lookAt(centre + sun * 150.0f, centre, up);
			// Snapped to whole texels in the light's frame: a map that slides
			// by fractions of a texel as the camera moves makes every shadow
			// edge crawl.
			float texel = 2.0f * extent / size;
			glm::vec4 origin = view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			glm::vec2 snapped = glm::round(glm::vec2(origin) / texel) * texel;
			view = glm::translate(glm::mat4(1.0f), glm::vec3(snapped - glm::vec2(origin), 0.0f)) * view;
			glm::mat4 projection = glm::ortho(-extent, extent, -extent, extent, 1.0f, 300.0f);
			lightViewProjection = projection * view;

			GS::RenderCommand::PushTarget();
			m_ShadowMap->Bind();
			GS::RenderCommand::Clear();
			GS::RenderCommand::SetCullFace(GS::CullFace::None);
			m_DepthMaterial->Set("u_LightViewProjection", lightViewProjection);
			for (const Item& item : items)
				if (item.CastShadow)
					GS::Renderer::SubmitSubmesh(m_DepthMaterial, item.Mesh, item.Submesh, item.Model);
			GS::RenderCommand::PopTarget();
			GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		}

		glm::vec3 sunRadiance = env.SunColor * env.SunIntensity;
		glm::vec3 skyColor = Linear(env.SkyColor), horizon = Linear(env.HorizonColor), ground = Linear(env.GroundColor);

		// The sky first, behind everything and writing no depth.
		GS::RenderCommand::SetDepthTest(false);
		GS::RenderCommand::SetDepthWrite(false);
		m_SkyMaterial->Set("u_InverseViewProjection", glm::inverse(camera.GetViewProjectionMatrix()));
		m_SkyMaterial->Set("u_CameraPosition", camera.GetPosition());
		m_SkyMaterial->Set("u_SunDirection", sun);
		m_SkyMaterial->Set("u_SunRadiance", sunRadiance);
		m_SkyMaterial->Set("u_SkyColor", skyColor);
		m_SkyMaterial->Set("u_HorizonColor", horizon);
		m_SkyMaterial->Set("u_GroundColor", ground);
		m_SkyMaterial->Set("u_SkyBrightness", 1.6f);
		m_SkyMaterial->Set("u_Exposure", env.Exposure);
		GS::Renderer::SubmitSubmesh(m_SkyMaterial, m_SkyQuad, 0, glm::mat4(1.0f));
		GS::RenderCommand::SetDepthWrite(true);
		GS::RenderCommand::SetDepthTest(true);

		// The scene.
		UploadLightsTo(*m_LitMaterial, scene, camera.GetPosition());
		GS::Material& base = *m_LitMaterial;
		base.Set("u_PointScale", env.PointLightScale);
		base.Set("u_SunDirection", sun);
		base.Set("u_SunRadiance", sunRadiance);
		base.Set("u_SkyColor", skyColor);
		base.Set("u_HorizonColor", horizon);
		base.Set("u_GroundColor", ground);
		base.Set("u_Ambient", env.AmbientIntensity);
		base.Set("u_FogDensity", env.FogDensity);
		base.Set("u_FogFalloff", env.FogFalloff);
		base.Set("u_FogColor", horizon * env.AmbientIntensity);
		base.Set("u_Exposure", env.Exposure);
		base.Set("u_LightViewProjection", lightViewProjection);
		base.Set("u_ShadowNormalOffset", shadows ? 2.5f * (2.0f * std::max(1.0f, env.ShadowExtent) / 2048.0f) : 0.0f);
		base.Set("u_HasShadows", shadows ? 1 : 0);
		base.Set("u_ShadowTexel", 1.0f / 2048.0f);
		if (shadows)
			base.SetTexture("u_ShadowMap", m_ShadowTexture, 3);

		if (m_LitInstances.size() < items.size())
			m_LitInstances.resize(items.size());
		for (size_t i = 0; i < items.size(); i++)
		{
			const Item& item = items[i];
			if (!m_LitInstances[i])
				m_LitInstances[i] = GS::Material::CreateInstance(m_LitMaterial);
			GS::Material& m = *m_LitInstances[i];
			m.Set("u_Color", item.Color);
			m.Set("u_Roughness", item.Roughness);
			m.Set("u_Metallic", item.Metallic);
			m.Set("u_Emission", item.Emission);
			m.Set("u_EntityID", item.Entity);
			m.Set("u_HasAlbedoMap", item.Albedo ? 1 : 0);
			if (item.Albedo)
				m.SetTexture("u_AlbedoMap", item.Albedo, 0);
			m.Set("u_HasNormalMap", item.Normal ? 1 : 0);
			if (item.Normal)
				m.SetTexture("u_NormalMap", item.Normal, 1);
			m.Set("u_HasRoughnessMap", item.Rough ? 1 : 0);
			if (item.Rough)
				m.SetTexture("u_RoughnessMap", item.Rough, 2);
			GS::Renderer::SubmitSubmesh(m_LitInstances[i], item.Mesh, item.Submesh, item.Model);
		}

		GS::Renderer::EndScene();
	}

	// The scene's enabled lights into a material, as UploadLights does for
	// the plain one.
	static void UploadLightsTo(GS::Material& material, GS::Scene& scene, const glm::vec3& cameraPosition)
	{
		constexpr int kMaxLights = 8;
		auto& lights = scene.View<GS::LightComponent>();
		int count = 0;
		for (size_t i = 0; i < lights.Size() && count < kMaxLights; i++)
		{
			GS::LightComponent& light = lights.Components()[i];
			auto* transform = scene.GetComponent<GS::TransformComponent>(lights.Owner(i));
			if (!light.Enabled || !transform)
				continue;
			std::string index = std::to_string(count);
			material.Set("u_LightPositions[" + index + "]", transform->Position);
			material.Set("u_LightColors[" + index + "]", Linear(glm::vec3(light.Color)));
			material.Set("u_LightRanges[" + index + "]", light.Radius);
			count++;
		}
		material.Set("u_LightCount", count);
		material.Set("u_CameraPosition", cameraPosition);
	}

private:
	std::vector<std::vector<std::shared_ptr<GS::Material>>> m_ExtraMaterials;
	std::shared_ptr<GS::Shader> m_LitShader, m_DepthShader, m_SkyShader;
	std::shared_ptr<GS::Material> m_LitMaterial, m_DepthMaterial, m_SkyMaterial;
	std::vector<std::shared_ptr<GS::Material>> m_LitInstances;
	std::shared_ptr<GS::Mesh> m_SkyQuad;
	std::unique_ptr<GS::Framebuffer> m_ShadowMap;
	std::shared_ptr<GS::Texture2D> m_ShadowTexture;
	std::shared_ptr<GS::Shader> m_Shader;
	std::shared_ptr<GS::Material> m_SceneMaterial;
};
