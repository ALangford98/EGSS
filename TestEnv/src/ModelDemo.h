#pragma once

// glTF 2.0: a model that is a *tree*, not a bag of triangles.
//
// The figure here is one 24-vertex cube. Every limb is that same cube seen
// through a different chain of transforms -- so the whole thing is 840 bytes of
// geometry and twenty nodes. An .obj of the same figure would be eleven baked
// copies of the cube's vertices and no way to tell which was an arm.
//
// Three things this demo exists to show:
//
//   * **The hierarchy composes.** The forearm hangs at an angle because its
//     parent elbow node is rotated, not because its vertices are. Turn on
//     "spin the shoulders" and the forearms follow, because the transform they
//     inherit changed and nothing else did.
//
//   * **Two containers, one model.** `figure.gltf` names its data in files
//     beside it; `figure.glb` carries the same bytes -- including the PNG --
//     inside one file. Loading both and comparing the geometry byte for byte
//     is the check that the container is not quietly changing the model.
//
//   * **A scaled parent is the classic trap.** The torso node is scaled
//     1.0 x 1.4 x 0.55. If the limbs hung off *it* rather than off an unscaled
//     sibling, every one of them would be squashed to 55% depth. The asset is
//     built the right way round on purpose, and the panel shows the tree so
//     the shape of it is visible rather than asserted.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Demo.h"

class ModelDemo : public DemoLayer
{
public:
	ModelDemo()
		: DemoLayer("Model"), m_Camera(50.0f, 16.0f / 9.0f, 0.1f, 200.0f)
	{
		RegisterParam("Model spin", &m_Spin);
		RegisterParam("Model shoulder", &m_ShoulderSpin);
	}

	void OnDemoAttach() override
	{
		BuildShader();
		LoadBoth();
	}
private:
	// --- Loading ------------------------------------------------------------

	// Everything one loaded file turns into: geometry on the GPU, one material
	// instance per glTF material, and the textures those materials name.
	//
	// Kept per file rather than merged, because the whole point of loading both
	// is to be able to say the two agree.
	struct Loaded
	{
		Egss::GltfModel Model;
		std::vector<std::shared_ptr<Egss::Mesh>> Meshes;
		std::vector<std::shared_ptr<Egss::Material>> Materials;
		std::vector<std::shared_ptr<Egss::Texture2D>> Textures;
		std::string Error;
		bool Ok = false;
	};

	void LoadBoth()
	{
		Load("assets/models/figure.gltf", m_Gltf);
		Load("assets/models/figure.glb", m_Glb);

		CompareContainers();
	}

	void Load(const std::string& path, Loaded& out)
	{
		out = Loaded();

		if (!Egss::GltfLoader::Load(path, out.Model, out.Error))
		{
			EGSS_ERROR("{0}", out.Error);
			return;
		}

		// Images first: a material needs the texture it points at to exist.
		out.Textures.resize(out.Model.Images.size());
		for (size_t i = 0; i < out.Model.Images.size(); i++)
		{
			const Egss::GltfImage& image = out.Model.Images[i];

			// The two shapes an image arrives in, and the reason
			// CreateFromMemory had to exist: a .glb's PNG never was a file.
			if (image.IsEmbedded())
				out.Textures[i].reset(Egss::Texture2D::CreateFromMemory(
					image.Bytes.data(), image.Bytes.size(), image.Name));
			else if (!image.Path.empty())
				out.Textures[i].reset(Egss::Texture2D::Create(image.Path));
		}

		out.Meshes.reserve(out.Model.Meshes.size());
		for (const Egss::MeshData& data : out.Model.Meshes)
			out.Meshes.push_back(std::make_shared<Egss::Mesh>(data, "gltf"));

		out.Materials.reserve(out.Model.Materials.size());
		for (const Egss::GltfMaterial& material : out.Model.Materials)
		{
			std::shared_ptr<Egss::Material> instance =
				Egss::Material::CreateInstance(m_SceneMaterial);

			instance->Set("u_BaseColour", material.BaseColour);
			instance->Set("u_Metallic", material.Metallic);
			instance->Set("u_Roughness", material.Roughness);
			instance->Set("u_Emissive", material.Emissive);

			// A material with no texture still has to sample something, or the
			// shader reads an unbound unit. A 1x1 white pixel is the standard
			// answer: white multiplied by the base colour factor *is* the base
			// colour, so one code path draws both cases.
			bool textured = material.BaseColourImage >= 0
				&& (size_t)material.BaseColourImage < out.Textures.size()
				&& out.Textures[material.BaseColourImage];

			instance->SetTexture("u_BaseColourMap",
				textured ? out.Textures[material.BaseColourImage] : m_White);
			instance->Set("u_HasTexture", textured ? 1 : 0);

			out.Materials.push_back(instance);
		}

		out.Ok = true;
		EGSS_INFO("{0}: {1} nodes, {2} instances, {3} triangles, generator '{4}'",
			path, out.Model.Nodes.size(), out.Model.Instances.size(),
			out.Model.TriangleCount(), out.Model.Generator);
	}

