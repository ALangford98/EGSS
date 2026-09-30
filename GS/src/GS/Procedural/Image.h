#pragma once

#include <cmath>
#include <vector>

namespace GS {

	// A float image, 1 channel (Grey) or 4 (RGBA), the material graph's only
	// data type. Floats rather than bytes so a chain of blends and levels
	// does not round to 1/255 at every step; quantised once, at export or
	// upload. Row 0 is v = 0 -- the bottom of a texture as OpenGL samples it.
	struct Image
	{
		int Width = 0, Height = 0, Channels = 1;
		std::vector<float> Data;

		static Image Filled(int width, int height, int channels, const float* value)
		{
			Image image;
			image.Width = width;
			image.Height = height;
			image.Channels = channels;
			image.Data.resize((size_t)width * height * channels);
			for (size_t i = 0; i < image.Data.size(); i++)
				image.Data[i] = value[i % channels];
			return image;
		}

		float& At(int x, int y, int c) { return Data[((size_t)y * Width + x) * Channels + c]; }
		float At(int x, int y, int c) const { return Data[((size_t)y * Width + x) * Channels + c]; }

		// Every neighbour read in the graph goes through here or Bilinear, so
		// every filter wraps -- the tiling invariant, enforced in one place.
		float Wrap(int x, int y, int c) const
		{
			x %= Width;  if (x < 0) x += Width;
			y %= Height; if (y < 0) y += Height;
			return At(x, y, c);
		}

		// UV in [0,1) maps pixel centres to (x + 0.5) / Width, so sampling at
		// a pixel's own centre returns that pixel exactly -- which is what
		// makes a whole-pixel offset an exact roll rather than a blur.
		float Bilinear(float u, float v, int c) const
		{
			float fx = u * Width - 0.5f, fy = v * Height - 0.5f;
			float x0f = std::floor(fx), y0f = std::floor(fy);
			float tx = fx - x0f, ty = fy - y0f;
			int x0 = (int)x0f, y0 = (int)y0f;
			float a = Wrap(x0, y0, c), b = Wrap(x0 + 1, y0, c);
			float d = Wrap(x0, y0 + 1, c), e = Wrap(x0 + 1, y0 + 1, c);
			return (a + (b - a) * tx) + ((d + (e - d) * tx) - (a + (b - a) * tx)) * ty;
		}
	};

}
