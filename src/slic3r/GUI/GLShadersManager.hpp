#ifndef slic3r_GLShadersManager_hpp_
#define slic3r_GLShadersManager_hpp_

#include "GLShader.hpp"

#include <utility>
#include <vector>
#include <string>
#include <memory>

namespace Slic3r {

class GLShadersManager
{
    std::vector<std::unique_ptr<GLShaderProgram>> m_shaders;

public:
    std::pair<bool, std::string> init();
    // call this method before to release the OpenGL context
    void shutdown();

    // returns nullptr if not found. The path tracer's programs may still be building, see GLShaderProgram::is_building().
    GLShaderProgram* get_shader(const std::string& shader_name);

    // returns currently active shader, nullptr if none
    GLShaderProgram* get_current_shader();
};

} // namespace Slic3r

#endif //  slic3r_GLShadersManager_hpp_
