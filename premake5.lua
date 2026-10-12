workspace "GS"
    architecture "x64"

    configurations
    {
        "Debug",
        "Release",
        "Dist"
    }

-- Sanitizers are a *generation* option rather than a fourth configuration.
--
-- A configuration would have to be repeated in all five project files, and the
-- three that describe vendored code have no opinion about any of this. An
-- option composes with the configurations instead: `--sanitize` can be had with
-- debug, release or dist, which matters because optimisation changes what
-- undefined behaviour *does* even though it does not change whether UBSan sees
-- it -- 2026-08-17's miscompile only bit in release.
-- Two modes rather than one flag, because **ASan and TSan cannot be combined**
-- -- they both want to own the process's memory layout, and asking for both is
-- a link error rather than a stronger check. So a race hunt is its own build,
-- and its own tree.
newoption {
    trigger = "sanitize",
    value = "MODE",
    description = "Instrument, in a bin/ tree of its own",
    allowed = {
        { "address", "ASan + UBSan: memory errors, leaks, undefined behaviour" },
        { "thread",  "TSan: data races" }
    }
}

newoption {
    trigger = "web",
    description = "Build for the browser with Emscripten (WebGL2): gs.py build <config> --target web"
}

newoption {
    trigger = "web-game",
    value = "PATH",
    description = "With --web: the game folder to preload into the page at /game"
}

newoption {
    trigger = "player-scripts",
    value = "PATH",
    description = "Build GSPlayer against one project's compiled-script registry header, into bin/Export-* (TestEnv's GameExporter)"
}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- The web build (export piece 5) is generated on the Linux host but is not
-- Linux: its own tree, GS_PLATFORM_WEB instead of GS_PLATFORM_LINUX (every
-- first-party "system:linux" block below stands aside under --web), and
-- WebAssembly exceptions -- Emscripten's default aborts on any throw, and the
-- engine and spdlog throw and catch.
if _OPTIONS["web"] then
    outputdir = outputdir .. "-web"
    filter "options:web"
        -- GS_GLES on the command line, not only in Core.h: gspch.h does not
        -- include Core.h, and a file testing it first would silently take the
        -- desktop GL path.
        defines { "GS_PLATFORM_WEB", "GS_GLES" }
        buildoptions { "-fwasm-exceptions" }
        linkoptions { "-fwasm-exceptions" }
    filter {}
end

-- Post-build copies for a Windows *target*. Premake expands {COPY}/{COPYDIR}
-- for the target's shell (cmd and xcopy), but when Windows is cross-built from
-- Linux (gs.py build --target windows) make runs them in the host's /bin/sh,
-- which choked on the first one. On a non-Windows host these spell the
-- host's own commands instead; built on Windows, premake's tokens as before.
function WindowsCopyFile(from, toDir)
    if os.host() ~= "windows" then
        return "mkdir -p " .. toDir .. " && cp -f " .. from .. " " .. toDir
    end
    return "{COPY} " .. from .. " " .. toDir
end

function WindowsCopyDir(from, to)
    if os.host() ~= "windows" then
        return "mkdir -p " .. to .. " && cp -rf " .. from .. "/. " .. to
    end
    return "{COPYDIR} " .. from .. " " .. to
end

-- Its own tree, so nothing links an instrumented object against a plain one
-- and no plain build is ever slowed by a stale instrumented object. A suffix on
-- the directory rather than flags added in place, because make cannot tell that
-- the flags changed and would leave both kinds of .o in the same folder.
if _OPTIONS["sanitize"] then
    outputdir = outputdir .. "-sanitize-" .. _OPTIONS["sanitize"]
end

--include directories 
IncludeDir = {}
IncludeDir["GLFW"] = "GS/vendor/glfw/include"
IncludeDir["Glad"] = "GS/vendor/Glad/include"
IncludeDir["glm"] = "GS/vendor/glm"
IncludeDir["ImGui"] = "GS/vendor/imgui"
IncludeDir["stb_image"] = "GS/vendor/stb_image"
IncludeDir["miniaudio"] = "GS/vendor/miniaudio"
IncludeDir["libvterm"] = "GS/vendor/libvterm/include"
IncludeDir["quickjs"] = "GS/vendor/quickjs"
IncludeDir["imnodes"] = "GS/vendor/imnodes"

include "GS/vendor/glfw"
include "GS/vendor/Glad"
include "GS/vendor/imgui_premake5.lua"
include "GS/vendor/libvterm_premake5.lua"
include "GS/vendor/quickjs_premake5.lua"

