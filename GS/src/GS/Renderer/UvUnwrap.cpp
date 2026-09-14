#include "gspch.h"
#include "GS/Renderer/UvUnwrap.h"

#include <limits>

namespace GS {

	bool UvUnwrap::HasUsableUVs(const MeshData& data)
	{
		if (data.Vertices.empty())
			return false;

		glm::vec2 uvMin(std::numeric_limits<float>::max());
		glm::vec2 uvMax(-std::numeric_limits<float>::max());
		for (const MeshVertex& v : data.Vertices)
		{
			uvMin = glm::min(uvMin, v.TexCoord);
			uvMax = glm::max(uvMax, v.TexCoord);
		}

		float area = (uvMax.x - uvMin.x) * (uvMax.y - uvMin.y);
		return area > 1e-6f;
	}

}
