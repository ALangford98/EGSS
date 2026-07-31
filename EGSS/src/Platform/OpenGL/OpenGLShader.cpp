#include "egsspch.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include "Egss/Log.h"

#include <fstream>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

namespace Egss {

	Shader* Shader::Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
	{
		return new OpenGLShader(name, vertexSrc, fragmentSrc);
	}

	Shader* Shader::Create(const std::string& filepath)
	{
		return new OpenGLShader(filepath);
	}

	static unsigned int ShaderTypeFromString(const std::string& type)
	{
		if (type == "vertex")
			return GL_VERTEX_SHADER;
		if (type == "fragment" || type == "pixel")
			return GL_FRAGMENT_SHADER;

		EGSS_CORE_ERROR("Unknown shader type '{0}'", type);
		return 0;
	}

	OpenGLShader::OpenGLShader(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
		: m_Name(name)
	{
		std::unordered_map<unsigned int, std::string> sources;
		sources[GL_VERTEX_SHADER] = vertexSrc;
		sources[GL_FRAGMENT_SHADER] = fragmentSrc;
		Compile(sources);
	}

	OpenGLShader::OpenGLShader(const std::string& filepath)
	{
		std::string source = ReadFile(filepath);
		auto shaderSources = PreProcess(source);
		Compile(shaderSources);

		// Name defaults to the filename without directory or extension.
		auto lastSlash = filepath.find_last_of("/\\");
		lastSlash = (lastSlash == std::string::npos) ? 0 : lastSlash + 1;
		auto lastDot = filepath.rfind('.');
		auto count = (lastDot == std::string::npos) ? filepath.size() - lastSlash : lastDot - lastSlash;
		m_Name = filepath.substr(lastSlash, count);
	}

	OpenGLShader::~OpenGLShader()
	{
		glDeleteProgram(m_RendererID);
	}

	std::string OpenGLShader::ReadFile(const std::string& filepath)
	{
		std::string result;
		std::ifstream in(filepath, std::ios::in | std::ios::binary);
		if (!in)
		{
			EGSS_CORE_ERROR("Could not open shader file '{0}'", filepath);
			return result;
		}

		in.seekg(0, std::ios::end);
		result.resize((size_t)in.tellg());
		in.seekg(0, std::ios::beg);
		in.read(&result[0], result.size());

		return result;
	}

	// Splits one file into stages on `#type <stage>` lines.
	std::unordered_map<unsigned int, std::string> OpenGLShader::PreProcess(const std::string& source)
	{
		std::unordered_map<unsigned int, std::string> shaderSources;

		const char* typeToken = "#type";
		size_t typeTokenLength = strlen(typeToken);
		size_t pos = source.find(typeToken, 0);
		while (pos != std::string::npos)
		{
			size_t eol = source.find_first_of("\r\n", pos);
			if (eol == std::string::npos)
			{
				EGSS_CORE_ERROR("Shader syntax error: no newline after #type");
				break;
			}

			size_t begin = pos + typeTokenLength + 1;
			std::string type = source.substr(begin, eol - begin);

			size_t nextLinePos = source.find_first_not_of("\r\n", eol);
			pos = source.find(typeToken, nextLinePos);

			unsigned int stage = ShaderTypeFromString(type);
			if (stage)
			{
				shaderSources[stage] = (pos == std::string::npos)
					? source.substr(nextLinePos)
					: source.substr(nextLinePos, pos - nextLinePos);
			}
		}

		return shaderSources;
	}

	void OpenGLShader::Compile(const std::unordered_map<unsigned int, std::string>& shaderSources)
	{
		unsigned int program = glCreateProgram();
		std::vector<unsigned int> shaderIDs;
		shaderIDs.reserve(shaderSources.size());

		for (auto& kv : shaderSources)
		{
			unsigned int type = kv.first;
			const std::string& source = kv.second;

			unsigned int shader = glCreateShader(type);
			const char* sourceCStr = source.c_str();
			glShaderSource(shader, 1, &sourceCStr, nullptr);
			glCompileShader(shader);

			int compiled = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
			if (!compiled)
			{
				int length = 0;
				glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
				std::vector<char> log(length);
				glGetShaderInfoLog(shader, length, &length, log.data());

				EGSS_CORE_ERROR("Shader '{0}' compilation failed: {1}", m_Name, log.data());
				glDeleteShader(shader);
				continue;
			}

			glAttachShader(program, shader);
			shaderIDs.push_back(shader);
		}

		glLinkProgram(program);

		int linked = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (!linked)
		{
			int length = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
			std::vector<char> log(length);
			glGetProgramInfoLog(program, length, &length, log.data());

			EGSS_CORE_ERROR("Shader '{0}' link failed: {1}", m_Name, log.data());

			glDeleteProgram(program);
			for (unsigned int id : shaderIDs)
				glDeleteShader(id);
			return;
		}

		for (unsigned int id : shaderIDs)
		{
			glDetachShader(program, id);
			glDeleteShader(id);
		}

		m_RendererID = program;
	}

	void OpenGLShader::Bind() const
	{
		glUseProgram(m_RendererID);
	}

	void OpenGLShader::Unbind() const
	{
		glUseProgram(0);
	}

	int OpenGLShader::GetUniformLocation(const std::string& name)
	{
		auto it = m_UniformLocationCache.find(name);
		if (it != m_UniformLocationCache.end())
			return it->second;

		int location = glGetUniformLocation(m_RendererID, name.c_str());
		if (location == -1)
			EGSS_CORE_WARN("Uniform '{0}' not found in shader '{1}'", name, m_Name);

		m_UniformLocationCache[name] = location;
		return location;
	}

	void OpenGLShader::SetInt(const std::string& name, int value)
	{
		glUniform1i(GetUniformLocation(name), value);
	}

	void OpenGLShader::SetFloat(const std::string& name, float value)
	{
		glUniform1f(GetUniformLocation(name), value);
	}

	void OpenGLShader::SetFloat3(const std::string& name, const glm::vec3& value)
	{
		glUniform3f(GetUniformLocation(name), value.x, value.y, value.z);
	}

	void OpenGLShader::SetFloat4(const std::string& name, const glm::vec4& value)
	{
		glUniform4f(GetUniformLocation(name), value.x, value.y, value.z, value.w);
	}

	void OpenGLShader::SetMat4(const std::string& name, const glm::mat4& value)
	{
		glUniformMatrix4fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
	}

}
