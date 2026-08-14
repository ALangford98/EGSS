#include "egsspch.h"
#include "Egss/Json.h"

namespace Egss {

	const JsonValue& JsonValue::Null()
	{
		// One shared instance, returned by every failed lookup. Static rather
		// than a member so that a null result can be chained -- `v["a"]["b"]`
		// on a missing "a" asks the null value for "b" and gets itself back.
		static const JsonValue s_Null;
		return s_Null;
	}

	size_t JsonValue::Size() const
	{
		return (m_Type == Type::Array || m_Type == Type::Object) ? m_Elements.size() : 0;
	}

	const JsonValue& JsonValue::operator[](size_t index) const
	{
		if (m_Type != Type::Array || index >= m_Elements.size())
			return Null();

		return m_Elements[index];
	}

	const JsonValue& JsonValue::operator[](const char* key) const
	{
		if (m_Type != Type::Object)
			return Null();

		for (size_t i = 0; i < m_Keys.size(); i++)
			if (m_Keys[i] == key)
				return m_Elements[i];

		return Null();
	}

	bool JsonValue::Has(const char* key) const
	{
		if (m_Type != Type::Object)
			return false;

		for (const std::string& k : m_Keys)
			if (k == key)
				return true;

		return false;
	}

	const std::string& JsonValue::KeyAt(size_t index) const
	{
		static const std::string s_Empty;
		if (m_Type != Type::Object || index >= m_Keys.size())
			return s_Empty;

		return m_Keys[index];
	}

	const JsonValue& JsonValue::ValueAt(size_t index) const
	{
		if (index >= m_Elements.size())
			return Null();

		return m_Elements[index];
	}

	// --- The parser ---------------------------------------------------------
	//
	// Recursive descent, which for a grammar this small is the shape that reads
	// closest to the specification: one function per production.

	class JsonParser
	{
	public:
		JsonParser(const char* text, size_t length)
			: m_Text(text), m_Length(length)
		{
		}

		bool Run(JsonValue& out, std::string& error)
		{
			SkipWhitespace();

			if (!ParseValue(out, 0))
			{
				error = m_Error;
				return false;
			}

			SkipWhitespace();

			// Trailing content is an error rather than something to ignore. A
			// second document glued onto the first is far more likely to be a
			// truncated write or a concatenation bug than something intended.
			if (m_Pos != m_Length)
			{
				error = Message("trailing content after the top-level value");
				return false;
			}

			return true;
		}
	private:
		// Depth is capped because the parser recurses and the input is a file
		// off disk. Without this, `[[[[[...` is a stack overflow rather than a
		// parse error -- a crash on malformed input, which is not a thing a
		// loader is allowed to do. 200 is far past anything glTF nests.
		static constexpr int MaxDepth = 200;

		bool ParseValue(JsonValue& out, int depth)
		{
			if (depth > MaxDepth)
				return Fail("nesting is too deep");

			if (m_Pos >= m_Length)
				return Fail("expected a value, found the end of the document");

			switch (m_Text[m_Pos])
			{
			case '{': return ParseObject(out, depth);
			case '[': return ParseArray(out, depth);
			case '"': return ParseStringValue(out);
			case 't': return ParseLiteral("true", JsonValue::Type::Bool, true, out);
			case 'f': return ParseLiteral("false", JsonValue::Type::Bool, false, out);
			case 'n': return ParseLiteral("null", JsonValue::Type::Null, false, out);
			default:  return ParseNumber(out);
			}
		}

		bool ParseObject(JsonValue& out, int depth)
		{
			m_Pos++;   // '{'
			out.m_Type = JsonValue::Type::Object;

			SkipWhitespace();
			if (Peek() == '}') { m_Pos++; return true; }

			for (;;)
			{
				SkipWhitespace();

				if (Peek() != '"')
					return Fail("expected a member name in an object");

				std::string key;
				if (!ParseStringLiteral(key))
					return false;

				SkipWhitespace();
				if (Peek() != ':')
					return Fail("expected ':' after a member name");
				m_Pos++;

				SkipWhitespace();

				out.m_Keys.push_back(std::move(key));
				out.m_Elements.emplace_back();
				if (!ParseValue(out.m_Elements.back(), depth + 1))
					return false;

				SkipWhitespace();

				if (Peek() == ',') { m_Pos++; continue; }
				if (Peek() == '}') { m_Pos++; return true; }

				return Fail("expected ',' or '}' in an object");
			}
		}

