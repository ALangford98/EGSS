#include "gspch.h"
#include "GS/Renderer/ObjWriter.h"

#include <fstream>
#include <sstream>

namespace GS {

	std::string ObjWriter::Write(const MeshData& data)
	{
		std::ostringstream out;

		for (const MeshVertex& vertex : data.Vertices)
			out << "v " << vertex.Position.x << " " << vertex.Position.y << " " << vertex.Position.z << "\n";

		for (const MeshVertex& vertex : data.Vertices)
			out << "vt " << vertex.TexCoord.x << " " << vertex.TexCoord.y << "\n";

		for (const MeshVertex& vertex : data.Vertices)
			out << "vn " << vertex.Normal.x << " " << vertex.Normal.y << " " << vertex.Normal.z << "\n";

		// .obj indices are 1-based. Each MeshVertex already fuses position/
		// uv/normal together, so a face corner uses the same index three
		// times (v/vt/vn) rather than three separate index streams.
		for (size_t i = 0; i + 2 < data.Indices.size(); i += 3)
		{
			unsigned int a = data.Indices[i] + 1;
			unsigned int b = data.Indices[i + 1] + 1;
			unsigned int c = data.Indices[i + 2] + 1;
			out << "f " << a << "/" << a << "/" << a
				<< " " << b << "/" << b << "/" << b
				<< " " << c << "/" << c << "/" << c << "\n";
		}

		return out.str();
	}

	bool ObjWriter::Save(const std::string& path, const MeshData& data, std::string& error)
	{
		std::ofstream file(path);
		if (!file.is_open())
		{
			error = "could not open '" + path + "' for writing";
			return false;
		}

		file << Write(data);
		return true;
	}

}
