#include "IconManager.hpp"
#include <cmath>
#include <numeric>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include "libslic3r/Utils.hpp" // ScopeGuard   

#include "3DScene.hpp" // glsafe
#include "GL/glew.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "imgui/imstb_rectpack.h" // distribute rectangles

using namespace Slic3r::GUI;

namespace priv {
// set shared pointer to point on bad texture
static void clear(IconManager::Icons &icons);
static const std::vector<std::pair<int, bool>>& get_states(IconManager::RasterType type);
static void draw_transparent_icon(const IconManager::Icon &icon); // only help function
}

IconManager::~IconManager() {
	priv::clear(m_icons);
	// release opengl texture is made in ~GLTexture()

    if (m_id != 0)
        glsafe(::glDeleteTextures(1, &m_id));
}

namespace {
NSVGimage *parse_file(const char * filepath) {
    FILE *fp = boost::nowide::fopen(filepath, "rb");
    assert(fp != nullptr);
    if (fp == nullptr)
        return nullptr;

    Slic3r::ScopeGuard sg([fp]() { fclose(fp); });

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Note: +1 is for null termination
    auto data_ptr = std::make_unique<char[]>(size+1);
    data_ptr[size] = '\0'; // Must be null terminated.

    size_t readed_size = fread(data_ptr.get(), 1, size, fp);
    assert(readed_size == size);
    if (readed_size != size)
        return nullptr;

    return nsvgParse(data_ptr.get(), "px", 96.0f);
}

void subdata(unsigned char *data, size_t data_stride, const std::vector<unsigned char> &data2, size_t data2_row) {
    assert(data_stride >= data2_row);
    for (size_t data2_offset = 0, data_offset = 0; 
        data2_offset < data2.size();
        data2_offset += data2_row, data_offset += data_stride)   
        ::memcpy((void *)(data + data_offset), (const void *)(data2.data() + data2_offset), data2_row);
}
}

IconManager::Icons IconManager::init(const InitTypes &input) 
{
    assert(!input.empty());
    if (input.empty())
        return {};

    // TODO: remove in future
    if (m_id != 0) {
        glsafe(::glDeleteTextures(1, &m_id));
        m_id = 0;
    }

    int total_surface = 0;
    for (const InitType &i : input)
        total_surface += i.size.x * i.size.y;
    const int surface_sqrt = (int)sqrt((float)total_surface) + 1;

    // Start packing
    // Pack our extra data rectangles first, so it will be on the upper-left corner of our texture (UV will have small values).
    const int TEX_HEIGHT_MAX = 1024 * 32;
    int width = (surface_sqrt >= 4096 * 0.7f) ? 4096 : (surface_sqrt >= 2048 * 0.7f) ? 2048 : (surface_sqrt >= 1024 * 0.7f) ? 1024 : 512;

    int num_nodes = width;
    std::vector<stbrp_node> nodes(num_nodes);
    stbrp_context context;
    stbrp_init_target(&context, width, TEX_HEIGHT_MAX, nodes.data(), num_nodes);

    ImVector<stbrp_rect> pack_rects;
    pack_rects.resize(input.size());
    memset(pack_rects.Data, 0, (size_t) pack_rects.size_in_bytes());
    for (size_t i = 0; i < input.size(); i++) {
        const ImVec2 &size = input[i].size;
        assert(size.x > 1);
        assert(size.y > 1);
        pack_rects[i].w = size.x;
        pack_rects[i].h = size.y;
    }
    int pack_rects_res = stbrp_pack_rects(&context, &pack_rects[0], pack_rects.Size);
    assert(pack_rects_res == 1);
    if (pack_rects_res != 1)
        return {};

    ImVec2 tex_size(width, width);
    for (const stbrp_rect &rect : pack_rects) {
        float x = rect.x + rect.w;
        float y = rect.y + rect.h;
        if(x > tex_size.x) tex_size.x = x;
        if(y > tex_size.y) tex_size.y = y;
    }
    
    Icons result(input.size());
    for (int i = 0; i < pack_rects.Size; i++) {
        const stbrp_rect &rect = pack_rects[i];
        assert(rect.was_packed);
        if (!rect.was_packed)
            return {};

        ImVec2 tl(rect.x / tex_size.x, rect.y / tex_size.y);
        ImVec2 br((rect.x + rect.w) / tex_size.x, (rect.y + rect.h) / tex_size.y);

        assert(input[i].size.x == rect.w);
        assert(input[i].size.y == rect.h);
        Icon icon = {input[i].size, tl, br};
        result[i] = std::make_shared<Icon>(std::move(icon));
    }
        
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    assert(rast != nullptr);
    if (rast == nullptr)
        return {};
    ScopeGuard sg_rast([rast]() { ::nsvgDeleteRasterizer(rast); });

    int channels = 4;
    int n_pixels = tex_size.x * tex_size.y;
    // store data for whole texture
    std::vector<unsigned char> data(n_pixels * channels, {0});

    // initialize original index locations
    std::vector<size_t> idx(input.size());
    std::iota(idx.begin(), idx.end(), 0);

    // Group same filename by sort inputs
    // sort indexes based on comparing values in input
    std::sort(idx.begin(), idx.end(), [&input](size_t i1, size_t i2) { return input[i1].filepath < input[i2].filepath; });
    for (size_t j: idx) {
        const InitType &i = input[j];
        if (i.filepath.empty())
            continue; // no file path only reservation of space for texture
        assert(boost::filesystem::exists(i.filepath));
        if (!boost::filesystem::exists(i.filepath))
            continue;
        assert(boost::algorithm::iends_with(i.filepath, ".svg"));
        if (!boost::algorithm::iends_with(i.filepath, ".svg"))
            continue;

        NSVGimage *image = parse_file(i.filepath.c_str());
        assert(image != nullptr);
        if (image == nullptr)
            return {};

        ScopeGuard sg_image([image]() { ::nsvgDelete(image); });

        float svg_scale = i.size.y / image->height;
        // scale should be same in both directions
        assert(is_approx(svg_scale, i.size.y / image->width));
                
        const stbrp_rect &rect = pack_rects[j];
        int n_pixels = rect.w * rect.h;
        std::vector<unsigned char> icon_data(n_pixels * channels, {0});
        ::nsvgRasterize(rast, image, 0, 0, svg_scale, icon_data.data(), i.size.x, i.size.y, i.size.x * channels);
        
        // makes white or gray only data in icon
        if (i.type == RasterType::white_only_data || 
            i.type == RasterType::gray_only_data) {
            unsigned char value = (i.type == RasterType::white_only_data) ? 255 : 127;
            for (size_t k = 0; k < icon_data.size(); k += channels)
                if (icon_data[k] != 0 || icon_data[k + 1] != 0 || icon_data[k + 2] != 0) {
                    icon_data[k]     = value;
                    icon_data[k + 1] = value;
                    icon_data[k + 2] = value;
                }
        }

        int start_offset = (rect.y*tex_size.x + rect.x) * channels;
        int data_stride = tex_size.x * channels;
        subdata(data.data() + start_offset, data_stride, icon_data, rect.w * channels);
    }

    if (m_id != 0) 
        glsafe(::glDeleteTextures(1, &m_id));

    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) m_id));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) tex_size.x, (GLsizei) tex_size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE,  (const void*) data.data()));    

    // bind no texture
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    for (const auto &i : result)
        i->tex_id = m_id;
    return result;
}

