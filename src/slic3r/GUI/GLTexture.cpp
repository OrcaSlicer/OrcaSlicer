//BBS:add i18n
#include "I18N.hpp"
//BBS: add fstream for debug output
//#include <fstream>

#include "libslic3r/libslic3r.h"
#include "GLTexture.hpp"

#include "3DScene.hpp"
#include "OpenGLManager.hpp"
#include "GUI_App.hpp"
#include "GLModel.hpp"

#include <GL/glew.h>

#include <wx/image.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <vector>
#include <algorithm>
#include <thread>
#include "FileHelp.hpp"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt/stb_dxt.h"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include "libslic3r/Utils.hpp"
#include "GUI_App.hpp"
#include <boost/log/trivial.hpp>
#include <wx/dcgraph.h>
namespace Slic3r {
namespace GUI {

void GLTexture::Compressor::reset()
{
	if (m_thread.joinable()) {
        m_abort_compressing = true;
		m_thread.join();
	    m_levels.clear();
	    m_num_levels_compressed = 0;
	    m_abort_compressing = false;
	}
	assert(m_levels.empty());
	assert(m_abort_compressing == false);
	assert(m_num_levels_compressed == 0);
}

void GLTexture::Compressor::start_compressing()
{
	// The worker thread should be stopped already.
	assert(! m_thread.joinable());
	assert(! m_levels.empty());
	assert(m_abort_compressing == false);
	assert(m_num_levels_compressed == 0);
	if (! m_levels.empty()) {
		std::thread thrd(&GLTexture::Compressor::compress, this);
    	m_thread = std::move(thrd);
    }
}

bool GLTexture::Compressor::unsent_compressed_data_available() const
{
	if (m_levels.empty())
		return false;
	// Querying the atomic m_num_levels_compressed value synchronizes processor caches, so that the data of m_levels modified by the worker thread are accessible to the calling thread.
	unsigned int num_compressed = m_num_levels_compressed;
	for (unsigned int i = 0; i < num_compressed; ++ i)
        if (! m_levels[i].sent_to_gpu && ! m_levels[i].compressed_data.empty())
            return true;
    return false;
}

void GLTexture::Compressor::send_compressed_data_to_gpu()
{
    // this method should be called inside the main thread of Slicer or a new OpenGL context (sharing resources) would be needed
	if (m_levels.empty())
		return;

    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_texture.m_id));
	// Querying the atomic m_num_levels_compressed value synchronizes processor caches, so that the dat of m_levels modified by the worker thread are accessible to the calling thread.
	int num_compressed = (int)m_num_levels_compressed;
    for (int i = 0; i < num_compressed; ++ i) {
        Level& level = m_levels[i];
        if (! level.sent_to_gpu && ! level.compressed_data.empty()) {
            glsafe(::glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)i, 0, 0, (GLsizei)level.w, (GLsizei)level.h, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)level.compressed_data.size(), (const GLvoid*)level.compressed_data.data()));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, i));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (i > 0) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR));
            level.sent_to_gpu = true;
            // we are done with the compressed data, we can discard it
            level.compressed_data.clear();
        }
    }
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    if (num_compressed == (int)m_levels.size())
        // Finalize the worker thread, close it.
    	this->reset();
}

void GLTexture::Compressor::compress()
{
    // reference: https://github.com/Cyan4973/RygsDXTc

    assert(m_num_levels_compressed == 0);
    assert(m_abort_compressing == false);

    for (Level& level : m_levels) {
        if (m_abort_compressing)
            break;

        // stb_dxt library, despite claiming that the needed size of the destination buffer is equal to (source buffer size)/4,
        // crashes if doing so, requiring a minimum of 64 bytes and up to a third of the source buffer size, so we set the destination buffer initial size to be half the source buffer size
        level.compressed_data = std::vector<unsigned char>(std::max((unsigned int)64, (unsigned int)level.src_data.size() / 2), 0);
        int compressed_size = 0;
        rygCompress(level.compressed_data.data(), level.src_data.data(), level.w, level.h, 1, compressed_size);
        level.compressed_data.resize(compressed_size);

        // we are done with the source data, we can discard it
        level.src_data.clear();
        ++ m_num_levels_compressed;
    }
}

