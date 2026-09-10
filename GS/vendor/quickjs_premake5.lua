-- QuickJS ships CMake/Makefile build files, not premake -- same situation
-- ImGui and libvterm were in. Lives outside the submodule so it survives a
-- re-clone. Deliberately excludes quickjs-libc.c: that file gives scripts
-- direct file/process/exec/signal access, which a sandboxed script host
-- has no business granting by default (see this plan's own Global
-- Constraints -- scripts get console.log only in v1).
project "quickjs"
	kind "StaticLib"
	language "C"
	cdialect "gnu11"
	staticruntime "off"
	warnings "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"quickjs/quickjs.h",
		"quickjs/quickjs.c",
		"quickjs/quickjs-atom.h",
		"quickjs/quickjs-opcode.h",
		"quickjs/quickjs-c-atomics.h",
		"quickjs/libregexp.c",
		"quickjs/libregexp.h",
		"quickjs/libregexp-opcode.h",
		"quickjs/libunicode.c",
		"quickjs/libunicode.h",
		"quickjs/libunicode-table.h",
		"quickjs/dtoa.c",
		"quickjs/dtoa.h",
		"quickjs/cutils.h",
		"quickjs/list.h",
	}

	includedirs
	{
		"quickjs",
	}

	defines
	{
		"_GNU_SOURCE",
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