std::vector<IconManager::Icons> IconManager::init(const std::vector<std::string> &file_paths, const ImVec2 &size, RasterType type)
{
    assert(!file_paths.empty());
    assert(size.x >= 1);
    assert(size.x < 256*16);

    // TODO: remove in future
    if (!m_icons.empty()) {
        // not first initialization
        priv::clear(m_icons);
        m_icons.clear();
        m_icons_texture.reset();
    }

    // only rectangle are supported
    assert(size.x == size.y);
    // no subpixel supported
    unsigned int width = static_cast<unsigned int>(std::abs(std::round(size.x)));
    assert(size.x == static_cast<float>(width));

    // state order has to match the enum IconState
    const auto& states = priv::get_states(type);
        
    bool compress  = false;
    bool is_loaded = m_icons_texture.load_from_svg_files_as_sprites_array(file_paths, states, width, compress);
    if (!is_loaded || (size_t) m_icons_texture.get_width() < (states.size() * width) ||
        (size_t) m_icons_texture.get_height() < (file_paths.size() * width)) {
        // bad load of icons, but all usage of m_icons_texture check that texture is initialized
        assert(false);
        m_icons_texture.reset();
        return {};
    }

    unsigned count_files = file_paths.size();
    // count icons per file
    unsigned count = states.size();
    // create result
    std::vector<Icons> result;
    result.reserve(count_files);

    Icon def_icon;
    def_icon.tex_id = m_icons_texture.get_id();
    def_icon.size   = size;

    // float beacouse of dividing
    float tex_height = static_cast<float>(m_icons_texture.get_height());
    float tex_width = static_cast<float>(m_icons_texture.get_width());

    //for (const auto &f: file_paths) {
    for (unsigned f = 0; f < count_files; ++f) {
        // NOTE: there are space between icons
        unsigned start_y = static_cast<unsigned>(f) * (width + 1) + 1;
        float y1 = start_y / tex_height;
        float y2 = (start_y + width) / tex_height;
        Icons file_icons;
        file_icons.reserve(count);
        //for (const auto &s : states) {
        for (unsigned j = 0; j < count; ++j) {
            auto icon = std::make_shared<Icon>(def_icon);
            // NOTE: there are space between icons
            unsigned start_x = static_cast<unsigned>(j) * (width + 1) + 1;
            float x1 = start_x / tex_width;
            float x2 = (start_x + width) / tex_width;
            icon->tl = ImVec2(x1, y1);
            icon->br = ImVec2(x2, y2);
            file_icons.push_back(icon);
            m_icons.push_back(std::move(icon));
        }
        result.emplace_back(std::move(file_icons));
    }
	return result;
}

