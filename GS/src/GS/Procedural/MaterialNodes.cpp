#include "gspch.h"
#include "GS/Procedural/MaterialGraph.h"
#include "GS/Procedural/Noise.h"

// The node catalogue and what each node computes. Kept apart from
// MaterialGraph.cpp, which only decides *when* a node runs and *what* it
// reads: adding a node type touches this file and nothing else.
//
// Every parameter is in UV units (a blur radius as a fraction of the image,
// a warp strength likewise), never pixels, so a quarter-resolution preview
// and a full export are the same material, one of them softer.

namespace GS {

	namespace {
		const float kRootHalf = 0.70710678f;   // sqrt(2)/2: 2D gradient noise's bound

		std::vector<NodeTypeInfo> BuildTable()
		{
			std::vector<NodeTypeInfo> t((size_t)NodeType::Count);
			auto P = [](const char* name, ParamKind kind, float mn, float mx, float def,
				std::vector<const char*> labels = {}) {
				return ParamInfo{ name, kind, mn, mx, def, std::move(labels) };
			};

			t[(int)NodeType::Noise] = { "Noise", "Generators", {}, { { "Out", PinType::Grey, 0.0f } },
				{ P("scale", ParamKind::Int, 1, 64, 4), P("octaves", ParamKind::Int, 1, 10, 5),
				  P("lacunarity", ParamKind::Int, 2, 4, 2), P("gain", ParamKind::Float, 0, 1, 0.5f),
				  P("seed", ParamKind::Int, 0, 9999, 0) } };
			t[(int)NodeType::Voronoi] = { "Voronoi", "Generators", {}, { { "Out", PinType::Grey, 0.0f } },
				{ P("scale", ParamKind::Int, 1, 64, 6), P("jitter", ParamKind::Float, 0, 1, 1),
				  P("mode", ParamKind::Enum, 0, 2, 0, { "F1", "F2 - F1", "Cell ID" }),
				  P("seed", ParamKind::Int, 0, 9999, 0) } };
			t[(int)NodeType::Pattern] = { "Pattern", "Generators", {},
				{ { "Mask", PinType::Grey, 0.0f }, { "Random", PinType::Grey, 0.0f } },
				{ P("rows", ParamKind::Int, 1, 64, 8), P("columns", ParamKind::Int, 1, 64, 4),
				  P("mortar", ParamKind::Float, 0, 0.5f, 0.05f), P("offset", ParamKind::Float, 0, 1, 0.5f),
				  P("seed", ParamKind::Int, 0, 9999, 0) } };
			t[(int)NodeType::Constant] = { "Constant", "Generators", {}, { { "Out", PinType::Grey, 0.0f } },
				{ P("value", ParamKind::Float, 0, 1, 0.5f) } };

			t[(int)NodeType::Levels] = { "Levels", "Filters", { { "In", PinType::Grey, 0.0f } },
				{ { "Out", PinType::Grey, 0.0f } },
				{ P("in min", ParamKind::Float, 0, 1, 0), P("in max", ParamKind::Float, 0, 1, 1),
				  P("gamma", ParamKind::Float, 0.1f, 10, 1), P("out min", ParamKind::Float, 0, 1, 0),
				  P("out max", ParamKind::Float, 0, 1, 1) } };
			t[(int)NodeType::Invert] = { "Invert", "Filters", { { "In", PinType::Grey, 0.0f } },
				{ { "Out", PinType::Grey, 0.0f } }, {} };
			t[(int)NodeType::Blur] = { "Blur", "Filters", { { "In", PinType::Grey, 0.0f } },
				{ { "Out", PinType::Grey, 0.0f } },
				{ P("radius", ParamKind::Float, 0, 0.1f, 0.01f),
				  P("mode", ParamKind::Enum, 0, 1, 1, { "Box", "Gaussian" }) } };
			// Offset defaults to 0.5, the value that displaces by nothing, so an
			// unconnected Warp is a pass-through rather than a shift.
			t[(int)NodeType::Warp] = { "Warp", "Filters",
				{ { "In", PinType::Grey, 0.0f }, { "Offset", PinType::Grey, 0.5f } },
				{ { "Out", PinType::Grey, 0.0f } },
				{ P("strength", ParamKind::Float, 0, 0.5f, 0.05f) } };
			t[(int)NodeType::Transform] = { "Transform", "Filters", { { "In", PinType::Any, 0.0f } },
				{ { "Out", PinType::Any, 0.0f } },
				{ P("tile", ParamKind::Int, 1, 16, 1), P("offset u", ParamKind::Float, 0, 1, 0),
				  P("offset v", ParamKind::Float, 0, 1, 0),
				  P("rotation", ParamKind::Enum, 0, 3, 0, { "0", "90", "180", "270" }) } };

			t[(int)NodeType::Blend] = { "Blend", "Combine",
				{ { "A", PinType::Any, 0.0f }, { "B", PinType::Any, 0.0f }, { "Mask", PinType::Grey, 1.0f } },
				{ { "Out", PinType::Any, 0.0f } },
				{ P("mode", ParamKind::Enum, 0, 7, 0,
					{ "Mix", "Add", "Subtract", "Multiply", "Overlay", "Min", "Max", "Difference" }),
				  P("opacity", ParamKind::Float, 0, 1, 1) } };

			t[(int)NodeType::GradientMap] = { "Gradient Map", "Colour", { { "In", PinType::Grey, 0.0f } },
				{ { "Out", PinType::Colour, 0.0f } }, {} };
			t[(int)NodeType::GradientMap].HasRamp = true;
			t[(int)NodeType::ColourConstant] = { "Colour", "Colour", {}, { { "Out", PinType::Colour, 0.0f } }, {} };
			t[(int)NodeType::ColourConstant].HasColour = true;

			t[(int)NodeType::HeightToNormal] = { "Height to Normal", "Derived", { { "Height", PinType::Grey, 0.5f } },
				{ { "Out", PinType::Colour, 0.0f } },
				{ P("strength", ParamKind::Float, 0, 10, 1) } };

			t[(int)NodeType::OutAlbedo] = { "Albedo Output", "Outputs", { { "Albedo", PinType::Colour, 0.5f } }, {}, {} };
			t[(int)NodeType::OutHeight] = { "Height Output", "Outputs", { { "Height", PinType::Grey, 0.5f } }, {}, {} };
			t[(int)NodeType::OutNormal] = { "Normal Output", "Outputs", { { "Normal", PinType::Colour, 0.5f } }, {}, {} };
			t[(int)NodeType::OutRoughness] = { "Roughness Output", "Outputs", { { "Roughness", PinType::Grey, 0.5f } }, {}, {} };
			for (NodeType o : { NodeType::OutAlbedo, NodeType::OutHeight, NodeType::OutNormal, NodeType::OutRoughness })
				t[(int)o].IsOutput = true;
			return t;
		}