GLTexture::Quad_UVs GLTexture::FullTextureUVs = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };

GLTexture::GLTexture()
    : m_compressor(*this)
{
}

GLTexture::~GLTexture()
{
    reset();
}

bool GLTexture::load_from_file(const std::string& filename, bool use_mipmaps, ECompressionType compression_type, bool apply_anisotropy)
{
    reset();

    if (!boost::filesystem::exists(filename))
        return false;

    if (boost::algorithm::iends_with(filename, ".png"))
        return load_from_png(filename, use_mipmaps, compression_type, apply_anisotropy);
    else
        return false;
}

bool GLTexture::load_from_svg_file(const std::string& filename, bool use_mipmaps, bool compress, bool apply_anisotropy, unsigned int max_size_px)
{
    reset();
    auto svg_file = filename;
    Utils::slash_to_back_slash(svg_file);
    if (!boost::filesystem::exists(svg_file))
        return false;

    if (boost::algorithm::iends_with(svg_file, ".svg"))
        return load_from_svg(svg_file, use_mipmaps, compress, apply_anisotropy, max_size_px);
    else
        return false;
}

bool GLTexture::load_from_raw_data(std::vector<unsigned char> data, unsigned int w, unsigned int h, bool apply_anisotropy)
{
    m_width = w;
    m_height = h;
    int n_pixels = m_width * m_height;
    if (n_pixels <= 0) {
        reset();
        return false;
    }

    // sends data to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_id));

    if (apply_anisotropy) {
        GLfloat max_anisotropy = OpenGLManager::get_gl_info().get_max_anisotropy();
        if (max_anisotropy > 1.0f)
            glsafe(::glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_anisotropy));
    }


    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));

    bool use_mipmaps = true;
    if (use_mipmaps) {
        // we manually generate mipmaps because glGenerateMipmap() function is not reliable on all graphics cards
        int lod_w = m_width;
        int lod_h = m_height;
        GLint level = 0;
        while (lod_w > 1 || lod_h > 1) {
            ++level;
            lod_w = std::max(lod_w / 2, 1);
            lod_h = std::max(lod_h / 2, 1);
            n_pixels = lod_w * lod_h;
            glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, (GLsizei)lod_w, (GLsizei)lod_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
        }

        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    }
    else {
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    }

    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

#if 0
    // debug output
    static int pass = 0;
    ++pass;

    wxImage output(m_width, m_height);
    output.InitAlpha();

    for (int h = 0; h < m_height; ++h) {
        int px_h = h * m_width;
        for (int w = 0; w < m_width; ++w) {
            int offset = (px_h + w) * 4;
            output.SetRGB(w, h, data.data()[offset + 0], data.data()[offset + 1], data.data()[offset + 2]);
            output.SetAlpha(w, h, data.data()[offset + 3]);
        }
    }

    std::string out_filename = resources_dir() + "/images/test_" + std::to_string(pass) + ".png";
    output.SaveFile(out_filename, wxBITMAP_TYPE_PNG);
#endif // 0
    return true;
}


