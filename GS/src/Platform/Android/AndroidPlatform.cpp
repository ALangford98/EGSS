#include "gspch.h"
#include "Platform/Android/AndroidPlatform.h"
#include "Platform/Android/AndroidWindow.h"

#include "GS/Application.h"
#include "GS/Log.h"

#include <android/asset_manager.h>
#include <android_native_app_glue.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace GS {
	// Defined by the game (GSPlayer), as on every platform.
	Application* CreateApplication();
}

namespace GS::Android {

	static android_app* s_App = nullptr;

	android_app* GetApp() { return s_App; }

	// Before the window object exists these only let native_app_glue do its
	// own bookkeeping (it sets app->window itself); afterwards they reach it.
	static void HandleCommand(android_app*, int32_t command)
	{
		if (AndroidWindow* window = AndroidWindow::Get())
			window->OnCommand(command);
	}

	static int32_t HandleInput(android_app*, AInputEvent* event)
	{
		AndroidWindow* window = AndroidWindow::Get();
		return window && window->OnInput(event) ? 1 : 0;
	}

	void PumpEvents(bool block)
	{
		int timeout = block ? -1 : 0;
		for (;;)
		{
			int events = 0;
			android_poll_source* source = nullptr;
			int result = ALooper_pollOnce(timeout, nullptr, &events, (void**)&source);
			if (result == ALOOPER_POLL_TIMEOUT || result == ALOOPER_POLL_ERROR)
				break;
			if (result >= 0 && source)
				source->process(s_App, source);
			if (s_App->destroyRequested)
				break;
			// Woken once; now drain whatever else is queued without waiting.
			timeout = 0;
		}
	}

	// An entry in game.files is a path the packager wrote, but it is still
	// checked: nothing in the list may land outside the destination.
	static bool SafeRelative(const std::filesystem::path& path)
	{
		if (path.empty() || path.is_absolute())
			return false;
		for (const auto& part : path)
			if (part == "..")
				return false;
		return true;
	}

	bool ExtractAssets(const std::string& destination)
	{
		AAssetManager* manager = s_App->activity->assetManager;

		AAsset* listing = AAssetManager_open(manager, "game.files", AASSET_MODE_BUFFER);
		if (!listing)
		{
			GS_CORE_ERROR("Android: the APK has no game.files -- not packaged by GameExporter?");
			return false;
		}
		std::string list((const char*)AAsset_getBuffer(listing), (size_t)AAsset_getLength(listing));
		AAsset_close(listing);

		// game.files is copied last, and compared first. Its first line names
		// the build, so a reinstalled game with the same file names but new
		// contents is still unpacked again; and a copy cut short (the process
		// killed mid-way) never wrote it, so it is redone on the next start.
		namespace fs = std::filesystem;
		fs::path root(destination);
		fs::path stamp = root / "game.files";
		{
			std::ifstream existing(stamp, std::ios::binary);
			std::stringstream previous;
			previous << existing.rdbuf();
			if (existing && previous.str() == list)
			{
				GS_CORE_INFO("Android: game already unpacked in {0}", destination);
				return true;
			}
		}

		// From empty: a file the previous version had and this one dropped
		// must not be found by the new game.
		std::error_code ec;
		fs::remove_all(root, ec);
		fs::create_directories(root, ec);

		std::istringstream lines(list);
		std::string line;
		std::getline(lines, line);   // the header: gs-files 1 <build>
		size_t count = 0;
		std::vector<char> buffer(1 << 16);
		while (std::getline(lines, line))
		{
			if (line.empty())
				continue;
			fs::path relative(line);
			if (!SafeRelative(relative))
			{
				GS_CORE_ERROR("Android: refusing to unpack '{0}'", line);
				return false;
			}

			AAsset* asset = AAssetManager_open(manager, line.c_str(), AASSET_MODE_STREAMING);
			if (!asset)
			{
				GS_CORE_ERROR("Android: game.files names '{0}', which the APK does not contain", line);
				return false;
			}
			fs::create_directories((root / relative).parent_path(), ec);
			std::ofstream out(root / relative, std::ios::binary);
			int read = 0;
			while ((read = AAsset_read(asset, buffer.data(), buffer.size())) > 0)
				out.write(buffer.data(), read);
			AAsset_close(asset);
			if (!out)
			{
				GS_CORE_ERROR("Android: could not write '{0}'", (root / relative).string());
				return false;
			}
			count++;
		}

		std::ofstream(stamp, std::ios::binary) << list;
		GS_CORE_INFO("Android: unpacked {0} files into {1}", count, destination);
		return true;
	}

	void Main(android_app* app)
	{
		s_App = app;
		app->onAppCmd = HandleCommand;
		app->onInputEvent = HandleInput;

		Log::Init();

		// The activity can start before its window exists; the renderer
		// cannot. Nothing is created until there is something to draw into.
		while (!app->window && !app->destroyRequested)
			PumpEvents(true);

		// Unpacked into the app's private storage, where the game reads it
		// with ordinary file I/O like every other platform -- Assets::Resolve,
		// std::ifstream, stb_image all stay as they are. The player starts in
		// this folder and runs game/ inside it, as a desktop package does.
		std::string root = std::string(app->activity->internalDataPath) + "/package";
		if (!app->destroyRequested && ExtractAssets(root) && chdir(root.c_str()) == 0)
		{
			static char name[] = "gsplayer";
			static char* argv[] = { name, nullptr };
			Application::SetCommandLine(1, argv);

			Application* application = CreateApplication();
			application->Run();
			delete application;
		}
		else if (!app->destroyRequested)
		{
			GS_CORE_ERROR("Android: could not unpack the game; closing");
			ANativeActivity_finish(app->activity);
		}

		// The process may be kept and android_main called again when the
		// game is next opened, with every static -- the Application
		// singleton, spdlog's registered loggers -- still set from this run.
		// Ending the process makes the next start a clean one.
		std::exit(0);
	}
}
