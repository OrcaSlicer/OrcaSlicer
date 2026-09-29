#include "libslic3r/libslic3r.h"
#include "SceneCache.hpp"

#include "3DScene.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "GLTexture.hpp"
#include "GUI_App.hpp"

#include <glad/gl.h>

namespace Slic3r {
namespace GUI {

void SceneCache::capture(Key key)
{
    m_valid = false;

    if (wxGetApp().get_shader("flat_texture") == nullptr)
        return;

    GLTexture::copy_from_framebuffer(m_texture_id, m_texture_size, key.size[0], key.size[1], GL_NEAREST);

    m_key = std::move(key);
    m_valid = true;
}

void SceneCache::render(GLModel& quad)
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat_texture");

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_BLEND));

    shader->start_using();
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    shader->set_uniform("uniform_texture", 0);

    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_texture_id));
    quad.render();
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    shader->stop_using();

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
}

void SceneCache::reset()
{
    m_valid = false;
    if (m_texture_id != 0) {
        glsafe(::glDeleteTextures(1, &m_texture_id));
        m_texture_id = 0;
    }
    m_texture_size = { { 0, 0 } };
}

} // namespace GUI
} // namespace Slic3r
