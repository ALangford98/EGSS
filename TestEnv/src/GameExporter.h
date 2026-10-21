#pragma once
// Exports a project as a game someone else can run: a folder holding the
// player (renamed after the game), libGS.so, and game/ -- the project with
// exactly the files it uses -- plus a .tar.gz of that folder. Linux for now;
// see docs/superpowers/specs/2026-10-08-packager-linux-export-design.md.
//
//   TestEnv --export <project folder> --out <dir> [--export-config dist]
//
// Run from the editor's binary folder (assets/typescript.js compiles the
// scripts). Piece 3's Export dialog calls Export() directly.
//
// Stages, each of which can fail the export with a message naming the
// file -- a package that breaks on someone else's machine is worse than no
// package:
//   1. Scan every scene the manifest lists: mesh files (.obj -> mtllib ->
//      texture maps), materials (.gsmat, self-contained), scripts.
//   2. Scripts: one with a GS-GENERATED <stem>.cpp beside it is a graduated
//      class and goes into the player's registry; any other is compiled
//      here to one self-contained .js (imports spliced in), so the shipped
//      game never loads the TypeScript compiler.
//   3. Build GSPlayer against that registry (gs.py build --player-scripts),
//      into bin/Export-*.
//   4. Stage: copy files, rewrite every scene path to its place under game/.
//   5. Archive with the system tar.
#include <GS.h>