void IconManager::release() {
	BOOST_LOG_TRIVIAL(error) << "Not implemented yet";
}

void priv::clear(IconManager::Icons &icons) {
    std::string message;
	for (auto &icon : icons) {
		// Exist more than this instance of shared ptr?
        long count = icon.use_count();
        if (count != 1) {
			// in existing icon change texture to non existing one
            icon->tex_id = 0;

            std::string descr = 
				((count > 2) ? (std::to_string(count - 1) + "x") : "") + // count
				std::to_string(icon->size.x) + "x" + std::to_string(icon->size.y); // resolution
            if (message.empty())
                message = descr;
            else
                message += ", " + descr;
		}
	}

    if (!message.empty())
		BOOST_LOG_TRIVIAL(warning) << "There is still used icons(" << message << ").";
}

const std::vector<std::pair<int, bool>> &priv::get_states(IconManager::RasterType type) {
    static std::vector<std::pair<int, bool>> color = {std::make_pair(0, false)};
    static std::vector<std::pair<int, bool>> white = {std::make_pair(1, false)};
    static std::vector<std::pair<int, bool>> gray = {std::make_pair(2, false)};
    static std::vector<std::pair<int, bool>> color_wite_gray = {
        std::make_pair(1, false), // Activable
        std::make_pair(0, false), // Hovered
        std::make_pair(2, false)  // Disabled
    };

    switch (type) {
    case IconManager::RasterType::color: return color;
    case IconManager::RasterType::white_only_data: return white;
    case IconManager::RasterType::gray_only_data: return gray;
    case IconManager::RasterType::color_wite_gray: return color_wite_gray;
    default: return color;
    }
}

void priv::draw_transparent_icon(const IconManager::Icon &icon)
{
    // Check input
    if (!icon.is_valid()) {
        assert(false);
        BOOST_LOG_TRIVIAL(warning) << "Drawing invalid Icon.";
        ImGui::Text("?");
        return;
    }

    // size UV texture coors [in texture ratio]
    ImVec2 size_uv(icon.br.x - icon.tl.x, icon.br.y - icon.tl.y);
    ImVec2 one_px(size_uv.x / icon.size.x, size_uv.y / icon.size.y);

    // use top left corner of first icon
    IconManager::Icon icon_px = icon; // copy
    // reduce uv coors to one pixel
    icon_px.tl = ImVec2(0, 0);
    icon_px.br = one_px;
    draw(icon_px);
}

#include "imgui/imgui_internal.h" //ImGuiWindow
namespace Slic3r::GUI {

void draw(const IconManager::Icon &icon, const ImVec2 &size, const ImVec4 &tint_col, const ImVec4 &border_col)
{
    // Check input
    if (!icon.is_valid()) {
        assert(false);
        BOOST_LOG_TRIVIAL(warning) << "Drawing invalid Icon.";
        ImGui::Text("?");
        return;
    }
    ImTextureID id = (void *)static_cast<intptr_t>(icon.tex_id);
    const ImVec2 &s  = (size.x < 1 || size.y < 1) ? icon.size : size;

    // Orca: Align icon center vertically
    ImGuiWindow  *window      = ImGui::GetCurrentWindow();
    ImGuiContext &g           = *GImGui;
    float         cursor_y    = window->DC.CursorPos.y;
    float         line_height = ImGui::GetTextLineHeight() + g.Style.FramePadding.y * 2;
    int           offset_y    = (line_height - s.y) / 2; // Make sure its int otherwise it will be pixelated
    window->DC.CursorPos.y += offset_y;

    ImGui::Image(id, s, icon.tl, icon.br, tint_col, border_col);

    // Reset offset
    window->DC.CursorPosPrevLine.y = cursor_y;
}

bool clickable(const IconManager::Icon &icon, const IconManager::Icon &icon_hover)
{
    // check of hover
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    float cursor_x = ImGui::GetCursorPosX()
        - window->DC.GroupOffset.x 
        - window->DC.ColumnsOffset.x;
    priv::draw_transparent_icon(icon);
    ImGui::SameLine(cursor_x);
    if (ImGui::IsItemHovered()) {
        // redraw image
        draw(icon_hover);
    } else {
        // redraw normal image
        draw(icon);
    }
    return ImGui::IsItemClicked();
}

bool button(const IconManager::Icon &activ, const IconManager::Icon &hover, const IconManager::Icon &disable, bool disabled)
{
    if (disabled) {
        draw(disable);
        return false;
    }
    return clickable(activ, hover);
}

} // namespace Slic3r::GUI
