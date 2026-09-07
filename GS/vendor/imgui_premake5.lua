-- ImGui ships no premake file, so EGSS supplies one. It deliberately lives
-- outside the submodule: a file added inside it would be lost on re-clone and
-- would leave the submodule permanently dirty. Every path below is therefore
-- relative to EGSS/vendor/.
project "ImGui"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	warnings "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"imgui/imconfig.h",
		"imgui/imgui.h",
		"imgui/imgui.cpp",
		"imgui/imgui_draw.cpp",
		"imgui/imgui_internal.h",
		"imgui/imgui_tables.cpp",
		"imgui/imgui_widgets.cpp",
		"imgui/imstb_rectpack.h",
		"imgui/imstb_textedit.h",
		"imgui/imstb_truetype.h",
		"imgui/imgui_demo.cpp",

		"imgui/backends/imgui_impl_glfw.h",
		"imgui/backends/imgui_impl_glfw.cpp",
		"imgui/backends/imgui_impl_opengl3.h",
		"imgui/backends/imgui_impl_opengl3.cpp"
	}

	includedirs
	{
		"imgui",
		"glfw/include"
	}

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "system:windows"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"
