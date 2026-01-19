#include "libslic3r/libslic3r.h"
#include "GLShader.hpp"

#include "3DScene.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Color.hpp"

#include <boost/nowide/fstream.hpp>
#include <GL/glew.h>
#include <cassert>

#include <boost/log/trivial.hpp>

namespace Slic3r {

GLShaderProgram::~GLShaderProgram()
{
    if (m_id > 0)
        glsafe(::glDeleteProgram(m_id));
}

bool GLShaderProgram::init_from_files(const std::string& name, const ShaderFilenames& filenames, const std::initializer_list<std::string_view> &defines)
{
    // Load a shader program from file, prepend defs block.
    auto load_from_file = [](const std::string& filename, const std::string &defs) {
        std::string path = resources_dir() + "/shaders/" + filename;
        boost::nowide::ifstream s(path, boost::nowide::ifstream::binary);
        if (!s.good()) {
            BOOST_LOG_TRIVIAL(error) << "Couldn't open file: '" << path << "'";
            return std::string();
        }

        s.seekg(0, s.end);
        int file_length = static_cast<int>(s.tellg());
        s.seekg(0, s.beg);
        std::string source(defs.size() + file_length, '\0');
        memcpy(source.data(), defs.c_str(), defs.size());
        s.read(source.data() + defs.size(), file_length);
        if (!s.good()) {
            BOOST_LOG_TRIVIAL(error) << "Error while loading file: '" << path << "'";
            return std::string();
        }
        s.close();

        if (! defs.empty()) {
            // Extract the version and flip the order of "defines" and version in the source block.
            size_t idx = source.find("\n", defs.size());
            if (idx != std::string::npos && strncmp(source.c_str() + defs.size(), "#version", 8) == 0) {
                // Swap the version line with the defines.
                size_t len = idx - defs.size() + 1;
                memmove(source.data(), source.c_str() + defs.size(), len);
                memcpy(source.data() + len, defs.c_str(), defs.size());
            }
        }

        return source;
    };

    // Create a block of C "defines" from list of symbols.
    std::string defines_program;
    for (std::string_view def : defines)
        // Our shaders are stored with "\r\n", thus replicate the same here for consistency. Likely "\n" would suffice, 
        // but we don't know all the OpenGL shader compilers around.
        defines_program += format("#define %s\r\n", def);

    ShaderSources sources = {};
    for (size_t i = 0; i < static_cast<size_t>(EShaderType::Count); ++i) {
        sources[i] = filenames[i].empty() ? std::string() : load_from_file(filenames[i], defines_program);
    }

    bool valid = !sources[static_cast<size_t>(EShaderType::Vertex)].empty() && !sources[static_cast<size_t>(EShaderType::Fragment)].empty() && sources[static_cast<size_t>(EShaderType::Compute)].empty();
    valid |= !sources[static_cast<size_t>(EShaderType::Compute)].empty() && sources[static_cast<size_t>(EShaderType::Vertex)].empty() && sources[static_cast<size_t>(EShaderType::Fragment)].empty() && 
              sources[static_cast<size_t>(EShaderType::Geometry)].empty() && sources[static_cast<size_t>(EShaderType::TessEvaluation)].empty() && sources[static_cast<size_t>(EShaderType::TessControl)].empty();

    return valid ? init_from_texts(name, sources) : false;
}

bool GLShaderProgram::init_from_texts(const std::string& name, const ShaderSources& sources)
{
    auto shader_type_as_string = [](EShaderType type) {
        switch (type)
        {
        case EShaderType::Vertex:         { return "vertex"; }
        case EShaderType::Fragment:       { return "fragment"; }
        case EShaderType::Geometry:       { return "geometry"; }
        case EShaderType::TessEvaluation: { return "tesselation evaluation"; }
        case EShaderType::TessControl:    { return "tesselation control"; }
        case EShaderType::Compute:        { return "compute"; }
        default:                          { return "unknown"; }
        }
    };

    auto create_shader = [](EShaderType type) {
        GLuint id = 0;
        switch (type)
        {
        case EShaderType::Vertex:         { id = ::glCreateShader(GL_VERTEX_SHADER); glcheck(); break; }
        case EShaderType::Fragment:       { id = ::glCreateShader(GL_FRAGMENT_SHADER); glcheck(); break; }
        case EShaderType::Geometry:       { id = ::glCreateShader(GL_GEOMETRY_SHADER); glcheck(); break; }
        case EShaderType::TessEvaluation: { id = ::glCreateShader(GL_TESS_EVALUATION_SHADER); glcheck(); break; }
        case EShaderType::TessControl:    { id = ::glCreateShader(GL_TESS_CONTROL_SHADER); glcheck(); break; }
        case EShaderType::Compute:        { id = ::glCreateShader(GL_COMPUTE_SHADER); glcheck(); break; }
        default:                          { break; }
        }
           
        return (id == 0) ? std::make_pair(false, GLuint(0)) : std::make_pair(true, id);
    };

    auto release_shaders = [](const std::array<GLuint, static_cast<size_t>(EShaderType::Count)>& shader_ids) {
        for (size_t i = 0; i < static_cast<size_t>(EShaderType::Count); ++i) {
            if (shader_ids[i] > 0)
                glsafe(::glDeleteShader(shader_ids[i]));
        }
    };

    assert(m_id == 0);

    m_name = name;

    std::array<GLuint, static_cast<size_t>(EShaderType::Count)> shader_ids = { 0 };

    for (size_t i = 0; i < static_cast<size_t>(EShaderType::Count); ++i) {
        const std::string& source = sources[i];
        if (!source.empty()) {
            EShaderType type = static_cast<EShaderType>(i);
            auto [result, id] = create_shader(type);
            if (result)
                shader_ids[i] = id;
            else {
                BOOST_LOG_TRIVIAL(error) << "glCreateShader() failed for " << shader_type_as_string(type) << " shader of shader program '" << name << "'";

                // release shaders
                release_shaders(shader_ids);
                return false;
            }

            const char* source_ptr = source.c_str();
            glsafe(::glShaderSource(id, 1, &source_ptr, nullptr));
            glsafe(::glCompileShader(id));
            GLint params;
            glsafe(::glGetShaderiv(id, GL_COMPILE_STATUS, &params));
            if (params == GL_FALSE) {
                // Compilation failed. 
                glsafe(::glGetShaderiv(id, GL_INFO_LOG_LENGTH, &params));
                std::vector<char> msg(params);
                glsafe(::glGetShaderInfoLog(id, params, &params, msg.data()));
                BOOST_LOG_TRIVIAL(error) << "Unable to compile " << shader_type_as_string(type) << " shader of shader program '" << name << "':\n" << msg.data();

                // release shaders
                release_shaders(shader_ids);
                return false;
            }
        }
    }

    m_id = ::glCreateProgram();
    glcheck();
    if (m_id == 0) {
        BOOST_LOG_TRIVIAL(error) << "glCreateProgram() failed for shader program '" << name << "'";

        // release shaders
        release_shaders(shader_ids);
        return false;
    }

    for (size_t i = 0; i < static_cast<size_t>(EShaderType::Count); ++i) {
        if (shader_ids[i] > 0)
            glsafe(::glAttachShader(m_id, shader_ids[i]));
    }

    glsafe(::glLinkProgram(m_id));
    GLint params;
    glsafe(::glGetProgramiv(m_id, GL_LINK_STATUS, &params));
    if (params == GL_FALSE) {
        // Linking failed. 
        glsafe(::glGetProgramiv(m_id, GL_INFO_LOG_LENGTH, &params));
        std::vector<char> msg(params);
        glsafe(::glGetProgramInfoLog(m_id, params, &params, msg.data()));
        BOOST_LOG_TRIVIAL(error) << "Unable to link shader program '" << name << "':\n" << msg.data();

        // release shaders
        release_shaders(shader_ids);

        // release shader program
        glsafe(::glDeleteProgram(m_id));
        m_id = 0;

        return false;
    }

    // release shaders, they are no more needed
    release_shaders(shader_ids);

    return true;
}

void GLShaderProgram::start_using() const
{
    assert(m_id > 0);
    glsafe(::glUseProgram(m_id));
}

void GLShaderProgram::stop_using() const
{
    glsafe(::glUseProgram(0));
}

void GLShaderProgram::set_uniform(int id, int value) const
{
    if (id >= 0)
        glsafe(::glUniform1i(id, value));
}

void GLShaderProgram::set_uniform(int id, bool value) const
{
    set_uniform(id, value ? 1 : 0);
}

void GLShaderProgram::set_uniform(int id, float value) const
{
    if (id >= 0)
        glsafe(::glUniform1f(id, value));
}

void GLShaderProgram::set_uniform(int id, double value) const
{
    set_uniform(id, static_cast<float>(value));
}

void GLShaderProgram::set_uniform(int id, const std::array<int, 2>& value) const
{
    if (id >= 0)
        glsafe(::glUniform2iv(id, 1, static_cast<const GLint*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const std::array<int, 3>& value) const
{
    if (id >= 0)
        glsafe(::glUniform3iv(id, 1, static_cast<const GLint*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const std::array<int, 4>& value) const
{
    if (id >= 0)
        glsafe(::glUniform4iv(id, 1, static_cast<const GLint*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const std::array<float, 2>& value) const
{
    if (id >= 0)
        glsafe(::glUniform2fv(id, 1, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const std::array<float, 3>& value) const
{
    if (id >= 0)
        glsafe(::glUniform3fv(id, 1, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const std::array<float, 4>& value) const
{
    if (id >= 0)
        glsafe(::glUniform4fv(id, 1, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const std::array<double, 4>& value) const
{
    const std::array<float, 4> f_value = { float(value[0]), float(value[1]), float(value[2]), float(value[3]) };
    set_uniform(id, f_value);
}

void GLShaderProgram::set_uniform(int id, const float* value, size_t size) const
{
    if (id >= 0) {
        if (size == 1)
            set_uniform(id, value[0]);
        else if (size == 2)
            glsafe(::glUniform2fv(id, 1, static_cast<const GLfloat*>(value)));
        else if (size == 3)
            glsafe(::glUniform3fv(id, 1, static_cast<const GLfloat*>(value)));
        else if (size == 4)
            glsafe(::glUniform4fv(id, 1, static_cast<const GLfloat*>(value)));
    }
}

void GLShaderProgram::set_uniform(int id, const Transform3f& value) const
{
    if (id >= 0)
        glsafe(::glUniformMatrix4fv(id, 1, GL_FALSE, static_cast<const GLfloat*>(value.matrix().data())));
}

void GLShaderProgram::set_uniform(int id, const Transform3d& value) const
{
    set_uniform(id, value.cast<float>());
}

void GLShaderProgram::set_uniform(int id, const Matrix3f& value) const
{
    if (id >= 0)
        glsafe(::glUniformMatrix3fv(id, 1, GL_FALSE, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const Matrix3d& value) const
{
    set_uniform(id, (Matrix3f)value.cast<float>());
}

void GLShaderProgram::set_uniform(int id, const Matrix4f& value) const
{
    if (id >= 0)
        glsafe(::glUniformMatrix4fv(id, 1, GL_FALSE, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const Matrix4d& value) const
{
    set_uniform(id, (Matrix4f)value.cast<float>());
}

void GLShaderProgram::set_uniform(int id, const Vec2f& value) const
{
    if (id >= 0)
        glsafe(::glUniform2fv(id, 1, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const Vec2d& value) const
{
    set_uniform(id, static_cast<Vec2f>(value.cast<float>()));
}

void GLShaderProgram::set_uniform(int id, const Vec3f& value) const
{
    if (id >= 0)
        glsafe(::glUniform3fv(id, 1, static_cast<const GLfloat*>(value.data())));
}

void GLShaderProgram::set_uniform(int id, const Vec3d& value) const
{
    set_uniform(id, static_cast<Vec3f>(value.cast<float>()));
}

void GLShaderProgram::set_uniform(int id, const ColorRGB& value) const
{
    set_uniform(id, value.data(), 3);
}

void GLShaderProgram::set_uniform(int id, const ColorRGBA& value) const
{
    set_uniform(id, value.data(), 4);
}

int GLShaderProgram::get_attrib_location(const char* name) const
{
    assert(m_id > 0);

    if (m_id <= 0)
        // Shader program not loaded. This should not happen.
        return -1;

    auto it = std::find_if(m_attrib_location_cache.begin(), m_attrib_location_cache.end(), [name](const auto& p) { return p.first == name; });
    if (it != m_attrib_location_cache.end())
        // Attrib ID cached.
        return it->second;

    int id = ::glGetAttribLocation(m_id, name);
    const_cast<GLShaderProgram*>(this)->m_attrib_location_cache.push_back({ name, id });
    return id;
}

int GLShaderProgram::get_uniform_location(const char* name) const
{
    assert(m_id > 0);

    if (m_id <= 0)
        // Shader program not loaded. This should not happen.
        return -1;

    auto it = std::find_if(m_uniform_location_cache.begin(), m_uniform_location_cache.end(), [name](const auto &p) { return p.first == name; });
    if (it != m_uniform_location_cache.end())
        // Uniform ID cached.
        return it->second;

    int id = ::glGetUniformLocation(m_id, name);
    const_cast<GLShaderProgram*>(this)->m_uniform_location_cache.push_back({ name, id });
    return id;
}

} // namespace Slic3r
