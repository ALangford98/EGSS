// TEMPORARY -- delete after verifying UvTemplateWriter's output resolution
// and pixel placement against a known, hand-placed triangle.
#pragma once
#include <GS.h>
#include <GS/Renderer/UvTemplateWriter.h>
#include <stb_image.h>
#include <filesystem>

namespace UvTemplateWriterTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::MeshData data;
		// A triangle covering roughly the UV square's lower-left region:
		// (0.1,0.1) - (0.6,0.1) - (0.1,0.6). Its centroid is well inside;
		// the opposite corner (0.9,0.9) is well outside.
		data.Vertices = {
			{ {0,0,0}, {0,0,1}, {0.1f, 0.1f} },
			{ {1,0,0}, {0,0,1}, {0.6f, 0.1f} },
			{ {0,1,0}, {0,0,1}, {0.1f, 0.6f} },
		};
		data.Indices = { 0, 1, 2 };
		GS::UvUnwrap::ChartAssignment chartIds = { 0 };

		const std::string path = "uv_template_test_output.png";
		std::string error;
		bool wrote = GS::UvTemplateWriter::Write(path, 64, data, chartIds, error);
		Check(wrote, "Write reports success: " + error);

		// By the time this runs (end of TestEnv's constructor), every
		// other layer's textures have already loaded through
		// OpenGLTexture, which sets the process-global
		// stbi_set_flip_vertically_on_load(1) to match OpenGL's
		// bottom-up convention (OpenGLTexture.cpp). UvTemplateWriter
		// itself writes unflipped, self-consistent pixels -- this read
		// needs to explicitly ask for that back, or it would read the
		// image upside down and this test would be checking the wrong
		// pixel entirely (caught by a real nonzero-pixel-bounds scan
		// that came out y-shifted by exactly a vertical mirror, not
		// assumed).
		stbi_set_flip_vertically_on_load(0);
		int w = 0, h = 0, channels = 0;
		stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
		Check(pixels != nullptr, "written PNG can be read back");
		if (pixels)
		{
			Check(w == 64 && h == 64, "output resolution matches the requested 64x64");

			int cx = (int)((0.1f + 0.6f + 0.1f) / 3.0f * 64.0f);
			int cy = (int)((0.1f + 0.1f + 0.6f) / 3.0f * 64.0f);
			uint8_t insideAlpha = pixels[(cy * 64 + cx) * 4 + 3];
			Check(insideAlpha > 0, "a pixel inside the triangle is non-transparent");

			int ox = (int)(0.9f * 64.0f), oy = (int)(0.9f * 64.0f);
			uint8_t outsideAlpha = pixels[(oy * 64 + ox) * 4 + 3];
			Check(outsideAlpha == 0, "a pixel outside the triangle stays transparent");

			stbi_image_free(pixels);
		}

		std::filesystem::remove(path);
		GS_TRACE("UvTemplateWriterTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
