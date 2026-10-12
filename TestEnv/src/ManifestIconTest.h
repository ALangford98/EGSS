// TEMPORARY -- delete after verifying WriteProjectManifest keeps the icon.
#pragma once
#include <GS.h>
#include <cstdlib>
#include "EditorProject.h"
namespace ManifestIconTest {
	inline void Run()
	{
		const char* dir = std::getenv("GS_ICON_TEST");
		if (!dir)
			return;
		ProjectManifest m;
		bool read = ReadProjectManifest(dir, m);
		std::string before = m.Icon;
		m.Icon.clear();   // as the editor's own callers build it
		bool wrote = WriteProjectManifest(dir, m);
		ProjectManifest again;
		ReadProjectManifest(dir, again);
		GS_TRACE("ManifestIconTest: read={0} before='{1}' wrote={2} after='{3}' {4}", read, before, wrote, again.Icon,
			again.Icon == before && !before.empty() ? "PASS" : "FAIL");
		std::exit(0);
	}
}
