#include "IMToolbar.hpp"

#include "3DScene.hpp"
#include <GL/glew.h>
#include <imgui/imgui_internal.h>
#include <imgui/imgui.h>

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "ImGuiWrapper.hpp"

namespace Slic3r {
namespace GUI {
IMToolbarItem::~IMToolbarItem()
{
    GLuint  id = (GLuint)(int64_t)texture_id;
    if (id != 0)
        glsafe(::glDeleteTextures(1, &id));
}


bool IMToolbarItem::generate_texture()
{
    GLint last_texture;
    unsigned m_image_texture{ 0 };
    unsigned char* pixels = (unsigned char*)(&image_data[0]);

    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    glsafe(::glGenTextures(1, &m_image_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_image_texture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));

    // Store our identifier
    texture_id = (ImTextureID)(intptr_t)m_image_texture;

    // Restore state
    glsafe(::glBindTexture(GL_TEXTURE_2D, last_texture));

    return true;
}

void IMToolbar::del_all_item()
{
    for (int i = 0; i < m_items.size(); i++) {
        delete m_items[i];
        m_items[i] = nullptr;
    }
    m_items.clear();
}

void IMToolbar::del_stats_item()
{
    delete m_all_plates_stats_item;
    m_all_plates_stats_item = nullptr;
}

void IMToolbar::set_enabled(bool enable)
{
    m_enabled = enable;
    if (!m_enabled)
        is_render_finish = false;
}

bool IMReturnToolbar::init()
{
    bool compress = false;
    GLint last_texture;
    unsigned m_image_texture{ 0 };

    std::string path = resources_dir() + "/images/";
    std::string file_name;

    file_name = path + "assemble_return.svg";

    ThumbnailData data;
    if (!get_data_from_svg(file_name, 20, data))
        return false;

    unsigned char* pixels = (unsigned char*)(&data.pixels[0]);
    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    glsafe(::glGenTextures(1, &m_image_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_image_texture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    if (compress && GLEW_EXT_texture_compression_s3tc)
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, data.width, data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, data.width, data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));

    // Store our identifier
    texture_id = (ImTextureID)(intptr_t)m_image_texture;

    // Restore state
    glsafe(::glBindTexture(GL_TEXTURE_2D, last_texture));

    return true;
}

} // namespace GUI
} // namespace Slic3r