bool GLTexture::load_from_svg_files_as_sprites_array(const std::vector<std::string>& filenames, const std::vector<std::pair<int, bool>>& states, unsigned int sprite_size_px, bool compress)
{
    reset();

    if (filenames.empty() || states.empty() || sprite_size_px == 0)
        return false;

    bool dark_mode = wxGetApp().app_config->get("dark_color_mode") == "1";

    // every tile needs to have a 1px border around it to avoid artifacts when linear sampling on its edges
    unsigned int sprite_size_px_ex = sprite_size_px + 1;

    m_width = 1 + (int)(sprite_size_px_ex * states.size());
    m_height = 1 + (int)(sprite_size_px_ex * filenames.size());

    int n_pixels = m_width * m_height;
    int sprite_n_pixels = sprite_size_px_ex * sprite_size_px_ex;
    int sprite_stride = sprite_size_px_ex * 4;
    int sprite_bytes = sprite_n_pixels * 4;

    if (n_pixels <= 0) {
        reset();
        return false;
    }

    std::vector<unsigned char> data(n_pixels * 4, 0);
    std::vector<unsigned char> sprite_data(sprite_bytes, 0);
    std::vector<unsigned char> sprite_white_only_data(sprite_bytes, 0); // normal
    std::vector<unsigned char> sprite_gray_only_data(sprite_bytes, 0); // disable
    std::vector<unsigned char> output_data(sprite_bytes, 0);

    //BBS
    std::vector<unsigned char> pressed_data(sprite_bytes, 0); // (gizmo) pressed
    std::vector<unsigned char> disable_data(sprite_bytes, 0);
    std::vector<unsigned char> hover_data(sprite_bytes, 0); // hover

    const unsigned char pressed_color[3] = {255, 255, 255};
    const unsigned char hover_color[3] = {255, 255, 255};
    const unsigned char normal_color[3] = {43, 52, 54};
    const unsigned char disable_color[3] = {200, 200, 200};
    const unsigned char pressed_color_dark[3] = {60, 60, 65};
    const unsigned char hover_color_dark[3] = {60, 60, 65};
    const unsigned char normal_color_dark[3] = {182, 182, 182};
    const unsigned char disable_color_dark[3] = {76, 76, 85};

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast == nullptr) {
        reset();
        return false;
    }

    int sprite_id = -1;
    for (const std::string& filename : filenames) {
        ++sprite_id;

        if (!boost::filesystem::exists(filename))
            continue;

        if (!boost::algorithm::iends_with(filename, ".svg"))
            continue;

        NSVGimage* image = nsvgParseFromFile(filename.c_str(), "px", 96.0f);
        if (image == nullptr)
            continue;

        float scale = (float)sprite_size_px / std::max(image->width, image->height);

        // offset by 1 to leave the first pixel empty (both in x and y)
        nsvgRasterize(rast, image, 1, 1, scale, sprite_data.data(), sprite_size_px, sprite_size_px, sprite_stride);

        ::memcpy((void*)pressed_data.data(), (const void*)sprite_data.data(), sprite_bytes);
        for (int i = 0; i < sprite_n_pixels; ++i) {
            int offset = i * 4;
            if (pressed_data.data()[offset + 0] == 0 &&
                pressed_data.data()[offset + 1] == 0 &&
                pressed_data.data()[offset + 2] == 0) {
                hover_data.data()[offset + 0] = dark_mode ? pressed_color_dark[0] : pressed_color[0];
                hover_data.data()[offset + 0] = dark_mode ? pressed_color_dark[1] : pressed_color[1];
                hover_data.data()[offset + 0] = dark_mode ? pressed_color_dark[2] : pressed_color[2];
            }
        }

        ::memcpy((void*)disable_data.data(), (const void*)sprite_data.data(), sprite_bytes);
        for (int i = 0; i < sprite_n_pixels; ++i) {
            int offset = i * 4;
            if (disable_data.data()[offset] != 0)
                ::memset((void*)&disable_data.data()[offset], 200, 3);
        }

        ::memcpy((void*)hover_data.data(), (const void*)sprite_data.data(), sprite_bytes);
        for (int i = 0; i < sprite_n_pixels; ++i) {
            int offset = i * 4;
            if (hover_data.data()[offset + 0] == 0 &&
                hover_data.data()[offset + 1] == 0 &&
                hover_data.data()[offset + 2] == 0)
            {
                hover_data.data()[offset + 0] = dark_mode ? hover_color_dark[0] : hover_color[0];
                hover_data.data()[offset + 1] = dark_mode ? hover_color_dark[1] : hover_color[1];
                hover_data.data()[offset + 2] = dark_mode ? hover_color_dark[2] : hover_color[2];
            }
        }

        ::memcpy((void*)sprite_white_only_data.data(), (const void*)sprite_data.data(), sprite_bytes);
        for (int i = 0; i < sprite_n_pixels; ++i) {
            int offset = i * 4;
            if (sprite_white_only_data.data()[offset + 0] != 0 ||
                sprite_white_only_data.data()[offset + 1] != 0 ||
                sprite_white_only_data.data()[offset + 2] != 0) {
                sprite_white_only_data.data()[offset + 0] = dark_mode ? normal_color_dark[0] : normal_color[0];
                sprite_white_only_data.data()[offset + 1] = dark_mode ? normal_color_dark[1] : normal_color[1];
                sprite_white_only_data.data()[offset + 2] = dark_mode ? normal_color_dark[2] : normal_color[2];
            }
        }

        ::memcpy((void*)sprite_gray_only_data.data(), (const void*)sprite_data.data(), sprite_bytes);
        for (int i = 0; i < sprite_n_pixels; ++i) {
            int offset = i * 4;
            if (sprite_gray_only_data.data()[offset + 0] != 0 ||
                sprite_gray_only_data.data()[offset + 1] != 0 ||
                sprite_gray_only_data.data()[offset + 2] != 0) {
                sprite_gray_only_data.data()[offset + 0] = dark_mode ? disable_color_dark[0] : disable_color[0];
                sprite_gray_only_data.data()[offset + 1] = dark_mode ? disable_color_dark[1] : disable_color[1];
                sprite_gray_only_data.data()[offset + 2] = dark_mode ? disable_color_dark[2] : disable_color[2];
            }
        }


        int sprite_offset_px = sprite_id * (int)sprite_size_px_ex * m_width;
        int state_id = -1;
        for (const std::pair<int, bool>& state : states) {
            ++state_id;

            // select the sprite variant
            std::vector<unsigned char>* src = nullptr;
            switch (state.first)
            {
            case 1: { src = &sprite_white_only_data; break; }
            case 2: { src = &sprite_gray_only_data; break; }
            default: { src = &hover_data; break; }
            }

            // applies background, if needed
            if (state.second) {
                src = &pressed_data;
            }

            ::memcpy((void*)output_data.data(), (const void*)src->data(), sprite_bytes);

            //BBS use BBS pressed style
            //if (state.second) {
            //    float inv_255 = 1.0f / 255.0f;
            //    // offset by 1 to leave the first pixel empty (both in x and y)
            //    for (unsigned int r = 1; r <= sprite_size_px; ++r) {
            //        unsigned int offset_r = r * sprite_size_px_ex;
            //        for (unsigned int c = 1; c <= sprite_size_px; ++c) {
            //            unsigned int offset = (offset_r + c) * 4;
            //            float alpha = (float)output_data.data()[offset + 3] * inv_255;
            //            output_data.data()[offset + 0] = (unsigned char)(output_data.data()[offset + 0] * alpha);
            //            output_data.data()[offset + 1] = (unsigned char)(output_data.data()[offset + 1] * alpha);
            //            output_data.data()[offset + 2] = (unsigned char)(output_data.data()[offset + 2] * alpha);
            //            output_data.data()[offset + 3] = (unsigned char)(128 * (1.0f - alpha) + output_data.data()[offset + 3] * alpha);
            //        }
            //    }
            //}

            int state_offset_px = sprite_offset_px + state_id * sprite_size_px_ex;
            for (int j = 0; j < (int)sprite_size_px_ex; ++j) {
                ::memcpy((void*)&data.data()[(state_offset_px + j * m_width) * 4], (const void*)&output_data.data()[j * sprite_stride], sprite_stride);
            }
        }

        nsvgDelete(image);
    }

    nsvgDeleteRasterizer(rast);

    // sends data to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_id));
    if (compress && OpenGLManager::are_compressed_textures_supported())
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    m_source = filenames.front();

