#include "gspch.h"
#include "GS/Procedural/MaterialExport.h"

#include <stb_image_write.h>

#include <filesystem>
#include <fstream>

namespace GS {

	namespace {
		bool Replace(const std::string& temp, const std::string& path, std::string& error)
		{
			std::error_code ec;
			std::filesystem::rename(temp, path, ec);
			if (ec)
			{
				error = "could not replace '" + path + "': " + ec.message();
				std::filesystem::remove(temp, ec);
				return false;
			}
			return true;
		}

		bool WritePng(const std::string& path, const Image& image, std::string& error)
		{
			std::vector<uint8_t> pixels = ToRGBA8(image);
			std::string temp = path + ".tmp";
			// Set every time, never assumed: the flag is global to stb, and
			// ScreenCapture sets it and leaves it set. Flipped because the
			// engine loads PNGs flipped (OpenGLTexture's
			// stbi_set_flip_vertically_on_load), and row 0 here is v = 0 --
			// the two flips cancel, so a loaded texture matches the graph.
			stbi_flip_vertically_on_write(1);
			if (!stbi_write_png(temp.c_str(), image.Width, image.Height, 4, pixels.data(), image.Width * 4))
			{
				error = "could not write '" + path + "'";
				std::error_code ec;
				std::filesystem::remove(temp, ec);
				return false;
			}
			return Replace(temp, path, error);
		}
	}

	std::vector<uint8_t> ToRGBA8(const Image& image)
	{
		std::vector<uint8_t> out((size_t)image.Width * image.Height * 4);
		auto byte = [](float v) { return (uint8_t)std::lround(glm::clamp(v, 0.0f, 1.0f) * 255.0f); };
		for (size_t p = 0; p < (size_t)image.Width * image.Height; p++)
		{
			if (image.Channels == 1)
			{
				uint8_t v = byte(image.Data[p]);
				out[p * 4 + 0] = out[p * 4 + 1] = out[p * 4 + 2] = v;
				out[p * 4 + 3] = 255;
			}
			else
			{
				for (int c = 0; c < 4; c++)
					out[p * 4 + c] = byte(image.Data[p * 4 + c]);
			}
		}
		return out;
	}

	bool ExportMaterial(MaterialGraph& graph, const std::string& gsmatPath, std::string& error)
	{
		std::filesystem::path base(gsmatPath);
		std::string dir = base.parent_path().string();
		std::string name = base.stem().string();
		auto file = [&](const std::string& suffix) {
			return (dir.empty() ? name : dir + "/" + name) + suffix;
		};

		int divisor = graph.Resolution() / graph.EvalResolution();
		graph.SetPreviewDivisor(1);

		struct Map { MaterialGraph::Output Which; const char* Suffix; };
		const Map maps[] = {
			{ MaterialGraph::Output::Albedo, "_albedo.png" },
			{ MaterialGraph::Output::Height, "_height.png" },
			{ MaterialGraph::Output::Normal, "_normal.png" },
			{ MaterialGraph::Output::Roughness, "_roughness.png" },
		};
		bool ok = true;
		for (const Map& map : maps)
			if (!WritePng(file(map.Suffix), graph.EvaluateOutput(map.Which), error))
			{
				ok = false;
				break;
			}

		if (ok)
		{
			// Only the albedo has an .mtl keyword the engine reads. The other
			// three are written for normal mapping and roughness shading,
			// which the renderer does not do yet.
			std::string mtl = file(".mtl"), temp = mtl + ".tmp";
			{
				std::ofstream out(temp, std::ios::binary);
				out << "# Generated from " << base.filename().string() << " -- edit the graph, not this file.\n"
					<< "newmtl " << name << "\n"
					<< "Kd 1 1 1\n"
					<< "map_Kd " << name << "_albedo.png\n";
				if (!out)
				{
					error = "could not write '" + mtl + "'";
					ok = false;
				}
			}
			ok = ok && Replace(temp, mtl, error);
		}

		graph.SetPreviewDivisor(divisor);
		return ok;
	}

}
