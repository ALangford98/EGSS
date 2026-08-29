#pragma once

// **The wind, drawn.**
//
// Everything else the weather does is a force: it pushes the player, lies the
// grass over, bends the trees. All of that is legible only if you already know
// to look for it. This is the wind as something you can see on its own -- soft
// streaks in the air, drifting at exactly the speed the model says the air is
// moving, so the grass and the streaks over it are two views of one number.
//
// **Painterly rather than physical, and deliberately.** Air is transparent;
// there is nothing there to draw. What these stand in for is what a painter
// actually puts on a canvas to say "windy" -- a few long soft strokes lying
// along the flow. So the geometry is honest (they really do travel at the wind
// speed, along the wind vector, and vanish when it drops) and the appearance is
// a brush stroke: soft at both ends, soft across, and never quite opaque.
//
// **One static mesh, advected on the GPU, wrapped in a box.** The streaks never
// need updating from the CPU and never need sorting. Each one carries its home
// position in the box and its own length, width and phase; the vertex shader
// drifts the whole field by `wind * time` and wraps it back into the box, which
// is centred on the camera. A streak that leaves downwind reappears upwind, so
// the field is endless and costs one draw call of a few thousand triangles.

#include <Egss.h>

#include <glm/glm.hpp>

#include <cmath>

namespace WindStreaks {

	// A box `extent` metres on a side is filled with `count` streaks. The mesh
	// is built once and never changes -- the drift happens in the shader.
	//
	// `Position` is the streak's home in the box, the same for all four
	// corners of a quad. `TexCoord` is the corner, 0..1 along and across.
	// `Normal` is not a normal: it carries the streak's own length, width and
	// phase, because a vertex has three floats spare there and this mesh has
	// no lighting to do. That is worth saying out loud rather than leaving for
	// someone to discover from the shader.
	inline Egss::MeshData BuildMesh(int count, float extent, unsigned int seed = 7u)
	{
		Egss::MeshData mesh;

		if (count <= 0)
			return mesh;

		// A small deterministic hash, so the same seed gives the same weather
		// twice -- which the replay system requires of anything visible.
		auto random = [&seed]()
		{
			seed = seed * 1664525u + 1013904223u;

			return (float)((seed >> 8) & 0xFFFFFF) / (float)0x1000000;
		};

		mesh.Vertices.reserve((size_t)count * 4);
		mesh.Indices.reserve((size_t)count * 6);

		for (int i = 0; i < count; i++)
		{
			glm::vec3 home(
				(random() - 0.5f) * extent,
				(random() - 0.5f) * extent,
				(random() - 0.5f) * extent);

			// **Lengths spread over a wide range on purpose.** A field of
			// equal strokes reads as a texture; a few long ones among many
			// short ones reads as a gust. Cubing a uniform is the cheapest way
			// to get that: most come out short, a handful come out very long.
			float t = random();

			float length = 4.0f + 55.0f * t * t * t;

			// **Wide enough to be a stroke.** The first pass made these 0.1 to
			// 0.45 m, which at a hundred metres is well under a pixel -- so
			// every soft profile in the fragment shader was thrown away by the
			// rasteriser and the field came out as hard thin scratches. A
			// brush stroke has to cover enough pixels for its own taper to be
			// visible, and that is a lower bound in *pixels*, which means
			// metres once the distance is fixed by the box.
			float width = 0.7f + 2.8f * random();
			float phase = random();

			glm::vec3 carried(length, width, phase);

			unsigned int at = (unsigned int)mesh.Vertices.size();

			mesh.Vertices.push_back({ home, carried, { 0.0f, 0.0f } });
			mesh.Vertices.push_back({ home, carried, { 1.0f, 0.0f } });
			mesh.Vertices.push_back({ home, carried, { 1.0f, 1.0f } });
			mesh.Vertices.push_back({ home, carried, { 0.0f, 1.0f } });

			mesh.Indices.insert(mesh.Indices.end(),
				{ at, at + 1, at + 2, at, at + 2, at + 3 });
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)mesh.Indices.size();

		mesh.Submeshes.push_back(all);
		mesh.RecalculateBounds();

		return mesh;
	}

	inline const char* VertexSource()
	{
		return R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;   // home in the box
			layout(location = 1) in vec3 a_Normal;     // length, width, phase
			layout(location = 2) in vec2 a_TexCoord;   // along, across

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;   // identity; the renderer binds it

			// Camera-relative, because the box is centred on the camera and
			// the wrap below only works about the origin.
			uniform vec3 u_Wind;
			uniform float u_Extent;
			uniform float u_Time;
			uniform float u_Speed;

			out vec2 v_Uv;
			out float v_Fade;

			void main()
			{
				// `length` would shadow the builtin used below.
				float reach = a_Normal.x;
				float width  = a_Normal.y;
				float phase  = a_Normal.z;

				// **Drift, then wrap.** The whole field moves at the wind, and
				// a streak that leaves the box downwind comes back in upwind.
				// The wrap is what makes this endless without the CPU ever
				// touching it -- and it has to happen about the camera, which
				// is the origin here, or the box would be left behind.
				vec3 home = a_Position + u_Wind * u_Time;

				vec3 at = mod(home + 0.5 * u_Extent, u_Extent) - 0.5 * u_Extent;

				// A frame: along the wind, and across it in the plane facing
				// the eye, so a stroke is always seen broadside rather than
				// edge-on. The eye is the origin, so the direction to it is
				// just -at.
				vec3 along = normalize(u_Wind + vec3(1e-6));

				vec3 toEye = normalize(-at);
				vec3 across = cross(along, toEye);

				float span = dot(across, across);

				// Looking straight down the wind there is no "across" -- the
				// stroke is end-on and has no width to show. Collapsing it is
				// the right answer and costs nothing.
				across = span > 1e-6 ? across / sqrt(span) : vec3(0.0);

				vec3 offset = along * ((a_TexCoord.x - 0.5) * reach)
					+ across * ((a_TexCoord.y - 0.5) * width);

				// **Fade near the far wall of the box**, or streaks appear out
				// of nothing at a fixed distance and the box becomes visible.
				float far = 0.5 * u_Extent;
				float away = length(at) / far;

				v_Fade = (1.0 - smoothstep(0.55, 1.0, away)) * u_Speed
					* (0.55 + 0.45 * sin(6.2831853 * phase + u_Time * 0.7));

				v_Uv = a_TexCoord;

				gl_Position = u_ViewProjection * u_Transform * vec4(at + offset, 1.0);
			}
		)";
	}

	inline const char* FragmentSource()
	{
		return R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec2 v_Uv;
			in float v_Fade;

			uniform vec3 u_Colour;
			uniform float u_Strength;

			void main()
			{
				// **A brush stroke, which is a shape and not a line.** Soft at
				// both ends and soft across, so nothing in the field has an
				// edge anywhere. `sin` on the unit interval is zero at both
				// ends and one in the middle, which is exactly the profile
				// wanted, and raising it to a power decides how much of the
				// stroke is its taper -- a high power across makes a thin
				// bright core, a low one along keeps the ends long and faint.
				float along = pow(sin(3.14159265 * v_Uv.x), 1.6);
				float across = pow(sin(3.14159265 * v_Uv.y), 2.4);

				float alpha = along * across * v_Fade * u_Strength;

				if (alpha < 0.002)
					discard;

				// Premultiplied: these add light rather than hiding what is
				// behind them, which is what keeps them reading as air and not
				// as ribbons.
				color = vec4(u_Colour * alpha, alpha);
			}
		)";
	}

}