		const std::vector<NodeTypeInfo>& Table()
		{
			static const std::vector<NodeTypeInfo> table = BuildTable();
			return table;
		}

		float ParamOf(const MaterialNode& node, const char* name)
		{
			return node.Params[FindParam(node.Type, name)];
		}

		Image Blank(int n, int channels)
		{
			Image image;
			image.Width = image.Height = n;
			image.Channels = channels;
			image.Data.assign((size_t)n * n * channels, 0.0f);
			return image;
		}

		// One pass of a separable 1D kernel along x (horizontal) or y, wrapping.
		Image Convolve1D(const Image& in, const std::vector<float>& kernel, bool horizontal)
		{
			Image out = Blank(in.Width, in.Channels);
			int half = (int)kernel.size() / 2;
			for (int y = 0; y < in.Height; y++)
				for (int x = 0; x < in.Width; x++)
					for (int c = 0; c < in.Channels; c++)
					{
						float sum = 0.0f;
						for (int k = -half; k <= half; k++)
							sum += kernel[k + half] * (horizontal ? in.Wrap(x + k, y, c) : in.Wrap(x, y + k, c));
						out.At(x, y, c) = sum;
					}
			return out;
		}

		float BlendChannel(int mode, float a, float b)
		{
			switch (mode)
			{
			case 0: return b;
			case 1: return a + b;
			case 2: return a - b;
			case 3: return a * b;
			case 4: return a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
			case 5: return std::min(a, b);
			case 6: return std::max(a, b);
			default: return std::abs(a - b);
			}
		}

