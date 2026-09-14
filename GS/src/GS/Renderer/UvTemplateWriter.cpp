#include "gspch.h"
#include "GS/Renderer/UvTemplateWriter.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace GS {

	namespace {

		// A small, fixed pastel palette -- cycled by chart id, not
		// computed from the mesh, so two charts are always visually
		// distinguishable regardless of how many there are.
		constexpr int kPaletteSize = 12;
		const uint8_t kPalette[kPaletteSize][3] = {
			{255,214,214}, {214,255,214}, {214,214,255}, {255,255,214},
			{255,214,255}, {214,255,255}, {255,234,214}, {214,255,234},
			{234,214,255}, {255,214,234}, {234,255,214}, {214,234,255},
		};

		void SetPixel(std::vector<uint8_t>& buffer, int resolution, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
		{
			if (x < 0 || y < 0 || x >= resolution || y >= resolution)
				return;
			size_t i = (size_t)(y * resolution + x) * 4;
			buffer[i + 0] = r; buffer[i + 1] = g; buffer[i + 2] = b; buffer[i + 3] = a;
		}

		// Bresenham -- a template wireframe needs a visible line, not an
		// anti-aliased one.
		void DrawLine(std::vector<uint8_t>& buffer, int resolution, glm::ivec2 p0, glm::ivec2 p1)
		{
			int dx = std::abs(p1.x - p0.x), sx = p0.x < p1.x ? 1 : -1;
			int dy = -std::abs(p1.y - p0.y), sy = p0.y < p1.y ? 1 : -1;
			int err = dx + dy;
			glm::ivec2 p = p0;
			while (true)
			{
				SetPixel(buffer, resolution, p.x, p.y, 60, 60, 60, 255);
				if (p == p1)
					break;
				int e2 = 2 * err;
				if (e2 >= dy) { err += dy; p.x += sx; }
				if (e2 <= dx) { err += dx; p.y += sy; }
			}
		}

		void FillTriangle(std::vector<uint8_t>& buffer, int resolution, glm::vec2 a, glm::vec2 b, glm::vec2 c, const uint8_t color[3])
		{
			int minX = std::max(0, (int)std::floor(std::min({ a.x, b.x, c.x })));
			int maxX = std::min(resolution - 1, (int)std::ceil(std::max({ a.x, b.x, c.x })));
			int minY = std::max(0, (int)std::floor(std::min({ a.y, b.y, c.y })));
			int maxY = std::min(resolution - 1, (int)std::ceil(std::max({ a.y, b.y, c.y })));

			float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
			if (std::abs(area) < 1e-6f)
				return;   // degenerate triangle in UV space -- nothing to fill

			for (int y = minY; y <= maxY; y++)
			{
				for (int x = minX; x <= maxX; x++)
				{
					glm::vec2 p((float)x + 0.5f, (float)y + 0.5f);
					float w0 = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / area;
					float w1 = ((c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x)) / area;
					float w2 = 1.0f - w0 - w1;
					if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
						SetPixel(buffer, resolution, x, y, color[0], color[1], color[2], 160);
				}
			}
		}

	}

	bool UvTemplateWriter::Write(const std::string& path, int resolution, const MeshData& data,
		const UvUnwrap::ChartAssignment& chartIdPerTriangle, std::string& error)
	{
		if (resolution <= 0)
		{
			error = "resolution must be positive";
			return false;
		}

		std::vector<uint8_t> buffer((size_t)resolution * resolution * 4, 0);

		size_t triCount = data.TriangleCount();
		for (size_t t = 0; t < triCount; t++)
		{
			glm::vec2 uv0 = data.Vertices[data.Indices[t * 3 + 0]].TexCoord * (float)resolution;
			glm::vec2 uv1 = data.Vertices[data.Indices[t * 3 + 1]].TexCoord * (float)resolution;
			glm::vec2 uv2 = data.Vertices[data.Indices[t * 3 + 2]].TexCoord * (float)resolution;

			int chartId = (t < chartIdPerTriangle.size()) ? chartIdPerTriangle[t] : 0;
			const uint8_t* color = kPalette[((chartId % kPaletteSize) + kPaletteSize) % kPaletteSize];
			FillTriangle(buffer, resolution, uv0, uv1, uv2, color);
		}
		for (size_t t = 0; t < triCount; t++)
		{
			glm::ivec2 uv0 = glm::ivec2(data.Vertices[data.Indices[t * 3 + 0]].TexCoord * (float)resolution);
			glm::ivec2 uv1 = glm::ivec2(data.Vertices[data.Indices[t * 3 + 1]].TexCoord * (float)resolution);
			glm::ivec2 uv2 = glm::ivec2(data.Vertices[data.Indices[t * 3 + 2]].TexCoord * (float)resolution);
			DrawLine(buffer, resolution, uv0, uv1);
			DrawLine(buffer, resolution, uv1, uv2);
			DrawLine(buffer, resolution, uv2, uv0);
		}

		std::error_code fsError;
		std::filesystem::path target(path);
		if (target.has_parent_path())
			std::filesystem::create_directories(target.parent_path(), fsError);

		int written = stbi_write_png(path.c_str(), resolution, resolution, 4, buffer.data(), resolution * 4);
		if (written == 0)
		{
			error = "stbi_write_png failed to write '" + path + "'";
			return false;
		}
		return true;
	}

}
