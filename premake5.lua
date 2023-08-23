workspace "EGSS"
    architecture "x64"

    configurations
    {
        "Debug",
        "Release",
        "Dist"
    }

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "EGSS"
    location "EGSS"
    kind "SharedLib"
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
        "%{prj.name}/vendor/spdlog/include"
    }

    filter "system:windows"
        cppdialect "C++17"
        staticruntime "On"
        systemversion "latest"

        defines
        {
            "EGSS_PLATFORM_WINDOWS",
            "EGSS_BUILD_DLL"
        }

        postbuildcommands
        {
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

        filter "configurations:Debug"
            defines "EGSS_DEBUG"
            symbols "On"

            filter "configurations:Release"
            defines "EGSS_RELEASE"
            symbols "On"

            filter "configurations:Dist"
            defines "EGSS_DIST"
            symbols "On"