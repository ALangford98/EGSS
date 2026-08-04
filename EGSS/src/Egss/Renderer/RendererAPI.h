#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace Egss {

	enum class BlendMode
	{
		// Straight alpha: what is drawn later covers what is underneath.
		// Right for sprites and UI.
		Alpha = 0,
		// Colours sum. Right for light: two lights overlapping should be
		// brighter than either, not one hiding the other.
		Additive,
		// Source times destination. Right for a surface being *lit*: draw the
		// light into the buffer first, then multiply the surface's own colour
		// through it, and a red wall under a white light stays red instead of
		// turning white. Additive cannot express that -- it can only ever make
		// things brighter, never tint them.
		Multiply,
		None
	};

	// The set of operations any graphics backend must provide. Keeping this
	// narrow is what makes a second backend possible later.
	class EGSS_API RendererAPI
	{
	public:
		enum class API
		{
			None = 0,
			OpenGL = 1
		};
	public:
		virtual ~RendererAPI() = default;

		virtual void Init() = 0;
		virtual void SetViewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear() = 0;

		// `firstIndex` is an offset into the index buffer, which is what lets one
		// buffer hold several submeshes and be drawn a range at a time. It
		// counts indices, not bytes -- the multiply is the backend's business.
		virtual void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray,
			unsigned int indexCount = 0, unsigned int firstIndex = 0) = 0;

		// Lines are drawn unindexed: consecutive vertex pairs, one segment
		// each. Sharing an index buffer would only pay off if segments shared
		// endpoints, which debug geometry generally doesn't.
		virtual void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount) = 0;

		// Unindexed triangles: every three vertices are one triangle. Used for
		// generated geometry like light polygons, where nothing is reused and
		// an index buffer would only add bookkeeping.
		virtual void DrawTriangles(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount) = 0;
		
		// virtual void DrawTriangles(const std::shared_ptr<VertexArray>& vertexArray,unsigned int vertexCount) = 0;
		// Widths above 1.0 are not guaranteed in a core profile and are
		// ignored by most drivers -- thick lines have to be built from quads.
		virtual void SetLineWidth(float width) = 0;

		// Global state, so anything already batched must be flushed before
		// changing it -- see the two-pass draw in the Lighting2D demo.
		virtual void SetBlendMode(BlendMode mode) = 0;

		// Additive overlays -- lights, glows -- must not depth-test against
		// each other. At equal depth the test rejects everything after the
		// first, so the second light would be silently discarded exactly where
		// it overlaps the first.
		virtual void SetDepthTest(bool enabled) = 0;

		// Discards triangles wound away from the camera, which for a closed
		// mesh is every face on its far side -- roughly half the fragments,
		// for free. Off by default: it is only safe once *all* geometry in the
		// pass is wound consistently, and one flipped model shows up as holes.
		virtual void SetBackfaceCulling(bool enabled) = 0;

		// Reads a rectangle of whatever framebuffer is currently bound back
		// into CPU memory, as tightly packed RGBA8. `out` needs room for
		// width * height * 4 bytes.
		//
		// Rows come back **bottom-up**, because that is the order the GL keeps
		// them in. Every image format worth writing is top-down, so anything
		// saving this has to flip.
		virtual void ReadPixels(unsigned int x, unsigned int y, unsigned int width,
			unsigned int height, unsigned char* out) = 0;

		inline static API GetAPI() { return s_API; }
	private:
		static API s_API;
	};

}
