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

include "EGSS/vendor/glfw"

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
        "%{prj.name}/src/**.cpp"
    }

    includedirs
    {
        "%{prj.name}/src",
        "%{prj.name}/vendor/spdlog/include",
        "%{IncludeDir.GLFW}",
    }

    links
    {
        "GLFW"
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
            defines "EGSS_DEBUG"
            symbols "On"

            filter "configurations:Release"
            defines "EGSS_RELEASE"
            symbols "On"

            filter "configurations:Dist"
            defines "EGSS_DIST"
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
        "EGSS/src"
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
            defines "EGSS_DEBUG"
            symbols "On"

            filter "configurations:Release"
            defines "EGSS_RELEASE"
            symbols "On"

            filter "configurations:Dist"
            defines "EGSS_DIST"
            symbols "On"