#if 0
    // debug output
    static int pass = 0;
    ++pass;

    wxImage output(m_width, m_height);
    output.InitAlpha();

    for (int h = 0; h < m_height; ++h) {
        int px_h = h * m_width;
        for (int w = 0; w < m_width; ++w) {
            int offset = (px_h + w) * 4;
            output.SetRGB(w, h, data.data()[offset + 0], data.data()[offset + 1], data.data()[offset + 2]);
            output.SetAlpha(w, h, data.data()[offset + 3]);
        }
    }

    std::string out_filename = resources_dir() + "/images/test_" + std::to_string(pass) + ".png";
    output.SaveFile(out_filename, wxBITMAP_TYPE_PNG);
#endif // 0

    return true;
}

void GLTexture::reset()
{
    if (m_id != 0)
        glsafe(::glDeleteTextures(1, &m_id));

    m_id = 0;
    m_width = 0;
    m_height = 0;
    m_source = "";
    m_compressor.reset();

    //BBS: GUI refactor
    m_original_width = m_original_height = 0;
}

bool GLTexture::generate_from_text_string(const std::string& text_str, wxFont &font, wxColor background, wxColor foreground)
{
    return generate_from_text(text_str, font, background, foreground);
}

bool GLTexture::generate_from_text(const std::string &text_str, wxFont &font, wxColor background, wxColor foreground)
{
    if (text_str.empty())
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":no text string, should not happen\n";
        return false;
    }

    wxString msg = from_u8(text_str);
    wxMemoryDC memDC;

    memDC.SetFont(font);

    // calculates texture size
    wxCoord w, h;
    memDC.GetMultiLineTextExtent(msg, &w, &h);

    m_original_width = (int)w;
    m_original_height = (int)h;
    m_width = (int)next_highest_power_of_2((uint32_t)w);
    m_height = (int)next_highest_power_of_2((uint32_t)h);

    // generates bitmap
    wxBitmap bitmap(m_width, m_height);

    memDC.SelectObject(bitmap);
    memDC.SetBackground(wxBrush(background));
    memDC.Clear();

    // draw message
    memDC.SetTextForeground(*wxWHITE);
    memDC.DrawLabel(msg, wxRect(0, 0, m_original_width, m_original_height), wxALIGN_CENTER);

    memDC.SelectObject(wxNullBitmap);

    // Convert the bitmap into a linear data ready to be loaded into the GPU.
    wxImage image = bitmap.ConvertToImage();

    // prepare buffer
    std::vector<unsigned char> data(4 * m_width * m_height, 0);
    const unsigned char* src = image.GetData();
    /* for debug use
    std::ofstream fout;
    fout.open(text_str+std::to_string(m_width)+"_"+std::to_string(m_height)+".rgb", std::ios::out);
    fout.write((const char*)src, 3 * m_width * m_height);
    fout.close();*/
    for (int h = 0; h < m_height; ++h) {
        unsigned char* dst = data.data() + 4 * h * m_width;
        for (int w = 0; w < m_width; ++w) {
            *dst++ = foreground.Red();
            *dst++ = foreground.Green();
            *dst++ = foreground.Blue();
            *dst++ = (unsigned char)std::min<int>(255, *src);
            src += 3;
        }
    }

    // sends buffer to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)m_id));
    if (OpenGLManager::are_compressed_textures_supported())
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    return true;
}

