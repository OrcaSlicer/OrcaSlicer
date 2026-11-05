#ifndef slic3r_GLGizmoTextureDisplacementInternal_hpp_
#define slic3r_GLGizmoTextureDisplacementInternal_hpp_

// Helpers shared between the translation units GLGizmoTextureDisplacement is defined across - see the
// comment at the top of GLGizmoTextureDisplacement.cpp. Nothing here belongs to the gizmo class, and
// nothing here is meant for any other file: this is the part of the single file's anonymous namespace
// that more than one of the pieces turned out to need, so it could not stay file-local to any of them.

#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <imgui.h>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::gizmo_td {

// The scrollbar of the panel's scrolling areas (the panel body and the texture library). ImGui's stock
// scrollbar is a wide, square-cornered slab in a tinted track - against the flat panel it reads as a raw
// widget bolted onto the edge - so it is slimmed to a rounded thumb over an invisible track. The child
// background goes transparent with it, so the area reads as part of the panel.
inline void push_slim_scrollbar_style(float width, bool dark)
{
    const ImVec4 grab = dark ? ImVec4(1.f, 1.f, 1.f, 0.26f) : ImVec4(0.f, 0.f, 0.f, 0.26f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, width);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.5f * width);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, grab);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(grab.x, grab.y, grab.z, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(grab.x, grab.y, grab.z, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
}

inline void pop_slim_scrollbar_style()
{
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
}

// Cuts a UTF-8 string down to `max_w` pixels, ending it in "..." when it had to be cut.
inline std::string ellipsize(std::string s, float max_w, float font_size = 0.f)
{
    const auto text_width = [&](const std::string &text) {
        return font_size > 0.f
                   ? ImGui::GetFont()->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), 0.f, text.c_str()).x
                   : ImGui::CalcTextSize(text.c_str()).x;
    };
    if (text_width(s) <= max_w)
        return s;
    while (!s.empty() && text_width(s + "...") > max_w) {
        unsigned char c;
        do { // a whole code point at a time
            c = static_cast<unsigned char>(s.back());
            s.pop_back();
        } while (!s.empty() && (c & 0xC0) == 0x80);
    }
    return s + "...";
}

// Uploads decoded 8-bit grayscale height data as an RGBA GPU texture for display in the panel.
// Shared by the per-layer thumbnail and the picker's library entries, which differ only in where
// their pixels came from.
//
// The source is downscaled to THUMBNAIL_MAX_PX first, with a box filter, and uploaded *without*
// mipmaps. Both halves of that matter, and together they are the fix for thumbnails rendering as
// garbage:
//   - GLTexture's mipmap levels are not real downscales (see load_from_raw_data()'s header) - every
//     level re-uploads the full-resolution buffer reinterpreted at a smaller size. A 512px texture
//     drawn into a ~48px row is minified ~10x, which is exactly the regime where OpenGL picks those
//     broken levels, so the thumbnail showed a scrambled crop of the image rather than the image.
//     Turning mipmaps off makes it sample level 0, which is the real picture.
//   - But level 0 at 512px sampled down to 48px with plain GL_LINEAR (a 2x2 tap) would alias badly
//     on exactly the high-frequency patterns these textures are (knurl, hexagons, weave). Box-
//     filtering down to roughly twice the displayed size first is what actually removes the
//     frequencies that would alias, and it cuts the VRAM these hold by ~16x as a bonus.
constexpr int THUMBNAIL_MAX_PX = 128;

// A mix is an interleave that only reads as its colour from a distance; up close it is stripes. So it
// is spent only where it beats the nearest single filament by this much (CIEDE2000). Two is about
// where a side-by-side difference stops being arguable; a margin of ten already turns most of a
// greyscale ramp - the shape a height texture actually traces - back into single filaments. The
// quantizer, the mix ranking and the shaded preview shader all apply it, so they agree on where a mix
// is used.
constexpr float PREFER_PURE_DE = 2.f;

// sRGB (0..1) <-> CIELAB, D65. Exactly what the preview shader's srgb_to_lab() computes, so the CPU
// quantizer, the mixed-palette entries and the per-fragment preview all match in the same space.
// Not slic3r/Utils/ColorSpaceConvert: its RGB2Lab wants 0..1 but its Lab2RGB hands back linear
// values on a 0..100 scale, and the earlier code fed the former 0..255 and divided the latter by 255 -
// self-consistent enough for the bake's nearest-entry search to rank sensibly, but the preview, which
// compares real Lab against those values, picked the darkest filament everywhere.
inline Vec3f srgb_to_lab(const Vec3f &c)
{
    const auto lin = [](float v) { return v > 0.04045f ? std::pow((v + 0.055f) / 1.055f, 2.4f) : v / 12.92f; };
    const float r = lin(c.x()), g = lin(c.y()), b = lin(c.z());
    const float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
    const float y = (0.2126f * r + 0.7152f * g + 0.0722f * b);
    const float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
    const auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    const float fx = f(x), fy = f(y), fz = f(z);
    return Vec3f(116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz));
}

