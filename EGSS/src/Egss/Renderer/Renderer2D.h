#pragma once

#include "Egss/Renderer/OrthographicCamera.h"
#include "Egss/Renderer/Texture.h"
#include "Egss/Renderer/SubTexture2D.h"

#include <glm/glm.hpp>

namespace Egss {

	// Batched 2D quad renderer.
	//
	// Quads are accumulated into one CPU-side vertex buffer between BeginScene
	// and EndScene, then uploaded and drawn in as few draw calls as possible.
	// A batch is flushed early when it runs out of vertex room or texture
	// slots, so the number of draw calls tracks the number of distinct
	// textures rather than the number of quads.
	class EGSS_API Renderer2D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const OrthographicCamera& camera);
		static void EndScene();
		static void Flush();

		// Flat colour
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);

		// Textured, optionally tiled and tinted
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size,
			const std::shared_ptr<Texture2D>& texture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size,
			const std::shared_ptr<Texture2D>& texture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));

		// Sprite-sheet regions. The whole point of an atlas: many distinct
		// sprites share one texture slot, so they batch together.
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size,
			const std::shared_ptr<SubTexture2D>& subTexture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size,
			const std::shared_ptr<SubTexture2D>& subTexture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));

		// Rotation is in degrees, about the z axis.
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size,
			float rotation, const glm::vec4& color);
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size,
			float rotation, const glm::vec4& color);
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size,
			float rotation, const std::shared_ptr<Texture2D>& texture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size,
			float rotation, const std::shared_ptr<Texture2D>& texture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size,
			float rotation, const std::shared_ptr<SubTexture2D>& subTexture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size,
			float rotation, const std::shared_ptr<SubTexture2D>& subTexture,
			float tilingFactor = 1.0f, const glm::vec4& tint = glm::vec4(1.0f));

		struct Statistics
		{
			unsigned int DrawCalls = 0;
			unsigned int QuadCount = 0;

			unsigned int GetTotalVertexCount() const { return QuadCount * 4; }
			unsigned int GetTotalIndexCount() const { return QuadCount * 6; }
		};

		static Statistics GetStats();
		static void ResetStats();
	private:
		static void StartBatch();
		static void NextBatch();
	};

}