bool GLTexture::generate_texture_from_text(const std::string& text_str, wxFont& font, int& ww, int& hh, int& hl, wxColor background, wxColor foreground)
{
    if (text_str.empty())
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":no text string, should not happen\n";
        return false;
    }

    wxString msg = _(text_str);
    wxMemoryDC memDC;

    memDC.SetFont(font);

    // calculates texture size
    wxCoord w, h, ll;
    wxClientDC dc(wxGetApp().GetTopWindow());
    dc.SetFont(font);
    dc.GetMultiLineTextExtent(msg, &w, &h, &ll, &font);


    m_original_width = (int)w;
    m_original_height = (int)h;
    m_width = (int)next_highest_power_of_2((uint32_t)w);
    m_height = (int)next_highest_power_of_2((uint32_t)h);
    ww = m_width;
    hh = m_height;
    hl = ll;
    // generates bitmap
    wxBitmap bitmap(m_width, m_height);

    memDC.SelectObject(bitmap);
    memDC.SetBackground(wxBrush(background));
    memDC.Clear();

    // draw message
    memDC.SetTextForeground(*wxWHITE);
    memDC.DrawLabel(msg, wxRect(0, 0, m_width, m_height), wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

    memDC.SelectObject(wxNullBitmap);

    // Convert the bitmap into a linear data ready to be loaded into the GPU.
    wxImage image = bitmap.ConvertToImage();

    // prepare buffer
    std::vector<unsigned char> data(4 * m_width * m_height, 0);
    const unsigned char* src = image.GetData();
    /* for debug use
    std::ofstream fout;
    fout.open(text_str+std::to_string(m_width)+"_"+std::to_string(m_height)+".rgb", std::ios::out);
    fout.write((const char*)src, 3 * m_width * m_height);
    fout.close();*/
    bool found = false;
    for (int h = 0; h < m_height; ++h) {
        unsigned char* dst = data.data() + 4 * h * m_width;
        for (int w = 0; w < m_width; ++w) {
            *dst++ = foreground.Red();
            *dst++ = foreground.Green();
            *dst++ = foreground.Blue();
            *dst++ = (unsigned char)std::min<int>(255, *src);
            if ((*src) != background.Red() && !found) {
                found = true;
                if (m_height - h < font.GetPointSize())
                    return false;
            }
            src += 3;
        }
    }
    if (!found)
        return false;

    found = false;
    src -= 3;
    for (int h = m_height; h > 0; --h) {
        for (int w = m_width; w > 0; --w) {
            if ((*src) != background.Red() && !found) {
                found = true;
                if (h < font.GetPointSize())
                    return false;
            }
            src -= 3;
        }
    }
    if (!found)
        return false;

    // sends buffer to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)m_id));
    if (OpenGLManager::are_compressed_textures_supported())
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    return true;
}

