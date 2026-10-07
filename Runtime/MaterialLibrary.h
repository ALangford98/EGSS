#pragma once
// An albedo, normal and roughness texture per .gsmat, shared by every entity whose
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
		// Tangent-space, +Y along +v -- read by the editor shader with a
		// frame rebuilt from screen-space derivatives (no vertex tangents).
		std::shared_ptr<GS::Texture2D> Normal;
		// Grey, read as the Blinn-Phong exponent and specular strength.
		std::shared_ptr<GS::Texture2D> Roughness;
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
		std::filesystem::path canonical = std::filesystem::weakly_canonical(GS::Assets::Resolve(path), ec);
		return ec ? path : canonical.string();
	}

	inline void Upload(std::shared_ptr<GS::Texture2D>& texture, const GS::Image& image)
	{
		if (!texture || (int)texture->GetWidth() != image.Width)
		{
			// A new texture on a size change: SetData asserts the size
			// matches, and a resolution change is an ordinary edit.
			texture.reset(GS::Texture2D::Create(image.Width, image.Height));
			texture->SetSmooth(true);
		}
		std::vector<uint8_t> pixels = GS::ToRGBA8(image);
		texture->SetData(pixels.data(), (unsigned int)pixels.size());
	}

	inline void UploadAll(Entry& entry, GS::MaterialGraph& graph)
	{
		Upload(entry.Albedo, graph.EvaluateOutput(GS::MaterialGraph::Output::Albedo));
		Upload(entry.Normal, graph.EvaluateOutput(GS::MaterialGraph::Output::Normal));
		Upload(entry.Roughness, graph.EvaluateOutput(GS::MaterialGraph::Output::Roughness));
	}

	inline void Fail(Entry& entry, const std::string& why)
	{
		entry.Albedo.reset();
		entry.Normal.reset();
		entry.Roughness.reset();
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
		UploadAll(entry, graph);
		entry.Warned = false;
	}

	// The entry for `path`, loaded on first use and re-checked against its
	// file at most once per s_RecheckSeconds.
	inline Entry& Resolve(const std::string& path)
	{
		std::string key = Key(path);
		auto found = s_Entries.find(key);
		auto now = std::chrono::steady_clock::now();
		if (found == s_Entries.end())
		{
			Entry& entry = s_Entries[key];
			entry.Checked = now;
			LoadFromDisk(key, entry);
			return entry;
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
		return entry;
	}

	// Null when the material cannot be had; the caller draws flat colour.
	inline std::shared_ptr<GS::Texture2D> Albedo(const std::string& path) { return Resolve(path).Albedo; }

	// Null exactly when Albedo is. A graph with no Normal and no Height
	// output still has one -- flat -- so a linked mesh always has both.
	inline std::shared_ptr<GS::Texture2D> Normal(const std::string& path) { return Resolve(path).Normal; }

	// Likewise; an absent Roughness output is a flat 0.5.
	inline std::shared_ptr<GS::Texture2D> Roughness(const std::string& path) { return Resolve(path).Roughness; }

	// The Material panel calls this after every change it evaluates -- at
	// the drag-preview resolution while a slider moves, so linked meshes
	// follow it at the panel's own cost.
	inline void NotifyEdited(const std::string& path, GS::MaterialGraph& graph)
	{
		Entry& entry = s_Entries[Key(path)];
		entry.Live = true;
		entry.Warned = false;
		UploadAll(entry, graph);
	}

}
