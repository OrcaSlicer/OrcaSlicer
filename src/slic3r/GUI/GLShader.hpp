#ifndef slic3r_GLShader_hpp_
#define slic3r_GLShader_hpp_

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include "libslic3r/Point.hpp"

namespace Slic3r {

class ColorRGB;
class ColorRGBA;

class GLShaderProgram
{
public:
    enum class EShaderType
    {
        Vertex,
        Fragment,
        Geometry,
        TessEvaluation,
        TessControl,
        Compute,
        Count
    };

    typedef std::array<std::string, static_cast<size_t>(EShaderType::Count)> ShaderFilenames;
    typedef std::array<std::string, static_cast<size_t>(EShaderType::Count)> ShaderSources;

private:
    std::string m_name;
    unsigned int m_id{ 0 };
    std::vector<std::pair<std::string, int>> m_attrib_location_cache;
    std::vector<std::pair<std::string, int>> m_uniform_location_cache;
    // The shaders of a program whose build start_init_from_files() began and finish_init() has not checked.
    std::vector<std::pair<EShaderType, unsigned int>> m_pending_shaders;

    bool start_init_from_texts(const std::string& name, const ShaderSources& sources);
    // Checks the program start_init_from_texts() began, waiting for the driver if it is not done.
    bool finish_init();

public:
    ~GLShaderProgram();

    bool init_from_files(const std::string& name, const ShaderFilenames& filenames, const std::initializer_list<std::string_view> &defines = {});
    bool init_from_texts(const std::string& name, const ShaderSources& sources);
    // Begins building the program as init_from_files() does, without waiting for the driver, which may compile
    // in threads of its own (GL_ARB_parallel_shader_compile). The program is usable once poll_build() returns
    // true and get_id() is not 0.
    bool start_init_from_files(const std::string& name, const ShaderFilenames& filenames, const std::initializer_list<std::string_view> &defines = {});
    // True while the build start_init_from_files() began is not checked yet. Makes no OpenGL call.
    bool is_building() const { return !m_pending_shaders.empty(); }
    // Checks the build once the driver is done with it, without waiting for it where the driver can tell, and
    // returns whether it is checked. A program that failed to build has get_id() 0.
    bool poll_build();

    const std::string& get_name() const { return m_name; }
    unsigned int get_id() const { return m_id; }

    void start_using() const;
    void stop_using() const;

    void set_uniform(const char* name, int value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, bool value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, float value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, double value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 2>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 3>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 2>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 3>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<double, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const float* value, size_t size) const { set_uniform(get_uniform_location(name), value, size); }
    void set_uniform(const char* name, const Transform3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Transform3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix4f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix4d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec2f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec2d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const ColorRGB& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const ColorRGBA& value) const { set_uniform(get_uniform_location(name), value); }

    void set_uniform(int id, int value) const;
    void set_uniform(int id, bool value) const;
    void set_uniform(int id, float value) const;
    void set_uniform(int id, double value) const;
    void set_uniform(int id, const std::array<int, 2>& value) const;
    void set_uniform(int id, const std::array<int, 3>& value) const;
    void set_uniform(int id, const std::array<int, 4>& value) const;
    void set_uniform(int id, const std::array<float, 2>& value) const;
    void set_uniform(int id, const std::array<float, 3>& value) const;
    void set_uniform(int id, const std::array<float, 4>& value) const;
    void set_uniform(int id, const std::array<double, 4>& value) const;
    void set_uniform(int id, const float* value, size_t size) const;
    void set_uniform(int id, const Transform3f& value) const;
    void set_uniform(int id, const Transform3d& value) const;
    void set_uniform(int id, const Matrix3f& value) const;
    void set_uniform(int id, const Matrix3d& value) const;
    void set_uniform(int id, const Matrix4f& value) const;
    void set_uniform(int id, const Matrix4d& value) const;
    void set_uniform(int id, const Vec2f& value) const;
    void set_uniform(int id, const Vec2d& value) const;
    void set_uniform(int id, const Vec3f& value) const;
    void set_uniform(int id, const Vec3d& value) const;
    void set_uniform(int id, const ColorRGB& value) const;
    void set_uniform(int id, const ColorRGBA& value) const;

    // returns -1 if not found
    int get_attrib_location(const char* name) const;
    // returns -1 if not found
    int get_uniform_location(const char* name) const;
};

} // namespace Slic3r

#endif /* slic3r_GLShader_hpp_ */