void GLTexture::render_texture(unsigned int tex_id, float left, float right, float bottom, float top)
{
    render_sub_texture(tex_id, left, right, bottom, top, FullTextureUVs);
}

void GLTexture::render_sub_texture(unsigned int tex_id, float left, float right, float bottom, float top, const GLTexture::Quad_UVs& uvs)
{
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    glsafe(::glEnable(GL_TEXTURE_2D));
    glsafe(::glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE));

    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)tex_id));

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2T2 };
    init_data.reserve_vertices(4);
    init_data.reserve_indices(6);

    // vertices
    init_data.add_vertex(Vec2f(left, bottom),  Vec2f(uvs.left_bottom.u, uvs.left_bottom.v));
    init_data.add_vertex(Vec2f(right, bottom), Vec2f(uvs.right_bottom.u, uvs.right_bottom.v));
    init_data.add_vertex(Vec2f(right, top),    Vec2f(uvs.right_top.u, uvs.right_top.v));
    init_data.add_vertex(Vec2f(left, top),     Vec2f(uvs.left_top.u, uvs.left_top.v));

    // indices
    init_data.add_triangle(0, 1, 2);
    init_data.add_triangle(2, 3, 0);

    GLModel model;
    model.init_from(std::move(init_data));

    GLShaderProgram* shader = wxGetApp().get_shader("flat_texture");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        model.render();
        shader->stop_using();
    }

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    glsafe(::glDisable(GL_TEXTURE_2D));
    glsafe(::glDisable(GL_BLEND));
}

static bool to_squared_power_of_two(const std::string& filename, int max_size_px, int& w, int& h)
{
    auto is_power_of_two = [](int v) { return v != 0 && (v & (v - 1)) == 0; };
    auto upper_power_of_two = [](int v) { v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v++; return v; };

    int new_w = std::max(w, h);
    if (!is_power_of_two(new_w))
        new_w = upper_power_of_two(new_w);

    while (new_w > max_size_px) {
        new_w /= 2;
    }

    const int new_h = new_w;
    const bool ret = (new_w != w || new_h != h);
    w = new_w;
    h = new_h;
    return ret;
}

