#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace Egss {

	// How a triangle is filled in. Debug views want the other two: `Line` shows
	// which vertices the mesher joined to which, and `Point` shows where it put
	// them -- both invisible under a shaded surface, and both the first thing
	// worth looking at when a mesh comes out wrong.
	enum class PolygonMode
	{
		Fill = 0,
		Line,
		Point
	};

	// Which winding direction a draw discards. Back is the ordinary case --
	// see SetCullFace. Front is what an inverted-hull outline wants: draw
	// the mesh again, scaled out slightly, in a flat colour, and only the
	// faces that would normally be the *far* side of the model survive,
	// seen from inside a slightly larger shell around it.
	enum class CullFace
	{
		None = 0,
		Back,
		Front
	};

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

		// Whether a draw *updates* the depth buffer, independent of whether it
		// is tested against it. A transparent surface wants both on for the
		// test and off for the write: tested, so opaque geometry already drawn
		// still occludes it correctly; not written, so a second transparent
		// surface behind it is not wrongly rejected by the first one's own
		// depth. Left on, two overlapping transparent surfaces draw in
		// whichever order the depth buffer prefers rather than the order they
		// were submitted, which for a blend is visibly wrong.
		virtual void SetDepthWrite(bool enabled) = 0;

		// Discards triangles wound toward `face`, which for a closed mesh
		// wound consistently is every triangle facing that way -- roughly half
		// the fragments, for free, when `face` is Back. `None` is the default:
		// culling either direction is only safe once *all* geometry in the
		// pass is wound consistently, and one flipped model shows up as holes.
		virtual void SetCullFace(CullFace face) = 0;

		// Applies to everything drawn until it is set back. `Line` and `Point`
		// are debug states: they are cheap to set and expensive to forget, so a
		// caller that sets one is responsible for putting Fill back.
		virtual void SetPolygonMode(PolygonMode mode) = 0;

		// Size of a point in pixels, for PolygonMode::Point and DrawPoints.
		virtual void SetPointSize(float size) = 0;

		// Reads a rectangle of whatever framebuffer is currently bound back
		// into CPU memory, as tightly packed RGBA8. `out` needs room for
		// width * height * 4 bytes.
		//
		// Rows come back **bottom-up**, because that is the order the GL keeps
		// them in. Every image format worth writing is top-down, so anything
		// saving this has to flip.
		virtual void ReadPixels(unsigned int x, unsigned int y, unsigned int width,
			unsigned int height, unsigned char* out) = 0;

		// How many textures one fragment shader may sample from at once, which
		// is what caps the size of a Renderer2D batch. Only valid after Init --
		// it is a driver query, and there is no context to ask before then.
		virtual unsigned int GetMaxTextureSlots() const = 0;

		inline static API GetAPI() { return s_API; }
	private:
		static API s_API;
	};

}