inline std::unique_ptr<GLTexture> upload_height_thumbnail(const DecodedHeightTexture &decoded, int max_px = THUMBNAIL_MAX_PX)
{
    if (decoded.empty())
        return nullptr;

    // Preserve aspect; never upscale a texture that is already small.
    const int scale = std::max(1, (std::max(decoded.width, decoded.height) + max_px - 1) / max_px);
    const int w     = std::max(1, decoded.width / scale);
    const int h     = std::max(1, decoded.height / scale);

    // A colour texture is shown in colour, so it can be told apart from its grey neighbours in the picker; its
    // height is the luminance of those same colours, so nothing about the relief is hidden by that.
    const bool                 color = decoded.has_color();
    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Average the source block this destination pixel covers.
            const int x0 = x * decoded.width / w, x1 = std::max(x0 + 1, (x + 1) * decoded.width / w);
            const int y0 = y * decoded.height / h, y1 = std::max(y0 + 1, (y + 1) * decoded.height / h);
            unsigned int sum[3] = { 0, 0, 0 };
            unsigned int n      = 0;
            for (int sy = y0; sy < y1 && sy < decoded.height; ++sy)
                for (int sx = x0; sx < x1 && sx < decoded.width; ++sx, ++n) {
                    const size_t si = size_t(sy) * size_t(decoded.width) + size_t(sx);
                    for (int c = 0; c < 3; ++c)
                        sum[c] += color ? decoded.rgb[si * 3 + size_t(c)] : decoded.pixels[si];
                }

            const size_t di = (size_t(y) * size_t(w) + size_t(x)) * 4;
            for (int c = 0; c < 3; ++c)
                rgba[di + size_t(c)] = (n > 0) ? static_cast<unsigned char>(sum[c] / n) : 0;
            rgba[di + 3] = 255;
        }

    auto texture = std::make_unique<GLTexture>();
    if (!texture->load_from_raw_data(std::move(rgba), (unsigned int) w, (unsigned int) h, false, /* use_mipmaps */ false))
        return nullptr;
    return texture;
}

// The image the adjust panel edits. For a relief layer that is the layer's own texture; for a
// ChromaStack picture it is the picture, never `image_data`, which there is the height map the solve
// wrote. Adjusting that would be sharpening the answer rather than the question, and the next settle
// would overwrite it anyway - which is why these controls used to be switched off for a chroma card.
inline const std::shared_ptr<std::vector<unsigned char>> &adjust_image_of(const TextureDisplacementLayer &layer)
{
    return (layer.chroma_enabled && layer.chroma_source) ? layer.chroma_source : layer.image_data;
}

// The panel's own look, for a popup opened from it.
//
// A popup is a window of its own and inherits none of the window style the panel pushed, so with
// nothing pushed it draws as bare ImGui: a translucent near-black sheet the panel reads straight
// through, square corners with a border, and the stock blue selection. Begin() reads the background
// and the rounding and the items read the Header colours, so the style has to be pushed before the
// popup opens and popped after it closes - which is what this owns, along with Begin and End:
//
//     if (PanelPopup popup{"my_popup"}) { ... }
//
// Not push_combo_style(): that one sets HeaderHovered transparent on purpose, because an Orca combo
// marks its hovered item with a border drawn by ImGui::BBLSelectable() rather than with a fill, so a
// plain Selectable() under it highlights as nothing at all. The teal here is the fill the rest of
// Orca's menus use (ImGuiWrapper::push_menu_style), which is what a menu item is supposed to do.
class PanelPopup
{
public:
    explicit PanelPopup(const char *id, ImGuiWindowFlags flags = 0)
    {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleColor(ImGuiCol_Header, ImGuiWrapper::COL_ORCA);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGuiWrapper::COL_ORCA);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGuiWrapper::COL_ORCA);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, ImGui::GetStyle().WindowRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
        m_open = ImGui::BeginPopup(id, flags);
    }
    ~PanelPopup()
    {
        if (m_open)
            ImGui::EndPopup();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    }
    explicit operator bool() const { return m_open; }

    PanelPopup(const PanelPopup &)            = delete;
    PanelPopup &operator=(const PanelPopup &) = delete;

private:
    bool m_open = false;
};

// push_combo_style(), with the fill a hovered item needs put back: ImGui::Combo() and a combo built
// by hand from Selectable() both mark the hovered item by filling it, and combo style leaves that
// colour transparent for BBLSelectable's border (see PanelPopup).
inline void push_panel_combo_style(const float scale)
{
    ImGuiWrapper::push_combo_style(scale);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGuiWrapper::COL_ORCA);
}

inline void pop_panel_combo_style()
{
    ImGui::PopStyleColor();
    ImGuiWrapper::pop_combo_style();
}

} // namespace Slic3r::GUI::gizmo_td

#endif // slic3r_GLGizmoTextureDisplacementInternal_hpp_