bool GLTexture::load_from_png(const std::string& filename, bool use_mipmaps, ECompressionType compression_type, bool apply_anisotropy)
{
    const bool compression_enabled = (compression_type != None) && OpenGLManager::are_compressed_textures_supported();

    // Load a PNG with an alpha channel.
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(filename.c_str()), wxBITMAP_TYPE_PNG)) {
        reset();
        return false;
    }

    m_width = image.GetWidth();
    m_height = image.GetHeight();

    bool requires_rescale = false;

    if (use_mipmaps && compression_enabled && OpenGLManager::force_power_of_two_textures()) {
        if (to_squared_power_of_two(boost::filesystem::path(filename).filename().string(), OpenGLManager::get_gl_info().get_max_tex_size(), m_width, m_height))
            requires_rescale = true;
    }

    if (compression_enabled && compression_type == MultiThreaded) {
        // the stb_dxt compression library seems to like only texture sizes which are a multiple of 4
        int width_rem = m_width % 4;
        int height_rem = m_height % 4;

        if (width_rem != 0) {
            m_width += (4 - width_rem);
            requires_rescale = true;
        }

        if (height_rem != 0) {
            m_height += (4 - height_rem);
            requires_rescale = true;
        }
    }

    if (requires_rescale)
        image = image.ResampleBicubic(m_width, m_height);

    int n_pixels = m_width * m_height;
    if (n_pixels <= 0) {
        reset();
        return false;
    }

    // Get RGB & alpha raw data from wxImage, pack them into an array.
    unsigned char* img_rgb = image.GetData();
    if (img_rgb == nullptr) {
        reset();
        return false;
    }

    unsigned char* img_alpha = image.GetAlpha();

    std::vector<unsigned char> data(n_pixels * 4, 0);
    for (int i = 0; i < n_pixels; ++i) {
        int data_id = i * 4;
        int img_id = i * 3;
        data[data_id + 0] = img_rgb[img_id + 0];
        data[data_id + 1] = img_rgb[img_id + 1];
        data[data_id + 2] = img_rgb[img_id + 2];
        data[data_id + 3] = (img_alpha != nullptr) ? img_alpha[i] : 255;
    }

    // sends data to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_id));

    if (apply_anisotropy) {
        GLfloat max_anisotropy = OpenGLManager::get_gl_info().get_max_anisotropy();
        if (max_anisotropy > 1.0f)
            glsafe(::glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_anisotropy));
    }

    if (compression_enabled) {
        if (compression_type == SingleThreaded)
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
        else {
            // initializes the texture on GPU
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
            // and send the uncompressed data to the compressor
            m_compressor.add_level((unsigned int)m_width, (unsigned int)m_height, data);
        }
    }
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));

    if (use_mipmaps) {
        // we manually generate mipmaps because glGenerateMipmap() function is not reliable on all graphics cards
        int lod_w = m_width;
        int lod_h = m_height;
        GLint level = 0;
        while (lod_w > 1 || lod_h > 1) {
            ++level;

            lod_w = std::max(lod_w / 2, 1);
            lod_h = std::max(lod_h / 2, 1);
            n_pixels = lod_w * lod_h;

            image = image.ResampleBicubic(lod_w, lod_h);

            data.resize(n_pixels * 4);

            img_rgb = image.GetData();
            img_alpha = image.GetAlpha();

            for (int i = 0; i < n_pixels; ++i) {
                int data_id = i * 4;
                int img_id = i * 3;
                data[data_id + 0] = img_rgb[img_id + 0];
                data[data_id + 1] = img_rgb[img_id + 1];
                data[data_id + 2] = img_rgb[img_id + 2];
                data[data_id + 3] = (img_alpha != nullptr) ? img_alpha[i] : 255;
            }

            if (compression_enabled) {
                if (compression_type == SingleThreaded)
                    glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
                else {
                    // initializes the texture on GPU
                    glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)lod_w, (GLsizei)lod_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
                    // and send the uncompressed data to the compressor
                    m_compressor.add_level((unsigned int)lod_w, (unsigned int)lod_h, data);
                }
            }
            else
                glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, (GLsizei)lod_w, (GLsizei)lod_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
        }

        if (!compression_enabled) {
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
        }
    }
    else {
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    }

    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    m_source = filename;

    if (compression_type == MultiThreaded)
        // start asynchronous compression
        m_compressor.start_compressing();

    return true;
}