		glm::vec4 SampleRamp(const std::vector<GradientStop>& ramp, float x)
		{
			if (ramp.empty())
				return glm::vec4(x, x, x, 1.0f);
			if (x <= ramp.front().Position)
				return ramp.front().Colour;
			for (size_t i = 1; i < ramp.size(); i++)
				if (x <= ramp[i].Position)
				{
					const GradientStop& a = ramp[i - 1];
					const GradientStop& b = ramp[i];
					float span = b.Position - a.Position;
					float t = span > 0.0f ? (x - a.Position) / span : 1.0f;
					return glm::mix(a.Colour, b.Colour, t);
				}
			return ramp.back().Colour;
		}
	}

	const NodeTypeInfo& GetNodeTypeInfo(NodeType type)
	{
		return Table()[(size_t)type];
	}

	bool NodeTypeFromName(const std::string& name, NodeType& out)
	{
		for (int i = 0; i < (int)NodeType::Count; i++)
			if (name == Table()[i].Name)
			{
				out = (NodeType)i;
				return true;
			}
		return false;
	}

	int FindParam(NodeType type, const char* name)
	{
		const std::vector<ParamInfo>& params = GetNodeTypeInfo(type).Params;
		for (size_t i = 0; i < params.size(); i++)
			if (std::strcmp(params[i].Name, name) == 0)
				return (int)i;
		return -1;
	}

	Image HeightToNormal(const Image& height, float strength)
	{
		const int n = height.Width;
		Image out = Blank(n, 4);
		for (int y = 0; y < height.Height; y++)
			for (int x = 0; x < n; x++)
			{
				auto h = [&](int dx, int dy) { return height.Wrap(x + dx, y + dy, 0); };
				// Sobel. Its weights sum to 4 across a 2-pixel difference, so
				// gx / 8 is the slope per pixel and gx / 8 * n the slope per UV
				// unit -- which keeps `strength` meaning the same at any
				// resolution.
				float gx = (h(1, -1) + 2.0f * h(1, 0) + h(1, 1)) - (h(-1, -1) + 2.0f * h(-1, 0) + h(-1, 1));
				float gy = (h(-1, 1) + 2.0f * h(0, 1) + h(1, 1)) - (h(-1, -1) + 2.0f * h(0, -1) + h(1, -1));
				float du = gx / 8.0f * (float)n;
				float dv = gy / 8.0f * (float)height.Height;
				glm::vec3 normal = glm::normalize(glm::vec3(-strength * du, -strength * dv, 1.0f));
				out.At(x, y, 0) = normal.x * 0.5f + 0.5f;
				out.At(x, y, 1) = normal.y * 0.5f + 0.5f;
				out.At(x, y, 2) = normal.z * 0.5f + 0.5f;
				out.At(x, y, 3) = 1.0f;
			}
		return out;
	}

	namespace Detail {

