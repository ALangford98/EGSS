#pragma once
// One albedo texture per .gsmat, shared by every entity whose
// MeshComponent::MaterialPath names it -- MeshCache's arrangement, for
// materials. A graph is evaluated once per file, not once per entity.
//
// Two ways an entry is filled: from disk the first time a path is asked
// for, and live from the Material panel (NotifyEdited) while that material
// is being edited, which is what makes linked entities follow a slider.
// A live entry is the panel's; a disk entry re-checks its file once a
// second, so a material saved or deleted outside the panel is noticed.
#include <GS.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace MaterialLibrary {

	struct Entry
	{
		std::shared_ptr<GS::Texture2D> Albedo;
		bool Live = false;       // fed by the panel; the file is not consulted
		bool Warned = false;     // one warning per failure, not one per frame
		std::filesystem::file_time_type Stamp{};
		std::chrono::steady_clock::time_point Checked{};
	};

	inline std::unordered_map<std::string, Entry> s_Entries;
	inline int s_FailureWarnings = 0;
	inline float s_RecheckSeconds = 1.0f;

	inline int FailureWarnings() { return s_FailureWarnings; }

	// "a/./b.gsmat", "a/b.gsmat" and an absolute spelling are one material:
	// the scene stores whatever path was picked, the panel whatever the file
	// tree handed it.
	inline std::string Key(const std::string& path)
	{
		std::error_code ec;
		std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
		return ec ? path : canonical.string();
	}

	inline void Upload(Entry& entry, const GS::Image& image)
	{
		if (!entry.Albedo || (int)entry.Albedo->GetWidth() != image.Width)
		{
			// A new texture on a size change: SetData asserts the size
			// matches, and a resolution change is an ordinary edit.
			entry.Albedo.reset(GS::Texture2D::Create(image.Width, image.Height));
			entry.Albedo->SetSmooth(true);
		}
		std::vector<uint8_t> pixels = GS::ToRGBA8(image);
		entry.Albedo->SetData(pixels.data(), (unsigned int)pixels.size());
	}

	inline void Fail(Entry& entry, const std::string& why)
	{
		entry.Albedo.reset();
		if (!entry.Warned)
		{
			GS_WARN("Material '{0}' unavailable ({1}); linked meshes use their flat colour", why.substr(0, why.find(':')), why);
			entry.Warned = true;
			s_FailureWarnings++;
		}
	}

	inline void LoadFromDisk(const std::string& key, Entry& entry)
	{
		std::error_code ec;
		entry.Stamp = std::filesystem::last_write_time(key, ec);
		if (ec)
		{
			Fail(entry, key + ": no such file");
			return;
		}
		GS::MaterialGraph graph;
		std::string error;
		if (!GS::LoadMaterialGraph(key, graph, error))
		{
			Fail(entry, error);
			return;
		}
		Upload(entry, graph.EvaluateOutput(GS::MaterialGraph::Output::Albedo));
		entry.Warned = false;
	}

	// Null when the material cannot be had; the caller draws flat colour.
	inline std::shared_ptr<GS::Texture2D> Albedo(const std::string& path)
	{
		std::string key = Key(path);
		auto found = s_Entries.find(key);
		auto now = std::chrono::steady_clock::now();
		if (found == s_Entries.end())
		{
			Entry& entry = s_Entries[key];
			entry.Checked = now;
			LoadFromDisk(key, entry);
			return entry.Albedo;
		}

		Entry& entry = found->second;
		if (!entry.Live && std::chrono::duration<float>(now - entry.Checked).count() >= s_RecheckSeconds)
		{
			entry.Checked = now;
			std::error_code ec;
			auto stamp = std::filesystem::last_write_time(key, ec);
			if (ec)
				Fail(entry, key + ": no such file");
			else if (stamp != entry.Stamp || !entry.Albedo)
				LoadFromDisk(key, entry);
		}
		return entry.Albedo;
	}

	// The Material panel calls this after every change it evaluates -- at
	// the drag-preview resolution while a slider moves, so linked meshes
	// follow it at the panel's own cost.
	inline void NotifyEdited(const std::string& path, GS::MaterialGraph& graph)
	{
		Entry& entry = s_Entries[Key(path)];
		entry.Live = true;
		entry.Warned = false;
		Upload(entry, graph.EvaluateOutput(GS::MaterialGraph::Output::Albedo));
	}

}