		bool ParseArray(JsonValue& out, int depth)
		{
			m_Pos++;   // '['
			out.m_Type = JsonValue::Type::Array;

			SkipWhitespace();
			if (Peek() == ']') { m_Pos++; return true; }

			for (;;)
			{
				SkipWhitespace();

				out.m_Elements.emplace_back();
				if (!ParseValue(out.m_Elements.back(), depth + 1))
					return false;

				SkipWhitespace();

				if (Peek() == ',') { m_Pos++; continue; }
				if (Peek() == ']') { m_Pos++; return true; }

				return Fail("expected ',' or ']' in an array");
			}
		}

		bool ParseStringValue(JsonValue& out)
		{
			std::string s;
			if (!ParseStringLiteral(s))
				return false;

			out.m_Type = JsonValue::Type::String;
			out.m_String = std::move(s);
			return true;
		}

		bool ParseStringLiteral(std::string& out)
		{
			m_Pos++;   // opening quote

			for (;;)
			{
				if (m_Pos >= m_Length)
					return Fail("unterminated string");

				char c = m_Text[m_Pos];

				if (c == '"') { m_Pos++; return true; }

				if (c != '\\')
				{
					// Control characters are illegal unescaped, and letting
					// them through silently would mean a corrupted file parses
					// into plausible-looking garbage.
					if ((unsigned char)c < 0x20)
						return Fail("a raw control character in a string");

					out.push_back(c);
					m_Pos++;
					continue;
				}

				m_Pos++;   // backslash
				if (m_Pos >= m_Length)
					return Fail("unterminated escape");

				char e = m_Text[m_Pos++];
				switch (e)
				{
				case '"':  out.push_back('"');  break;
				case '\\': out.push_back('\\'); break;
				case '/':  out.push_back('/');  break;
				case 'b':  out.push_back('\b'); break;
				case 'f':  out.push_back('\f'); break;
				case 'n':  out.push_back('\n'); break;
				case 'r':  out.push_back('\r'); break;
				case 't':  out.push_back('\t'); break;
				case 'u':
					if (!ParseUnicodeEscape(out))
						return false;
					break;
				default:
					return Fail("unknown escape sequence");
				}
			}
		}

