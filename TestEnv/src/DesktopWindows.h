#pragma once

// Where the open windows are, for a wallpaper that reacts to them.
//
// On a Wayland session the X11 client list is useless -- it holds XWayland
// clients and nothing else, which on a normal KDE desk is two windows out of
// twenty. The only component that can see them all is the compositor, so the
// chain is: a KWin script reports geometry over D-Bus, `tools/egss-windows.py`
// owns the name and writes what arrives to a file, and this reads that file.
//
// Nothing here starts any of that. If the bridge is not running the file does
// not exist, this reports no windows, and the wallpaper behaves as it did
// before -- which is the right failure for a decoration.
//
// **The file is in KWin's coordinates, which are not the wallpaper's.** KWin
// works in logical pixels: a 3840x2160 monitor at scale 1.5 is 2560x1440 to a
// script, while the wallpaper is an XWayland client living in physical pixels.
// Each line carries the window's rectangle *and* its output's, so the scale is
// the ratio between that output's logical size and the physical size XRandR
// reported for the monitor of the same name -- 1.5 here, derived rather than
// assumed, because a desk can mix scales.

#include <Egss.h>

#include <cstdio>
#include <sys/stat.h>

class DesktopWindows
{
public:
	struct Rect
	{
		float X = 0.0f, Y = 0.0f, Width = 0.0f, Height = 0.0f;
	};

	// True when the set of windows changed, so a caller can rebuild whatever it
	// derives from them instead of doing it every step.
	bool Poll(const std::vector<Egss::MonitorInfo>& monitors)
	{
		const char* runtime = std::getenv("XDG_RUNTIME_DIR");
		std::string path = std::string(runtime ? runtime : "/tmp") + "/egss-windows";

		struct stat info;

		if (stat(path.c_str(), &info) != 0)
		{
			// The bridge is not running, or has just stopped. Either way the
			// desk is unobstructed as far as this is concerned.
			if (m_Rects.empty())
				return false;

			m_Rects.clear();
			m_Stamp = 0;
			return true;
		}

		// mtime and size together: a rename swaps the inode, and a same-second
		// rewrite of a different list is common while a window is being
		// dragged.
		long long stamp = (long long)info.st_mtime * 1000000
			+ (long long)info.st_size;

		if (stamp == m_Stamp)
			return false;

		m_Stamp = stamp;

		std::FILE* file = std::fopen(path.c_str(), "rb");
		if (!file)
			return false;

		std::string payload;
		char buffer[4096];
		size_t read = 0;

		while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
			payload.append(buffer, read);

		std::fclose(file);

		return Parse(payload, monitors);
	}

	const std::vector<Rect>& Rects() const { return m_Rects; }

private:
	bool Parse(const std::string& payload, const std::vector<Egss::MonitorInfo>& monitors)
	{
		std::vector<Rect> parsed;

		size_t start = 0;

		while (start < payload.size())
		{
			size_t end = payload.find(';', start);
			if (end == std::string::npos)
				end = payload.size();

			std::string line = payload.substr(start, end - start);
			start = end + 1;

			if (line.empty())
				continue;

			// x,y,w,h,outX,outY,outW,outH,name
			//
			// **Split rather than scanned.** The first version used one
			// `sscanf` with a `%*[^,]` for a scale field, and KWin sent that
			// field empty -- `%*[^,]` needs at least one character, so the
			// conversion stopped there, the output name came back empty, every
			// rectangle was dropped for having no matching monitor, and the
			// wallpaper reported no windows at all while the bridge was
			// happily receiving them.
			std::vector<std::string> fields;
			size_t at = 0;

			while (at <= line.size())
			{
				size_t comma = line.find(',', at);
				if (comma == std::string::npos)
					comma = line.size();

				fields.push_back(line.substr(at, comma - at));
				at = comma + 1;
			}

			if (fields.size() < 9)
				continue;

			float values[8] = {};
			for (int i = 0; i < 8; i++)
				values[i] = (float)std::atof(fields[i].c_str());

			const std::string& name = fields[8];

			const Egss::MonitorInfo* monitor = Find(monitors, name);

			if (!monitor || values[6] <= 0.0f || values[7] <= 0.0f)
				continue;

			float scaleX = (float)monitor->Width / values[6];
			float scaleY = (float)monitor->Height / values[7];

			Rect rect;
			rect.X = (float)monitor->X + (values[0] - values[4]) * scaleX;
			rect.Y = (float)monitor->Y + (values[1] - values[5]) * scaleY;
			rect.Width = values[2] * scaleX;
			rect.Height = values[3] * scaleY;

			parsed.push_back(rect);
		}

		bool changed = parsed.size() != m_Rects.size();

		for (size_t i = 0; !changed && i < parsed.size(); i++)
		{
			const Rect& a = parsed[i];
			const Rect& b = m_Rects[i];

			changed = a.X != b.X || a.Y != b.Y
				|| a.Width != b.Width || a.Height != b.Height;
		}

		m_Rects.swap(parsed);
		return changed;
	}

	static const Egss::MonitorInfo* Find(const std::vector<Egss::MonitorInfo>& monitors,
		const std::string& name)
	{
		for (const Egss::MonitorInfo& monitor : monitors)
			if (monitor.Name == name)
				return &monitor;

		return nullptr;
	}

	std::vector<Rect> m_Rects;
	long long m_Stamp = 0;
};
