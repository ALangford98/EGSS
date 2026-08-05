#pragma once

// A lit, textured cube -- the smallest thing that exercises the whole 3D path.
//
// Worth noticing how little of this is new. The vertex buffer, index buffer,
// buffer layout, shader, and texture are the same types the 2D renderer uses;
// only the camera and the shader differ. What is *not* used is Renderer2D:
// its batching is quad-specific, so meshes go through Renderer::Submit
// instead, one draw call each.
//
// Things marked TRY: are deliberate places to experiment.

#include <Egss.h>
#include <cstring>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Demo.h"

class Cube3D : public DemoLayer
{
public:
	Cube3D()
		: DemoLayer("Cube3D"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f)
	{
	}

	void OnDemoAttach() override
	{
		m_Camera.SetPosition({ 0.0f, 1.6f, 6.0f });
		m_Camera.SetRotation(-90.0f, -12.0f);

		BuildMeshes();
		// Before the scene: an entity that brings its own materials needs the
		// scene material to instance them from, and that is made here.
		BuildShader();
		BuildTexture();
		BuildScene();
		BuildAudio();
		BuildTarget();
	}

	// -- attach runs for every demo whichever one is showing, which is how
	// these hums ended up playing under the other demos.
	void OnDemoActivated() override { StartEmitters(); }

	void OnDemoDeactivated() override
	{
		for (Egss::VoiceHandle& emitter : m_Emitters)
		{
			Egss::AudioEngine::Stop(emitter);
			emitter = Egss::InvalidVoice;
		}
	}

	void BuildAudio()
	{
		m_HumClip = MakeHum(196.0f, 2.0f);
		m_ChimeClip = MakeHum(587.0f, 2.0f);

		m_EmitterPositions[0] = { -1.6f, 0.0f, 0.0f };
		m_EmitterPositions[1] = { 1.6f, 0.0f, -1.6f };

		// Deliberately not started here. OnAttach runs for every pushed layer
		// whichever demo is selected, so starting a loop here plays it forever
		// underneath whatever else you switch to. OnUpdate starts them once
		// this demo is actually running.
	}

	// A looping clip has to be seamless or the loop point clicks. Using a
	// whole number of cycles in the buffer means the end lines up with the
	// start exactly.
	static std::shared_ptr<Egss::AudioClip> MakeHum(float frequency, float seconds)
	{
		const unsigned int rate = Egss::AudioEngine::GetSampleRate();
		unsigned int frames = (unsigned int)(seconds * rate);

		float cycles = std::round(frequency * seconds);
		float exactFrequency = cycles / seconds;
		frames = (unsigned int)std::round(cycles * rate / exactFrequency);

		std::vector<float> samples(frames);
		for (unsigned int i = 0; i < frames; i++)
		{
			float t = (float)i / (float)rate;
			// A little second harmonic, so it is not a pure sine.
			samples[i] = (std::sin(glm::two_pi<float>() * exactFrequency * t) * 0.6f
				+ std::sin(glm::two_pi<float>() * exactFrequency * 2.0f * t) * 0.2f) * 0.5f;
		}

		return Egss::AudioClip::CreateFromSamples(std::move(samples), 1);
	}

	void StartEmitters()
	{
		for (int i = 0; i < 2; i++)
		{
			Egss::AudioEngine::Stop(m_Emitters[i]);

			Egss::Audio3DParams params;
			params.Position = m_EmitterPositions[i];
			params.Volume = 0.5f;
			params.Loop = true;
			params.MinDistance = m_EmitterMinDistance;
			params.MaxDistance = m_EmitterMaxDistance;
			params.DopplerFactor = m_DopplerFactor;

			m_Emitters[i] = Egss::AudioEngine::PlayAt(
				i == 0 ? m_HumClip : m_ChimeClip, params);
		}
	}

	// ---------------------------------------------------------------------
	// Geometry
	//
	// ---------------------------------------------------------------------
	// The geometry used to be built by hand right here -- 24 vertices, 36
	// indices, a layout and two buffers, all inline in the demo. Mesh does
	// that now, so a demo asks for geometry instead of describing it.
	// ---------------------------------------------------------------------
	void BuildMeshes()
	{
		m_Primitives[0].reset(Egss::Mesh::CreateCube(1.0f));
		m_Primitives[1].reset(Egss::Mesh::CreateSphere(0.6f, 32, 16));
		m_Primitives[2].reset(Egss::Mesh::CreatePlane(2.0f));

		// Anything in assets/models, so the path box starts somewhere useful
		// rather than at whatever the working directory happens to be.
		std::strncpy(m_LoadPath, "assets/models/torus.obj", sizeof(m_LoadPath) - 1);

		// Loaded at startup so the scene has something that came off disk. A
		// failure is not fatal -- the entity just gets a primitive instead.
		m_Loaded.reset(Egss::Mesh::Load("assets/models/icosahedron.obj"));

		// And one model that brings its own materials, which is a different
		// thing from bringing its own geometry.
		m_Beacon.reset(Egss::Mesh::Load("assets/models/beacon.obj"));
	}

	// Turns a mesh's `mtllib` references into one material per submesh.
	//
	// The mesh names its materials and the .mtl defines them; matching the two
	// up by name is all this does. A submesh naming a material the file does
	// not define keeps the scene material, which reads as "wrong colour"
	// rather than "missing object".
	std::vector<std::shared_ptr<Egss::Material>> LoadMaterialsFor(
		const std::shared_ptr<Egss::Mesh>& mesh, const std::string& modelPath)
	{
		std::vector<std::shared_ptr<Egss::Material>> materials;
		if (!mesh)
			return materials;

		std::string directory = Egss::MtlLoader::DirectoryOf(modelPath);

		std::vector<Egss::ObjMaterial> defined;
		for (const std::string& library : mesh->GetMaterialLibraries())
		{
			std::vector<Egss::ObjMaterial> batch;
			std::string error;
			if (Egss::MtlLoader::Load(directory + library, batch, error))
				defined.insert(defined.end(), batch.begin(), batch.end());
			else
				EGSS_WARN("Cube3D: {0}", error);
		}

		// One cache across the whole model: two materials sharing a texture
		// would otherwise upload it twice.
		std::unordered_map<std::string, std::shared_ptr<Egss::Texture2D>> textures;

		// This shader has no uniform for most of what an .mtl carries. Naming
		// only what exists keeps FromObj from setting uniforms that are not
		// there and logging about each one, every frame.
		Egss::ObjMaterialUniforms names;
		names.Ambient.clear();
		names.Specular.clear();
		names.Emissive.clear();
		names.SpecularExponent.clear();
		names.Opacity.clear();
		names.DiffuseMap.clear();   // the demo's checkerboard stays on the base

		for (const Egss::Submesh& submesh : mesh->GetSubmeshes())
		{
			auto it = std::find_if(defined.begin(), defined.end(),
				[&](const Egss::ObjMaterial& m) { return m.Name == submesh.Material; });

			materials.push_back(it != defined.end()
				? Egss::Material::FromObj(*it, m_SceneMaterial, directory, names, &textures)
				: Egss::Material::CreateInstance(m_SceneMaterial));
		}

		return materials;
	}

