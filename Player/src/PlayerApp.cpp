// GSPlayer: runs a GS project as a game -- no editor, no demos, no panels.
//
//   GSPlayer <project folder> [engine flags: --lockstep, --capture ...]
//
// Reads the folder's project.gsproj, names the window after the project,
// resolves the project's assets against the folder (GS::Assets), loads its
// active scene and runs it through the same Runtime/ code the editor's Play
// uses: GameSession for physics and scripts, SceneRenderer for the frame.
// The same project captured here and in editor Play (--start-play) at the
// same step is the same image -- that is how this is verified. See
// docs/superpowers/specs/2026-10-07-game-runtime-player-design.md.
#include <GS.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "GameSession.h"
#include "ProjectManifest.h"
#include "SceneRenderer.h"

namespace {

	// Printed to stderr as well as the log: someone launching a game from a
	// shell needs to see why it did not start.
	[[noreturn]] void Fail(const std::string& message, int code)
	{
		GS_ERROR("GSPlayer: {0}", message);
		std::fprintf(stderr, "GSPlayer: %s\n", message.c_str());
		std::exit(code);
	}

	class GameLayer : public GS::Layer
	{
	public:
		GameLayer(std::string folder, ProjectManifest manifest)
			: Layer("Game"), m_Folder(std::move(folder)), m_Manifest(std::move(manifest)) {}

		void OnAttach() override
		{
			GS::Window& window = GS::Application::Get().GetWindow();
			window.SetTitle(m_Manifest.Name.empty() ? std::string("GS Game") : m_Manifest.Name);

			GS::Assets::SetRoot(m_Folder);
			std::string scenePath = GS::Assets::Resolve(m_Manifest.SceneRelativePath);
			if (!m_Scene.Load(scenePath))
				Fail("could not load scene '" + scenePath + "'", 1);

			// The same target the editor's Scene view draws into -- colour,
			// entity id, depth -- so the frame is drawn the same way.
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

			// A scene with no active CameraComponent still shows something:
			// the editor fly camera's starting view.
			m_FallbackCamera.SetPosition({ 0.0f, 1.6f, 6.0f });
			m_FallbackCamera.SetRotation(-90.0f, -12.0f);

			m_Renderer.Init();

			// Before the first fixed step, as the editor's --start-play does.
			m_Session = std::make_unique<GameSession>(m_Scene, m_Engine);
			m_Session->Start();
		}

		void OnDetach() override
		{
			if (m_Session)
				m_Session->Stop();
		}

		void OnFixedUpdate(GS::Timestep step) override
		{
			m_Session->FixedUpdate(step);
		}

		void OnUpdate(GS::Timestep) override
		{
			GS::Window& window = GS::Application::Get().GetWindow();
			unsigned int width = window.GetWidth(), height = window.GetHeight();
			if (width == 0 || height == 0)
				return;   // minimised
			const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();
			if (spec.Width != width || spec.Height != height)
				m_Framebuffer->Resize(width, height);
			GS::RenderCommand::SetViewport(0, 0, width, height);

			float aspect = (float)width / (float)height;
			GS::PerspectiveCamera* camera = &m_GameCamera;
			if (!SceneRenderer::FindActiveCamera(m_Scene, aspect, m_GameCamera))
			{
				m_FallbackCamera.SetProjection(45.0f, aspect, 0.1f, 1000.0f);
				camera = &m_FallbackCamera;
			}

			m_Framebuffer->Bind();
			GS::RenderCommand::SetClearColor({ 0.10f, 0.11f, 0.13f, 1.0f });
			GS::RenderCommand::Clear();
			m_Framebuffer->ClearAttachment(1, -1);
			GS::RenderCommand::SetCullFace(GS::CullFace::Back);
			m_Renderer.Render(m_Scene, *camera);
			GS::RenderCommand::SetCullFace(GS::CullFace::None);
			m_Framebuffer->Unbind();

			Blit();
		}

	private:
		// As the editor does it: the colour attachment drawn as one quad.
		void Blit()
		{
			unsigned int handle = m_Framebuffer->GetColorAttachmentRendererID(0);
			if (!m_ColorAttachment || m_ColorHandle != handle)
			{
				m_ColorAttachment.reset(GS::Texture2D::CreateFromHandle(handle,
					m_Framebuffer->GetSpecification().Width, m_Framebuffer->GetSpecification().Height));
				m_ColorHandle = handle;
			}
			GS::RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
			GS::RenderCommand::Clear();
			GS::Renderer2D::BeginScene(m_BlitCamera);
			GS::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(2.0f), m_ColorAttachment);
			GS::Renderer2D::EndScene();
		}

		std::string m_Folder;
		ProjectManifest m_Manifest;
		GS::Scene m_Scene;
		ScriptEngine m_Engine;
		std::unique_ptr<GameSession> m_Session;
		SceneRenderer m_Renderer;

		std::unique_ptr<GS::Framebuffer> m_Framebuffer;
		std::shared_ptr<GS::Texture2D> m_ColorAttachment;
		unsigned int m_ColorHandle = 0;
		GS::OrthographicCamera m_BlitCamera{ -1.0f, 1.0f, -1.0f, 1.0f };
		GS::PerspectiveCamera m_GameCamera{ 45.0f, 16.0f / 9.0f, 0.1f, 1000.0f };
		GS::PerspectiveCamera m_FallbackCamera{ 45.0f, 16.0f / 9.0f, 0.1f, 1000.0f };
	};

	class Player : public GS::Application
	{
	public:
		Player()
		{
			const std::vector<std::string>& arguments = GS::Application::GetCommandLine();
			if (arguments.size() < 2 || arguments[1].rfind("--", 0) == 0)
				Fail("usage: GSPlayer <project folder> [engine flags]", 2);

			// "game", "game/" and "." are the same folder.
			std::filesystem::path folder = std::filesystem::path(arguments[1]).lexically_normal();
			std::string folderString = folder.string();
			while (folderString.size() > 1 && folderString.back() == '/')
				folderString.pop_back();

			ProjectManifest manifest;
			if (!ReadProjectManifest(folderString, manifest))
				Fail("no readable " + std::string(ProjectManifestFilename()) + " with a scene in '" + folderString + "'", 1);

			PushLayer(new GameLayer(folderString, manifest));
		}
	};

}

GS::Application* GS::CreateApplication()
{
	return new Player();
}