#include "MotionClip.h"
#include "ProjectManifest.h"
#include "ScriptEngine.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace GameExporter {

	struct Options
	{
		std::string ProjectFolder;
		std::string OutDir;
		std::string Config = "dist";
		// "linux", "windows" (cross-built with llvm-mingw), "web"
		// (Emscripten + WebGL2), "android" (the NDK; an arm64 APK) or
		// "ios" (an Xcode project, built on a Mac -- not here).
		std::string Target = "linux";
		// Told each stage as it starts -- the Export dialog shows it. Called
		// on whatever thread Export runs on.
		std::function<void(const std::string&)> OnStage;
	};

	struct Result
	{
		bool Ok = false;
		std::string Error;
		std::string PackageDir;
		std::string Archive;
		std::string Executable;
		std::string AppImage;
	};

	namespace Detail {
		namespace fs = std::filesystem;

		// Letters, digits, '-' and '_' kept; anything else becomes a space,
		// and runs of spaces collapse to one. "Rock/Paper (v2)" becomes
		// "Rock Paper v2" -- a name, not a path, and no stray hyphens in
		// the executable built from it.
		inline std::string SafeName(const std::string& name)
		{
			std::string out;
			for (char c : name)
			{
				bool keep = std::isalnum((unsigned char)c) || c == '-' || c == '_';
				char next = keep ? c : ' ';
				if (next == ' ' && (out.empty() || out.back() == ' '))
					continue;
				out += next;
			}
			while (!out.empty() && out.back() == ' ')
				out.pop_back();
			return out.empty() ? std::string("Game") : out;
		}

		// The executable: lower case, spaces as hyphens.
		inline std::string ExecutableName(const std::string& safeName)
		{
			std::string out;
			for (char c : safeName)
				out += c == ' ' ? '-' : (char)std::tolower((unsigned char)c);
			return out;
		}

		inline bool ReadFile(const fs::path& path, std::string& out)
		{
			std::ifstream file(path, std::ios::in | std::ios::binary);
			if (!file)
				return false;
			std::stringstream buffer;
			buffer << file.rdbuf();
			out = buffer.str();
			return true;
		}

		inline bool IsInside(const fs::path& path, const fs::path& folder)
		{
			fs::path rel = path.lexically_relative(folder);
			return !rel.empty() && *rel.begin() != "..";
		}

		struct Exporter
		{
			fs::path Project;   // absolute
			fs::path Game;      // <package>/game
			ScriptEngine& Engine;
			std::string Error;
			std::set<std::string> Copied;
			// Graduated scripts: absolute .cpp path and class name.
			std::vector<std::pair<std::string, std::string>> Compiled;

			Exporter(fs::path project, fs::path game, ScriptEngine& engine)
				: Project(std::move(project)), Game(std::move(game)), Engine(engine) {}

			bool Fail(const std::string& message)
			{
				if (Error.empty())
					Error = message;
				return false;
			}

			// Where a stored path's file is, the way the editor would find it:
			// absolute, else under the project, else from the working
			// directory (projects store binary-relative paths).
			bool Locate(const std::string& stored, fs::path& out)
			{
				std::error_code ec;
				fs::path p(stored);
				if (p.is_absolute())
					out = p;
				else if (fs::exists(Project / p, ec))
					out = Project / p;
				else
					out = fs::absolute(p, ec);
				out = out.lexically_normal();
				return fs::is_regular_file(out, ec);
			}

			// Its place under game/: project-relative if inside the project,
			// else external/ with its stored layout kept.
			std::string PackagePath(const std::string& stored, const fs::path& source)
			{
				if (IsInside(source, Project))
					return source.lexically_relative(Project).generic_string();
				fs::path p(stored);
				fs::path tail = p.is_absolute() ? source.relative_path() : p.lexically_normal();
				if (!tail.empty() && *tail.begin() == "..")
					tail = source.relative_path();
				return (fs::path("external") / tail).generic_string();
			}

			bool Copy(const fs::path& source, const std::string& packagePath)
			{
				if (!Copied.insert(packagePath).second)
					return true;
				std::error_code ec;
				fs::path target = Game / packagePath;
				fs::create_directories(target.parent_path(), ec);
				fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
				if (ec)
					return Fail("could not copy '" + source.string() + "': " + ec.message());
				return true;
			}

			// A file named inside another (mtllib in an .obj, map_Kd in an
			// .mtl) is relative to that file; copying it the same distance
			// from the copy keeps the reference valid untouched.
			bool CopySibling(const fs::path& ownerSource, const std::string& ownerPackage,
				const std::string& reference, const std::string& what)
			{
				std::error_code ec;
				fs::path source = (ownerSource.parent_path() / reference).lexically_normal();
				if (!fs::is_regular_file(source, ec))
					return Fail(what + " '" + reference + "' named by '" + ownerSource.string() + "' does not exist");
				std::string package = (fs::path(ownerPackage).parent_path() / reference).lexically_normal().generic_string();
				return Copy(source, package);
			}

			bool CopyObjChain(const fs::path& objSource, const std::string& objPackage)
			{
				std::string text;
				if (!ReadFile(objSource, text))
					return Fail("could not read '" + objSource.string() + "'");
				std::istringstream lines(text);
				std::string line;
				while (std::getline(lines, line))
				{
					std::istringstream fields(line);
					std::string key, file;
					fields >> key;
					if (key != "mtllib")
						continue;
					fields >> std::ws;
					std::getline(fields, file);
					while (!file.empty() && (file.back() == '\r' || file.back() == ' '))
						file.pop_back();
					if (!CopySibling(objSource, objPackage, file, "material library"))
						return false;
					fs::path mtlSource = (objSource.parent_path() / file).lexically_normal();
					std::string mtlPackage = (fs::path(objPackage).parent_path() / file).lexically_normal().generic_string();
					if (!CopyMtlTextures(mtlSource, mtlPackage))
						return false;
				}
				return true;
			}

			bool CopyMtlTextures(const fs::path& mtlSource, const std::string& mtlPackage)
			{
				std::string text;
				if (!ReadFile(mtlSource, text))
					return Fail("could not read '" + mtlSource.string() + "'");
				std::istringstream lines(text);
				std::string line;
				while (std::getline(lines, line))
				{
					std::istringstream fields(line);
					std::string key;
					fields >> key;
					if (key.rfind("map_", 0) != 0 && key != "bump" && key != "norm" && key != "disp")
						continue;
					// The file is the last token: options such as "-bm 0.5"
					// may come before it.
					std::string token, file;
					while (fields >> token)
						file = token;
					if (!file.empty() && !CopySibling(mtlSource, mtlPackage, file, "texture"))
						return false;
				}
				return true;
			}

			static std::string RestOfLine(std::istringstream& fields)
			{
				std::string rest;
				std::getline(fields >> std::ws, rest);
				while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' '))
					rest.pop_back();
				return rest;
			}

			// One scene: every file it names copied, every path rewritten.
			bool ExportScene(const std::string& sceneRelative)
			{
				fs::path sceneSource = (Project / sceneRelative).lexically_normal();
				std::string text;
				if (!ReadFile(sceneSource, text))
					return Fail("scene '" + sceneRelative + "' named by project.gsproj does not exist");

				std::ostringstream out;
				std::istringstream lines(text);
				std::string line, tag = "?";
				while (std::getline(lines, line))
				{
					std::istringstream fields(line);
					std::string kind;
					fields >> kind;
					if (kind == "tag")
						tag = RestOfLine(fields);

					if (kind == "mesh")
					{
						std::string path, rest;
						fields >> path;
						rest = RestOfLine(fields);
						// Built-in meshes -- primitives, and the engine's own
						// models (model:cannon.base) -- are made by the player,
						// not loaded: nothing to ship.
						if (path.rfind("primitive:", 0) != 0 && path.rfind("model:", 0) != 0)
						{
							fs::path source;
							if (!Locate(path, source))
								return Fail("mesh '" + path + "' on entity '" + tag + "' in " + sceneRelative + " does not exist");
							std::string package = PackagePath(path, source);
							if (!Copy(source, package))
								return false;
							if (source.extension() == ".obj" && !CopyObjChain(source, package))
								return false;
							path = package;
						}
						out << "mesh " << path << (rest.empty() ? "" : " " + rest) << "\n";
						continue;
					}
					if (kind == "material")
					{
						std::string path = RestOfLine(fields);
						fs::path source;
						if (!Locate(path, source))
							return Fail("material '" + path + "' on entity '" + tag + "' in " + sceneRelative + " does not exist");
						std::string package = PackagePath(path, source);
						if (!Copy(source, package))
							return false;
						out << "material " << package << "\n";
						continue;
					}
					if (kind == "script")
					{
						std::string path = RestOfLine(fields), package;
						if (!ExportScript(path, tag, sceneRelative, package))
							return false;
						out << "script " << package << "\n";
						continue;
					}
					out << line << "\n";
				}

				std::error_code ec;
				fs::path target = Game / sceneRelative;
				fs::create_directories(target.parent_path(), ec);
				std::ofstream file(target, std::ios::binary);
				if (!(file << out.str()))
					return Fail("could not write '" + target.string() + "'");
				return true;
			}

			bool ExportScript(const std::string& path, const std::string& tag, const std::string& scene, std::string& package)
			{
				fs::path source;
				if (!Locate(path, source))
					return Fail("script '" + path + "' on entity '" + tag + "' in " + scene + " does not exist");
				package = PackagePath(path, source);

				// Graduated: the class goes into the player; no file ships,
				// and the scene keeps a path only for the class name.
				fs::path cpp = fs::path(source).replace_extension(".cpp");
				std::string firstLine;
				{
					std::ifstream file(cpp);
					std::getline(file, firstLine);
				}
				if (firstLine.rfind("// GS-GENERATED", 0) == 0)
				{
					std::string name = ScriptEngine::ClassNameFromPath(source.string());
					bool known = false;
					for (const auto& entry : Compiled)
						known = known || entry.second == name;
					if (!known)
						Compiled.push_back({ cpp.string(), name });
					return true;
				}

				std::string tsSource, js, error;
				if (!ReadFile(source, tsSource))
					return Fail("could not read script '" + source.string() + "'");
				if (!Engine.CompileScriptToJS(source.string(), tsSource, error, js))
					return Fail("script '" + path + "' did not compile: " + error);
				package = fs::path(package).replace_extension(".js").generic_string();
				if (!Copied.insert(package).second)
					return true;
				std::error_code ec;
				fs::path target = Game / package;
				fs::create_directories(target.parent_path(), ec);
				std::ofstream file(target, std::ios::binary);
				if (!(file << js))
					return Fail("could not write '" + target.string() + "'");
				return true;
			}
		};

		inline std::string Capitalized(std::string config)
		{
			if (!config.empty())
				config[0] = (char)std::toupper((unsigned char)config[0]);
			return config;
		}
	}

	inline Result Export(const Options& options, ScriptEngine& engine)
	{
		namespace fs = std::filesystem;
		using namespace Detail;
		Result result;
		std::error_code ec;
		auto stage = [&](const std::string& what) { if (options.OnStage) options.OnStage(what); };
		stage("Scanning the project and compiling scripts");

		fs::path project = fs::absolute(options.ProjectFolder, ec).lexically_normal();
		std::string projectString = project.string();
		while (projectString.size() > 1 && projectString.back() == '/')
			projectString.pop_back();
		project = projectString;

		ProjectManifest manifest;
		if (!ReadProjectManifest(projectString, manifest))
		{
			result.Error = "no readable " + std::string(ProjectManifestFilename()) + " with a scene in '" + projectString + "'";
			return result;
		}

		std::string name = SafeName(manifest.Name.empty() ? project.filename().string() : manifest.Name);
		fs::path out = fs::absolute(options.OutDir, ec).lexically_normal();
		// One folder per target, so a Linux and a Windows export of the same
		// game into one output folder cannot collide.
		bool windows = options.Target == "windows";
		bool web = options.Target == "web";
		bool android = options.Target == "android";
		bool ios = options.Target == "ios";
		if (!windows && !web && !android && !ios && options.Target != "linux")
		{
			result.Error = "unknown export target '" + options.Target + "' (linux, windows, web, android, ios)";
			return result;
		}
		std::string platform = options.Target;
		fs::path package = out / platform / name;
		fs::path build = out / ".build" / platform / name;

		// A re-export replaces the package: stale files from a previous one
		// must not ride along.
		fs::remove_all(package, ec);
		fs::create_directories(package / "game", ec);
		fs::create_directories(build, ec);
		if (ec)
		{
			result.Error = "could not create '" + package.string() + "': " + ec.message();
			return result;
		}

		// A failed export leaves nothing behind: a half-written package that
		// looks finished is worse than none.
		struct RemoveOnFailure
		{
			const Result& Outcome;
			fs::path Package;
			~RemoveOnFailure()
			{
				std::error_code ignored;
				if (!Outcome.Ok)
					fs::remove_all(Package, ignored);
			}
		} cleanup{ result, package };

		Exporter exporter(project, package / "game", engine);

		std::set<std::string> scenes;
		for (const SceneEntry& entry : manifest.Scenes)
			scenes.insert(entry.RelativePath);
		scenes.insert(manifest.SceneRelativePath);
		for (const std::string& scene : scenes)
			if (!exporter.ExportScene(scene))
			{
				result.Error = exporter.Error;
				return result;
			}
		fs::copy_file(project / ProjectManifestFilename(), package / "game" / ProjectManifestFilename(),
			fs::copy_options::overwrite_existing, ec);

		// The game's UI screens (Runtime/GameUI.h). No scene names them --
		// a screen is asked for by name while the game runs -- so the whole
		// ui/ folder goes, as it is.
		if (fs::is_directory(project / "ui"))
		{
			fs::create_directories(package / "game" / "ui", ec);
			fs::copy(project / "ui", package / "game" / "ui", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.Error = "could not copy the project's ui folder: " + ec.message();
				return result;
			}
		}

		// The project's controls (GS::InputMap), when it has rebound any.
		if (fs::exists(project / GS::InputMap::FileName()))
			fs::copy_file(project / GS::InputMap::FileName(), package / "game" / GS::InputMap::FileName(),
				fs::copy_options::overwrite_existing, ec);

		// The game UI's fonts (GameUI::Font), unless the project has its own:
		// the player has no editor assets to borrow them from. 1.1 MB.
		{
			fs::path fonts = fs::path("assets") / "fonts";   // the editor runs from its binary folder
			fs::create_directories(package / "game" / "fonts", ec);
			for (const char* file : { "DejaVuSans.ttf", "DejaVuSansMono.ttf", "DejaVuSansMono-LICENSE.txt" })
				if (!fs::exists(project / "fonts" / file))
					fs::copy_file(fonts / file, package / "game" / "fonts" / file, fs::copy_options::overwrite_existing, ec);
				else
					fs::copy_file(project / "fonts" / file, package / "game" / "fonts" / file, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.Error = "could not copy the game UI's fonts: " + ec.message();
				return result;
			}
		}

		// The recordings humanoids walk and run from (Runtime/Locomotion.h),
		// at the path they are asked for under the game's root, where
		// GS::Assets::Resolve finds them. 1 MB; without them a humanoid falls
		// back to the old physical walk.
		{
			fs::path from = fs::path("assets") / "mocap" / "cmu";
			fs::path to = package / "game" / "assets" / "mocap" / "cmu";
			fs::create_directories(to, ec);
			for (const std::string& file : LocomotionClipFiles())
				if (!ec)
					fs::copy_file(from / file, to / file, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.Error = "could not copy the locomotion clips: " + ec.message();
				return result;
			}
		}

		// This project's compiled-script registry, and the player built
		// against it.
		fs::path header = build / "compiled_scripts.gen.h";
		{
			std::ofstream h(header);
			h << "// Generated by GameExporter for '" << manifest.Name << "' -- do not edit.\n"
			  << "#pragma once\n#include <vector>\n";
			for (const auto& [cpp, cls] : exporter.Compiled)
				h << "#include \"" << cpp << "\"\n";
			h << "inline const std::vector<CompiledScriptEntry> s_CompiledScripts = {\n";
			for (const auto& [cpp, cls] : exporter.Compiled)
				h << "\tMakeCompiledScriptEntry<GeneratedScripts::" << cls << ">(\"" << cls << "\"),\n";
			h << "};\n";
		}

#ifdef GS_REPO_ROOT
		std::string root = GS_REPO_ROOT;
#else
		result.Error = "exporting needs the engine source tree (GS_REPO_ROOT) and is Linux-only for now";
		return result;
#endif
		fs::path log = build / "build.log";

		// iOS: no player is built here -- the iOS SDK exists only inside
		// Xcode. The package is an Xcode project holding the engine source,
		// the compiled scripts and game/, which Xcode builds and signs.
		if (ios)
		{
			ec.clear();
			fs::path icon = manifest.Icon.empty()
				? fs::path(root) / "TestEnv" / "assets" / "export" / "default-icon.png"
				: project / manifest.Icon;
			if (!fs::is_regular_file(icon, ec))
			{
				result.Error = "icon '" + icon.string() + "' named by project.gsproj does not exist";
				return result;
			}
			fs::path xcode = build / "xcode";
			std::string generate = "python3 \"" + root + "/tools/ios_project.py\" --game \""
				+ (package / "game").string() + "\" --name \"" + name + "\" --icon \"" + icon.string()
				+ "\" --scripts \"" + header.string() + "\" --out \"" + xcode.string()
				+ "\" > \"" + log.string() + "\" 2>&1";
			stage("Writing the Xcode project");
			if (std::system(generate.c_str()) != 0)
			{
				result.Error = "writing the Xcode project failed -- see " + log.string();
				return result;
			}
			fs::remove_all(package, ec);
			fs::rename(xcode, package, ec);
			if (ec)
			{
				result.Error = "could not move the Xcode project into place: " + ec.message();
				return result;
			}

			stage("Archiving");
			fs::path archive = out / (name + "-ios-xcode.zip");
			fs::remove(archive, ec);
			std::string pack = "cd \"" + (out / platform).string() + "\" && python3 -m zipfile -c \"" + archive.string() + "\" \"" + name + "\"";
			if (std::system(pack.c_str()) != 0)
			{
				result.Error = "archiving failed for '" + archive.string() + "'";
				return result;
			}
			result.Ok = true;
			result.PackageDir = package.string();
			result.Archive = archive.string();
			result.Executable = (package / (name + ".xcodeproj")).string();
			return result;
		}

		std::string command = "python3 \"" + root + "/gs.py\" build " + options.Config
			+ (windows ? " --target windows" : "")
			+ (android ? " --target android" : "")
			+ (web ? " --target web --web-game \"" + (package / "game").string() + "\"" : "")
			+ " --player-scripts \"" + header.string() + "\" > \"" + log.string() + "\" 2>&1";
		GS_INFO("GameExporter: building the player for '{0}' ({1})", manifest.Name, options.Config);
		stage("Building the player (about a minute)");
		if (std::system(command.c_str()) != 0)
		{
			result.Error = "building the player failed -- see " + log.string();
			return result;
		}

		std::string config = Capitalized(options.Config);
		stage("Packaging");
		fs::path bin = fs::path(root) / "bin";

		// The web: the page, its script, the wasm module and the preloaded
		// filesystem (.data -- the game is inside it, so game/ was only the
		// build's input). index.html carries the game's name; a README says
		// it must be served, since browsers will not load .wasm from file://.
		if (web)
		{
			ec.clear();   // earlier steps may leave it set; the copies below check it
			fs::path built = bin / ("Export-" + config + "-linux-x86_64-web") / "GSPlayer";
			for (const char* file : { "GSPlayer.js", "GSPlayer.wasm", "GSPlayer.data" })
				if (!ec)
					fs::copy_file(built / file, package / file, fs::copy_options::overwrite_existing, ec);
			std::string page;
			if (ec || !ReadFile(built / "GSPlayer.html", page))
			{
				result.Error = "could not copy the built page: " + (ec ? ec.message() : std::string("no GSPlayer.html"));
				return result;
			}
			size_t title = page.find("<title>GS Game</title>");
			if (title != std::string::npos)
				page.replace(title, std::strlen("<title>GS Game</title>"), "<title>" + name + "</title>");
			std::ofstream(package / "index.html", std::ios::binary) << page;
			std::ofstream(package / "README.txt") << name << " -- a GS game for the web.\n\n"
				"Serve this folder with any static web server and open index.html, for example:\n"
				"    python3 -m http.server 8000\n"
				"    then http://localhost:8000/\n"
				"Opening index.html straight from disk will not work: browsers refuse to\n"
				"load the game's .wasm and .data files from a file:// page.\n"
				"Needs a browser with WebGL 2 (any current Firefox, Chrome, Edge or Safari).\n";
			fs::remove_all(package / "game", ec);

			stage("Archiving");
			fs::path archive = out / (name + "-web.zip");
			fs::remove(archive, ec);
			std::string pack = "cd \"" + (out / platform).string() + "\" && python3 -m zipfile -c \"" + archive.string() + "\" \"" + name + "\"";
			if (std::system(pack.c_str()) != 0)
			{
				result.Error = "archiving failed for '" + archive.string() + "'";
				return result;
			}
			result.Ok = true;
			result.PackageDir = package.string();
			result.Archive = archive.string();
			result.Executable = (package / "index.html").string();
			return result;
		}
		// Android: one signed APK, assembled by gs.py apk from the player
		// library and game/ (which goes inside it, under assets/). Installed
		// with `adb install`, or opened on the phone itself.
		if (android)
		{
			ec.clear();
			fs::path icon = manifest.Icon.empty()
				? fs::path(root) / "TestEnv" / "assets" / "export" / "default-icon.png"
				: project / manifest.Icon;
			if (!fs::is_regular_file(icon, ec))
			{
				result.Error = "icon '" + icon.string() + "' named by project.gsproj does not exist";
				return result;
			}
			fs::path lib = bin / ("Export-" + config + "-linux-ARM64-android") / "GSPlayer" / "libgsplayer.so";
			fs::path apk = package / (name + ".apk");
			std::string assemble = "python3 \"" + root + "/gs.py\" apk --lib \"" + lib.string()
				+ "\" --game \"" + (package / "game").string() + "\" --name \"" + name
				+ "\" --icon \"" + icon.string() + "\" --out \"" + apk.string()
				+ "\" >> \"" + log.string() + "\" 2>&1";
			stage("Assembling and signing the APK");
			if (std::system(assemble.c_str()) != 0 || !fs::exists(apk, ec))
			{
				result.Error = "assembling the APK failed -- see " + log.string();
				return result;
			}
			fs::remove_all(package / "game", ec);
			std::ofstream(package / "README.txt") << name << " -- a GS game for Android (8.0 or later, 64-bit ARM).\n\n"
				"Install it from a computer with the phone connected and USB debugging on:\n"
				"    adb install -r \"" << name << ".apk\"\n"
				"or copy the .apk to the phone and open it there (allow installing from\n"
				"that app when asked). It is signed with a debug key: fine for testing\n"
				"and sharing directly, not for a store.\n";
			result.Ok = true;
			result.PackageDir = package.string();
			result.Archive = apk.string();
			result.Executable = apk.string();
			return result;
		}
		fs::path executable = package / (ExecutableName(name) + (windows ? ".exe" : ""));
		// What ships beside the player: the engine library, and on Windows
		// llvm-mingw's C++ runtime -- shipped as DLLs rather than linked
		// statically, because a static copy in both the exe and GS.dll would
		// give them separate C++ runtimes, and strings, shared pointers and
		// exceptions cross that boundary.
		std::vector<fs::path> libraries;
		fs::path player;
		if (windows)
		{
			player = bin / ("Export-" + config + "-windows-x86_64") / "GSPlayer" / "GSPlayer.exe";
			fs::path runtime = fs::path(root) / "vendor" / "bin" / "llvm-mingw" / "x86_64-w64-mingw32" / "bin";
			libraries = { bin / (config + "-windows-x86_64") / "GS" / "GS.dll", runtime / "libc++.dll", runtime / "libunwind.dll" };
		}
		else
		{
			player = bin / ("Export-" + config + "-linux-x86_64") / "GSPlayer" / "GSPlayer";
			libraries = { bin / (config + "-linux-x86_64") / "GS" / "libGS.so" };
		}
		fs::copy_file(player, executable, fs::copy_options::overwrite_existing, ec);
		for (const fs::path& library : libraries)
			if (!ec)
				fs::copy_file(library, package / library.filename(), fs::copy_options::overwrite_existing, ec);
		if (ec)
		{
			result.Error = "could not copy the built player: " + ec.message();
			return result;
		}
		fs::permissions(executable, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
			| fs::perms::others_read | fs::perms::others_exec, ec);

		// A .tar.gz for Linux, a .zip for Windows (what each platform's
		// users expect); either way <Game Name>/ is its single top folder.
		stage("Archiving");
		fs::path archive = out / (name + "-" + platform + "-x86_64" + (windows ? ".zip" : ".tar.gz"));
		fs::remove(archive, ec);
		std::string pack = windows
			? "cd \"" + (out / platform).string() + "\" && python3 -m zipfile -c \"" + archive.string() + "\" \"" + name + "\""
			: "tar -czf \"" + archive.string() + "\" -C \"" + (out / platform).string() + "\" \"" + name + "\"";
		if (std::system(pack.c_str()) != 0)
		{
			result.Error = "archiving failed for '" + archive.string() + "'";
			return result;
		}
		result.Executable = executable.string();
		if (windows)
		{
			result.Ok = true;
			result.PackageDir = package.string();
			result.Archive = archive.string();
			return result;
		}

		// The AppImage: the same package as one double-clickable file. An
		// AppDir is laid out -- AppRun, a .desktop entry, the icon, the
		// package under usr/bin -- and appimagetool squashes it onto the
		// pinned type-2 runtime. Run with APPIMAGE_EXTRACT_AND_RUN so neither
		// the tool nor the result needs FUSE on the machine.
		stage("Building the AppImage");
		fs::path tools = fs::path(root) / "vendor" / "bin" / "appimagetool";
		if (!fs::exists(tools / "appimagetool", ec) || !fs::exists(tools / "runtime-x86_64", ec))
		{
			std::string fetch = "python3 \"" + root + "/gs.py\" fetch appimagetool >> \"" + log.string() + "\" 2>&1";
			if (std::system(fetch.c_str()) != 0)
			{
				result.Error = "could not fetch appimagetool -- see " + log.string();
				return result;
			}
		}
		std::string exe = ExecutableName(name);
		fs::path appDir = build / "AppDir";
		fs::remove_all(appDir, ec);
		fs::create_directories(appDir / "usr" / "bin", ec);
		fs::copy(package, appDir / "usr" / "bin", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		fs::path icon = manifest.Icon.empty()
			? fs::path(root) / "TestEnv" / "assets" / "export" / "default-icon.png"
			: project / manifest.Icon;
		if (!fs::is_regular_file(icon, ec))
		{
			result.Error = "icon '" + icon.string() + "' named by project.gsproj does not exist";
			return result;
		}
		fs::copy_file(icon, appDir / (exe + ".png"), fs::copy_options::overwrite_existing, ec);
		fs::copy_file(icon, appDir / ".DirIcon", fs::copy_options::overwrite_existing, ec);
		{
			std::ofstream desktop(appDir / (exe + ".desktop"));
			desktop << "[Desktop Entry]\nType=Application\nName=" << name << "\nExec=" << exe
				<< "\nIcon=" << exe << "\nCategories=Game;\nTerminal=false\n";
			std::ofstream run(appDir / "AppRun");
			// The player changes to its own folder itself; AppRun only has
			// to find it inside the mounted (or extracted) image.
			run << "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\nexec \"$HERE/usr/bin/" << exe << "\" \"$@\"\n";
		}
		fs::permissions(appDir / "AppRun", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
			| fs::perms::others_read | fs::perms::others_exec, ec);
		fs::path appImage = out / (name + "-x86_64.AppImage");
		fs::remove(appImage, ec);
		std::string squash = "ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 \"" + (tools / "appimagetool").string()
			+ "\" --runtime-file \"" + (tools / "runtime-x86_64").string() + "\" \"" + appDir.string()
			+ "\" \"" + appImage.string() + "\" >> \"" + log.string() + "\" 2>&1";
		if (std::system(squash.c_str()) != 0 || !fs::exists(appImage, ec))
		{
			result.Error = "appimagetool failed -- see " + log.string();
			return result;
		}

		result.Ok = true;
		result.AppImage = appImage.string();
		result.PackageDir = package.string();
		result.Archive = archive.string();
		result.Executable = executable.string();
		return result;
	}

}