		// `inputs` holds one image per declared input -- the connected
		// source, or a default already filled in by the evaluator -- so a
		// node body never asks what is connected. `outputs` is sized to the
		// node's output count; each is written at resolution `n`.
		void EvaluateNodeBody(const MaterialNode& node, const std::vector<const Image*>& inputs,
			int n, int outChannels, std::vector<Image>& outputs)
		{
			auto uv = [n](int x, int y) { return glm::vec2((x + 0.5f) / n, (y + 0.5f) / n); };

			switch (node.Type)
			{
			case NodeType::Noise:
			{
				int scale = (int)ParamOf(node, "scale");
				int octaves = (int)ParamOf(node, "octaves");
				int lacunarity = (int)ParamOf(node, "lacunarity");
				float gain = ParamOf(node, "gain");
				uint32_t seed = (uint32_t)ParamOf(node, "seed");
				float amplitude = 0.0f, a = 1.0f;
				for (int i = 0; i < octaves; i++) { amplitude += a; a *= gain; }
				// Divided by the largest the sum can possibly be, so the
				// result is provably in [0,1] without a clamp. The typical
				// spread is well inside that; Levels is how contrast is added.
				float norm = 0.5f / (kRootHalf * amplitude);
				Image& out = outputs[0] = Blank(n, 1);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
						out.At(x, y, 0) = 0.5f + norm * Noise::Fbm(uv(x, y) * (float)scale,
							glm::ivec2(scale), octaves, lacunarity, gain, seed);
				break;
			}
			case NodeType::Voronoi:
			{
				int scale = (int)ParamOf(node, "scale");
				float jitter = ParamOf(node, "jitter");
				int mode = (int)ParamOf(node, "mode");
				uint32_t seed = (uint32_t)ParamOf(node, "seed");
				Image& out = outputs[0] = Blank(n, 1);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
					{
						Noise::VoronoiResult r = Noise::Voronoi(uv(x, y) * (float)scale, glm::ivec2(scale), jitter, seed);
						float v = mode == 0 ? r.F1 * kRootHalf
							: mode == 1 ? (r.F2 - r.F1) * kRootHalf
							: Noise::HashToUnit(r.CellId);
						out.At(x, y, 0) = std::min(v, 1.0f);
					}
				break;
			}
			case NodeType::Pattern:
			{
				int rows = (int)ParamOf(node, "rows");
				int columns = (int)ParamOf(node, "columns");
				float mortar = ParamOf(node, "mortar");
				float offset = ParamOf(node, "offset");
				uint32_t seed = (uint32_t)ParamOf(node, "seed");
				// Mortar is a fraction of a brick's *height*, applied as the
				// same absolute thickness both ways -- so in a brick's own
				// column units it is mortar * columns / rows.
				float halfV = mortar * 0.5f;
				float halfU = mortar * (float)columns / (float)rows * 0.5f;
				Image& mask = outputs[0] = Blank(n, 1);
				Image& random = outputs[1] = Blank(n, 1);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
					{
						glm::vec2 p = uv(x, y);
						float rv = p.y * rows;
						int row = (int)std::floor(rv);
						float fv = rv - row;
						float ru = p.x * columns + ((row & 1) ? offset : 0.0f);
						int column = (int)std::floor(ru);
						float fu = ru - column;
						bool isMortar = fv < halfV || fv > 1.0f - halfV || fu < halfU || fu > 1.0f - halfU;
						mask.At(x, y, 0) = isMortar ? 0.0f : 1.0f;
						int wrapped = ((column % columns) + columns) % columns;
						random.At(x, y, 0) = Noise::HashToUnit(Noise::Hash(wrapped, row, seed));
					}
				break;
			}
			case NodeType::Constant:
			{
				float value = ParamOf(node, "value");
				outputs[0] = Image::Filled(n, n, 1, &value);
				break;
			}
			case NodeType::ColourConstant:
			{
				outputs[0] = Image::Filled(n, n, 4, &node.Colour.x);
				break;
			}
			case NodeType::Levels:
			{
				float inMin = ParamOf(node, "in min"), inMax = ParamOf(node, "in max");
				float gamma = ParamOf(node, "gamma");
				float outMin = ParamOf(node, "out min"), outMax = ParamOf(node, "out max");
				float span = std::max(inMax - inMin, 1e-6f);
				Image out = *inputs[0];
				for (float& v : out.Data)
				{
					float t = glm::clamp((v - inMin) / span, 0.0f, 1.0f);
					v = outMin + (outMax - outMin) * std::pow(t, 1.0f / gamma);
				}
				outputs[0] = std::move(out);
				break;
			}
			case NodeType::Invert:
			{
				Image out = *inputs[0];
				for (float& v : out.Data)
					v = 1.0f - v;
				outputs[0] = std::move(out);
				break;
			}
			case NodeType::Blur:
			{
				float radius = ParamOf(node, "radius") * (float)n;
				bool gaussian = (int)ParamOf(node, "mode") == 1;
				if (radius < 0.5f)
				{
					outputs[0] = *inputs[0];
					break;
				}
				std::vector<float> kernel;
				if (gaussian)
				{
					// Radius taken as 2 sigma: most of the visible spread,
					// with the kernel itself running to 3 sigma.
					float sigma = radius * 0.5f;
					int half = (int)std::ceil(sigma * 3.0f);
					float sum = 0.0f;
					for (int k = -half; k <= half; k++)
					{
						kernel.push_back(std::exp(-(float)(k * k) / (2.0f * sigma * sigma)));
						sum += kernel.back();
					}
					for (float& w : kernel)
						w /= sum;
				}
				else
				{
					int half = (int)std::lround(radius);
					kernel.assign(2 * half + 1, 1.0f / (float)(2 * half + 1));
				}
				outputs[0] = Convolve1D(Convolve1D(*inputs[0], kernel, true), kernel, false);
				break;
			}
			case NodeType::Warp:
			{
				float strength = ParamOf(node, "strength");
				const Image& in = *inputs[0];
				const Image& offset = *inputs[1];
				Image& out = outputs[0] = Blank(n, 1);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
					{
						glm::vec2 p = uv(x, y);
						float d = (offset.At(x, y, 0) - 0.5f) * strength;
						out.At(x, y, 0) = in.Bilinear(p.x + d, p.y + d, 0);
					}
				break;
			}
			case NodeType::Transform:
			{
				int tile = (int)ParamOf(node, "tile");
				glm::vec2 offset(ParamOf(node, "offset u"), ParamOf(node, "offset v"));
				int rotation = (int)ParamOf(node, "rotation");
				const Image& in = *inputs[0];
				Image& out = outputs[0] = Blank(n, in.Channels);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
					{
						// Rotation in quarter turns about the centre maps pixel
						// centres onto pixel centres, so it resamples nothing.
						glm::vec2 q = uv(x, y) - 0.5f;
						for (int r = 0; r < rotation; r++)
							q = glm::vec2(-q.y, q.x);
						glm::vec2 s = (q + 0.5f) * (float)tile + offset;
						for (int c = 0; c < in.Channels; c++)
							out.At(x, y, c) = in.Bilinear(s.x, s.y, c);
					}
				break;
			}
			case NodeType::Blend:
			{
				int mode = (int)ParamOf(node, "mode");
				float opacity = ParamOf(node, "opacity");
				const Image& a = *inputs[0];
				const Image& b = *inputs[1];
				const Image& mask = *inputs[2];
				Image& out = outputs[0] = Blank(n, outChannels);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
					{
						float weight = mask.At(x, y, 0) * opacity;
						for (int c = 0; c < outChannels; c++)
						{
							float av = a.At(x, y, c);
							float f = glm::clamp(BlendChannel(mode, av, b.At(x, y, c)), 0.0f, 1.0f);
							out.At(x, y, c) = av + (f - av) * weight;
						}
					}
				break;
			}
			case NodeType::GradientMap:
			{
				const Image& in = *inputs[0];
				Image& out = outputs[0] = Blank(n, 4);
				for (int y = 0; y < n; y++)
					for (int x = 0; x < n; x++)
					{
						glm::vec4 colour = SampleRamp(node.Ramp, in.At(x, y, 0));
						for (int c = 0; c < 4; c++)
							out.At(x, y, c) = colour[c];
					}
				break;
			}
			case NodeType::HeightToNormal:
			{
				outputs[0] = HeightToNormal(*inputs[0], ParamOf(node, "strength"));
				break;
			}
			default:
				// Output nodes have no outputs; the graph reads their input.
				break;
			}
		}

	}

}
