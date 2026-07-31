#pragma once

#include "Egss/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace Egss {

	// A rectangular region of a larger texture.
	//
	// This is what makes an atlas useful: dozens of distinct sprites share one
	// Texture2D, so they occupy a single texture slot and batch together into
	// one draw call instead of one per sprite.
	class EGSS_API SubTexture2D
	{
	public:
		// min and max are normalised texture coordinates (0..1).
		SubTexture2D(const std::shared_ptr<Texture2D>& texture, const glm::vec2& min, const glm::vec2& max);

		const std::shared_ptr<Texture2D>& GetTexture() const { return m_Texture; }
		const glm::vec2* GetTexCoords() const { return m_TexCoords; }

		// Convenience for a regular grid: `coords` is the cell index,
		// `cellSize` the cell dimensions in pixels, and `spriteSize` how many
		// cells the sprite spans.
		static SubTexture2D* CreateFromCoords(const std::shared_ptr<Texture2D>& texture,
			const glm::vec2& coords, const glm::vec2& cellSize,
			const glm::vec2& spriteSize = { 1.0f, 1.0f });
	private:
		std::shared_ptr<Texture2D> m_Texture;
		// Bottom-left, bottom-right, top-right, top-left, matching the winding
		// Renderer2D uses for its quad vertices.
		glm::vec2 m_TexCoords[4];
	};

}