	// The two files are written from one description by one script, so any
	// difference is the loader's. Compared here rather than in a test because
	// it costs nothing and it is the kind of thing that breaks silently.
	void CompareContainers()
	{
		m_Comparison = "not loaded";
		if (!m_Gltf.Ok || !m_Glb.Ok)
			return;

		if (m_Gltf.Model.Meshes.size() != m_Glb.Model.Meshes.size()
			|| m_Gltf.Model.Instances.size() != m_Glb.Model.Instances.size())
		{
			m_Comparison = "the two containers disagree on structure";
			m_ContainersAgree = false;
			return;
		}

		size_t vertices = 0;
		for (size_t i = 0; i < m_Gltf.Model.Meshes.size(); i++)
		{
			const Egss::MeshData& a = m_Gltf.Model.Meshes[i];
			const Egss::MeshData& b = m_Glb.Model.Meshes[i];

			if (a.Vertices.size() != b.Vertices.size() || a.Indices != b.Indices
				|| std::memcmp(a.Vertices.data(), b.Vertices.data(),
					a.Vertices.size() * sizeof(Egss::MeshVertex)) != 0)
			{
				m_Comparison = "mesh " + std::to_string(i) + " differs between the containers";
				m_ContainersAgree = false;
				return;
			}

			vertices += a.Vertices.size();
		}

		// And the placements, which live in the node tree rather than the mesh.
		for (size_t i = 0; i < m_Gltf.Model.Instances.size(); i++)
		{
			if (std::memcmp(&m_Gltf.Model.Instances[i].Transform,
				&m_Glb.Model.Instances[i].Transform, sizeof(glm::mat4)) != 0)
			{
				m_Comparison = "instance " + std::to_string(i) + " is placed differently";
				m_ContainersAgree = false;
				return;
			}
		}

		m_ContainersAgree = true;
		m_Comparison = ".gltf and .glb agree exactly: " + std::to_string(vertices)
			+ " vertices and " + std::to_string(m_Gltf.Model.Instances.size())
			+ " placements, byte for byte";
	}

	const Loaded& Active() const { return m_ShowGlb ? m_Glb : m_Gltf; }

	// --- Update -------------------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		float dt = step;

		m_Time += dt;

		if (m_Spin)
			m_Orbit += 25.0f * dt;