	// ---------------------------------------------------------------------
	// The scene
	//
	// Five objects, each an entity with a transform and a mesh. Nothing here
	// tracks them by hand any more: the render loop asks the scene for every
	// MeshComponent and walks it, and picking hands back an entity the panel
	// and the gizmo both understand.
	// ---------------------------------------------------------------------
	void BuildScene()
	{
		auto add = [this](const char* name, const std::shared_ptr<Egss::Mesh>& mesh,
			const glm::vec3& position, const glm::vec4& color, float scale = 1.0f)
		{
			Egss::Entity entity = m_Scene.CreateEntity(name);

			auto* transform = entity.Get<Egss::TransformComponent>();
			transform->Position = position;
			transform->Scale = glm::vec3(scale);

			Egss::MeshComponent mesh_;
			mesh_.Geometry = mesh;
			mesh_.Color = color;
			entity.Add<Egss::MeshComponent>(mesh_);

			return entity;
		};

		// A wide, flat cube rather than the plane primitive: a plane is one
		// quad with a single normal, so it goes uniformly dark as the light
		// moves, and a floor is where that is most obvious.
		Egss::Entity floor = add("Floor", m_Primitives[0], { 0.0f, -1.2f, 0.0f },
			{ 0.55f, 0.57f, 0.62f, 1.0f });
		floor.Get<Egss::TransformComponent>()->Scale = { 12.0f, 0.2f, 12.0f };

		add("Cube",   m_Primitives[0], { -2.2f, 0.0f,  0.0f }, { 1.00f, 0.55f, 0.35f, 1.0f });
		add("Sphere", m_Primitives[1], {  0.0f, 0.0f,  0.0f }, { 0.45f, 0.75f, 1.00f, 1.0f });
		add("Riser",  m_Primitives[0], {  2.2f, 0.0f, -1.4f }, { 0.65f, 1.00f, 0.55f, 1.0f }, 0.7f);

		m_Spinner = add("Icosahedron", m_Loaded ? m_Loaded : m_Primitives[1],
			{ 2.2f, 0.4f, 1.2f }, { 0.90f, 0.60f, 1.00f, 1.0f }, 0.8f).GetId();

		// The one object in the scene whose colours it does not choose. Three
		// submeshes, three materials, all of them out of beacon.mtl -- so the
		// slate base, the brass post and the pale head are the file's decision
		// and the Color below is never applied to them.
		if (m_Beacon)
		{
			Egss::Entity beacon = add("Beacon", m_Beacon, { -2.2f, -1.1f, 1.6f },
				{ 1.0f, 1.0f, 1.0f, 1.0f });

			auto* component = beacon.Get<Egss::MeshComponent>();
			component->Materials = LoadMaterialsFor(m_Beacon, "assets/models/beacon.obj");
			component->MaterialsFromFile = true;
		}

		m_Selected = m_Spinner;
	}

	// Loads whatever is in the path box. A failure logs and leaves the current
	// mesh alone -- a missing file should not empty the scene.
	void LoadMeshFromPath()
	{
		Egss::Mesh* loaded = Egss::Mesh::Load(m_LoadPath);
		if (!loaded)
		{
			m_LoadError = "Could not load '" + std::string(m_LoadPath) + "' -- see the log";
			return;
		}

		m_LoadError.clear();
		m_Loaded.reset(loaded);

		// Assign it to whatever is selected, so loading a file has a visible
		// effect rather than quietly filling a slot.
		if (auto* mesh = m_Scene.GetComponent<Egss::MeshComponent>(m_Selected))
			mesh->Geometry = m_Loaded;

		FrameMesh();
	}

	// Puts the camera where the whole mesh fits, whatever size the file turned
	// out to be. A loaded model can be 0.1 units across or 500, and neither is
	// worth discovering by flying around looking for it.
	void FrameMesh()
	{
		auto* transform = m_Scene.GetComponent<Egss::TransformComponent>(m_Selected);
		auto* mesh = m_Scene.GetComponent<Egss::MeshComponent>(m_Selected);
		if (!transform || !mesh || !mesh->Geometry)
			return;

		// Half the vertical field of view is the angle from the view centre to
		// the top of the frame, so the distance that fits a sphere of radius r
		// is r / sin(halfFov). The 1.6 is headroom.
		float halfFov = glm::radians(m_Camera.GetFov() * 0.5f);
		float scale = glm::max(glm::max(transform->Scale.x, transform->Scale.y), transform->Scale.z);
		float radius = glm::max(mesh->Geometry->GetBoundsRadius(), 0.001f) * scale;
		float distance = (radius / std::sin(halfFov)) * 1.6f;

		glm::vec3 target = transform->Position + mesh->Geometry->GetBoundsCentre() * scale;

		// A three-quarter view, the angle modelling software frames to. Looking
		// straight on hides the depth that is the whole point of a 3D model.
		m_Camera.SetRotation(-90.0f, -30.0f);
		m_Camera.SetPosition(target - m_Camera.GetForward() * distance);
	}

	// The offscreen target. The second attachment is what makes picking
	// possible: an integer texture the fragment shader writes an entity into,
	// alongside the colour nobody would want it mixed with.
	void BuildTarget()
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();

		Egss::FramebufferSpecification spec;
		spec.Width = window.GetWidth();
		spec.Height = window.GetHeight();
		spec.Attachments = {
			Egss::FramebufferTextureFormat::RGBA8,
			Egss::FramebufferTextureFormat::RED_INTEGER,
			Egss::FramebufferTextureFormat::DEPTH24STENCIL8
		};

