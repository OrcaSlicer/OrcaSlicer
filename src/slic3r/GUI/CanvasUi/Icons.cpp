#include "Icons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui/imgui.h>

#include "Context.hpp"
#include "ImGuiBridge.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "Metrics.hpp"
#include "Tokens.hpp"
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

const char* icon_name(Icon icon)
{
    static constexpr std::array<const char*, size_t(Icon::Count)> names = {
        "home", "rotate_left", "rotate_right", "cube", "eye", "eye_off", "fit", "check", "chevrons_up", "chevrons_down", "sliders", "reset",
        "clock", "chevron_down", "triangle_alert", "info_circle", "arrow_right"};
    return names[size_t(icon)];
}

IconCache::IconCache() = default;

IconCache::~IconCache() = default;

unsigned int IconCache::texture(Icon icon, int size, float stroke)
{
    std::unique_ptr<GLTexture>& texture = m_textures[{icon, size, int(std::lround(stroke * 100.f))}];
    if (!texture) {
        texture = std::make_unique<GLTexture>();
        NSVGimage* image = nsvgParseFromFile(var(std::string(icon_name(icon)) + ".svg").c_str(), "px", 96.f);
        if (image == nullptr || image->width <= 0.f) {
            if (image != nullptr)
                nsvgDelete(image);
            return 0;
        }
        const float scale = float(size) / image->width;
        for (NSVGshape* shape = image->shapes; shape != nullptr; shape = shape->next)
            shape->strokeWidth = stroke / scale;
        std::vector<unsigned char> pixels(size_t(size) * size_t(size) * 4, 0);
        if (NSVGrasterizer* rasterizer = nsvgCreateRasterizer()) {
            nsvgRasterize(rasterizer, image, 0.f, 0.f, scale, pixels.data(), size, size, size * 4);
            nsvgDeleteRasterizer(rasterizer);
        }
        nsvgDelete(image);
        texture->load_from_raw_data(std::move(pixels), unsigned(size), unsigned(size), false, false);
    }
    return texture->get_id();
}

// Only draw_icon() reaches the context's icon cache.
struct IconAccess
{
    static IconCache* icons(const Context& context) { return context.m_icons; }
};

void draw_icon(ImDrawList& list, Icon icon, const ImVec2& center, float size, ImU32 color)
{
    IconCache* cache = IconAccess::icons(*Context::current());
    if (cache == nullptr)
        return;
    const int          px = std::max(1, int(std::lround(size)));
    const unsigned int id = cache->texture(icon, px, Context::current()->metrics().px(Stroke::md));
    if (id == 0)
        return;
    const ImVec2 min(std::round(center.x - .5f * float(px)), std::round(center.y - .5f * float(px)));
    list.AddImage(ImTextureID(intptr_t(id)), min, ImVec2(min.x + float(px), min.y + float(px)), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), color);
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
