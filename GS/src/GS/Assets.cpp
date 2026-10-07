#include "gspch.h"
#include "GS/Assets.h"

#include <filesystem>

namespace GS::Assets {

	namespace {
		std::string s_Root;
	}

	void SetRoot(const std::string& root)
	{
		s_Root = root;
		// One spelling, so "dir" and "dir/" resolve identically.
		while (s_Root.size() > 1 && (s_Root.back() == '/' || s_Root.back() == '\\'))
			s_Root.pop_back();
	}

	const std::string& GetRoot()
	{
		return s_Root;
	}

	std::string Resolve(const std::string& path)
	{
		if (path.empty() || s_Root.empty() || path.rfind("primitive:", 0) == 0)
			return path;
		std::filesystem::path p(path);
		if (p.is_absolute())
			return path;
		std::error_code ec;
		std::string rooted = s_Root + "/" + path;
		if (std::filesystem::exists(rooted, ec))
			return rooted;
		return path;
	}

}
