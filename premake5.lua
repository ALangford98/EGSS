workspace "EGSS"
    architecture "x64"

    configurations
    {
        "Debug",
        "Release",
        "Dist"
    }

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

--include directories 
IncludeDir = {}
IncludeDir["GLFW"] = "EGSS/vendor/glfw/include"
IncludeDir["Glad"] = "EGSS/vendor/Glad/include"
IncludeDir["glm"] = "EGSS/vendor/glm"
IncludeDir["ImGui"] = "EGSS/vendor/imgui"
IncludeDir["stb_image"] = "EGSS/vendor/stb_image"

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
        "%{prj.name}/vendor/stb_image/**.cpp"
    }

    includedirs
    {
        "%{prj.name}/src",
        "%{prj.name}/vendor/spdlog/include",
        "%{IncludeDir.GLFW}",
        "%{IncludeDir.Glad}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.stb_image}",
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

        removefiles { "%{prj.name}/src/Platform/Linux/**" }

        defines
        {
            "EGSS_PLATFORM_WINDOWS",
            "EGSS_BUILD_DLL"
        }

        links
        {
            "opengl32.lib"
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
        defines { "EGSS_DEBUG", "EGSS_ENABLE_ASSERTS" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines "EGSS_RELEASE"
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "EGSS_DIST"
        runtime "Release"
        optimize "Full"
        symbols "Off"

            

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
        "EGSS"
    }

    filter "system:windows"
        cppdialect "C++17"
        staticruntime "On"
        systemversion "latest"

        defines
        {
            "EGSS_PLATFORM_WINDOWS",
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

    filter "configurations:Debug"
        defines { "EGSS_DEBUG", "EGSS_ENABLE_ASSERTS" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        defines "EGSS_RELEASE"
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        defines "EGSS_DIST"
        runtime "Release"
        optimize "Full"
        symbols "Off"