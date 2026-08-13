#include "egsspch.h"
#include "Egss/Debug/ReplayParams.h"
#include "Egss/Log.h"

namespace Egss {

	namespace {

		struct Parameter
		{
			std::string Name;
			ReplayParams::Kind Kind = ReplayParams::Kind::Float;
			void* Value = nullptr;
		};

		std::vector<Parameter> s_Parameters;

		void Register(const std::string& name, ReplayParams::Kind kind, void* value)
		{
			if (!value)
				return;

			// Replacing rather than appending. A demo that rebuilds its scene and
			// registers again would otherwise leave an entry pointing at the old
			// object, and the recorder would happily sample it.
			for (Parameter& existing : s_Parameters)
			{
				if (existing.Name == name)
				{
					existing.Kind = kind;
					existing.Value = value;
					return;
				}
			}

			s_Parameters.push_back({ name, kind, value });
		}

	}

	void ReplayParams::RegisterFloat(const std::string& name, float* value)
	{
		Register(name, Kind::Float, value);
	}

	void ReplayParams::RegisterInt(const std::string& name, int* value)
	{
		Register(name, Kind::Int, value);
	}

	void ReplayParams::RegisterBool(const std::string& name, bool* value)
	{
		Register(name, Kind::Bool, value);
	}

	void ReplayParams::Clear()
	{
		s_Parameters.clear();
	}

	size_t ReplayParams::Count()
	{
		return s_Parameters.size();
	}

	const std::string& ReplayParams::NameAt(size_t index)
	{
		static const std::string empty;
		return index < s_Parameters.size() ? s_Parameters[index].Name : empty;
	}

	ReplayParams::Kind ReplayParams::KindAt(size_t index)
	{
		return index < s_Parameters.size() ? s_Parameters[index].Kind : Kind::Float;
	}

	int ReplayParams::FindByName(const std::string& name)
	{
		for (size_t i = 0; i < s_Parameters.size(); i++)
			if (s_Parameters[i].Name == name)
				return (int)i;

		return -1;
	}

	unsigned int ReplayParams::ReadBits(size_t index)
	{
		if (index >= s_Parameters.size())
			return 0;

		const Parameter& parameter = s_Parameters[index];
		unsigned int bits = 0;

		switch (parameter.Kind)
		{
		case Kind::Float:
			// memcpy rather than a cast through a pointer: type punning through
			// a reinterpret_cast is undefined, and a float's bit pattern is
			// exactly what has to survive the round trip for a replay to be a
			// replay rather than an approximation of one.
			std::memcpy(&bits, parameter.Value, sizeof(bits));
			break;

		case Kind::Int:
			std::memcpy(&bits, parameter.Value, sizeof(bits));
			break;

		case Kind::Bool:
			bits = *static_cast<const bool*>(parameter.Value) ? 1u : 0u;
			break;
		}

		return bits;
	}

	void ReplayParams::WriteBits(size_t index, unsigned int bits)
	{
		if (index >= s_Parameters.size())
			return;

		Parameter& parameter = s_Parameters[index];

		switch (parameter.Kind)
		{
		case Kind::Float:
			std::memcpy(parameter.Value, &bits, sizeof(bits));
			break;

		case Kind::Int:
			std::memcpy(parameter.Value, &bits, sizeof(bits));
			break;

		case Kind::Bool:
			*static_cast<bool*>(parameter.Value) = (bits != 0);
			break;
		}
	}

}
