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

	void Render(GS::Scene& scene, GS::PerspectiveCamera& camera)
	{
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

				if (!mesh.MaterialsFromFile)
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

		GS::Renderer::EndScene();
	}

private:
	std::shared_ptr<GS::Shader> m_Shader;
	std::shared_ptr<GS::Material> m_SceneMaterial;
};