bool GLTexture::load_from_svg(const std::string& filename, bool use_mipmaps, bool compress, bool apply_anisotropy, unsigned int max_size_px)
{
    const bool compression_enabled = compress && OpenGLManager::are_compressed_textures_supported();

    NSVGimage* image = nsvgParseFromFile(filename.c_str(), "px", 96.0f);
    if (image == nullptr) {
        reset();
        return false;
    }

    const float scale = (float)max_size_px / std::max(image->width, image->height);

    m_width = (int)(scale * image->width);
    m_height = (int)(scale * image->height);

    if (use_mipmaps && compression_enabled && OpenGLManager::force_power_of_two_textures())
        to_squared_power_of_two(boost::filesystem::path(filename).filename().string(), max_size_px, m_width, m_height);

    float scale_w = (float)m_width / image->width;
    float scale_h = (float)m_height / image->height;

    if (compression_enabled) {
        // the stb_dxt compression library seems to like only texture sizes which are a multiple of 4
        int width_rem = m_width % 4;
        int height_rem = m_height % 4;

        if (width_rem != 0)
            m_width += (4 - width_rem);

        if (height_rem != 0)
            m_height += (4 - height_rem);
    }

    const int n_pixels = m_width * m_height;

    if (n_pixels <= 0) {
        reset();
        nsvgDelete(image);
        return false;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast == nullptr) {
        nsvgDelete(image);
        reset();
        return false;
    }

    // creates the temporary buffer only once, with max size, and reuse it for all the levels, if generating mipmaps
    std::vector<unsigned char> data(n_pixels * 4, 0);
    nsvgRasterizeXY(rast, image, 0, 0, scale_w, scale_h, data.data(), m_width, m_height, m_width * 4);

    // sends data to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_id));

    if (apply_anisotropy) {
        GLfloat max_anisotropy = OpenGLManager::get_gl_info().get_max_anisotropy();
        if (max_anisotropy > 1.0f)
            glsafe(::glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_anisotropy));
    }

    if (compression_enabled) {
        // initializes the texture on GPU
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
        // and send the uncompressed data to the compressor
        m_compressor.add_level((unsigned int)m_width, (unsigned int)m_height, data);
    }
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));

    if (use_mipmaps) {
        // we manually generate mipmaps because glGenerateMipmap() function is not reliable on all graphics cards
        int lod_w = m_width;
        int lod_h = m_height;
        GLint level = 0;
        while (lod_w >= 4 && lod_h >= 4) {
            ++level;

            lod_w = std::max(lod_w / 2, 1);
            lod_h = std::max(lod_h / 2, 1);
            scale_w /= 2.0f;
            scale_h /= 2.0f;

            data.resize(lod_w * lod_h * 4);

            nsvgRasterizeXY(rast, image, 0, 0, scale_w, scale_h, data.data(), lod_w, lod_h, lod_w * 4);
            if (compression_enabled) {
                // initializes the texture on GPU
                glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)lod_w, (GLsizei)lod_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
                // and send the uncompressed data to the compressor
                m_compressor.add_level((unsigned int)lod_w, (unsigned int)lod_h, data);
            }
            else
                glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, (GLsizei)lod_w, (GLsizei)lod_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
        }

        if (!compression_enabled) {
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
        }
    }
    else {
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    }

    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    m_source = filename;

    if (compression_enabled)
        // start asynchronous compression
        m_compressor.start_compressing();

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    return true;
}

} // namespace GUI
} // namespace Slic3r