		// \uXXXX, encoded out as UTF-8.
		//
		// Surrogate pairs are joined rather than passed through: a lone
		// surrogate is not valid UTF-8, and the place this shows up is a model
		// exported from a tool with a non-ASCII node name -- which should load,
		// not fail. Anything above the BMP arrives as a pair, so without this
		// an emoji in a filename becomes two broken code points.
		bool ParseUnicodeEscape(std::string& out)
		{
			unsigned int code = 0;
			if (!ParseHex4(code))
				return false;

			if (code >= 0xD800 && code <= 0xDBFF)
			{
				// High surrogate: a low one must follow.
				if (m_Pos + 1 < m_Length && m_Text[m_Pos] == '\\' && m_Text[m_Pos + 1] == 'u')
				{
					size_t save = m_Pos;
					m_Pos += 2;

					unsigned int low = 0;
					if (!ParseHex4(low))
						return false;

					if (low >= 0xDC00 && low <= 0xDFFF)
						code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
					else
						m_Pos = save;   // not a pair after all; leave it alone
				}
			}

			// UTF-8, by the book.
			if (code < 0x80)
			{
				out.push_back((char)code);
			}
			else if (code < 0x800)
			{
				out.push_back((char)(0xC0 | (code >> 6)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
			else if (code < 0x10000)
			{
				out.push_back((char)(0xE0 | (code >> 12)));
				out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
			else
			{
				out.push_back((char)(0xF0 | (code >> 18)));
				out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
				out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}

			return true;
		}

		bool ParseHex4(unsigned int& out)
		{
			if (m_Pos + 4 > m_Length)
				return Fail("truncated \\u escape");

			out = 0;
			for (int i = 0; i < 4; i++)
			{
				char c = m_Text[m_Pos++];
				out <<= 4;

				if (c >= '0' && c <= '9')      out |= (unsigned int)(c - '0');
				else if (c >= 'a' && c <= 'f') out |= (unsigned int)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') out |= (unsigned int)(c - 'A' + 10);
				else return Fail("a non-hex digit in a \\u escape");
			}

			return true;
		}

		bool ParseNumber(JsonValue& out)
		{
			size_t start = m_Pos;

			if (Peek() == '-')
				m_Pos++;

			// JSON forbids a leading zero on a multi-digit integer, and forbids
			// a bare '.' either side. Checked because strtod would happily
			// accept "01" and ".5" and hide a malformed file.
			if (m_Pos < m_Length && m_Text[m_Pos] == '0')
			{
				m_Pos++;
			}
			else
			{
				size_t digits = m_Pos;
				while (m_Pos < m_Length && IsDigit(m_Text[m_Pos]))
					m_Pos++;

				if (m_Pos == digits)
					return Fail("expected a digit at the start of a number");
			}

			if (m_Pos < m_Length && m_Text[m_Pos] == '.')
			{
				m_Pos++;
				size_t digits = m_Pos;
				while (m_Pos < m_Length && IsDigit(m_Text[m_Pos]))
					m_Pos++;

				if (m_Pos == digits)
					return Fail("expected a digit after the decimal point");
			}

			if (m_Pos < m_Length && (m_Text[m_Pos] == 'e' || m_Text[m_Pos] == 'E'))
			{
				m_Pos++;
				if (m_Pos < m_Length && (m_Text[m_Pos] == '+' || m_Text[m_Pos] == '-'))
					m_Pos++;

				size_t digits = m_Pos;
				while (m_Pos < m_Length && IsDigit(m_Text[m_Pos]))
					m_Pos++;

				if (m_Pos == digits)
					return Fail("expected a digit in the exponent");
			}

			// The text has already been validated, so the only job left is the
			// decimal-to-binary conversion, which is not worth doing by hand --
			// getting the rounding right is genuinely difficult and strtod is
			// required to get it right.
			//
			// The copy into a buffer is because the input is not
			// null-terminated: it may be a chunk in the middle of a .glb.
			std::string text(m_Text + start, m_Pos - start);
			out.m_Type = JsonValue::Type::Number;
			out.m_Number = std::strtod(text.c_str(), nullptr);
			return true;
		}

		bool ParseLiteral(const char* word, JsonValue::Type type, bool boolValue, JsonValue& out)
		{
			size_t n = std::strlen(word);
			if (m_Pos + n > m_Length || std::memcmp(m_Text + m_Pos, word, n) != 0)
				return Fail("expected a literal (true, false or null)");

			m_Pos += n;
			out.m_Type = type;
			out.m_Bool = boolValue;
			return true;
		}

		static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

		char Peek() const { return m_Pos < m_Length ? m_Text[m_Pos] : '\0'; }

		void SkipWhitespace()
		{
			while (m_Pos < m_Length)
			{
				char c = m_Text[m_Pos];
				if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
					m_Pos++;
				else
					break;
			}
		}

		std::string Message(const std::string& what) const
		{
			return "JSON at byte " + std::to_string(m_Pos) + ": " + what;
		}

		bool Fail(const std::string& what)
		{
			// The first failure is the informative one. Recursive descent
			// unwinds through every enclosing production on the way out, and
			// each would happily overwrite the message with "expected ',' or
			// ']'" -- which describes where the error was noticed rather than
			// where it was.
			if (m_Error.empty())
				m_Error = Message(what);

			return false;
		}

		const char* m_Text;
		size_t m_Length;
		size_t m_Pos = 0;
		std::string m_Error;
	};

	bool JsonValue::Parse(const char* text, size_t length, JsonValue& out, std::string& error)
	{
		error.clear();
		out = JsonValue();

		// A UTF-8 byte-order mark is not JSON and the spec says so, but plenty
		// of editors write one and refusing the file helps nobody.
		if (length >= 3 && (unsigned char)text[0] == 0xEF
			&& (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		{
			text += 3;
			length -= 3;
		}

		JsonParser parser(text, length);

		JsonValue parsed;
		if (!parser.Run(parsed, error))
			return false;

		out = std::move(parsed);
		return true;
	}

}
