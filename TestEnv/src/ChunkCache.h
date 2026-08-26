#pragma once

#include <Egss.h>

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// A file of voxel chunks, keyed by chunk coordinate, that survives the process.
//
// The world here is procedural and deterministic, so nothing is *lost* by
// regenerating it -- the cache buys time, not data. What makes that worth
// having is the asymmetry: producing a chunk costs a density evaluation per
// voxel (4,096 of them), and reading one back costs a seek and a memcpy.
//
// **The dangerous failure is a stale cache**, not a missing one: change the
// terrain function, forget to clear the file, and the world silently comes back
// as the old one while the code says otherwise. That is a whole session lost to
// a wrong assumption, so it is not left to a version number somebody has to
// remember to bump. The caller passes a *fingerprint*, and OpenWorld builds it
// by sampling its own density function at fixed points and hashing the results
// -- a fingerprint derived from what the function does, which cannot drift out
// of step with it. A mismatch discards the file and starts again.
class ChunkCache
{
public:
	~ChunkCache() { Close(); }

	// Opens or creates `path`. A file whose magic or fingerprint disagrees is
	// truncated rather than repaired -- it describes a different world, and the
	// only correct thing to do with it is throw it away.
	void Open(const std::string& path, unsigned long long fingerprint)
	{
		Close();

		m_Path = path;
		m_Fingerprint = fingerprint;

		if (ReadIndex())
			return;

		// Either absent, or from another world. Start it over.
		m_Index.clear();

		std::ofstream fresh(m_Path, std::ios::binary | std::ios::trunc);
		if (!fresh)
			return;

		fresh.write(s_Magic, 8);
		fresh.write(reinterpret_cast<const char*>(&m_Fingerprint), sizeof(m_Fingerprint));
		fresh.close();

		m_Rebuilt = true;
		OpenForAppend();
	}

	void Close()
	{
		if (m_Out.is_open())
			m_Out.close();
		if (m_In.is_open())
			m_In.close();

		m_Index.clear();
		m_Hits = m_Misses = m_Written = 0;
		m_Rebuilt = false;
	}

	bool Has(const glm::ivec3& chunk) const
	{
		return m_Index.find(Key(chunk)) != m_Index.end();
	}

	// Fills `out` with the stored bytes, or returns false if this chunk is not
	// in the file.
	bool Read(const glm::ivec3& chunk, std::vector<unsigned char>& out)
	{
		auto it = m_Index.find(Key(chunk));
		if (it == m_Index.end() || !m_In.is_open())
		{
			m_Misses++;
			return false;
		}

		out.resize(it->second.Length);

		m_In.clear();
		m_In.seekg((std::streamoff)it->second.Offset);
		m_In.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());

		if (!m_In)
		{
			// A truncated file is not worth a recovery path; treat the entry as
			// absent and let the caller regenerate.
			m_In.clear();
			m_Misses++;
			return false;
		}

		m_Hits++;
		return true;
	}

	// Appends a chunk. Writing record by record rather than dumping an index at
	// the end means a run that is killed still leaves every completed record
	// readable -- the scan on the next open stops at the first short read.
	void Write(const glm::ivec3& chunk, const std::vector<unsigned char>& bytes)
	{
		if (!m_Out.is_open() || bytes.empty())
			return;

		Entry entry;
		entry.Length = (unsigned int)bytes.size();

		int coord[3] = { chunk.x, chunk.y, chunk.z };
		m_Out.write(reinterpret_cast<const char*>(coord), sizeof(coord));
		m_Out.write(reinterpret_cast<const char*>(&entry.Length), sizeof(entry.Length));

		entry.Offset = (unsigned long long)m_Out.tellp();
		m_Out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
		m_Out.flush();

		entry.Chunk = chunk;

		m_Index[Key(chunk)] = entry;
		m_Written++;
	}

	size_t Entries() const { return m_Index.size(); }

	// Which chunks the file holds. A procedural world's cache is a set of
	// exceptions to the generator, and a caller that stores only what was
	// edited needs the list back to know what to *not* generate.
	std::vector<glm::ivec3> Chunks() const
	{
		std::vector<glm::ivec3> out;
		out.reserve(m_Index.size());

		for (const auto& [key, entry] : m_Index)
			out.push_back(entry.Chunk);

		return out;
	}
	unsigned int Hits() const { return m_Hits; }
	unsigned int Misses() const { return m_Misses; }
	unsigned int Written() const { return m_Written; }
	bool Rebuilt() const { return m_Rebuilt; }
	const std::string& Path() const { return m_Path; }

	unsigned long long BytesOnDisk() const
	{
		std::ifstream f(m_Path, std::ios::binary | std::ios::ate);
		return f ? (unsigned long long)f.tellg() : 0ull;
	}

private:
	struct Entry
	{
		unsigned long long Offset = 0;
		unsigned int Length = 0;
		glm::ivec3 Chunk = glm::ivec3(0);
	};

	// **Twenty-one bits an axis, not ten.**
	//
	// This used to bias by 512 and pack into a thousand per axis, which is
	// four times what OpenWorld's 800-voxel field needs and a two-hundredth of
	// what a planet does: at 250 km a chunk index reaches 20,900, and every
	// one past 511 aliased onto another chunk -- a cache that silently hands
	// back the wrong ground. The same wrap the planet's own chunk key hit and
	// was widened for, in a second place.
	//
	// The file is unaffected: a record carries its own coordinates, so this is
	// only how the index is held in memory.
	static constexpr int KeyBias = 1 << 20;

	static size_t Key(const glm::ivec3& c)
	{
		return ((size_t)(c.z + KeyBias) << 42)
			| ((size_t)(c.y + KeyBias) << 21)
			| (size_t)(c.x + KeyBias);
	}

	// Walks the records reading only their headers, skipping each payload with
	// a seek. The index is built without the payloads ever being read, which is
	// what keeps opening a large cache cheap.
	bool ReadIndex()
	{
		std::ifstream f(m_Path, std::ios::binary);
		if (!f)
			return false;

		char magic[8] = {};
		f.read(magic, 8);
		if (!f || std::memcmp(magic, s_Magic, 8) != 0)
			return false;

		unsigned long long stored = 0;
		f.read(reinterpret_cast<char*>(&stored), sizeof(stored));
		if (!f || stored != m_Fingerprint)
			return false;

		m_Index.clear();

		for (;;)
		{
			int coord[3];
			f.read(reinterpret_cast<char*>(coord), sizeof(coord));
			if (!f)
				break;

			unsigned int length = 0;
			f.read(reinterpret_cast<char*>(&length), sizeof(length));
			if (!f || length == 0)
				break;

			Entry entry;
			entry.Offset = (unsigned long long)f.tellg();
			entry.Length = length;

			f.seekg((std::streamoff)length, std::ios::cur);
			if (!f)
				break;   // truncated final record -- everything before it stands

			entry.Chunk = { coord[0], coord[1], coord[2] };

			m_Index[Key(entry.Chunk)] = entry;
		}

		f.close();
		OpenForAppend();
		return true;
	}

	void OpenForAppend()
	{
		m_Out.open(m_Path, std::ios::binary | std::ios::app);
		m_In.open(m_Path, std::ios::binary);
	}

	static constexpr const char* s_Magic = "EGSSVOX1";

	std::string m_Path;
	unsigned long long m_Fingerprint = 0;

	std::unordered_map<size_t, Entry> m_Index;
	std::ofstream m_Out;
	std::ifstream m_In;

	unsigned int m_Hits = 0, m_Misses = 0, m_Written = 0;
	bool m_Rebuilt = false;
};