project "GS"
    -- Must match the case of the source folder: on Windows this resolved to
    -- GS/ anyway, but a case-sensitive filesystem would make a second dir.
    location "GS"
    kind "SharedLib"
    language "C++"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    pchheader "gspch.h"
    pchsource "GS/src/gspch.cpp"

    files
    {
        "%{prj.name}/src/**.h",
        "%{prj.name}/src/**.cpp",
        "%{prj.name}/vendor/stb_image/**.h",
        "%{prj.name}/vendor/stb_image/**.cpp",
        "%{prj.name}/vendor/miniaudio/**.h",
        "%{prj.name}/vendor/miniaudio/**.c"
    }

    -- miniaudio.c is C, so it must not be handed the C++ precompiled header.
    filter "files:**.c"
        flags { "NoPCH" }

    filter {}

    includedirs
    {
        "%{prj.name}/src",
        "%{prj.name}/vendor/spdlog/include",
        "%{IncludeDir.GLFW}",
        "%{IncludeDir.Glad}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.stb_image}",
        "%{IncludeDir.miniaudio}",
    }

    links
    {
        "GLFW",
        "Glad",
        "ImGui"
    }

    -- Glad provides the GL headers; stop GLFW from pulling in its own.
    defines
    {
        "GLFW_INCLUDE_NONE"
    }

    filter "system:windows"
        cppdialect "C++17"
        staticruntime "On"
        systemversion "latest"

        -- C4251: exported classes hold std::string/vector/shared_ptr members.
        -- Harmless here because the engine and app share one runtime, and it
        -- would otherwise fire on nearly every GS_API class.
        disablewarnings { "4251" }

        -- libc++ (llvm-mingw) no longer ships a private header the bundled
        -- fmt includes on Windows; see the shim's own comment.
        includedirs { "%{prj.name}/src/Platform/Windows/shim" }

        defines
        {
            "GS_PLATFORM_WINDOWS",
            "GS_BUILD_DLL",
            -- windows.h otherwise defines min/max as macros, which break
            -- every std::min/std::max in the engine.
            "NOMINMAX",
            "WIN32_LEAN_AND_MEAN"
        }

        links
        {
            "opengl32",
            "gdi32",
            "user32",
            "shell32",
            "winmm",
            "ws2_32"
        }

        postbuildcommands
        {
            WindowsCopyFile("%{cfg.buildtarget.relpath}", "../bin/" .. outputdir .. "/TestEnv"),
            WindowsCopyFile("%{cfg.buildtarget.relpath}", "../bin/" .. outputdir .. "/GSPlayer")
        }

    filter { "system:linux", "not options:web" }
        cppdialect "C++17"
        staticruntime "off"
        pic "On"

        defines
        {
            "GS_PLATFORM_LINUX",
            "GS_BUILD_DLL"
        }

        -- GLFW is built as a static lib with the X11 backend, so its
        -- transitive deps have to be named here.
        links
        {
            "GL",
            "X11",
            "pthread",
            "dl",
            "m"
        }

        postbuildcommands
        {
            ("{MKDIR} ../bin/" .. outputdir .. "/TestEnv"),
            ("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/TestEnv" ),
            ("{MKDIR} ../bin/" .. outputdir .. "/GSPlayer"),
            ("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/GSPlayer" )
        }

    -- The browser: one wasm module, so the engine links in statically. GLFW
    -- comes from Emscripten (-sUSE_GLFW=3) and GL from WebGL2, so neither
    -- vendored GLFW nor glad is built or linked; a shim stands in for
    -- glad's header.
    filter "options:web"
        kind "StaticLib"
        removeincludedirs { "%{IncludeDir.Glad}" }
        includedirs { "%{prj.name}/src/Platform/GLES/shim" }
        removelinks { "GLFW", "Glad" }

    filter "configurations:Debug"
        defines { "GS_DEBUG", "GS_ENABLE_ASSERTS", "GS_PROFILE" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "GS_RELEASE", "GS_PROFILE" }
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "GS_DIST"
        runtime "Release"
        optimize "Full"
        symbols "Off"

    -- ASan and UBSan together, and symbols regardless of configuration -- a
    -- sanitizer report without a file and line is most of the value gone.
    --
    -- Only the two first-party projects are instrumented. ASan's malloc
    -- bookkeeping is process-wide, so a use-after-free in vendored code is
    -- still caught; what an uninstrumented GLFW loses is the redzones around
    -- its own stack and global arrays, which is a fair trade for not reading
    -- three thousand lines of somebody else's undefined behaviour.
    --
    -- Recoverable on purpose: UBSan's default is to print and carry on, so one
    -- sweep names every site it hits rather than only the first.
    filter "options:sanitize=address"
        buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=address,undefined" }
        symbols "On"

    -- TSan instruments every memory access to track happens-before, so it has
    -- to see *both* sides of a race to report one. The audio mixer runs on
    -- miniaudio's device thread inside libGS, and the demo code that claims
    -- and stops voices runs on the main thread in TestEnv, so both first-party
    -- projects being instrumented is what makes this able to say anything at
    -- all.
    filter "options:sanitize=thread"
        buildoptions { "-fsanitize=thread", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=thread" }
        symbols "On"

            

project "TestEnv"
    location "TestEnv"
    kind "ConsoleApp"
    language "C++" 

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "%{prj.name}/src/**.h",
        "%{prj.name}/src/**.cpp",
        "Runtime/**.h"
    }

    includedirs
    {
        "GS/vendor/spdlog/include",
        "GS/src",
        "%{IncludeDir.glm}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.libvterm}",
        "%{IncludeDir.quickjs}",
        "%{IncludeDir.stb_image}",
        "%{IncludeDir.imnodes}",
        -- Runtime/ is what a shipped game runs on, shared with GSPlayer:
        -- see docs/superpowers/specs/2026-10-07-game-runtime-player-design.md.
        "Runtime"
    }

    links
    {
        "GS",
        "ImGui",
        "libvterm",
        "quickjs"
    }

    -- Assets are loaded by relative path at runtime, so they have to sit next
    -- to the executable. Copying rather than symlinking keeps the output
    -- directory self-contained.
    --
    -- The two platforms need different arguments, because {COPYDIR} expands to
    -- tools that disagree about what the destination means: `cp -rf src dst`
    -- puts src *inside* dst when dst exists (so repeated builds would nest
    -- assets/assets/assets), while xcopy copies the contents. Naming the
    -- parent on one and the folder on the other gives the same result on both.
    filter "system:windows"
        cppdialect "C++17"
        staticruntime "On"
        systemversion "latest"

        disablewarnings { "4251" }

        defines
        {
            "GS_PLATFORM_WINDOWS",
        }

        postbuildcommands
        {
            WindowsCopyDir("%{wks.location}/TestEnv/assets", "%{cfg.targetdir}/assets")
        }

    filter { "system:linux", "not options:web" }
        cppdialect "C++17"
        staticruntime "off"

        defines
        {
            "GS_PLATFORM_LINUX",
            -- Absolute, baked in at generation time -- the GSS-to-C++
            -- transpiler's syntax-check step needs to hand g++ real
            -- -I paths, and the executable has no other way to find
            -- GS/src from wherever it happens to be run (bin/<config>/
            -- TestEnv/, always three directories deep, but "always
            -- three directories deep" is exactly the kind of assumption
            -- that quietly breaks if this layout ever changes).
            'GS_REPO_ROOT="' .. _MAIN_SCRIPT_DIR .. '"',
        }

        links
        {
            "pthread",
            "dl",
            "util"
        }

        -- Find libGS.so next to the executable instead of on the system path.
        linkoptions { "-Wl,-rpath,'$$ORIGIN'" }

        postbuildcommands
        {
            ("{COPYDIR} %{wks.location}/TestEnv/assets %{cfg.targetdir}")
        }

    filter "configurations:Debug"
        defines { "GS_DEBUG", "GS_ENABLE_ASSERTS", "GS_PROFILE" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "GS_RELEASE", "GS_PROFILE" }
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "GS_DIST"
        runtime "Release"
        optimize "Full"
        symbols "Off"

    -- ASan and UBSan together, and symbols regardless of configuration -- a
    -- sanitizer report without a file and line is most of the value gone.
    --
    -- Only the two first-party projects are instrumented. ASan's malloc
    -- bookkeeping is process-wide, so a use-after-free in vendored code is
    -- still caught; what an uninstrumented GLFW loses is the redzones around
    -- its own stack and global arrays, which is a fair trade for not reading
    -- three thousand lines of somebody else's undefined behaviour.
    --
    -- Recoverable on purpose: UBSan's default is to print and carry on, so one
    -- sweep names every site it hits rather than only the first.
    filter "options:sanitize=address"
        buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=address,undefined" }
        symbols "On"

    -- TSan instruments every memory access to track happens-before, so it has
    -- to see *both* sides of a race to report one. The audio mixer runs on
    -- miniaudio's device thread inside libGS, and the demo code that claims
    -- and stops voices runs on the main thread in TestEnv, so both first-party
    -- projects being instrumented is what makes this able to say anything at
    -- all.
    filter "options:sanitize=thread"
        buildoptions { "-fsanitize=thread", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=thread" }
        symbols "On"

-- GSPlayer: runs a project as a game, with no editor in it -- export piece 1,
-- docs/superpowers/specs/2026-10-07-game-runtime-player-design.md. Shares
-- the header-only Runtime/ with TestEnv; links only what a game needs (no
-- ImGui panels, no terminal, no node editor).
project "GSPlayer"
    location "Player"
    kind "ConsoleApp"
    language "C++"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "Player/src/**.h",
        "Player/src/**.cpp",
        "Runtime/**.h"
    }

    includedirs
    {
        "GS/vendor/spdlog/include",
        "GS/src",
        "%{IncludeDir.glm}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.quickjs}",
        "%{IncludeDir.stb_image}",
        "Runtime"
    }

    links
    {
        "GS",
        "quickjs"
    }

    -- An export build: this player compiled against one project's own
    -- compiled scripts (the registry header GameExporter generates), into
    -- folders of its own so the normal player build is never overwritten.
    if _OPTIONS["player-scripts"] then
        targetdir ("bin/Export-" .. outputdir .. "/%{prj.name}")
        objdir ("bin-int/Export-" .. outputdir .. "/%{prj.name}")
        defines { 'GS_COMPILED_SCRIPTS_HEADER="' .. _OPTIONS["player-scripts"] .. '"' }
    end

    filter "system:windows"
        cppdialect "C++17"
        staticruntime "On"
        systemversion "latest"
        disablewarnings { "4251" }
        defines { "GS_PLATFORM_WINDOWS", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }
        includedirs { "GS/src/Platform/Windows/shim" }
        postbuildcommands
        {
            WindowsCopyDir("%{wks.location}/TestEnv/assets", "%{cfg.targetdir}/assets")
        }

    filter { "system:linux", "not options:web" }
        cppdialect "C++17"
        staticruntime "off"
        defines { "GS_PLATFORM_LINUX" }
        links { "pthread", "dl" }
        -- libGS.so beside the binary, as TestEnv -- what lets a packaged
        -- game carry its own engine library.
        linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
        -- The engine's own assets (the TypeScript compiler scripts need,
        -- fonts) and, for now, the demo projects to run. Piece 2's packager
        -- will copy only what one game uses.
        postbuildcommands
        {
            ("{COPYDIR} %{wks.location}/TestEnv/assets %{cfg.targetdir}")
        }

    -- A shipped Windows game opens no console window behind itself. Debug
    -- and Release keep the console for their logs.
    filter { "system:windows", "configurations:Dist" }
        kind "WindowedApp"

    -- GSPlayer.html (+ .js, .wasm, .data): the game preloaded into the
    -- in-memory filesystem at /game when --web-game names it. 5 MB of stack:
    -- Emscripten's default 64 KB is far less than the physics and QuickJS
    -- use on desktop.
    filter "options:web"
        targetextension ".html"
        links { "ImGui" }
        linkoptions {
            "-sUSE_GLFW=3",
            "-sMIN_WEBGL_VERSION=2",
            "-sMAX_WEBGL_VERSION=2",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sFORCE_FILESYSTEM=1",
            "-sSTACK_SIZE=5MB",
            "-sEXPORTED_RUNTIME_METHODS=FS",
            "--shell-file %{wks.location}/Player/web/shell.html",
        }
    if _OPTIONS["web-game"] then
        filter "options:web"
            linkoptions { '--preload-file "' .. _OPTIONS["web-game"] .. '@/game"' }
    end

    filter "configurations:Debug"
        defines { "GS_DEBUG", "GS_ENABLE_ASSERTS", "GS_PROFILE" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "GS_RELEASE", "GS_PROFILE" }
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "GS_DIST"
        runtime "Release"
        optimize "Full"
        symbols "Off"

    filter "options:sanitize=address"
        buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=address,undefined" }
        symbols "On"

    filter "options:sanitize=thread"
        buildoptions { "-fsanitize=thread", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=thread" }
        symbols "On"
