-- libvterm ships no premake file (just a Makefile), so GS supplies one, the
-- same way it does for ImGui (see imgui_premake5.lua). Lives outside the
-- submodule so it survives a re-clone and never leaves the submodule dirty.
-- Compiles only the core VT100/xterm state-machine sources -- none of
-- libvterm's own bin/ frontend tools, which this engine doesn't use.
project "libvterm"
	kind "StaticLib"
	language "C"
	cdialect "C99"
	staticruntime "off"
	warnings "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"libvterm/include/vterm.h",
		"libvterm/include/vterm_keycodes.h",
		"libvterm/src/vterm_internal.h",
		"libvterm/src/encoding.c",
		"libvterm/src/keyboard.c",
		"libvterm/src/mouse.c",
		"libvterm/src/parser.c",
		"libvterm/src/pen.c",
		"libvterm/src/screen.c",
		"libvterm/src/state.c",
		"libvterm/src/unicode.c",
		"libvterm/src/vterm.c",
	}

	-- "src" is needed too, not just "include": encoding.c and unicode.c
	-- #include their own sibling headers/tables ("utf8.h", "encoding/*.inc",
	-- "fullwidth.inc") with quoted, relative-to-src includes.
	includedirs
	{
		"libvterm/include",
		"libvterm/src",
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