		// Turning the shoulder nodes and re-flattening is the whole argument
		// for a hierarchy: two numbers change, and four limb segments move,
		// because the forearms inherit what the shoulders did.
		if (m_ShoulderSpin)
			Animate();
	}

	void Animate()
	{
		Loaded& active = m_ShowGlb ? m_Glb : m_Gltf;
		if (!active.Ok)
			return;

		float swing = std::sin(m_Time * 1.5f) * 35.0f;

		for (Egss::GltfNode& node : active.Model.Nodes)
		{
			if (node.Name == "shoulder.L" || node.Name == "shoulder.R")
			{
				float sign = node.Name == "shoulder.L" ? 1.0f : -1.0f;

				// Rebuilt from the original translation rather than multiplied
				// onto the existing matrix, or the error accumulates and the
				// arms drift off the body over a minute.
				glm::vec3 offset(0.62f * sign, 0.62f, 0.0f);
				node.Local = glm::translate(glm::mat4(1.0f), offset)
					* glm::rotate(glm::mat4(1.0f), glm::radians(12.0f * sign),
						glm::vec3(0.0f, 0.0f, 1.0f))
					* glm::rotate(glm::mat4(1.0f), glm::radians(swing),
						glm::vec3(1.0f, 0.0f, 0.0f));
			}
		}

		Reflatten(active.Model);
	}

	// Re-walks the tree and recomputes every instance transform.
	//
	// The loader does this once at load; doing it again is what makes a moved
	// node move everything beneath it. Kept here rather than in the engine
	// because a real animation system would drive it from samplers, and this
	// demo is not that -- see the note about skinning in GltfLoader.h.
	static void Reflatten(Egss::GltfModel& model)
	{
		model.Instances.clear();

		std::vector<bool> visited(model.Nodes.size(), false);
		for (int root : model.RootNodes)
			Walk(model, root, glm::mat4(1.0f), visited);
	}

	static void Walk(Egss::GltfModel& model, int index, const glm::mat4& parent,
		std::vector<bool>& visited)
	{
		if (index < 0 || (size_t)index >= model.Nodes.size() || visited[index])
			return;

		visited[index] = true;

		const Egss::GltfNode& node = model.Nodes[index];
		glm::mat4 world = parent * node.Local;

		if (node.Mesh >= 0)
		{
			Egss::GltfInstance instance;
			instance.Node = index;
			instance.Mesh = node.Mesh;
			instance.Transform = world;
			model.Instances.push_back(instance);
		}

		for (int child : node.Children)
			Walk(model, child, world, visited);
	}

	// --- Draw ---------------------------------------------------------------

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		(void)ts;

		Egss::RenderCommand::SetClearColor({ 0.09f, 0.10f, 0.13f, 1.0f });
		Egss::RenderCommand::Clear();

		const Loaded& active = Active();
		if (!active.Ok)
			return;

		FrameCamera(active.Model);

		Egss::Renderer::BeginScene(m_Camera);

		m_SceneMaterial->Set("u_LightDirection", glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)));
		m_SceneMaterial->Set("u_CameraPosition", m_Camera.GetPosition());
		m_SceneMaterial->Set("u_Ambient", m_Ambient);

		if (m_Wireframe)
			Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Line);

		for (const Egss::GltfInstance& instance : active.Model.Instances)
		{
			const std::shared_ptr<Egss::Mesh>& mesh = active.Meshes[instance.Mesh];

			for (unsigned int s = 0; s < (unsigned int)mesh->GetSubmeshes().size(); s++)
			{
				int materialIndex = mesh->GetSubmeshes()[s].MaterialIndex;

				std::shared_ptr<Egss::Material> material =
					(materialIndex >= 0 && (size_t)materialIndex < active.Materials.size())
					? active.Materials[materialIndex] : m_Fallback;

				// The inverse transpose: the matrix that keeps a normal
				// perpendicular to what it was perpendicular to under a
				// non-uniform scale, which the model matrix does not.
				//
				// It is correct in general and wrong to leave out of a loader
				// that will be handed arbitrary files. But it is worth being
				// honest about this asset: substituting the plain model matrix
				// here produces a **byte-identical** capture, measured.
				//
				// The reason is arithmetic rather than luck. Each world
				// transform is R*S -- rotations on the parent nodes, a diagonal
				// scale on the leaf -- and for diagonal S, (R*S)^-T is R*S^-1.
				// Both send the cube's axis-aligned normal e_x to a vector
				// along R*e_x, differing only in length, and the shader
				// normalises. A sheared node, or normals not aligned with the
				// scale axes, would separate them; a box under an axis-aligned
				// scale cannot.
				glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(instance.Transform)));
				material->Set("u_NormalMatrix", glm::mat4(normalMatrix));

				Egss::Renderer::SubmitSubmesh(material, mesh, s,
					m_Model * instance.Transform);
			}
		}

		if (m_Wireframe)
			Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Fill);

		Egss::Renderer::EndScene();
	}

	// Frames whatever was loaded, rather than assuming a size. A model's units
	// are its author's business -- this one is about 3 m tall, but the next
	// file might be in centimetres.
	void FrameCamera(const Egss::GltfModel& model)
	{
		float radius = model.BoundsRadius();
		if (radius < 1e-4f)
			radius = 1.0f;

		float distance = radius / std::tan(glm::radians(50.0f * 0.5f)) * 1.4f;

		glm::vec3 centre = model.BoundsCentre();
		float angle = glm::radians(m_Orbit);

		m_Camera.SetPosition(centre + glm::vec3(
			std::sin(angle) * distance, radius * 0.35f, std::cos(angle) * distance));

		// Yaw 0 looks along +x here, so the angle that points back at the
		// centre from an orbit position is -(orbit) - 90 degrees.
		m_Camera.SetRotation(-m_Orbit - 90.0f, -8.0f);
	}

	// --- Panel --------------------------------------------------------------

	void OnDemoImGui() override
	{
		ImGui::Begin("glTF model");

		const Loaded& active = Active();

		if (!active.Ok)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", active.Error.c_str());
			ImGui::End();
			return;
		}

		if (ImGui::RadioButton(".gltf + .bin + .png", !m_ShowGlb)) m_ShowGlb = false;
		ImGui::SameLine();
		if (ImGui::RadioButton(".glb", m_ShowGlb)) m_ShowGlb = true;

		ImGui::TextColored(m_ContainersAgree ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f)
											 : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
			"%s", m_Comparison.c_str());

		ImGui::Separator();
		ImGui::Text("%zu nodes, %zu carrying a mesh", active.Model.Nodes.size(),
			active.Model.Instances.size());
		ImGui::Text("%zu meshes, %zu triangles in total", active.Model.Meshes.size(),
			active.Model.TriangleCount());
		ImGui::Text("generator: %s", active.Model.Generator.c_str());

		glm::vec3 size = active.Model.BoundsSize();
		ImGui::Text("world bounds %.2f x %.2f x %.2f m", size.x, size.y, size.z);

		ImGui::Separator();
		ImGui::Checkbox("Spin", &m_Spin);
		ImGui::SameLine();
		ImGui::Checkbox("Swing the shoulders", &m_ShoulderSpin);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Rotates two nodes. Four limb segments move, because\n"
				"the forearms inherit the shoulders' transform -- which is\n"
				"the thing a hierarchy buys you over baked vertices.");

		ImGui::SliderFloat("Ambient", &m_Ambient, 0.0f, 1.0f);
		ImGui::Checkbox("Wireframe", &m_Wireframe);

		ImGui::Separator();
		if (ImGui::TreeNodeEx("Node tree", ImGuiTreeNodeFlags_DefaultOpen))
		{
			for (int root : active.Model.RootNodes)
				DrawNode(active.Model, root);

			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Materials"))
		{
			for (size_t i = 0; i < active.Model.Materials.size(); i++)
			{
				const Egss::GltfMaterial& material = active.Model.Materials[i];
				ImGui::ColorButton(("##c" + std::to_string(i)).c_str(),
					ImVec4(material.BaseColour.r, material.BaseColour.g,
						material.BaseColour.b, 1.0f));
				ImGui::SameLine();
				ImGui::Text("%s  metallic %.2f  rough %.2f%s", material.Name.c_str(),
					material.Metallic, material.Roughness,
					material.BaseColourImage >= 0 ? "  [textured]" : "");
			}

			ImGui::TreePop();
		}

		ImGui::End();
	}

	void DrawNode(const Egss::GltfModel& model, int index)
	{
		if (index < 0 || (size_t)index >= model.Nodes.size())
			return;

		const Egss::GltfNode& node = model.Nodes[index];

		std::string label = node.Name.empty()
			? ("node " + std::to_string(index)) : node.Name;
		if (node.Mesh >= 0)
			label += "  [mesh " + std::to_string(node.Mesh) + "]";

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
		if (node.Children.empty())
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

		bool open = ImGui::TreeNodeEx((void*)(intptr_t)index, flags, "%s", label.c_str());

		if (open && !node.Children.empty())
		{
			for (int child : node.Children)
				DrawNode(model, child);

			ImGui::TreePop();
		}
	}

	// --- Shader -------------------------------------------------------------

	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			uniform mat4 u_NormalMatrix;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;
			out vec2 v_TexCoord;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				v_WorldPosition = world.xyz;
				// Not u_Transform: see the note where u_NormalMatrix is set.
				v_Normal = mat3(u_NormalMatrix) * a_Normal;
				v_TexCoord = a_TexCoord;
				gl_Position = u_ViewProjection * world;
			}
		)";

		// Not a real PBR shader -- there is no image-based lighting here, so
		// "metallic" has nothing to reflect. Metalness tints the highlight
		// toward the base colour and roughness widens it, which is enough for
		// the loaded numbers to visibly do something without pretending the
		// engine has a PBR pipeline it does not.
		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_WorldPosition;
			in vec3 v_Normal;
			in vec2 v_TexCoord;

			uniform vec4 u_BaseColour;
			uniform vec3 u_Emissive;
			uniform float u_Metallic;
			uniform float u_Roughness;
			uniform int u_HasTexture;
			uniform sampler2D u_BaseColourMap;

			uniform vec3 u_LightDirection;
			uniform vec3 u_CameraPosition;
			uniform float u_Ambient;

			void main()
			{
				vec4 base = u_BaseColour;
				if (u_HasTexture == 1)
					base *= texture(u_BaseColourMap, v_TexCoord);

				vec3 n = normalize(v_Normal);
				vec3 l = normalize(-u_LightDirection);
				vec3 v = normalize(u_CameraPosition - v_WorldPosition);
				vec3 h = normalize(l + v);

				float diffuse = max(dot(n, l), 0.0);

				// Roughness to a Blinn-Phong exponent. The mapping is a
				// convention, not physics: 2/r^4 - 2 is the usual one, and it
				// puts roughness 0 at a very tight highlight and roughness 1
				// at none at all.
				float r = max(u_Roughness, 0.05);
				float shininess = 2.0 / (r * r * r * r) - 2.0;
				float specular = pow(max(dot(n, h), 0.0), shininess);

				// A metal's highlight takes the colour of the metal; a
				// dielectric's is white. That one line is most of what makes
				// metalness look like metalness.
				vec3 highlight = mix(vec3(1.0), base.rgb, u_Metallic);

				// A metal has no diffuse reflection -- all of its colour comes
				// from what it reflects. With no environment to reflect, a
				// fully metallic surface therefore renders **black**, which
				// looks exactly like a broken material rather than like the
				// correct answer to an ill-posed question.
				//
				// So: a two-colour hemisphere standing in for a sky and a
				// ground, picked by which way the normal faces. It is a cheat
				// and not an environment map, but it is the cheapest thing
				// that makes metallic and rough visibly mean something.
				vec3 sky = vec3(0.45, 0.52, 0.65);
				vec3 ground = vec3(0.16, 0.14, 0.12);
				vec3 environment = mix(ground, sky, n.y * 0.5 + 0.5);

				vec3 lit = base.rgb * (u_Ambient + diffuse * (1.0 - u_Metallic))
					+ base.rgb * environment * u_Metallic
					+ highlight * specular * (1.0 - r);

				color = vec4(lit + u_Emissive, base.a);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("Gltf", vertexSrc, fragmentSrc));
		m_SceneMaterial = Egss::Material::Create(m_Shader);
		m_Fallback = Egss::Material::CreateInstance(m_SceneMaterial);

		// The stand-in for "this material has no texture". One white pixel, so
		// the shader can sample unconditionally.
		m_White.reset(Egss::Texture2D::Create(1, 1));
		unsigned int white = 0xFFFFFFFF;
		m_White->SetData(&white, sizeof(white));

		m_Fallback->Set("u_BaseColour", glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
		m_Fallback->Set("u_Metallic", 0.0f);
		m_Fallback->Set("u_Roughness", 0.8f);
		m_Fallback->Set("u_HasTexture", 0);
		m_Fallback->SetTexture("u_BaseColourMap", m_White);
	}

	Egss::PerspectiveCamera m_Camera;

	Loaded m_Gltf, m_Glb;
	bool m_ShowGlb = false;
	bool m_ContainersAgree = false;
	std::string m_Comparison = "not loaded";

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_SceneMaterial;
	std::shared_ptr<Egss::Material> m_Fallback;
	std::shared_ptr<Egss::Texture2D> m_White;

	glm::mat4 m_Model = glm::mat4(1.0f);

	float m_Orbit = 35.0f;
	float m_Time = 0.0f;
	float m_Ambient = 0.25f;
	bool m_Spin = true;
	bool m_ShoulderSpin = false;
	bool m_Wireframe = false;
};