		m_Framebuffer.reset(Egss::Framebuffer::Create(spec));
	}

	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			// Both are set for you by Renderer::Submit.
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;
			out vec2 v_TexCoord;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				v_WorldPosition = world.xyz;
				// mat3() drops the translation, which a direction must not
				// have. This is only correct while the scale is uniform; with
				// non-uniform scale you need the inverse-transpose instead.
				v_Normal        = mat3(u_Transform) * a_Normal;
				v_TexCoord      = a_TexCoord;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;
			// Matches the framebuffer's integer attachment. Meshes are drawn
			// one at a time rather than batched, so this can be a uniform --
			// no per-vertex attribute needed, unlike the 2D path.
			layout(location = 1) out int entityID;

			in vec3 v_WorldPosition;
			in vec3 v_Normal;
			in vec2 v_TexCoord;

			uniform sampler2D u_Texture;
			uniform vec4 u_Color;
			// Scene-wide, and separate from u_Color on purpose: a material
			// loaded from an .mtl owns its colour, so a global tint cannot be
			// folded into the same uniform without overwriting the file's.
			uniform vec4 u_Tint;

			// A point light, not a directional one. A directional light has no
			// position, so there would be nothing for a gizmo to drag.
			uniform vec3 u_LightPosition;
			uniform vec3 u_LightColor;
			uniform float u_LightRange;
			uniform vec3 u_CameraPosition;
			uniform float u_AmbientStrength;
			uniform int u_EntityID;

			void main()
			{
				vec3 normal  = normalize(v_Normal);

				vec3 lightVector = u_LightPosition - v_WorldPosition;
				float lightDistance = length(lightVector);
				vec3 toLight = lightVector / max(lightDistance, 0.0001);

				// Inverse-square, which is how light actually falls off, times
				// a linear fade so it reaches zero at the stated range instead
				// of trailing off forever.
				float attenuation = 1.0 / (1.0 + 0.08 * lightDistance * lightDistance);
				attenuation *= clamp(1.0 - lightDistance / u_LightRange, 0.0, 1.0);

				// Lambert: how square-on the surface is to the light. Faces
				// turned away give a negative dot, hence the clamp.
				float diffuse = max(dot(normal, toLight), 0.0);

				// Blinn-Phong specular, off the half-vector between the light
				// and the eye. Cheaper than reflecting, and better behaved at
				// grazing angles.
				vec3 toEye    = normalize(u_CameraPosition - v_WorldPosition);
				vec3 halfway  = normalize(toLight + toEye);
				float specular = pow(max(dot(normal, halfway), 0.0), 48.0);

				vec3 base    = texture(u_Texture, v_TexCoord).rgb * u_Color.rgb * u_Tint.rgb;
				vec3 lit     = base * u_AmbientStrength
				             + base * diffuse * u_LightColor * attenuation
				             + specular * u_LightColor * attenuation * 0.35;

				color = vec4(lit, 1.0);
				entityID = u_EntityID;
			}
		)";

		m_Shader.reset(Egss::Shader::Create("Cube3D", vertexSrc, fragmentSrc));

		// Into the library, so anything else that wants this program can ask
		// for it by name instead of being handed the pointer.
		Egss::Renderer::GetShaderLibrary().Add(m_Shader);

		// The scene material: everything that is the same for every object
		// drawn this frame. Its values are refreshed once per frame in
		// RenderMeshes, not per mesh.
		m_SceneMaterial = Egss::Material::Create(m_Shader);
	}

	// A checkerboard, so the cube's faces and their orientation are readable
	// without any asset files. Texture2D::Create("path.png") for real art.
	void BuildTexture()
	{
		constexpr unsigned int size = 16;
		std::vector<unsigned int> pixels(size * size);

		for (unsigned int y = 0; y < size; y++)
		{
			for (unsigned int x = 0; x < size; x++)
			{
				bool light = ((x / 2) + (y / 2)) % 2 == 0;
				unsigned int shade = light ? 0xffe8e8e8 : 0xff6a6a72;
				pixels[y * size + x] = shade;
			}
		}

		m_Texture.reset(Egss::Texture2D::Create(size, size));
		m_Texture->SetData(pixels.data(), (unsigned int)(pixels.size() * sizeof(unsigned int)));
	}

	// Presentation only -- no OnFixedUpdate. Nothing here is simulated: the
	// camera is feel, and the spin is decoration, so both want the real frame
	// time rather than a fixed step. Not everything needs the simulation loop.

	// Presentation only -- no OnFixedUpdate. Nothing here is simulated: the
	// camera is feel, and the spin is decoration, so both want the real frame
	// time rather than a fixed step. Not everything needs the simulation loop.
	// On the fixed step, so how far the camera travels does not depend on the
	// frame rate. Anything driven by held keys belongs here -- it is also what
	// lets a session be recorded and replayed and arrive in the same place.
	void OnDemoFixedUpdate(Egss::Timestep fixedStep) override
	{
		m_PreviousCameraPosition = m_Camera.GetPosition();
		MoveCamera(fixedStep);

		if (m_Spinning)
			m_Rotation += fixedStep * 35.0f;
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{

		m_FrameTime = ts.GetMilliseconds();

		glm::vec3 previousCameraPosition = m_PreviousCameraPosition;

		// The listener rides the camera. PerspectiveCamera already hands back
		// position, forward and up in exactly the form a listener wants.
		Egss::AudioListener listener;
		listener.Position = m_Camera.GetPosition();
		listener.Forward = m_Camera.GetForward();
		listener.Up = m_Camera.GetUp();
		// Velocity is only needed for Doppler, and is just how far the camera
		// moved this frame over how long the frame took.
		listener.Velocity = ts > 0.0f
			? (m_Camera.GetPosition() - previousCameraPosition) / (float)ts
			: glm::vec3(0.0f);

		Egss::AudioEngine::SetListener(listener);

		// Occlusion, which until now the 3D demo simply could not do: Raycast
		// was 2D, so a hum behind a cube sounded exactly like a hum in front of
		// one. The rays go emitter-to-listener through the scene's own meshes.
		//
		// Cheap enough per frame at two emitters and a handful of objects, and
		// deliberately not cached: the whole point is that it changes as you
		// walk around. A scene with hundreds of emitters would want it spread
		// across frames instead.
		if (m_ApplyOcclusion)
		{
			EGSS_PROFILE_SCOPE("Cube3D::Occlusion");

			for (int i = 0; i < 2; i++)
			{
				if (!Egss::AudioEngine::IsPlaying(m_Emitters[i]))
					continue;

				m_EmitterOcclusion[i] = Egss::Raycast3D::Occlusion(m_Scene,
					m_EmitterPositions[i], listener.Position, m_OcclusionSpread,
					m_OcclusionRays);

				Egss::AudioEngine::SetVoiceOcclusion(m_Emitters[i], m_EmitterOcclusion[i]);
			}
		}
		else
		{
			for (int i = 0; i < 2; i++)
			{
				m_EmitterOcclusion[i] = 0.0f;
				if (Egss::AudioEngine::IsPlaying(m_Emitters[i]))
					Egss::AudioEngine::SetVoiceOcclusion(m_Emitters[i], 0.0f);
			}
		}

		// The spinner's rotation is a property of the entity now, not a global
		// the draw loop reaches for. Advanced on the fixed step -- see
		// OnDemoFixedUpdate -- so its speed does not follow the frame rate.
		if (auto* transform = m_Scene.GetComponent<Egss::TransformComponent>(m_Spinner))
			transform->Rotation.y = m_Rotation;

		ResizeTarget();
		UpdateGizmo();

		// --- Pass 1: the scene, into the picking framebuffer ---
		m_Framebuffer->Bind();

		Egss::RenderCommand::SetClearColor({ 0.06f, 0.07f, 0.09f, 1.0f });
		Egss::RenderCommand::Clear();
		// glClear only carries a float colour, so the integer attachment needs
		// its own call. -1 means "nothing here".
		m_Framebuffer->ClearAttachment(1, -1);

		// Safe here because every mesh -- primitives and .obj alike -- is wound
		// counter-clockwise. Off again before the debug lines, which are not
		// closed geometry and would half disappear.
		Egss::RenderCommand::SetBackfaceCulling(m_BackfaceCulling);

		RenderMeshes();

		Egss::RenderCommand::SetBackfaceCulling(false);

		// Debug lines under a perspective camera. Renderer2D::BeginScene takes
		// any Camera, so the line batch works here exactly as it does in 2D --
		// the "2D" in the name is about the primitives, not the projection.
		Egss::Renderer2D::BeginScene(m_Camera);

		if (m_ShowGrid)
			DrawGrid();

		if (m_ShowEmitters)
			DrawEmitters();

		DrawSelectionBox();

		if (m_ShowGizmo)
			DrawGizmo();

		// A marker where the light is, so it can be seen and grabbed even when
		// it sits outside the lit geometry.
		DrawLightMarker();

		Egss::Renderer2D::EndScene();

		// Read back while the framebuffer is still bound and the batch has
		// already been flushed. Both matter.
		ReadHoveredEntity();

		m_Framebuffer->Unbind();

		// --- Pass 2: the result, to the window ---
		Egss::RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
		Egss::RenderCommand::Clear();

		BlitToWindow();
	}

	// A system: every entity with a mesh, drawn with its own transform. It does
	// not know or care which of them also has a body, a light or a name.
	void RenderMeshes()
	{
		Egss::Renderer::BeginScene(m_Camera);

		// Everything that is not per-object, onto the scene material once. These
		// used to be a run of SetFloat3 calls against a bound shader, which
		// worked only because the draws happened immediately afterwards -- the
		// values lived in the program, not in anything describing the object.
		m_SceneMaterial->Set("u_LightPosition", m_LightPosition);
		m_SceneMaterial->Set("u_LightColor", m_LightColor);
		m_SceneMaterial->Set("u_LightRange", m_LightRange);
		m_SceneMaterial->Set("u_CameraPosition", m_Camera.GetPosition());
		m_SceneMaterial->Set("u_AmbientStrength", m_Ambient);
		m_SceneMaterial->Set("u_Tint", m_Tint);
		m_SceneMaterial->SetTexture("u_Texture", m_Texture, 0);

		auto& meshes = m_Scene.View<Egss::MeshComponent>();
		m_DrawnMeshes = 0;
		m_DrawnSubmeshes = 0;

		for (size_t i = 0; i < meshes.Size(); i++)
		{
			Egss::MeshComponent& mesh = meshes.Components()[i];
			if (!mesh.Visible || !mesh.Geometry)
				continue;

			Egss::EntityId entity = meshes.Owner(i);
			auto* transform = m_Scene.GetComponent<Egss::TransformComponent>(entity);
			if (!transform)
				continue;

			const std::vector<Egss::Submesh>& submeshes = mesh.Geometry->GetSubmeshes();

			// An entity may arrive without materials -- it is normal to add
			// geometry before deciding how it looks -- so give each submesh an
			// instance of the scene material the first time it is drawn.
			if (mesh.Materials.size() < submeshes.size())
				mesh.Materials.resize(submeshes.size());

			for (size_t s = 0; s < submeshes.size(); s++)
			{
				if (!mesh.Materials[s])
					mesh.Materials[s] = Egss::Material::CreateInstance(m_SceneMaterial);

				// A material that came out of an .mtl already has its colour,
				// and it is the whole reason the file was loaded.
				if (!mesh.MaterialsFromFile)
					mesh.Materials[s]->Set("u_Color", mesh.Color);

				// The slot index, not the handle -- the attachment is a *signed*
				// integer texture, and a handle whose generation passes 2047
				// exceeds INT_MAX and reads back negative.
				mesh.Materials[s]->Set("u_EntityID", (int)Egss::EntityIds::Index(entity));

				// TransformComponent already composes scale, then rotate, then
				// translate, in that order. Swapping any two changes the result.
				Egss::Renderer::SubmitSubmesh(mesh.Materials[s], mesh.Geometry,
					(unsigned int)s, transform->GetTransform());
			}

			m_DrawnMeshes++;
			m_DrawnSubmeshes += (unsigned int)submeshes.size();
		}

		Egss::Renderer::EndScene();
	}

	// Keeps the target the same size as the window, so a framebuffer pixel and
	// a window pixel are the same thing and the mouse needs no rebasing.
	void ResizeTarget()
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();
		const Egss::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		if (window.GetWidth() > 0 && window.GetHeight() > 0 &&
			(spec.Width != window.GetWidth() || spec.Height != window.GetHeight()))
			m_Framebuffer->Resize(window.GetWidth(), window.GetHeight());
	}

	// Draws the offscreen colour attachment over the window as one quad.
	void BlitToWindow()
	{
		// Rebuilt whenever the framebuffer is recreated: a resize makes new
		// textures and the old handle is gone.
		unsigned int handle = m_Framebuffer->GetColorAttachmentRendererID(0);
		if (!m_ColorAttachment || m_ColorHandle != handle)
		{
			m_ColorAttachment.reset(Egss::Texture2D::CreateFromHandle(handle,
				m_Framebuffer->GetSpecification().Width,
				m_Framebuffer->GetSpecification().Height));
			m_ColorHandle = handle;
		}

		Egss::Renderer2D::BeginScene(m_BlitCamera);
		Egss::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(2.0f), m_ColorAttachment);
		Egss::Renderer2D::EndScene();
	}

	// What is under the cursor, straight out of the integer attachment.
	//
	// Pixel-exact and free of geometry maths: no bounding boxes, no ray-mesh
	// intersection, and it is right for a torus's hole -- which any bounding
	// volume would claim you had clicked.
	void ReadHoveredEntity()
	{
		m_Hovered = Egss::InvalidEntity;

		// Dragging a gizmo handle must not re-pick, or the first frame of a
		// drag would select whatever is behind the handle.
		if (ImGui::GetIO().WantCaptureMouse || m_DragAxis >= 0)
			return;

		auto [mouseX, mouseY] = Egss::Input::GetMousePosition();
		const Egss::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		// Flip y: window coordinates count down, GL counts up.
		int x = (int)mouseX;
		int y = (int)((float)spec.Height - mouseY);

		if (x < 0 || y < 0 || x >= (int)spec.Width || y >= (int)spec.Height)
			return;

		int slot = m_Framebuffer->ReadPixel(1, x, y);
		if (slot < 0)
			return;

		m_Hovered = m_Scene.EntityAtIndex((unsigned int)slot);
	}

	// A wireframe box round the selected object, sized from its mesh bounds so
	// it fits whatever geometry the entity happens to hold.
	void DrawSelectionBox()
	{
		for (int pass = 0; pass < 2; pass++)
		{
			Egss::EntityId entity = (pass == 0) ? m_Hovered : m_Selected;
			if (!m_Scene.IsValid(entity) || (pass == 0 && entity == m_Selected))
				continue;

			auto* transform = m_Scene.GetComponent<Egss::TransformComponent>(entity);
			auto* mesh = m_Scene.GetComponent<Egss::MeshComponent>(entity);
			if (!transform || !mesh || !mesh->Geometry)
				continue;

			glm::vec4 color = (pass == 0)
				? glm::vec4(0.45f, 0.85f, 1.0f, 1.0f)    // hovered
				: glm::vec4(1.00f, 0.85f, 0.3f, 1.0f);   // selected

			glm::vec3 lo = mesh->Geometry->GetBoundsMin();
			glm::vec3 hi = mesh->Geometry->GetBoundsMax();
			glm::mat4 model = transform->GetTransform();

			// The eight corners, transformed. Doing it in model space and then
			// transforming means the box rotates with the object instead of
			// being an axis-aligned shell that swells as it spins.
			glm::vec3 corner[8];
			for (int i = 0; i < 8; i++)
			{
				glm::vec3 local(
					(i & 1) ? hi.x : lo.x,
					(i & 2) ? hi.y : lo.y,
					(i & 4) ? hi.z : lo.z);
				corner[i] = glm::vec3(model * glm::vec4(local, 1.0f));
			}

			// Each edge joins two corners differing in exactly one bit.
			for (int i = 0; i < 8; i++)
			{
				for (int bit = 1; bit <= 4; bit <<= 1)
				{
					if (i & bit)
						continue;
					Egss::Renderer2D::DrawLine(corner[i], corner[i | bit], color);
				}
			}
		}
	}

	// A box round each emitter, brightened by how loud it currently is, so
	// what you hear and what you see agree.
	// A small three-axis star at the light's position.
	void DrawLightMarker()
	{
		glm::vec4 color(m_LightColor.x, m_LightColor.y, m_LightColor.z, 1.0f);
		const float size = 0.18f;

		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 offset(0.0f);
			offset[axis] = size;
			Egss::Renderer2D::DrawLine(m_LightPosition - offset, m_LightPosition + offset, color);
		}
	}

	void DrawEmitters()
	{
		for (int i = 0; i < 2; i++)
		{
			Egss::VoiceDebug debug;
			bool live = Egss::AudioEngine::GetVoiceDebug(m_Emitters[i], debug);

			float brightness = live ? 0.25f + debug.Gain * 0.75f : 0.15f;
			glm::vec4 color = i == 0
				? glm::vec4(0.35f, 0.85f, 1.0f, 1.0f) * brightness
				: glm::vec4(1.0f, 0.75f, 0.35f, 1.0f) * brightness;
			color.a = 1.0f;

			const glm::vec3& p = m_EmitterPositions[i];
			const float r = 0.22f;

			// A wireframe cube, drawn from the lines we already have.
			glm::vec3 corners[8] = {
				{ p.x - r, p.y - r, p.z - r }, { p.x + r, p.y - r, p.z - r },
				{ p.x + r, p.y + r, p.z - r }, { p.x - r, p.y + r, p.z - r },
				{ p.x - r, p.y - r, p.z + r }, { p.x + r, p.y - r, p.z + r },
				{ p.x + r, p.y + r, p.z + r }, { p.x - r, p.y + r, p.z + r }
			};

			for (int e = 0; e < 4; e++)
			{
				Egss::Renderer2D::DrawLine(corners[e], corners[(e + 1) % 4], color);
				Egss::Renderer2D::DrawLine(corners[4 + e], corners[4 + (e + 1) % 4], color);
				Egss::Renderer2D::DrawLine(corners[e], corners[4 + e], color);
			}
		}
	}

	// A ground plane and the world axes. Both are the sort of thing you want
	// permanently available once a scene stops being a single object at the
	// origin.
	void DrawGrid()
	{
		const int half = 6;
		const float y = -0.9f;
		const glm::vec4 gridColor = { 0.25f, 0.27f, 0.32f, 1.0f };

		for (int i = -half; i <= half; i++)
		{
			Egss::Renderer2D::DrawLine({ (float)i, y, (float)-half }, { (float)i, y, (float)half }, gridColor);
			Egss::Renderer2D::DrawLine({ (float)-half, y, (float)i }, { (float)half, y, (float)i }, gridColor);
		}

		// X red, Y green, Z blue -- the usual convention, and the fastest way
		// to work out which way a scene is facing.
		Egss::Renderer2D::DrawLine({ 0, y, 0 }, { 2.0f, y, 0 }, { 0.9f, 0.25f, 0.25f, 1.0f });
		Egss::Renderer2D::DrawLine({ 0, y, 0 }, { 0, y + 2.0f, 0 }, { 0.3f, 0.85f, 0.3f, 1.0f });
		Egss::Renderer2D::DrawLine({ 0, y, 0 }, { 0, y, 2.0f }, { 0.35f, 0.5f, 0.95f, 1.0f });
	}


	// ---------------------------------------------------------------------
	// The gizmo
	//
	// Three axis handles at the selected object's origin -- X red, Y green,
	// Z blue, the convention every 3D tool uses. Dragging one slides the
	// object along that axis only.
	//
	// The whole thing rests on two bits of maths:
	//
	//   * a **ray through the cursor**, because a mouse position is a line in
	//     3D, not a point;
	//   * the **closest point between that ray and the axis line**, which is
	//     what turns "where the mouse is" into "how far along X".
	//
	// Grabbing stores where on the axis you took hold, so the object does not
	// jump to the cursor -- it keeps the offset, exactly like a real tool.
	// ---------------------------------------------------------------------

	// A mouse position is a line through the scene. This returns its origin
	// and direction in world space.
	void ScreenRay(const glm::vec2& mouse, glm::vec3& outOrigin, glm::vec3& outDirection) const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();
		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();

		// Pixels -> clip space, flipping y because window coordinates count
		// downwards and clip space counts up.
		float x = (mouse.x / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.y / height) * 2.0f;

		glm::mat4 inverse = glm::inverse(m_Camera.GetViewProjectionMatrix());

		// The near and far plane points that project to this pixel. The
		// perspective divide is what makes them different.
		glm::vec4 nearPoint = inverse * glm::vec4(x, y, -1.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);

		nearPoint /= nearPoint.w;
		farPoint /= farPoint.w;

		outOrigin = glm::vec3(nearPoint);
		outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
	}

	// World position -> pixels. Returns false behind the camera, where the
	// projection would happily produce a plausible-looking wrong answer.
	bool WorldToScreen(const glm::vec3& world, glm::vec2& outScreen) const
	{
		Egss::Window& window = Egss::Application::Get().GetWindow();

		glm::vec4 clip = m_Camera.GetViewProjectionMatrix() * glm::vec4(world, 1.0f);
		if (clip.w <= 0.0001f)
			return false;

		glm::vec3 ndc = glm::vec3(clip) / clip.w;

		outScreen = {
			(ndc.x * 0.5f + 0.5f) * (float)window.GetWidth(),
			(1.0f - (ndc.y * 0.5f + 0.5f)) * (float)window.GetHeight()
		};
		return true;
	}

	// How far along `axis` the point nearest the cursor ray sits. This is the
	// heart of axis dragging: it collapses a 3D pick down to one number.
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

		// Looking straight down the axis: every point on it projects to the
		// same pixel, so there is no meaningful answer.
		if (std::abs(denominator) < 0.00001f)
			return false;

		outT = (b * e - c * d) / denominator;
		return true;
	}

	// Where the gizmo sits, and what dragging it moves. Returns null when
	// nothing is selected, which is why every caller checks.
	glm::vec3* GizmoPosition()
	{
		if (m_GizmoOnLight)
			return &m_LightPosition;

		auto* transform = m_Scene.GetComponent<Egss::TransformComponent>(m_Selected);
		return transform ? &transform->Position : nullptr;
	}

	// Distance in pixels from the cursor to an axis handle, for picking.
	float AxisScreenDistance(int axis, const glm::vec2& mouse) const
	{
		glm::vec3* target = const_cast<Cube3D*>(this)->GizmoPosition();
		if (!target)
			return std::numeric_limits<float>::max();

		glm::vec3 origin = *target;
		glm::vec3 direction(0.0f);
		direction[axis] = 1.0f;

		glm::vec2 a, b;
		if (!WorldToScreen(origin, a) || !WorldToScreen(origin + direction * m_GizmoLength, b))
			return std::numeric_limits<float>::max();

        // Distance from the cursor to the line segment a-b.
		glm::vec2 segment = b - a;
		float lengthSquared = glm::dot(segment, segment);
		if (lengthSquared < 0.0001f)
			return glm::length(mouse - a);

		float t = glm::clamp(glm::dot(mouse - a, segment) / lengthSquared, 0.0f, 1.0f);
		return glm::length(mouse - (a + segment * t));
	}

	void UpdateGizmo()
	{
		glm::vec2 mouse = { Egss::Input::GetMousePosition().first,
							Egss::Input::GetMousePosition().second };

		bool down = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT)
			&& !ImGui::GetIO().WantCaptureMouse;

		// A grab needs the button to go *down* while over a handle, not merely
		// to be down. Polling "is it held" grabs whatever the cursor happens to
		// be near if the button was already held when the demo started -- which
		// is exactly what a button held across a demo switch looks like.
		bool justPressed = down && !m_MouseDownLastFrame;
		m_MouseDownLastFrame = down;

		bool pressed = down;

		if (!pressed)
		{
			m_DragAxis = -1;
			m_HoverAxis = -1;

			// Highlight whichever handle the cursor is over, so it is obvious
			// what a click would grab.
			for (int axis = 0; axis < 3; axis++)
			{
				if (AxisScreenDistance(axis, mouse) < m_GizmoPickPixels)
				{
					m_HoverAxis = axis;
					break;
				}
			}
			return;
		}

		glm::vec3 rayOrigin, rayDirection;
		ScreenRay(mouse, rayOrigin, rayDirection);

		// --- Grab ---
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

		// --- Drag ---
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
			{ 1.0f, 0.25f, 0.25f, 1.0f },   // X red
			{ 0.30f, 1.0f, 0.35f, 1.0f },   // Y green
			{ 0.35f, 0.55f, 1.0f, 1.0f }    // Z blue
		};

		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 direction(0.0f);
			direction[axis] = 1.0f;

			glm::vec4 color = axisColors[axis];
			if (axis == m_DragAxis || (m_DragAxis < 0 && axis == m_HoverAxis))
				color = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);   // highlighted

			glm::vec3 tip = origin + direction * m_GizmoLength;
			Egss::Renderer2D::DrawLine(origin, tip, color);

			// A little cross at the tip, so the end of the handle is visible
			// even when the line is nearly edge-on to the camera.
			glm::vec3 a(0.0f), b(0.0f);
			a[(axis + 1) % 3] = 0.06f;
			b[(axis + 2) % 3] = 0.06f;

			Egss::Renderer2D::DrawLine(tip - a, tip + a, color);
			Egss::Renderer2D::DrawLine(tip - b, tip + b, color);
		}
	}

	// Fly camera: WASD along the ground, Q/E straight up and down, arrows to
	// look. Keyboard-only on purpose -- mouse look would need cursor capture,
	// which the engine doesn't have yet.
	void MoveCamera(Egss::Timestep ts)
	{
		glm::vec3 position = m_Camera.GetPosition();
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		float move = m_MoveSpeed * ts;
		float look = m_LookSpeed * ts;

		// Forward and right come from the camera's own orientation, so "left"
		// always means left of where you are facing.
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) position += m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) position += m_Camera.GetRight() * move;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) position.y += move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) position.y -= move;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  yaw -= look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) yaw += look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    pitch += look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  pitch -= look;

		// --- Middle-drag to look ---
		// Mouse *delta*, not position: how far it moved since last frame, not
		// where it is. Kept every frame whether or not the button is held, or
		// the first frame of a drag would jump by however far the cursor had
		// travelled since it was last looked at.
		glm::vec2 mouse = { Egss::Input::GetMousePosition().first,
							Egss::Input::GetMousePosition().second };
		glm::vec2 delta = mouse - m_PreviousMouse;
		m_PreviousMouse = mouse;

		bool looking = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_MIDDLE)
			&& !ImGui::GetIO().WantCaptureMouse;

		if (looking)
		{
			// Degrees per pixel, so it feels the same at any frame rate --
			// deliberately *not* scaled by the timestep, because the mouse has
			// already moved a real distance.
			yaw += delta.x * m_MouseLookSensitivity;
			pitch -= delta.y * m_MouseLookSensitivity;
		}

		m_Camera.SetPosition(position);
		m_Camera.SetRotation(yaw, pitch);
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			// Only the projection cares about the window's shape. Get this
			// wrong in 3D and everything looks subtly stretched.
			if (e.GetHeight() > 0)
				m_Camera.SetAspectRatio((float)e.GetWidth() / (float)e.GetHeight());

			return false;
		});

		dispatcher.Dispatch<Egss::MouseButtonPressedEvent>([this](Egss::MouseButtonPressedEvent& e)
		{
			// A click on empty space deselects, which is what makes the
			// selection feel like it belongs to the scene rather than the
			// panel. Grabbing a gizmo handle must not change the selection.
			if (e.GetMouseButton() == EGSS_MOUSE_BUTTON_LEFT
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0)
			{
				m_Selected = m_Hovered;
				m_GizmoOnLight = false;
			}

			return false;
		});

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			// Switching demos is DemoSelector's job, not this layer's -- it
			// sits above this one and consumes F1 before it gets here.

			if (e.GetKeyCode() == EGSS_KEY_SPACE)
				m_Spinning = !m_Spinning;

			return false;
		});
	}

	void OnDemoImGui() override
	{

		// Clear of the Demos panel on first run; ImGui remembers it after.
		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Cube3D");

		ImGui::Text("WASD   move      Q/E  up / down");
		ImGui::Text("Arrows look      Middle-drag  mouse look");
		ImGui::Text("Space  pause spin");

		ImGui::Text("Click an object to select it.");

		// --- Hierarchy --------------------------------------------------
		ImGui::SeparatorText("Scene");

		if (m_Scene.IsValid(m_Hovered))
		{
			auto* tag = m_Scene.GetComponent<Egss::TagComponent>(m_Hovered);
			ImGui::Text("Hovered: %s", tag ? tag->Name.c_str() : "?");
		}
		else
		{
			ImGui::TextDisabled("Hovered: -");
		}

		ImGui::BeginChild("hierarchy", ImVec2(0.0f, 96.0f), ImGuiChildFlags_Borders);
		for (Egss::EntityId entity : m_Scene.GetEntities())
		{
			auto* tag = m_Scene.GetComponent<Egss::TagComponent>(entity);
			if (!tag)
				continue;

			// The label alone forms an ImGui widget's ID, so two entities
			// sharing a name would share a row. PushID separates them.
			ImGui::PushID((int)entity);
			if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
				m_Selected = entity;
			ImGui::PopID();
		}
		ImGui::EndChild();

		// --- Inspector --------------------------------------------------
		ImGui::SeparatorText("Transform");

		ImGui::Checkbox("Gizmo drags the light", &m_GizmoOnLight);
		ImGui::Checkbox("Show gizmo", &m_ShowGizmo);
		ImGui::SameLine();
		ImGui::TextDisabled(m_DragAxis >= 0 ? "dragging %c" : "drag an axis",
			"XYZ"[m_DragAxis < 0 ? 0 : m_DragAxis]);

		if (m_GizmoOnLight)
		{
			ImGui::DragFloat3("Light position", &m_LightPosition.x, 0.01f);
			ImGui::SliderFloat("Range", &m_LightRange, 1.0f, 40.0f);
			ImGui::ColorEdit3("Colour", &m_LightColor.x);
		}
		else if (auto* transform = m_Scene.GetComponent<Egss::TransformComponent>(m_Selected))
		{
			auto* tag = m_Scene.GetComponent<Egss::TagComponent>(m_Selected);
			ImGui::Text("%s  (id %u)", tag ? tag->Name.c_str() : "?", m_Selected);

			ImGui::DragFloat3("Position", &transform->Position.x, 0.01f);
			ImGui::DragFloat3("Rotation", &transform->Rotation.x, 0.5f);
			ImGui::DragFloat3("Scale", &transform->Scale.x, 0.01f, 0.05f, 20.0f);

			if (auto* mesh = m_Scene.GetComponent<Egss::MeshComponent>(m_Selected))
			{
				ImGui::ColorEdit4("Mesh colour", &mesh->Color.x);
				ImGui::Checkbox("Visible", &mesh->Visible);
				if (mesh->Geometry)
					ImGui::TextDisabled("%s: %zu verts, %zu tris",
						mesh->Geometry->GetName().c_str(),
						mesh->Geometry->GetVertexCount(),
						mesh->Geometry->GetTriangleCount());
			}

			if (ImGui::Button("Reset transform"))
			{
				transform->Rotation = glm::vec3(0.0f);
				transform->Scale = glm::vec3(1.0f);
			}
			ImGui::SameLine();
			if (ImGui::Button("Frame it"))
				FrameMesh();
		}
		else
		{
			ImGui::TextDisabled("Nothing selected.");
		}

		ImGui::SliderFloat("Gizmo length", &m_GizmoLength, 0.3f, 3.0f);
		ImGui::SliderFloat("Look sensitivity", &m_MouseLookSensitivity, 0.02f, 0.6f);

		// --- Model ------------------------------------------------------
		// Assigns geometry to whatever is selected. A mesh is shared, so
		// pointing two entities at the same one costs nothing extra.
		ImGui::SeparatorText("Model");

		auto* selectedMesh = m_Scene.GetComponent<Egss::MeshComponent>(m_Selected);

		if (!selectedMesh)
		{
			ImGui::TextDisabled("Select an object to change its mesh.");
		}
		else
		{
			const char* meshNames[] = { "Cube", "Sphere", "Plane", "Loaded file" };
			int choice = -1;
			for (int i = 0; i < 3; i++)
				if (selectedMesh->Geometry == m_Primitives[i])
					choice = i;
			if (selectedMesh->Geometry == m_Loaded)
				choice = 3;

			if (ImGui::Combo("Mesh", &choice, meshNames, 4))
			{
				if (choice < 3)
					selectedMesh->Geometry = m_Primitives[choice];
				else if (m_Loaded)
					selectedMesh->Geometry = m_Loaded;
			}

			ImGui::InputText(".obj path", m_LoadPath, sizeof(m_LoadPath));
			ImGui::SameLine();
			if (ImGui::Button("Load"))
				LoadMeshFromPath();

			if (!m_LoadError.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_LoadError.c_str());

			if (selectedMesh->Geometry)
			{
				glm::vec3 size = selectedMesh->Geometry->GetBoundsSize();
				ImGui::TextDisabled("Bounds %.2f x %.2f x %.2f  (r %.2f)",
					size.x, size.y, size.z, selectedMesh->Geometry->GetBoundsRadius());
			}
		}

		ImGui::Separator();
		glm::vec3 p = m_Camera.GetPosition();
		ImGui::Text("Camera  %.2f, %.2f, %.2f", p.x, p.y, p.z);
		ImGui::Text("Yaw %.0f  Pitch %.0f", m_Camera.GetYaw(), m_Camera.GetPitch());
		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		// Meshes and draws stopped being the same number once a model could
		// carry several materials: the beacon is one mesh and three draws.
		ImGui::Text("Draw calls: %u submeshes across %d meshes + 1 line batch + 1 blit",
			m_DrawnSubmeshes, m_DrawnMeshes);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("A submesh is one range of indices drawn with one material.\n"
				"beacon.obj has three, from its three `usemtl` lines -- one\n"
				"vertex buffer, three draws. Everything else here has one.");
		ImGui::Checkbox("Back-face culling", &m_BackfaceCulling);
		ImGui::Checkbox("Show grid", &m_ShowGrid);

		ImGui::Separator();
		ImGui::Text("Audio: %s   %u voices", Egss::AudioEngine::GetBackendName(),
			Egss::AudioEngine::GetActiveVoiceCount());
		ImGui::Checkbox("Show emitters", &m_ShowEmitters);

		for (int i = 0; i < 2; i++)
		{
			Egss::VoiceDebug debug;
			if (Egss::AudioEngine::GetVoiceDebug(m_Emitters[i], debug))
			{
				ImGui::Text("emitter %d: %.2fm  gain %.2f  pan %+.2f  doppler %.3f  occl %.0f%%",
					i, debug.Distance, debug.Gain, debug.Pan, debug.PitchScale,
					m_EmitterOcclusion[i] * 100.0f);
			}
			else
			{
				ImGui::TextDisabled("emitter %d: not playing", i);
			}
		}

		ImGui::Checkbox("Occlusion", &m_ApplyOcclusion);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Rays from each emitter to the camera, through the scene's\n"
				"own meshes. Walk a cube between yourself and an emitter and\n"
				"watch the figure climb -- it grades rather than switching,\n"
				"because several rays are cast in a ring around the line and\n"
				"the count of blocked ones is the answer.\n"
				"This is what the 3D demo could not do until Raycast stopped\n"
				"being 2D-only.");

		if (m_ApplyOcclusion)
		{
			ImGui::SliderFloat("Ray spread", &m_OcclusionSpread, 0.0f, 1.5f);
			ImGui::SliderInt("Occlusion rays", &m_OcclusionRays, 1, 17);
		}

		if (ImGui::SliderFloat("Emitter range", &m_EmitterMaxDistance, 1.0f, 40.0f))
			StartEmitters();
		if (ImGui::SliderFloat("Doppler", &m_DopplerFactor, 0.0f, 8.0f))
			StartEmitters();
		if (ImGui::Button("Restart emitters"))
			StartEmitters();

		ImGui::Separator();
		ImGui::SliderFloat("Ambient", &m_Ambient, 0.0f, 1.0f);
		ImGui::ColorEdit3("Light colour", &m_LightColor.x);
		ImGui::ColorEdit4("Tint", &m_Tint.x);

		ImGui::End();
	}

