#include "egsspch.h"
#include "Egss/Renderer/Renderer2D.h"

#include "Egss/Renderer/VertexArray.h"
#include "Egss/Renderer/Shader.h"
#include "Egss/Renderer/RenderCommand.h"
#include "Egss/Log.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Egss {

	struct QuadVertex
	{
		glm::vec3 Position;
		glm::vec4 Color;
		glm::vec2 TexCoord;
		float TexIndex;
		float TilingFactor;
	};

	struct Renderer2DData
	{
		static constexpr unsigned int MaxQuads = 10000;
		static constexpr unsigned int MaxVertices = MaxQuads * 4;
		static constexpr unsigned int MaxIndices = MaxQuads * 6;
		// OpenGL 3.3 guarantees at least 16 fragment texture units, so this is
		// the largest value that is safe without querying the driver.
		static constexpr unsigned int MaxTextureSlots = 16;

		std::shared_ptr<VertexArray> QuadVertexArray;
		std::shared_ptr<VertexBuffer> QuadVertexBuffer;
		std::shared_ptr<Shader> TextureShader;
		std::shared_ptr<Texture2D> WhiteTexture;

		unsigned int QuadIndexCount = 0;
		QuadVertex* QuadVertexBufferBase = nullptr;
		QuadVertex* QuadVertexBufferPtr = nullptr;

		std::array<std::shared_ptr<Texture2D>, MaxTextureSlots> TextureSlots;
		// Slot 0 is permanently the white texture, so flat-colour quads take
		// the same path as textured ones.
		unsigned int TextureSlotIndex = 1;

		glm::vec4 QuadVertexPositions[4];

		Renderer2D::Statistics Stats;
	};

	static Renderer2DData s_Data;

	void Renderer2D::Init()
	{
		s_Data.QuadVertexArray.reset(VertexArray::Create());

		s_Data.QuadVertexBuffer.reset(VertexBuffer::Create(Renderer2DData::MaxVertices * sizeof(QuadVertex)));
		s_Data.QuadVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position"     },
			{ ShaderDataType::Float4, "a_Color"        },
			{ ShaderDataType::Float2, "a_TexCoord"     },
			{ ShaderDataType::Float,  "a_TexIndex"     },
			{ ShaderDataType::Float,  "a_TilingFactor" }
		});
		s_Data.QuadVertexArray->AddVertexBuffer(s_Data.QuadVertexBuffer);

		s_Data.QuadVertexBufferBase = new QuadVertex[Renderer2DData::MaxVertices];

		// Quad indices never change, so they're generated once up front.
		unsigned int* quadIndices = new unsigned int[Renderer2DData::MaxIndices];
		unsigned int offset = 0;
		for (unsigned int i = 0; i < Renderer2DData::MaxIndices; i += 6)
		{
			quadIndices[i + 0] = offset + 0;
			quadIndices[i + 1] = offset + 1;
			quadIndices[i + 2] = offset + 2;

			quadIndices[i + 3] = offset + 2;
			quadIndices[i + 4] = offset + 3;
			quadIndices[i + 5] = offset + 0;

			offset += 4;
		}

		std::shared_ptr<IndexBuffer> quadIB;
		quadIB.reset(IndexBuffer::Create(quadIndices, Renderer2DData::MaxIndices));
		s_Data.QuadVertexArray->SetIndexBuffer(quadIB);
		delete[] quadIndices;

		// 1x1 white pixel, so a flat-colour quad is just a white texture
		// multiplied by its colour.
		s_Data.WhiteTexture.reset(Texture2D::Create(1, 1));
		unsigned int whiteTextureData = 0xffffffff;
		s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(unsigned int));

		// GLSL 330 cannot index a sampler array with a non-constant
		// expression, so the switch below is not a stylistic choice -- it is
		// the only legal way to select a slot at this version.
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec4 a_Color;
			layout(location = 2) in vec2 a_TexCoord;
			layout(location = 3) in float a_TexIndex;
			layout(location = 4) in float a_TilingFactor;

			uniform mat4 u_ViewProjection;

			out vec4 v_Color;
			out vec2 v_TexCoord;
			out float v_TexIndex;
			out float v_TilingFactor;

			void main()
			{
				v_Color        = a_Color;
				v_TexCoord     = a_TexCoord;
				v_TexIndex     = a_TexIndex;
				v_TilingFactor = a_TilingFactor;
				gl_Position    = u_ViewProjection * vec4(a_Position, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec4 v_Color;
			in vec2 v_TexCoord;
			in float v_TexIndex;
			in float v_TilingFactor;

			uniform sampler2D u_Textures[16];

			void main()
			{
				vec4 texColor = vec4(1.0);
				vec2 uv = v_TexCoord * v_TilingFactor;

				switch (int(v_TexIndex))
				{
					case  0: texColor = texture(u_Textures[ 0], uv); break;
					case  1: texColor = texture(u_Textures[ 1], uv); break;
					case  2: texColor = texture(u_Textures[ 2], uv); break;
					case  3: texColor = texture(u_Textures[ 3], uv); break;
					case  4: texColor = texture(u_Textures[ 4], uv); break;
					case  5: texColor = texture(u_Textures[ 5], uv); break;
					case  6: texColor = texture(u_Textures[ 6], uv); break;
					case  7: texColor = texture(u_Textures[ 7], uv); break;
					case  8: texColor = texture(u_Textures[ 8], uv); break;
					case  9: texColor = texture(u_Textures[ 9], uv); break;
					case 10: texColor = texture(u_Textures[10], uv); break;
					case 11: texColor = texture(u_Textures[11], uv); break;
					case 12: texColor = texture(u_Textures[12], uv); break;
					case 13: texColor = texture(u_Textures[13], uv); break;
					case 14: texColor = texture(u_Textures[14], uv); break;
					case 15: texColor = texture(u_Textures[15], uv); break;
				}

				color = texColor * v_Color;
			}
		)";

		s_Data.TextureShader.reset(Shader::Create("Renderer2D", vertexSrc, fragmentSrc));

		int samplers[Renderer2DData::MaxTextureSlots];
		for (unsigned int i = 0; i < Renderer2DData::MaxTextureSlots; i++)
			samplers[i] = i;

		s_Data.TextureShader->Bind();
		for (unsigned int i = 0; i < Renderer2DData::MaxTextureSlots; i++)
			s_Data.TextureShader->SetInt("u_Textures[" + std::to_string(i) + "]", samplers[i]);

		s_Data.TextureSlots[0] = s_Data.WhiteTexture;

		// Unit quad in local space; every draw transforms these four corners.
		s_Data.QuadVertexPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[1] = {  0.5f, -0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[2] = {  0.5f,  0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[3] = { -0.5f,  0.5f, 0.0f, 1.0f };

		EGSS_CORE_INFO("Renderer2D initialized ({0} quads/batch, {1} texture slots)",
			Renderer2DData::MaxQuads, Renderer2DData::MaxTextureSlots);
	}

	void Renderer2D::Shutdown()
	{
		delete[] s_Data.QuadVertexBufferBase;
		s_Data.QuadVertexBufferBase = nullptr;
	}

	void Renderer2D::BeginScene(const OrthographicCamera& camera)
	{
		s_Data.TextureShader->Bind();
		s_Data.TextureShader->SetMat4("u_ViewProjection", camera.GetViewProjectionMatrix());

		StartBatch();
	}

	void Renderer2D::StartBatch()
	{
		s_Data.QuadIndexCount = 0;
		s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;
		s_Data.TextureSlotIndex = 1;
	}

	void Renderer2D::EndScene()
	{
		Flush();
	}

	void Renderer2D::Flush()
	{
		if (s_Data.QuadIndexCount == 0)
			return;

		// Only the portion actually written is uploaded.
		unsigned int dataSize = (unsigned int)((unsigned char*)s_Data.QuadVertexBufferPtr -
			(unsigned char*)s_Data.QuadVertexBufferBase);
		s_Data.QuadVertexBuffer->SetData(s_Data.QuadVertexBufferBase, dataSize);

		for (unsigned int i = 0; i < s_Data.TextureSlotIndex; i++)
			s_Data.TextureSlots[i]->Bind(i);

		s_Data.TextureShader->Bind();
		s_Data.QuadVertexArray->Bind();
		RenderCommand::DrawIndexed(s_Data.QuadVertexArray, s_Data.QuadIndexCount);

		s_Data.Stats.DrawCalls++;
	}

	// Flush the current batch and begin another. Called when a batch runs out
	// of vertex room or texture slots.
	void Renderer2D::NextBatch()
	{
		Flush();
		StartBatch();
	}

	// The full texture, in the winding Renderer2D uses for quad corners.
	static const glm::vec2 s_FullTexCoords[4] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
	};

	// Core path: every DrawQuad overload funnels into this.
	static void SubmitQuad(const glm::mat4& transform, const glm::vec4& color,
		float textureIndex, float tilingFactor, const glm::vec2* texCoords)
	{
		for (int i = 0; i < 4; i++)
		{
			s_Data.QuadVertexBufferPtr->Position = glm::vec3(transform * s_Data.QuadVertexPositions[i]);
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = texCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadVertexBufferPtr++;
		}

		s_Data.QuadIndexCount += 6;
		s_Data.Stats.QuadCount++;
	}

	// Returns the slot the texture occupies in the current batch, adding it if
	// necessary and starting a new batch if the slots are exhausted.
	static float ResolveTextureSlot(const std::shared_ptr<Texture2D>& texture)
	{
		for (unsigned int i = 1; i < s_Data.TextureSlotIndex; i++)
		{
			if (s_Data.TextureSlots[i] == texture)
				return (float)i;
		}

		if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
		{
			Renderer2D::Flush();
			s_Data.QuadIndexCount = 0;
			s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;
			s_Data.TextureSlotIndex = 1;
		}

		float index = (float)s_Data.TextureSlotIndex;
		s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
		s_Data.TextureSlotIndex++;
		return index;
	}

	static bool BatchIsFull()
	{
		return s_Data.QuadIndexCount >= Renderer2DData::MaxIndices;
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, color);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
	{
		if (BatchIsFull())
			NextBatch();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
			glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		SubmitQuad(transform, color, 0.0f, 1.0f, s_FullTexCoords);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size,
		const std::shared_ptr<Texture2D>& texture, float tilingFactor, const glm::vec4& tint)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, texture, tilingFactor, tint);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size,
		const std::shared_ptr<Texture2D>& texture, float tilingFactor, const glm::vec4& tint)
	{
		if (BatchIsFull())
			NextBatch();

		float textureIndex = ResolveTextureSlot(texture);

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
			glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		SubmitQuad(transform, tint, textureIndex, tilingFactor, s_FullTexCoords);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size,
		float rotation, const glm::vec4& color)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, color);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size,
		float rotation, const glm::vec4& color)
	{
		if (BatchIsFull())
			NextBatch();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
			glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f }) *
			glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		SubmitQuad(transform, color, 0.0f, 1.0f, s_FullTexCoords);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size,
		float rotation, const std::shared_ptr<Texture2D>& texture, float tilingFactor, const glm::vec4& tint)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, texture, tilingFactor, tint);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size,
		float rotation, const std::shared_ptr<Texture2D>& texture, float tilingFactor, const glm::vec4& tint)
	{
		if (BatchIsFull())
			NextBatch();

		float textureIndex = ResolveTextureSlot(texture);

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
			glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f }) *
			glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		SubmitQuad(transform, tint, textureIndex, tilingFactor, s_FullTexCoords);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size,
		const std::shared_ptr<SubTexture2D>& subTexture, float tilingFactor, const glm::vec4& tint)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, subTexture, tilingFactor, tint);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size,
		const std::shared_ptr<SubTexture2D>& subTexture, float tilingFactor, const glm::vec4& tint)
	{
		if (BatchIsFull())
			NextBatch();

		// The slot is resolved from the atlas, so every sprite cut from the
		// same sheet shares one slot.
		float textureIndex = ResolveTextureSlot(subTexture->GetTexture());

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
			glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		SubmitQuad(transform, tint, textureIndex, tilingFactor, subTexture->GetTexCoords());
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size,
		float rotation, const std::shared_ptr<SubTexture2D>& subTexture, float tilingFactor, const glm::vec4& tint)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, subTexture, tilingFactor, tint);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size,
		float rotation, const std::shared_ptr<SubTexture2D>& subTexture, float tilingFactor, const glm::vec4& tint)
	{
		if (BatchIsFull())
			NextBatch();

		float textureIndex = ResolveTextureSlot(subTexture->GetTexture());

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
			glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f }) *
			glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		SubmitQuad(transform, tint, textureIndex, tilingFactor, subTexture->GetTexCoords());
	}

	Renderer2D::Statistics Renderer2D::GetStats()
	{
		return s_Data.Stats;
	}

	void Renderer2D::ResetStats()
	{
		s_Data.Stats = Statistics();
	}

}
