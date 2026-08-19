workspace "EGSS"
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
newoption {
    trigger = "sanitize",
    description = "Instrument with ASan and UBSan, in a bin/ tree of their own"
}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Its own tree, so nothing links an instrumented object against a plain one
-- and no plain build is ever slowed by a stale instrumented object. A suffix on
-- the directory rather than flags added in place, because make cannot tell that
-- the flags changed and would leave both kinds of .o in the same folder.
if _OPTIONS["sanitize"] then
    outputdir = outputdir .. "-sanitize"
end

--include directories 
IncludeDir = {}
IncludeDir["GLFW"] = "EGSS/vendor/glfw/include"
IncludeDir["Glad"] = "EGSS/vendor/Glad/include"
IncludeDir["glm"] = "EGSS/vendor/glm"
IncludeDir["ImGui"] = "EGSS/vendor/imgui"
IncludeDir["stb_image"] = "EGSS/vendor/stb_image"
IncludeDir["miniaudio"] = "EGSS/vendor/miniaudio"

include "EGSS/vendor/glfw"
include "EGSS/vendor/Glad"
include "EGSS/vendor/imgui_premake5.lua"

project "EGSS"
    -- Must match the case of the source folder: on Windows this resolved to
    -- EGSS/ anyway, but a case-sensitive filesystem would make a second dir.
    location "EGSS"
    kind "SharedLib"
    language "C++"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    pchheader "egsspch.h"
    pchsource "EGSS/src/egsspch.cpp"

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
        -- would otherwise fire on nearly every EGSS_API class.
        disablewarnings { "4251" }

        removefiles { "%{prj.name}/src/Platform/Linux/**" }

        defines
        {
            "EGSS_PLATFORM_WINDOWS",
            "EGSS_BUILD_DLL"
        }

        links
        {
            "opengl32",
            "gdi32",
            "user32",
            "shell32",
            "winmm"
        }

        postbuildcommands
        {
            ("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/TestEnv" )
        }

    filter "system:linux"
        cppdialect "C++17"
        staticruntime "off"
        pic "On"

        removefiles { "%{prj.name}/src/Platform/Windows/**" }

        defines
        {
            "EGSS_PLATFORM_LINUX",
            "EGSS_BUILD_DLL"
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
            ("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/TestEnv" )
        }

    filter "configurations:Debug"
        defines { "EGSS_DEBUG", "EGSS_ENABLE_ASSERTS", "EGSS_PROFILE" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "EGSS_RELEASE", "EGSS_PROFILE" }
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "EGSS_DIST"
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
    filter "options:sanitize"
        buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=address,undefined" }
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
        "%{prj.name}/src/**.cpp"
    }

    includedirs
    {
        "EGSS/vendor/spdlog/include",
        "EGSS/src",
        "%{IncludeDir.glm}",
        "%{IncludeDir.ImGui}"
    }

    links 
    {
        "EGSS",
        "ImGui"
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
            "EGSS_PLATFORM_WINDOWS",
        }

        postbuildcommands
        {
            ("{COPYDIR} %{wks.location}/TestEnv/assets %{cfg.targetdir}/assets")
        }

    filter "system:linux"
        cppdialect "C++17"
        staticruntime "off"

        defines
        {
            "EGSS_PLATFORM_LINUX",
        }

        links
        {
            "pthread",
            "dl"
        }

        -- Find libEGSS.so next to the executable instead of on the system path.
        linkoptions { "-Wl,-rpath,'$$ORIGIN'" }

        postbuildcommands
        {
            ("{COPYDIR} %{wks.location}/TestEnv/assets %{cfg.targetdir}")
        }

    filter "configurations:Debug"
        defines { "EGSS_DEBUG", "EGSS_ENABLE_ASSERTS", "EGSS_PROFILE" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines { "EGSS_RELEASE", "EGSS_PROFILE" }
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "EGSS_DIST"
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
    filter "options:sanitize"
        buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }
        linkoptions { "-fsanitize=address,undefined" }
        symbols "On"