private:
	Egss::PerspectiveCamera m_Camera;

	// The geometry available to assign. Entities point at these rather than
	// owning meshes, so switching costs nothing and switching back reloads
	// nothing.
	std::shared_ptr<Egss::Mesh> m_Primitives[3];   // cube, sphere, plane
	std::shared_ptr<Egss::Mesh> m_Loaded;
	std::shared_ptr<Egss::Mesh> m_Beacon;

	char m_LoadPath[256] = { 0 };
	std::string m_LoadError;
	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Texture2D> m_Texture;

	// The base every mesh's own material instances from. Holds the light, the
	// camera and the ambient level -- the things that are the same for every
	// object drawn this frame.
	std::shared_ptr<Egss::Material> m_SceneMaterial;

	float m_MoveSpeed = 3.0f;
	float m_LookSpeed = 70.0f;

	float m_Rotation = 0.0f;
	bool m_Spinning = true;

	std::shared_ptr<Egss::AudioClip> m_HumClip;
	std::shared_ptr<Egss::AudioClip> m_ChimeClip;
	Egss::VoiceHandle m_Emitters[2] = { Egss::InvalidVoice, Egss::InvalidVoice };
	glm::vec3 m_EmitterPositions[2];
	float m_EmitterOcclusion[2] = { 0.0f, 0.0f };
	bool m_ApplyOcclusion = true;
	// Half a metre either side of the line. Wider grades more gently, because
	// more of the ring clears an edge before the centre does.
	float m_OcclusionSpread = 0.35f;
	int m_OcclusionRays = 5;
	float m_EmitterMinDistance = 1.0f;
	float m_EmitterMaxDistance = 12.0f;
	float m_DopplerFactor = 2.0f;
	bool m_ShowEmitters = true;

	bool m_ShowGrid = true;

	// The light is an object in the scene now, so it has a position the gizmo
	// can drag.
	glm::vec3 m_LightPosition = { 1.6f, 1.6f, 1.8f };
	glm::vec3 m_LightColor = { 1.0f, 0.96f, 0.9f };
	float m_LightRange = 12.0f;
	float m_Ambient = 0.10f;

	// The scene, and what is picked in it. m_Selected is what the gizmo and the
	// inspector act on; m_Hovered is only what the cursor is over this frame.
	Egss::Scene m_Scene;
	Egss::EntityId m_Selected = Egss::InvalidEntity;
	Egss::EntityId m_Hovered = Egss::InvalidEntity;
	Egss::EntityId m_Spinner = Egss::InvalidEntity;

	// When true the gizmo drags the light rather than the selected entity. The
	// light is not an entity -- it is a set of shader uniforms -- so it cannot
	// be picked, and needs a way to be reached.
	bool m_GizmoOnLight = false;
	bool m_ShowGizmo = true;

	// Rendered offscreen so the integer attachment has somewhere to go, then
	// blitted back as one quad.
	std::shared_ptr<Egss::Framebuffer> m_Framebuffer;
	std::shared_ptr<Egss::Texture2D> m_ColorAttachment;
	unsigned int m_ColorHandle = 0;
	Egss::OrthographicCamera m_BlitCamera{ -1.0f, 1.0f, -1.0f, 1.0f };

	bool m_BackfaceCulling = true;
	int m_DrawnMeshes = 0;
	// Meshes and submeshes are no longer the same count: one multi-material
	// model is one mesh and several draws, and seeing both makes that visible
	// rather than a thing you have to remember.
	unsigned int m_DrawnSubmeshes = 0;

	glm::vec2 m_PreviousMouse = { 0.0f, 0.0f };
	float m_MouseLookSensitivity = 0.18f;

	bool m_MouseDownLastFrame = false;
	int m_HoverAxis = -1;
	int m_DragAxis = -1;
	float m_DragStartT = 0.0f;
	glm::vec3 m_DragStartPosition = { 0.0f, 0.0f, 0.0f };
	float m_GizmoLength = 1.0f;
	float m_GizmoPickPixels = 12.0f;

	// Multiplied into every mesh's own colour, so one slider tints the scene.
	glm::vec4 m_Tint = { 1.0f, 1.0f, 1.0f, 1.0f };

	float m_FrameTime = 0.0f;

	// Where the camera was before the last fixed step moved it. The listener's
	// velocity is derived from this, and it has to be sampled on the step that
	// does the moving rather than per frame -- otherwise on a frame with no
	// step the camera has not moved and Doppler reads zero.
	glm::vec3 m_PreviousCameraPosition = { 0.0f, 0.0f, 0.0f };
};
