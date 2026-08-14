#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	// A JSON document, parsed into a tree.
	//
	// Written rather than vendored. glTF is the only thing in the engine that
	// needs JSON, and it uses a plain subset of it -- no huge documents, no
	// exotic numbers, no streaming. A general-purpose library would be tens of
	// thousands of lines on every translation unit that touched a model, and a
	// submodule on a project where a missing submodule is already the most
	// confusing way for a fresh clone to fail.
	//
	// Objects keep their members in a **vector, in file order**, and look up by
	// linear scan. That is deliberate rather than lazy: glTF objects have a
	// handful of keys each, a map would cost an allocation per node to save
	// nothing at that size, and file order is occasionally worth having (it is
	// what makes a round-trip diff readable). The one place it would matter --
	// thousands of accessors -- is indexed by *position in an array*, not by
	// name, so it never sees the scan.
	class EGSS_API JsonValue
	{
	public:
		enum class Type { Null, Bool, Number, String, Array, Object };

		JsonValue() = default;

		Type GetType() const { return m_Type; }

		bool IsNull() const { return m_Type == Type::Null; }
		bool IsBool() const { return m_Type == Type::Bool; }
		bool IsNumber() const { return m_Type == Type::Number; }
		bool IsString() const { return m_Type == Type::String; }
		bool IsArray() const { return m_Type == Type::Array; }
		bool IsObject() const { return m_Type == Type::Object; }

		// --- Reading -------------------------------------------------------
		//
		// Every accessor takes a fallback and never throws. A loader for a file
		// format spends most of its lines asking for things that are optional,
		// and `GetFloat("scale", 1.0f)` says what the spec says -- "default 1"
		// -- in one line instead of four.

		bool GetBool(bool fallback = false) const { return m_Type == Type::Bool ? m_Bool : fallback; }
		double GetDouble(double fallback = 0.0) const { return m_Type == Type::Number ? m_Number : fallback; }
		float GetFloat(float fallback = 0.0f) const { return m_Type == Type::Number ? (float)m_Number : fallback; }
		int GetInt(int fallback = 0) const { return m_Type == Type::Number ? (int)m_Number : fallback; }
		const std::string& GetString(const std::string& fallback) const
		{
			return m_Type == Type::String ? m_String : fallback;
		}
		std::string GetString() const { return m_Type == Type::String ? m_String : std::string(); }

		// Element count, for arrays and objects alike. 0 for everything else,
		// so `for (size_t i = 0; i < v.Size(); i++)` is safe on a missing key.
		size_t Size() const;

		// Array element. Out of range gives the shared null value rather than
		// undefined behaviour, so a truncated file misparses instead of
		// crashing.
		const JsonValue& operator[](size_t index) const;

		// The same thing for a plain `int`, and it has to exist: `v[0]` is
		// otherwise ambiguous, because a literal 0 is both an int and a null
		// pointer constant, so it matches the `const char*` overload just as
		// well as the numeric one. Loader code is nothing but `v["meshes"][0]`,
		// so this is not an edge case worth making callers cast around.
		const JsonValue& operator[](int index) const
		{
			return index < 0 ? Null() : (*this)[(size_t)index];
		}

		// Object member. A missing key gives null, which every accessor above
		// turns into its fallback -- so an absent object and an absent field
		// read identically, and optional-with-a-default needs no `Has` call.
		const JsonValue& operator[](const char* key) const;

		bool Has(const char* key) const;

		// Member name at `index`, for the few places that must enumerate rather
		// than ask -- glTF's `extensions` is the one that matters.
		const std::string& KeyAt(size_t index) const;
		const JsonValue& ValueAt(size_t index) const;

		// --- Parsing -------------------------------------------------------

		// Fills `out` and returns true, or fills `error` with a message naming
		// the byte offset and returns false. Never throws.
		static bool Parse(const char* text, size_t length, JsonValue& out, std::string& error);
		static bool Parse(const std::string& text, JsonValue& out, std::string& error)
		{
			return Parse(text.data(), text.size(), out, error);
		}

		// The value every failed lookup returns.
		static const JsonValue& Null();
	private:
		friend class JsonParser;

		Type m_Type = Type::Null;
		bool m_Bool = false;
		double m_Number = 0.0;
		std::string m_String;

		// Arrays use Elements; objects use both, index for index. Two vectors
		// rather than a vector of pairs so that an array pays nothing for the
		// keys it does not have.
		std::vector<JsonValue> m_Elements;
		std::vector<std::string> m_Keys;
	};

}
