#include "GLGizmoTextureDisplacement.hpp"

#include <Eigen/Core>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include "ColorSpaceConvert.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"

#include <slic3r/GUI/AnimatedTexture.hpp>
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp" // GLToolbar::Default_Icons_Size, to match the toolbar's icon size
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GuiColor.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp" // wxGetApp().mainframe, as the projector window's parent
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/GUI/TextureProjectorFrame.hpp"
#include "slic3r/GUI/UVEditorCanvas.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPrepareJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementDebugJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPreviewJob.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <chrono>
#include <future>
#include <cstdio>
#include <glad/gl.h>
#include <imgui.h>
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <memory>
#include "slic3r/GUI/GLTexture.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include <string>
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include <math.h>
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/IconManager.hpp"
#include <optional>
#include "libslic3r/Config.hpp"
#include "libslic3r/TextureBake/TextureBakeDebug.hpp"
#include <ratio>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <vector>
#include <utility>
#include <wx/gdicmn.h>
#include <wx/busycursor.h>
#include <wx/string.h>
#include <wx/filedlg.h>

namespace Slic3r::GUI {

namespace {
// ImGuiWrapper::slider_float()'s trailing `power` parameter is forwarded straight into
// ImGui::SliderFloat()'s ImGuiSliderFlags argument - this ImGui (1.83) replaced the old float
// "power" curve API with a flags word but kept the parameter in the same position, and
// ImGuiWrapper never caught up with the rename. Passing the Logarithmic flag through it is
// therefore how a log-scaled slider gets requested here.
//
// Depth and tile size both want it: they span two to three orders of magnitude, and the values
// that need the most precision (a few hundredths of a millimetre of relief, a very fine tile) are
// all bunched into the very bottom of a linear range, where a single pixel of slider travel is a
// bigger jump than the whole useful region.
constexpr float ImGuiLogSlider = float(ImGuiSliderFlags_Logarithmic);

// The scrollbar of the panel's scrolling areas (the panel body and the texture library). ImGui's stock
// scrollbar is a wide, square-cornered slab in a tinted track - against the flat panel it reads as a raw
// widget bolted onto the edge - so it is slimmed to a rounded thumb over an invisible track. The child
// background goes transparent with it, so the area reads as part of the panel.
void push_slim_scrollbar_style(float width, bool dark)
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

void pop_slim_scrollbar_style()
{
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
}

// Cuts a UTF-8 string down to `max_w` pixels, ending it in "..." when it had to be cut.
std::string ellipsize(std::string s, float max_w, float font_size = 0.f)
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

// Everything above is about drawing a ~48 px panel row, and none of it applies to the height texture
// the fast-preview *shader* samples: that one is magnified across the model, not minified into a
// row, and every texel it loses is relief the preview cannot show. It gets its own upload at (up to)
// this size, so the shaded preview reads the same height field the bake does instead of a 128 px box
// blur of it - which is what made Fast look flatter and softer than the result it was previewing.
constexpr int HEIGHT_TEX_MAX_PX = 2048;

// Size the image-adjustment panel's live preview is computed and uploaded at. The panel draws the image
// in a box about 240 px wide, so there is nothing to show above this - and the preview is recomputed on
// every slider move, which is what makes its cost worth bounding. Measured on a 1024x1024 with every
// knob engaged, a full-size pass is 13-19 ms grayscale / 40-60 ms in colour (130-190 ms at 2048 in
// colour); the same pass on a 384 px copy is 3-4 ms / 9-12 ms, and it runs off the UI thread besides.
// The full-size image is only ever adjusted once, on commit.
constexpr int ADJUST_PREVIEW_MAX_PX = 384;

// Library textures the texture picker decodes per frame. Each decode runs on the GUI thread, so the
// rest of the visible tiles fill in over the following frames instead of in one stall.
constexpr int PICKER_LOADS_PER_FRAME = 2;

// How many rings of vertex-adjacent triangles either side of the paint's edge join the border refine
// band (see collect_paint_region()). Two is enough to grade the size change without the band's own
// cost growing to matter: it is a ring around a perimeter, not an area.
constexpr int BORDER_BAND_RINGS = 2;

// Is `p` inside triangle `t`, given that it already lies in the triangle's plane? Barycentric via the
// three sub-triangle cross products, compared against the whole triangle's normal. Used to carry a
// partly painted source triangle's coverage onto the children of a subdivision, which are coplanar
// with it by construction (subdivision only adds edge midpoints).
bool point_in_triangle_coplanar(const Vec3f &p, const std::array<Vec3f, 3> &t)
{
    const Vec3f n = (t[1] - t[0]).cross(t[2] - t[0]);
    const float n2 = n.squaredNorm();
    if (n2 < 1e-20f)
        return false; // degenerate: it covers no area, so nothing is inside it
    // A small negative tolerance, scaled by the triangle, keeps a centroid sitting exactly on a shared
    // edge from falling through the gap between two neighbouring pieces.
    const float eps = -1e-4f * n2;
    return (t[1] - t[0]).cross(p - t[0]).dot(n) >= eps &&
           (t[2] - t[1]).cross(p - t[1]).dot(n) >= eps &&
           (t[0] - t[2]).cross(p - t[2]).dot(n) >= eps;
}

// Carries one texture-displacement paint mask from `src_mesh` onto `dst_mesh` - a different
// tessellation of the same surface (an isotropic remesh, in practice).
//
// TriangleSelector::remap_painting(), which ModelVolume::restore_painting() uses for the four
// standard paint channels, is not usable here. It runs one select_patch() flood fill per
// (painted sub-triangle x overlapping target facet) pair, and select_patch() splits the *target's*
// triangles down to a 0.02-0.05 mm edge limit along every cursor boundary, behind an
// O(target triangles) visited-set allocation per call. A coarse import painted with a fine brush
// carries tens of thousands of painted sub-triangles, so that is tens of millions of sub-triangles
// created on the target - which is what made Remesh, and Standard mode's Bake (which remeshes for
// you), appear to hang rather than finish.
//
// Each target triangle here asks one question instead: is the point on the source surface closest
// to my centroid inside a painted piece? One AABB-tree query per target triangle, whole facets
// only, no splitting - O(target log source). `src_tree` is built over `src_mesh.its` by the caller
// and shared across the layers; `dst_to_src` brings the target's vertices into the source's frame.
TriangleSelector::TriangleSplittingData remap_texture_paint_spatial(
    const TriangleMesh &src_mesh, const TriangleSelector::TriangleSplittingData &src_data,
    const AABBTreeIndirect::Tree3f &src_tree, const TriangleMesh &dst_mesh, const Vec3f &dst_to_src)
{
    const indexed_triangle_set &src  = src_mesh.its;
    const indexed_triangle_set &dst  = dst_mesh.its;
    const size_t                ntri = src.indices.size();
    if (src_data.bitstream.empty() || ntri == 0 || dst.indices.empty() || src_tree.empty())
        return {};

    // The painted patch, split into per-source-triangle pieces - the same shape
    // collect_paint_region() builds, and for the same reason: a source triangle the brush only
    // partly covered has to answer "is this point painted" geometrically rather than be rounded to
    // painted-or-not, which leaves a ragged fringe along any curved brush boundary.
    TriangleSelector src_sel(src_mesh);
    src_sel.deserialize(src_data, false);
    std::vector<int>           piece_src;
    const indexed_triangle_set patch = src_sel.get_facets_strict(EnforcerBlockerType::ENFORCER, &piece_src);
    if (patch.indices.empty())
        return {};

    const auto tri_area2 = [](const Vec3f &a, const Vec3f &b, const Vec3f &c) {
        return (b - a).cross(c - a).norm();
    };
    std::vector<float> src_area2(ntri, 0.f), covered2(ntri, 0.f);
    for (size_t i = 0; i < ntri; ++i)
        src_area2[i] = tri_area2(src.vertices[size_t(src.indices[i][0])], src.vertices[size_t(src.indices[i][1])],
                                 src.vertices[size_t(src.indices[i][2])]);
    for (size_t j = 0; j < patch.indices.size() && j < piece_src.size(); ++j) {
        if (size_t(piece_src[j]) >= ntri)
            continue;
        const stl_triangle_vertex_indices &t = patch.indices[j];
        covered2[size_t(piece_src[j])] += tri_area2(patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                                    patch.vertices[size_t(t[2])]);
    }
    std::vector<uint8_t> full(ntri, 0);
    for (size_t i = 0; i < ntri; ++i)
        full[i] = (src_area2[i] > 0.f && covered2[i] >= 0.999f * src_area2[i]) ? 1 : 0;

    // CSR pieces, kept for partly covered sources only - a full one answers every query "painted".
    std::vector<int> part_start(ntri + 1, 0);
    for (size_t j = 0; j < patch.indices.size() && j < piece_src.size(); ++j)
        if (size_t(piece_src[j]) < ntri && !full[size_t(piece_src[j])])
            ++part_start[size_t(piece_src[j]) + 1];
    for (size_t i = 0; i < ntri; ++i)
        part_start[i + 1] += part_start[i];
    std::vector<std::array<Vec3f, 3>> part;
    part.resize(size_t(part_start[ntri])); // not a constructor call: `vector<T> p(size_t(x[n]));` parses
                                           // as a function declaration, and every use of `part` below
                                           // then fails with something that does not mention the cause.
    {
        std::vector<int> fill(part_start.begin(), part_start.begin() + ntri);
        for (size_t j = 0; j < patch.indices.size() && j < piece_src.size(); ++j) {
            const size_t S = size_t(piece_src[j]);
            if (S >= ntri || full[S])
                continue;
            const stl_triangle_vertex_indices &t = patch.indices[j];
            part[size_t(fill[S]++)] = { patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                        patch.vertices[size_t(t[2])] };
        }
    }

    // Classify in parallel, then write the mask serially - TriangleSelector is not thread safe.
    std::vector<uint8_t> painted(dst.indices.size(), 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, dst.indices.size()),
                      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const stl_triangle_vertex_indices &t = dst.indices[i];
            const Vec3f centroid = (dst.vertices[size_t(t[0])] + dst.vertices[size_t(t[1])] +
                                    dst.vertices[size_t(t[2])]) / 3.f + dst_to_src;
            size_t hit     = 0;
            Vec3f  hit_pos = Vec3f::Zero();
            if (AABBTreeIndirect::squared_distance_to_indexed_triangle_set(src.vertices, src.indices, src_tree,
                                                                          centroid, hit, hit_pos) < 0.f ||
                hit >= ntri)
                continue;
            if (full[hit]) {
                painted[i] = 1;
                continue;
            }
            for (int k = part_start[hit]; k < part_start[hit + 1]; ++k)
                if (point_in_triangle_coplanar(hit_pos, part[size_t(k)])) {
                    painted[i] = 1;
                    break;
                }
        }
    });

    TriangleSelector dst_sel(dst_mesh);
    for (size_t i = 0; i < painted.size(); ++i)
        if (painted[i])
            dst_sel.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    return dst_sel.serialize();
}

// Edge of the RGB lookup cube make_palette_quantizer() builds. 24 gives 13824 cells - far finer than
// the difference between any two printable colours - and costs one DeltaE00 per cell per palette
// entry to fill.
constexpr int PALETTE_LUT_EDGE = 24;

// The shaded preview shader's palette arrays. The palette itself stays within the paint mask's
// EnforcerBlockerType::ExtruderMax states, since every entry has to become a filament.
constexpr int PALETTE_MAX_ENTRIES = 64;

// A mix is an interleave that only reads as its colour from a distance; up close it is stripes. So it
// is spent only where it beats the nearest single filament by this much (CIEDE2000). Two is about
// where a side-by-side difference stops being arguable; a margin of ten already turns most of a
// greyscale ramp - the shape a height texture actually traces - back into single filaments. The
// quantizer, the mix ranking and the shaded preview shader all apply it, so they agree on where a mix
// is used.
constexpr float PREFER_PURE_DE = 2.f;

// mix_targets(): the most pixels read per layer, and the histogram bins kept over all layers. Together
// they bound rank_mixes() to candidates x MIX_TARGET_BINS colour differences, the same order as filling
// the quantizer's lookup cube.
constexpr size_t MIX_TARGET_SAMPLES = size_t(1) << 20;
constexpr size_t MIX_TARGET_BINS    = 256;
// rank_mixes() stops once the best remaining mix would improve the match by less than this, in
// CIEDE2000 averaged over every pixel of the colouring layers (see mix_targets()): a mix that only
// touches a few stray pixels is not worth a filament slot.
constexpr float MIN_MIX_GAIN = 0.05f;

// The project's mixed filament slots, one string each, as the palette cache compares them: anything
// that changes which of them a mix can reuse changes this.
std::vector<std::string> mixed_slot_signature(const DynamicPrintConfig &project_config)
{
    std::vector<std::string> out;
    const auto *is_mixed = project_config.option<ConfigOptionBools>("filament_is_mixed");
    const auto *comps    = project_config.option<ConfigOptionStrings>("filament_mixed_components");
    const auto *ratios   = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    const auto *gradient = project_config.option<ConfigOptionBools>("filament_mixed_gradient");
    if (is_mixed == nullptr || comps == nullptr || ratios == nullptr)
        return out;
    for (size_t i = 0; i < is_mixed->values.size(); ++i)
        if (is_mixed->values[i])
            out.push_back(std::to_string(i) + ':' + (i < comps->values.size() ? comps->values[i] : std::string()) + '|' +
                          (i < ratios->values.size() ? ratios->values[i] : std::string()) + '|' +
                          (gradient != nullptr && i < gradient->values.size() && gradient->values[i] ? "g" : ""));
    return out;
}

// Whether two palettes would draw and print the same.
bool same_palette(const std::vector<PrintableColor> &l, const std::vector<PrintableColor> &r)
{
    return std::equal(l.begin(), l.end(), r.begin(), r.end(), [](const PrintableColor &x, const PrintableColor &y) {
        return x.a == y.a && x.b == y.b && x.num == y.num && x.den == y.den && x.rgb == y.rgb;
    });
}

// sRGB (0..1) <-> CIELAB, D65. Exactly what the preview shader's srgb_to_lab() computes, so the CPU
// quantizer, the mixed-palette entries and the per-fragment preview all match in the same space.
// Not slic3r/Utils/ColorSpaceConvert: its RGB2Lab wants 0..1 but its Lab2RGB hands back linear
// values on a 0..100 scale, and the earlier code fed the former 0..255 and divided the latter by 255 -
// self-consistent enough for the bake's nearest-entry search to rank sensibly, but the preview, which
// compares real Lab against those values, picked the darkest filament everywhere.
static Vec3f srgb_to_lab(const Vec3f &c)
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

static Vec3f lab_to_srgb(const Vec3f &lab)
{
    const float fy = (lab.x() + 16.f) / 116.f, fx = lab.y() / 500.f + fy, fz = fy - lab.z() / 200.f;
    const auto  finv = [](float t) { return t > 0.206893f ? t * t * t : (t - 16.f / 116.f) / 7.787f; };
    const float x = finv(fx) * 0.95047f, y = finv(fy), z = finv(fz) * 1.08883f;
    const float r = 3.2406f * x - 1.5372f * y - 0.4986f * z;
    const float g = -0.9689f * x + 1.8758f * y + 0.0415f * z;
    const float b = 0.0557f * x - 0.2040f * y + 1.0570f * z;
    const auto gam = [](float v) {
        v = std::clamp(v, 0.f, 1.f);
        return v > 0.0031308f ? 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f : 12.92f * v;
    };
    return Vec3f(gam(r), gam(g), gam(b));
}

std::unique_ptr<GLTexture> upload_height_thumbnail(const DecodedHeightTexture &decoded, int max_px = THUMBNAIL_MAX_PX)
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

// The same upload, of the texture's *colour* rather than its height, for the fast preview to quantize
// per fragment. Null for a grayscale texture - there is nothing to show.
//
// Box-filtered down like the height is, and for a sharper reason: the fast preview quantizes every
// fragment independently, so any texel-scale noise left in the image becomes a scatter of single-pixel
// colour flips on screen. Filtering on the way to the GPU is where that is cheapest to remove.
std::unique_ptr<GLTexture> upload_color_texture(const DecodedHeightTexture &decoded, int max_px)
{
    if (!decoded.has_color())
        return nullptr;

    const int scale = std::max(1, (std::max(decoded.width, decoded.height) + max_px - 1) / max_px);
    const int w     = std::max(1, decoded.width / scale);
    const int h     = std::max(1, decoded.height / scale);

    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = x * decoded.width / w, x1 = std::max(x0 + 1, (x + 1) * decoded.width / w);
            const int y0 = y * decoded.height / h, y1 = std::max(y0 + 1, (y + 1) * decoded.height / h);
            unsigned int sum[3] = { 0, 0, 0 };
            unsigned int n      = 0;
            for (int sy = y0; sy < y1 && sy < decoded.height; ++sy)
                for (int sx = x0; sx < x1 && sx < decoded.width; ++sx, ++n) {
                    const size_t si = (size_t(sy) * size_t(decoded.width) + size_t(sx)) * 3;
                    for (int c = 0; c < 3; ++c)
                        sum[c] += decoded.rgb[si + size_t(c)];
                }
            const size_t di = (size_t(y) * size_t(w) + size_t(x)) * 4;
            for (int c = 0; c < 3; ++c)
                rgba[di + size_t(c)] = (n > 0) ? static_cast<unsigned char>(sum[size_t(c)] / n) : 0;
            rgba[di + 3] = 255;
        }

    auto texture = std::make_unique<GLTexture>();
    if (!texture->load_from_raw_data(std::move(rgba), (unsigned int) w, (unsigned int) h, false, false))
        return nullptr;
    return texture;
}

// Intersects the camera ray through `mouse_pos` (screen coords) with the plane passing through
// `plane_point_local`/`plane_normal_local` (mesh-local coords, transformed to world by `trafo`).
// Returns false if the ray is parallel to the plane or the plane is behind the camera.
bool ray_plane_hit(const Camera &camera, const Vec2d &mouse_pos, const Transform3d &trafo,
                    const Vec3f &plane_point_local, const Vec3f &plane_normal_local, Vec3d &out_world_hit)
{
    Vec3d ray_origin, ray_dir;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, ray_origin, ray_dir);

    const Vec3d plane_point_world  = trafo * plane_point_local.cast<double>();
    const Vec3d plane_normal_world = (trafo.matrix().block(0, 0, 3, 3).inverse().transpose() * plane_normal_local.cast<double>()).normalized();

    const double denom = ray_dir.dot(plane_normal_world);
    if (std::abs(denom) < 1e-8)
        return false;

    const double t = (plane_point_world - ray_origin).dot(plane_normal_world) / denom;
    if (t < 0.0)
        return false;

    out_world_hit = ray_origin + ray_dir * t;
    return true;
}
} // namespace

GLGizmoTextureDisplacement::GLGizmoTextureDisplacement(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoTextureDisplacement::on_init()
{
    m_shortcut = Shortcut::GizmoDisplacement;
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();


    m_desc["cursor_size"]   = _L("Brush size");
    m_desc["circle"]        = _L("Circle");
    m_desc["sphere"]        = _L("Sphere");
    m_desc["remove_layer"]  = _L("Remove");
    m_desc["bake"]          = _L_CONTEXT("Bake", "Texture Displacement");


    m_desc["paint"]            = _L("Paint");
    m_desc["erase"]            = _L("Erase");
    m_desc["gap_area"]         = _L("Gap area");
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["toggle_wireframe"] = _L("Toggle Wireframe");

    std::pair<wxString, wxString> paint_shortcut            = {_L("Left mouse button"),         m_desc["paint"]};
    std::pair<wxString, wxString> erase_shortcut            = {shift + _L("Left mouse button"), m_desc["erase"]};
    std::pair<wxString, wxString> toggle_wireframe_shortcut = {alt + shift + _L_CONTEXT("Enter", "Keyboard Shortcut"), m_desc["toggle_wireframe"]};

    m_shortcuts_brush = {
        paint_shortcut,
        erase_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]},
        toggle_wireframe_shortcut
    };

    m_shortcuts_bucket_fill = {
        paint_shortcut,
        erase_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]},
        toggle_wireframe_shortcut
    };

    m_shortcuts_gap_fill = {
        {ctrl + _L("Mouse wheel"), m_desc["gap_area"]},
        toggle_wireframe_shortcut
    };

    // Both ways into the same input box on any slider in the panel: ImGui turns a slider into one when
    // it is Ctrl+clicked or when it is given the keyboard focus, which is what the edit icon asks for.
    m_desc["type_value"] = _L("Type an exact value into a slider");
    m_shortcuts_sliders  = {
        {ctrl + _L("Left mouse button"), m_desc["type_value"]},
        {_L("Edit icon at a slider's right end"), m_desc["type_value"]},
    };
    for (std::vector<std::pair<wxString, wxString>> *list : {&m_shortcuts_brush, &m_shortcuts_bucket_fill, &m_shortcuts_gap_fill})
        list->insert(list->end(), m_shortcuts_sliders.begin(), m_shortcuts_sliders.end());

    return true;
}

std::string GLGizmoTextureDisplacement::on_get_name() const
{
    return _u8L("Texture displacement");
}

bool GLGizmoTextureDisplacement::on_is_activable() const
{
    return m_parent.get_canvas_type() != GLCanvas3D::CanvasAssembleView && GLGizmoPainterBase::on_is_activable();
}

void GLGizmoTextureDisplacement::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
    m_preview_glmodel.reset();
    m_part_previews.clear();
    m_shaded_preview_glmodel.reset();
    m_paint_overlay_glmodel.reset();
    m_paint_overlay_dirty = false;
    m_painted_colors = PaintedColors{};
    m_painted_colors_key.clear();
    m_painted_colors_glmodel.reset();
    m_painted_colors_drawn_key.clear();
    m_painted_colors_runs.clear();
    m_seed_fill_last_mesh_id = -1; // a hover from this session must not count in the next
    // Any preview still in flight is superseded: raising the shared counter makes it abort at its next
    // progress poll, and its completion handler then finds nothing to do.
    m_preview_generation->fetch_add(1);
    m_preview_job_pending = false;
    // The adjustment panel holds a full-size decode of its layer's image, which is the largest thing
    // here; a closed gizmo should not keep it alive. Discards the edit, as closing the gizmo on an open
    // panel implies.
    close_image_adjust();
    // The palette caches hold the last volume's images; a closed gizmo should not keep them alive.
    m_palette_cache.clear();
    m_palette_quantizer      = nullptr;
    m_palette_pure_quantizer = nullptr;
    m_palette_filaments.clear();
    m_palette_images.clear();
    m_mix_ranking.reset();
    m_palette_slots.clear();
    m_palette_changed = false;
    m_uvcheck_glmodel.reset();
    m_wireframe_overlay_glmodel.reset();
    m_wireframe_overlay_vcount = 0;
    m_seam_glmodel.reset();
    m_seam_hover_glmodel.reset();
    m_seam_hover_edge     = { -1, -1 };
    m_seam_hover_vertex   = -1;
    m_seam_anchor_glmodel.reset();
    m_seam_path_mode      = false;
    m_seam_path_anchor    = -1;
    m_subdivide_editing       = false;
    m_subdivide_preview_tris = -1;
    m_subdivide_preview_glmodel.reset();
    m_shaded_active_chart  = -1;
    m_shaded_active_face.clear();
    m_shaded_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
    m_island_drag_active = false;
    m_island_move_set.clear();
    // The pane closes below, taking the highlight with it, and the view must not open the next session in a
    // Fast the user never picked. Set directly: nothing is to be queued against a gizmo that is closing.
    if (m_island_view.switched)
        m_use_shaded_preview = false;
    m_island_view = IslandViewFollow{};
    m_adjust_texture_mode = false;
    m_seam_edit_mode      = false;
    m_adjust_drag_handle  = AdjustHandle::None;
    m_adjust_anchor_valid = false;
    // The pane is request-only: closing the gizmo drops the request, so reopening it later doesn't
    // silently reopen the pane too.
    m_show_uv_editor = false;
    wxGetApp().plater()->show_uv_editor(false);

    // Destroyed, not just hidden: unlike the UV pane (owned by Plater), this frame is owned here, and
    // it holds a callback capturing `this`. Leaving it alive past the gizmo would leave that callback
    // pointing at a gizmo that is no longer driving anything.
    if (m_projector_frame != nullptr) {
        m_projector_frame->Destroy();
        m_projector_frame = nullptr;
    }
    m_projector_tex_source    = nullptr; // a rebuilt frame starts with no texture in it
    m_projector_tex_smoothing = -1.f;
}

PainterGizmoType GLGizmoTextureDisplacement::get_painter_type() const
{
    return PainterGizmoType::TEXTURE_DISPLACEMENT;
}

wxString GLGizmoTextureDisplacement::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    // Right button does the opposite of the toggle; Shift always erases.
    const bool erasing = shift_down || (m_erase_mode != (button_down == GLGizmoPainterBase::Button::Right));
    return erasing ? _L("Erase texture displacement paint") : _L("Paint texture displacement");
}

void GLGizmoTextureDisplacement::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // Once anything is painted, m_preview_glmodel holds the true displaced result (same algorithm
    // Bake uses). The untouched original topology (what render_triangles() draws) coincides exactly
    // with it everywhere except the displaced area, so both can be drawn: the real preview geometry
    // first, then the selectors' highlight with a small depth bias so it wins the depth test on the
    // coincident surface. Where the surface has actually been displaced, the raised preview geometry
    // legitimately occludes the flat highlight - that visible relief is itself the "this is painted"
    // indicator in that area.
    //
    // The shaded preview never actually moves geometry (it only shades), so its depth is identical to
    // the highlight's *everywhere*: the depth-biased opaque highlight would win the depth test across the
    // whole surface and hide the shading entirely. So render_triangles() leaves the textured volume out
    // there, and the translucent tint drawn further down is the paint feedback instead.
    // Coalesced shaded-preview rebuild from an in-progress UV island drag (see on_island_edited): done here, at
    // most once per drawn frame, rather than synchronously in the UV canvas's mouse-move handler.
    if (m_use_shaded_preview && m_shaded_preview_dirty) {
        rebuild_shaded_preview_mesh();
        m_shaded_preview_dirty = false;
    }
    // Same coalescing for the paint tint, but on its own flag: a stroke marks this every mouse move
    // (see on_mouse()) and it only costs the painted patch
    if (m_paint_overlay_dirty) {
        rebuild_paint_overlay();
        m_paint_overlay_dirty = false;
    }
    rebuild_other_paint_overlay(); // a no-op unless another layer's paint, the active layer or the preview changed
    // is_initialized() alone is not enough: render_shaded_preview_mesh() also needs an active layer
    // with a decoded texture and a compiled shader, and bails silently without them. Hiding the real
    // volume for a shaded pass that then draws nothing is what made the model vanish - most obviously
    // with zero layers, but equally with a layer that has no texture picked yet.
    const bool use_shaded = m_use_shaded_preview && m_shaded_preview_glmodel.is_initialized() && shaded_preview_ready();
    // Checker/Distortion are built from the *base* patch and drawn with a polygon offset, which biases
    // depth values - it does not move the geometry. It therefore cannot win against a surface that
    // genuinely stands in front, and the displaced preview does exactly that: it rises above the base
    // surface by the layer's depth. Drawn underneath a UV-check overlay it simply occludes it, which is
    // why those two modes looked like they did nothing. Leave it out and let the undisplaced volume show
    // through instead (toggle_model_objects_visibility below) - that one *is* coincident with the
    // overlay, which is what the offset assumes, and it is the surface whose mapping is being inspected.
    const bool use_true_preview = !use_shaded && m_uv_check_mode == UVCheckMode::None && has_true_preview();
    // In Checker/Distortion mode the UV-check overlay *is* the surface visualization the user is
    // looking at, so the opaque paint-selection highlight must not be drawn on top of it - same
    // reasoning as skipping it for the shaded preview (see bug #12). Without this the painted area
    // covers the checker/heatmap and it can't be seen.
    const bool show_paint_overlay = m_uv_check_mode == UVCheckMode::None;

    // Hide the real volume only when something is actually going to be drawn in its place; otherwise
    // put it back. Getting this wrong leaves an invisible model, so it is decided once, here, rather
    // than per branch below.
    ModelVolume *mv = texture_volume();
    // The parts a preview is standing in for, and whose real volumes therefore have to be hidden and
    // whose selectors must not be drawn over it. The shaded view only ever shades the layer stack's own
    // part; the true-displacement view covers every painted part of a multibody object.
    std::vector<const ModelVolume *> previewed;
    if (use_shaded && mv != nullptr) {
        previewed.push_back(mv);
    } else if (use_true_preview) {
        if (mv != nullptr && m_preview_glmodel.is_initialized())
            previewed.push_back(mv);
        for (const std::unique_ptr<PartPreview> &preview : m_part_previews)
            if (const ModelVolume *part = preview_part(*preview); part != nullptr)
                previewed.push_back(part);
    }
    m_parent.toggle_model_objects_visibility(true);
    for (const ModelVolume *volume : previewed)
        m_parent.toggle_model_objects_visibility(false, m_c->selection_info()->model_object(),
                                                  m_c->selection_info()->get_active_instance(), volume);

    // Over a preview, the paint feedback - the selectors' highlight in the Normal view, the tint in both -
    // is drawn only while a mouse button is down: during a stroke, a fill click included. Between strokes
    // it would cover the preview: the highlight buries it under a flat plane wherever the relief does not
    // rise, and the tint washes it green. The debug view shows a captured stage rather than a preview, so
    // it keeps the feedback.
    const bool paint_feedback = is_painting() || m_debug_stage >= 0;

    // Whether the textured volume's selector - and with it a fill tool's contour - is drawn this frame.
    bool textured_selector_drawn = true;
    if (use_shaded) {
        render_shaded_preview_mesh();
        // The shaded mesh is the textured volume alone, so the other model parts are still the selectors' to draw.
        render_triangles(selection, previewed);
        textured_selector_drawn = false;
    } else if (use_true_preview) {
        render_preview_mesh();
        glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glPolygonOffset(-1.0f, -1.0f));
        // Without paint feedback, only the parts with no preview over them: render_preview_mesh() draws
        // the rest.
        render_triangles(selection, paint_feedback ? std::vector<const ModelVolume *>{} : previewed);
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        textured_selector_drawn = paint_feedback;
    } else {
        // render_triangles() *is* the model in a painter gizmo (it draws every model-part volume with the
        // selector's colours), not an overlay on top of one - so it still has to run under a UV-check
        // overlay, or nothing draws the surface at all and the checker floats alone over an empty scene.
        // Deliberately without the depth bias the branch above applies: the checker/heatmap is drawn later
        // with its own -1 offset and has to win against this. Biasing both by the same amount is what made
        // the painted area cover the checker and is why this call used to be skipped outright.
        render_triangles(selection);
    }

    // The model's colour paint, over whichever surface was drawn, left out where that surface's preview shows
    // paint of its own: every layer's for the Normal mesh, the active layer's otherwise.
    const bool painted_colors_drawn = show_paint_overlay && m_debug_stage < 0 && render_painted_colors(use_true_preview);

    // Every other layer's paint, in muted grey, so all layers stay visible while one of them is edited. Drawn
    // before the active layer's tint so that one reads on top where the two overlap.
    if (show_paint_overlay)
        render_paint_overlay(m_other_paint_glmodel);

    // The translucent paint tint. Needed in the shaded view because the opaque highlight above is
    // skipped there, and in the true-displacement view because the displaced surface rises *above*
    // the undisplaced overlay geometry and hides it exactly where the relief is strongest - in both
    // cases leaving an erase stroke with no visible effect until the next full preview rebuild. With no
    // preview, the highlight is the selectors' own, except during a stroke over the colour paint: the paint
    // there is not flushed yet, so the colours are still drawn over the stroke's highlight.
    const bool preview_drawn = use_shaded || use_true_preview;
    if (show_paint_overlay && (preview_drawn ? paint_feedback : painted_colors_drawn && is_painting()))
        render_paint_overlay(m_paint_overlay_glmodel);

    // A fill tool's contour is drawn with the selectors, under the colour paint drawn since - which covers it
    // on a steep face, where the paint's slope-scaled offset outruns the contour's fixed one. Put it back on top,
    // at the depth its first draw stored, hence LEQUAL. Only where the selector was drawn this frame: drawing it
    // is what rebuilds the contour. The tool test matters because the base keeps the last hovered mesh when the
    // tool changes.
    const bool fill_tool = m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL ||
                           (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER);
    const int  textured_mesh_id = texture_volume_raycaster_index();
    if (painted_colors_drawn && textured_selector_drawn && fill_tool && textured_mesh_id >= 0 &&
        textured_mesh_id == m_seed_fill_last_mesh_id && size_t(textured_mesh_id) < m_triangle_selectors.size()) {
        const ModelObject *mo = m_c->selection_info()->model_object();
        GLint depth_func = GL_LESS;
        glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
        glsafe(::glDepthFunc(GL_LEQUAL));
        m_triangle_selectors[size_t(textured_mesh_id)]->render_paint_contour(
            mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix());
        glsafe(::glDepthFunc(GLenum(depth_func)));
    }

    // The UV editor's island selection, shown on the model. Polled here rather than pushed: the pane
    // changes its selection in its own mouse handling, and a compare of a few ints per frame is free.
    if (const std::vector<int> *islands = island_highlight_selection()) {
        if (*islands != m_island_overlay_selection)
            rebuild_island_overlay(*islands);
        render_island_overlay();
    } else if (m_island_overlay_glmodel.is_initialized()) {
        m_island_overlay_glmodel.reset();
        m_island_overlay_selection.clear();
    }

    // Diagnostic overlays, drawn on top of whatever preview is active (both pull toward the camera
    // with a polygon offset so they win the depth test against the coincident surface).
    if (m_uv_check_mode != UVCheckMode::None)
        render_uvcheck_mesh();
    // While previewing a subdivision, its wireframe stands in for the mesh wireframe - it shows the
    // density the model *would* have. The normal wireframe toggle is left untouched underneath, so it
    // returns to whatever it was once the preview ends (which is what keeps an already-on wireframe on).
    if (m_subdivide_editing)
        render_subdivide_preview();
    else if (m_wireframe_overlay)
        render_wireframe_overlay();
    // Marked seams are always shown for an LSCM layer, so existing cuts are visible before entering
    // seam-edit mode - but they matter most while marking.
    render_seam_overlay();

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    if (m_adjust_texture_mode)
        render_adjust_texture_gizmo();
    else if (!m_seam_edit_mode) // the brush cursor is meaningless while marking seams
        render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

bool GLGizmoTextureDisplacement::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_seam_edit_mode)
        return on_mouse_seam(mouse_event);
    if (m_adjust_texture_mode)
        return on_mouse_adjust_texture(mouse_event);

    const bool handled = GLGizmoPainterBase::on_mouse(mouse_event);
    // A consumed drag/click is a paint (or erase) event: the base class has already updated the live
    // TriangleSelector, but nothing is flushed to the model - and so nothing rebuilds - until the
    // stroke ends. Mark the tint stale so it follows the brush from the first frame instead. Only the
    // flag is set here; the rebuild is coalesced to once per drawn frame in render_painter_gizmo().
    if (handled && (mouse_event.Dragging() || mouse_event.LeftDown() || mouse_event.RightDown() ||
                    mouse_event.LeftUp() || mouse_event.RightUp()))
        m_paint_overlay_dirty = true;
    return handled;
}

bool GLGizmoTextureDisplacement::on_mouse_seam(const wxMouseEvent &mouse_event)
{
    // Hold Ctrl to orbit/pan the camera while in seam mode, exactly as the base painter lets you do
    // while painting: with Ctrl down we consume nothing, so the canvas gets the drag and moves the
    // view. Without this, seam mode swallowed every left-drag and the camera couldn't be rotated.
    if (mouse_event.CmdDown())
        return false;

    // Left-click marks/unmarks an edge; swallow the rest of the left-button stream so a drag doesn't
    // paint, but let everything else through so the camera still orbits/pans/zooms normally.
    if (mouse_event.LeftDown()) {
        const Vec2d pos(mouse_event.GetX(), mouse_event.GetY());
        if (m_seam_path_mode) {
            // Two-click shortest-path seam: first click sets the start vertex, the next seams the whole
            // path to it and becomes the new start (so a seam line chains click by click).
            const int v = seam_vertex_at(pos);
            if (v >= 0) {
                if (m_seam_path_anchor < 0)
                    m_seam_path_anchor = v;
                else {
                    mark_seam_path(m_seam_path_anchor, v);
                    m_seam_path_anchor = v;
                }
                rebuild_seam_anchor_overlay();
                m_parent.set_as_dirty();
            }
        } else {
            toggle_seam_at(pos);
        }
        return true;
    }
    // Live hover: highlight what a click would pick, so the clickable target is obvious (the "I don't
    // know how it works" the user hit). In normal mode that is an edge; in shortest-path mode it is a
    // vertex. Don't swallow the motion - the camera still needs it.
    if (mouse_event.Moving()) {
        const Vec2d pos(mouse_event.GetX(), mouse_event.GetY());
        if (m_seam_path_mode) {
            const int v = seam_vertex_at(pos);
            if (v != m_seam_hover_vertex) {
                m_seam_hover_vertex = v;
                m_seam_hover_edge   = { -1, -1 };
                rebuild_seam_hover_overlay();
                m_parent.set_as_dirty();
            }
        } else {
            const std::pair<int, int> edge = seam_edge_at(pos);
            if (edge != m_seam_hover_edge) {
                m_seam_hover_edge   = edge;
                m_seam_hover_vertex = -1;
                rebuild_seam_hover_overlay();
                m_parent.set_as_dirty();
            }
        }
    }
    if (mouse_event.LeftUp() || (mouse_event.Dragging() && mouse_event.LeftIsDown()))
        return true;
    return false;
}

int GLGizmoTextureDisplacement::texture_volume_raycaster_index() const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return -1;

    int idx = -1, count = 0;
    for (const ModelVolume *v : mo->volumes) {
        if (!v->is_model_part())
            continue;
        if (v == mv) { idx = count; break; }
        ++count;
    }
    return (idx >= 0 && idx < int(m_c->raycaster()->raycasters().size())) ? idx : -1;
}

std::pair<int, int> GLGizmoTextureDisplacement::seam_edge_at(const Vec2d &mouse_pos) const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return { -1, -1 };

    const int idx = texture_volume_raycaster_index();
    if (idx < 0)
        return { -1, -1 };
    const auto &raycasters = m_c->raycaster()->raycasters();

    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo     = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera    = wxGetApp().plater()->get_camera();

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t facet = 0;
    if (!raycasters[size_t(idx)]->unproject_on_mesh(mouse_pos, trafo, camera, hit, normal,
                                                     m_c->object_clipper()->get_clipping_plane(), &facet))
        return { -1, -1 };

    const indexed_triangle_set &its = mv->mesh().its;
    if (facet >= its.indices.size())
        return { -1, -1 };
    const stl_triangle_vertex_indices &tri = its.indices[facet];

    // The facet edge nearest the hit point (point-to-segment distance in mesh space).
    const auto seg_dist = [](const Vec3f &p, const Vec3f &a, const Vec3f &b) {
        const Vec3f  ab = b - a;
        const float  l2 = ab.squaredNorm();
        const float  t  = (l2 > 1e-12f) ? std::clamp((p - a).dot(ab) / l2, 0.f, 1.f) : 0.f;
        return (p - (a + ab * t)).norm();
    };
    int   best_i = 0;
    float best_d = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float d = seg_dist(hit, its.vertices[tri[i]], its.vertices[tri[(i + 1) % 3]]);
        if (d < best_d) { best_d = d; best_i = i; }
    }
    const int a = tri[best_i], b = tri[(best_i + 1) % 3];
    return { std::min(a, b), std::max(a, b) };
}

void GLGizmoTextureDisplacement::toggle_seam_at(const Vec2d &mouse_pos)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;
    const std::pair<int, int> edge = seam_edge_at(mouse_pos);
    if (edge.first < 0)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mark texture seam"), UndoRedo::SnapshotType::GizmoAction);
    auto &seams = layer->lscm_seam_edges;
    if (const auto it = std::find(seams.begin(), seams.end(), edge); it != seams.end())
        seams.erase(it);
    else
        seams.push_back(edge);
    rebuild_preview();
}

int GLGizmoTextureDisplacement::seam_vertex_at(const Vec2d &mouse_pos) const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return -1;
    const int idx = texture_volume_raycaster_index();
    if (idx < 0)
        return -1;
    const auto &raycasters = m_c->raycaster()->raycasters();

    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo     = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera    = wxGetApp().plater()->get_camera();

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t facet = 0;
    if (!raycasters[size_t(idx)]->unproject_on_mesh(mouse_pos, trafo, camera, hit, normal,
                                                     m_c->object_clipper()->get_clipping_plane(), &facet))
        return -1;
    const indexed_triangle_set &its = mv->mesh().its;
    if (facet >= its.indices.size())
        return -1;
    const stl_triangle_vertex_indices &tri = its.indices[facet];
    int   best = tri[0];
    float best_d = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float d = (hit - its.vertices[tri[i]]).squaredNorm();
        if (d < best_d) { best_d = d; best = tri[i]; }
    }
    return best;
}

void GLGizmoTextureDisplacement::mark_seam_path(int v_from, int v_to)
{
    TextureDisplacementLayer *layer = active_layer();
    const ModelVolume        *mv    = texture_volume();
    if (layer == nullptr || mv == nullptr || v_from < 0 || v_to < 0 || v_from == v_to)
        return;
    const indexed_triangle_set &its = mv->mesh().its;
    const size_t                n   = its.vertices.size();
    if (size_t(v_from) >= n || size_t(v_to) >= n)
        return;

    // Shortest path over the mesh's edge graph (Dijkstra, edge weight = length). Built on demand; one
    // pass per click is fine even on a dense mesh.
    std::vector<std::vector<std::pair<int, float>>> adj(n);
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            const float w = (its.vertices[size_t(a)] - its.vertices[size_t(b)]).norm();
            adj[size_t(a)].push_back({ b, w });
            adj[size_t(b)].push_back({ a, w });
        }

    std::vector<float> dist(n, std::numeric_limits<float>::infinity());
    std::vector<int>   prev(n, -1);
    using QN = std::pair<float, int>;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> pq;
    dist[size_t(v_from)] = 0.f;
    pq.push({ 0.f, v_from });
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[size_t(u)])
            continue;
        if (u == v_to)
            break;
        for (const auto &[w, ew] : adj[size_t(u)]) {
            const float nd = d + ew;
            if (nd < dist[size_t(w)]) {
                dist[size_t(w)] = nd;
                prev[size_t(w)] = u;
                pq.push({ nd, w });
            }
        }
    }
    if (prev[size_t(v_to)] < 0)
        return; // unreachable (disconnected components)

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mark texture seam path"), UndoRedo::SnapshotType::GizmoAction);
    auto &seams = layer->lscm_seam_edges;
    for (int v = v_to; v != v_from && v >= 0; v = prev[size_t(v)]) {
        const int p = prev[size_t(v)];
        if (p < 0)
            break;
        const std::pair<int, int> e{ std::min(v, p), std::max(v, p) };
        if (std::find(seams.begin(), seams.end(), e) == seams.end())
            seams.push_back(e);
    }
    rebuild_preview();
}

void GLGizmoTextureDisplacement::rebuild_seam_anchor_overlay()
{
    m_seam_anchor_glmodel.reset();
    const ModelVolume *mv = texture_volume();
    if (!m_seam_edit_mode || !m_seam_path_mode || mv == nullptr || m_seam_path_anchor < 0)
        return;
    const indexed_triangle_set &its = mv->mesh().its;
    if (size_t(m_seam_path_anchor) >= its.vertices.size())
        return;

    // The anchor's incident edges, so the path's start vertex is visible on the model.
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    unsigned nn = 0;
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            if (a == m_seam_path_anchor || b == m_seam_path_anchor) {
                init_data.add_vertex(its.vertices[size_t(a)]);
                init_data.add_vertex(its.vertices[size_t(b)]);
                init_data.add_line(nn, nn + 1);
                nn += 2;
            }
        }
    if (!init_data.is_empty())
        m_seam_anchor_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::rebuild_seam_hover_overlay()
{
    m_seam_hover_glmodel.reset();
    const ModelVolume *mv = texture_volume();
    if (!m_seam_edit_mode || mv == nullptr)
        return;
    const indexed_triangle_set &its = mv->mesh().its;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };

    if (m_seam_path_mode) {
        // Highlight the hovered vertex as its ring of incident edges, so the click target is legible on
        // a dense mesh (matching the green anchor's style, in the hover yellow render_seam_overlay uses).
        if (m_seam_hover_vertex < 0 || size_t(m_seam_hover_vertex) >= its.vertices.size())
            return;
        unsigned nn = 0;
        for (const stl_triangle_vertex_indices &tri : its.indices)
            for (int i = 0; i < 3; ++i) {
                const int a = tri[i], b = tri[(i + 1) % 3];
                if (a == m_seam_hover_vertex || b == m_seam_hover_vertex) {
                    init_data.add_vertex(its.vertices[size_t(a)]);
                    init_data.add_vertex(its.vertices[size_t(b)]);
                    init_data.add_line(nn, nn + 1);
                    nn += 2;
                }
            }
        if (!init_data.is_empty())
            m_seam_hover_glmodel.init_from(std::move(init_data));
        return;
    }

    if (m_seam_hover_edge.first < 0 || size_t(m_seam_hover_edge.first) >= its.vertices.size() ||
        size_t(m_seam_hover_edge.second) >= its.vertices.size())
        return;
    init_data.reserve_vertices(2);
    init_data.reserve_indices(2);
    init_data.add_vertex(its.vertices[size_t(m_seam_hover_edge.first)]);
    init_data.add_vertex(its.vertices[size_t(m_seam_hover_edge.second)]);
    init_data.add_line(0, 1);
    m_seam_hover_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::rebuild_seam_overlay()
{
    m_seam_glmodel.reset();
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (mv == nullptr || layer == nullptr || layer->lscm_seam_edges.empty())
        return;
    const indexed_triangle_set &its = mv->mesh().its;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(layer->lscm_seam_edges.size() * 2);
    init_data.reserve_indices(layer->lscm_seam_edges.size() * 2);
    unsigned n = 0;
    for (const auto &[a, b] : layer->lscm_seam_edges) {
        if (a < 0 || b < 0 || size_t(a) >= its.vertices.size() || size_t(b) >= its.vertices.size())
            continue;
        init_data.add_vertex(its.vertices[size_t(a)]);
        init_data.add_vertex(its.vertices[size_t(b)]);
        init_data.add_line(n, n + 1);
        n += 2;
    }
    if (!init_data.is_empty())
        m_seam_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::render_seam_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    const bool         have_marked = m_seam_glmodel.is_initialized();
    const bool         have_hover  = m_seam_edit_mode && m_seam_hover_glmodel.is_initialized();
    const bool         have_anchor = m_seam_edit_mode && m_seam_anchor_glmodel.is_initialized();
    if (mo == nullptr || mv == nullptr || (!have_marked && !have_hover && !have_anchor))
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_POLYGON_OFFSET_LINE));
    glsafe(::glPolygonOffset(-2.0f, -2.0f)); // pull further forward than the wireframe so seams read on top
    // A seam edge is geometrically the same line as a wireframe edge, so a mere polygon offset is a
    // fragile way to make the red seam beat the white wireframe - drivers apply GL_POLYGON_OFFSET_LINE
    // inconsistently, and the two lines then z-fight and the wireframe wins. When the wireframe is on,
    // or while actively marking, just draw the seams with depth testing off so they are unconditionally
    // on top - being visible is the one thing this overlay has to guarantee.
    const bool seams_on_top = m_wireframe_overlay || m_seam_edit_mode;
    if (seams_on_top)
        glsafe(::glDisable(GL_DEPTH_TEST));
#if !SLIC3R_OPENGL_ES
    const bool wide = !OpenGLManager::get_gl_info().is_core_profile();
    if (wide)
        glsafe(::glLineWidth(4.0f));
#endif // !SLIC3R_OPENGL_ES
    if (have_marked) {
        m_seam_glmodel.set_color(ColorRGBA(1.0f, 0.15f, 0.15f, 1.0f)); // Blender's seam red
        m_seam_glmodel.render();
    }
    // The edge a click would toggle, in yellow and pulled the furthest forward, so it is unmistakable
    // which edge is being targeted while marking seams.
    if (have_hover) {
        glsafe(::glPolygonOffset(-3.0f, -3.0f));
        m_seam_hover_glmodel.set_color(ColorRGBA(1.0f, 0.9f, 0.15f, 1.0f));
        m_seam_hover_glmodel.render();
    }
    // The shortest-path start vertex, shown as its ring of incident edges in green.
    if (have_anchor) {
        glsafe(::glPolygonOffset(-3.0f, -3.0f));
        m_seam_anchor_glmodel.set_color(ColorRGBA(0.2f, 1.0f, 0.4f, 1.0f));
        m_seam_anchor_glmodel.render();
    }
#if !SLIC3R_OPENGL_ES
    if (wide)
        glsafe(::glLineWidth(1.0f));
#endif // !SLIC3R_OPENGL_ES
    glsafe(::glDisable(GL_POLYGON_OFFSET_LINE));
    if (seams_on_top)
        glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::render_preview_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;

    auto *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    const Selection   &selection       = m_parent.get_selection();
    const Transform3d  instance_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix();
    const Camera      &camera          = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix     = camera.get_view_matrix();

    shader->start_using();
    // Each part is drawn through its own matrix: a multibody object's parts sit in different places
    // inside the object, so one matrix for all of them would pile every preview onto the first part.
    const auto draw = [&](const ModelVolume &volume, GLModel &model, const std::vector<PreviewColorRun> &runs) {
        if (!model.is_initialized())
            return;
        const Transform3d trafo_matrix = instance_matrix * volume.get_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                            trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
        if (runs.empty()) {
            model.render();
        } else {
            // set_color() writes the uniform the shader reads, so one call per group is all the
            // per-triangle colour this needs.
            for (const PreviewColorRun &run : runs) {
                model.set_color(run.color);
                model.render(run.range, shader);
            }
        }
    };

    if (const ModelVolume *owner = texture_volume(); owner != nullptr)
        draw(*owner, m_preview_glmodel, m_preview_color_runs);
    for (const std::unique_ptr<PartPreview> &preview : m_part_previews)
        if (const ModelVolume *part = preview_part(*preview); part != nullptr)
            draw(*part, preview->glmodel, preview->color_runs);
    shader->stop_using();
}

const ModelVolume *GLGizmoTextureDisplacement::preview_part(const PartPreview &preview) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return nullptr;
    for (const ModelVolume *mv : mo->volumes)
        if (mv->id() == preview.volume_id)
            return mv;
    return nullptr; // the part was removed, or its mesh replaced, since the preview was built
}

float GLGizmoTextureDisplacement::layer_texture_aspect(const TextureDisplacementLayer &layer)
{
    // decode_height_texture() is cached on the image_data allocation, so this is a hash lookup rather
    // than a PNG decode - cheap enough to call per rebuild.
    const DecodedHeightTexture tex = decode_height_texture(layer);
    return (tex.width > 0 && tex.height > 0) ? float(tex.width) / float(tex.height) : 1.f;
}

indexed_triangle_set GLGizmoTextureDisplacement::patch_in_world(const indexed_triangle_set &patch) const
{
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return patch;
    // The bake's own frame, so the projection this feeds agrees with what bakes.
    const Transform3d to_world = texture_displacement_bake_frame(texture_displacement_volume_to_world(*mv));
    if (to_world.matrix().isApprox(Transform3d::Identity().matrix()))
        return patch;

    indexed_triangle_set world = patch;
    for (Vec3f &v : world.vertices)
        v = (to_world * v.cast<double>()).cast<float>();
    return world;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::model_uv_corners(const indexed_triangle_set     &local_patch,
                                                                const TextureDisplacementLayer &layer,
                                                                const std::vector<int>         *patch_source) const
{
    const ModelVolume *mv = texture_volume();
    if (layer.projection_method != TextureProjectionMethod::ModelUV || patch_source == nullptr || mv == nullptr ||
        !mv->has_texture_uv())
        return {};
    // The channel is indexed against the volume's *local* mesh, and a uv is a coordinate in the
    // texture rather than on the model, so unlike every projected method this never goes through
    // patch_in_world() - there is no millimetre measurement here for a world scale to change.
    std::vector<Vec2f> uv = compute_model_corner_uvs(mv->mesh().its, mv->texture_uv, local_patch, *patch_source);
    for (Vec2f &p : uv)
        // Already a uv: no tiling scale, no aspect correction - see apply_uv_transform().
        p = apply_uv_transform(p, layer, 1.f, /* normalized_uv */ true);
    return uv;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::compute_layer_vertex_uvs(const indexed_triangle_set     &local_patch,
                                                                       const TextureDisplacementLayer &layer,
                                                                       const std::vector<int>         *patch_source) const
{
    if (layer.projection_method == TextureProjectionMethod::ModelUV)
        // Collapsed to one per vertex the same way the unwrap is - a vertex has one position, so the
        // displacement can only read one height for it.
        return collapse_corner_uvs(local_patch, model_uv_corners(local_patch, layer, patch_source));

    // The bake maps the texture in world millimetres, so "Tile size (mm)" means the same thing on a
    // scaled instance as it does on an untouched one (see build_texture_displacement()). Everything
    // that has to agree with the bake - the fast preview's uvs, the checker/distortion overlays -
    // therefore has to project from the same world positions, not from the volume's own.
    const indexed_triangle_set patch  = patch_in_world(local_patch);
    const float                aspect = layer_texture_aspect(layer);
    if (layer.projection_method == TextureProjectionMethod::LSCM) {
        // compute_lscm_uvs() returns the unwrap's own (raw, mm) coordinates with the island placement
        // folded in - it does *not* apply the layer's tiling/rotation/offset. The bake applies those
        // on top (sample_layer_height()'s lscm branch runs the result through sample_at()), so the
        // shader's precomputed-uv path has to as well, or the fast preview samples millimetre-valued
        // coordinates as if they were uv and shows the texture at a wildly wrong scale.
        std::vector<Vec2f> uv = compute_lscm_uvs(patch, layer);
        for (Vec2f &p : uv)
            p = apply_uv_transform(p, layer, aspect);
        return uv;
    }
    if (layer.projection_method == TextureProjectionMethod::ViewProjected) {
        std::vector<Vec2f> uv(patch.vertices.size());
        for (size_t vi = 0; vi < patch.vertices.size(); ++vi) {
            if (layer.view_project_projective) {
                // Matches sample_layer_height()'s projective branch, including skipping
                // apply_uv_transform(). A vertex behind the projector gets a uv far outside [0,1] so
                // it samples as nothing, rather than the mirrored coordinate a blind divide gives.
                if (!project_uv_projective(layer.view_project_matrix, patch.vertices[vi], uv[vi]))
                    uv[vi] = Vec2f(-1e6f, -1e6f);
                continue;
            }
            // Negated up, as sample_layer_height()'s affine branch has it: v runs down the image.
            const Vec2f planar(patch.vertices[vi].dot(layer.view_project_right),
                               -patch.vertices[vi].dot(layer.view_project_up));
            uv[vi] = apply_uv_transform(planar, layer, aspect);
        }
        return uv;
    }
    return {}; // Triplanar / Cylindrical / Spherical: the shader projects on its own
}

int GLGizmoTextureDisplacement::layer_projection_frame(const indexed_triangle_set     &local_patch,
                                                       const TextureDisplacementLayer &layer,
                                                       Vec3f &center, Vec3f &axis) const
{
    center = Vec3f::Zero();
    axis   = Vec3f::UnitZ();
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || (layer.projection_method != TextureProjectionMethod::Cylindrical &&
                          layer.projection_method != TextureProjectionMethod::Spherical))
        return 0;
    // The bake averages the *whole mesh's* vertex normals over the patch's corners, so this has to as
    // well: a patch-only average would sometimes quantize to a different world axis and wrap the
    // texture the other way round. Both meshes go through patch_in_world() first, which is the frame
    // the shaders' tex_pos lives in.
    Vec3f average_normal;
    texture_displacement_patch_frame(patch_in_world(local_patch),
                                     texture_displacement_vertex_normals(patch_in_world(mv->mesh().its)),
                                     center, axis, average_normal);
    return layer.projection_method == TextureProjectionMethod::Cylindrical ? 1 : 2;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::compute_layer_corner_uvs(const indexed_triangle_set     &local_patch,
                                                                        const TextureDisplacementLayer &layer,
                                                                        const std::vector<int>         *patch_source) const
{
    if (layer.projection_method == TextureProjectionMethod::ModelUV)
        return model_uv_corners(local_patch, layer, patch_source);
    if (layer.projection_method == TextureProjectionMethod::LSCM) {
        const indexed_triangle_set patch  = patch_in_world(local_patch);
        const float                aspect = layer_texture_aspect(layer);
        std::vector<Vec2f>         uv     = compute_lscm_corner_uvs(patch, layer);
        for (Vec2f &p : uv)
            p = apply_uv_transform(p, layer, aspect);
        return uv;
    }
    // Single-valued per point: fan the per-vertex result out over the corners.
    const std::vector<Vec2f> per_vertex = compute_layer_vertex_uvs(local_patch, layer, patch_source);
    if (per_vertex.size() != local_patch.vertices.size())
        return {};
    std::vector<Vec2f> corner(local_patch.indices.size() * 3);
    for (size_t f = 0; f < local_patch.indices.size(); ++f)
        for (int k = 0; k < 3; ++k)
            corner[f * 3 + size_t(k)] = per_vertex[size_t(local_patch.indices[f][k])];
    return corner;
}

void GLGizmoTextureDisplacement::rebuild_shaded_preview_mesh()
{
    m_shaded_preview_glmodel.reset();

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;

    // Uses the *live* selector (not the flushed model facet data), so this reflects an in-progress
    // stroke immediately rather than only once it ends - the point of this preview mode is to be
    // the fast, no-CPU-meshing one.
    // The source map comes along because a ModelUV layer's uvs are read per base-mesh triangle; every
    // other mapping ignores it.
    std::vector<int>           patch_source;
    const indexed_triangle_set patch =
        m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER, &patch_source);
    if (patch.indices.empty())
        return;

    // No "patch.vertices.size() == mesh vertex count" check here, and that is the point: a *brush*
    // stroke splits triangles, so the selector appends split vertices and the patch array is longer
    // than the mesh's. An earlier version bailed out on that as "shouldn't happen", which meant the
    // shaded model was never built while brushing and render_painter_gizmo() silently fell back to the
    // Normal (true-displacement) preview - Fast looked broken for brush and fine for Face/Connected
    // area, because only the brush splits. Everything below indexes the patch's own vertex array, so
    // the extra vertices are simply carried through.

    // Unpainted triangles, so the surrounding surface still renders (the render path hides the real
    // model in shaded mode). get_facets_strict() returns the same vertex array whatever state is asked.
    const indexed_triangle_set rest = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::NONE);

    // For LSCM we hand the shader the finished per-vertex texture uv (island placement + tiling/
    // rotation/offset already folded in, exactly what the bake samples), because it cannot be
    // reconstructed in the fragment shader the way a triplanar projection can. This is also what
    // makes the fast preview follow the UV editor: the uvs move when an island is dragged, so this
    // mesh rebuilds (on drag end) with them. The other projections keep projecting in-shader.
    // Per *corner*, not per vertex: the mesh below is flat (unshared) anyway, so each triangle can
    // carry its own chart's UVs - see compute_layer_corner_uvs().
    const TextureDisplacementLayer *active = active_layer();
    std::vector<Vec2f> corner_uv = active != nullptr ? compute_layer_corner_uvs(patch, *active, &patch_source) :
                                                       std::vector<Vec2f>{};
    m_shaded_preview_uses_vertex_uv = corner_uv.size() == patch.indices.size() * 3;
    if (!m_shaded_preview_uses_vertex_uv)
        corner_uv.clear();
    m_shaded_projection_mode = (active != nullptr && !m_shaded_preview_uses_vertex_uv) ?
                                 layer_projection_frame(patch, *active, m_shaded_patch_center, m_shaded_patch_axis) : 0;

    // Which triangles the in-flight UV drag moves. Computed here, against the very patch this mesh is
    // built from, so the flags can never be indexed by a different triangle count than they were sized
    // for (the drag starts from the flushed facet data, a brush stroke changes the live selector).
    compute_shaded_active_faces(m_shaded_active_chart >= 0 ? m_island_move_set : std::vector<int>{}, patch.indices.size());

    // Colour is quantized per *fragment* in the shader now (see the .fs), so this mesh carries no
    // colour of its own - the palette and the colour texture are uniforms, and every pixel matches the
    // image rather than the facet it landed on. What the *bake* will produce, at facet resolution, is
    // what the Normal view shows.
    m_shaded_preview_palette = (active != nullptr && active->color_enabled) ? cached_palette()
                                                                         : std::vector<PaletteEntry>{};

    GLModel::Geometry init_data;
    // P3N3T2: normal.x carries the paint weight, tex_coord the precomputed uv (see the vertex shader).
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };

    // Per-triangle weighting via a *flat* (unshared-vertex) mesh: every corner of a painted triangle
    // gets weight 1, every corner of an unpainted one weight 0. This is what a coarse mesh needs - one
    // painted face of a raw cube has no strictly-interior vertex (all 8 are shared), so per-vertex
    // weighting would either bleed onto the neighbours (boundary weight 1) or vanish outright (boundary
    // weight 0, which is what made a single face show nothing). Duplicating vertices costs no shading
    // quality here because the preview shader takes its surface normal from screen-space derivatives of
    // position (dFdx/dFdy), not from a per-vertex normal. normal.y flags the UV-editor island being
    // dragged so the shader can move just that island via the island_delta uniform.
    const size_t   tri_total = patch.indices.size() + rest.indices.size();
    init_data.reserve_vertices(tri_total * 3);
    init_data.reserve_indices(tri_total * 3);
    unsigned vcount = 0;
    const auto emit_triangles = [&](const indexed_triangle_set &its, float weight, bool painted) {
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const stl_triangle_vertex_indices &tri = its.indices[f];
            // One value for the whole triangle: island_active is an interpolated varying, so the three
            // corners have to agree or the shader moves part of a triangle and not the rest.
            const float act = (painted && f < m_shaded_active_face.size() && m_shaded_active_face[f]) ? 1.f : 0.f;
            for (int i = 0; i < 3; ++i) {
                const int   idx = tri[i];
                const Vec2f uv  = (painted && m_shaded_preview_uses_vertex_uv) ? corner_uv[f * 3 + size_t(i)]
                                                                             : Vec2f::Zero();
                init_data.add_vertex(its.vertices[size_t(idx)], Vec3f(weight, act, 0.f), uv);
            }
            init_data.add_triangle(vcount, vcount + 1, vcount + 2);
            vcount += 3;
        }
    };
    emit_triangles(patch, 1.f, true); // painted -> shaded as relief, and coloured by the shader
    // Untouched surface: flat, so it still shows but carries no relief - and uncoloured, which is what the
    // bake leaves it as (EnforcerBlockerType::NONE, i.e. the volume's own filament).
    emit_triangles(rest, 0.f, false);

    m_shaded_preview_glmodel.init_from(std::move(init_data));
    // GLModel::render() unconditionally re-sets the shader's "uniform_color" from this internal
    // color field right before drawing (see GLModel.cpp) - setting the uniform manually in
    // render_shaded_preview_mesh() would just get overwritten by it, so it must be set here instead.
    // GLModel::Geometry defaults to BLACK, which is exactly what showed up before this was added.
    m_shaded_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);

    // The mesh now reflects the islands' current placement, so any live drag delta is measured from
    // here: reset it to identity and record the dragged island's baked transform.
    m_shaded_island_delta = Eigen::Matrix<float, 2, 3>::Identity();
    const TextureDisplacementLayer *al = active_layer();
    if (m_shaded_active_chart >= 0 && al != nullptr) {
        const std::vector<Eigen::Matrix<float, 2, 3>> xf = uv_editor_island_transforms(*al);
        m_shaded_baked_active_xf = (size_t(m_shaded_active_chart) < xf.size()) ? xf[size_t(m_shaded_active_chart)]
                                                                           : Eigen::Matrix<float, 2, 3>::Identity();
    } else {
        m_shaded_baked_active_xf = Eigen::Matrix<float, 2, 3>::Identity();
    }
}

void GLGizmoTextureDisplacement::compute_shaded_active_faces(const std::vector<int> &charts, size_t patch_face_count)
{
    m_shaded_active_face.clear();
    if (charts.empty() || patch_face_count == 0)
        return;
    const PatchUnwrap &u = m_uv_editor_unwrap;
    if (u.source_face.size() != u.indices.size())
        return;
    m_shaded_active_face.assign(patch_face_count, 0);
    // Flag every triangle of every chart being moved. For a group/multi move that is more than one
    // chart, but since such a move is a pure translation the shader applies the same delta to them all
    // (see on_island_edited) - exactly the "joined islands move together" behaviour.
    for (size_t t = 0; t < u.indices.size(); ++t) {
        const int f  = u.source_face[t];
        const int v0 = u.indices[t][0]; // a triangle lies in one chart, so any corner names it
        if (f < 0 || size_t(f) >= m_shaded_active_face.size() || v0 < 0 || size_t(v0) >= u.vertex_chart.size())
            continue;
        if (std::find(charts.begin(), charts.end(), u.vertex_chart[size_t(v0)]) != charts.end())
            m_shaded_active_face[size_t(f)] = 1;
    }
}

int GLGizmoTextureDisplacement::island_group_of(const std::vector<int> &groups, int c)
{
    return (c >= 0 && size_t(c) < groups.size() && groups[size_t(c)] >= 0) ? groups[size_t(c)] : c;
}

void GLGizmoTextureDisplacement::join_island_groups(std::vector<int> &groups, int a, int b, int chart_count)
{
    if (a < 0 || b < 0 || chart_count <= 0)
        return;
    // Materialise to a full explicit table first, so singletons (which were implicit) get a concrete id
    // that the relabel loop below can match on.
    if (int(groups.size()) < chart_count) {
        const size_t old = groups.size();
        groups.resize(size_t(chart_count));
        for (size_t i = old; i < groups.size(); ++i)
            groups[i] = int(i);
    }
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i] < 0)
            groups[i] = int(i);
    const int ga = groups[size_t(a)], gb = groups[size_t(b)];
    if (ga == gb)
        return;
    const int g = std::min(ga, gb);
    for (int &x : groups)
        if (x == ga || x == gb)
            x = g;
}

std::vector<int> GLGizmoTextureDisplacement::build_island_move_set(const TextureDisplacementLayer &layer, int primary) const
{
    std::vector<int> set;
    const int        chart_count = std::max(m_uv_editor_unwrap.chart_count, 0);

    // Seed with the pane's multi-selection (falling back to just the primary if the canvas has none).
    std::vector<int> seeds;
    if (const UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas())
        seeds = canvas->selected_islands();
    if (seeds.empty() && primary >= 0)
        seeds.push_back(primary);

    const auto add = [&set](int c) {
        if (c >= 0 && std::find(set.begin(), set.end(), c) == set.end())
            set.push_back(c);
    };
    for (int s : seeds) {
        add(s);
        // Pull in every chart sharing s's join group, so a joined pair moves as one.
        const int gs = island_group_of(layer.island_groups, s);
        for (int c = 0; c < chart_count; ++c)
            if (island_group_of(layer.island_groups, c) == gs)
                add(c);
    }
    add(primary); // never leave the primary out, whatever the selection state
    return set;
}

void GLGizmoTextureDisplacement::render_shaded_preview_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume  *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_shaded_preview_glmodel.is_initialized())
        return;

    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->empty())
        return;

    // Full-resolution height upload (smoothing-aware), whose grayscale value lives in the R channel
    // exactly as the shader samples it. Deliberately *not* the layer-list panel's thumbnail: that one
    // is box-filtered down to 128 px for a ~48 px row, and feeding it to the shader cost the preview
    // three quarters of the height map's detail - and, since height_tex_texel is derived from it, also
    // flattened the shading gradient and made the parallax march skip itself at angles where it should
    // run. Cached per image_data and smoothing, so no PNG is decoded per frame.
    GLTexture *tex = get_layer_height_texture(*layer);
    if (tex == nullptr || tex->get_width() <= 0 || tex->get_height() <= 0)
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("texture_displacement_shaded");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera      &camera      = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    const ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    // The shader works on world-space normals (it projects the texture in world millimetres, as the
    // bake does), so the normal matrix is the view rotation alone - no model part.
    const Matrix3d view_normal_matrix = camera.get_view_matrix().matrix().block(0, 0, 3, 3);
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.0);
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex->get_id()));
    // Match DecodedHeightTexture::sample()'s tiling. The sampler's wrap mode is the only place the
    // GPU path can express this, and nothing ever set it - so it sat at GL_REPEAT no matter what the
    // layer said: a MirroredRepeat layer previewed as a plain repeat, and a layer with tiling *off*
    // previewed as an endless tiling where the bake produces one placement and nothing around it.
    // CLAMP_TO_BORDER with a zero border is the exact analogue of sample()'s "outside [0,1) is 0".
    // GL_CLAMP_TO_BORDER is desktop-GL only; on ES the nearest thing is CLAMP_TO_EDGE, which smears
    // the border row instead of vanishing - still much closer to the bake than an endless repeat.
#if SLIC3R_OPENGL_ES
    const GLint no_tile_wrap = GL_CLAMP_TO_EDGE;
#else
    const GLint no_tile_wrap = GL_CLAMP_TO_BORDER;
#endif
    const GLint wrap = !layer->tile_enabled                                      ? no_tile_wrap :
                       (layer->tile_method == TextureTileMethod::MirroredRepeat) ? GL_MIRRORED_REPEAT :
                                                                                   GL_REPEAT;
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap));
#if !SLIC3R_OPENGL_ES
    if (wrap == GL_CLAMP_TO_BORDER) {
        static const GLfloat border[4] = { 0.f, 0.f, 0.f, 0.f };
        glsafe(::glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border));
    }
#endif // !SLIC3R_OPENGL_ES
    shader->set_uniform("height_tex", 0);
    shader->set_uniform("height_tex_texel", Vec2f(1.f / float(tex->get_width()), 1.f / float(tex->get_height())));
    shader->set_uniform("depth_mm", layer->depth_mm);
    shader->set_uniform("tiling_scale", layer->tiling_scale);
    // Read off the uploaded texture rather than the decoded one: they are the same image, and this is
    // the aspect the sampler will actually see.
    shader->set_uniform("tex_aspect", float(tex->get_width()) / float(tex->get_height()));
    shader->set_uniform("rotation_rad", layer->rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer->offset);
    shader->set_uniform("invert", layer->invert);
    // The parallax step in the triplanar path needs the real height, not just its gradient, so it
    // needs the midlevel the bake subtracts - and the camera position in the volume's own local space,
    // to build the view ray it walks along. Without the parallax the pattern is welded to the base
    // surface: it does not slide as the camera orbits and does not deepen with depth_mm, which is
    // exactly when the fast preview stops looking like geometry.
    shader->set_uniform("midlevel", layer->midlevel);
    // The texture frame is world space anchored at the volume's origin (texture_displacement_bake_frame()):
    // the shader subtracts the anchor from the world position, and the eye is handed over the same way.
    const Vec3d tex_anchor = trafo_matrix.translation();
    shader->set_uniform("tex_anchor", Vec3f(tex_anchor.cast<float>()));
    shader->set_uniform("eye_model_pos", Vec3f((camera.get_position() - tex_anchor).cast<float>()));
    // When set, the shader samples at the per-vertex uv baked into the mesh (LSCM) rather than
    // projecting; see rebuild_shaded_preview_mesh().
    shader->set_uniform("use_vertex_uv", m_shaded_preview_uses_vertex_uv);
    // Cylindrical/Spherical wrap around the painted patch's own centre, which no fragment can derive:
    // captured with the mesh (see rebuild_shaded_preview_mesh()) and handed over here. 0 is the planar
    // projection every other in-shader path uses.
    shader->set_uniform("projection_mode", m_shaded_projection_mode);
    shader->set_uniform("patch_center", m_shaded_patch_center);
    shader->set_uniform("patch_axis", m_shaded_patch_axis);

    // The printable palette, in RGB for display and in Lab for the match. Uploaded rather than matched
    // on the CPU because the quantization is per fragment here. Count 0 means "no layer is colouring",
    // and the shader keeps the model's own colour for every fragment.
    const GLTexture *color_tex = get_layer_color_texture(*layer);
    const int        palette_count =
        (color_tex != nullptr) ? int(std::min(m_shaded_preview_palette.size(), size_t(PALETTE_MAX_ENTRIES))) : 0;
    shader->set_uniform("palette_count", palette_count);
    shader->set_uniform("has_color_tex", color_tex != nullptr);
    // A flat-colour image is matched against single filaments only, as the bake does.
    shader->set_uniform("pure_only", color_tex != nullptr && analyze_texture_detail(*layer).flat_colors);
    shader->set_uniform("prefer_pure_de", PREFER_PURE_DE);
    for (int i = 0; i < palette_count; ++i) {
        const PaletteEntry &e   = m_shaded_preview_palette[size_t(i)];
        const std::string   idx = "[" + std::to_string(i) + "]";
        // An entry's colour is what it prints as: its filament's, or for a mix its mixed filament slot's.
        shader->set_uniform(("palette_rgb" + idx).c_str(), e.rgb);
        shader->set_uniform(("palette_lab" + idx).c_str(), srgb_to_lab(e.rgb));
        // Only so the shader can tell a mix (a != b) from a single filament.
        shader->set_uniform(("palette_a" + idx).c_str(), e.a);
        shader->set_uniform(("palette_b" + idx).c_str(), e.b);
    }
    if (color_tex != nullptr) {
        shader->set_uniform("color_tex", 1);
        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) color_tex->get_id()));
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }
    // The live UV-editor island drag rides this 2x3 affine (identity except mid-drag); only the flagged
    // island's vertices apply it, so a drag is a uniform update rather than a mesh rebuild.
    const Eigen::Matrix<float, 2, 3> &d = m_shaded_island_delta;
    shader->set_uniform("island_delta_lin", std::array<float, 4>{ d(0, 0), d(0, 1), d(1, 0), d(1, 1) });
    shader->set_uniform("island_delta_tr", Vec2f(d(0, 2), d(1, 2)));
    m_shaded_preview_glmodel.render();
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    shader->stop_using();
}

bool GLGizmoTextureDisplacement::shaded_preview_ready() const
{
    // Mirrors render_shaded_preview_mesh()'s own preconditions. Kept as a separate query because the
    // caller has to know whether the shaded pass will draw *before* it hides the real volume for it.
    if (m_c->selection_info() == nullptr || m_c->selection_info()->model_object() == nullptr)
        return false;
    if (texture_volume() == nullptr)
        return false;
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->empty())
        return false;
    // The same texture render_shaded_preview_mesh() will bind, not the panel thumbnail - the two are
    // separate caches and either can fail on its own.
    const GLTexture *tex = const_cast<GLGizmoTextureDisplacement *>(this)->get_layer_height_texture(*layer);
    if (tex == nullptr || tex->get_width() <= 0 || tex->get_height() <= 0)
        return false;
    return wxGetApp().get_shader("texture_displacement_shaded") != nullptr;
}

// Appends a painted patch to an overlay, lifted onto the displaced surface where that has the base mesh's
// topology (see rebuild_paint_overlay()).
static void append_paint_patch(GLModel::Geometry &out, const indexed_triangle_set &patch, const std::vector<Vec3f> *displaced)
{
    unsigned n = unsigned(out.vertices_count());
    for (const stl_triangle_vertex_indices &tri : patch.indices) {
        for (int i = 0; i < 3; ++i) {
            const size_t idx = size_t(tri[i]);
            out.add_vertex((displaced != nullptr && idx < displaced->size()) ? (*displaced)[idx] : patch.vertices[idx]);
        }
        out.add_triangle(n, n + 1, n + 2);
        n += 3;
    }
}

void GLGizmoTextureDisplacement::rebuild_other_paint_overlay()
{
    const ModelVolume *mv = texture_volume();
    // What it depends on: the volume, which layer is active, every other layer's paint (by its timestamp) and the
    // displaced positions it is lifted onto. Compared every frame, rebuilt only when it differs.
    std::string key;
    if (mv != nullptr) {
        key = std::to_string(mv->id().id) + ":" + std::to_string(m_active_layer_slot) + (m_use_shaded_preview ? ":b:" : ":t:") +
              std::to_string(reinterpret_cast<uintptr_t>(m_preview_its.vertices.data())) + ":" +
              std::to_string(m_preview_its.vertices.size());
        for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.slot != m_active_layer_slot && l.slot >= 0 && l.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
                key += "|" + std::to_string(l.slot) + "@" + std::to_string(mv->texture_displacement_facet(l.slot).timestamp());
    }
    if (key == m_other_paint_key)
        return;
    m_other_paint_key = std::move(key);
    m_other_paint_glmodel.reset();
    if (mv == nullptr)
        return;

    const std::vector<Vec3f> *displaced = nullptr;
    if (!m_use_shaded_preview && m_preview_its.vertices.size() == mv->mesh().its.vertices.size() &&
        !m_preview_its.vertices.empty())
        displaced = &m_preview_its.vertices;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers) {
        if (l.slot == m_active_layer_slot || l.slot < 0 || l.slot >= int(TEXTURE_DISPLACEMENT_MAX_LAYERS) ||
            mv->texture_displacement_facet(l.slot).empty())
            continue;
        TriangleSelector selector(mv->mesh());
        selector.deserialize(mv->texture_displacement_facet(l.slot).get_data(), false);
        append_paint_patch(init_data, selector.get_facets_strict(EnforcerBlockerType::ENFORCER), displaced);
    }
    if (init_data.is_empty())
        return;
    m_other_paint_glmodel.init_from(std::move(init_data));
    // Neutral grey: painted, but not the layer the brush is working on.
    m_other_paint_glmodel.set_color(ColorRGBA(0.55f, 0.58f, 0.60f, 0.35f));
}

std::vector<size_t> GLGizmoTextureDisplacement::PaintedColors::outside(const std::vector<bool> &excluded) const
{
    std::vector<size_t> out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        if (size_t(source[i]) >= excluded.size() || !excluded[size_t(source[i])])
            out.push_back(i);
    return out;
}

GLGizmoTextureDisplacement::PaintedColors GLGizmoTextureDisplacement::painted_colors(const TriangleMesh &mesh,
                                                                                     const TriangleSelector::TriangleSplittingData &paint)
{
    PaintedColors out;
    TriangleSelector selector(mesh);
    selector.deserialize(paint, false);
    for (const EnforcerBlockerType state : TriangleSelector::extract_used_facet_states(paint)) {
        if (state == EnforcerBlockerType::NONE)
            continue;
        std::vector<int>           source;
        const indexed_triangle_set part = selector.get_facets_strict(state, &source);
        // Every state comes back over the same vertex array, only the triangles differ.
        if (out.facets.vertices.empty())
            out.facets.vertices = part.vertices;
        out.facets.indices.insert(out.facets.indices.end(), part.indices.begin(), part.indices.end());
        out.source.insert(out.source.end(), source.begin(), source.end());
        out.state.resize(out.facets.indices.size(), int(state));
    }
    return out;
}

void GLGizmoTextureDisplacement::rebuild_painted_colors(bool whole_stack)
{
    const ModelVolume *mv    = texture_volume();
    const bool         shown = mv != nullptr && any_layer_colors(*mv) && !mv->mmu_segmentation_facets.empty();

    // The sub-triangles, keyed on what they were read from: the volume, its mesh and the paint.
    std::string key;
    if (shown)
        key = std::to_string(mv->id().id) + ":" + std::to_string(reinterpret_cast<uintptr_t>(mv->mesh_ptr().get())) + ":" +
              std::to_string(mv->mmu_segmentation_facets.timestamp());
    if (key != m_painted_colors_key) {
        m_painted_colors_key = std::move(key);
        m_painted_colors     = shown ? painted_colors(mv->mesh(), mv->mmu_segmentation_facets.get_data()) : PaintedColors{};
        m_painted_colors_drawn_key.clear();
        m_painted_colors_glmodel.reset();
        m_painted_colors_runs.clear();
    }
    if (m_painted_colors.state.empty())
        return;

    // The part drawn, keyed on whose paint is left out and on that paint.
    std::string drawn_key = m_painted_colors_key + (whole_stack ? std::string(":all") : ":" + std::to_string(m_active_layer_slot));
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot >= 0 && l.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
            drawn_key += "|" + std::to_string(l.slot) + "@" + std::to_string(mv->texture_displacement_facet(l.slot).timestamp());
    if (drawn_key == m_painted_colors_drawn_key)
        return;
    m_painted_colors_drawn_key = std::move(drawn_key);
    m_painted_colors_glmodel.reset();
    m_painted_colors_runs.clear();

    // Whole model triangles, as the facets record what they touch: a triangle the paint only partly covers
    // is left to the preview.
    std::vector<bool> excluded(mv->mesh().its.indices.size(), false);
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers) {
        if (l.slot < 0 || l.slot >= int(TEXTURE_DISPLACEMENT_MAX_LAYERS) || (!whole_stack && l.slot != m_active_layer_slot))
            continue;
        for (const TriangleSelector::TriangleBitStreamMapping &m : mv->texture_displacement_facet(l.slot).get_data().triangles_to_split)
            if (m.triangle_idx >= 0 && size_t(m.triangle_idx) < excluded.size())
                excluded[size_t(m.triangle_idx)] = true;
    }

    // One model, its triangles in filament order (painted_colors() groups them), drawn a range per filament.
    const std::vector<size_t> kept = m_painted_colors.outside(excluded);
    GLModel::Geometry         init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(kept.size() * 3);
    init_data.reserve_indices(kept.size() * 3);
    unsigned n = 0;
    for (const size_t t : kept) {
        const int state = m_painted_colors.state[t];
        if (m_painted_colors_runs.empty() || m_painted_colors_runs.back().first != state)
            m_painted_colors_runs.push_back({ state, { size_t(n), size_t(n) } });
        for (int i = 0; i < 3; ++i)
            init_data.add_vertex(m_painted_colors.facets.vertices[size_t(m_painted_colors.facets.indices[t][i])]);
        init_data.add_triangle(n, n + 1, n + 2);
        n += 3;
        m_painted_colors_runs.back().second.second = size_t(n);
    }
    if (!init_data.is_empty())
        m_painted_colors_glmodel.init_from(std::move(init_data));
}

bool GLGizmoTextureDisplacement::render_painted_colors(bool whole_stack)
{
    rebuild_painted_colors(whole_stack);
    const ModelObject *mo     = m_c->selection_info()->model_object();
    const ModelVolume *mv     = texture_volume();
    GLShaderProgram   *shader = wxGetApp().get_shader("mm_gouraud");
    if (mo == nullptr || mv == nullptr || shader == nullptr || !m_painted_colors_glmodel.is_initialized())
        return false;

    const Selection             &selection    = m_parent.get_selection();
    const Transform3d            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera                &camera       = wxGetApp().plater()->get_camera();
    const Transform3d           &view_matrix  = camera.get_view_matrix();
    const Matrix3d               normal_matrix = trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    const std::vector<ColorRGBA> colors        = wxGetApp().plater()->get_extruders_colors();

    shader->start_using();
    // Set up as render_triangles() sets it up, so the colours are lit, clipped and slope-marked exactly as the
    // neutral surface they cover.
    const ClippingPlaneDataWrapper clp_data = get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix", Matrix3d(view_matrix.matrix().block(0, 0, 3, 3) * normal_matrix));
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.);
    shader->set_uniform("slope.actived", m_parent.is_using_slope());
    shader->set_uniform("slope.volume_world_normal_matrix", Matrix3f(normal_matrix.cast<float>()));
    shader->set_uniform("slope.normal_z", float(-std::cos(Geometry::deg2rad(m_highlight_by_angle_threshold_deg))));
    shader->set_uniform("slope.up_direction", get_tilt_up_direction());
    shader->set_uniform("show_wireframe", false);
    // Pulled forward and without depth writes, as the tint is (see render_paint_overlay()). The tint is
    // drawn after this, so it shows on top.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.f, -2.f));
    glsafe(::glDepthMask(GL_FALSE));
    bool drawn = false;
    for (const auto &[state, range] : m_painted_colors_runs)
        if (state >= 1 && size_t(state) <= colors.size()) {
            m_painted_colors_glmodel.set_color(adjust_color_for_rendering(colors[size_t(state - 1)]));
            m_painted_colors_glmodel.render(range, shader);
            drawn = true;
        }
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
    return drawn;
}

void GLGizmoTextureDisplacement::rebuild_paint_overlay()
{
    m_paint_overlay_glmodel.reset();
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;

    // The *live* selector, so an in-progress stroke shows immediately - which is the whole point:
    // this is the only feedback that a brush actually added or erased anything until the (much more
    // expensive) preview catches up at stroke end.
    const indexed_triangle_set patch = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER);
    if (patch.indices.empty())
        return;

    // In the true-displacement view the surface on screen is the *raised* one, and a tint built on
    // the flat base mesh would sink underneath it wherever the relief is deepest - which is precisely
    // where the user is looking. The bake is topology-preserving (patch vertex i is mesh vertex i, see
    // build_texture_displacement()), so the displaced positions can be read straight across. Vertices
    // the brush split live past the end of that array and keep their flat position; they sit on the
    // patch boundary, where the displacement is smallest anyway.
    const std::vector<Vec3f> *displaced = nullptr;
    if (!m_use_shaded_preview && m_preview_its.vertices.size() == mv->mesh().its.vertices.size() &&
        !m_preview_its.vertices.empty())
        displaced = &m_preview_its.vertices;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(patch.indices.size() * 3);
    init_data.reserve_indices(patch.indices.size() * 3);
    append_paint_patch(init_data, patch, displaced);
    m_paint_overlay_glmodel.init_from(std::move(init_data));
    // GLModel::render() re-sets "uniform_color" from this field just before drawing, so the colour
    // has to be set here rather than as a uniform at draw time.
    m_paint_overlay_glmodel.set_color(ColorRGBA(0.16f, 0.79f, 0.35f, 0.38f));
}

void GLGizmoTextureDisplacement::render_paint_overlay(GLModel &overlay)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !overlay.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Translucent, and pulled toward the camera so it wins the depth test against the coincident
    // shaded surface - and against the selectors' highlight at -1 in the Normal view, by the full depth
    // unit OpenGL guarantees to tell apart. Depth writes are off: this is a tint, and letting it own the
    // depth buffer would make the wireframe and seam overlays drawn after it fight with geometry that is
    // not really there. Blending is already enabled by render_painter_gizmo().
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.f, -2.f));
    glsafe(::glDepthMask(GL_FALSE));
    overlay.render();
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

const std::vector<int> *GLGizmoTextureDisplacement::island_highlight_selection()
{
    const TextureDisplacementLayer *al        = active_layer();
    const UVEditorCanvas           *uv_canvas = wxGetApp().plater()->get_uv_editor_canvas();
    if (m_show_uv_editor && al != nullptr && al->projection_method == TextureProjectionMethod::LSCM &&
        uv_canvas != nullptr && !m_uv_editor_unwrap.empty())
        return &uv_canvas->selected_islands();
    return nullptr;
}

void GLGizmoTextureDisplacement::rebuild_island_overlay(const std::vector<int> &selection)
{
    m_island_overlay_glmodel.reset();
    m_island_overlay_selection = selection;
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || selection.empty() || m_uv_editor_unwrap.empty())
        return;
    // The unwrap was made from the painted patch in the bake frame; the same extraction on the
    // volume's own mesh gives the same triangles and vertex order in local coordinates, which is the
    // frame the overlay is drawn in (with the volume's transform, like the paint tint).
    const indexed_triangle_set patch = extract_painted_patch(mv->mesh().its, m_uv_editor_state.facets);
    const PatchUnwrap         &uw    = m_uv_editor_unwrap;
    std::vector<uint8_t>       chosen(size_t(std::max(uw.chart_count, 0)), 0);
    for (const int c : selection)
        if (c >= 0 && size_t(c) < chosen.size())
            chosen[size_t(c)] = 1;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    unsigned n = 0;
    for (const stl_triangle_vertex_indices &tri : uw.indices) {
        const int v0 = tri[0];
        if (v0 < 0 || size_t(v0) >= uw.vertex_chart.size())
            continue;
        const int c = uw.vertex_chart[size_t(v0)];
        if (c < 0 || size_t(c) >= chosen.size() || !chosen[size_t(c)])
            continue;
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            const int u = tri[k];
            ok = u >= 0 && size_t(u) < uw.source_vertex.size() && uw.source_vertex[size_t(u)] >= 0 &&
                 size_t(uw.source_vertex[size_t(u)]) < patch.vertices.size();
        }
        if (!ok)
            continue;
        for (int k = 0; k < 3; ++k)
            init_data.add_vertex(patch.vertices[size_t(uw.source_vertex[size_t(tri[k])])]);
        init_data.add_triangle(n, n + 1, n + 2);
        n += 3;
    }
    if (n == 0)
        return;
    m_island_overlay_glmodel.init_from(std::move(init_data));
    m_island_overlay_glmodel.set_color(ColorRGBA(0.10f, 0.55f, 0.95f, 0.45f)); // the pane's selection blue
}

void GLGizmoTextureDisplacement::render_island_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_island_overlay_glmodel.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Above the paint tint (a larger offset), translucent, no depth writes - a marker, not geometry.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.0f, -2.0f));
    glsafe(::glDepthMask(GL_FALSE));
    m_island_overlay_glmodel.render();
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::rebuild_uvcheck_mesh()
{
    m_uvcheck_glmodel.reset();
    if (m_uv_check_mode == UVCheckMode::None)
        return;

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;
    std::vector<int>           patch_source; // only ModelUV reads it; see compute_layer_vertex_uvs()
    const indexed_triangle_set patch =
        m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER, &patch_source);
    if (patch.indices.empty())
        return;
    // Everything below works in the *patch's* vertex space, not the mesh's. Those agree only until a
    // brush stroke splits a triangle, after which the patch array is longer - and since patch triangle
    // indices are used to index it, reading the mesh's array instead would run off the end. An earlier
    // version guarded that by bailing out, which quietly disabled the Checker and Distortion overlays
    // for anything painted with the brush.
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;

    // The checker samples wherever the projection puts it; the projections the shader can't
    // reconstruct (LSCM, ViewProjected) get a precomputed per-vertex uv, the rest project in-shader.
    std::vector<Vec2f> uv       = compute_layer_vertex_uvs(patch, *layer, &patch_source);
    const bool         have_uvs = uv.size() == patch.vertices.size();
    m_uvcheck_uses_vertex_uv    = have_uvs;
    m_uvcheck_projection_mode   = have_uvs ? 0 :
                                      layer_projection_frame(patch, *layer, m_uvcheck_patch_center, m_uvcheck_patch_axis);
    // Per corner as well: under LSCM a seam vertex has a
    // different uv in each island it borders, so the shared-vertex form drew one triangle per face from
    // a neighbouring island's placement. Only the *drawing* needs this; the distortion metric below is
    // a per-vertex average by construction and keeps using `uv`.
    const std::vector<Vec2f> corner_uv = compute_layer_corner_uvs(patch, *layer, &patch_source);
    const bool               have_corner_uvs = have_uvs && corner_uv.size() == patch.indices.size() * 3;

    // Per-vertex area distortion in [0,1] (0.5 == ideal), only when both requested and possible.
    std::vector<float> distortion(patch.vertices.size(), 0.5f);
    if (m_uv_check_mode == UVCheckMode::Distortion && have_uvs) {
        std::vector<float> tri_log(patch.indices.size(), 0.f);
        for (size_t f = 0; f < patch.indices.size(); ++f) {
            const stl_triangle_vertex_indices &t = patch.indices[f];
            const float a3 = 0.5f * (patch.vertices[t[1]] - patch.vertices[t[0]]).cross(patch.vertices[t[2]] - patch.vertices[t[0]]).norm();
            const Vec2f e0 = uv[t[1]] - uv[t[0]];
            const Vec2f e1 = uv[t[2]] - uv[t[0]];
            const float a2 = 0.5f * std::abs(e0.x() * e1.y() - e0.y() * e1.x());
            tri_log[f] = (a3 > 1e-12f && a2 > 1e-12f) ? std::log2(a2 / a3) : 0.f;
        }
        // Centre the heatmap on the patch's own median stretch, so a globally-scaled unwrap reads as
        // uniformly "ideal" and only *relative* stretching (the thing that matters) shows up as colour.
        std::vector<float> sorted = tri_log;
        float              median = 0.f;
        if (!sorted.empty()) {
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            median = sorted[sorted.size() / 2];
        }
        std::vector<float> sum(patch.vertices.size(), 0.f);
        std::vector<int>   cnt(patch.vertices.size(), 0);
        for (size_t f = 0; f < patch.indices.size(); ++f) {
            // +/- 2 stops (4x stretch either way) spans the full blue->red range.
            const float d = std::clamp(0.5f + (tri_log[f] - median) / 4.f, 0.f, 1.f);
            for (int k = 0; k < 3; ++k) {
                sum[patch.indices[f][k]] += d;
                ++cnt[patch.indices[f][k]];
            }
        }
        for (size_t v = 0; v < distortion.size(); ++v)
            if (cnt[v] > 0)
                distortion[v] = sum[v] / float(cnt[v]);
    }

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };
    // Flat (one vertex per triangle corner), so each triangle can carry its own island's uv - see
    // have_corner_uvs above. Costs nothing in shading quality: the overlay shades from uv and the
    // interpolated distortion value alone, never from a per-vertex normal.
    init_data.reserve_vertices(patch.indices.size() * 3);
    init_data.reserve_indices(patch.indices.size() * 3);
    unsigned vcount = 0;
    for (size_t f = 0; f < patch.indices.size(); ++f) {
        const stl_triangle_vertex_indices &tri = patch.indices[f];
        for (int k = 0; k < 3; ++k) {
            const size_t vi = size_t(tri[k]);
            init_data.add_vertex(patch.vertices[vi], Vec3f(distortion[vi], 0.f, 0.f),
                                 have_corner_uvs ? corner_uv[f * 3 + size_t(k)] :
                                                   (have_uvs ? uv[vi] : Vec2f::Zero()));
        }
        init_data.add_triangle(vcount, vcount + 1, vcount + 2);
        vcount += 3;
    }

    m_uvcheck_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::render_uvcheck_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_uvcheck_glmodel.is_initialized())
        return;
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("texture_displacement_uvcheck");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    const ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    // The shader works on world-space normals (it projects the texture in world millimetres, as the
    // bake does), so the normal matrix is the view rotation alone - no model part.
    const Matrix3d view_normal_matrix = camera.get_view_matrix().matrix().block(0, 0, 3, 3);
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.0);
    shader->set_uniform("tex_anchor", Vec3f(trafo_matrix.translation().cast<float>())); // see the preview shader
    shader->set_uniform("mode", m_uv_check_mode == UVCheckMode::Distortion ? 1 : 0);
    shader->set_uniform("checker_freq", 4.f); // squares per texture tile
    shader->set_uniform("tiling_scale", layer->tiling_scale);
    shader->set_uniform("rotation_rad", layer->rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer->offset);
    shader->set_uniform("use_vertex_uv", m_uvcheck_uses_vertex_uv);
    shader->set_uniform("projection_mode", m_uvcheck_projection_mode);
    shader->set_uniform("patch_center", m_uvcheck_patch_center);
    shader->set_uniform("patch_axis", m_uvcheck_patch_axis);
    // Was never uploaded, so the checker disagreed with the bake for any non-square height map.
    shader->set_uniform("tex_aspect", layer_texture_aspect(*layer));

    // Coincident with the base surface, so pull it toward the camera to win the depth test.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    m_uvcheck_glmodel.render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::build_wireframe_from_its(const indexed_triangle_set &its)
{
    m_wireframe_overlay_glmodel.reset();
    m_wireframe_overlay_vcount = its.vertices.size();
    if (its.indices.empty())
        return;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(its.vertices.size());
    init_data.reserve_indices(its.indices.size() * 6);
    for (const Vec3f &v : its.vertices)
        init_data.add_vertex(v);
    // One segment per triangle edge; shared edges drawn twice, harmless for a wireframe and far
    // cheaper than deduplicating a million of them.
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i)
            init_data.add_line(unsigned(tri[i]), unsigned(tri[(i + 1) % 3]));

    if (!init_data.is_empty())
        m_wireframe_overlay_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::rebuild_wireframe_overlay()
{
    if (!m_wireframe_overlay) {
        m_wireframe_overlay_glmodel.reset();
        m_wireframe_overlay_vcount = 0;
        return;
    }
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;
    const indexed_triangle_set &its = mv->mesh().its;
    if (its.indices.empty())
        return;

    // Building from the base mesh (shaded/paint mode); its topology only changes on bake/subdivide, and
    // this runs on every rebuild_preview(), so rebuild only when the vertex count actually changes.
    if (m_wireframe_overlay_glmodel.is_initialized() && m_wireframe_overlay_vcount == its.vertices.size())
        return;
    build_wireframe_from_its(its);
}

void GLGizmoTextureDisplacement::refresh_wireframe()
{
    if (!m_wireframe_overlay) {
        m_wireframe_overlay_glmodel.reset();
        m_wireframe_overlay_vcount = 0;
        return;
    }
    // The wireframe has to sit on whatever mesh is actually on screen. In the true-displacement view
    // that is the raised preview geometry (m_preview_its) - drawing the flat base mesh's edges there
    // leaves them buried inside the relief, which is why the wireframe "didn't show in real mode". In
    // Fast (shaded) mode or with nothing painted, the surface is the undisplaced base mesh.
    if (!m_use_shaded_preview && !m_preview_its.indices.empty())
        build_wireframe_from_its(m_preview_its);
    else
        rebuild_wireframe_overlay();
}

void GLGizmoTextureDisplacement::render_wireframe_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_wireframe_overlay_glmodel.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Pull the lines toward the camera so they sit on the surface rather than z-fighting into it.
    glsafe(::glEnable(GL_POLYGON_OFFSET_LINE));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    m_wireframe_overlay_glmodel.set_color(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f)); // white, so it reads on any material
    m_wireframe_overlay_glmodel.render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_LINE));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::rebuild_preview()
{
    // Raised first: any in-flight job's result (captured generation from before this call) will
    // now compare unequal to m_preview_generation and be discarded when it completes, even if it
    // finishes after the job queued below - and, since the counter is shared with the worker, that
    // job also notices mid-run and aborts rather than computing a result nobody will use.
    m_preview_generation->fetch_add(1);
    // Everything rebuilt from here on reads the current palette. Cleared before any of the early returns
    // below, which would otherwise leave it set and re-run this every frame.
    m_palette_changed = false;
    update_uv_editor();
    rebuild_shaded_preview_mesh();
    rebuild_paint_overlay();
    rebuild_uvcheck_mesh();
    rebuild_seam_overlay();
    // The adaptive subdivision preview is driven by the painted area, so it has to follow the paint
    // while it is open - a plain stroke changes which triangles would be refined. The uniform preview
    // depends only on the mesh, so it is left to its own controls.
    if (m_subdivide_editing && m_subdivide_adaptive)
        rebuild_subdivide_preview();

    // The debug view owns m_preview_glmodel while it is up. A stage is a snapshot of a run that has
    // already finished, so letting the live preview overwrite it would replace the thing being
    // inspected with something else; leaving the view is explicit (its Close button).
    if (m_debug_stage >= 0)
        return;

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || !any_part_painted()) {
        m_preview_glmodel.reset();
        m_part_previews.clear();
        m_preview_its = indexed_triangle_set{}; // no displaced mesh; wireframe falls back to the base
        m_preview_job_pending = false;
        refresh_wireframe();
        return;
    }
    // In Fast/paint modes the wireframe follows the base mesh and can be built now; the true-displacement
    // view's wireframe needs the displaced mesh, which only exists once the job below completes.
    if (m_use_shaded_preview) {
        refresh_wireframe();
        // Fast view: the shader *is* the preview, and m_preview_glmodel is never drawn. Running the
        // full CPU displacement anyway - which is what happened on every stroke and slider release -
        // was the single largest cost in the gizmo, and it bought nothing. The switch back to the
        // true-displacement view queues it (see the View row in on_render_input_window()).
        m_preview_job_pending = false;
        return;
    }

    queue_preview_job();
}

indexed_triangle_set GLGizmoTextureDisplacement::group_preview_by_color(const indexed_triangle_set   &its,
                                                                        const std::vector<uint8_t>   &triangle_color,
                                                                        const std::vector<ColorRGBA> &entry_colors,
                                                                        std::vector<PreviewColorRun> &runs)
{
    runs.clear();
    if (triangle_color.size() != its.indices.size() || entry_colors.empty())
        return its;

    indexed_triangle_set sorted;
    sorted.vertices = its.vertices;
    sorted.indices.reserve(its.indices.size());
    for (int want = 0; want <= int(entry_colors.size()); ++want) {
        const size_t first = sorted.indices.size();
        for (size_t i = 0; i < its.indices.size(); ++i)
            if (int(triangle_color[i]) == want)
                sorted.indices.push_back(its.indices[i]);
        if (sorted.indices.size() == first)
            continue;
        runs.push_back({ { first * 3, sorted.indices.size() * 3 },
                         want == 0 ? GLVolume::NEUTRAL_COLOR : entry_colors[size_t(want - 1)] });
    }
    return sorted;
}

void GLGizmoTextureDisplacement::queue_preview_job()
{
    // A job in flight when the gizmo closes still runs its completion handler, which would otherwise
    // happily queue the follow-up run it was holding - against a gizmo nobody is looking at any more.
    if (m_state != On)
        return;
    const ModelVolume *owner = texture_volume();
    if (owner == nullptr)
        return;
    // Every painted part, so the preview shows the whole of what Bake will do rather than its first
    // part - the same set bake() displaces.
    const std::vector<ModelVolume *> parts = painted_parts();
    if (parts.empty())
        return;

    // One in flight at a time; everything requested meanwhile collapses into a single follow-up run
    // issued from the completion handler. See m_preview_job_running.
    if (m_preview_job_running) {
        m_preview_job_pending = true;
        return;
    }

    const uint64_t generation = m_preview_generation->load();

    TextureDisplacementPreviewInput input;
    // Captured here rather than read in the handler: the palette is main-thread state, and the preview
    // has to be grouped against the same palette it was computed with, not whatever it is by the time
    // the result lands. Read off the layer stack's own part, since that is where the stack lives.
    input.color = color_settings_for(*owner);
    input.volumes.reserve(parts.size());
    // Which part each result belongs to, so the handler can tell the layer stack's own preview - the one
    // the overlays and the wireframe are keyed to - from the rest.
    std::vector<ObjectID> part_ids;
    part_ids.reserve(parts.size());
    const Vec3d              anchor = texture_anchor();
    const std::vector<float> drops  = resting_drops(parts);
    for (size_t i = 0; i < parts.size(); ++i) {
        const ModelVolume               *part = parts[i];
        TextureDisplacementPreviewVolume entry;
        entry.base_mesh = part->mesh().its;
        entry.layers    = layers_for_part(*part);
        entry.options   = owner->texture_displacement_options;
        // The preview is looked at, not printed or sliced: skip the simplification and the repair that
        // follows it, which are most of the one-run pipeline's time. The relief is the same.
        if (entry.options.pipeline_v2)
            entry.options.v2_max_triangles_k = 0;
        entry.volume_to_world = texture_displacement_volume_to_world(*part);
        entry.texture_anchor  = anchor;
        entry.resting_drop    = drops[i];
        if (part->has_texture_uv())
            entry.model_uv = part->texture_uv;
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            entry.facets_data[size_t(slot)] = part->texture_displacement_facet(slot).get_data();
        input.volumes.push_back(std::move(entry));
        part_ids.push_back(part->id());
    }
    const ObjectID owner_id = owner->id();

    // The result names a palette entry per triangle (index + 1), so these are the colours to draw it in:
    // a single filament's own, or for a mix the colour its slot will show once a bake creates it.
    std::vector<ColorRGBA> entry_colors;
    entry_colors.reserve(input.color.palette.size());
    for (const PrintableColor &e : input.color.palette)
        entry_colors.emplace_back(e.rgb.x(), e.rgb.y(), e.rgb.z(), 1.f);

    m_preview_job_running = true;
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementPreviewJob>(std::move(input), generation, m_preview_generation,
        [this, entry_colors, part_ids, owner_id](std::vector<TextureDisplacementPreviewResult> results,
                                                 uint64_t result_generation) {
            m_preview_job_running = false;
            if (result_generation != m_preview_generation->load()) {
                // Superseded while this was computing (it will have aborted early and come back
                // empty). Whatever the newest state is, it still needs a run.
                m_preview_job_pending = true;
            } else if (results.empty()) {
                // Aborted or cancelled rather than finished - the handler runs on every outcome so
                // the in-flight latch above always clears. Keep whatever preview is already on screen
                // rather than blanking it; there is no new result to show, not a new empty one.
            } else {
                // A finished run carries the whole object, so it replaces the previous one outright: a
                // part that is no longer painted must lose its preview, or its flat volume would stay
                // hidden behind geometry from before.
                m_preview_glmodel.reset();
                m_preview_color_runs.clear();
                m_preview_its = indexed_triangle_set{};
                m_part_previews.clear();

                for (size_t i = 0; i < results.size() && i < part_ids.size(); ++i) {
                    indexed_triangle_set its = std::move(results[i].mesh);
                    if (its.indices.empty())
                        continue; // this part's layers moved nothing; its own volume is drawn instead

                    std::vector<PreviewColorRun> runs;
                    indexed_triangle_set         grouped =
                        group_preview_by_color(its, results[i].triangle_color, entry_colors, runs);

                    if (part_ids[i] == owner_id) {
                        m_preview_color_runs = std::move(runs);
                        m_preview_glmodel.init_from(grouped);
                        m_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);
                        // Keep the displaced mesh so the wireframe overlay can be drawn on the raised
                        // surface actually shown in the true-displacement view.
                        m_preview_its = std::move(its);
                    } else {
                        auto preview        = std::make_unique<PartPreview>();
                        preview->volume_id  = part_ids[i];
                        preview->color_runs = std::move(runs);
                        preview->glmodel.init_from(grouped);
                        preview->glmodel.set_color(GLVolume::NEUTRAL_COLOR);
                        m_part_previews.push_back(std::move(preview));
                    }
                }

                refresh_wireframe();
                // The paint tint rides the displaced surface in this view, so it follows the new mesh.
                m_paint_overlay_dirty = true;
            }
            if (m_preview_job_pending) {
                m_preview_job_pending = false;
                if (!m_use_shaded_preview)
                    queue_preview_job(); // no-ops if the gizmo has closed in the meantime
            }
            m_parent.set_as_dirty();
        }));
}

void GLGizmoTextureDisplacement::update_uv_editor()
{
    Plater         *plater    = wxGetApp().plater();
    UVEditorCanvas *uv_canvas = plater->get_uv_editor_canvas();
    if (uv_canvas == nullptr)
        return;
    // Wired on every call rather than with the first unwrap: Unwrap itself is one of the pane's commands.
    uv_canvas->set_command_callback([this](UVEditorCanvas::Command cmd, float value) { on_uv_command(int(cmd), value); });

    const ModelVolume        *mv    = texture_volume();
    TextureDisplacementLayer *layer = active_layer();
    // The pane is opened only on the user's explicit request (m_show_uv_editor), and only for an LSCM
    // layer - never automatically just because something is painted. Keep the cached state/unwrap so
    // that switching the toggle back on re-shows instantly (and re-solves if the paint changed while
    // it was hidden, via the state comparison below).
    if (!m_show_uv_editor || mv == nullptr || layer == nullptr ||
        layer->projection_method != TextureProjectionMethod::LSCM) {
        m_uv_editor_bg = UVBackground::None; // the next time it is shown, upload the background afresh
        push_uv_pane_state();
        plater->show_uv_editor(false);
        return;
    }

    // A vertex/edge edit committing (or an undo reverting one) changes the per-vertex UV overrides
    // without going through the Unwrap button. Detect that and force a re-solve, so the pane's geometry
    // stays in step with what will bake - the one exception to "only re-solve on Unwrap".
    {
        size_t sig = 1469598103934665603ull; // FNV-1a seed
        const auto mix = [&sig](uint64_t x) { sig = (sig ^ x) * 1099511628211ull; };
        mix(layer->lscm_uv_overrides.size());
        for (const auto &[v, uv] : layer->lscm_uv_overrides) {
            mix(uint64_t(uint32_t(v)));
            mix(uint64_t(uint32_t(int32_t(std::llround(uv.x() * 1024.f)))));
            mix(uint64_t(uint32_t(int32_t(std::llround(uv.y() * 1024.f)))));
        }
        if (sig != m_uv_overrides_sig) {
            m_uv_overrides_sig  = sig;
            m_uv_unwrap_pending = true;
        }
    }

    UVEditorState state;
    state.slot       = m_active_layer_slot;
    state.image_data = layer->image_data.get();
    state.seam_angle = layer->lscm_seam_angle_deg;
    state.padding    = layer->island_padding_mm;
    state.facets     = mv->texture_displacement_facet(m_active_layer_slot).get_data();
    state.seam_edges = layer->lscm_seam_edges;

    // The re-solve happens only when the user pressed "Unwrap" (m_uv_unwrap_pending). Every other call
    // into here - a paint stroke ending, a slider release, the check mode changing - must not pay for
    // a fresh LSCM solve; it just re-applies the cheap affine transforms over whatever unwrap already
    // exists. If the paint changed underneath but the user hasn't asked to re-unwrap, the pane keeps
    // showing the last unwrap on purpose (that is the whole point of making it an explicit action).
    bool unwrap_changed = false;
    if (m_uv_unwrap_pending) {
        m_uv_unwrap_pending = false;
        // World millimetres, the space the bake unwraps in - otherwise the pane would lay the islands
        // out at the volume's own scale and show the texture at a different size than it bakes at.
        const indexed_triangle_set patch = patch_in_world(extract_painted_patch(mv->mesh().its, state.facets));
        if (patch.indices.empty()) {
            // Nothing painted to unwrap: clear what the pane showed but keep it open - its status line says what to do.
            m_uv_editor_state  = UVEditorState{};
            m_uv_editor_unwrap = PatchUnwrap{};
            m_uv_editor_distortion_colors.clear();
            uv_canvas->set_islands({});
            push_uv_pane_state();
            plater->show_uv_editor(true);
            return;
        }
        // Padding disabled (0): the user asked to pack islands with no gap between them.
        m_uv_editor_unwrap = compute_patch_unwrap(patch, layer->lscm_seam_angle_deg, 0.f, layer->lscm_seam_edges);
        m_island_overlay_glmodel.reset(); // the islands were renumbered: rebuilt from the pane's selection next frame
        m_island_overlay_selection.clear();
        // Re-apply any stored UV edits onto the fresh unwrap, so the pane shows exactly what
        // compute_lscm_uvs() will bake (which applies the same overrides).
        apply_lscm_uv_overrides(m_uv_editor_unwrap, layer->lscm_uv_overrides);
        m_uv_editor_state  = std::move(state);
        unwrap_changed     = true;
        // Precompute the distortion heatmap now, while the patch is in hand - relative stretch doesn't
        // change when islands are only moved, so this need not be redone on a drag. It is fed to the
        // canvas below only while the Distortion check mode is on.
        compute_uv_editor_distortion_colors(patch);
    }

    // The pane background either mirrors the height texture (default) or shows a UV checker (#7). It is uploaded
    // before, and independently of, the unwrap - the texture belongs on screen before anything is unwrapped - and
    // only when the choice, the layer's image or its smoothing changed, because the height image is large.
    const UVBackground desired_bg = (m_uv_check_mode == UVCheckMode::Checker) ? UVBackground::Checker : UVBackground::Height;
    const bool bg_smoothing_changed = (desired_bg == UVBackground::Height) &&
                                      (m_uv_editor_bg_smoothing != layer->smoothing ||
                                       !(m_uv_editor_bg_adjust == layer->preview_adjust));
    if (desired_bg != m_uv_editor_bg || layer->image_data.get() != m_uv_editor_bg_image || bg_smoothing_changed) {
        m_uv_editor_bg_smoothing = layer->smoothing;
        m_uv_editor_bg_adjust    = layer->preview_adjust;
        m_uv_editor_bg_image     = layer->image_data.get();
        m_uv_editor_bg           = desired_bg;
        if (desired_bg == UVBackground::Checker) {
            // An even squares-per-axis count so the pattern tiles seamlessly across the UV unit
            // boundary (texcoord == position repeats it once per tile). Softened grays, not pure
            // black/white, so it doesn't fight the island wires drawn over it.
            constexpr int              tex = 512, squares = 8, cell = tex / squares;
            std::vector<unsigned char> checker(size_t(tex) * size_t(tex));
            for (int y = 0; y < tex; ++y)
                for (int x = 0; x < tex; ++x)
                    checker[size_t(y) * tex + x] = ((x / cell + y / cell) & 1) ? 205 : 70;
            uv_canvas->set_background_texture(checker, tex, tex);
        } else {
            const DecodedHeightTexture height = decode_height_texture(*layer);
            if (!height.empty()) {
                uv_canvas->set_background_texture(height.pixels, height.width, height.height);
            } else {
                // Not recorded as uploaded, so the next update tries again rather than keeping a blank backdrop.
                uv_canvas->set_background_texture({}, 0, 0);
                m_uv_editor_bg = UVBackground::None;
            }
        }
    }
    // The backdrop's extent and repeat follow the tile settings, with or without an unwrap.
    uv_canvas->set_uv_transform(layer->tiling_scale, layer->rotation_deg, layer->tile_enabled,
                                layer->tile_method == TextureTileMethod::MirroredRepeat);

    if (m_uv_editor_unwrap.empty()) {
        // Nothing unwrapped yet: the pane opens anyway, because Unwrap itself lives in it. Its canvas shows the
        // texture and its status line says to paint the area and press Unwrap.
        push_uv_pane_state();
        plater->show_uv_editor(true);
        return;
    }

    // Grow (never shrink) the layer's island list to cover every chart. Shrinking would throw away a
    // hand placement the moment a stroke temporarily merged two islands, and a stale extra entry is
    // harmless - island_transform_matrix() only looks up the charts that actually exist.
    if (layer->islands.size() < size_t(m_uv_editor_unwrap.chart_count))
        layer->islands.resize(size_t(m_uv_editor_unwrap.chart_count));

    // Connected-net layout (on by default): a *fresh* unwrap is unfolded so adjacent charts sit
    // edge-to-edge (cube -> a net), rather than as separately packed squares. Only when the user pressed
    // Unwrap (m_uv_apply_connected_net) - a re-segmentation renumbers charts anyway, so any hand
    // placement from before is already meaningless. A *refresh* re-solve (a committed vertex edit, or an
    // undo) must NOT relayout, or it would throw away every island placement on every vertex edit.
    if (unwrap_changed && m_uv_apply_connected_net && layer->auto_connect_islands) {
        std::vector<TextureIsland> net = compute_connected_net(m_uv_editor_unwrap);
        if (net.size() == size_t(m_uv_editor_unwrap.chart_count)) {
            if (layer->islands.size() < net.size())
                layer->islands.resize(net.size());
            for (size_t i = 0; i < net.size(); ++i)
                layer->islands[i] = net[i];
        }
    }
    if (unwrap_changed)
        m_uv_apply_connected_net = false; // consumed; a refresh re-solve leaves placements alone

    // The geometry goes over in the unwrap's *raw* mm coordinates and is only re-uploaded when the
    // unwrap itself changed. Everything a slider or a drag can touch - island placement, tiling,
    // rotation, offset - is an affine map on top of that, so it goes over as one 2x3 matrix per
    // island instead. That is the whole reason dragging an island is now free: a patch of a million
    // triangles has a million UVs to re-transform and re-upload otherwise, and it was doing exactly
    // that on every single mouse-move event.
    if (unwrap_changed) {
        UVEditorCanvas::Islands view;
        view.uvs            = m_uv_editor_unwrap.uvs;
        view.indices        = m_uv_editor_unwrap.indices;
        view.vertex_island  = m_uv_editor_unwrap.vertex_chart;
        view.boundary_edges = m_uv_editor_unwrap.boundary_edges;
        view.island_count   = m_uv_editor_unwrap.chart_count;

        uv_canvas->set_island_edit_callback(
            [this](int island, const Vec2f &offset_delta, float rotation_delta, float scale_factor, bool finished) {
                on_island_edited(island, offset_delta, rotation_delta, scale_factor, finished);
            });
        uv_canvas->set_vertex_edit_callback(
            [this](const std::vector<std::pair<int, Vec2f>> &edits) { on_uv_vertex_edited(edits); });
        uv_canvas->set_islands(std::move(view));
    }

    uv_canvas->set_select_mode(static_cast<UVEditorCanvas::SelectMode>(m_uv_select_mode));
    uv_canvas->set_island_transforms(uv_editor_island_transforms(*layer));
    // The distortion heatmap tints the island fills only while its check mode is on; otherwise the
    // canvas falls back to its default light-green wash.
    if (m_uv_check_mode == UVCheckMode::Distortion)
        uv_canvas->set_island_fill_colors(m_uv_editor_distortion_colors);
    else
        uv_canvas->set_island_fill_colors({});

    push_uv_pane_state();
    plater->show_uv_editor(true);
}

void GLGizmoTextureDisplacement::compute_uv_editor_distortion_colors(const indexed_triangle_set &patch)
{
    m_uv_editor_distortion_colors.clear();
    const PatchUnwrap &u = m_uv_editor_unwrap;
    if (u.empty() || u.chart_count <= 0)
        return;

    // log2(uv area / 3D area) per unwrap triangle - the same measure the 3D distortion overlay uses.
    std::vector<float> tri_log(u.indices.size(), 0.f);
    std::vector<int>   tri_chart(u.indices.size(), -1);
    for (size_t f = 0; f < u.indices.size(); ++f) {
        const stl_triangle_vertex_indices &t = u.indices[f];
        if (t[0] < 0 || size_t(t[0]) >= u.vertex_chart.size())
            continue;
        tri_chart[f] = u.vertex_chart[size_t(t[0])];
        const auto p3 = [&](int uv_idx) -> Vec3f {
            const int sv = (size_t(uv_idx) < u.source_vertex.size()) ? u.source_vertex[size_t(uv_idx)] : -1;
            return (sv >= 0 && size_t(sv) < patch.vertices.size()) ? patch.vertices[size_t(sv)] : Vec3f::Zero();
        };
        const float a3 = 0.5f * (p3(t[1]) - p3(t[0])).cross(p3(t[2]) - p3(t[0])).norm();
        const Vec2f e0 = u.uvs[size_t(t[1])] - u.uvs[size_t(t[0])];
        const Vec2f e1 = u.uvs[size_t(t[2])] - u.uvs[size_t(t[0])];
        const float a2 = 0.5f * std::abs(e0.x() * e1.y() - e0.y() * e1.x());
        tri_log[f] = (a3 > 1e-12f && a2 > 1e-12f) ? std::log2(a2 / a3) : 0.f;
    }

    // Centre on the median stretch, so a globally scaled unwrap reads as uniform and only *relative*
    // stretching shows up - matching the 3D overlay's convention.
    std::vector<float> sorted = tri_log;
    float              median = 0.f;
    if (!sorted.empty()) {
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        median = sorted[sorted.size() / 2];
    }

    std::vector<double> chart_sum(size_t(u.chart_count), 0.0);
    std::vector<int>    chart_cnt(size_t(u.chart_count), 0);
    for (size_t f = 0; f < tri_log.size(); ++f) {
        const int c = tri_chart[f];
        if (c >= 0 && c < u.chart_count) {
            chart_sum[size_t(c)] += tri_log[f];
            ++chart_cnt[size_t(c)];
        }
    }

    // Blue (compressed) -> green (ideal) -> red (stretched), +/- 2 stops spanning the full range.
    const auto heat = [](float t) -> ColorRGBA {
        t = std::clamp(t, 0.f, 1.f);
        const ColorRGBA blue{ 0.15f, 0.35f, 1.0f, 0.5f }, green{ 0.2f, 0.9f, 0.3f, 0.5f }, red{ 1.0f, 0.2f, 0.15f, 0.5f };
        if (t < 0.5f) { const float s = t * 2.f;         return blue * (1.f - s) + green * s; }
        const float s = (t - 0.5f) * 2.f;                return green * (1.f - s) + red * s;
    };

    m_uv_editor_distortion_colors.resize(size_t(u.chart_count), heat(0.5f));
    for (int c = 0; c < u.chart_count; ++c)
        if (chart_cnt[size_t(c)] > 0) {
            const float avg = float(chart_sum[size_t(c)] / chart_cnt[size_t(c)]);
            m_uv_editor_distortion_colors[size_t(c)] = heat(std::clamp(0.5f + (avg - median) / 4.f, 0.f, 1.f));
        }
}

void GLGizmoTextureDisplacement::on_uv_command(int cmd, float value)
{
    m_uv_command_queue.emplace_back(cmd, value);
    m_parent.set_as_dirty(); // the panel render that runs it
}

void GLGizmoTextureDisplacement::process_uv_commands()
{
    std::vector<std::pair<int, float>> queue;
    queue.swap(m_uv_command_queue);
    for (const auto &[cmd, value] : queue)
        run_uv_command(cmd, value);
}

void GLGizmoTextureDisplacement::apply_view_mode(int mode)
{
    m_use_shaded_preview = (mode == 1);
    m_uv_check_mode    = (mode == 2) ? UVCheckMode::Checker :
                         (mode == 3) ? UVCheckMode::Distortion : UVCheckMode::None;
    rebuild_uvcheck_mesh();
    if (m_use_shaded_preview)
        rebuild_shaded_preview_mesh();
    else
        // The Fast view skips the CPU displacement entirely (see rebuild_preview()), so leaving it means
        // m_preview_glmodel may be stale or absent - ask for it now.
        queue_preview_job();
    // ...and the paint tint between the base and the displaced surface, for the same reason.
    m_paint_overlay_dirty = true;
    refresh_wireframe(); // Normal<->Fast swaps the wireframe between displaced and base mesh
    update_uv_editor();  // mirror the checker / distortion heatmap into the UV pane too
    m_parent.set_as_dirty();
}

int GLGizmoTextureDisplacement::view_mode() const
{
    return m_use_shaded_preview                         ? 1 :
           m_uv_check_mode == UVCheckMode::Checker    ? 2 :
           m_uv_check_mode == UVCheckMode::Distortion ? 3 : 0;
}

int GLGizmoTextureDisplacement::IslandViewFollow::step(bool shown_now, int view, bool debug_open)
{
    shown = shown_now;
    if (!shown) {
        // The view is back to Normal already if a debug stage took it, and Normal applied over a stage would
        // queue a preview job that replaces the stage being inspected.
        const bool restore = switched && view == 1;
        switched = keep_normal = false;
        return restore ? 0 : -1;
    }
    if (view != 0 || keep_normal || debug_open)
        return -1;
    switched = true;
    return 1;
}

void GLGizmoTextureDisplacement::follow_island_highlight()
{
    const std::vector<int> *islands = island_highlight_selection();
    const int view = m_island_view.step(islands != nullptr && !islands->empty(), view_mode(), m_debug_stage >= 0);
    if (view >= 0)
        apply_view_mode(view);
}

void GLGizmoTextureDisplacement::push_uv_pane_state()
{
    UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
    if (canvas == nullptr)
        return;

    UVEditorCanvas::PaneState       st;
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (mv != nullptr && layer != nullptr && layer->projection_method == TextureProjectionMethod::LSCM) {
        st.has_layer       = true;
        st.layer_name      = from_u8(layer->name.empty() ? Slic3r::format(_u8L("Layer %1%"), layer->slot + 1) : layer->name);
        st.tile_mm         = layer->tiling_scale;
        st.seam_angle_deg  = layer->lscm_seam_angle_deg;
        st.connect_islands = layer->auto_connect_islands;
        st.has_seams       = !layer->lscm_seam_edges.empty();
        st.has_uv_edits    = !layer->lscm_uv_overrides.empty();

        // Retried while empty: the image may not decode yet, and a blank thumbnail must not stick.
        if (layer->image_data.get() != m_uv_thumb_source || m_uv_thumb_rgb.empty()) {
            m_uv_thumb_source = nullptr;
            m_uv_thumb_rgb.clear();
            const DecodedHeightTexture tex = decode_height_texture(*layer);
            if (!tex.empty()) {
                m_uv_thumb_source = layer->image_data.get();
                // Box-filtered, so a fine pattern such as bricks averages out instead of aliasing into noise.
                const int  n     = UV_THUMB_PX;
                const bool color = tex.has_color();
                m_uv_thumb_rgb.resize(size_t(n) * n * 3);
                for (int y = 0; y < n; ++y) {
                    const int y0 = y * tex.height / n, y1 = std::max(y0 + 1, (y + 1) * tex.height / n);
                    for (int x = 0; x < n; ++x) {
                        const int x0 = x * tex.width / n, x1 = std::max(x0 + 1, (x + 1) * tex.width / n);
                        uint64_t  sum[3]{};
                        for (int sy = y0; sy < y1; ++sy)
                            for (int sx = x0; sx < x1; ++sx) {
                                const size_t src = size_t(sy) * size_t(tex.width) + size_t(sx);
                                for (int ch = 0; ch < 3; ++ch)
                                    sum[ch] += color ? tex.rgb[src * 3 + ch] : tex.pixels[src];
                            }
                        const uint64_t count = uint64_t(y1 - y0) * uint64_t(x1 - x0);
                        for (int ch = 0; ch < 3; ++ch)
                            m_uv_thumb_rgb[size_t(y * n + x) * 3 + ch] = (unsigned char) (sum[ch] / count);
                    }
                }
            }
        }
        if (!m_uv_thumb_rgb.empty()) {
            st.thumbnail_px  = UV_THUMB_PX;
            st.thumbnail_rgb = m_uv_thumb_rgb;
        }

        if (!m_uv_editor_unwrap.empty()) {
            // Stale when the unwrap on screen was made from another layer, other paint, other seams or another
            // seam angle than this layer has now - the same things update_uv_editor() re-solves for.
            UVEditorState now;
            now.slot        = m_active_layer_slot;
            now.image_data  = layer->image_data.get();
            now.seam_angle  = layer->lscm_seam_angle_deg;
            now.padding     = layer->island_padding_mm;
            now.facets      = mv->texture_displacement_facet(m_active_layer_slot).get_data();
            now.seam_edges  = layer->lscm_seam_edges;
            st.unwrapped    = true;
            st.unwrap_stale = !(now == m_uv_editor_state);
            st.island_count = m_uv_editor_unwrap.chart_count;
            st.face_count   = m_uv_editor_unwrap.indices.size();
        }
    }
    st.mark_seams = m_seam_edit_mode;
    st.seam_path  = m_seam_path_mode;
    st.background = m_uv_check_mode == UVCheckMode::Checker    ? UVEditorCanvas::Background::Checker :
                    m_uv_check_mode == UVCheckMode::Distortion ? UVEditorCanvas::Background::Distortion :
                                                                 UVEditorCanvas::Background::Height;
    canvas->set_pane_state(std::move(st));
}

void GLGizmoTextureDisplacement::run_uv_command(int cmd, float value)
{
    using Command = UVEditorCanvas::Command;
    if (cmd == int(Command::PaneClosed)) {
        // Closed with the pane's own X: keep it closed until asked again, and upload the background afresh then.
        m_show_uv_editor = false;
        m_uv_editor_bg   = UVBackground::None;
        // The seam tool belongs to the pane: left on with the pane gone, every stroke on the model would
        // be swallowed as a seam click and nothing would paint.
        if (m_seam_edit_mode) {
            m_seam_edit_mode    = false;
            m_seam_hover_edge   = { -1, -1 };
            m_seam_hover_vertex = -1;
            m_seam_hover_glmodel.reset();
            m_seam_path_anchor  = -1;
            m_seam_anchor_glmodel.reset();
            push_uv_pane_state();
        }
        return;
    }
    if (cmd == int(Command::SetBackground)) {
        // Height goes back to whichever of Normal / Fast was showing; Checker and Distortion are views of their own.
        const int background = std::clamp(int(std::lround(value)), 0, 2);
        apply_view_mode(background == 1 ? 2 : background == 2 ? 3 : (m_use_shaded_preview ? 1 : 0));
        return;
    }

    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;

    if (cmd == int(UVEditorCanvas::Command::AverageScale)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Average island scale"), UndoRedo::SnapshotType::GizmoAction);
        average_island_scales(layer->islands);
        rebuild_preview();
    } else if (cmd == int(UVEditorCanvas::Command::CutSelectedIsland)) {
        UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
        const int       chart  = canvas != nullptr ? canvas->selected_island() : -1;
        if (chart < 0) {
            show_error(nullptr, _u8L("Select an island in the UV editor first."));
            return;
        }
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut texture island"), UndoRedo::SnapshotType::GizmoAction);
        cut_island(*layer, chart);
        rebuild_preview();
    } else if (cmd == int(UVEditorCanvas::Command::JoinSelected)) {
        UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
        const int       chart  = canvas != nullptr ? canvas->selected_island() : -1;
        if (chart < 0) {
            show_error(nullptr, _u8L("Select an island in the UV editor first."));
            return;
        }
        // Join to whichever neighbouring island (one it shares an edge with) is currently placed
        // nearest - i.e. the one it was dragged up against.
        const Eigen::Matrix<float, 2, 3> sel_m = island_transform_matrix(chart, m_uv_editor_unwrap, layer->islands);
        const Vec2f sel_c = sel_m.block<2, 2>(0, 0) * m_uv_editor_unwrap.chart_centroid[size_t(chart)] + sel_m.col(2);
        int   best_parent = -1;
        float best_d2     = std::numeric_limits<float>::max();
        TextureIsland best_place, cand;
        for (int p = 0; p < m_uv_editor_unwrap.chart_count; ++p) {
            if (p == chart)
                continue;
            if (!join_chart_placement(m_uv_editor_unwrap, layer->islands, chart, p, cand))
                continue; // not a neighbour
            const Eigen::Matrix<float, 2, 3> pm = island_transform_matrix(p, m_uv_editor_unwrap, layer->islands);
            const Vec2f pc = pm.block<2, 2>(0, 0) * m_uv_editor_unwrap.chart_centroid[size_t(p)] + pm.col(2);
            const float d2 = (pc - sel_c).squaredNorm();
            if (d2 < best_d2) { best_d2 = d2; best_parent = p; best_place = cand; }
        }
        if (best_parent < 0) {
            show_error(nullptr, _u8L("This island has no neighbour it shares an edge with."));
            return;
        }
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Join texture island"), UndoRedo::SnapshotType::GizmoAction);
        if (layer->islands.size() <= size_t(chart))
            layer->islands.resize(size_t(chart) + 1);
        layer->islands[size_t(chart)] = best_place;
        // Record the join so the two (and anything already grouped with either) move together from now
        // on, not just visually snap once.
        join_island_groups(layer->island_groups, chart, best_parent, m_uv_editor_unwrap.chart_count);
        rebuild_preview();
    } else if (cmd == int(UVEditorCanvas::Command::UnjoinSelected)) {
        UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
        const int       chart  = canvas != nullptr ? canvas->selected_island() : -1;
        if (chart < 0 || size_t(chart) >= layer->islands.size()) {
            show_error(nullptr, _u8L("Select an island in the UV editor first."));
            return;
        }
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Unjoin texture island"), UndoRedo::SnapshotType::GizmoAction);
        layer->islands[size_t(chart)] = TextureIsland{}; // back to its own packed position
        // Break its join link too, so it stops moving with the others (the rest stay grouped).
        if (size_t(chart) < layer->island_groups.size())
            layer->island_groups[size_t(chart)] = chart;
        rebuild_preview();
    }
    else if (cmd == int(Command::PickTexture)) {
        // The same texture library the layer card opens; the panel renders it this frame.
        m_picker_slot            = m_active_layer_slot;
        m_picker_open_request    = true;
        m_picker_reveal_selected = true;
    } else if (cmd == int(Command::Unwrap)) {
        // The solve runs only on this - painting, the seam angle and seam marking never trigger it.
        m_uv_unwrap_pending      = true;
        m_uv_apply_connected_net = true; // a genuine re-unwrap may relayout the islands
        m_show_uv_editor         = true;
        update_uv_editor();
    } else if (cmd == int(Command::SetSeamAngle)) {
        const float angle = std::clamp(value, 5.f, 90.f);
        if (angle != layer->lscm_seam_angle_deg) {
            layer->lscm_seam_angle_deg = angle;
            rebuild_preview(); // the bake unwraps with it; this also marks the pane's unwrap stale
        }
    } else if (cmd == int(Command::SetConnectIslands)) {
        const bool connect = value != 0.f;
        if (connect != layer->auto_connect_islands) {
            layer->auto_connect_islands = connect;
            // Apply (or, when turned off, just stop re-applying) right away rather than waiting for the next re-unwrap.
            if (connect && !m_uv_editor_unwrap.empty()) {
                std::vector<TextureIsland> net = compute_connected_net(m_uv_editor_unwrap);
                if (net.size() == size_t(m_uv_editor_unwrap.chart_count)) {
                    if (layer->islands.size() < net.size())
                        layer->islands.resize(net.size());
                    for (size_t i = 0; i < net.size(); ++i)
                        layer->islands[i] = net[i];
                }
            }
            rebuild_preview();
        }
    } else if (cmd == int(Command::SetSelectMode)) {
        m_uv_select_mode = std::clamp(int(std::lround(value)), 0, 2); // the canvas has already switched
    } else if (cmd == int(Command::SetMarkSeams)) {
        m_seam_edit_mode = value != 0.f;
        if (m_seam_edit_mode)
            m_adjust_texture_mode = false; // the two click modes are mutually exclusive
        else {
            m_seam_hover_edge   = { -1, -1 }; // drop the hover highlight when leaving the mode
            m_seam_hover_vertex = -1;
            m_seam_hover_glmodel.reset();
            m_seam_path_anchor = -1;
            m_seam_anchor_glmodel.reset();
        }
        push_uv_pane_state();
    } else if (cmd == int(Command::SetSeamPath)) {
        m_seam_path_mode    = value != 0.f;
        m_seam_path_anchor  = -1;
        m_seam_hover_edge   = { -1, -1 }; // hover target type changes with the mode
        m_seam_hover_vertex = -1;
        m_seam_hover_glmodel.reset();
        m_seam_anchor_glmodel.reset();
        push_uv_pane_state();
    } else if (cmd == int(Command::ClearSeams)) {
        if (!layer->lscm_seam_edges.empty()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear texture seams"), UndoRedo::SnapshotType::GizmoAction);
            layer->lscm_seam_edges.clear();
            rebuild_preview();
        }
    } else if (cmd == int(Command::ClearUVEdits)) {
        if (!layer->lscm_uv_overrides.empty()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear texture UV edits"), UndoRedo::SnapshotType::GizmoAction);
            layer->lscm_uv_overrides.clear();
            m_uv_unwrap_pending = true; // re-solve so the pane drops the edited coords
            update_uv_editor();
            rebuild_preview();
        }
    }
    // FrameAll/ToggleSnap are handled inside the canvas; ProjectFromView is not wired yet. SelectionChanged
    // only asks for this frame: follow_island_highlight() and the overlay read the selection back themselves.
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::capture_view_projection(TextureDisplacementLayer &layer)
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return;

    const Camera     &camera    = wxGetApp().plater()->get_camera();
    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo     = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    // The view matrix's rotation rows are the camera axes in world space; bring them into the
    // volume's local frame (where the mesh vertices live) so the projector rides along with the part.
    const Matrix3d view_rot   = camera.get_view_matrix().matrix().block<3, 3>(0, 0);
    const Matrix3d trafo_rot  = trafo.matrix().block<3, 3>(0, 0);
    const Matrix3d world_to_local = trafo_rot.inverse();
    const Vec3d    local_right = world_to_local * Vec3d(view_rot.row(0).transpose());
    const Vec3d    local_up    = world_to_local * Vec3d(view_rot.row(1).transpose());

    // Unit axes: the projected planar coordinate must stay in mm so tiling_scale keeps meaning mm.
    layer.view_project_right = local_right.norm() > 1e-9 ? Vec3f(local_right.normalized().cast<float>()) : Vec3f::UnitX();
    layer.view_project_up    = local_up.norm() > 1e-9 ? Vec3f(local_up.normalized().cast<float>()) : Vec3f::UnitY();
}

void GLGizmoTextureDisplacement::show_projector(bool show)
{
    if (!show) {
        if (m_projector_frame != nullptr)
            m_projector_frame->Hide();
        return;
    }

    if (m_projector_frame == nullptr) {
        // Parented to the main frame, not to Plater: Plater is a wxPanel, and wxFRAME_FLOAT_ON_PARENT
        // wants a real top-level window to float above.
        m_projector_frame         = new TextureProjectorFrame(wxGetApp().mainframe);
        m_projector_tex_source    = nullptr; // fresh window, nothing uploaded into it yet
        m_projector_tex_smoothing = -1.f;
        m_projector_frame->set_opacity(m_projector_opacity);

        // Opened centred over the 3D canvas, at about half its size: the frame is meant to be
        // dragged onto part of the model, so starting somewhere on top of it beats the OS's default
        // cascade position, which is often off over the sidebar.
        if (const wxGLCanvas *cnv = m_parent.get_wxglcanvas(); cnv != nullptr) {
            const wxRect  area = cnv->GetScreenRect();
            const wxSize  size(std::max(160, area.width / 2), std::max(160, area.height / 2));
            m_projector_frame->SetSize(wxRect(area.GetTopLeft() + wxPoint((area.width - size.x) / 2,
                                                                         (area.height - size.y) / 2),
                                              size));
        }
    }

    m_projector_frame->Show();
    m_projector_frame->Raise();
    update_projector();
}

void GLGizmoTextureDisplacement::update_projector()
{
    if (m_projector_frame == nullptr || !m_projector_frame->IsShown())
        return;

    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->projection_method != TextureProjectionMethod::ViewProjected) {
        m_projector_frame->set_texture({}, 0, 0);
        m_projector_tex_source    = nullptr;
        m_projector_tex_smoothing = -1.f;
        return;
    }

    // Only re-upload when the pixels actually changed - this is reached from the panel's per-edit
    // flush, and rebuilding the bitmap from identical bytes every time would be pure waste.
    if (m_projector_tex_source != layer->image_data.get() || m_projector_tex_smoothing != layer->smoothing) {
        const DecodedHeightTexture height = decode_height_texture(*layer);
        if (height.empty())
            m_projector_frame->set_texture({}, 0, 0);
        else
            m_projector_frame->set_texture(height.pixels, height.width, height.height);
        m_projector_tex_source    = layer->image_data.get();
        m_projector_tex_smoothing = layer->smoothing;
    }
}

int GLGizmoTextureDisplacement::apply_projection_frame()
{
    TextureDisplacementLayer *layer = active_layer();
    const ModelVolume        *mv    = texture_volume();
    const ModelObject        *mo    = m_c->selection_info()->model_object();
    wxGLCanvas               *cnv   = m_parent.get_wxglcanvas();
    if (layer == nullptr || mv == nullptr || mo == nullptr || cnv == nullptr || m_projector_frame == nullptr ||
        !m_projector_frame->IsShown())
        return -1;

    // The frame's gate, brought from screen coordinates into the GL viewport's pixel space. Two
    // conversions, both necessary: ScreenToClient() because the viewport's origin is the canvas's
    // top-left, not the desktop's, and the retina scale because the viewport is sized in physical
    // pixels (see GLCanvas3D::get_canvas_size()) while wx hands out logical ones.
    const wxRect  gate   = m_projector_frame->client_rect_on_screen();
    const wxPoint tl     = cnv->ScreenToClient(gate.GetTopLeft());
    const double  scale  = double(m_parent.get_scale());
    const double  rx = double(tl.x) * scale, ry = double(tl.y) * scale;
    const double  rw = double(gate.width) * scale, rh = double(gate.height) * scale;
    if (rw < 1.0 || rh < 1.0)
        return -1;

    const Camera             &camera = wxGetApp().plater()->get_camera();
    const std::array<int, 4> &vp     = camera.get_viewport();
    const Selection          &sel    = m_parent.get_selection();
    const Transform3d trafo = mo->instances[sel.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    // Local space -> clip space, the same product the renderer uses, so the mapping agrees with what
    // is actually on screen rather than with an idealisation of it.
    const Eigen::Matrix4d K = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix() * trafo.matrix();

    // Window coordinates follow igl::project's convention (as CameraUtils::project does):
    //     win_x = vp.x + vp.w * (ndc.x + 1) / 2,  and y measured downward as vp.h - win_y_gl.
    // Turning those into uv = (win - rect_origin) / rect_size gives u and v as affine functions of
    // ndc.x and ndc.y, and since ndc = clip.xyz / clip.w, multiplying through by clip.w leaves a
    // plain linear combination of K's rows - i.e. one 3x4 matrix carrying the perspective divide.
    const double A = double(vp[2]) / (2.0 * rw);
    const double B = (double(vp[0]) + double(vp[2]) * 0.5 - rx) / rw;
    const double C = -double(vp[3]) / (2.0 * rh);
    const double D = (double(vp[3]) * 0.5 - double(vp[1]) - ry) / rh;

    const Eigen::Vector4d row_u = A * K.row(0).transpose() + B * K.row(3).transpose();
    const Eigen::Vector4d row_v = C * K.row(1).transpose() + D * K.row(3).transpose();
    const Eigen::Vector4d row_w = K.row(3).transpose();

    std::array<float, 12> m{};
    for (int i = 0; i < 4; ++i) {
        m[size_t(i)]     = float(row_u[i]);
        m[size_t(4 + i)] = float(row_v[i]);
        m[size_t(8 + i)] = float(row_w[i]);
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Apply texture projection frame"),
                                  UndoRedo::SnapshotType::GizmoAction);

    layer->projection_method       = TextureProjectionMethod::ViewProjected;
    layer->view_project_projective = true;
    layer->view_project_matrix     = m;
    // Off, so the height sampler returns 0 outside [0,1) and the frame's border becomes a hard edge
    // of the displacement rather than the first seam of an endless repeat.
    layer->tile_enabled = false;
    // The affine axes are kept up to date too, so turning the projective mapping off later leaves a
    // sane flat projection from this same viewpoint instead of whatever was captured long ago.
    capture_view_projection(*layer);

    const int painted = select_visible_faces(&m);
    m_preview_params_dirty = true;
    rebuild_preview();
    m_parent.set_as_dirty();
    return painted;
}

void GLGizmoTextureDisplacement::cut_island(TextureDisplacementLayer &layer, int chart)
{
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;
    const std::vector<Vec3f> &verts = mv->mesh().its.vertices;
    const PatchUnwrap        &u     = m_uv_editor_unwrap;

    // Collect the chart's triangles back in mesh-vertex space, and its 3D bounding box.
    std::vector<std::array<int, 3>> tris;
    Vec3f lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vec3f hi(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    for (const stl_triangle_vertex_indices &t : u.indices) {
        if (t[0] < 0 || size_t(t[0]) >= u.vertex_chart.size() || u.vertex_chart[size_t(t[0])] != chart)
            continue;
        std::array<int, 3> bt{};
        bool               ok = true;
        for (int k = 0; k < 3; ++k) {
            const int uvv = t[k];
            if (uvv < 0 || size_t(uvv) >= u.source_vertex.size()) { ok = false; break; }
            const int base = u.source_vertex[size_t(uvv)];
            if (base < 0 || size_t(base) >= verts.size()) { ok = false; break; }
            bt[k] = base;
            lo    = lo.cwiseMin(verts[size_t(base)]);
            hi    = hi.cwiseMax(verts[size_t(base)]);
        }
        if (ok)
            tris.push_back(bt);
    }
    if (tris.empty())
        return;

    // Cut perpendicular to the longest axis, through the centroid - a long thin island is split
    // across its narrow middle, which is exactly the "islands might be very long" case.
    const Vec3f  ext  = hi - lo;
    const int    axis = (ext.x() >= ext.y() && ext.x() >= ext.z()) ? 0 : (ext.y() >= ext.z() ? 1 : 2);
    const float  mid  = 0.5f * (lo[axis] + hi[axis]);

    std::set<std::pair<int, int>> seams(layer.lscm_seam_edges.begin(), layer.lscm_seam_edges.end());
    for (const std::array<int, 3> &bt : tris)
        for (int i = 0; i < 3; ++i) {
            const int a = bt[i], b = bt[(i + 1) % 3];
            // Endpoints on opposite sides of the plane -> this edge crosses it -> make it a seam.
            if ((verts[size_t(a)][axis] < mid) != (verts[size_t(b)][axis] < mid))
                seams.insert({ std::min(a, b), std::max(a, b) });
        }
    layer.lscm_seam_edges.assign(seams.begin(), seams.end());
}

// unwrap mm -> texture uv, per island: the island's own hand placement, then the layer's
// tiling/rotation/offset. Both are affine, so they compose into one matrix the canvas can hand
// straight to a shader.
std::vector<Eigen::Matrix<float, 2, 3>>
GLGizmoTextureDisplacement::uv_editor_island_transforms(const TextureDisplacementLayer &layer)
{
    const float uv_scale = (layer.tiling_scale > 1e-6f) ? (1.f / layer.tiling_scale) : 1.f;
    const float rad      = layer.rotation_deg * float(M_PI) / 180.f;
    const float cs       = std::cos(rad) * uv_scale;
    const float sn       = std::sin(rad) * uv_scale;

    Eigen::Matrix2f uv_linear;
    uv_linear << cs, -sn,
                 sn,  cs;

    m_uv_editor_bbox_min = Vec2f(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    m_uv_editor_bbox_max = Vec2f(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

    std::vector<Eigen::Matrix<float, 2, 3>> transforms(size_t(std::max(m_uv_editor_unwrap.chart_count, 0)));
    for (int c = 0; c < m_uv_editor_unwrap.chart_count; ++c) {
        const Eigen::Matrix<float, 2, 3> island = island_transform_matrix(c, m_uv_editor_unwrap, layer.islands);
        Eigen::Matrix<float, 2, 3>      &m      = transforms[size_t(c)];
        m.block<2, 2>(0, 0) = uv_linear * island.block<2, 2>(0, 0);
        m.col(2)            = uv_linear * island.col(2) + layer.offset;
    }

    // Only for the panel's readout; the canvas computes its own bounds.
    for (size_t i = 0; i < m_uv_editor_unwrap.uvs.size(); ++i) {
        const int c = m_uv_editor_unwrap.vertex_chart[i];
        if (c < 0 || size_t(c) >= transforms.size())
            continue;
        const Vec2f uv = transforms[size_t(c)].block<2, 2>(0, 0) * m_uv_editor_unwrap.uvs[i] + transforms[size_t(c)].col(2);
        m_uv_editor_bbox_min = m_uv_editor_bbox_min.cwiseMin(uv);
        m_uv_editor_bbox_max = m_uv_editor_bbox_max.cwiseMax(uv);
    }
    return transforms;
}

void GLGizmoTextureDisplacement::on_island_edited(int island, const Vec2f &offset_delta, float rotation_delta,
                                                  float scale_factor, bool finished)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || island < 0)
        return;
    if (size_t(island) >= layer->islands.size())
        layer->islands.resize(size_t(island) + 1);

    // A pure translation (no rotation, no scale) is the only gesture that moves a whole group/selection
    // together; rotate and scale stay on the primary island alone. This split is what makes flagging a
    // group's worth of vertices on the GPU safe: the shared shader delta is a pure translation, so the
    // same offset is correct for every flagged island.
    const bool is_move = rotation_delta == 0.f && scale_factor == 1.f;

    if (!m_island_drag_active) {
        // Taken before the first delta lands, so one undo reverts the whole drag rather than just
        // its final mouse-move.
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move texture island"), UndoRedo::SnapshotType::GizmoAction);
        m_island_drag_active = true;
        // Decide the moved set once, at drag start: the whole selection + join groups for a move, or
        // just the primary for a rotate/scale.
        m_island_move_set = is_move ? build_island_move_set(*layer, island) : std::vector<int>{ island };
        // Set up the GPU drag: bake the mesh once (via the dirty flag), which is also what flags the
        // moved islands' triangles. From then on the drag is a uniform update, no rebuild - see
        // render_shaded_preview_mesh().
        m_shaded_active_chart = island;
        m_shaded_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
        m_shaded_preview_dirty = true;
    }

    // Apply the edit. A move goes to every island in the moved set (same offset -> they translate as
    // one); a rotate/scale goes only to the primary, about its own centroid.
    const std::vector<int>  single{ island };
    const std::vector<int> &targets = (is_move && !m_island_move_set.empty()) ? m_island_move_set : single;
    for (int c : targets) {
        if (c < 0)
            continue;
        if (size_t(c) >= layer->islands.size())
            layer->islands.resize(size_t(c) + 1);
        TextureIsland &target = layer->islands[size_t(c)];
        target.offset += offset_delta;
        if (c == island) {
            target.rotation_deg += rotation_delta;
            // Guarded: a scale that reaches zero is unrecoverable (every subsequent factor multiplies it)
            // and would collapse the island to a point - exactly the failure this feature hit once already.
            target.scale = std::clamp(target.scale * scale_factor, 0.001f, 1000.f);
        }
    }

    if (finished) {
        m_island_drag_active = false;
        m_shaded_active_chart  = -1;
        m_shaded_active_face.clear();
        m_island_move_set.clear();
        m_shaded_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
        rebuild_preview(); // the real displaced geometry moved: recompute it once, at the end
    } else {
        const std::vector<Eigen::Matrix<float, 2, 3>> xf = uv_editor_island_transforms(*layer);
        if (UVEditorCanvas *uv_canvas = wxGetApp().plater()->get_uv_editor_canvas()) {
            // Live feedback in the pane, and deliberately *just* the transforms: nothing about the
            // unwrap changed, so none of the vertex buffers need touching. This is what makes a drag
            // interactive on a patch with a million triangles.
            uv_canvas->set_island_transforms(xf);
        }
        // Move the island on the model live through the shader's island_delta uniform - no mesh
        // rebuild. delta = F_current * F_baked^-1 in final-uv space. 
        // The one rebuild that bakes the flags is
        // scheduled at drag start above and consumed once per frame by render_painter_gizmo().
        if (m_use_shaded_preview && m_shaded_active_chart == island && size_t(island) < xf.size()) {
            Eigen::Matrix3f cur = Eigen::Matrix3f::Identity();
            cur.topRows<2>()    = xf[size_t(island)];
            Eigen::Matrix3f bak = Eigen::Matrix3f::Identity();
            bak.topRows<2>()    = m_shaded_baked_active_xf;
            m_shaded_island_delta = (cur * bak.inverse()).topRows<2>();
            m_parent.set_as_dirty();
        }
    }
}

void GLGizmoTextureDisplacement::on_uv_vertex_edited(const std::vector<std::pair<int, Vec2f>> &edits)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || edits.empty() || m_uv_editor_unwrap.empty())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Edit texture UV"), UndoRedo::SnapshotType::GizmoAction);

    for (const auto &[unwrapped, raw_uv] : edits) {
        if (unwrapped < 0 || size_t(unwrapped) >= m_uv_editor_unwrap.source_vertex.size())
            continue;
        // Keep the gizmo's own unwrap copy in step, so the pane and the bake agree without a re-solve.
        if (size_t(unwrapped) < m_uv_editor_unwrap.uvs.size())
            m_uv_editor_unwrap.uvs[size_t(unwrapped)] = raw_uv;

        // Keyed by this one unwrapped copy, not by its mesh vertex: a seam vertex has a copy in each island it
        // borders, and only the one that was dragged may move. Small list, linear scan is fine.
        const int key = lscm_uv_override_key(unwrapped);
        auto it = std::find_if(layer->lscm_uv_overrides.begin(), layer->lscm_uv_overrides.end(),
                               [key](const std::pair<int, Vec2f> &p) { return p.first == key; });
        if (it != layer->lscm_uv_overrides.end())
            it->second = raw_uv;
        else
            layer->lscm_uv_overrides.emplace_back(key, raw_uv);
    }

    rebuild_preview(); // the baked displacement samples the moved uv now
}

bool GLGizmoTextureDisplacement::update_adjust_anchor()
{
    const ModelVolume *mv = texture_volume();
    m_adjust_anchor_valid = mv != nullptr &&
        compute_layer_paint_anchor(mv->mesh().its, mv->texture_displacement_facet(m_active_layer_slot).get_data(),
                                    m_adjust_anchor_pos, m_adjust_anchor_normal);
    return m_adjust_anchor_valid;
}

TextureDisplacementLayer *GLGizmoTextureDisplacement::active_layer()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == m_active_layer_slot)
            return &l;
    return nullptr;
}

const TextureDisplacementLayer *GLGizmoTextureDisplacement::active_layer() const
{
    return const_cast<GLGizmoTextureDisplacement *>(this)->active_layer();
}

Vec3f GLGizmoTextureDisplacement::adjust_plane_point() const
{
    const ModelVolume *mv        = texture_volume();
    const float        bbox_size = (mv != nullptr) ? float(mv->mesh().bounding_box().size().norm()) : 1.f;
    const float        lift      = bbox_size * 0.01f + 0.2f; // clear of the surface, to avoid z-fighting
    return m_adjust_anchor_pos + m_adjust_anchor_normal * lift;
}

Vec3f GLGizmoTextureDisplacement::adjust_handle_center(const TextureDisplacementLayer &layer) const
{
    // apply_uv_transform() maps a planar mm coordinate p to uv = R(p / tiling_scale) + offset, and
    // on_mouse_adjust_texture() drives offset by  offset = offset_start - R(delta / tiling_scale).
    // Inverting that, the handle's displacement from the anchor is  - R^-1(offset) * tiling_scale -
    // which, substituted into the drag equation, moves the handle by exactly `delta`. So the handle
    // follows the cursor precisely, and is back on the anchor exactly when offset is zero.
    const float rad = layer.rotation_deg * float(M_PI) / 180.f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    // Undo the non-square v scaling first - it is the last thing apply_uv_transform() does, so it is
    // the first thing to come off on the way back.
    const float aspect = layer_texture_aspect(layer);
    const Vec2f o(layer.offset.x(), (aspect > 0.f) ? layer.offset.y() / aspect : layer.offset.y());
    const Vec2f unrotated(o.x() * cs + o.y() * sn, -o.x() * sn + o.y() * cs);
    const Vec2f planar = -unrotated * layer.tiling_scale;

    Vec3f u_axis, v_axis;
    adjust_tangent_basis(u_axis, v_axis);
    return adjust_plane_point() + u_axis * planar.x() + v_axis * planar.y();
}

void GLGizmoTextureDisplacement::adjust_tangent_basis(Vec3f &u_axis, Vec3f &v_axis) const
{
    // libslic3r's own basis, asked for rather than restated here, so the ring's "0 degrees" and the
    // offset handle's plane always agree with what project_texture_displacement_uv() does - including
    // v pointing *down* the texture and u flipping on the far side of each plane.
    box_projection_axes(m_adjust_anchor_normal, u_axis, v_axis);
}

void GLGizmoTextureDisplacement::render_adjust_texture_gizmo()
{
    if (!m_adjust_anchor_valid)
        return;

    const ModelObject              *mo    = m_c->selection_info()->model_object();
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (mo == nullptr || mv == nullptr || layer == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    // Handle sizes scale with the volume so they stay usable on both tiny and huge models.
    const float bbox_size    = float(mv->mesh().bounding_box().size().norm());
    const float panel_half   = bbox_size * 0.04f + 1.f;
    const float arrow_length = panel_half * 2.2f;

    // Tracks the layer's offset, so the handle actually travels with the texture as it is dragged.
    const Vec3f handle_center_local = adjust_handle_center(*layer);

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    shader->start_using();

    const Camera &camera = wxGetApp().plater()->get_camera();

    Vec3f u_axis, v_axis;
    adjust_tangent_basis(u_axis, v_axis);
    Transform3d plane_transform     = Transform3d::Identity();
    plane_transform.linear().col(0) = u_axis.cast<double>();
    plane_transform.linear().col(1) = v_axis.cast<double>();
    plane_transform.linear().col(2) = m_adjust_anchor_normal.cast<double>();
    plane_transform.translation()   = handle_center_local.cast<double>();

    // Pan panel: a flat square lying in the patch's own tangent plane. Dragging anywhere on it
    // moves the texture freely along both axes at once.
    if (!m_adjust_panel_glmodel.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);
        init_data.add_vertex(Vec3f(-1.f, -1.f, 0.f));
        init_data.add_vertex(Vec3f(1.f, -1.f, 0.f));
        init_data.add_vertex(Vec3f(1.f, 1.f, 0.f));
        init_data.add_vertex(Vec3f(-1.f, 1.f, 0.f));
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(0, 2, 3);
        m_adjust_panel_glmodel.init_from(std::move(init_data));
    }
    Transform3d view_model_matrix = camera.get_view_matrix() * trafo_matrix * plane_transform *
        Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d(panel_half, panel_half, panel_half));
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    ColorRGBA panel_color = m_adjust_drag_handle == AdjustHandle::Pan ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE();
    panel_color.a(0.45f);
    m_adjust_panel_glmodel.set_color(panel_color);
    m_adjust_panel_glmodel.render();

    // Axis arrows: a shaft plus a small V-shaped arrowhead, both along local +X. Reused for both
    // the U and V axes below by swapping which world direction local +X is transformed to.
    if (!m_adjust_arrow_glmodel.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);
        init_data.add_vertex(Vec3f(0.f, 0.f, 0.f));
        init_data.add_vertex(Vec3f(1.f, 0.f, 0.f));
        init_data.add_vertex(Vec3f(0.82f, 0.08f, 0.f));
        init_data.add_vertex(Vec3f(0.82f, -0.08f, 0.f));
        init_data.add_line(0, 1);
        init_data.add_line(1, 2);
        init_data.add_line(1, 3);
        m_adjust_arrow_glmodel.init_from(std::move(init_data));
    }
#if !SLIC3R_OPENGL_ES
    if (!OpenGLManager::get_gl_info().is_core_profile())
        glsafe(::glLineWidth(2.0f));
#endif // !SLIC3R_OPENGL_ES

    auto render_arrow = [&](const Vec3f &axis, const Vec3f &other_axis, bool is_active) {
        Transform3d arrow_transform     = Transform3d::Identity();
        arrow_transform.linear().col(0) = axis.cast<double>();
        arrow_transform.linear().col(1) = other_axis.cast<double>();
        arrow_transform.linear().col(2) = m_adjust_anchor_normal.cast<double>();
        arrow_transform.translation()   = handle_center_local.cast<double>();

        const Transform3d vmm = camera.get_view_matrix() * trafo_matrix * arrow_transform *
            Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d(arrow_length, arrow_length, arrow_length));
        shader->set_uniform("view_model_matrix", vmm);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        m_adjust_arrow_glmodel.set_color(is_active ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE());
        m_adjust_arrow_glmodel.render();
    };
    render_arrow(u_axis, v_axis, m_adjust_drag_handle == AdjustHandle::AxisU);
    render_arrow(v_axis, u_axis, m_adjust_drag_handle == AdjustHandle::AxisV);

    shader->stop_using();
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

bool GLGizmoTextureDisplacement::on_mouse_adjust_texture(const wxMouseEvent &mouse_event)
{
    if (!m_adjust_anchor_valid)
        return false;

    ModelVolume              *mv    = texture_volume();
    ModelObject              *mo    = m_c->selection_info()->model_object();
    TextureDisplacementLayer *layer = active_layer();
    if (mv == nullptr || mo == nullptr || layer == nullptr)
        return false;

    const Selection   &selection    = m_parent.get_selection();
    const Transform3d  trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera      &camera       = wxGetApp().plater()->get_camera();

    const float bbox_size    = float(mv->mesh().bounding_box().size().norm());
    const float panel_half   = bbox_size * 0.04f + 1.f;
    const float arrow_length = panel_half * 2.2f;
    // Where the handle is drawn (moves with the layer's offset) vs. the plane the drag is measured
    // against (fixed at the anchor). Keeping them apart is what stops the handle's own motion from
    // feeding back into the delta that produced it.
    const Vec3f handle_center_local = adjust_handle_center(*layer);
    const Vec3f drag_plane_local    = adjust_plane_point();
    const Vec3d handle_center_world = trafo_matrix * handle_center_local.cast<double>();
    const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());

    Vec3f u_axis, v_axis;
    adjust_tangent_basis(u_axis, v_axis);

    const Point handle_screen = CameraUtils::project(camera, handle_center_world);
    const Vec2d handle_screen_d(double(handle_screen.x()), double(handle_screen.y()));

    // Point-to-segment distance in screen space, for the arrow shafts.
    auto dist_to_segment_px = [](const Vec2d &p, const Vec2d &a, const Vec2d &b) {
        const Vec2d ab = b - a;
        const double len2 = ab.squaredNorm();
        const double t = (len2 > 1e-9) ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
        return (p - (a + ab * t)).norm();
    };

    if (mouse_event.LeftDown()) {
        const Vec3d u_tip_world = trafo_matrix * (handle_center_local + u_axis * arrow_length).cast<double>();
        const Vec3d v_tip_world = trafo_matrix * (handle_center_local + v_axis * arrow_length).cast<double>();
        const Point u_tip_screen = CameraUtils::project(camera, u_tip_world);
        const Point v_tip_screen = CameraUtils::project(camera, v_tip_world);
        const Vec2d u_tip_screen_d(double(u_tip_screen.x()), double(u_tip_screen.y()));
        const Vec2d v_tip_screen_d(double(v_tip_screen.x()), double(v_tip_screen.y()));

        // Panel screen-space "radius", approximated from one corner (a loose circle around the
        // square is close enough for hit-testing purposes).
        const Vec3d panel_corner_world = trafo_matrix * (handle_center_local + (u_axis + v_axis) * panel_half).cast<double>();
        const Point panel_corner_screen = CameraUtils::project(camera, panel_corner_world);
        const double panel_screen_radius = (Vec2d(double(panel_corner_screen.x()), double(panel_corner_screen.y())) - handle_screen_d).norm();

        constexpr double pick_tolerance_px = 8.0;
        const double dist_to_u = dist_to_segment_px(mouse_pos, handle_screen_d, u_tip_screen_d);
        const double dist_to_v = dist_to_segment_px(mouse_pos, handle_screen_d, v_tip_screen_d);
        const double dist_to_panel = (handle_screen_d - mouse_pos).norm();

        // Arrows take priority over the panel (their tips extend past it), then the panel covers
        // the broader central area.
        if (dist_to_u <= pick_tolerance_px && dist_to_u <= dist_to_v)
            m_adjust_drag_handle = AdjustHandle::AxisU;
        else if (dist_to_v <= pick_tolerance_px)
            m_adjust_drag_handle = AdjustHandle::AxisV;
        else if (dist_to_panel <= panel_screen_radius)
            m_adjust_drag_handle = AdjustHandle::Pan;
        else {
            m_adjust_drag_handle = AdjustHandle::None;
            return false;
        }

        m_adjust_drag_start_offset = layer->offset;

        Vec3d world_hit;
        if (ray_plane_hit(camera, mouse_pos, trafo_matrix, drag_plane_local, m_adjust_anchor_normal, world_hit)) {
            const Vec3f local_hit = (trafo_matrix.inverse() * world_hit).cast<float>();
            m_adjust_drag_start_planar = project_planar(local_hit, m_adjust_anchor_normal);
        }
        return true;
    }

    if (mouse_event.Dragging() && m_adjust_drag_handle != AdjustHandle::None) {
        Vec3d world_hit;
        if (!ray_plane_hit(camera, mouse_pos, trafo_matrix, drag_plane_local, m_adjust_anchor_normal, world_hit))
            return true;
        const Vec3f local_hit      = (trafo_matrix.inverse() * world_hit).cast<float>();
        const Vec2f current_planar = project_planar(local_hit, m_adjust_anchor_normal);

        Vec2f delta_planar = current_planar - m_adjust_drag_start_planar;
        // project_planar()'s (x, y) axes are exactly u_axis/v_axis (see adjust_tangent_basis()),
        // so zeroing one component constrains the drag to only the other axis.
        if (m_adjust_drag_handle == AdjustHandle::AxisU)
            delta_planar.y() = 0.f;
        else if (m_adjust_drag_handle == AdjustHandle::AxisV)
            delta_planar.x() = 0.f;

        const float scale        = (layer->tiling_scale > 1e-6f) ? (1.f / layer->tiling_scale) : 1.f;
        const Vec2f delta_scaled = delta_planar * scale;
        const float rad          = layer->rotation_deg * float(M_PI) / 180.f;
        const float cs = std::cos(rad), sn = std::sin(rad);
        Vec2f delta_rotated(delta_scaled.x() * cs - delta_scaled.y() * sn, delta_scaled.x() * sn + delta_scaled.y() * cs);
        // ...and the same v scaling apply_uv_transform() applies for a non-square texture, so the
        // handle keeps tracking the cursor exactly instead of drifting on the v axis.
        delta_rotated.y() *= layer_texture_aspect(*layer);
        // Increasing `offset` shifts which texel is sampled at a fixed world position, which
        // visually slides the pattern the *opposite* way - subtracting is this session's
        // best-effort reasoning about the direction that feels like "dragging the texture",
        // unverified against an actual render (see header comment).
        layer->offset = m_adjust_drag_start_offset - delta_rotated;

        m_preview_params_dirty = true;
        m_parent.set_as_dirty();
        return true;
    }

    if (mouse_event.LeftUp() && m_adjust_drag_handle != AdjustHandle::None) {
        m_adjust_drag_handle = AdjustHandle::None;
        rebuild_preview();
        m_preview_params_dirty = false;
        return true;
    }

    return false;
}

ModelVolume* GLGizmoTextureDisplacement::texture_volume()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return nullptr;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            return mv;
    return nullptr;
}

const ModelVolume* GLGizmoTextureDisplacement::texture_volume() const
{
    return const_cast<GLGizmoTextureDisplacement *>(this)->texture_volume();
}

std::vector<ModelVolume *> GLGizmoTextureDisplacement::texture_parts()
{
    std::vector<ModelVolume *> out;
    ModelObject               *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return out;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            out.push_back(mv);
    return out;
}

std::vector<ModelVolume *> GLGizmoTextureDisplacement::painted_parts()
{
    std::vector<ModelVolume *> out;
    for (ModelVolume *mv : texture_parts())
        if (mv->is_texture_displacement_painted())
            out.push_back(mv);
    return out;
}

bool GLGizmoTextureDisplacement::any_part_painted() const
{
    for (const ModelVolume *mv : const_cast<GLGizmoTextureDisplacement *>(this)->texture_parts())
        if (mv->is_texture_displacement_painted())
            return true;
    return false;
}

std::vector<TextureDisplacementLayer> GLGizmoTextureDisplacement::layers_for_part(const ModelVolume &part) const
{
    const ModelVolume *owner = texture_volume();
    if (owner == nullptr)
        return {};
    std::vector<TextureDisplacementLayer> out = owner->texture_displacement_layers;
    if (&part == owner)
        return out;
    for (TextureDisplacementLayer &layer : out) {
        layer.lscm_seam_edges.clear();
        layer.islands.clear();
        layer.island_groups.clear();
        layer.lscm_uv_overrides.clear();
    }
    return out;
}

Vec3d GLGizmoTextureDisplacement::texture_anchor() const
{
    const ModelVolume *owner = texture_volume();
    return owner != nullptr ? Vec3d(texture_displacement_volume_to_world(*owner).translation()) : Vec3d::Zero();
}

std::vector<float> GLGizmoTextureDisplacement::resting_drops(const std::vector<ModelVolume *> &parts)
{
    const auto bottom = [](const ModelVolume &part) {
        return part.mesh().transformed_bounding_box(texture_displacement_volume_to_world(part)).min.z();
    };
    // Each part's bottom is measured the same way as the object's, so the lowest part's drop is exactly
    // 0 and a single-part object bakes against its own bottom as it always has.
    double object_bottom = std::numeric_limits<double>::max();
    for (const ModelVolume *part : texture_parts())
        object_bottom = std::min(object_bottom, bottom(*part));
    std::vector<float> out;
    out.reserve(parts.size());
    for (const ModelVolume *part : parts)
        out.push_back(float(bottom(*part) - object_bottom));
    return out;
}

Transform3d GLGizmoTextureDisplacement::prepare_texture_frame(const ModelVolume &part)
{
    if (texture_parts().size() < 2)
        return Transform3d::Identity();
    return texture_displacement_bake_frame(texture_displacement_volume_to_world(part), texture_anchor());
}

void GLGizmoTextureDisplacement::update_model_object()
{
    store_panel_state();
    bool         updated = false;
    ModelObject *mo      = m_c->selection_info()->model_object();
    int          idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        FacetsAnnotation &facet = mv->texture_displacement_facet(m_active_layer_slot);
        // See m_selectors_stale: the mask could not be loaded into this selector, so an empty selector
        // here is "failed to load", not "nothing painted", and writing it back would erase the paint.
        // A selector that does hold something is the user's own work and must be flushed as usual.
        if (m_selectors_stale && !facet.empty() && m_triangle_selectors[idx]->serialize().triangles_to_split.empty())
            continue;
        updated |= facet.set(*m_triangle_selectors[idx]);
    }

    // The fast (shaded) preview reads the live selector, so it has to be rebuilt after any stroke that
    // flushes here - not only when set() reports a change. Rebuilding it via rebuild_preview() below
    // is gated on `updated`, which misses e.g. the first paint into a slot; marking it dirty makes the
    // render loop (render_painter_gizmo) rebuild it next frame regardless. Without this, fast preview -
    // now the default view - stayed blank until a full reload (select-whole-model / reopen).
    m_shaded_preview_dirty = true;

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        rebuild_preview();
    }
}

void GLGizmoTextureDisplacement::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    // Opening the gizmo puts the panel back where it was left: the layer and the mode come off the
    // volume, because they are the user's place in this model's stack, and the dock state off the
    // application config, because it is about their window. Only on open - this also runs on an
    // undo/redo and on a layer switch, where overriding what the user is doing now would be wrong.
    if (first_update) {
        if (const ModelVolume *mv = texture_volume(); mv != nullptr) {
            const TextureDisplacementOptions &opts = mv->texture_displacement_options;
            m_panel_mode = std::clamp(opts.panel_mode, 0, 1);
            // Only if that slot still holds a layer: the stack can have been edited by anything that
            // does not go through the panel, and a selection pointing at nothing shows an empty panel.
            const auto &stack = mv->texture_displacement_layers;
            if (std::any_of(stack.begin(), stack.end(),
                            [&opts](const TextureDisplacementLayer &l) { return l.slot == opts.active_slot; }))
                m_active_layer_slot = opts.active_slot;
        }
        m_undocked = wxGetApp().app_config->get_bool(UNDOCKED_CONFIG_KEY);
    }

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    // The base keeps the last mesh a fill tool hovered, and render_painter_gizmo() reads it as a hover that is
    // still on: a new set of selectors has none.
    m_seed_fill_last_mesh_id = -1;

    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    m_selectors_stale = false;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        const TriangleMesh                            *mesh = &mv->mesh();
        const TriangleSelector::TriangleSplittingData &data =
            mv->texture_displacement_facet(m_active_layer_slot).get_data();
        // The same bound TriangleSelector::deserialize() checks before it gives up - silently, with a
        // void return and no way to report it. A mask recorded before the mesh was replaced indexes
        // triangles that no longer exist, and the selector then comes back empty even though the mask
        // is not. That has to be caught here, because the next update_model_object() would otherwise
        // write the empty selector back over the mask: the paint would vanish, and the bake would
        // report "nothing is painted" about the very data the flush had just deleted.
        const size_t facet_count = mesh->its.indices.size();
        for (const TriangleSelector::TriangleBitStreamMapping &m : data.triangles_to_split)
            if (m.triangle_idx < 0 || size_t(m.triangle_idx) >= facet_count) {
                m_selectors_stale = true;
                break;
            }
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // The mask uses a single state (ENFORCER): a higher state in the file is reset to NONE.
        m_triangle_selectors.back()->deserialize(data, false, EnforcerBlockerType::ENFORCER);
        m_triangle_selectors.back()->request_update_render_data();
    }

    // Start a freshly opened, never-textured volume with one layer already in place, so the panel is
    // ready to paint straight away rather than showing an empty layer list. Only on first open, and
    // only when there are none - never during an undo/redo or layer-switch reload (which also come
    // through here), where silently adding a layer would be wrong.
    if (first_update)
        if (ModelVolume *tv = texture_volume(); tv != nullptr && tv->texture_displacement_layers.empty()) {
            add_texture_layer(); // takes its own snapshot and rebuilds the preview
            return;
        }

    rebuild_preview();
}

void GLGizmoTextureDisplacement::store_panel_state()
{
    if (ModelVolume *mv = texture_volume(); mv != nullptr) {
        mv->texture_displacement_options.active_slot = m_active_layer_slot;
        mv->texture_displacement_options.panel_mode  = m_panel_mode;
    }
}

void GLGizmoTextureDisplacement::set_active_layer(int slot)
{
    if (slot == m_active_layer_slot)
        return;
    // Flush edits made while the previous layer was active before switching what the selectors
    // reflect - otherwise they would be silently lost.
    update_model_object();
    m_active_layer_slot = slot;
    store_panel_state();
    update_from_model_object(false);
    // The on-canvas gizmo (if on) is anchored to whichever layer is active - keep it in sync
    // instead of leaving it pointing at the previous layer's (now stale) paint patch.
    if (m_adjust_texture_mode)
        update_adjust_anchor();
    // Refresh every preview/overlay (shaded, UV editor, seams, ...) for the newly active layer.
    rebuild_preview();
}

void GLGizmoTextureDisplacement::ensure_panel_icons()
{
    if (m_panel_icons_tried)
        return;
    m_panel_icons_tried = true;

    // Order is irrelevant; the map keys by file name. Loaded with color_wite_gray so each icon has both
    // a monochrome ("normal", grey in the current theme) and an original-colour variant.
    static const std::vector<std::string> names = {
        "texture_displacement_brush.svg", "texture_displacement_face.svg", "texture_displacement_connected_area.svg",
        "texture_displacement_real_preview.svg", "texture_displacement_fast_preview.svg",
        "texture_displacement_checker.svg", "texture_displacement_distortion.svg",
        "texture_displacement_wireframe.svg", "texture_displacement_cross.svg",
        "texture_displacement_uv_select_island.svg", "texture_displacement_uv_select_edge.svg",
        "cog.svg", // the layer card's "adjust image" button
        "texture_displacement_uv_select_vertex.svg",
        // Paint / Erase, brush cursor shape, mapping, tiling, placement and the dock toggle.
        "texture_displacement_add.svg", "texture_displacement_add_negative.svg", "circle_paint.svg",
        "menu_obj_sphere.svg", "menu_obj_cube.svg", "menu_obj_cylinder.svg", "texture_displacement_map_unwrap.svg",
        "texture_displacement_map_view.svg", "texture_displacement_map_model_uv.svg",
        "texture_displacement_tile_repeat.svg", "menu_mirror_x.svg",
        "texture_displacement_adjust.svg", "canvas_drag.svg", "texture_displacement_move_up.svg",
        "texture_displacement_move_down.svg", "texture_displacement_drag.svg",
        "texture_displacement_select_all.svg", "texture_displacement_erase_all.svg",
        // The "type a value instead" icon at the right end of every slider.
        "edit.svg",
    };
    std::vector<std::string> paths;
    paths.reserve(names.size());
    for (const std::string &n : names)
        paths.push_back(resources_dir() + "/images/" + n);

    // Runs from the panel render, i.e. with a GL context current, so the upload is safe here.
    //
    // Rasterized at twice GLToolbar::Default_Icons_Size rather than at it: these are drawn at the
    // toolbar's icon size, which is that constant scaled by DPI (toolbar_icon_scale() folds in
    // em_unit), so on a 200% display the draw size reaches 80 px. Rasterizing at 40 would upscale a
    // 40 px bitmap there, which is what actually reads as blurry - downscaling does not. The icon set
    // is rasterized once for the gizmo's lifetime, so it cannot re-raster on a DPI change; sizing for
    // the larger case and letting ImGui shrink it is the version that looks right on both.
    const std::vector<IconManager::Icons> icons =
        m_panel_icons.init(paths, ImVec2(2 * GLToolbar::Default_Icons_Size, 2 * GLToolbar::Default_Icons_Size),
                           IconManager::RasterType::color_wite_gray);
    for (size_t i = 0; i < names.size() && i < icons.size(); ++i)
        m_panel_icon_map[names[i]] = icons[i];
}

// Seconds on a real clock, for the tooltips' own timing.
//
// Not ImGui::GetTime(): ImGuiWrapper::new_frame() feeds io.DeltaTime the real frame time but caps it
// at 1/60 s, so the idle gaps of the event-driven canvas don't count as time a +/- button is held.
// That cap also means ImGui's clock stalls while the canvas is idle - a pointer resting still on a
// control renders nothing, so the tooltip delay would never run out on ImGui's clock. A real clock
// keeps the tooltip timing and media playback independent of how often the canvas renders.
static double tip_now()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point base = clock::now();
    return std::chrono::duration<double>(clock::now() - base).count();
}

// How long the pointer has to rest on a control before its tooltip appears. The panel is dense and the
// tooltip lands on the model beside it, so with no wait at all a sweep from the top of the panel down to
// Bake flashes a dozen large panels over the scene. Once one has just been up the wait drops to almost
// nothing, so reading several settings in a row stays quick - but not to nothing, or that same sweep
// would flash again the moment one tooltip had been seen.
static constexpr double TIP_DELAY       = 0.45;
static constexpr double TIP_DELAY_AGAIN = 0.10;
static constexpr double TIP_STICKY      = 0.70;

void GLGizmoTextureDisplacement::panel_tooltip(const wxString &text, const std::vector<TipMedia> &media)
{
    panel_tooltip(into_u8(text), media);
}

void GLGizmoTextureDisplacement::panel_tooltip(const std::string &text, const std::vector<TipMedia> &media)
{
    // Nothing explains a control while the control is being used: dragging a slider, typing a value
    // into one, picking from a combo, dragging a layer card. The pointer sits on the very thing being
    // edited, so without this the tooltip covers the model exactly while the user is watching it change.
    // The full wait comes back afterwards rather than the short one - having just let go of a slider is
    // not a reason to be shown a picture of it.
    if (ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown()) {
        m_tip_item       = ImVec2(-1.f, -1.f);
        m_tip_last_shown = -1e9;
        return;
    }

    // The item's position stands in for its identity: moving to another control restarts the wait, and
    // staying on this one carries it on.
    const double now  = tip_now();
    const ImVec2 item = ImGui::GetItemRectMin();
    if (item.x != m_tip_item.x || item.y != m_tip_item.y) {
        m_tip_item        = item;
        m_tip_hover_since = now;
    }
    if (now - m_tip_hover_since < (now - m_tip_last_shown < TIP_STICKY ? TIP_DELAY_AGAIN : TIP_DELAY)) {
        // A pointer resting on a control produces no events at all, so unless the wait asks for frames
        // itself nothing redraws the canvas and the wait never finishes - the tooltip then appears only
        // on the next pixel of mouse movement.
        //
        // request_extra_frame() and not schedule_extra_frame(): the latter only arranges a timer, whose
        // event an idle event is assumed to follow, and GLCanvas3D::on_idle() renders nothing unless the
        // canvas was already dirty when it ran. request_extra_frame() ends on_idle with
        // evt.RequestMore(), which obliges wx to deliver the next one. It is what 3DBed's fade uses.
        // Costing a few hundred milliseconds of frames is the cheaper half of that trade.
        m_parent.request_extra_frame();
        return;
    }
    m_tip_last_shown = now;

    // ImGui hides a window on the first frame it is submitted, to measure it before showing it - and
    // for a tooltip it does so every time one reappears, because the windows are recycled (see the
    // Popup|Tooltip branch of Begin()). So the frame on which a tooltip is decided is never the frame
    // on which it is seen, and one that then asks for nothing more stays invisible until the next pixel
    // of mouse movement happens to produce another frame. Keep frames coming until it has settled.
    const int shown_frame = ImGui::GetFrameCount();
    m_tip_settled         = (shown_frame == m_tip_shown_frame + 1) ? m_tip_settled + 1 : 0;
    m_tip_shown_frame     = shown_frame;
    if (m_tip_settled < 2)
        m_parent.request_extra_frame();

    const ImGuiStyle &style = ImGui::GetStyle();
    const float       gap   = m_imgui->scaled(0.5f);
    const float       wrap  = m_imgui->scaled(20.f);
    // The width has to be known here, not left to ImGui, because the tooltip is placed by its left edge
    // (below) and an illustration has to be laid out against something. One carrying illustrations takes
    // the full wrap width; a plain one takes exactly what its text needs, as it did when ImGui sized it,
    // so a two-word tooltip is not a wide box with two words in it.
    const float text_w = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).x;
    const float width  = (media.empty() ? std::min(wrap, std::ceil(text_w)) : wrap) + 2.f * style.WindowPadding.x;

    // Beside the anchor (the panel, or the texture picker when the tooltip comes from inside it), never
    // over it - the same reasoning, and the same placement, as the picker popup itself. ImGui normally
    // keeps a tooltip glued to the mouse; it honours a position set here instead (see the Orca branch in
    // imgui's Begin()), and still clamps the result inside the canvas.
    const bool  left = m_tooltip_anchor_min.x - gap - width >= 0.f;
    const float x    = left ? m_tooltip_anchor_min.x - gap - width : m_tooltip_anchor_max.x + gap;
    ImGui::SetNextWindowPos(ImVec2(x, ImGui::GetItemRectMin().y)); // level with the control it explains
    ImGui::SetNextWindowSize(ImVec2(width, 0.f));                  // height still follows the content

    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::PushTextWrapPos(0.f); // the window's own edge, which is what the fixed width above sets
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    render_tip_media(media, width - 2.f * style.WindowPadding.x);
    ImGui::EndTooltip();
}

void GLGizmoTextureDisplacement::render_tip_media(const std::vector<TipMedia> &media, float width)
{
    // Collect what is actually there first: the layout divides the width between the illustrations, so
    // one that is missing has to drop out of the count rather than leave a gap.
    std::vector<std::pair<const AnimatedTexture *, const TipMedia *>> shown;
    std::string                                                       key;
    for (const TipMedia &m : media) {
        const AnimatedTexture *texture =
            m_tip_media.get((boost::filesystem::path(resources_dir()) / "images" / "texture_displacement" / m.file).string());
        if (texture != nullptr && texture->is_valid() && texture->width() > 0 && texture->height() > 0) {
            shown.emplace_back(texture, &m);
            key += m.file + "|";
        }
    }
    if (shown.empty())
        return;

    // Each animation plays from its first frame when its tooltip opens: these are short loops showing
    // one setting going from one value to another, and joining one halfway through shows the user the
    // end of a change they never saw the start of.
    // Restart on a different tooltip, and on the same one opening again - which is what a gap in the
    // frames it was drawn on means, since it is drawn on every frame for as long as it is on screen.
    const int this_frame = ImGui::GetFrameCount();
    if (key != m_tip_media_key || this_frame != m_tip_media_frame + 1) {
        m_tip_media_key   = key;
        m_tip_media_start = tip_now();
    }
    m_tip_media_frame = this_frame;
    const double time = tip_now() - m_tip_media_start;

    const ImGuiStyle &style   = ImGui::GetStyle();
    const float       spacing = style.ItemSpacing.x;
    const float       cell_w  = (width - spacing * float(shown.size() - 1)) / float(shown.size());
    const ImU32       frame   = ImGui::GetColorU32(ImGuiCol_Text, 0.25f);

    // A single illustration would otherwise be drawn at the tooltip's full width, and a square one at
    // full width is a picture as tall as the text beside it is long. Capped and centred in its column.
    const float max_h = m_imgui->scaled(10.f);

    ImGui::Spacing();
    for (size_t i = 0; i < shown.size(); ++i) {
        const AnimatedTexture *texture = shown[i].first;
        const TipMedia        &entry   = *shown[i].second;
        const float            cell_x  = ImGui::GetCursorPosX();

        float w = cell_w;
        float h = std::round(cell_w * float(texture->height()) / float(texture->width()));
        if (h > max_h) {
            w = std::round(max_h * float(texture->width()) / float(texture->height()));
            h = max_h;
        }

        ImGui::BeginGroup();
        const unsigned int id = texture->texture_id(time);
        ImGui::SetCursorPosX(cell_x + std::round(0.5f * (cell_w - w)));
        ImGui::Image((ImTextureID) (intptr_t) id, ImVec2(w, h));
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), frame, style.FrameRounding);
        // The canvas only redraws on demand, so an animation has to ask for its own frames. It asks for
        // them as fast as they come rather than pacing itself with a timer: which frame is drawn is
        // decided by the clock above, so a redraw that arrives early simply draws the same frame again,
        // and a guaranteed frame is worth more here than a frugal one that may never arrive.
        if (texture->is_animated())
            m_parent.request_extra_frame();

        if (!entry.caption.empty()) {
            // Centred under its own illustration, and never wider than its column.
            const std::string caption = into_u8(entry.caption);
            const float       text_w  = ImGui::CalcTextSize(caption.c_str()).x;
            ImGui::SetCursorPosX(cell_x + std::max(0.f, (cell_w - text_w) * 0.5f));
            ImGui::PushTextWrapPos(cell_x + cell_w);
            ImGui::TextDisabled("%s", caption.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();

        if (i + 1 < shown.size())
            ImGui::SameLine(0.f, spacing);
    }
}

void GLGizmoTextureDisplacement::add_texture_layer()
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> used{};
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot >= 0 && size_t(l.slot) < used.size())
            used[size_t(l.slot)] = true;
    int free_slot = -1;
    for (size_t i = 0; i < used.size(); ++i)
        if (!used[i]) { free_slot = int(i); break; }
    if (free_slot < 0) {
        show_error(nullptr, _u8L("Maximum number of texture displacement layers reached."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    TextureDisplacementLayer layer;
    layer.slot = free_slot;
    // A model that came with a UV channel is mapped by it. Re-unwrapping such a model throws away the
    // layout it was authored against and can only be worse than using it, so the default follows the
    // model rather than the other way round; the Mapping row still offers every other method.
    if (mv->has_texture_uv())
        layer.projection_method = TextureProjectionMethod::ModelUV;
    // Start the layer off on the first library texture rather than on nothing at all: a textureless
    // layer looks broken (painting on it appears to do nothing, because there is no height map to
    // displace by). The user swaps it for another from the layer's own picker.
    const std::vector<TextureLibraryEntry> &library = texture_library();
    if (!library.empty())
        if (const LibraryTexture *tex = get_library_texture(library.front().path)) {
            layer.name       = library.front().name;
            layer.path       = library.front().path;
            layer.image_data = tex->image_data;
        }
    mv->texture_displacement_layers.push_back(std::move(layer));
    // A new layer opens with every setting showing; there is nothing on it yet to hide settings from.
    m_layer_expanded[size_t(free_slot)] = true;

    set_active_layer(free_slot);
    // set_active_layer() is a no-op when the new slot happens to be the one already active (slot 0,
    // for the very first layer added), so the preview would not pick the new texture up on its own.
    rebuild_preview();
    m_parent.set_as_dirty();
}

const GLGizmoTextureDisplacement::LibraryTexture *GLGizmoTextureDisplacement::get_library_texture(const std::string &path)
{
    if (auto it = m_library_textures.find(path); it != m_library_textures.end())
        return it->second.image_data ? &it->second : nullptr;

    LibraryTexture entry;
    std::string    error;
    entry.image_data = load_texture_image_data(path, error);
    if (entry.image_data) {
        TextureDisplacementLayer probe;
        probe.image_data = entry.image_data;
        entry.thumbnail  = upload_height_thumbnail(decode_height_texture(probe, /* keep_decoded */ false));
        if (!entry.thumbnail)
            entry.image_data.reset(); // decoded to nothing usable - treat it as a failed load
    } else {
        BOOST_LOG_TRIVIAL(error) << "Texture displacement: could not load texture " << path << ": " << error;
    }

    // Cached whether it loaded or not: a file that failed is remembered as unusable, so the picker
    // does not retry (and re-log) it on every frame it is on screen.
    const auto [pos, inserted] = m_library_textures.emplace(path, std::move(entry));
    return pos->second.image_data ? &pos->second : nullptr;
}

void GLGizmoTextureDisplacement::set_layer_texture(TextureDisplacementLayer &layer, const TextureLibraryEntry &entry)
{
    const LibraryTexture *tex = get_library_texture(entry.path);
    if (tex == nullptr) {
        show_error(nullptr, _u8L("Could not load the selected texture."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change texture displacement texture"), UndoRedo::SnapshotType::GizmoAction);
    layer.name = entry.name;
    layer.path = entry.path;
    // Handing over the library's own buffer (rather than a copy) is what lets decode_height_texture()
    // - whose cache is keyed by exactly this pointer - hit straight away instead of re-decoding
    // the PNG the first time the layer is previewed or baked.
    layer.image_data = tex->image_data;
    // The user's own textures are listed most recently used first.
    if (entry.source == TextureSource::User)
        touch_user_texture(entry.path);

    rebuild_preview();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::import_custom_texture(TextureDisplacementLayer &layer)
{
    const wxString wildcard = _L("Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp");
    wxFileDialog   dialog(nullptr, _L("Choose a texture image (height map)"), wxEmptyString, wxEmptyString, wildcard,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    // Converts to the 8-bit grayscale PNG the bake code understands and copies it into the user's
    // own texture folder, so it stays available for later models (and survives an app update, which
    // rewrites the shipped folder wholesale).
    std::string                              error;
    const std::optional<TextureLibraryEntry> entry = import_texture_to_library(into_u8(dialog.GetPath()), error);
    if (!entry) {
        show_error(nullptr, error);
        return;
    }
    set_layer_texture(layer, *entry);
}

void GLGizmoTextureDisplacement::ease_wheel_scroll(float &target, float &applied)
{
    ImGuiIO    &io         = ImGui::GetIO();
    const float scroll_now = ImGui::GetScrollY();
    const float scroll_max = ImGui::GetScrollMaxY();

    // Anything that moved the scroll without us - dragging the grab, a keyboard/gamepad nav step, the
    // content shrinking under a clamped offset, a SetScroll*() call - has to re-seed the target, or the
    // easing below would immediately drag the view back to where it last animated to.
    if (applied < 0.f || std::abs(scroll_now - applied) > 0.5f)
        target = scroll_now;

    if (io.MouseWheel != 0.f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        target -= io.MouseWheel * ImGui::GetFontSize() * 4.f;
    // Whole pixels: ImGui floors whatever SetScrollY() is given, so a fractional target could never be
    // reached and the "still gliding" test below would stay true forever, repainting the canvas for good.
    target = std::floor(std::clamp(target, 0.f, scroll_max));

    const float delta = target - scroll_now;
    if (std::abs(delta) >= 1.f) {
        // Exponential ease, formulated against the frame time so the glide takes the same wall time
        // whether the canvas is running at 30 or 144 fps. The last sub-pixel step would be floored
        // away, so land on the target outright once the remainder is that small.
        const float t    = 1.f - std::exp(-20.f * std::clamp(io.DeltaTime, 1.f / 240.f, 1.f / 15.f));
        const float step = (std::abs(delta * t) < 1.f) ? delta : delta * t;
        const float next = std::floor(scroll_now + step);
        ImGui::SetScrollY(next);
        applied = next;
        m_parent.set_as_dirty(); // nothing else would redraw mid-glide once the mouse stops
    } else {
        applied = scroll_now;
    }
}

void GLGizmoTextureDisplacement::render_texture_library_popup(const ImVec2 &panel_min, const ImVec2 &panel_max)
{
    ModelVolume              *mv    = texture_volume();
    TextureDisplacementLayer *layer = nullptr;
    if (mv != nullptr)
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.slot == m_picker_slot)
                layer = &l;

    static const char *const popup_id = "##texture_library";
    if (m_picker_open_request) {
        m_picker_open_request = false;
        if (layer != nullptr)
            ImGui::OpenPopup(popup_id);
    }

    const float       tile        = std::round(m_imgui->scaled(3.2f));
    const float       spacing     = std::round(m_imgui->scaled(0.4f));
    const float       grid_w      = 4.f * tile + 3.f * spacing;
    const float       gap         = m_imgui->scaled(0.5f);
    const float       scrollbar_w = m_imgui->scaled(0.5f);
    const float       ring_w      = std::max(2.f, std::round(m_imgui->scaled(0.1f))); // the current texture's ring
    const float       frame_h     = ImGui::GetFrameHeight();
    const ImVec4      orca        = ImGuiWrapper::COL_ORCA;
    const ImGuiStyle &style       = ImGui::GetStyle();

    // A fixed size rather than a fit-to-content one. The width is the four-column grid, a gutter and the
    // slim scrollbar beside it, plus the window padding, so the columns always fit and no horizontal
    // scrollbar is needed; it derives from scaled values, so it tracks the DPI like the rest.
    const float fixed_w = grid_w + spacing + scrollbar_w + 2.f * style.WindowPadding.x;

    // BeginPopup() ORs in ImGuiWindowFlags_AlwaysAutoResize (deps_src/imgui/imgui.cpp:8371) and no flag
    // turns it back off. A SetNextWindowSize() overrides it for each axis it sets (imgui.cpp:5983-5996),
    // so an explicit size is the only way to fix the popup's size; the grid child below then scrolls.
    //
    // Top-aligned with the panel, which keeps it clear of the gizmo toolbar above a docked panel, and as
    // tall as the panel or 70% of the window, whichever is taller, while keeping a gap inside the window.
    // The height follows the window and the panel, never the content, so it does not jump as packs load or
    // sections collapse.
    const ImGuiViewport *viewport  = ImGui::GetMainViewport();
    const float          vp_top    = viewport->Pos.y + gap;
    const float          vp_bottom = viewport->Pos.y + viewport->Size.y - gap;
    const float          height    = std::max(std::min(std::max(0.70f * viewport->Size.y, panel_max.y - panel_min.y), vp_bottom - vp_top),
                                              2.f * frame_h);
    const float          popup_y   = std::max(vp_top, std::min(panel_min.y, vp_bottom - height));
    // Beside the panel rather than over it, so the layer being retextured stays in view: to its left when
    // there is room (the docked panel sits against the right edge of the canvas), otherwise to its right.
    const float width   = viewport->Size.x > 0.f ? std::min(fixed_w, viewport->Size.x) : fixed_w;
    const bool  left    = panel_min.x - gap - width >= 0.f;
    const float popup_x = left ? panel_min.x - gap - width : panel_max.x + gap;
    // Cond_Always, not Appearing: a viewport resize while the popup is open must move and resize it too.
    ImGui::SetNextWindowPos(ImVec2(popup_x, popup_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    // Drawn as the panel's own sheet: its opaque background and rounding, no border. A popup is filled with
    // PopupBg, which the panel's toolbar style leaves at ImGui's translucent near-black - a dark window
    // beside a light panel, with whatever it covers showing through. Only Begin reads these, so they are
    // popped at once and the tooltips shown from inside keep their own look.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, style.WindowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
    // The window itself never scrolls; the grid child below does.
    const bool open = ImGui::BeginPopup(popup_id, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (!open)
        return;
    if (layer == nullptr) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    // While this popup is up its own tooltips belong beside it, not beside the panel it is already
    // covering the left of. Restored before every exit below.
    const ImVec2 prev_anchor_min = m_tooltip_anchor_min;
    const ImVec2 prev_anchor_max = m_tooltip_anchor_max;
    m_tooltip_anchor_min         = ImGui::GetWindowPos();
    m_tooltip_anchor_max         = ImVec2(m_tooltip_anchor_min.x + ImGui::GetWindowSize().x,
                                          m_tooltip_anchor_min.y + ImGui::GetWindowSize().y);

    // The header stays put while the grid scrolls under it: the title says what the popup is for, and the
    // close button is there for anyone who does not know that a click outside closes it.
    bool close_clicked = false;
    {
        ImDrawList  *hdl = ImGui::GetWindowDrawList();
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 0.5f * (frame_h - ImGui::GetTextLineHeight())));
        m_imgui->bold_text(_u8L("Choose a texture"));
        const ImVec2 btn(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - frame_h, row.y);
        ImGui::SetCursorScreenPos(btn);
        close_clicked       = ImGui::InvisibleButton("##close", ImVec2(frame_h, frame_h));
        const bool   hot    = ImGui::IsItemHovered();
        const ImVec2 c(btn.x + 0.5f * frame_h, btn.y + 0.5f * frame_h);
        const float  k      = std::round(0.18f * frame_h);
        const float  line_w = std::max(1.f, std::round(m_imgui->scaled(0.08f)));
        const ImU32  x_col  = ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : 0.6f);
        if (hot)
            hdl->AddCircleFilled(c, 0.5f * frame_h, ImGui::GetColorU32(ImGuiCol_Text, 0.08f));
        hdl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), x_col, line_w);
        hdl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), x_col, line_w);
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + frame_h + style.ItemSpacing.y));
        ImGui::Separator();
    }

    const std::vector<TextureLibraryEntry> &library      = texture_library();
    // A reinstalled pack can drop new bytes at a path already cached on this gizmo, and get_library_texture()
    // caches a failure as well as a success, so a moved generation makes every cached entry suspect.
    if (const uint64_t generation = texture_library_generation(); generation != m_library_generation) {
        m_library_textures.clear();
        m_library_generation = generation;
    }

    // The grid scrolls as the panel body does: the same slim scrollbar, and the wheel eased by
    // ease_wheel_scroll() instead of ImGui's five-line jumps (hence NoScrollWithMouse).
    push_slim_scrollbar_style(scrollbar_w, wxGetApp().dark_mode());
    ImGui::BeginChild("##texture_grid", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_NoScrollWithMouse);
    ease_wheel_scroll(m_picker_scroll_target, m_picker_scroll_applied);

    ImDrawList                        *dl = ImGui::GetWindowDrawList(); // the child's, so tiles clip to the grid
    std::optional<TextureLibraryEntry> chosen;
    std::optional<TextureLibraryEntry> to_remove;
    bool                               import_clicked = false;
    bool                               download_clicked = false;
    int                                loads_left       = PICKER_LOADS_PER_FRAME;
    bool                               loads_deferred   = false;

    // Tiles are placed on the four-column grid explicitly rather than flowed with SameLine(): a user tile
    // overlays a remove button on its corner, and that button would otherwise be "the previous item" the
    // next tile lines up against.
    const auto tile_pos = [&](const ImVec2 &origin, int i) {
        return ImVec2(origin.x + float(i % 4) * (tile + spacing), origin.y + float(i / 4) * (tile + spacing));
    };
    // Leaves the cursor below a grid of `count` tiles, with the grid's area registered as content.
    const auto end_grid = [&](const ImVec2 &origin, int count) {
        ImGui::SetCursorScreenPos(origin);
        if (count > 0) {
            const int rows = (count + 3) / 4;
            ImGui::Dummy(ImVec2(grid_w, float(rows) * tile + float(rows - 1) * spacing));
        }
    };
    // One section's tiles, in the order the section holds them (shipped alphabetically, pack textures by
    // name, the user's own most recently used first - the order texture_library() produced); a user tile
    // carries a remove button on hover. Returns how many tiles it laid out, drawn or not.
    const auto grid = [&](const ImVec2 &origin, const std::vector<const TextureLibraryEntry *> &entries, bool removable) {
        int n = 0;
        for (const TextureLibraryEntry *entry : entries) {
            const ImVec2 rect_min = tile_pos(origin, n++);
            const ImVec2 rect_max = ImVec2(rect_min.x + tile, rect_min.y + tile);
            const bool   current  = entry->path == layer->path;
            // The selected tile is centred even when it is off screen, so it is drawn and loaded next frame.
            if (current && m_picker_reveal_selected) {
                ImGui::SetScrollFromPosY(0.5f * (rect_min.y + rect_max.y) - ImGui::GetWindowPos().y, 0.5f);
                m_parent.request_extra_frame();
            }
            if (!ImGui::IsRectVisible(rect_min, rect_max))
                continue;
            // Thumbnails decode on this thread, so only a few per frame; the rest fill in on later frames.
            const bool cached = m_library_textures.count(entry->path) != 0;
            if (!cached && loads_left == 0) {
                loads_deferred = true;
                dl->AddRectFilled(rect_min, rect_max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
                continue;
            }
            if (!cached)
                --loads_left;
            ImGui::SetCursorScreenPos(rect_min);
            ImGui::PushID(entry->path.c_str());
            const LibraryTexture *tex     = get_library_texture(entry->path);
            GLTexture            *thumb   = tex != nullptr ? tex->thumbnail.get() : nullptr;
            bool                  clicked = false;
            if (thumb != nullptr) {
                // Centre-crop a non-square image into the square tile instead of squashing it.
                const float aspect = thumb->get_height() > 0 ? float(thumb->get_width()) / float(thumb->get_height()) : 1.f;
                ImVec2      uv0(0.f, 0.f), uv1(1.f, 1.f);
                if (aspect > 1.f) {
                    uv0.x = 0.5f * (1.f - 1.f / aspect);
                    uv1.x = 1.f - uv0.x;
                } else if (aspect < 1.f) {
                    uv0.y = 0.5f * (1.f - aspect);
                    uv1.y = 1.f - uv0.y;
                }
                clicked = ImGui::ImageButton((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(tile, tile), uv0, uv1, 0);
            } else {
                clicked = ImGui::Button("?", ImVec2(tile, tile));
            }
            if (removable)
                ImGui::SetItemAllowOverlap(); // the remove button drawn over its corner takes its own clicks
            const ImVec2 a       = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool   hovered = ImGui::IsItemHovered();
            // The layer's current texture carries an accent ring, and the tile under the pointer a thinner,
            // fainter one. Both are drawn inside the tile, where the grid's clip rect never cuts them.
            if (current || hovered) {
                const float w = current ? ring_w : std::round(0.5f * ring_w);
                dl->AddRect(ImVec2(a.x + 0.5f * w, a.y + 0.5f * w), ImVec2(b.x - 0.5f * w, b.y - 0.5f * w),
                            ImGui::GetColorU32(current ? orca : ImVec4(orca.x, orca.y, orca.z, 0.6f)), 0.f, 0, w);
            }
            if (hovered)
                panel_tooltip(entry->name);

            // Clipped by the grid's clip rect, so a tile scrolled out of the grid or under its scrollbar
            // does not offer its remove button. IsMouseHoveringRect() clips by default.
            if (removable && ImGui::IsMouseHoveringRect(a, b)) {
                const float  r = std::round(tile * 0.17f);
                const ImVec2 c(b.x - r - 2.f, a.y + r + 2.f);
                ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
                const bool remove_clicked = ImGui::InvisibleButton("##remove", ImVec2(2.f * r, 2.f * r));
                const bool hot            = ImGui::IsItemHovered();
                dl->AddCircleFilled(c, r, hot ? IM_COL32(214, 72, 64, 235) : IM_COL32(0, 0, 0, 170));
                const float k = 0.42f * r;
                dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32_WHITE, 1.5f);
                dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), IM_COL32_WHITE, 1.5f);
                if (hot)
                    panel_tooltip(_u8L("Remove from My textures"));
                if (remove_clicked)
                    to_remove = *entry;
            }
            ImGui::PopID();
            if (clicked)
                chosen = *entry;
        }
        return n;
    };

    // Every section collapses; the state is remembered for the app run. Pack keys use pack_name, a
    // capability name unique per type across loaded packages and stable across reinstalls. This function-local
    // static survives popup closes, never touches app_config, and resets on restart.
    static std::map<std::string, bool> s_section_collapsed; // true = collapsed; absent = expanded

    const auto split_pack_title = [](const std::string &title) -> std::pair<std::string, std::string> {
        size_t split = std::string::npos;
        for (size_t i = 0; i + 3 < title.size(); ++i)
            if (title[i] == ' ' && (title[i + 1] == 'b' || title[i + 1] == 'B') &&
                (title[i + 2] == 'y' || title[i + 2] == 'Y') && title[i + 3] == ' ')
                split = i;
        if (split == std::string::npos)
            return { title, {} };

        std::string name = title.substr(0, split);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
        std::string attribution = title.substr(split);
        size_t      first         = 0;
        while (first < attribution.size() && std::isspace(static_cast<unsigned char>(attribution[first])))
            ++first;
        attribution.erase(0, first);
        if (name.empty() || attribution.empty())
            return { title, {} };
        return { name, attribution };
    };

    // A section heading, one row as wide as the grid: a click anywhere on it collapses or expands the
    // section. Child headings indent their contents while keeping their counts right-aligned. Attribution
    // lines use a smaller disabled style; the grid follows closer than the next heading does.
    // Returns true when the section is open, so the caller knows whether to draw its contents.
    const auto section_header = [&](const std::string &key, const std::string &title, size_t count, float indent,
                                    const std::string &attribution, const std::string &full_title) {
        bool        &collapsed = s_section_collapsed[key]; // default-inserts false -> expanded
        const ImVec2 row       = ImGui::GetCursorScreenPos();
        const bool   two_lines = !attribution.empty();
        const float  line_h          = ImGui::GetTextLineHeight();
        const float  small_font_size = ImGui::GetFontSize() * 0.8f;
        const float  small_line_h    = line_h * 0.8f;
        const float  row_h           = two_lines ? std::max(frame_h, line_h + small_line_h + spacing) : frame_h;
        const ImVec2 row_max(row.x + grid_w, row.y + row_h);
        if (ImGui::InvisibleButton(key.c_str(), ImVec2(grid_w, row_h)))
            collapsed = !collapsed;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            dl->AddRectFilled(row, row_max, ImGui::GetColorU32(ImGuiCol_Text, 0.05f), style.FrameRounding);

        const float inset = std::round(0.25f * frame_h);
        const float arrow = std::round(ImGui::GetFontSize() * 0.55f);
        const float cx = row.x + inset + indent + 0.5f * arrow;
        const float cy = row.y + 0.5f * row_h;
        const float hs = 0.3f * arrow;
        const ImU32 accent = ImGui::GetColorU32(orca);
        if (collapsed)
            dl->AddTriangleFilled(ImVec2(cx - 0.5f * hs, cy - hs), ImVec2(cx + 0.5f * hs, cy), ImVec2(cx - 0.5f * hs, cy + hs), accent);
        else
            dl->AddTriangleFilled(ImVec2(cx - hs, cy - 0.5f * hs), ImVec2(cx + hs, cy - 0.5f * hs), ImVec2(cx, cy + 0.5f * hs), accent);

        const std::string n     = std::to_string(count);
        const float       n_w   = ImGui::CalcTextSize(n.c_str()).x;
        const float       tx    = row.x + inset + indent + arrow + spacing;
        const float       max_text_w = std::max(0.f, row_max.x - inset - n_w - spacing - tx);
        const std::string shown = ellipsize(title, max_text_w);
        const std::string shown_attribution = two_lines ? ellipsize(attribution, max_text_w, small_font_size) : std::string{};
        const float       title_y            = two_lines ? row.y + 0.5f * (row_h - line_h - small_line_h)
                                                         : row.y + 0.5f * (frame_h - line_h);
        const float       count_y            = row.y + 0.5f * (row_h - line_h);
        dl->AddText(ImVec2(tx, title_y), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        if (two_lines)
            dl->AddText(ImGui::GetFont(), small_font_size, ImVec2(tx, title_y + line_h),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), shown_attribution.c_str());
        dl->AddText(ImVec2(row_max.x - inset - n_w, count_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), n.c_str());
        if (hovered && (shown != title || shown_attribution != attribution))
            panel_tooltip(full_title); // a long plugin-supplied name, cut by the fixed width
        ImGui::SetCursorScreenPos(ImVec2(row.x, row_max.y + spacing));
        return !collapsed;
    };

    // Built-in is first, plugin packs are grouped under Texture libraries, and My
    // textures stays last so its import tile and help line describe that group. Pack names identify both
    // their child section and state.
    struct Section
    {
        std::string                              key;
        std::string                              title;
        std::vector<const TextureLibraryEntry *> entries;
    };
    std::vector<Section>          sections;
    std::map<std::string, size_t> pack_section; // pack_name -> index in `sections`
    sections.push_back({ "builtin:", _u8L("Built-in"), {} });
    for (const TextureLibraryEntry &entry : library) {
        if (entry.source == TextureSource::Shipped) {
            sections.front().entries.push_back(&entry); // the section exists even when the library is empty
            continue;
        }
        if (entry.source == TextureSource::User)
            continue; // added below, so My textures stays last
        if (entry.source != TextureSource::Plugin)
            continue;
        const auto [it, inserted] = pack_section.emplace(entry.pack_name, sections.size());
        if (inserted)
            sections.push_back({ "pack:" + entry.pack_name, entry.pack_name, {} });
        sections[it->second].entries.push_back(&entry);
    }
    sections.push_back({ "user:", _u8L("My textures"), {} });
    for (const TextureLibraryEntry &entry : library)
        if (entry.source == TextureSource::User)
            sections.back().entries.push_back(&entry);

    const float child_indent = std::round(m_imgui->scaled(0.8f));
    const Section &built_in = sections.front();
    if (section_header(built_in.key, built_in.title, built_in.entries.size(), 0.f, {}, built_in.title)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int    count  = grid(origin, built_in.entries, false);
        end_grid(origin, count);
        if (count == 0)
            ImGui::TextDisabled("%s", _u8L("No textures found.").c_str());
    }

    size_t subscribed_count = 0;
    for (size_t i = 1; i + 1 < sections.size(); ++i)
        subscribed_count += sections[i].entries.size();

    const std::string subscribed_title = _u8L("Texture libraries");
    if (section_header("subscribed:", subscribed_title, subscribed_count, 0.f, {}, subscribed_title)) {
        for (size_t i = 1; i + 1 < sections.size(); ++i) {
            const Section &pack = sections[i];
            const auto [name, attribution] = split_pack_title(pack.title);
            if (!section_header(pack.key, name, pack.entries.size(), child_indent, attribution, pack.title))
                continue;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const int    count  = grid(origin, pack.entries, false);
            end_grid(origin, count);
        }

        ImGui::Dummy(ImVec2(0.f, spacing));
        const std::string download_label = _u8L("Explore more texture libraries");
        if (ImGui::Button(download_label.c_str(), ImVec2(grid_w, frame_h)))
            download_clicked = true;
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Opens OrcaCloud in the Home page to browse for plugins."));
    }

    const Section &user = sections.back();
    if (section_header(user.key, user.title, user.entries.size(), 0.f, {}, user.title)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int    count  = grid(origin, user.entries, true);
        ImGui::SetCursorScreenPos(tile_pos(origin, count));
        if (ImGui::Button("+##import_texture", ImVec2(tile, tile)))
            import_clicked = true;
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Import your own image as a height map. It is converted to the format the slicer "
                               "bakes from and saved to your personal texture folder, kept separate from the "
                               "textures shipped with OrcaSlicer."));
        end_grid(origin, count + 1);
        // Belongs to this section: it describes the import tile and the remove-on-hover tiles, both of
        // which live here, so it is hidden with them rather than left describing controls off-screen.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + grid_w);
        ImGui::TextDisabled("%s", _u8L("PNG, JPG or BMP - converted to a height map on import. Hover one of yours to remove it.").c_str());
        ImGui::PopTextWrapPos();
    }
    if (loads_deferred)
        m_parent.request_extra_frame();
    // Done whether or not the current texture was drawn: one in a collapsed section stays where it is.
    m_picker_reveal_selected = false;
    ImGui::EndChild();
    pop_slim_scrollbar_style();

    if (chosen || import_clicked || close_clicked || download_clicked)
        ImGui::CloseCurrentPopup();
    m_tooltip_anchor_min = prev_anchor_min;
    m_tooltip_anchor_max = prev_anchor_max;
    ImGui::EndPopup();
    // The OrcaCloud plugin hub filtered to resource textures; the pane's session handoff redirects here.
    if (download_clicked)
        wxGetApp().open_orca_cloud("/app/plugins/plugin-hub?types=resource-textures");

    // Only once the loop over the library is done: importing, picking (which reorders the recently used)
    // and removing all change the library the loop was iterating.
    if (chosen)
        set_layer_texture(*layer, *chosen);
    else if (import_clicked)
        import_custom_texture(*layer);
    else if (to_remove) {
        MessageDialog dlg(nullptr,
                          wxString::Format(_L("Remove \"%s\" from My textures?\n\nThe image file is deleted from your "
                                              "texture folder. Layers already using it keep it."),
                                           from_u8(to_remove->name)),
                          _L("Remove texture"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() == wxID_YES) {
            std::string error;
            if (remove_user_texture(to_remove->path, error))
                m_library_textures.erase(to_remove->path); // its cached thumbnail and bytes go with it
            else
                show_error(nullptr, error);
        }
        // The modal dialog took the focus, which closes the popup; the user was choosing a texture and
        // still is, so bring it back.
        m_picker_open_request = true;
    }
}
bool GLGizmoTextureDisplacement::load_image_adjust_source(const TextureDisplacementLayer &layer)
{
    if (layer.empty())
        return false;

    // The *raw* image: neither the layer's Smoothing nor the panel's own preview_adjust, so what the
    // sliders act on is always the image as it is actually stored rather than the last preview of it.
    // Without clearing preview_adjust here the adjustment would compound on itself every reload.
    TextureDisplacementLayer raw = layer;
    raw.smoothing                = 0.f;
    raw.preview_adjust           = TextureAdjustments{};

    DecodedHeightTexture decoded = decode_height_texture(raw);
    if (decoded.empty())
        return false;

    m_adjust_source         = std::move(decoded);
    m_adjust_preview_source = downscale_height_texture(m_adjust_source, ADJUST_PREVIEW_MAX_PX);
    m_adjust_source_image   = layer.image_data.get();
    // The unadjusted image, so the box is filled now rather than one background pass later. Pairing it
    // with an identity `shown` is what makes update_image_adjust_preview() relaunch for the sliders'
    // current values against the new source instead of deciding it is already showing them.
    m_adjust_preview_shown = TextureAdjustments{};
    m_adjust_preview_tex   = upload_height_thumbnail(m_adjust_preview_source, ADJUST_PREVIEW_MAX_PX);
    m_adjust_preview_valid = m_adjust_preview_tex != nullptr;
    return true;
}

void GLGizmoTextureDisplacement::open_image_adjust(const TextureDisplacementLayer &layer)
{
    if (layer.empty() || layer.slot < 0)
        return;

    close_image_adjust(); // reverts and clears any previous panel

    if (!load_image_adjust_source(layer))
        return;

    m_adjust           = TextureAdjustments{};
    m_adjust_applied   = TextureAdjustments{};
    m_adjust_slot      = layer.slot;
    const ModelVolume *mv = texture_volume();
    m_adjust_volume_id = mv != nullptr ? mv->id().id : 0;
}

void GLGizmoTextureDisplacement::invalidate_adjust_textures()
{
    // The GPU copies of a layer's image - the list thumbnail, the preview shader's height and colour
    // textures - are keyed on the image_data pointer and the layer's Smoothing, and a live adjustment
    // changes neither: the pointer still names the same PNG. Rather than widen three cache keys for a
    // panel that is open a few seconds at a time, the panel drops the copies itself whenever it moves
    // the adjustment. Resetting the source is enough; the next get_layer_*() rebuilds from the decode,
    // which does see the adjustment.
    m_thumbnail_source.fill({});
    m_color_tex_source.reset();
    m_height_tex_source.reset();
}

void GLGizmoTextureDisplacement::close_image_adjust()
{
    // Revert the layer's live adjustment. Every exit that reaches here is a discard: a commit resets
    // preview_adjust itself, before pointing the layer at the file that already carries the edit, so this
    // cannot undo a committed adjustment.
    //
    // Found by the id recorded when the panel opened rather than through texture_volume(), which answers
    // for the *current* selection. One of the ways to get here is the selection having moved on, and
    // reverting the newly selected object's layer instead would both miss the adjustment that is still
    // live on the original volume and clear one the user never touched.
    bool reverted = false;
    if (m_adjust_slot >= 0 && m_adjust_volume_id != 0)
        for (ModelObject *mo : wxGetApp().model().objects) {
            for (ModelVolume *mv : mo->volumes) {
                if (mv->id().id != m_adjust_volume_id)
                    continue;
                for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
                    if (l.slot == m_adjust_slot && !l.preview_adjust.identity()) {
                        l.preview_adjust = TextureAdjustments{};
                        reverted         = true;
                    }
            }
        }

    if (reverted)
        invalidate_adjust_textures();

    // std::async's future blocks here until the pass finishes. That is a few milliseconds on an image
    // this small, and the alternative - a detached thread writing into the members of a closed panel -
    // is worse.
    if (m_adjust_preview_job.valid())
        m_adjust_preview_job.wait();
    m_adjust_preview_job = {};

    m_adjust_slot           = -1;
    m_adjust                = TextureAdjustments{};
    m_adjust_applied        = TextureAdjustments{};
    m_adjust_source         = DecodedHeightTexture{};
    m_adjust_preview_source = DecodedHeightTexture{};
    m_adjust_source_image   = nullptr;
    m_adjust_volume_id      = 0;
    m_adjust_preview_tex.reset();
    m_adjust_preview_valid = false;

    // Only while the gizmo is up: on_shutdown() also comes through here, and rebuilding a preview for a
    // gizmo that is closing would be work for a model nobody is looking at. The reverted layer data is
    // what matters there, and that is already done above.
    if (reverted && m_state == On) {
        rebuild_preview();
        m_parent.set_as_dirty();
    }
}

void GLGizmoTextureDisplacement::update_image_adjust_preview()
{
    if (m_adjust_preview_source.empty())
        return;

    // Collect a finished pass. wait_for(0) rather than get() outright: this runs inside the frame, and
    // blocking on the worker here would hand back to the UI thread the very stall the worker exists to
    // spare it.
    if (m_adjust_preview_job.valid() &&
        m_adjust_preview_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const DecodedHeightTexture adjusted = m_adjust_preview_job.get();
        m_adjust_preview_job                = {};
        if (std::unique_ptr<GLTexture> tex = upload_height_thumbnail(adjusted, ADJUST_PREVIEW_MAX_PX)) {
            m_adjust_preview_tex   = std::move(tex);
            m_adjust_preview_shown = m_adjust_preview_running;
            m_adjust_preview_valid = true;
        }
    }

    // One pass at a time: whatever the sliders did while it ran is picked up by the next launch, so a
    // drag collapses to the value the sliders ended on instead of queuing one pass per frame.
    if (!m_adjust_preview_job.valid() && m_adjust != m_adjust_preview_shown) {
        m_adjust_preview_running = m_adjust;
        // Captured by value: the pass must not read members the UI thread goes on editing while it runs.
        m_adjust_preview_job = std::async(std::launch::async,
                                          [src = m_adjust_preview_source, adj = m_adjust_preview_running] {
                                              return apply_texture_adjustments(src, adj);
                                          });
    }

    // A pass in flight has no other way to get its result on screen: without a redraw request the canvas
    // can sit idle, and the preview would only catch up on the next unrelated frame.
    if (m_adjust_preview_job.valid())
        m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::commit_image_adjust(TextureDisplacementLayer &layer)
{
    if (m_adjust_source.empty() || m_adjust.identity()) {
        // Nothing was changed, so there is nothing to save.
        close_image_adjust();
        return;
    }

    // The full-size pass, once: 13-19 ms grayscale / 40-60 ms colour on a 1024x1024 with every knob
    // engaged. Run on the UI thread deliberately, unlike the live preview: this is a single click, and the
    // rebuild that follows it - a CPU mesh displacement of the painted patch - is already an order of
    // magnitude more work, synchronously, so moving only this off the thread would buy nothing a user
    // could perceive.
    const DecodedHeightTexture adjusted = apply_texture_adjustments(m_adjust_source, m_adjust);

    std::string                              error;
    const std::string                        stem  = layer.name.empty() ? std::string("texture") : layer.name;
    const std::optional<TextureLibraryEntry> entry = save_texture_to_library(adjusted, stem, error);
    if (!entry) {
        show_error(nullptr, error);
        return; // the panel stays open, and the layer keeps the live adjustment: the edit is not lost
    }

    // Cleared *before* the layer is pointed at the new file, and this ordering is the whole correctness of
    // the commit: the saved image already has the adjustment baked into its pixels, so leaving
    // preview_adjust set would apply brightness, contrast and the rest a second time on top of itself.
    layer.preview_adjust = TextureAdjustments{};
    // set_layer_texture() does the rest - the name, the recently-used ordering, a fresh image_data buffer
    // (which is what invalidates every cache keyed by the old pointer), the undo snapshot and the rebuild.
    set_layer_texture(layer, *entry);

    close_image_adjust();
}

void GLGizmoTextureDisplacement::render_image_adjust_window(const ImVec2 &panel_min, const ImVec2 &panel_max)
{
    if (m_adjust_slot < 0)
        return;

    ModelVolume *mv = texture_volume();
    // A different volume is selected than the one this panel was opened on. Slots are numbered per
    // volume, so looking m_adjust_slot up here would find an unrelated layer rather than nothing.
    if (mv == nullptr || mv->id().id != m_adjust_volume_id) {
        close_image_adjust();
        return;
    }

    TextureDisplacementLayer *layer = nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == m_adjust_slot)
            layer = &l;
    if (layer == nullptr) {
        // The layer went away under the panel (removed, or merged away by an undo).
        close_image_adjust();
        return;
    }

    // The layer's image can be swapped from the texture picker while this panel is open - the panel is a
    // window now, so the rest of the gizmo stays usable behind it. Re-read the source when that happens,
    // keeping the slider positions: the user is adjusting this layer, and the 3D view has already moved to
    // the new image (decode_height_texture() reads layer.image_data, which changed), so re-reading here is
    // also what keeps the panel's photo and the model showing the same thing.
    if (layer->image_data.get() != m_adjust_source_image && !load_image_adjust_source(*layer)) {
        close_image_adjust(); // the new image cannot be decoded; nothing left to adjust
        return;
    }

    const float img_w = std::round(m_imgui->scaled(16.f));
    const float gap   = m_imgui->scaled(0.5f);
    // Placed beside the panel on first appearance only, and left wherever the user drags it after that -
    // it is a window, not a popup, so its position is theirs to keep.
    const float win_w = img_w + 2.f * ImGui::GetStyle().WindowPadding.x;
    const bool  left  = panel_min.x - gap - win_w >= 0.f;
    ImGui::SetNextWindowPos(ImVec2(left ? panel_min.x - gap - win_w : panel_max.x + gap,
                                   panel_min.y + m_imgui->scaled(4.f)), ImGuiCond_Appearing);

    bool keep_open = true;
    if (!ImGui::Begin((_u8L("Adjust image") + "##texture_adjust").c_str(), &keep_open,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!keep_open)
            close_image_adjust();
        return;
    }

    // Collect a finished pass and start one for the current slider values before the image is drawn, so
    // the newest available result is what lands on screen this frame.
    update_image_adjust_preview();

    // The photo itself, at the image's own aspect so a non-square texture is not stretched.
    const float aspect = m_adjust_source.height > 0 ? float(m_adjust_source.width) / float(m_adjust_source.height) : 1.f;
    const float img_h  = std::round(img_w / std::max(0.05f, aspect));
    if (m_adjust_preview_valid && m_adjust_preview_tex)
        ImGui::Image((ImTextureID) (intptr_t) m_adjust_preview_tex->get_id(), ImVec2(img_w, img_h));
    else
        ImGui::Dummy(ImVec2(img_w, img_h));

    // The label column is sized off the widest of the five labels, so the tracks line up whatever the
    // translation does to their lengths.
    float label_w = 0.f;
    for (const wxString &l : { _L("Brightness"), _L("Contrast"), _L("Sharpness"), _L("Blur"), _L("Hue") })
        label_w = std::max(label_w, ImGui::CalcTextSize(into_u8(l).c_str()).x);
    label_w += m_imgui->scaled(1.2f);
    // An explicit track width rather than a fill-to-the-right one: this window is AlwaysAutoResize, so its
    // width comes from its content, and an item asking for "whatever is left" would make the width depend
    // on itself. The image above sets the content width, and the sliders match it.
    const float slider_w = std::max(m_imgui->scaled(4.f), img_w - label_w);

    const auto row = [&](const char *id, const wxString &label, float *v, float v_min, float v_max,
                         const char *format, bool enabled, const std::string &tip) {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
        ImGui::SetNextItemWidth(slider_w);
        m_imgui->disabled_begin(!enabled);
        ImGui::SliderFloat(id, v, v_min, v_max, format, ImGuiSliderFlags_AlwaysClamp);
        m_imgui->disabled_end();
        // Only on an enabled row: a disabled item does not report hover, so a tooltip on one would never
        // be shown. What has to be said about a disabled slider is written out below it instead.
        if (enabled && !tip.empty() && ImGui::IsItemHovered())
            m_imgui->tooltip(tip, ImGui::GetFontSize() * 20.f);
    };

    row("##adj_brightness", _L("Brightness"), &m_adjust.brightness, -1.f, 1.f, "%.2f", true, {});
    row("##adj_contrast", _L("Contrast"), &m_adjust.contrast, -1.f, 1.f, "%.2f", true, {});
    row("##adj_sharpness", _L("Sharpness"), &m_adjust.sharpness, 0.f, 1.f, "%.2f", true,
        _u8L("Brings out the finest detail in the image. Applied after Blur, so the two can be used "
             "together: blur away the speckle, then sharpen the edges that remain."));
    row("##adj_blur", _L("Blur"), &m_adjust.blur, 0.f, 1.f, "%.2f", true,
        _u8L("Blurs the image itself. The layer's own Smoothing slider softens the relief without altering "
             "the image, and is the one to reach for first."));
    // Grey has no hue to turn, and the shipped library is grayscale - so on most textures this is a
    // slider that would visibly do nothing. Disabled with the reason given, rather than hidden.
    const bool has_color = m_adjust_source.has_color();
    row("##adj_hue", _L("Hue"), &m_adjust.hue_deg, -180.f, 180.f, "%.0f°", has_color, {});
    if (!has_color) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + img_w);
        ImGui::TextDisabled("%s", _u8L("This texture is grayscale, so it has no hue to shift.").c_str());
        ImGui::PopTextWrapPos();
    }

    if (m_imgui->button(_L("Reset")))
        m_adjust = TextureAdjustments{};

    // Hand the sliders to the layer, which is what puts the adjusted image in front of the displacement:
    // decode_height_texture() applies preview_adjust, so the relief, the colour and the thumbnails all
    // follow from here. Written every frame rather than only on a change, so an undo snapshot or a
    // reselection that resets the layer cannot leave the model showing an adjustment the panel has moved
    // on from.
    layer->preview_adjust = m_adjust;
    if (m_adjust != m_adjust_applied) {
        m_adjust_applied = m_adjust;
        invalidate_adjust_textures();
        // Marked dirty rather than rebuilt here: the panel's own flag is consumed by the main panel's
        // "rebuild once the mouse is released" rule, so dragging a slider recomputes the displacement
        // once, on release, instead of on every frame of the drag. That is what keeps the UI responsive
        // while a full-size resample and a mesh displacement happen behind it.
        m_preview_params_dirty = true;
        // This window is drawn after the block that consumes that flag, so the rebuild happens on a later
        // frame - which means there has to *be* one. Cheap insurance: a slider release that generated no
        // further canvas event would otherwise leave the flag set and the model unchanged until something
        // unrelated redrew.
        m_parent.set_as_dirty();
    }

    ImGui::Separator();
    m_imgui->text_wrapped(_u8L("The model updates when you release a slider. Save as copy writes the "
                               "adjusted image to your texture library and points this layer at it."), img_w);

    const bool cancel = m_imgui->button(_L("Cancel"));
    ImGui::SameLine();
    // Nothing to save until a slider has actually moved.
    m_imgui->disabled_begin(m_adjust.identity());
    const bool as_copy = m_imgui->button(_L("Save as copy"));
    m_imgui->disabled_end();

    ImGui::End();

    if (cancel || !keep_open)
        close_image_adjust();
    else if (as_copy)
        commit_image_adjust(*layer);
}

void GLGizmoTextureDisplacement::remove_texture_layer(int slot)
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Remove texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    auto &layers = mv->texture_displacement_layers;
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                 [slot](const TextureDisplacementLayer &l) { return l.slot == slot; }),
                 layers.end());
    if (slot >= 0 && slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS)) {
        // Every model part: paint left in the slot on another part would still bake, and the next layer
        // added reuses the slot.
        for (ModelVolume *part : texture_parts())
            part->texture_displacement_facet(slot).reset();
        m_thumbnails[size_t(slot)].reset();
        m_thumbnail_source[size_t(slot)].reset();
        m_layer_expanded[size_t(slot)] = false;
    }
    if (m_picker_slot == slot)
        m_picker_slot = -1;

    if (m_active_layer_slot == slot)
        update_from_model_object(false); // also rebuilds the preview
    else
        rebuild_preview(); // a non-active layer's contribution to the combined preview changed

    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::swap_layer_slots(int slot_a, int slot_b)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    ModelVolume *mv = texture_volume();
    const int    n  = int(TEXTURE_DISPLACEMENT_MAX_LAYERS);
    if (mo == nullptr || mv == nullptr || slot_a == slot_b || slot_a < 0 || slot_b < 0 || slot_a >= n || slot_b >= n)
        return;

    // Every model part, not only the texture volume: the selectors paint the active slot on all of them.
    for (ModelVolume *v : mo->volumes) {
        if (!v->is_model_part())
            continue;
        TriangleSelector::TriangleSplittingData data_a = v->texture_displacement_facet(slot_a).get_data();
        TriangleSelector::TriangleSplittingData data_b = v->texture_displacement_facet(slot_b).get_data();
        v->texture_displacement_facet(slot_a).set_data(std::move(data_b));
        v->texture_displacement_facet(slot_b).set_data(std::move(data_a));
    }
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers) {
        if (l.slot == slot_a)
            l.slot = slot_b;
        else if (l.slot == slot_b)
            l.slot = slot_a;
    }

    const size_t a = size_t(slot_a), b = size_t(slot_b);
    std::swap(m_thumbnails[a], m_thumbnails[b]);
    std::swap(m_thumbnail_source[a], m_thumbnail_source[b]);
    std::swap(m_thumbnail_smoothing[a], m_thumbnail_smoothing[b]);
    std::swap(m_layer_expanded[a], m_layer_expanded[b]);
    if (m_active_layer_slot == slot_a)
        m_active_layer_slot = slot_b;
    else if (m_active_layer_slot == slot_b)
        m_active_layer_slot = slot_a;
    if (m_picker_slot == slot_a)
        m_picker_slot = slot_b;
    else if (m_picker_slot == slot_b)
        m_picker_slot = slot_a;
}

void GLGizmoTextureDisplacement::move_texture_layer(int slot, int to_index)
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;

    std::vector<int> slots;
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        slots.push_back(l.slot);
    std::sort(slots.begin(), slots.end());
    const auto it = std::find(slots.begin(), slots.end(), slot);
    if (it == slots.end())
        return;
    const int from = int(it - slots.begin());
    // An insertion index counts the moved layer's own position, which is gone once it is lifted out.
    const int to = std::clamp(to_index > from ? to_index - 1 : to_index, 0, int(slots.size()) - 1);
    if (to == from)
        return;

    update_model_object(); // flush the active layer's strokes into its slot before slots are exchanged
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reorder texture displacement layers"),
                                  UndoRedo::SnapshotType::GizmoAction);
    // Walked one neighbour at a time, so every layer in between shifts by one and keeps its order.
    const int step = to > from ? 1 : -1;
    for (int i = from; i != to; i += step)
        swap_layer_slots(slots[size_t(i)], slots[size_t(i + step)]);

    // The active layer's mask now sits in a different slot, so the selectors are reloaded from there.
    update_from_model_object(false); // also rebuilds the preview
    if (m_adjust_texture_mode)
        update_adjust_anchor();
    m_parent.set_as_dirty();
}

int GLGizmoTextureDisplacement::select_visible_faces(const std::array<float, 12> *uv_clip)
{
    const ModelVolume *owner = texture_volume();
    ModelObject       *mo    = m_c->selection_info()->model_object();
    // Every model part, at its selector's and its raycaster's index (see texture_parts()): what is in
    // view gets painted whichever part it belongs to, and one part can hide another.
    const std::vector<ModelVolume *>         parts      = texture_parts();
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    if (owner == nullptr || mo == nullptr || parts.size() > m_triangle_selectors.size() || parts.size() > raycasters.size())
        return 0;

    const Selection   &selection      = m_parent.get_selection();
    const Transform3d  instance       = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix();
    const Transform3d  owner_to_world = instance * owner->get_matrix();
    const Camera      &camera         = wxGetApp().plater()->get_camera();

    // Pass 1 (cheap): drop back-facing triangles. Under perspective the view direction varies across
    // the model, so it is taken per triangle from the eye to the centroid; under an orthographic
    // camera get_position() is still a point on the view axis, so the same expression stays correct
    // in direction terms for everything actually on screen.
    const Vec3d eye = camera.get_position();
    const bool  ortho = camera.get_type() == Camera::EType::Ortho;
    const Vec3d fwd   = camera.get_dir_forward();

    std::vector<Geometry::Transformation> trafos;
    std::vector<std::vector<Vec3f>>       centroids(parts.size());    // world coords, what get_unobscured_idxs() expects
    std::vector<std::vector<unsigned>>    front_facing(parts.size()); // parallel: which facet each centroid came from
    trafos.reserve(parts.size());
    for (size_t p = 0; p < parts.size(); ++p) {
        const ModelVolume          &part = *parts[p];
        const indexed_triangle_set &its  = part.mesh().its;
        trafos.emplace_back(instance * part.get_matrix());
        const Transform3d &to_world = trafos.back().get_matrix();
        // The projection frame maps the owner's local coordinates (see apply_projection_frame()), so
        // another part's centroids are brought into them before the clip test.
        const Transform3d to_owner = (&part == owner) ? Transform3d::Identity() : Transform3d(owner_to_world.inverse() * to_world);

        // Normals transform by the inverse transpose, not by the matrix itself - with a non-uniform
        // scale the two differ, and using the wrong one flips the facing test on the scaled axes.
        const Matrix3d normal_matrix = to_world.matrix().block<3, 3>(0, 0).inverse().transpose();

        centroids[p].reserve(its.indices.size() / 2);
        front_facing[p].reserve(its.indices.size() / 2);
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const stl_triangle_vertex_indices &tri = its.indices[f];
            const Vec3d a = its.vertices[tri[0]].cast<double>();
            const Vec3d b = its.vertices[tri[1]].cast<double>();
            const Vec3d c = its.vertices[tri[2]].cast<double>();
            const Vec3d n_world = normal_matrix * (b - a).cross(c - a);
            if (n_world.squaredNorm() < 1e-20)
                continue; // degenerate triangle: no meaningful normal, so no meaningful facing test
            const Vec3d centroid_local = (a + b + c) / 3.0;
            const Vec3d centroid_world = to_world * centroid_local;
            const Vec3d view_dir       = ortho ? fwd : Vec3d(centroid_world - eye);
            if (n_world.dot(view_dir) >= 0.0)
                continue; // facing away from the camera

            // Clip to the projection frame before the raycast, not after: outside the frame the texture
            // samples to nothing anyway, so those facets would only be painted to no effect - and this
            // is also what keeps the ray queries proportional to the framed area instead of the model.
            if (uv_clip != nullptr) {
                Vec2f uv;
                if (!project_uv_projective(*uv_clip, (to_owner * centroid_local).cast<float>(), uv))
                    continue; // behind the projector
                if (uv.x() < 0.f || uv.x() > 1.f || uv.y() < 0.f || uv.y() > 1.f)
                    continue;
            }
            centroids[p].emplace_back(centroid_world.cast<float>());
            front_facing[p].push_back(unsigned(f));
        }
    }

    // Pass 2 (the expensive one): a real ray query per surviving centroid, so geometry in front of a
    // front-facing triangle correctly hides it - the far inner wall of a cup is front-facing but not
    // visible. This is why the whole thing is click-driven rather than live. Each raycaster only sees
    // its own part's mesh, so a centroid has to be clear of every one of them; its own part goes first,
    // since that is what hides most of them.
    size_t visible = 0;
    {
        wxBusyCursor wait;
        const ClippingPlane *clipping_plane = m_c->object_clipper()->get_clipping_plane();
        for (size_t p = 0; p < parts.size(); ++p) {
            std::vector<Vec3f>    &points = centroids[p];
            std::vector<unsigned> &facets = front_facing[p];
            const auto keep_unobscured_by = [&](size_t q) {
                const std::vector<unsigned> clear = raycasters[q]->get_unobscured_idxs(trafos[q], camera, points, clipping_plane);
                std::vector<Vec3f>    kept_points;
                std::vector<unsigned> kept_facets;
                kept_points.reserve(clear.size());
                kept_facets.reserve(clear.size());
                for (unsigned i : clear) {
                    kept_points.push_back(points[i]);
                    kept_facets.push_back(facets[i]);
                }
                points = std::move(kept_points);
                facets = std::move(kept_facets);
            };
            if (!points.empty())
                keep_unobscured_by(p);
            for (size_t q = 0; q < parts.size() && !points.empty(); ++q)
                if (q != p)
                    keep_unobscured_by(q);
            visible += facets.size();
        }
    }
    if (visible == 0)
        return 0;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Select visible faces for texture displacement"),
                                  UndoRedo::SnapshotType::GizmoAction);

    // Replaces the layer's paint rather than adding to it, on every part including those nothing was
    // visible on: the checkbox means "project onto what I can see", so a second capture from a new
    // angle should not leave the previous angle painted.
    // TriangleSelectorGUI, not the TriangleSelector base: request_update_render_data() is declared on
    // the GUI subclass, so binding to the base here would drop it.
    for (size_t p = 0; p < parts.size(); ++p) {
        TriangleSelectorGUI &selector = *m_triangle_selectors[p];
        selector.reset();
        for (unsigned f : front_facing[p])
            selector.set_facet(int(f), EnforcerBlockerType::ENFORCER);
        selector.request_update_render_data(true);
    }

    update_model_object();
    m_parent.set_as_dirty();
    return int(visible);
}

void GLGizmoTextureDisplacement::select_whole_model()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Select whole model for texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    int idx = -1;
    for (const ModelVolume *v : mo->volumes) {
        if (!v->is_model_part())
            continue;
        ++idx;
        const size_t facet_count = v->mesh().its.indices.size();
        for (size_t i = 0; i < facet_count; ++i)
            m_triangle_selectors[idx]->set_facet(int(i), EnforcerBlockerType::ENFORCER);
        m_triangle_selectors[idx]->request_update_render_data(true);
    }
    update_model_object();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::subdivide_model()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    // Every model part, like the remesh: this is a plain mesh operation on the object, and a part left
    // coarse is a part no height map can raise relief on.
    const std::vector<ModelVolume *> parts = texture_parts();
    if (mo == nullptr || parts.empty() || m_subdivide_count < 1)
        return; // 0 passes means "no subdivision" - don't take a snapshot for a no-op

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Subdivide model for texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    for (ModelVolume *mv : parts) {
        // Same save/replace/restore-painting dance GLGizmoSimplify uses when it re-tessellates a
        // volume's mesh: supported/seam/mmu/fuzzy-skin masks get remapped onto the new triangles,
        // texture-displacement doesn't (no remap support for it yet) and is dropped instead of being
        // left referring to triangle indices that no longer mean the same thing.
        std::optional<TriangleSelector::SavedPainting> saved_painting = mv->save_painting();

        // max_edge_length 0 means "no triangle is ever small enough", so every non-degenerate edge is
        // split on each of the m_subdivide_count passes - i.e. a plain "subdivide the whole mesh N times".
        TriangleMesh new_mesh(subdivide_mesh_uniform(mv->mesh().its, 0.f, m_subdivide_count));
        mv->set_mesh(std::move(new_mesh));
        mv->set_new_unique_id();
        mv->calculate_convex_hull();
        mv->restore_painting(saved_painting);
    }

    if (ObjectList *obj_list = wxGetApp().obj_list()) {
        const ModelObjectPtrs &objs = plater->model().objects;
        auto it = std::find(objs.begin(), objs.end(), mo);
        if (it != objs.end())
            obj_list->update_info_items(size_t(it - objs.begin()));
    }

    plater->changed_object(*mo);
    update_from_model_object(false); // reload selectors/preview against the new mesh + cleared paint
    m_parent.set_as_dirty();
}

bool GLGizmoTextureDisplacement::collect_paint_region(
    const TriangleMesh &mesh, const TextureDisplacementFacetsData &facets,
    std::vector<uint8_t> &region,
    std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> *paint)
{
    const indexed_triangle_set &its  = mesh.its;
    const size_t                ntri = its.indices.size();
    const size_t                nvert = its.vertices.size();

    region.assign(ntri, 0);
    if (paint)
        for (LayerPaintMap &pm : *paint)
            pm = LayerPaintMap{};

    // Twice the area of each source triangle, for the "is this one covered edge to edge" test below.
    // Only the paint carry-forward needs it, and the live subdivide preview calls this on every
    // slider frame, so it is not built for the region-only path.
    const auto tri_area2 = [](const Vec3f &a, const Vec3f &b, const Vec3f &c) {
        return (b - a).cross(c - a).norm();
    };
    std::vector<float> source_area2;
    if (paint) {
        source_area2.resize(ntri);
        for (size_t i = 0; i < ntri; ++i)
            source_area2[i] = tri_area2(its.vertices[size_t(its.indices[i][0])],
                                        its.vertices[size_t(its.indices[i][1])],
                                        its.vertices[size_t(its.indices[i][2])]);
    }

    bool any_paint = false;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
        const TriangleSelector::TriangleSplittingData &data = facets[size_t(slot)];
        if (!TriangleSelector::has_facets(data, EnforcerBlockerType::ENFORCER))
            continue;

        // The refine region is exactly the original triangles the brush touched. `triangles_to_split`
        // lists precisely those: serialize() records an entry for every original triangle that is
        // either split (i.e. partially painted, which is the patch boundary) or carries a non-default
        // state (fully painted). No dilation - an earlier version marked every triangle sharing a
        // *vertex* with the patch, which on a coarse model pulls in a whole fan of huge unpainted
        // neighbours and then refines them to the resolution floor, since the height field the detail
        // test samples is not restricted to the painted area. The conformal closure inside
        // subdivide_mesh_adaptive() already grades the size change outward on its own.
        for (const TriangleSelector::TriangleBitStreamMapping &m : data.triangles_to_split)
            if (size_t(m.triangle_idx) < ntri)
                region[m.triangle_idx] |= REFINE_PAINTED;

        if (paint) {
            TriangleSelector sel(mesh);
            sel.deserialize(data, false);
            // get_facets_strict() now reports which source triangle each sub-triangle came from, which
            // is what lets partial coverage be carried forward geometrically instead of being rounded
            // away. Note a *fully* painted source can still come back as several sub-triangles (T-joint
            // splits forced by a refined neighbour), so "wholly painted" is an area test, not a
            // one-piece test.
            std::vector<int>           src;
            const indexed_triangle_set patch = sel.get_facets_strict(EnforcerBlockerType::ENFORCER, &src);

            LayerPaintMap &pm = (*paint)[slot];
            pm.full.assign(ntri, 0);
            pm.part_start.assign(ntri + 1, 0);

            std::vector<float> covered2(ntri, 0.f);
            for (size_t j = 0; j < patch.indices.size() && j < src.size(); ++j) {
                if (size_t(src[j]) >= ntri)
                    continue;
                const stl_triangle_vertex_indices &t = patch.indices[j];
                covered2[size_t(src[j])] += tri_area2(patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                                      patch.vertices[size_t(t[2])]);
            }
            for (size_t i = 0; i < ntri; ++i)
                pm.full[i] = (source_area2[i] > 0.f && covered2[i] >= 0.999f * source_area2[i]) ? 1 : 0;

            // Only partly covered sources need their pieces kept - a full one answers every query with
            // "painted", and an untouched one with "not painted".
            for (size_t j = 0; j < patch.indices.size() && j < src.size(); ++j)
                if (size_t(src[j]) < ntri && !pm.full[size_t(src[j])])
                    ++pm.part_start[size_t(src[j]) + 1];
            for (size_t i = 0; i < ntri; ++i)
                pm.part_start[i + 1] += pm.part_start[i];
            pm.part.resize(size_t(pm.part_start[ntri]));
            {
                std::vector<int> fill(pm.part_start.begin(), pm.part_start.begin() + ntri);
                for (size_t j = 0; j < patch.indices.size() && j < src.size(); ++j) {
                    const size_t S = size_t(src[j]);
                    if (S >= ntri || pm.full[S])
                        continue;
                    const stl_triangle_vertex_indices &t = patch.indices[j];
                    pm.part[size_t(fill[S]++)] = { patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                                   patch.vertices[size_t(t[2])] };
                }
            }
        }
        any_paint = true;
    }
    if (!any_paint)
        return false;

    // The band straddling the paint's edge. The bake steps the surface from full displacement to zero
    // across it, and nothing else in the refinement criteria can see that step: the chord-error
    // sampler has no per-point paint test, so just outside the paint it keeps reporting the same
    // smooth height field and reports no error at all. Left alone, the transition therefore stays at
    // whatever density the input had - which is what makes the rim of an unpainted island a ring of
    // big, steeply tilted triangles.
    //
    // Seeded from the vertices shared by a painted and an unpainted triangle (the actual edge of the
    // paint) and grown outward over vertex adjacency, so the band covers both sides of the step.
    if (BORDER_BAND_RINGS > 0) {
        // Vertex -> incident triangles, CSR-style (counted, prefix-summed, filled). This runs on every
        // frame of the subdivide preview's sliders, so it must not allocate a small vector per vertex.
        std::vector<int> vstart(nvert + 1, 0);
        for (size_t i = 0; i < ntri; ++i)
            for (int k = 0; k < 3; ++k)
                if (size_t(its.indices[i][k]) < nvert)
                    ++vstart[size_t(its.indices[i][k]) + 1];
        for (size_t v = 0; v < nvert; ++v)
            vstart[v + 1] += vstart[v];
        // static_cast, not size_t(...): the latter parses as a parameter declaration (see the note
        // above the identical prefix sum on `part`).
        std::vector<int> vtri(static_cast<size_t>(vstart[nvert]), 0);
        {
            std::vector<int> fill(vstart.begin(), vstart.begin() + nvert);
            for (size_t i = 0; i < ntri; ++i)
                for (int k = 0; k < 3; ++k)
                    if (size_t(its.indices[i][k]) < nvert)
                        vtri[size_t(fill[size_t(its.indices[i][k])]++)] = int(i);
        }

        // A vertex used by both a painted and an unpainted triangle sits exactly on the paint's edge.
        std::vector<uint8_t> ring_vertex(nvert, 0);
        for (size_t v = 0; v < nvert; ++v) {
            bool painted = false, unpainted = false;
            for (int k = vstart[v]; k < vstart[v + 1]; ++k)
                ((region[size_t(vtri[size_t(k)])] & REFINE_PAINTED) ? painted : unpainted) = true;
            ring_vertex[v] = (painted && unpainted) ? 1 : 0;
        }

        for (int ring = 0; ring < BORDER_BAND_RINGS; ++ring) {
            std::vector<uint8_t> next = ring_vertex;
            for (size_t v = 0; v < nvert; ++v) {
                if (!ring_vertex[v])
                    continue;
                for (int k = vstart[v]; k < vstart[v + 1]; ++k) {
                    const size_t t = size_t(vtri[size_t(k)]);
                    region[t] |= REFINE_BORDER;
                    // Grow through this triangle's other corners, for the following ring.
                    for (int c = 0; c < 3; ++c)
                        if (size_t(its.indices[t][c]) < nvert)
                            next[size_t(its.indices[t][c])] = 1;
                }
            }
            ring_vertex.swap(next);
        }
    }
    return true;
}

TextureDisplacementFacetsData GLGizmoTextureDisplacement::masks_after_subdivision(
    const TriangleMesh &new_mesh, const std::vector<int> &source,
    const std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> &paint)
{
    // Children inherit their parent's source triangle, and subdivision only ever adds edge midpoints,
    // so every new triangle lies inside its source and on the same surface - which means the source's
    // painted *pieces* can be queried directly by point containment. That is what keeps a brush outline
    // smooth: rounding each source to wholly painted or not instead leaves a ragged fringe of isolated
    // triangles along any curved boundary, and the refined mesh then reproduces that fringe exactly
    // rather than hiding it.
    TextureDisplacementFacetsData       out{};
    const indexed_triangle_set         &its = new_mesh.its;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
        const LayerPaintMap &pm = paint[size_t(slot)];
        if (pm.empty())
            continue;
        TriangleSelector sel(new_mesh);
        for (size_t i = 0; i < source.size() && i < its.indices.size(); ++i) {
            const size_t S = size_t(source[i]);
            if (S >= pm.full.size())
                continue;
            bool painted = pm.full[S] != 0;
            if (!painted && pm.part_start[S] != pm.part_start[S + 1]) {
                const stl_triangle_vertex_indices &t = its.indices[i];
                const Vec3f centroid = (its.vertices[size_t(t[0])] + its.vertices[size_t(t[1])] +
                                        its.vertices[size_t(t[2])]) / 3.f;
                for (int k = pm.part_start[S]; k < pm.part_start[S + 1] && !painted; ++k)
                    painted = point_in_triangle_coplanar(centroid, pm.part[size_t(k)]);
            }
            if (painted)
                sel.set_facet(int(i), EnforcerBlockerType::ENFORCER);
        }
        out[size_t(slot)] = sel.serialize();
    }
    return out;
}

double GLGizmoTextureDisplacement::painted_area_mm2(const ModelVolume &mv)
{
    // Keyed on the paint generation, which every stroke and every layer edit raises, so a panel that
    // asks for this on every frame walks the mesh only when the answer can have changed.
    const std::string key = std::to_string(mv.id().id) + ":" + std::to_string(mv.mesh().facets_count()) + ":" +
                            std::to_string(m_preview_generation->load());
    if (key == m_painted_area_key)
        return m_painted_area_mm2;

    const TriangleMesh &mesh = mv.mesh();
    std::vector<uint8_t> region;
    double               area = 0.;
    if (collect_paint_region(mesh, facets_data_of(mv), region, nullptr)) {
        // In the frame the bake refines in, so a scaled instance is measured at the size it prints at.
        const Transform3d   frame = texture_displacement_bake_frame(texture_displacement_volume_to_world(mv));
        const indexed_triangle_set &its = mesh.its;
        for (size_t t = 0; t < its.indices.size() && t < region.size(); ++t) {
            if ((region[t] & REFINE_PAINTED) == 0)
                continue;
            const stl_triangle_vertex_indices &tri = its.indices[t];
            const Vec3d a = frame * its.vertices[size_t(tri[0])].cast<double>();
            const Vec3d b = frame * its.vertices[size_t(tri[1])].cast<double>();
            const Vec3d c = frame * its.vertices[size_t(tri[2])].cast<double>();
            area += 0.5 * (b - a).cross(c - a).norm();
        }
    }
    m_painted_area_mm2 = area;
    m_painted_area_key = key;
    return area;
}

size_t GLGizmoTextureDisplacement::estimated_refined_triangles(const ModelVolume &mv, float edge_mm)
{
    if (edge_mm <= 0.f)
        return 0;
    return size_t(4.0 * painted_area_mm2(mv) / (double(edge_mm) * double(edge_mm)));
}

const V2Resolution &GLGizmoTextureDisplacement::v2_recommendation(const ModelVolume &mv)
{
    // Rebuilt only when something it depends on changes: the surface area scan is O(triangles) and the
    // panel asks for this every frame.
    std::string key = std::to_string(mv.id().id) + ":" + std::to_string(mv.mesh().facets_count());
    for (const TextureDisplacementLayer &l : mv.texture_displacement_layers)
        key += Slic3r::format("|%1%:%2%:%3%:%4%", l.slot, reinterpret_cast<uintptr_t>(l.image_data.get()), l.tiling_scale, l.depth_mm);
    const Transform3d trafo = texture_displacement_volume_to_world(mv);
    for (int i = 0; i < 12; ++i)
        key += Slic3r::format(",%1%", trafo.matrix()(i / 4, i % 4));
    if (key != m_v2_rec_key) {
        m_v2_rec     = recommend_v2_resolution(mv.mesh().its, mv.texture_displacement_layers, trafo);
        m_v2_rec_key = std::move(key);
    }
    return m_v2_rec;
}

TextureDisplacementFacetsData GLGizmoTextureDisplacement::facets_data_of(const ModelVolume &mv)
{
    TextureDisplacementFacetsData out{};
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        out[size_t(i)] = mv.texture_displacement_facet(i).get_data();
    return out;
}

bool GLGizmoTextureDisplacement::any_layer_colors(const ModelVolume &mv)
{
    for (const TextureDisplacementLayer &layer : mv.texture_displacement_layers)
        if (layer.color_enabled && !layer.empty() && height_texture_has_color(layer))
            return true;
    return false;
}

TextureColorSettings GLGizmoTextureDisplacement::color_settings_for(const ModelVolume &mv)
{
    TextureColorSettings out;
    if (!any_layer_colors(mv))
        return out; // nothing is colouring: every colour path stays switched off
    out.palette          = cached_palette();
    out.palette_pure     = make_palette(m_palette_filaments, {});
    out.despeckle_passes = mv.texture_displacement_options.color_despeckle;
    return out;
}

const std::vector<GLGizmoTextureDisplacement::PaletteEntry> &GLGizmoTextureDisplacement::cached_palette()
{
    // Rebuilt only when something it depends on actually changes. The shaded preview rebuilds on every
    // paint stroke and the panel asks every frame, while ranking the mixes and filling the quantizer's
    // lookup cube each take tens of milliseconds - paying that per stroke is the difference between
    // painting that keeps up and painting that stutters.
    //
    // Two levels. The ranking depends only on the filaments and the images, so dragging the count, or a
    // bake creating slots, re-picks from it without ranking again; the palette and its quantizers depend
    // on that pick as well.
    const ModelVolume     *mv        = texture_volume();
    const bool             mixing    = mv != nullptr && mv->texture_displacement_options.color_mix_enabled;
    const int              mix_count = mv != nullptr ? std::max(0, mv->texture_displacement_options.color_mix_count) : 0;
    std::vector<ColorRGBA> filaments = filament_palette();
    // The images themselves rather than their addresses, so a freed and reallocated image can never
    // pass for the old one. Whether one has colour, and whether its colours are flat, follows from it.
    std::vector<std::shared_ptr<std::vector<unsigned char>>> images;
    if (mv != nullptr)
        for (const TextureDisplacementLayer &layer : mv->texture_displacement_layers)
            if (layer.color_enabled && !layer.empty())
                images.push_back(layer.image_data);

    const bool ranking_stale = filaments != m_palette_filaments || images != m_palette_images;
    if (ranking_stale) {
        m_palette_filaments = std::move(filaments);
        m_palette_images    = std::move(images);
        m_mix_ranking.reset();
    }
    if (mixing && !m_mix_ranking)
        m_mix_ranking = rank_mixes(m_palette_filaments, mix_targets(mv->texture_displacement_layers),
                                   int(EnforcerBlockerType::ExtruderMax) - int(m_palette_filaments.size()));

    // Which mixes the project can still print: those it has a fixed slot for, plus as many new ones as
    // there are free slots.
    const PresetBundle            &bundle     = *wxGetApp().preset_bundle;
    const int                      free_slots = std::max(0, int(EnforcerBlockerType::ExtruderMax) - int(bundle.filament_presets.size()));
    const std::vector<std::string> slots      = mixed_slot_signature(bundle.project_config);
    if (ranking_stale || m_palette_cache.empty() || mixing != m_palette_mixing || mix_count != m_palette_mix_count ||
        free_slots != m_palette_free_slots || slots != m_palette_slots) {
        m_palette_mixing     = mixing;
        m_palette_mix_count  = mix_count;
        m_palette_free_slots = free_slots;
        m_palette_slots      = slots;
        std::vector<PaletteEntry> mixes;
        if (mixing && m_mix_ranking)
            mixes = pick_mixes(*m_mix_ranking, mix_count, free_slots, [&bundle](const PaletteEntry &e) {
                return find_fixed_mixed_filament(bundle.project_config, {unsigned(e.a + 1), unsigned(e.b + 1)},
                                                 {e.a_percent(), 100 - e.a_percent()}) >= 0;
            });
        std::vector<PaletteEntry> palette = make_palette(m_palette_filaments, mixes);
        // The previews keep what they were drawn with, so a palette that really changed under them -
        // a filament or a mixed slot edited in the sidebar - has to send them round again.
        m_palette_changed        = m_palette_changed || (!m_palette_cache.empty() && !same_palette(palette, m_palette_cache));
        m_palette_cache          = std::move(palette);
        m_palette_quantizer      = nullptr;
        m_palette_pure_quantizer = nullptr;
    }
    return m_palette_cache;
}

std::pair<ColorQuantizeFn, ColorQuantizeFn> GLGizmoTextureDisplacement::palette_quantizers()
{
    cached_palette();
    if (!m_palette_quantizer) {
        m_palette_quantizer      = make_palette_quantizer(m_palette_cache);
        const bool has_mixes     = m_palette_cache.size() > m_palette_filaments.size();
        m_palette_pure_quantizer = has_mixes ? make_palette_quantizer(make_palette(m_palette_filaments, {})) : m_palette_quantizer;
    }
    return { m_palette_quantizer, m_palette_pure_quantizer };
}

std::vector<ColorRGBA> GLGizmoTextureDisplacement::filament_palette()
{
    std::vector<ColorRGBA> all = wxGetApp().plater()->get_extruders_colors();

    // Physical filaments only. A mix bakes into a mixed filament slot of its own, and those slots are
    // extruders too - mixing them again would hand a blend components naming a virtual slot, where it
    // can only name physical ones ("Mixed filament has invalid or mismatched components"). Mixed slots
    // are kept after the physical ones, so filament i here is extruder i.
    const auto *is_mixed = wxGetApp().preset_bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
    std::vector<ColorRGBA> palette;
    palette.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i)
        if (is_mixed == nullptr || i >= is_mixed->values.size() || !is_mixed->values[i])
            palette.push_back(all[i]);

    // A paint mask can only name so many states, and every mix spends one beside these.
    if (palette.size() > size_t(EnforcerBlockerType::ExtruderMax))
        palette.resize(size_t(EnforcerBlockerType::ExtruderMax));
    return palette;
}

std::vector<GLGizmoTextureDisplacement::MixTarget> GLGizmoTextureDisplacement::mix_targets(
    const std::vector<TextureDisplacementLayer> &layers)
{
    // Each layer's bins, weighted by their share of that layer's pixels.
    std::vector<std::vector<MixTarget>> per_layer;
    for (const TextureDisplacementLayer &layer : layers) {
        if (!layer.color_enabled || layer.empty() || analyze_texture_detail(layer).flat_colors)
            continue;
        TextureDisplacementLayer raw = layer;
        raw.smoothing                = 0.f;
        const DecodedHeightTexture tex = decode_height_texture(raw);
        if (!tex.has_color())
            continue;

        // 16 levels per channel, each bin keeping the mean of the colours that fell in it: a coarse grid
        // to gather on, without snapping every colour to a bin corner.
        struct Bin
        {
            double   r = 0., g = 0., b = 0.;
            uint32_t n = 0;
        };
        std::vector<Bin> bins(size_t(16 * 16 * 16));
        // Past the budget, one pixel from each run of `stride`, at an offset jittered by a fixed-seed
        // generator. A fixed step would sample a lattice that a striped texture can line up with, so
        // that only one of its colours is ever seen; jittered, stripes of any period or orientation are
        // sampled in proportion, and the same image still always gives the same targets. The offset
        // comes from the generator's high bits: a power-of-two LCG's low bits repeat every 2, 4, 8...
        // steps, and the stride is a power of two for exactly the images large enough to need this.
        const size_t npx     = size_t(tex.width) * size_t(tex.height);
        const size_t stride  = std::max<size_t>(1, npx / MIX_TARGET_SAMPLES);
        uint64_t     state   = 0x9E3779B97F4A7C15ull;
        size_t       sampled = 0;
        for (size_t start = 0; start < npx; start += stride) {
            state          = state * 6364136223846793005ull + 1442695040888963407ull;
            const size_t i = start + size_t((uint64_t(uint32_t(state >> 32)) * uint64_t(stride)) >> 32);
            if (i >= npx)
                break;
            const uint8_t *px  = &tex.rgb[i * 3];
            Bin           &bin = bins[size_t(px[0] >> 4) * 256 + size_t(px[1] >> 4) * 16 + size_t(px[2] >> 4)];
            bin.r += px[0];
            bin.g += px[1];
            bin.b += px[2];
            ++bin.n;
            ++sampled;
        }
        std::vector<MixTarget> targets;
        for (const Bin &bin : bins)
            if (bin.n > 0) {
                const double inv = 1. / (255. * double(bin.n));
                targets.push_back({ srgb_to_lab(Vec3f(float(bin.r * inv), float(bin.g * inv), float(bin.b * inv))),
                                    float(double(bin.n) / double(sampled)) });
            }
        per_layer.push_back(std::move(targets));
    }

    // The layers weigh the same and together 1, settled before the pruning below: what rank_mixes() sums
    // over the kept bins is then a mean over every pixel of the colouring layers, with the pixels of a
    // dropped bin counted as no better off.
    std::vector<MixTarget> out;
    for (std::vector<MixTarget> &targets : per_layer)
        for (MixTarget &t : targets) {
            t.weight /= float(per_layer.size());
            out.push_back(t);
        }
    if (out.size() > MIX_TARGET_BINS) {
        std::partial_sort(out.begin(), out.begin() + MIX_TARGET_BINS, out.end(),
                          [](const MixTarget &l, const MixTarget &r) { return l.weight > r.weight; });
        out.resize(MIX_TARGET_BINS);
    }
    return out;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::rank_mixes(
    const std::vector<ColorRGBA> &filaments, const std::vector<MixTarget> &targets, int limit)
{
    const int n = int(filaments.size());
    if (n < 2 || targets.empty() || limit <= 0)
        return {};

    // Every pair at every short-cycle ratio, coloured as its slot will be.
    std::vector<std::string> hex(filaments.size());
    for (int i = 0; i < n; ++i)
        hex[size_t(i)] = encode_color(filaments[size_t(i)]);
    std::vector<PaletteEntry> candidates;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int den = 2; den <= 6; ++den)
                for (int num = 1; num < den; ++num) {
                    if (std::gcd(num, den) != 1)
                        continue; // 2/4 is 1/2, already there
                    PaletteEntry e{ Vec3f::Zero(), i, j, num, den };
                    ColorRGB     blended;
                    if (!decode_color(blend_color_multi({ hex[size_t(i)], hex[size_t(j)] },
                                                        { e.a_percent(), 100 - e.a_percent() }),
                                      blended))
                        continue;
                    e.rgb = Vec3f(blended.r(), blended.g(), blended.b());
                    candidates.push_back(e);
                }

    // How far each target is from the nearest single filament, and from every candidate.
    const size_t       nt = targets.size(), nc = candidates.size();
    std::vector<Vec3f> filament_lab(size_t(n), Vec3f::Zero());
    for (int i = 0; i < n; ++i)
        filament_lab[size_t(i)] = srgb_to_lab(Vec3f(filaments[size_t(i)].r(), filaments[size_t(i)].g(), filaments[size_t(i)].b()));
    const auto de = [](const Vec3f &l, const Vec3f &r) { return DeltaE00(l.x(), l.y(), l.z(), r.x(), r.y(), r.z()); };
    std::vector<float> pure_d(nt, std::numeric_limits<float>::max());
    for (size_t t = 0; t < nt; ++t)
        for (const Vec3f &lab : filament_lab)
            pure_d[t] = std::min(pure_d[t], de(targets[t].lab, lab));
    std::vector<float> dist(nc * nt);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nc), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t c = range.begin(); c < range.end(); ++c) {
            const Vec3f lab = srgb_to_lab(candidates[c].rgb);
            for (size_t t = 0; t < nt; ++t)
                dist[c * nt + t] = de(targets[t].lab, lab);
        }
    });

    // Greedy: each round takes the candidate that lowers the weighted error the most. A candidate only
    // counts where it beats the single filament by PREFER_PURE_DE, since everywhere else the quantizer
    // picks the filament anyway.
    std::vector<float>        current = pure_d;
    std::vector<char>         taken(nc, 0);
    std::vector<PaletteEntry> out;
    const auto                counts = [&](size_t c, size_t t) {
        const float d = dist[c * nt + t];
        return d < current[t] && d <= pure_d[t] - PREFER_PURE_DE;
    };
    while (int(out.size()) < limit) {
        size_t best      = nc;
        double best_gain = 0.;
        for (size_t c = 0; c < nc; ++c) {
            if (taken[c])
                continue;
            double gain = 0.;
            for (size_t t = 0; t < nt; ++t)
                if (counts(c, t))
                    gain += double(targets[t].weight) * double(current[t] - dist[c * nt + t]);
            if (gain > best_gain) {
                best_gain = gain;
                best      = c;
            }
        }
        if (best == nc || best_gain < double(MIN_MIX_GAIN))
            break;
        taken[best] = 1;
        out.push_back(candidates[best]);
        for (size_t t = 0; t < nt; ++t)
            if (counts(best, t))
                current[t] = dist[best * nt + t];
    }
    return out;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::pick_mixes(
    const std::vector<PaletteEntry> &ranking, int count, int free_slots, const std::function<bool(const PaletteEntry &)> &reusable)
{
    std::vector<PaletteEntry> out;
    for (const PaletteEntry &e : ranking) {
        if (int(out.size()) >= count)
            break;
        // Out of free slots, a later mix that already has one still fits.
        if (reusable && reusable(e)) {
            out.push_back(e);
        } else if (free_slots > 0) {
            out.push_back(e);
            --free_slots;
        }
    }
    return out;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::make_palette(
    const std::vector<ColorRGBA> &filaments, const std::vector<PaletteEntry> &mixes)
{
    std::vector<PaletteEntry> out;
    out.reserve(filaments.size() + mixes.size());
    for (int i = 0; i < int(filaments.size()); ++i)
        out.push_back({ Vec3f(filaments[size_t(i)].r(), filaments[size_t(i)].g(), filaments[size_t(i)].b()), i, i, 1, 1 });
    out.insert(out.end(), mixes.begin(), mixes.end());
    return out;
}

std::vector<int> GLGizmoTextureDisplacement::palette_filaments(const std::vector<PaletteEntry>                &palette,
                                                               const std::vector<uint8_t>                     &triangle_color,
                                                               const std::function<int(const PaletteEntry &)> &slot_for_mix)
{
    std::vector<char> used(palette.size(), 0);
    for (const uint8_t v : triangle_color)
        if (v > 0 && size_t(v) <= palette.size())
            used[size_t(v) - 1] = 1;

    std::vector<int> out(palette.size(), -1);
    for (size_t i = 0; i < palette.size(); ++i) {
        const PaletteEntry &e = palette[i];
        if (!e.is_mix()) {
            out[i] = e.a;
        } else if (used[i]) {
            const int slot = slot_for_mix ? slot_for_mix(e) : -1;
            // No room for another slot: the component that dominates the blend is the nearest the print
            // can come.
            out[i] = slot >= 0 ? slot : (e.num * 2 >= e.den ? e.a : e.b);
        }
    }
    return out;
}

ColorQuantizeFn GLGizmoTextureDisplacement::make_palette_quantizer(const std::vector<PaletteEntry> &palette)
{
    if (palette.empty())
        return nullptr;

    // Lab once per entry, not once per lookup.
    struct Lab { float l, a, b; };
    std::vector<Lab> palette_lab(palette.size());
    for (size_t i = 0; i < palette.size(); ++i) {
        const Vec3f lab = srgb_to_lab(palette[i].rgb);
        palette_lab[i]  = { lab.x(), lab.y(), lab.z() };
    }

    constexpr int E   = PALETTE_LUT_EDGE;
    auto          lut = std::make_shared<std::vector<uint8_t>>(size_t(E) * E * E, 0);
    tbb::parallel_for(tbb::blocked_range<int>(0, E), [&](const tbb::blocked_range<int> &range) {
        for (int r = range.begin(); r < range.end(); ++r)
            for (int g = 0; g < E; ++g)
                for (int b = 0; b < E; ++b) {
                    // Cell centre, so the quantization error is symmetric across the cell.
                    float l0, a0, b0;
                    const Vec3f lab0 = srgb_to_lab(Vec3f((r + 0.5f) / E, (g + 0.5f) / E, (b + 0.5f) / E));
                    l0 = lab0.x(); a0 = lab0.y(); b0 = lab0.z();
                    int   best  = 0, best_pure = -1;
                    float best_d = std::numeric_limits<float>::max(), best_pure_d = best_d;
                    for (size_t i = 0; i < palette_lab.size(); ++i) {
                        const float d = DeltaE00(l0, a0, b0, palette_lab[i].l, palette_lab[i].a, palette_lab[i].b);
                        if (d < best_d) {
                            best_d = d;
                            best   = int(i);
                        }
                        if (!palette[i].is_mix() && d < best_pure_d) {
                            best_pure_d = d;
                            best_pure   = int(i);
                        }
                    }
                    // A mix only where it clearly beats the nearest single filament, see PREFER_PURE_DE.
                    if (best_pure >= 0 && palette[size_t(best)].is_mix() && best_pure_d - best_d < PREFER_PURE_DE)
                        best = best_pure;
                    (*lut)[(size_t(r) * E + size_t(g)) * E + size_t(b)] = uint8_t(best);
                }
    });

    return [lut](const Vec3f &rgb) -> int {
        constexpr int E = PALETTE_LUT_EDGE;
        const int r = std::clamp(int(rgb.x() * E), 0, E - 1);
        const int g = std::clamp(int(rgb.y() * E), 0, E - 1);
        const int b = std::clamp(int(rgb.z() * E), 0, E - 1);
        return int((*lut)[(size_t(r) * E + size_t(g)) * E + size_t(b)]);
    };
}

TextureDisplacementPrepareResult GLGizmoTextureDisplacement::prepare_mesh(
    const indexed_triangle_set &base, const TextureDisplacementFacetsData &masks,
    const std::vector<TextureDisplacementLayer> &layers, const std::vector<Vec2f> &model_uv,
    const Transform3d &texture_frame, const TextureDisplacementPrepareParams &params,
    const TextureColorSettings &color_settings, const DisplacementProgressFn &progress,
    BakeStageRecorder *debug)
{
    TextureDisplacementPrepareResult out;
    const auto                       report = [&progress](int pct) { return !progress || progress(pct); };

    // The samplers are built over a copy of the current mesh moved into `texture_frame`, and queried
    // with points and normals mapped across, so the stages below go on measuring the mesh itself.
    const bool           framed = !texture_frame.matrix().isApprox(Transform3d::Identity().matrix());
    const Matrix3d       frame_normal = texture_frame.linear().inverse().transpose();
    indexed_triangle_set framed_mesh;
    const auto           mesh_to_sample = [&](const indexed_triangle_set &its) -> const indexed_triangle_set & {
        if (!framed)
            return its;
        framed_mesh = its;
        for (Vec3f &v : framed_mesh.vertices)
            v = (texture_frame * v.cast<double>()).cast<float>();
        return framed_mesh;
    };
    const auto sample_in_frame = [&](auto sampler) -> decltype(sampler) {
        if (!framed || !sampler)
            return sampler;
        return [sampler, frame = texture_frame, frame_normal](const Vec3f &pos, const Vec3f &normal) {
            return sampler((frame * pos.cast<double>()).cast<float>(),
                           (frame_normal * normal.cast<double>()).normalized().cast<float>());
        };
    };

    // Stage capture for the debug view. Timed from the end of the previous stage, so the capture -
    // a copy plus an edge scan - never lands inside the measurement it reports.
    auto       stage_clock = std::chrono::steady_clock::now();
    const auto capture     = [&](const char *name, const indexed_triangle_set &m,
                             const std::string &detail = {}) {
        if (debug == nullptr)
            return;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_clock).count();
        debug->capture(name, m.vertices, m.indices, ms, detail);
        stage_clock = std::chrono::steady_clock::now();
    };
    capture("input", base, "as handed to prepare");

    TriangleMesh                  mesh(base);
    TextureDisplacementFacetsData current = masks;
    bool                          changed = false;
    bool                          had_paint = false;
    for (const TriangleSelector::TriangleSplittingData &m : masks)
        had_paint = had_paint || TriangleSelector::has_facets(m, EnforcerBlockerType::ENFORCER);
    // The model's own UV channel for whatever `mesh` currently is. Every stage below keeps the input's
    // surface - it evens out, refines or cuts the triangles lying on it - so each corner reads its uv
    // straight off `base`'s channel at its own position instead of being carried stage by stage, which
    // would compound each stage's approximation. Untouched while nothing has replaced the mesh yet.
    const auto uv_now = [&]() {
        return (model_uv.empty() || !changed) ? model_uv : transfer_uv_channel(base, model_uv, mesh.its);
    };
    if (!report(1))
        return out;

    // 1. Even out the triangle density, and carry the paint onto the result. Everything downstream is
    //    driven by that paint, so losing it is a hard stop rather than something to bake around - and
    //    stopping here, before anything is committed, leaves the user's model exactly as it was.
    if (params.remesh_edge_mm > 0.f) {
        indexed_triangle_set remeshed;
        if (plan_remesh(mesh.its, params.remesh_edge_mm, params.remesh_sharp_deg, remeshed)) {
            TriangleMesh                   new_mesh(std::move(remeshed));
            const AABBTreeIndirect::Tree3f old_tree =
                AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.its.vertices, mesh.its.indices);
            TextureDisplacementFacetsData carried{};
            bool                          any = false;
            for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i) {
                // Both meshes are the volume's own local frame, so there is no shift between them.
                carried[size_t(i)] =
                    remap_texture_paint_spatial(mesh, current[size_t(i)], old_tree, new_mesh, Vec3f::Zero());
                any = any || !carried[size_t(i)].bitstream.empty();
            }
            mesh    = std::move(new_mesh);
            current = std::move(carried);
            changed = true;
            capture("remesh", mesh.its,
                    "target " + std::to_string(params.remesh_edge_mm) + " mm" +
                        (had_paint && !any ? ", paint lost" : ""));
            if (had_paint && !any) {
                out.paint_lost = true;
                return out;
            }
        } else if (debug != nullptr) {
            capture("remesh", mesh.its, "skipped: remeshing changed nothing");
        }
    }
    if (!report(50))
        return {};

    // 2. Refine where the texture bends - the painted area only, plus the graded band the conformal
    //    closure pulls in around it.
    if (params.subdiv_target_mm > 0.f) {
        std::vector<uint8_t>                                       region;
        std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> paint;
        if (collect_paint_region(mesh, current, region, &paint)) {
            // Feature-adaptive: sample the combined displacement so refinement follows texture
            // curvature. A null sampler (only LSCM layers, or nothing decodable) falls back to the
            // length baseline alone.
            const std::vector<Vec2f> uv_stage = uv_now();
            const std::vector<Vec2f> *uv_ptr  = uv_stage.empty() ? nullptr : &uv_stage;
            const indexed_triangle_set &sampled = mesh_to_sample(mesh.its);
            HeightFieldSampler sampler;
            if (params.subdiv_feature)
                sampler = sample_in_frame(make_combined_displacement_sampler(sampled, layers, current, uv_ptr));
            // Colour boundaries need triangles of their own - the chord test cannot see them, since
            // the height field is perfectly smooth across a change of filament.
            ColorFieldSampler color;
            // A flat-colour layer is matched against single filaments only, as the bake does, so its
            // boundaries are refined where the bake will actually change filament.
            if (params.subdiv_color_edge_mm > 0.f && !color_settings.empty())
                color = sample_in_frame(make_combined_color_sampler(sampled, layers, current,
                                                                    make_palette_quantizer(color_settings.palette),
                                                                    make_palette_quantizer(color_settings.palette_pure),
                                                                    uv_ptr));
            // The refinement follows perceived colour: a mix is one colour here, however the slicer
            // interleaves its filaments layer by layer.
            // "Min edge" is a feature-mode control (it is the floor the curvature test refines down
            // to); in plain adaptive mode the target edge length is the only criterion, so the floor
            // must not be allowed to silently override a target the user set below it.
            const float tol   = params.subdiv_feature ? params.subdiv_detail_mm : 0.f;
            const float floor = params.subdiv_feature ? params.subdiv_min_edge_mm : 0.f;
            // Step textures (a grid, a knurl: two levels with sharp edges between them) are cut into
            // walls after refining rather than refined into ramps, see 3. below. The verdict is taken
            // here, on the coarse mesh, because the refinement has to know: in step mode it leaves
            // the stepped triangles to the cutter, which is only right if the cutter then cuts them.
            // The step width and the seam gap both span two texels of the finest layer: the bilinear
            // blend across a step is a texel wide, so a seam copy a texel from the step samples a
            // pure side.
            float texel_mm = std::numeric_limits<float>::max();
            if (params.cut_steps && sampler)
                for (const TextureDisplacementLayer &layer : layers) {
                    if (layer.empty())
                        continue;
                    const DecodedHeightTexture &tex = decode_height_texture(layer);
                    if (tex.width > 0 && layer.tiling_scale > 0.f)
                        texel_mm = std::min(texel_mm, layer.tiling_scale / float(tex.width));
                }
            const bool cut_steps = texel_mm < std::numeric_limits<float>::max() &&
                                   texture_has_steps_to_cut(mesh.its, region, sampler, 2.f * texel_mm, 2.f * texel_mm);
            bool        aborted = false;
            std::vector<int> source;
            // The budget is "triangles the refinement may *add*", so the mesh's own count is the
            // baseline - otherwise the control would be meaningless (or a dead end) on a dense model.
            indexed_triangle_set refined =
                subdivide_mesh_adaptive(mesh.its, region, params.subdiv_target_mm,
                                        int(mesh.its.indices.size()) + params.subdiv_added_triangles, &source,
                                        sampler, tol, floor, params.subdiv_border_mm, [&](int pct) {
                                            aborted = !report(50 + pct / 2);
                                            return !aborted;
                                        },
                                        color, params.subdiv_color_edge_mm, cut_steps);
            if (aborted)
                return {};
            if (refined.indices.size() != mesh.its.indices.size()) {
                mesh    = TriangleMesh(std::move(refined));
                current = masks_after_subdivision(mesh, source, paint);
                changed = true;
                capture("adaptive subdivide", mesh.its,
                        params.subdiv_feature ? "feature adaptive" : "length only");
            } else if (debug != nullptr) {
                capture("adaptive subdivide", mesh.its, "skipped: already meets the criteria");
            }

            // 3. Cut the painted area along the texture's sharp steps so they bake as walls. The
            //    refinement above left the stepped triangles alone, so this is where a binary
            //    texture's edges get their geometry. Paint is carried the same way as through the
            //    subdivision: every output triangle lies inside the pre-subdivision triangle it
            //    descends from, so the source maps compose.
            if (cut_steps) {
                std::vector<uint8_t> cut_region(mesh.its.indices.size(), 0);
                for (size_t i = 0; i < cut_region.size() && i < source.size(); ++i)
                    cut_region[i] = (region[size_t(source[i])] & REFINE_PAINTED) ? 1 : 0;
                const std::vector<Vec2f> uv_refined = uv_now();
                const HeightFieldSampler refined_sampler =
                    sample_in_frame(make_combined_displacement_sampler(mesh_to_sample(mesh.its), layers, current,
                                                                       uv_refined.empty() ? nullptr : &uv_refined));
                std::vector<int>     cut_source;
                size_t               cuts = 0;
                indexed_triangle_set cut = cut_mesh_at_steps(mesh.its, cut_region, refined_sampler,
                                                             2.f * texel_mm, 2.f * texel_mm, 0.f,
                                                             &cut_source, &cuts);
                if (cuts > 0) {
                    for (int &s : cut_source)
                        s = source[size_t(s)];
                    mesh    = TriangleMesh(std::move(cut));
                    current = masks_after_subdivision(mesh, cut_source, paint);
                    changed = true;
                    capture("step cut", mesh.its, std::to_string(cuts) + " triangles cut");
                } else if (debug != nullptr) {
                    capture("step cut", mesh.its, "skipped: the refined mesh has no steps left to cut");
                }
            } else if (debug != nullptr && params.cut_steps) {
                capture("step cut", mesh.its, "skipped: not a step texture");
            }
        }
    }
    if (!report(100) || !changed)
        return out; // an empty result: nothing to commit, which is not a failure

    out.uv    = uv_now();
    out.mesh  = std::move(mesh.its);
    out.masks = std::move(current);
    return out;
}

void GLGizmoTextureDisplacement::subdivide_model_adaptive()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_subdivide_target_mm <= 0.f)
        return;

    update_model_object(); // flush any in-progress stroke into the committed masks first

    if (!any_part_painted()) {
        show_error(nullptr, _u8L("Paint the area you want to subdivide first - adaptive subdivision only "
                                 "refines where you have painted."));
        return;
    }

    TextureDisplacementPrepareParams params;
    params.subdiv_target_mm       = m_subdivide_target_mm;
    params.subdiv_detail_mm       = m_subdivide_detail_mm;
    params.subdiv_min_edge_mm     = m_subdivide_min_edge_mm;
    params.subdiv_border_mm       = m_subdivide_border_mm;
    params.subdiv_color_edge_mm   = m_subdivide_color_mm;
    params.subdiv_feature         = m_subdivide_feature;
    params.subdiv_added_triangles = m_subdivide_budget_k * 1000;
    queue_prepare(params, _u8L("Adaptive subdivide for texture displacement"), /* then_bake */ false,
                  _u8L("Nothing to subdivide - the painted area already meets the target edge length and "
                       "detail tolerance, or the triangle budget is already used up."));
}

void GLGizmoTextureDisplacement::smooth_model()
{
    ModelVolume *mv = texture_volume();
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return;
    const TextureDisplacementOptions &opts = mv->texture_displacement_options;
    if (opts.smooth_strength <= 0.f || opts.smooth_iterations <= 0)
        return;

    update_model_object(); // flush any in-progress stroke, so the painted region below is current

    // Movable = the vertices of the painted triangles, so the first ring of purely unpainted vertices
    // outside them stays put and anchors the result. Restricted rather than whole-model on purpose:
    // this runs on geometry that has already been baked, and relaxing the whole thing would quietly
    // round off every unrelated feature on the part. With "Ignore outer ring" the patch's own rim is
    // held as well - see TextureDisplacementOptions::smooth_skip_border.
    const indexed_triangle_set &its = mv->mesh().its;
    std::vector<uint8_t>        painted_face(its.indices.size(), 0);
    bool                        any = false;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        for (const TriangleSelector::TriangleBitStreamMapping &m : mv->texture_displacement_facet(slot).get_data().triangles_to_split)
            if (size_t(m.triangle_idx) < its.indices.size()) {
                painted_face[size_t(m.triangle_idx)] = 1;
                any                                  = true;
            }
    if (!any) {
        show_error(nullptr, _u8L("Paint the area you want to smooth first - smoothing only touches the "
                                 "painted part of the model."));
        return;
    }
    std::vector<uint8_t> movable(its.vertices.size(), 0);
    for (size_t i = 0; i < its.indices.size(); ++i)
        if (painted_face[i])
            for (int k = 0; k < 3; ++k)
                movable[size_t(its.indices[i][k])] = 1;
    if (opts.smooth_skip_border)
        for (size_t i = 0; i < its.indices.size(); ++i)
            if (!painted_face[i])
                for (int k = 0; k < 3; ++k)
                    movable[size_t(its.indices[i][k])] = 0; // also used by unpainted geometry -> the rim
    if (std::none_of(movable.begin(), movable.end(), [](uint8_t m) { return m != 0; })) {
        // Every painted vertex is on the patch's rim, so "Ignore outer ring" leaves nothing to move.
        show_error(nullptr, _u8L("Nothing to smooth - the painted area is only one triangle deep, so with "
                                 "\"Ignore outer ring\" on there are no interior vertices to relax."));
        return;
    }

    indexed_triangle_set smoothed = its;
    {
        wxBusyCursor wait;
        smooth_mesh_vertices(smoothed, movable, opts.smooth_strength, opts.smooth_iterations);
    }

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Smooth texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    // No save/restore-painting dance here, unlike subdivide and remesh: smoothing only moves vertices,
    // it does not touch the triangle list, so every paint channel still refers to exactly the triangles
    // it did before. set_mesh() clears the extra facets, so this saves and puts back the
    // texture-displacement masks verbatim - no remap needed, and none of them is lost.
    std::optional<TriangleSelector::SavedPainting>                    saved_painting = mv->save_painting();
    std::array<TriangleSelector::TriangleSplittingData, TEXTURE_DISPLACEMENT_MAX_LAYERS> saved_texture;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        saved_texture[size_t(i)] = mv->texture_displacement_facet(i).get_data();

    mv->set_mesh(TriangleMesh(std::move(smoothed)));
    mv->set_new_unique_id();
    mv->calculate_convex_hull();
    mv->restore_painting(saved_painting);
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        mv->texture_displacement_facet(i).set_data(std::move(saved_texture[size_t(i)]));

    if (ObjectList *obj_list = wxGetApp().obj_list()) {
        const ModelObjectPtrs &objs = plater->model().objects;
        auto it = std::find(objs.begin(), objs.end(), mo);
        if (it != objs.end())
            obj_list->update_info_items(size_t(it - objs.begin()));
    }
    plater->changed_object(*mo);
    update_from_model_object(false);
    m_parent.set_as_dirty();
}

bool GLGizmoTextureDisplacement::plan_remesh(const indexed_triangle_set &src, float target_edge_mm,
                                             float sharp_angle_deg, indexed_triangle_set &out)
{
    if (target_edge_mm <= 0.f)
        return false;

    // Its cost and memory grow with the *square* of 1/target_edge, and it runs single-threaded on the
    // UI thread, so the target has to be bounded against the part's actual surface area rather than
    // taken at face value. Standard mode asks for 1 mm whatever it is handed, and the slider goes down
    // to 0.1 mm: on a 150 mm part those are ~500 k and ~50 M triangles respectively - minutes to hours
    // of frozen UI, which is indistinguishable from a hang. Raising the target instead still gives the
    // subdivider the even density it needs, and the subdivision that follows is where the detail was
    // always going to come from anyway. An equilateral triangle of edge L covers sqrt(3)/4 * L^2.
    static constexpr double REMESH_MAX_TRIANGLES = 150000.0;
    double area = 0.0;
    for (const stl_triangle_vertex_indices &tri : src.indices)
        area += 0.5 * double((src.vertices[size_t(tri[1])] - src.vertices[size_t(tri[0])])
                                 .cross(src.vertices[size_t(tri[2])] - src.vertices[size_t(tri[0])])
                                 .norm());
    if (const double budget_edge = std::sqrt(area / (0.4330127 * REMESH_MAX_TRIANGLES));
        budget_edge > double(target_edge_mm)) {
        BOOST_LOG_TRIVIAL(info) << "Texture displacement: remesh target raised from " << target_edge_mm
                                << " mm to " << budget_edge << " mm to stay within "
                                << int(REMESH_MAX_TRIANGLES) << " triangles.";
        target_edge_mm = float(budget_edge);
    }

    // Five iterations with three relaxation passes each, rather than CGAL's three-and-one. Splitting
    // and collapsing bring edge lengths near the target but leave the vertices where they fell, so it
    // is the relaxation count that decides how even the result looks - and three passes in total was
    // nowhere near enough. Fifteen costs proportionally more, but only on an explicit Remesh.
    indexed_triangle_set remeshed = MeshBoolean::cgal::remesh_isotropic(
        src, double(target_edge_mm), /* iterations */ 5, double(sharp_angle_deg),
        /* relaxation steps */ 3);
    // remesh_isotropic() signals failure by handing the input straight back, so compare against it
    // structurally. Vertex count alone is not enough: a remesh that only redistributes triangles at
    // roughly the current density legitimately lands on the same count, and treating that as failure
    // meant a perfectly good result got thrown away with an error message.
    if (remeshed.indices.empty() ||
        (remeshed.vertices.size() == src.vertices.size() && remeshed.indices.size() == src.indices.size() &&
         remeshed.indices == src.indices))
        return false;

    out = std::move(remeshed);
    return true;
}

void GLGizmoTextureDisplacement::remesh_model()
{
    if (texture_volume() == nullptr || m_remesh_target_edge_mm <= 0.f)
        return;

    update_model_object(); // the paint rides across the remesh, so flush any in-progress stroke first

    TextureDisplacementPrepareParams params;
    params.remesh_edge_mm   = m_remesh_target_edge_mm;
    params.remesh_sharp_deg = m_remesh_keep_sharp_edges ? m_remesh_sharp_angle_deg : 0.f;
    queue_prepare(params, _u8L("Remesh model for texture displacement"), /* then_bake */ false,
                  _u8L("Remeshing did not change the model. It may be non-manifold (open edges or edges "
                       "shared by more than two triangles), or the target edge length may already be met."),
                  PrepareScope::AllParts);
}

void GLGizmoTextureDisplacement::rebuild_subdivide_preview()
{
    m_subdivide_preview_glmodel.reset();
    m_subdivide_preview_tris = -1;
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;

    // The same subdivision Apply would commit, kept in a throwaway mesh and shown only as a
    // wireframe - the model itself is not touched until Apply.
    indexed_triangle_set its;
    if (m_subdivide_adaptive) {
        if (m_subdivide_target_mm <= 0.f)
            return;
        const TextureDisplacementFacetsData facets = facets_data_of(*mv);
        std::vector<uint8_t>                region;
        if (!collect_paint_region(mv->mesh(), facets, region, nullptr))
            return; // nothing painted yet: nothing to preview
        HeightFieldSampler sampler;
        if (m_subdivide_feature)
            sampler = make_combined_displacement_sampler(mv->mesh().its, mv->texture_displacement_layers, facets);
        // Feature mode: curvature (Detail tolerance) on top of the Max-edge baseline, down to the
        // Min-edge floor. Plain adaptive: tol 0, so only the target-edge-length criterion applies.
        const float tol   = m_subdivide_feature ? m_subdivide_detail_mm : 0.f;
        const float floor = m_subdivide_feature ? m_subdivide_min_edge_mm : 0.f;
        // Same colour criterion Apply will use, so the previewed wireframe is the mesh that commits.
        ColorFieldSampler color;
        if (m_subdivide_color_mm > 0.f && any_layer_colors(*mv)) {
            const auto [quantize, quantize_pure] = palette_quantizers();
            color = make_combined_color_sampler(mv->mesh().its, mv->texture_displacement_layers, facets, quantize,
                                                quantize_pure);
        }
        its = subdivide_mesh_adaptive(mv->mesh().its, region, m_subdivide_target_mm,
                                      int(mv->mesh().its.indices.size()) + m_subdivide_budget_k * 1000,
                                      nullptr, sampler, tol, floor, m_subdivide_border_mm, nullptr, color,
                                      m_subdivide_color_mm);
    } else {
        if (m_subdivide_count < 1)
            return;
        its = subdivide_mesh_uniform(mv->mesh().its, 0.f, m_subdivide_count);
    }
    if (its.indices.empty())
        return;
    m_subdivide_preview_tris = int(its.indices.size());

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(its.vertices.size());
    init_data.reserve_indices(its.indices.size() * 6);
    for (const Vec3f &v : its.vertices)
        init_data.add_vertex(v);
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i)
            init_data.add_line(unsigned(tri[i]), unsigned(tri[(i + 1) % 3]));
    if (!init_data.is_empty())
        m_subdivide_preview_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::render_subdivide_preview()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_subdivide_preview_glmodel.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_POLYGON_OFFSET_LINE));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    m_subdivide_preview_glmodel.set_color(ColorRGBA(0.2f, 0.9f, 1.0f, 0.7f)); // cyan, reads as "preview"
    m_subdivide_preview_glmodel.render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_LINE));
    shader->stop_using();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_thumbnail(const TextureDisplacementLayer &layer)
{
    if (layer.empty() || layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return nullptr;

    const size_t slot = size_t(layer.slot);
    if (m_thumbnails[slot] && m_thumbnail_source[slot].lock() == layer.image_data &&
        m_thumbnail_smoothing[slot] == layer.smoothing)
        return m_thumbnails[slot].get();

    // Reuses the already-decoded, already-cached grayscale pixels (see decode_height_texture()'s
    // own cache in TextureDisplacement.cpp) - only the gray-to-RGBA expansion and GPU upload below are
    // new work. Rebuilt when the texture *or the smoothing* changes, so the fast/shaded preview - which
    // samples this GPU texture directly - reflects the current smoothing rather than the raw image.
    std::unique_ptr<GLTexture> texture = upload_height_thumbnail(decode_height_texture(layer));
    if (!texture)
        return nullptr;

    m_thumbnails[slot]          = std::move(texture);
    m_thumbnail_source[slot]    = layer.image_data;
    m_thumbnail_smoothing[slot] = layer.smoothing;
    return m_thumbnails[slot].get();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_color_texture(const TextureDisplacementLayer &layer)
{
    if (layer.empty() || !layer.color_enabled)
        return nullptr;
    if (m_color_tex && m_color_tex_source.lock() == layer.image_data && m_color_tex_smoothing == layer.smoothing)
        return m_color_tex.get();

    std::unique_ptr<GLTexture> texture = upload_color_texture(decode_height_texture(layer), HEIGHT_TEX_MAX_PX);
    if (!texture) {
        m_color_tex.reset();
        m_color_tex_source.reset();
        return nullptr;
    }
    m_color_tex           = std::move(texture);
    m_color_tex_source    = layer.image_data;
    m_color_tex_smoothing = layer.smoothing;
    return m_color_tex.get();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_height_texture(const TextureDisplacementLayer &layer)
{
    if (layer.empty())
        return nullptr;

    // One slot, not one per layer: the preview shader only ever shades the *active* layer, so a single
    // full-resolution upload is enough and the VRAM cost stays at one texture rather than eight.
    if (m_height_tex && m_height_tex_source.lock() == layer.image_data && m_height_tex_smoothing == layer.smoothing)
        return m_height_tex.get();

    std::unique_ptr<GLTexture> texture = upload_height_thumbnail(decode_height_texture(layer), HEIGHT_TEX_MAX_PX);
    if (!texture)
        return nullptr;

    m_height_tex           = std::move(texture);
    m_height_tex_source    = layer.image_data;
    m_height_tex_smoothing = layer.smoothing;
    return m_height_tex.get();
}

void GLGizmoTextureDisplacement::bake(bool own_snapshot)
{
    ModelVolume *owner = texture_volume();
    if (owner == nullptr || m_bake_in_progress)
        return;

    // Make sure the currently active layer's in-progress edits are flushed into the model before
    // baking, otherwise the most recent, not-yet-committed strokes would be silently skipped.
    update_model_object();

    // Every painted part, not only the one holding the layer stack: the brush paints across all of
    // them (see update_model_object()), so baking one would leave the user's paint on the others
    // untouched and their geometry flat.
    const std::vector<ModelVolume *> parts = painted_parts();
    if (parts.empty()) {
        show_error(nullptr, _u8L("Nothing is painted, there is nothing to bake."));
        return;
    }

    const Vec3d                                anchor = texture_anchor();
    const std::vector<float>                   drops  = resting_drops(parts);
    std::vector<TextureDisplacementBakeVolume> volumes;
    volumes.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i)
        volumes.push_back(texture_displacement_bake_volume(*parts[i], layers_for_part(*parts[i]),
                                                           owner->texture_displacement_options, anchor, drops[i]));

    m_bake_in_progress = true;
    queue_texture_displacement_bake(std::move(volumes), color_settings_for(*owner), [this]() {
        m_bake_in_progress = false;
        // Baking replaces each baked part's mesh (new id, new topology) without changing the object's
        // id or volume count, so GLGizmoPainterBase::data_changed()'s usual change-detection never
        // notices it needs to reload - do it explicitly here, otherwise the gizmo keeps painting
        // and rendering against the stale, pre-bake TriangleSelectorPatch until the object is
        // deselected and reselected.
        if (m_state == On && m_c->selection_info() && m_c->selection_info()->model_object())
            update_from_model_object(false);
        m_parent.set_as_dirty();
    }, own_snapshot);
}

// Standard mode's fixed recipe. These are both the values the hidden controls are pinned to and what
// bake_standard() drives its remesh and subdivision with - one set of numbers, so the preview cannot
// disagree with the bake. Chosen to be safe on an arbitrary imported part rather than optimal on any
// particular one: a 1 mm isotropic remesh gives the subdivider an even starting density whatever the
// input looked like, and the subdivision then spends up to 1.5 M triangles chasing texture curvature
// down to a 0.02 mm floor, which is finer than any FDM nozzle will resolve. The triangle budget is not
// here: it is the one control Standard mode still shows, so it belongs to the user (its default is
// m_subdivide_budget_k's initialiser).
static constexpr float STD_REMESH_EDGE_MM     = 1.0f;
static constexpr float STD_REMESH_SHARP_DEG   = 40.f;
static constexpr float STD_SUBDIV_MAX_EDGE_MM = 20.f;
static constexpr float STD_SUBDIV_DETAIL_MM   = 0.02f;
// 0.1 mm rather than the 0.02 it was: a hard step in the texture never satisfies the chord tolerance
// however finely it is split, so the floor is what stops it, and 0.02 mm carpeted every step edge with
// triangles five times finer than a nozzle can print - most of the budget, on almost every texture.
static constexpr float STD_SUBDIV_MIN_EDGE_MM = 0.1f;
// Edge length the band straddling the paint's edge is refined to. This is the one number that decides
// how clean the rim of an unpainted island looks: the bake steps the surface from full displacement to
// zero across that band, and nothing else in the criteria can see the step (see collect_paint_region()).
static constexpr float STD_SUBDIV_BORDER_MM   = 0.4f;
// Edge length a colour boundary is refined to. Finer than the border band because a colour edge is
// what the eye actually lands on - a stepped outline around a printed decal reads as a defect in a
// way a slightly coarse relief transition does not - and it costs triangles along an outline only.
static constexpr float STD_SUBDIV_COLOR_MM    = 0.25f;

bool GLGizmoTextureDisplacement::apply_standard_mode_presets(ModelVolume *mv)
{
    bool       changed = false;
    const auto pin     = [&changed](auto &field, auto value) {
        if (field != value) {
            field   = value;
            changed = true;
        }
    };

    if (mv != nullptr) {
        pin(mv->texture_displacement_options.displace_border, true);
        pin(mv->texture_displacement_options.smooth_enabled, false);
    }
    pin(m_subdivide_adaptive, true);
    pin(m_subdivide_feature, true);
    pin(m_subdivide_target_mm, STD_SUBDIV_MAX_EDGE_MM);
    pin(m_subdivide_detail_mm, STD_SUBDIV_DETAIL_MM);
    pin(m_subdivide_min_edge_mm, STD_SUBDIV_MIN_EDGE_MM);
    pin(m_subdivide_border_mm, STD_SUBDIV_BORDER_MM);
    pin(m_subdivide_color_mm, STD_SUBDIV_COLOR_MM);
    // Deliberately *not* pinned: the triangle budget stays visible and editable in Standard mode, so
    // pinning it would fight the user's own slider every frame.
    pin(m_remesh_target_edge_mm, STD_REMESH_EDGE_MM);
    pin(m_remesh_keep_sharp_edges, true);
    pin(m_remesh_sharp_angle_deg, STD_REMESH_SHARP_DEG);
    return changed;
}

void GLGizmoTextureDisplacement::bake_standard()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_bake_in_progress || m_prepare_in_progress)
        return;

    update_model_object(); // flush the active layer's in-progress strokes before anything reads them
    if (!any_part_painted()) {
        show_error(nullptr, _u8L("Nothing is painted, there is nothing to bake."));
        return;
    }
    apply_standard_mode_presets(mv); // belt and braces: never bake with values the panel is not showing

    // It refines as part of the bake, so preparing first would refine a second time at another
    // target.
    if (mv->texture_displacement_options.pipeline_v2) {
        bake();
        return;
    }

    // The whole recipe in one go. Either stage having nothing to do is normal, not a failure - a mesh
    // that is already even needs no remesh, one that is already fine enough for the texture needs no
    // subdivision - so no "nothing changed" message here: it goes straight on to the displacement.
    TextureDisplacementPrepareParams params;
    params.remesh_edge_mm         = STD_REMESH_EDGE_MM;
    params.remesh_sharp_deg       = STD_REMESH_SHARP_DEG;
    params.subdiv_target_mm       = STD_SUBDIV_MAX_EDGE_MM;
    params.subdiv_detail_mm       = STD_SUBDIV_DETAIL_MM;
    params.subdiv_min_edge_mm     = STD_SUBDIV_MIN_EDGE_MM;
    params.subdiv_border_mm       = STD_SUBDIV_BORDER_MM;
    params.subdiv_color_edge_mm   = STD_SUBDIV_COLOR_MM;
    params.subdiv_feature         = true;
    params.subdiv_added_triangles = m_subdivide_budget_k * 1000;
    params.cut_steps              = true;
    queue_prepare(params, _u8L("Bake texture displacement"), /* then_bake */ true, {});
}

// ---------------------------------------------------------------------------------------------
// Bake stage debug view
// ---------------------------------------------------------------------------------------------

void GLGizmoTextureDisplacement::run_stage_debug()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_debug_in_progress || m_bake_in_progress || m_prepare_in_progress)
        return;

    update_model_object(); // flush the active layer's in-progress strokes, as a real bake would
    if (!mv->is_texture_displacement_painted()) {
        show_error(nullptr, _u8L("Nothing is painted, so there are no bake stages to capture."));
        return;
    }

    // The default (one-run) pipeline refines as part of the bake, and Pro mode has the user prepare the
    // mesh themselves with the controls above - in both cases there is no preparation to replay, and
    // running one anyway would show stages the Bake button would not.
    const bool run_prepare = !pro_mode() && !mv->texture_displacement_options.pipeline_v2;

    TextureDisplacementPrepareParams params;
    if (run_prepare) {
        apply_standard_mode_presets(mv); // capture what the panel is showing, exactly as Bake does
        params.remesh_edge_mm         = STD_REMESH_EDGE_MM;
        params.remesh_sharp_deg       = STD_REMESH_SHARP_DEG;
        params.subdiv_target_mm       = STD_SUBDIV_MAX_EDGE_MM;
        params.subdiv_detail_mm       = STD_SUBDIV_DETAIL_MM;
        params.subdiv_min_edge_mm     = STD_SUBDIV_MIN_EDGE_MM;
        params.subdiv_border_mm       = STD_SUBDIV_BORDER_MM;
        params.subdiv_color_edge_mm   = STD_SUBDIV_COLOR_MM;
        params.subdiv_feature         = true;
        params.subdiv_added_triangles = m_subdivide_budget_k * 1000;
        params.cut_steps              = true;
    }

    // Free the previous run's meshes before the next one allocates its own: every stage is a full
    // copy of the mesh, so holding two runs at once doubles what is already the expensive part.
    exit_debug_view();

    m_debug_in_progress = true;
    queue_texture_displacement_debug(
        *mv, color_settings_for(*mv), params, prepare_texture_frame(*mv), resting_drops({mv}).front(), run_prepare,
        m_debug_check_topology,
        [this](std::vector<BakeStageSnapshot> stages) {
            m_debug_in_progress = false;
            if (!stages.empty()) {
                m_debug_stages = std::move(stages);
                // Land on the last stage: that is what a real bake would have committed, and the
                // question is usually what it looks like before stepping back to find where it went
                // wrong.
                show_debug_stage(int(m_debug_stages.size()) - 1);
            }
            m_parent.set_as_dirty();
        });
}

void GLGizmoTextureDisplacement::show_debug_stage(int index)
{
    if (index < 0 || size_t(index) >= m_debug_stages.size())
        return;
    const BakeStageSnapshot &stage = m_debug_stages[size_t(index)];
    if (stage.mesh.empty())
        return; // over the memory cap: its counts are still listed, there is just nothing to draw

    m_debug_stage = index;
    // Drawn through the ordinary true-displacement preview, so the Fast view - a shader trick over
    // the base mesh that never draws m_preview_glmodel - has to be left first.
    m_use_shaded_preview = false;
    m_preview_color_runs.clear();
    m_preview_glmodel.reset();
    // The stage is a capture of one part's run, so the other parts go back to being drawn as themselves
    // rather than keeping a live preview the captured stages say nothing about.
    m_part_previews.clear();

    indexed_triangle_set its;
    its.vertices = stage.mesh.vertices;
    its.indices  = stage.mesh.indices;
    m_preview_glmodel.init_from(its);
    m_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);
    // Kept so the wireframe overlay draws this stage's edges rather than the base mesh's - which is
    // most of the point, since what a refinement stage did is a change in the edges, not the surface.
    m_preview_its = std::move(its);
    refresh_wireframe();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::exit_debug_view()
{
    const bool was_showing = m_debug_stage >= 0;
    m_debug_stage          = -1;
    m_debug_stages.clear();
    m_debug_stages.shrink_to_fit();
    if (was_showing)
        rebuild_preview(); // hands m_preview_glmodel back to the live preview
}

void GLGizmoTextureDisplacement::render_debug_stage_panel(ModelVolume *mv)
{
    if (mv == nullptr)
        return;

    ImGui::Separator();
    if (!ImGui::CollapsingHeader(_u8L("Bake stages (debug)").c_str()))
        return;

    m_imgui->disabled_begin(m_debug_in_progress || m_bake_in_progress || m_prepare_in_progress ||
                            !mv->is_texture_displacement_painted());
    if (m_imgui->button(m_debug_in_progress ? _L("Capturing...") : _L("Capture stages")))
        run_stage_debug();
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        panel_tooltip(_u8L("Run the same bake the Bake button runs, keeping the mesh after every "
                           "stage, and step through them here. Nothing is committed to the model - "
                           "this is for finding which stage a bad result came from."));

    ImGui::SameLine();
    m_imgui->bbl_checkbox(_L("Check topology"), m_debug_check_topology);
    if (ImGui::IsItemHovered())
        panel_tooltip(_u8L("Count open and non-manifold edges after each stage, which is how a stage "
                           "that tore the mesh is spotted. It scans every edge, so it adds noticeably "
                           "to the capture on a fine bake."));

    if (m_debug_stages.empty())
        return;

    // Stage list. Selecting one draws it; the wireframe overlay is what makes a refinement stage
    // readable, so it is worth leaving on while stepping.
    ImGui::BeginChild("##bake_stages", ImVec2(0.f, m_imgui->scaled(9.f)), true,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    for (size_t i = 0; i < m_debug_stages.size(); ++i) {
        const BakeStageSnapshot &st = m_debug_stages[i];
        char                     label[256];
        std::snprintf(label, sizeof(label), "%zu. %-18s %8zu tris %8.1f ms##stage%zu", i,
                      st.name.c_str(), st.triangles, st.ms, i);
        if (ImGui::Selectable(label, int(i) == m_debug_stage) && !st.mesh.empty())
            show_debug_stage(int(i));
    }
    ImGui::EndChild();

    // Prev / next, so stepping does not mean aiming at a list entry.
    m_imgui->disabled_begin(m_debug_stage <= 0);
    if (m_imgui->button(_L("< Previous")))
        show_debug_stage(m_debug_stage - 1);
    m_imgui->disabled_end();
    ImGui::SameLine();
    m_imgui->disabled_begin(m_debug_stage < 0 || size_t(m_debug_stage + 1) >= m_debug_stages.size());
    if (m_imgui->button(_L("Next >")))
        show_debug_stage(m_debug_stage + 1);
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (m_imgui->button(_L("Close")))
        exit_debug_view();

    if (m_debug_stage >= 0 && size_t(m_debug_stage) < m_debug_stages.size()) {
        const BakeStageSnapshot &st = m_debug_stages[size_t(m_debug_stage)];
        m_imgui->text(from_u8(st.name) + ": " + std::to_string(st.triangles) + " " +
                      _u8L("triangles") + ", " + std::to_string(st.vertices) + " " + _u8L("vertices"));
        if (!st.detail.empty())
            m_imgui->text(from_u8(st.detail));
        if (st.topology_checked) {
            // Zero on both counts is the claim every stage's header makes; anything else is the bug.
            const std::string topo = _u8L("Open edges") + ": " + std::to_string(st.open_edges) + "   " +
                                     _u8L("Non-manifold") + ": " + std::to_string(st.non_manifold_edges) +
                                     "   " + _u8L("Degenerate") + ": " + std::to_string(st.degenerate);
            if (st.open_edges > 0 || st.non_manifold_edges > 0)
                m_imgui->warning_text(from_u8(topo));
            else
                m_imgui->text(topo);
        }
    }

    double total_ms = 0.0;
    for (const BakeStageSnapshot &st : m_debug_stages)
        total_ms += st.ms;
    char total[128];
    std::snprintf(total, sizeof(total), "%s: %.1f ms", _u8L("Total").c_str(), total_ms);
    m_imgui->text(total);

    if (m_imgui->button(_L("Write stage meshes"))) {
        const std::string dir =
            (boost::filesystem::temp_directory_path() / "orca-bake-stages").string();
        const size_t written = dump_bake_stages(m_debug_stages, dir);
        if (written > 0)
            show_info(nullptr, from_u8(_u8L("Wrote the stage meshes as OBJ files to:") + "\n" + dir));
        else
            show_error(nullptr, _u8L("Could not write the stage meshes."));
    }
    if (ImGui::IsItemHovered())
        panel_tooltip(_u8L("Write every captured stage out as an OBJ file, so they can be opened "
                           "side by side in a mesh viewer."));
}

void GLGizmoTextureDisplacement::queue_prepare(const TextureDisplacementPrepareParams &params,
                                               const std::string &snapshot_name, bool then_bake,
                                               const std::string &unchanged_msg, PrepareScope scope)
{
    ModelVolume *owner = texture_volume();
    if (owner == nullptr || m_prepare_in_progress || m_bake_in_progress)
        return;

    // Preparing only the layer stack's own part would hand the displacement a coarse mesh everywhere
    // else - a height map can only move vertices that already exist, so on a multibody object that is
    // the difference between relief and a flat face. See PrepareScope for which parts that means.
    const std::vector<ModelVolume *> parts = scope == PrepareScope::AllParts ? texture_parts() : painted_parts();
    if (parts.empty())
        return;

    TextureDisplacementPrepareInput input;
    input.params        = params;
    input.snapshot_name = snapshot_name;
    input.volumes.reserve(parts.size());
    for (const ModelVolume *part : parts) {
        TextureDisplacementPrepareVolume entry;
        entry.volume_id = part->id();
        entry.base_mesh = part->mesh().its;
        entry.masks     = facets_data_of(*part);
        entry.layers    = layers_for_part(*part);
        if (part->has_texture_uv())
            entry.model_uv = part->texture_uv;
        entry.texture_frame = prepare_texture_frame(*part);
        input.volumes.push_back(std::move(entry));
    }
    if (params.subdiv_color_edge_mm > 0.f)
        input.color = color_settings_for(*owner);

    m_prepare_in_progress = true;
    queue_texture_displacement_prepare(std::move(input), [this, then_bake, unchanged_msg](
                                                             TextureDisplacementPrepareOutcome outcome) {
        m_prepare_in_progress = false;
        // The commit replaced the volume's mesh (new id, new topology) without changing the object's id
        // or volume count, which is not something GLGizmoPainterBase::data_changed() can detect - so the
        // reload is explicit, exactly as it is after a bake. Done for every outcome: even a run that
        // committed nothing may have left the panel showing a stale triangle count.
        if (m_state == On && m_c->selection_info() && m_c->selection_info()->model_object())
            update_from_model_object(false);
        m_parent.set_as_dirty();

        switch (outcome) {
        case TextureDisplacementPrepareOutcome::Failed:
            return; // cancelled, or the volume went away while the job ran - say nothing, do nothing
        case TextureDisplacementPrepareOutcome::PaintLost:
            show_error(nullptr, _u8L("The painted area could not be carried onto the remeshed model, so "
                                     "nothing was changed. Switch to Pro mode and remesh before painting."));
            return;
        case TextureDisplacementPrepareOutcome::Unchanged:
            if (!unchanged_msg.empty())
                show_error(nullptr, unchanged_msg);
            break;
        case TextureDisplacementPrepareOutcome::Committed:
            break;
        }
        // ... and then the displacement itself, in the background exactly as the Pro-mode button does.
        // It commits into the snapshot the prepare opened - but only if the prepare opened one: a run
        // that found nothing to do took none, and a bake chained onto that has to push its own or it
        // would not be undoable at all.
        if (then_bake)
            bake(/* own_snapshot */ outcome == TextureDisplacementPrepareOutcome::Unchanged);
    });
}

void GLGizmoTextureDisplacement::render_paint_cursor_hint()
{
    // Only in the plain paint/select modes; seam and adjust modes have their own click semantics where
    // an add/remove sign would just be noise.
    if (m_seam_edit_mode || m_adjust_texture_mode)
        return;
    const ImGuiIO &io = ImGui::GetIO();
    // The pointer must be over the 3D view, not over this panel (or any other ImGui window).
    if (io.WantCaptureMouse || !ImGui::IsMousePosValid())
        return;

    // Shift erases (see handle_snapshot_action_name()), and so does a plain stroke in Erase mode.
    const bool  removing = io.KeyShift || m_erase_mode;
    const ImU32 color    = removing ? IM_COL32(235, 70, 60, 255) : IM_COL32(90, 210, 110, 255);
    const char *glyph    = removing ? "-" : "+";

    ImDrawList  *dl = ImGui::GetForegroundDrawList();
    const float  fs = ImGui::GetFontSize() * 1.5f;
    const ImVec2 at(io.MousePos.x + 15.f, io.MousePos.y - fs - 6.f);
    // A translucent dark disc behind the glyph so it reads on any material colour.
    dl->AddCircleFilled(ImVec2(at.x + fs * 0.28f, at.y + fs * 0.5f), fs * 0.62f, IM_COL32(0, 0, 0, 150));
    dl->AddText(ImGui::GetFont(), fs, at, color, glyph);
}

void GLGizmoTextureDisplacement::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;
    ModelVolume *mv = texture_volume();

    // The palette also follows project state nobody tells this gizmo about - the filaments and mixed
    // slots in the sidebar - so it is checked once a frame, and a change re-runs the previews.
    cached_palette();
    if (m_palette_changed)
        m_preview_params_dirty = true;

    float  scale = m_parent.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32

    const float approx_height = m_imgui->scaled(24.f);
    y = std::min(y, bottom_limit - approx_height);

    // Docked (the default) the panel is pinned to the right edge of the 3D canvas and cannot be
    // moved. Deliberately *not* next to the gizmo toolbar, which is where `x` points and where every
    // other gizmo's window goes: this panel is far taller than those (layer stack plus the whole
    // per-layer control set), so at the toolbar it sits right on top of the part of the model being
    // painted. Pinning it to the canvas edge also parks it against the UV editor, since that pane is
    // docked on the right and the canvas therefore ends exactly at the pane's left edge - so the
    // panel follows the pane in and out instead of being clipped by it.
    //
    // Undocked it becomes an ordinary floating window: a title bar to drag it by, and no forced
    // position - the position is deliberately not seeded on undock, so the window stays exactly
    // where it already was and the user just gains the ability to move it from there.
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    if (!m_undocked) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
        // Right-aligned (pivot 1), so the width the panel auto-resized to last frame does not need to
        // be known here. Width 0 skips GizmoImguiSetNextWIndowPos()'s own left-aligned fit-to-canvas
        // clamp, which would push the window back off the edge it is being pinned to.
        float right = float(m_parent.get_canvas_size().get_width()) - m_imgui->scaled(0.5f);
        GizmoImguiSetNextWIndowPos(right, y, 0.f, 0.f, ImGuiCond_Always, 1.0f, 0.0f);
    }

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), flags);
    // Where this frame's tooltips place themselves (see panel_tooltip()). The size is last frame's -
    // this window auto-resizes, so its height is only known once its content has been laid out - which
    // is close enough for a tooltip that needs a frame of hovering to appear in the first place.
    m_tooltip_anchor_min = ImGui::GetWindowPos();
    m_tooltip_anchor_max = ImVec2(m_tooltip_anchor_min.x + ImGui::GetWindowSize().x,
                                  m_tooltip_anchor_min.y + ImGui::GetWindowSize().y);
    ensure_panel_icons();
    process_uv_commands(); // clicks from the UV editor pane, run here where the GL context is current
    // After the pane's commands, which can close it or change the view, and before the View row, so the row
    // lights the view this frame ends up in and a click on it has the last word.
    follow_island_highlight();
    const float previous_body_h   = m_panel_body_h;
    const float previous_footer_h = m_panel_footer_h;

    // Pinned every frame while Standard is active, so what Preview shows is always what Bake will do.
    if (!pro_mode() && apply_standard_mode_presets(mv))
        m_preview_params_dirty = true;

    // Layout follows the Option C design: a fixed-width panel, a header pinned at the top, a body that
    // scrolls as one, and a footer with Bake pinned at the bottom. Rows are laid out against panel_w and
    // against the body child's own fixed width - never against this window's width, which under
    // AlwaysAutoResize is derived from its content and would feed back into it.
    const bool        dark     = wxGetApp().dark_mode();
    const bool        busy     = m_bake_in_progress || m_prepare_in_progress;
    const ImGuiStyle &style    = ImGui::GetStyle();
    const float       panel_w  = m_imgui->scaled(21.5f);
    const float       frame_h  = ImGui::GetFrameHeight();
    const float       icon_md  = std::round(frame_h * 1.25f); // tool and view buttons
    const float       icon_sm  = std::round(frame_h * 1.05f); // buttons sitting in a row of text
    const float       gap_s    = std::round(m_imgui->scaled(0.4f));
    const float       label_w  = m_imgui->scaled(4.8f); // label column of a label + slider row
    const float       card_pad = std::round(m_imgui->scaled(0.55f));
    const ImVec4      orca     = ImGuiWrapper::COL_ORCA;
    const ImVec4      col_link = ImGuiWrapper::COL_ORCA;
    const ImVec4      col_frame = ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    const ImU32       col_card = ImGui::GetColorU32(ImGuiCol_Text, dark ? 0.045f : 0.047f);
    const ImU32       col_line = ImGui::GetColorU32(ImGuiCol_Text, dark ? 0.08f : 0.09f);
    const ImU32       col_sep  = ImGui::GetColorU32(ImGuiCol_Separator);

    const auto   scoped_combo   = [&](const char *id, int *v, const char *const items[], int n) {
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        const bool changed = ImGui::Combo(id, v, items, n);
        ImGuiWrapper::pop_combo_style();
        return changed;
    };
    const auto hover_tip = [&](const auto &text, const std::vector<TipMedia> &media = {}) {
        if (ImGui::IsItemHovered())
            panel_tooltip(text, media);
    };

    // Framed icon toggle: a 1 px frame, and when on a teal frame over a teal tint with the icon in its
    // original colours (IconManager's color_wite_gray variant [1]); off, the theme's grey variant [0].
    // `unavailable`, when not empty, draws it faded but still hoverable, so the tooltip can say why it
    // cannot be used - a disabled ImGui item would swallow the hover and leave the user guessing.
    const auto icon_toggle = [&](int uid, const std::string &iconfile, bool active, float sz, const wxString &label,
                                 const wxString &tip_text, const std::vector<TipMedia> &media = {},
                                 const wxString &unavailable = wxString()) -> bool {
        const bool  na      = !unavailable.empty();
        const auto  it      = m_panel_icon_map.find(iconfile);
        const float pad     = std::max(1.f, std::round(sz * 0.14f));
        const int   variant = active ? 1 : 0;
        bool        clicked = false;
        ImGui::PushID(uid);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, na ? style.Alpha * 0.32f : style.Alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(orca.x, orca.y, orca.z, 0.22f) : ImVec4(0.f, 0.f, 0.f, 0.f));
        if (na) {
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.f, 0.f, 0.f, 0.f));
        }
        if (it != m_panel_icon_map.end() && int(it->second.size()) > variant && it->second[size_t(variant)]->is_valid()) {
            const IconManager::Icon &ic = *it->second[size_t(variant)];
            clicked = m_imgui->image_button((ImTextureID) (intptr_t) ic.tex_id, ImVec2(sz - 2.f * pad, sz - 2.f * pad), ic.tl, ic.br,
                                            int(pad));
        } else {
            clicked = ImGui::Button(label.ToUTF8().data(), ImVec2(0.f, sz)); // the control is never lost to a missing icon
        }
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            ImGui::GetColorU32(active ? orca : col_frame), style.FrameRounding);
        ImGui::PopStyleColor(na ? 3 : 1);
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        if (ImGui::IsItemHovered())
            panel_tooltip(na ? tip_text + "\n\n" + unavailable : tip_text, media);
        return clicked && !na;
    };
    // Borderless icon button (non-toggle): always the grey monochrome variant.
    const auto icon_button = [&](int uid, const std::string &iconfile, float sz, const wxString &label,
                                 const wxString &tip_text, const std::vector<TipMedia> &media = {}) -> bool {
        const auto  it      = m_panel_icon_map.find(iconfile);
        const float pad     = std::max(1.f, std::round(sz * 0.14f));
        bool        clicked = false;
        ImGui::PushID(uid);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        if (it != m_panel_icon_map.end() && !it->second.empty() && it->second[0]->is_valid()) {
            const IconManager::Icon &ic = *it->second[0];
            clicked = m_imgui->image_button((ImTextureID) (intptr_t) ic.tex_id, ImVec2(sz - 2.f * pad, sz - 2.f * pad), ic.tl, ic.br,
                                            int(pad));
        } else {
            clicked = ImGui::Button(label.ToUTF8().data(), ImVec2(0.f, sz));
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopID();
        if (!tip_text.empty() && ImGui::IsItemHovered())
            panel_tooltip(tip_text, media);
        return clicked;
    };
    // A short vertical rule between groups of buttons on one row; call it right after an item.
    const auto vsep = [&](float h) {
        ImGui::SameLine(0.f, gap_s);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x, p.y + h * 0.8f), col_sep);
        ImGui::Dummy(ImVec2(1.f, h));
        ImGui::SameLine(0.f, gap_s);
    };
    const auto heading = [&](const wxString &text) {
        m_imgui->push_bold_font();
        m_imgui->text(text);
        m_imgui->pop_bold_font();
    };
    // Label column + a slider filling the rest of the row, value printed inside the track.
    // `right_inset` keeps a layer card's inner padding.
    const auto slider_label = [&](const wxString &label) {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
    };

    // ---- Typed values on sliders ----
    // Every slider in the panel goes through typed_slider(), so all of them take a number the same
    // two ways: Ctrl+click on the track, or the edit icon at the right end for the same box without
    // the modifier. Both end up in ImGui's own "temp input" mode - the icon only asks ImGui to focus
    // the slider, which is what turns it into an input box (see SliderScalar) - so the parsing,
    // the clamping and the Enter/click-away behaviour are ImGui's, not a second implementation.

    // Retires a click whose row is no longer being drawn - the panel changed mode in between - so that
    // it cannot fire later when that row comes back. Before any row is drawn, so a live one still wins.
    if (!m_slider_type_request.empty() && ImGui::GetFrameCount() > m_slider_type_request_frame + 1)
        m_slider_type_request.clear();

    // Exactly as tall as the slider it sits beside, so it lines up with the track without the row
    // having to be re-centred around it; icon_button() pads the glyph itself down to about 0.7 of this.
    const float edit_icon_w = frame_h;
    // What the slider's own width has to give up for the icon beside it.
    const float edit_icon_room = edit_icon_w + gap_s;

    // One end of a range, through the slider's own format, so the bounds in the tooltip read exactly
    // like the value in the track. A parenthesised note in the format - "(auto)" - is dropped: it
    // describes the current state, not the bound.
    const auto fmt_bound = [](const char *format, auto v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), format, v);
        std::string out(buf);
        const size_t paren = out.find('(');
        if (paren != std::string::npos)
            out.erase(paren);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    };
    const auto range_text = [](const std::string &lo, const std::string &hi) {
        return Slic3r::format(_u8L("Range: %1% to %2%"), lo, hi);
    };
    const auto edit_icon_tip = [&](const std::string &range) {
        return _u8L("Type a value instead of dragging. Ctrl+click on the slider does the same.") + "\n\n" + range;
    };

    // Draws `slider` at `item_w` (SetNextItemWidth's convention: negative measures back from the right
    // edge, and the caller has already left edit_icon_room in it), then the edit icon. The range goes on
    // both tooltips; `tip` and its illustrations `media` on the slider's. Returns whatever the slider returned.
    const auto typed_row = [&](const char *id, float item_w, const wxString &tip, const std::vector<TipMedia> &media,
                               const std::string &range, const std::function<bool()> &slider) -> bool {
        // Deferred from the click a frame ago: SetKeyboardFocusHere() is a request for the *next*
        // frame, so it has to be issued before the slider it names, not from the icon beside it.
        if (m_slider_type_request == id) {
            ImGui::SetKeyboardFocusHere();
            m_slider_type_request.clear();
            m_parent.set_as_dirty(); // the focus only lands on a frame that actually gets drawn
        }
        ImGui::SetNextItemWidth(item_w);
        const bool changed = slider();
        // AllowWhenDisabled: several of these rows are drawn inside disabled_begin(), and a greyed
        // slider is exactly when the user needs to be told what it is and why it is not moving.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            wxString full = tip;
            if (!full.IsEmpty())
                full += "\n\n";
            panel_tooltip(full + from_u8(range), media);
        }
        // SameLine() puts the cursor back at the slider's own top (ItemSize records the Y the item was
        // drawn at), so an icon of the same height lands on the track wherever the row placed it.
        ImGui::SameLine(0.f, gap_s);
        if (icon_button(int(ImGui::GetID(id)), "edit.svg", edit_icon_w, _L("Edit"), from_u8(edit_icon_tip(range)))) {
            m_slider_type_request       = id;
            m_slider_type_request_frame = ImGui::GetFrameCount();
            m_parent.set_as_dirty(); // the next frame is where the request above is turned into focus
        }
        return changed;
    };

    // The two forms every slider in the panel is one of. They derive the range from the bounds they are
    // already given, so no row states its bounds or its format twice and the tooltip cannot drift from
    // the track. `item_w` is as typed_row() takes it.
    const auto typed_slider = [&](const char *id, float item_w, float *v, float v_min, float v_max, const char *format,
                                  bool log, const wxString &tip, const std::vector<TipMedia> &media = {}) -> bool {
        return typed_row(id, item_w, tip, media, range_text(fmt_bound(format, v_min), fmt_bound(format, v_max)), [&] {
            return ImGui::SliderFloat(id, v, v_min, v_max, format,
                                      ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
        });
    };
    const auto typed_slider_int = [&](const char *id, float item_w, int *v, int v_min, int v_max, const char *format,
                                      bool log, const wxString &tip, const std::vector<TipMedia> &media = {}) -> bool {
        return typed_row(id, item_w, tip, media, range_text(fmt_bound(format, v_min), fmt_bound(format, v_max)), [&] {
            return ImGui::SliderInt(id, v, v_min, v_max, format,
                                    ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
        });
    };

    // Label column + the slider filling the rest of the row.
    const auto float_row = [&](const char *id, const wxString &label, float *v, float v_min, float v_max, const char *format,
                               bool log, float right_inset, const wxString &tip, const std::vector<TipMedia> &media = {}) -> bool {
        slider_label(label);
        return typed_slider(id, -(std::max(right_inset, 1.f) + edit_icon_room), v, v_min, v_max, format, log, tip, media);
    };
    const auto int_row = [&](const char *id, const wxString &label, int *v, int v_min, int v_max, const char *format,
                             float right_inset, const wxString &tip, const std::vector<TipMedia> &media = {}) -> bool {
        slider_label(label);
        return typed_slider_int(id, -(std::max(right_inset, 1.f) + edit_icon_room), v, v_min, v_max, format, false, tip, media);
    };
    const auto layer_name = [](const TextureDisplacementLayer &l) {
        return l.name.empty() ? Slic3r::format(_u8L("Layer %1%"), l.slot + 1) : l.name;
    };
    // Painted on any model part. The selectors flush into the model at the end of every stroke.
    const auto slot_painted = [mo](int slot) {
        for (const ModelVolume *v : mo->volumes)
            if (v->is_model_part() && !v->texture_displacement_facet(slot).empty())
                return true;
        return false;
    };

    // ---- Header: title, Standard / Pro, dock toggle. Pinned above the body. ----
    {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        heading(_L("Texture displacement"));

        const std::string labels[2] = { _u8L_CONTEXT("Standard", "Mode"), _u8L("Pro") };
        const float       seg_pad   = m_imgui->scaled(0.5f);
        const float seg_w[2] = { ImGui::CalcTextSize(labels[0].c_str()).x + 2.f * seg_pad, ImGui::CalcTextSize(labels[1].c_str()).x + 2.f * seg_pad };
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + panel_w - (seg_w[0] + seg_w[1] + gap_s + icon_sm)));

        // Standard / Pro is a mode, not an option: Standard hides every mesh-preparation control and folds
        // the whole recipe into Bake, Pro shows all of it and hands the ordering to the user.
        const ImVec2 seg_min = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, style.ItemSpacing.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        m_imgui->disabled_begin(busy);
        for (int m = 0; m < 2; ++m) {
            if (m > 0)
                ImGui::SameLine();
            const bool on = m_panel_mode == m;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? orca : ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? orca : ImVec4(orca.x, orca.y, orca.z, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, orca);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1.f, 1.f, 1.f, 1.f) : style.Colors[ImGuiCol_TextDisabled]);
            const bool clicked = ImGui::Button((labels[m] + "##panel_mode").c_str(), ImVec2(seg_w[m], frame_h));
            ImGui::PopStyleColor(4);
            hover_tip(m == 0 ? _u8L("Standard - pick a texture, paint where it goes, press Bake. Everything the "
                                   "mesh needs is done for you in that one step.") :
                               _u8L("Pro - the mesh preparation is yours to run: Remesh, Subdivide and Bake as "
                                   "separate steps, with every setting on show. For when Standard's result is "
                                   "not what you wanted and you know why."));
            if (clicked && !on) {
                m_panel_mode = m;
                store_panel_state();
                if (!pro_mode()) {
                    // Leaving the subdivision preview open would strand a wireframe whose controls just
                    // disappeared, so close it as part of the switch.
                    m_subdivide_editing      = false;
                    m_subdivide_preview_tris = -1;
                    m_subdivide_preview_glmodel.reset();
                    if (apply_standard_mode_presets(mv))
                        m_preview_params_dirty = true;
                }
                m_parent.set_as_dirty();
            }
        }
        m_imgui->disabled_end();
        ImGui::PopStyleVar(2);
        ImGui::GetWindowDrawList()->AddRect(seg_min, ImVec2(ImGui::GetItemRectMax().x, seg_min.y + frame_h),
                                            ImGui::GetColorU32(col_frame), style.FrameRounding);

        ImGui::SameLine(0.f, gap_s);
        if (icon_button(806, "canvas_drag.svg", icon_sm, m_undocked ? _L("Dock panel") : _L("Undock panel"),
                        _L("Detach this panel so it can be dragged anywhere over the 3D view, or dock it back beside "
                           "the toolbar.")))
            m_undocked = !m_undocked;
            wxGetApp().app_config->set_bool(UNDOCKED_CONFIG_KEY, m_undocked);
    }
    ImGui::Separator();

    // ---- Body: everything between the header and the footer, scrolling as one. ----
    // Sized from last frame's content and footer heights (see m_panel_body_h): as tall as its content,
    // but never so tall that the footer drops below the bottom of the canvas.
    const float bottom     = m_undocked ? ImGui::GetIO().DisplaySize.y : bottom_limit;
    const float max_body_h = std::max(m_imgui->scaled(8.f), bottom - ImGui::GetCursorScreenPos().y - m_panel_footer_h -
                                                                 style.WindowPadding.y - style.ItemSpacing.y);
    const float body_h     = m_panel_body_h > 0.f ? std::min(m_panel_body_h + 1.f, max_body_h) : max_body_h;

    push_slim_scrollbar_style(m_imgui->scaled(0.5f), dark);
    // NoScrollWithMouse: ease_wheel_scroll() eases the scroll instead of teleporting five text lines per
    // notch, which on blocks this tall lost the reader's place.
    ImGui::BeginChild("##td_body", ImVec2(panel_w, body_h), false, ImGuiWindowFlags_NoScrollWithMouse);
    ease_wheel_scroll(m_panel_scroll_target, m_panel_scroll_applied);

    // Everything that changes the layer list is deferred until the list is no longer being drawn:
    // removing or reordering shifts mv->texture_displacement_layers under the pointers the loop holds,
    // and switching the active layer mid-loop would draw two cards open for a frame.
    int slot_to_remove = -1;
    int move_slot      = -1;
    int move_to        = -1;
    int activate_slot  = -1;

    TextureDisplacementLayer *active = mv != nullptr ? active_layer() : nullptr;

    // ---- Paint: which layer strokes land in, and Paint / Erase ----
    {
        ImGui::AlignTextToFramePadding();
        heading(_L("Paint"));
        const std::string into      = active != nullptr ? Slic3r::format(_u8L("into %1%"), layer_name(*active)) :
                                                          _u8L("add a layer to paint");
        const float       toggles_x = ImGui::GetWindowContentRegionMax().x - (2.f * icon_sm + gap_s);
        const float       text_end  = toggles_x - (2.f * gap_s + 1.f);
        ImGui::SameLine();
        const std::string shown = ellipsize(into, std::max(0.f, text_end - ImGui::GetCursorPosX()));
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), text_end - ImGui::CalcTextSize(shown.c_str()).x));
        ImGui::TextDisabled("%s", shown.c_str());
        vsep(icon_sm);
        ImGui::SetCursorPosX(toggles_x);
        if (icon_toggle(820, "texture_displacement_add.svg", !m_erase_mode, icon_sm, _L("Paint"),
                        _L("Paint - add the active layer where you click or drag. The right button erases, and so does "
                           "holding Shift."),
                        { { "paint.gif" } }))
            m_erase_mode = false;
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(821, "texture_displacement_add_negative.svg", m_erase_mode, icon_sm, _L("Erase"),
                        _L("Erase - remove the active layer where you click or drag. The right button paints."),
                        { { "erase.gif" } }))
            m_erase_mode = true;
    }

    // ---- Tools: brush / face / connected area, then the whole-model actions, then the active tool's own
    // control filling the rest of the row ----
    // "Face" and "Connected area" reuse the exact same selection machinery every other paint gizmo has
    // (single-facet click, and angle-limited flood fill respectively).
    {
        const bool is_brush_mode = m_tool_type == ToolType::BRUSH && m_cursor_type != TriangleSelector::CursorType::POINTER;
        const bool is_face_mode  = m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER;
        const bool is_area_mode  = m_tool_type == ToolType::SMART_FILL;
        const float row_y        = ImGui::GetCursorPosY();
        if (icon_toggle(801, "texture_displacement_brush.svg", is_brush_mode, icon_md, _L("Brush"),
                        _L("Brush - paint over the surface by dragging"), { { "paint.gif" } })) {
            m_tool_type = ToolType::BRUSH;
            if (m_cursor_type == TriangleSelector::CursorType::POINTER)
                m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        }
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(802, "texture_displacement_face.svg", is_face_mode, icon_md, _L("Face"), _L("Face - click individual triangles"),
                        { { "tool_face.gif" } })) {
            m_tool_type   = ToolType::BRUSH;
            m_cursor_type = TriangleSelector::CursorType::POINTER;
        }
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(803, "texture_displacement_connected_area.svg", is_area_mode, icon_md, _L("Connected area"),
                        _L("Connected area - flood-fill the region reachable without crossing an edge sharper than the "
                           "angle threshold"),
                        { { "tool_smart_fill.gif" } })) {
            m_tool_type   = ToolType::SMART_FILL;
            m_cursor_type = TriangleSelector::CursorType::POINTER;
        }

        // Whole model: paint every face with the active layer, or clear its paint from all of them.
        const wxString whole_na = busy               ? _L("Wait for the bake to finish.") :
                                  active == nullptr  ? _L("Add a layer first.") :
                                                       wxString();
        const wxString erase_na = !whole_na.empty()                     ? whole_na :
                                  !slot_painted(m_active_layer_slot) ? _L("The active layer has no paint yet.") :
                                                                          wxString();
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(806, "texture_displacement_select_all.svg", false, icon_md, _L("Select whole model"),
                        _L("Select whole model - paint every face of the model with the active layer"),
                        { { "select_all.png" } }, whole_na))
            select_whole_model();
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(807, "texture_displacement_erase_all.svg", false, icon_md, _L("Erase whole model"),
                        _L("Erase whole model - clear the active layer's paint from every face"),
                        { { "erase_all.png" } }, erase_na)) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset texture displacement selection"),
                                          UndoRedo::SnapshotType::GizmoAction);
            int idx = -1;
            for (ModelVolume *v : mo->volumes)
                if (v->is_model_part()) {
                    ++idx;
                    m_triangle_selectors[idx]->reset();
                    m_triangle_selectors[idx]->request_update_render_data(true);
                }
            update_model_object();
            m_parent.set_as_dirty();
        }

        // The active tool's control fills the rest of the row, each part centred on the (taller) tool icons.
        const float row_end       = ImGui::GetWindowContentRegionMax().x;
        const auto  centre_on_row = [&](float h) { ImGui::SetCursorPosY(row_y + std::round((icon_md - h) * 0.5f)); };
        vsep(icon_md);
        if (is_brush_mode) {
            centre_on_row(frame_h);
            typed_slider("##cursor_radius",
                         std::max(1.f, row_end - ImGui::GetCursorPosX() - (3.f * gap_s + 1.f + 2.f * icon_sm) - edit_icon_room),
                         &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f mm", false, m_desc.at("cursor_size"),
                         { { "cursor_size.gif" } });
            vsep(icon_sm);
            const bool is_circle = m_cursor_type == TriangleSelector::CursorType::CIRCLE;
            centre_on_row(icon_sm);
            if (icon_toggle(804, "circle_paint.svg", is_circle, icon_sm, m_desc.at("circle"),
                            _L("Circle - paints everything under the brush as seen from the camera")))
                m_cursor_type = TriangleSelector::CursorType::CIRCLE;
            ImGui::SameLine(0.f, gap_s);
            centre_on_row(icon_sm);
            if (icon_toggle(805, "menu_obj_sphere.svg", !is_circle, icon_sm, m_desc.at("sphere"),
                            _L("Sphere - paints only within a ball around the point under the cursor")))
                m_cursor_type = TriangleSelector::CursorType::SPHERE;
        } else if (is_area_mode) {
            centre_on_row(frame_h);
            typed_slider("##smart_fill_angle", std::max(1.f, row_end - ImGui::GetCursorPosX() - edit_icon_room),
                         &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f°", false,
                         _L("Angle threshold - the fill stops at edges sharper than this"),
                         { { "smart_fill_angle_low.png", _L("Low") }, { "smart_fill_angle_high.png", _L("High") } });
        } else {
            centre_on_row(frame_h);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", ellipsize(_u8L("Click a triangle to paint it"),
                                                std::max(0.f, row_end - ImGui::GetCursorPosX())).c_str());
        }
    }

    // ---- View: Normal / Fast / Checker / Distortion as one group, Wireframe on its own ----
    // The underlying state stays m_use_shaded_preview + m_uv_check_mode.
    {
        const int cur_mode = view_mode();
        int  new_mode       = cur_mode;
        int  picked         = -1; // the button clicked, the active one included
        bool wf_toggle      = false;
        bool open_uv_editor = false;
        // Checker and Distortion both draw *the unwrap* - the first the texture grid laid over it, the second
        // its stretch - so they only mean anything for a layer mapped with Unwrap (LSCM). On the default
        // triplanar mapping (or cylindrical / spherical / from view) they are faded out with the reason in the
        // tooltip, rather than being offered and then showing nothing.
        const wxString uv_view_na = active == nullptr ? _L("Add a layer first.") :
                                    active->projection_method != TextureProjectionMethod::LSCM ?
                                                        _L("Only for a layer mapped with Unwrap (LSCM) - set the "
                                                           "active layer's Mapping to Unwrap to use this view.") :
                                                        wxString();
        // Either view over a layer that stopped being an unwrap shows nothing at all, so fall back to Normal.
        if ((cur_mode == 2 || cur_mode == 3) && !uv_view_na.empty())
            new_mode = 0;

        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", _u8L("View").c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + m_imgui->scaled(2.6f)));
        if (icon_toggle(701, "texture_displacement_real_preview.svg", cur_mode == 0, icon_md, _L("Normal"),
                        _L("Normal - the real geometry, exactly what Bake will produce. Slower to update"),
                        { { "tile_on.png" } }))
            new_mode = picked = 0;
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(702, "texture_displacement_fast_preview.svg", cur_mode == 1, icon_md, _L("Fast"),
                        _L("Fast - the relief is only shaded on, not built, and only for the layer you are "
                           "editing. Updates instantly while you paint"),
                        { { "view_fast.png" } }))
            new_mode = picked = 1;
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(703, "texture_displacement_checker.svg", cur_mode == 2, icon_md, _L("Checker"),
                        _L("Checker - a test grid instead of the texture. Where the squares stay square the "
                           "texture is undistorted; where they stretch, it will too. Opens the UV editor if "
                           "it is closed"),
                        { { "view_checker.png" } }, uv_view_na)) {
            new_mode       = picked = 2;
            open_uv_editor = true;
        }
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(704, "texture_displacement_distortion.svg", cur_mode == 3, icon_md, _L("Distortion"),
                        _L("Distortion - blue-to-red stretch heatmap over the unwrap. Opens the UV editor if "
                           "it is closed"),
                        { { "view_distortion.png" } }, uv_view_na)) {
            new_mode       = picked = 3;
            open_uv_editor = true;
        }
        vsep(icon_md);
        if (icon_toggle(705, "texture_displacement_wireframe.svg", m_wireframe_overlay, icon_md, _L("Wireframe"),
                        _L("Wireframe - overlay the mesh edges; independent of the view above"),
                        { { "view_wireframe.png" } }))
            wf_toggle = true;

        const std::string auto_label = _u8L("Auto");
        const float       auto_w     = frame_h * 0.78f /*ratio from BBLCheckbox*/ + style.ItemInnerSpacing.x + ImGui::CalcTextSize(auto_label.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - auto_w));
        if (m_imgui->bbl_checkbox(wxString::FromUTF8(auto_label) + "##auto_update", m_auto_update) && m_auto_update)
            rebuild_preview(); // catch up anything that changed while it was off
        hover_tip(_u8L("Rebuilds the preview as soon as anything changes. Turn it off on a heavy model if painting "
                        "or dragging a slider starts to stutter - the preview then waits until you let go."));

        // Both are views of the unwrap, so picking one brings the UV editor up with it - including when that
        // view is already the active one and only the pane is missing.
        if (open_uv_editor && !m_show_uv_editor) {
            m_show_uv_editor = true;
            if (new_mode == cur_mode)
                update_uv_editor(); // otherwise apply_view_mode() below does it
        }
        if (picked >= 0)
            m_island_view.picked(picked);
        if (new_mode != cur_mode)
            apply_view_mode(new_mode);
        if (wf_toggle) {
            m_wireframe_overlay = !m_wireframe_overlay;
            refresh_wireframe();
            m_parent.set_as_dirty();
        }
    }

    ImGui::Separator();
    // ---- Texture layers: the active layer as an open card, every other one as a single row ----
    {
        ImGui::AlignTextToFramePadding();
        heading(_L("Texture layers"));
        if (mv != nullptr) {
            const std::string count = std::to_string(mv->texture_displacement_layers.size()) + " / " +
                                      std::to_string(TEXTURE_DISPLACEMENT_MAX_LAYERS);

            const std::string add_label = "+  " + _u8L("Add layer");

            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(count.c_str()).x - ImGui::CalcTextSize(add_label.c_str()).x) - (style.FrameBorderSize + style.FramePadding.x) * 2.f - style.ItemSpacing.x);
            ImGui::TextDisabled("%s", count.c_str());

            ImGui::SameLine();
            const bool full = mv->texture_displacement_layers.size() >= TEXTURE_DISPLACEMENT_MAX_LAYERS;
            //Slic3r::format(into_u8(_L_PLURAL("All %1% layer used", "All %1% layers used", unsigned(TEXTURE_DISPLACEMENT_MAX_LAYERS))), TEXTURE_DISPLACEMENT_MAX_LAYERS)
            m_imgui->disabled_begin(busy || full);
            if (ImGui::Button((add_label + "##add_layer").c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.f)))
                add_texture_layer();
            m_imgui->disabled_end();
        }
    }

    std::vector<TextureDisplacementLayer *> ordered;
    if (mv != nullptr) {
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            ordered.push_back(&l);
        std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) { return a->slot < b->slot; });
    }

    ImDrawList *dl         = ImGui::GetWindowDrawList();
    const float content_rx = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float rounding   = style.FrameRounding + 1.f;

    // Opens the texture library for a layer, making it the active one too (after the loop).
    const auto open_picker = [&](int slot) {
        m_picker_slot            = slot;
        m_picker_open_request    = true;
        m_picker_reveal_selected = true;
    };
    // The six-dot drag icon, drawn into `list` over [x, x + w] x [y, y + h].
    const auto draw_drag_icon = [&](ImDrawList *list, float x, float y, float w, float h, ImU32 tint) {
        const auto it = m_panel_icon_map.find("texture_displacement_drag.svg");
        if (it != m_panel_icon_map.end() && !it->second.empty() && it->second[0]->is_valid()) {
            // The icon's dots sit in the middle of its square, so it is drawn at the row's height centred on the
            // narrow hit area; only its transparent margins overhang the neighbouring widgets.
            const IconManager::Icon &ic = *it->second[0];
            const float              cx = x + 0.5f * w;
            list->AddImage((ImTextureID) (intptr_t) ic.tex_id, ImVec2(cx - 0.5f * h, y), ImVec2(cx + 0.5f * h, y + h), ic.tl, ic.br, tint);
        } else {
            const float r = std::max(1.f, std::round(m_imgui->scaled(0.08f)));
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 2; ++col)
                    list->AddCircleFilled(ImVec2(x + w * (0.3f + 0.4f * col), y + h * 0.5f + float(row - 1) * 4.f * r), r, tint);
        }
    };
    const auto drag_tint = [dark](int alpha) { return dark ? IM_COL32(255, 255, 255, alpha) : IM_COL32(90, 90, 90, alpha); };

    const float grip_w = std::round(m_imgui->scaled(0.7f));
    // Six-dot handle that drags a layer onto another one to reorder the stack.
    const auto grip = [&](const TextureDisplacementLayer &l, float h) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##grip", ImVec2(grip_w, h));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        draw_drag_icon(dl, p.x, p.y, grip_w, h, drag_tint(hot ? 255 : 150));
        hover_tip(_u8L("Drag onto another layer to reorder. The first layer sits on the bare surface; each one after "
                        "it adds its relief on top of the ones before."));
        // ImGui's own preview is a tooltip holding whatever is submitted here. The panel draws a copy of the whole
        // layer row under the cursor instead (after the layer list), so that one is switched off.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            const int slot = l.slot;
            ImGui::SetDragDropPayload("TD_LAYER", &slot, sizeof(slot));
            ImGui::EndDragDropSource();
        }
    };

    // The layer being dragged, while a reorder drag is in flight. Its card stays in place as a faded ghost.
    int dragged_slot = -1;
    if (const ImGuiPayload *payload = ImGui::GetDragDropPayload(); payload != nullptr && payload->IsDataType("TD_LAYER"))
        dragged_slot = *static_cast<const int *>(payload->Data);
    int dragged_index = -1;
    for (size_t i = 0; i < ordered.size(); ++i)
        if (ordered[i]->slot == dragged_slot)
            dragged_index = int(i);
    const ImU32 ghost_col = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.65f);

    // A layer card or row [mn, mx] as a drop target. Dropped anywhere on another layer, the dragged one takes its
    // place and the layers in between shift over by one - so it lands above the target when moving up and below it
    // when moving down. A teal line on that side shows where; ImGui's default highlight rectangle says only
    // "something is here", so it is replaced by the line.
    //
    // Deciding by which half of the target the cursor is over instead made dropping onto a neighbour's near half a
    // silent no-op ("after the layer I am already after"), and that is exactly where a drag to the top ends.
    const auto drop_target = [&](size_t index, int slot, const ImVec2 &mn, const ImVec2 &mx) {
        if (!ImGui::BeginDragDropTarget())
            return;
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("TD_LAYER", ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                                                 ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const int  from  = *static_cast<const int *>(payload->Data);
            const bool after = dragged_index >= 0 && dragged_index < int(index);
            if (from != slot) {
                const float y = after ? mx.y + 2.f : mn.y - 2.f;
                dl->AddLine(ImVec2(mn.x, y), ImVec2(mx.x, y), ImGui::GetColorU32(orca), 2.f);
            }
            if (payload->IsDelivery()) {
                move_slot = from;
                move_to   = int(index) + (after ? 1 : 0);
            }
        }
        ImGui::EndDragDropTarget();
    };

    for (size_t li = 0; li < ordered.size(); ++li) {
        TextureDisplacementLayer &layer     = *ordered[li];
        const bool                is_active = layer.slot == m_active_layer_slot;
        const bool                painted   = slot_painted(layer.slot);
        ImGui::PushID(layer.slot);

        if (is_active) {
            // The card's height is not known until it is laid out, so its content goes into a foreground
            // channel and the backing rectangle into a background one, merged at the end - the standard
            // ImGui "rect behind a group" trick.
            const ImVec2 card_min = ImGui::GetCursorScreenPos();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + card_pad);
            ImGui::Indent(card_pad);
            ImGui::BeginGroup();

            // Header: grip, thumbnail and name (both open the library), paint state, order, remove.
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap_s, style.ItemSpacing.y));
                grip(layer, frame_h);
                ImGui::SameLine();
                if (GLTexture *thumb = get_layer_thumbnail(layer)) {
                    if (ImGui::ImageButton((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(frame_h, frame_h), ImVec2(0.f, 0.f),
                                           ImVec2(1.f, 1.f), 0))
                        open_picker(layer.slot);
                } else if (ImGui::Button("?##thumb", ImVec2(frame_h, frame_h))) {
                    open_picker(layer.slot);
                }
                hover_tip(_u8L("Pick a different image for this layer. The paint, depth and tiling stay as they are."));
                {
                    // The teal outline on the thumbnail is what marks this card as the layer being painted.
                    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    dl->AddRect(ImVec2(a.x - 2.f, a.y - 2.f), ImVec2(b.x + 2.f, b.y + 2.f), ImGui::GetColorU32(orca), style.FrameRounding);
                }
                ImGui::SameLine();

                const std::string not_painted = _u8L("not painted");
                const float right_w = (painted ? 0.f : ImGui::CalcTextSize(not_painted.c_str()).x + gap_s) + 4.f * frame_h + 3.f * gap_s;
                const float name_w  = std::max(frame_h, ImGui::GetWindowContentRegionMax().x - card_pad - ImGui::GetCursorPosX() - gap_s - right_w);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(orca.x, orca.y, orca.z, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(orca.x, orca.y, orca.z, 0.35f));
                ImGui::AlignTextToFramePadding();
                const std::string shown = ellipsize(layer_name(layer), name_w - 2.f * style.FramePadding.x);
                if (ImGui::Selectable((shown + "##name").c_str(), false, 0, ImVec2(name_w, frame_h)))
                    open_picker(layer.slot);
                ImGui::PopStyleColor(2);
                hover_tip(_u8L("Pick a different image for this layer. The paint, depth and tiling stay as they are."));
                if (!painted) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", not_painted.c_str());
                }
                // Between the name and the order arrows: it acts on the image the name refers to, so it
                // belongs with the name rather than with the controls that reorder the stack.
                ImGui::SameLine();
                m_imgui->disabled_begin(busy || layer.empty());
                if (icon_button(613 + layer.slot, "cog.svg", frame_h, _L("Adjust image"),
                                _u8L("Brightness, contrast, sharpness, blur and hue, shown on the image and on the "
                                     "model as you set them. Save as copy writes the result to your texture library "
                                     "and points this layer at it.")))
                    open_image_adjust(layer);
                m_imgui->disabled_end();
                ImGui::SameLine();
                m_imgui->disabled_begin(busy || li == 0);
                if (icon_button(611, "texture_displacement_move_up.svg", frame_h, _L("Move up"), _L("Move up - applied earlier"))) {
                    move_slot = layer.slot;
                    move_to   = int(li) - 1;
                }
                m_imgui->disabled_end();
                ImGui::SameLine();
                m_imgui->disabled_begin(busy || li + 1 >= ordered.size());
                if (icon_button(612, "texture_displacement_move_down.svg", frame_h, _L("Move down"), _L("Move down - applied later"))) {
                    move_slot = layer.slot;
                    move_to   = int(li) + 2;
                }
                m_imgui->disabled_end();
                ImGui::SameLine();
                m_imgui->disabled_begin(busy);
                if (icon_button(600 + layer.slot, "texture_displacement_cross.svg", frame_h, m_desc.at("remove_layer"),
                                _L("Remove this layer")))
                    slot_to_remove = layer.slot;
                m_imgui->disabled_end();
                ImGui::PopStyleVar();
            }

            // The three controls nearly every layer needs; everything else waits behind "More settings".
            // Depth and tile size are logarithmic, so both ends of their wide ranges stay usable.
            m_preview_params_dirty |= float_row("##depth", _L("Depth"), &layer.depth_mm, 0.01f, 10.f, "%.3f mm", true, card_pad,
                                                _L("Height of the relief: how far white in the texture lifts the surface, in "
                                                   "millimetres. Black does not move it at all, unless Midlevel says otherwise."),
                                                { { "depth.gif" } });
            // Mapped by the model's own UV layout there is no millimetre scale to set: the layout is the
            // placement (see TextureProjectionMethod::ModelUV), so the slider would move nothing. The row
            // states why rather than hiding: float_row()'s tooltip is shown for a disabled slider too.
            const bool tile_size_na = layer.projection_method == TextureProjectionMethod::ModelUV;
            m_imgui->disabled_begin(tile_size_na);
            m_preview_params_dirty |= float_row("##tile_size", _L("Tile size"), &layer.tiling_scale, 0.2f, 200.f, "%.2f mm", true, card_pad,
                                                tile_size_na ?
                                                    _L("Mapped by the model's own UV layout, which is already the placement - one "
                                                       "copy of the texture covers it exactly, so there is no tile size to set.") :
                                                    _L("How wide one copy of the texture is on the model. Smaller repeats the pattern "
                                                       "more often and makes its detail finer; with Tile off, this is the size of the "
                                                       "single copy."),
                                                tile_size_na ? std::vector<TipMedia>{} :
                                                               std::vector<TipMedia>{ { "tile_size_large.png", _L("Larger") },
                                                                                      { "tile_size_small.png", _L("Smaller") } });
            m_imgui->disabled_end();
            m_preview_params_dirty |= float_row("##rotation", _L("Rotation"), &layer.rotation_deg, 0.f, 360.f, "%.0f°", false, card_pad,
                                                _L("Turns the texture on the surface, in degrees - for lining a pattern up with an "
                                                   "edge of the model."),
                                                { { "rotation.gif" } });

            if (m_layer_expanded[size_t(layer.slot)]) {
                {
                    // A dashed rule between the basic and the advanced settings.
                    const ImVec2 p    = ImGui::GetCursorScreenPos();
                    const float  x1   = content_rx - card_pad;
                    const float  dash = std::round(m_imgui->scaled(0.25f));
                    for (float xx = p.x; xx < x1; xx += 2.f * dash)
                        dl->AddLine(ImVec2(xx, p.y), ImVec2(std::min(xx + dash, x1), p.y), col_line);
                    ImGui::Dummy(ImVec2(1.f, 1.f));
                }

                // Midlevel: the height that means "don't move". At 0 the surface only ever bulges outwards;
                // at 0.5 mid-grey is neutral and darker texels cut inwards.
                m_preview_params_dirty |= float_row("##midlevel", _L("Midlevel"), &layer.midlevel, 0.f, 10.f, "%.2f", false, card_pad,
                                                    _L("Which gray stays where the surface already is. At 0 the texture only pushes "
                                                       "outwards; at 0.5 mid-gray stays put, so darker grays cut in and lighter ones "
                                                       "still push out - one image both embosses and engraves.\n\n"
                                                       "What cuts in has to fit: inside a sharp corner or through a thin wall, a deep "
                                                       "cut can pass through the other side."),
                                                    { { "midlevel.gif" } });
                if (layer.midlevel > 0.f && layer.depth_mm > 1.f) {
                    ImGui::PushTextWrapPos(content_rx - card_pad);
                    m_imgui->warning_text(_L("Deep inward displacement may self-intersect."));
                    ImGui::PopTextWrapPos();
                }

                m_preview_params_dirty |= float_row("##smoothing", _L("Smoothing"), &layer.smoothing, 0.f, 1.f, "%.2f", false, card_pad,
                                                    _L("Blurs the image before it is used, which rounds off hard steps and removes "
                                                       "speckle from a noisy photo. Raise it if the relief looks harsh or grainy; it "
                                                       "costs fine detail."),
                                                    { { "smoothing_off.png", _L("Off") }, { "smoothing_on.png", _L("On") } });

                // Edge fade: the relief flattens toward the boundary of the painted area.
                m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Edge fade") + "##edge_smoothing", layer.edge_smoothing);
                hover_tip(_u8L("Flattens the relief as it approaches the edge of the painted area, so it "
                               "blends into the bare surface instead of stopping at a step."),
                          { { "edge_fade_off.png", _L("Off") }, { "edge_fade_on.png", _L("On") } });
                ImGui::SameLine();
                m_imgui->disabled_begin(!layer.edge_smoothing);
                m_preview_params_dirty |= typed_slider("##edge_amount", -(card_pad + edit_icon_room),
                                                       &layer.edge_smoothing_amount, 0.02f, 1.f, "%.2f", false,
                                                       _L("How far in the fade reaches, as a share of the painted area. Small "
                                                          "values soften a narrow band at the edge; 1 flattens almost all of it."));
                m_imgui->disabled_end();

                // Invert and Colours share a row.
                {
                    const float x0 = ImGui::GetCursorPosX();
                    m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Invert"), layer.invert);
                    hover_tip(_u8L("Turns the relief inside out: what stood out is cut in, and the other way "
                                   "round. The same as using a negative of the image."),
                              { { "invert_off.png", _L("Off") }, { "invert_on.png", _L("On") } });
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + 0.5f * (ImGui::GetWindowContentRegionMax().x - card_pad - x0)));

                    // Only offered for a texture that actually has colour - the shipped library is grayscale,
                    // and a checkbox that silently does nothing on nine textures out of ten is worse than no
                    // checkbox. Disabled rather than hidden so it is clear the feature exists and what it wants.
                    const bool has_color     = decode_height_texture(layer).has_color();
                    bool       color_enabled = layer.color_enabled && has_color;
                    m_imgui->disabled_begin(!has_color);
                    if (m_imgui->bbl_checkbox(_L("Colors"), color_enabled)) {
                        layer.color_enabled    = color_enabled;
                        m_preview_params_dirty = true;
                    }
                    m_imgui->disabled_end();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        panel_tooltip(has_color ?
                                          _u8L("Prints the painted area in the texture's colors as well as its "
                                               "relief. Each color is matched to the nearest of your loaded "
                                               "filaments; anything you did not paint keeps the object's own.") :
                                          _u8L("This texture is a grayscale height map, so it has no colors to "
                                               "apply. Import a color image to use this."));

                    // The rest of colour belongs to the whole stack, not to this layer, so it only appears once -
                    // under whichever layer turned colour on.
                    if (color_enabled) {
                        TextureDisplacementOptions &opts = mv->texture_displacement_options;
                        if (m_imgui->bbl_checkbox(_L("Mix filaments"), opts.color_mix_enabled))
                            m_preview_params_dirty = true;
                        hover_tip(_u8L("Interleaves two filaments to fake the colors in between, so a handful "
                                       "of filaments can cover a photo or a gradient. An image of flat colors "
                                       "prints the same either way. Off uses one filament per area."),
                                  { { "mix_off.png", _L("Off") }, { "mix_on.png", _L("On") } });
                        if (opts.color_mix_enabled) {
                            cached_palette(); // brings m_palette_filaments up to date
                            // Every mix can become a filament slot, and there are only so many beside the
                            // physical filaments.
                            const int max_mixes = std::max(1, int(EnforcerBlockerType::ExtruderMax) - int(m_palette_filaments.size()));
                            if (int_row("##color_mix_count", _L("Mixed colors"), &opts.color_mix_count, 1, max_mixes, "%d", card_pad,
                                        _L("The most mixed filaments a bake may add. They are picked from the "
                                           "texture's colors, and only the ones the bake actually uses are created."),
                                        { { "mixed_colors_few.png", _L("Few") }, { "mixed_colors_many.png", _L("Many") } }))
                                m_preview_params_dirty = true;
                            // After the slider, so a change shows in the same frame.
                            const int n_colors    = int(cached_palette().size());
                            const int n_filaments = int(m_palette_filaments.size());
                            // TRN The %2% part of "%1% printable colors %2%"; %1% is the number of filaments.
                            const std::string from_filaments = Slic3r::format(into_u8(_L_PLURAL("from %1% filament", "from %1% filaments", n_filaments)), n_filaments);
                            // TRN %1% is the number of printable colors, %2% is "from N filaments".
                            ImGui::TextDisabled("%s", Slic3r::format(into_u8(_L_PLURAL("%1% printable color %2%", "%1% printable colors %2%", n_colors)),
                                                                     n_colors, from_filaments).c_str());
                        }
                        if (int_row("##color_despeckle", _L("Denoise"), &opts.color_despeckle, 0, 6, "%d", card_pad,
                                    _L("Cleans up single stray triangles of the wrong color, which detail finer "
                                       "than the mesh leaves behind. Raise it if the result looks speckled, "
                                       "lower it if small features are being swallowed."),
                                    { { "denoise_off.png", _L("Off") }, { "denoise_on.png", _L("On") } }))
                            m_preview_params_dirty = true;
                    }
                }

                // Mapping, as one icon per TextureProjectionMethod, in the enum's own order.
                {
                    const float x0 = ImGui::GetCursorPosX();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", _u8L_CONTEXT("Mapping", "Texture Displacement").c_str());
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    struct MappingIcon
                    {
                        const char *file;
                        wxString    label;
                        wxString    tip;
                        const char *picture; // the same ball with the checker laid on it this way, or null for none
                    };
                    const MappingIcon mappings[] = {
                        { "menu_obj_cube.svg", _L("Triplanar (blended)"),
                          _L("Triplanar - projects the texture from all three axes at once and blends between them, so a "
                             "patch wrapping around a sharp edge has no seam."),
                          "map_cube.png" },
                        { "menu_obj_cylinder.svg", _L("Cylindrical"),
                          _L("Cylindrical - wraps the texture around the painted area's own center, for round shapes."),
                          "map_cylinder.png" },
                        { "menu_obj_sphere.svg", _L("Spherical"),
                          _L("Spherical - wraps the texture around the painted area's own center in both directions."),
                          "map_sphere.png" },
                        { "texture_displacement_map_unwrap.svg", _L("Unwrap (LSCM)"),
                          _L("Unwrap - flattens the painted area and maps the texture onto it with as little stretching as "
                             "possible. The area is cut into pieces at its sharp edges first (see Seam angle), so each "
                             "piece can lie flat on its own."),
                          "map_unwrap.png" },
                        { "texture_displacement_map_view.svg", _L("From view"),
                          _L("From view - projects straight onto the painted area from where you are looking, like a slide "
                             "projector."),
                          "map_view.png" },
                        { "texture_displacement_map_model_uv.svg", _L("Model UV"),
                          _L("Model UV - uses the texture coordinates the model itself came with, so a texture made for "
                             "that model lands exactly where it was painted. Nothing is unwrapped, and Tile size does "
                             "not apply: the model's own layout is the placement."),
                          nullptr },
                    };
                    // Only a volume that actually carries a UV channel can be mapped by it. Shown
                    // greyed out rather than hidden, so the reason is visible instead of the control
                    // simply being missing.
                    const wxString model_uv_na =
                        (mv != nullptr && mv->has_texture_uv()) ?
                            wxString() :
                            _L("This model carries no texture coordinates of its own. Only a model imported with a UV "
                               "channel - a textured OBJ, say - can be mapped by them.");
                    for (int mi = 0; mi < int(IM_ARRAYSIZE(mappings)); ++mi) {
                        if (mi > 0)
                            ImGui::SameLine(0.f, gap_s);
                        const auto method = static_cast<TextureProjectionMethod>(mi);
                        const wxString na = (method == TextureProjectionMethod::ModelUV) ? model_uv_na : wxString();
                        const std::vector<TipMedia> picture = mappings[mi].picture != nullptr ?
                                                                  std::vector<TipMedia>{ { mappings[mi].picture } } :
                                                                  std::vector<TipMedia>{};
                        if (icon_toggle(830 + mi, mappings[mi].file, layer.projection_method == method, icon_sm, mappings[mi].label,
                                        mappings[mi].tip, picture, na) &&
                            layer.projection_method != method) {
                            // Capture the current view the moment "From view" is chosen, so it does something
                            // sensible immediately rather than projecting from a stale/default direction.
                            if (method == TextureProjectionMethod::ViewProjected)
                                capture_view_projection(layer);
                            layer.projection_method = method;
                            m_preview_params_dirty  = true;
                            // Unwrap and its tools live in the UV editor pane, so picking it opens the pane
                            // (via the preview rebuild this triggers).
                            if (method == TextureProjectionMethod::LSCM)
                                m_show_uv_editor = true;
                        }
                    }
                }

                if (layer.projection_method == TextureProjectionMethod::LSCM) {
                    // Unwrap and everything that edits it live in the UV editor pane, next to the islands they change.
                    // Picking this mapping opens the pane; this brings it back after it has been closed.
                    if (m_imgui->button(_u8L("Open UV editor"))) {
                        m_show_uv_editor = true;
                        update_uv_editor();
                    }
                    hover_tip(_u8L("Opens the UV editor, where you can see how the texture is laid out over the "
                                    "painted area, cut seams and move the pieces around."));
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    if (m_uv_editor_unwrap.empty())
                        ImGui::TextDisabled("%s", _u8L("Not unwrapped yet").c_str());
                    else
                        ImGui::TextDisabled("%s", Slic3r::format(into_u8(_L_PLURAL("%1% island", "%1% islands", m_uv_editor_unwrap.chart_count)),
                                                                 m_uv_editor_unwrap.chart_count).c_str());
                }

                if (layer.projection_method == TextureProjectionMethod::ViewProjected) {
                    // Re-capture the projector from wherever the camera is now: orbit the model, press this, and
                    // the texture is re-laid from the new angle.
                    if (m_imgui->button(_u8L("Capture current view"))) {
                        capture_view_projection(layer);
                        // Selecting the visible faces *after* capturing means the projector axes are already the
                        // ones the selection was made against - the two describe the same viewpoint.
                        if (m_project_only_visible && select_visible_faces() == 0)
                            show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                     "part you want to project onto."));
                        m_preview_params_dirty = true;
                        update_projector();
                    }
                    hover_tip(_u8L("Projects the texture onto the painted area from where you are looking now, like "
                                    "a slide projector. Faces turned away from you stretch, so line the view up with "
                                    "the surface you care about first."));

                    if (m_imgui->bbl_checkbox(_L("Project only on visible"), m_project_only_visible)) {
                        if (m_project_only_visible && select_visible_faces() == 0)
                            show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                     "part you want to project onto."));
                        m_preview_params_dirty = true;
                        update_projector();
                    }
                    hover_tip(_u8L("Paints only the faces you can actually see right now - facing you and not hidden "
                                    "behind anything - and projects onto those. Replaces what the layer had painted."));

                    bool projector_open = m_projector_frame != nullptr && m_projector_frame->IsShown();
                    if (m_imgui->bbl_checkbox(_L("Projection frame"), projector_open))
                        show_projector(projector_open);
                    hover_tip(_u8L("Opens a window you drag over the model. Whatever you can see through it is what "
                                    "gets the texture, and its border becomes the edge of the projection."));

                    if (projector_open) {
                        if (int_row("##projector_opacity", _L("Opacity"), &m_projector_opacity, 20, 255, "%d", card_pad,
                                    _L("How solid the frame window looks while you place it. Lower to see the "
                                       "model through it; it does not affect the result.")))
                            m_projector_frame->set_opacity(m_projector_opacity);

                        if (m_imgui->button(_u8L("Apply projection frame"))) {
                            const int painted_count = apply_projection_frame();
                            if (painted_count == 0)
                                show_error(nullptr, _u8L("Nothing of the model is inside the frame - move it over the "
                                                         "part you want to project onto."));
                            else if (painted_count < 0)
                                show_error(nullptr, _u8L("The frame could not be applied. Make sure it overlaps the 3D view."));
                        }
                        hover_tip(_u8L("Projects the texture through the frame from where you are looking now and "
                                        "paints the faces inside it. Replaces what the layer had painted; the result "
                                        "sticks to the model, so you can orbit afterwards."));
                    }

                    if (layer.view_project_projective) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", _u8L("Placed by projection frame.").c_str());
                        ImGui::SameLine();
                        if (m_imgui->button(_u8L("Clear"))) {
                            // Back to the plain axis projection, where tiling/rotation/offset mean something again -
                            // the matrix path deliberately ignores them.
                            layer.view_project_projective = false;
                            layer.tile_enabled            = true;
                            m_preview_params_dirty        = true;
                        }
                    }
                }

                // Tile, and how it repeats.
                {
                    const float x0 = ImGui::GetCursorPosX();
                    m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Tile") + "##tile_enabled", layer.tile_enabled);
                    hover_tip(_u8L("Repeats the texture across the painted area. Off places one copy, like a decal, "
                                    "at the size set by Tile size."),
                              { { "tile_off.png", _L("Off") }, { "tile_on.png", _L("On") } });
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    const wxString tile_na = layer.tile_enabled ? wxString() : _L("Turn Tile on to choose how the texture repeats.");
                    if (icon_toggle(840, "texture_displacement_tile_repeat.svg", layer.tile_method == static_cast<TextureTileMethod>(0),
                                    icon_sm, _L("Repeat"), _L("Repeat"), { { "tile_repeat.png" } }, tile_na)) {
                        layer.tile_method      = static_cast<TextureTileMethod>(0);
                        m_preview_params_dirty = true;
                    }
                    ImGui::SameLine(0.f, gap_s);
                    if (icon_toggle(841, "menu_mirror_x.svg", layer.tile_method == static_cast<TextureTileMethod>(1), icon_sm,
                                    _L("Mirrored repeat"), _L("Mirrored repeat - every other tile is flipped, so edges meet seamlessly"),
                                    { { "tile_mirror.png" } }, tile_na)) {
                        layer.tile_method      = static_cast<TextureTileMethod>(1);
                        m_preview_params_dirty = true;
                    }
                }

                // Blend, and placement on the model.
                {
                    const float x0 = ImGui::GetCursorPosX();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", _u8L("Blend").c_str());
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    const std::string blend_add      = _u8L_CONTEXT("Add", "Texture Displacement");
                    const std::string blend_override  = _u8L_CONTEXT("Override", "Texture Displacement");
                    const std::string blend_subtract = _u8L_CONTEXT("Subtract", "Texture Displacement");
                    const std::string blend_multiply = _u8L_CONTEXT("Multiply", "Texture Displacement");
                    const std::string blend_divide   = _u8L("Divide");
                    const char *blend_items[] = { blend_add.c_str(), blend_override.c_str(), blend_subtract.c_str(),
                                                  blend_multiply.c_str(), blend_divide.c_str() };
                    // The combo's order is the panel's, not the enum's - Override reads as the sibling of Add and
                    // belongs next to it, while TextureBlendMode's numbering is fixed by what projects already
                    // store. Hence the explicit mapping rather than a cast of the index.
                    static const TextureBlendMode blend_modes[] = { TextureBlendMode::Add, TextureBlendMode::Override,
                                                                    TextureBlendMode::Subtract, TextureBlendMode::Multiply,
                                                                    TextureBlendMode::Divide };
                    static_assert(IM_ARRAYSIZE(blend_items) == IM_ARRAYSIZE(blend_modes),
                                  "every Blend entry needs its mode");
                    // The first layer has nothing before it to combine with - build_texture_displacement() makes it
                    // add regardless - so its combo shows Add and cannot be changed.
                    const bool base_layer = li == 0;
                    int        blend_mode = 0;
                    if (!base_layer)
                        for (int bi = 0; bi < int(IM_ARRAYSIZE(blend_modes)); ++bi)
                            if (blend_modes[bi] == layer.blend_mode)
                                blend_mode = bi;
                    ImGui::SetNextItemWidth(-(card_pad + gap_s + icon_sm));
                    m_imgui->disabled_begin(base_layer);
                    if (scoped_combo("##blend_mode", &blend_mode, blend_items, IM_ARRAYSIZE(blend_items)) && !base_layer &&
                        blend_mode >= 0 && blend_mode < int(IM_ARRAYSIZE(blend_modes))) {
                        layer.blend_mode       = blend_modes[blend_mode];
                        m_preview_params_dirty = true;
                    }
                    m_imgui->disabled_end();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        panel_tooltip(base_layer ?
                                          _u8L("The base layer has nothing beneath it to combine with, so it always adds.") :
                                          _u8L("What this layer does where it overlaps the ones below. Add and "
                                               "Subtract pile relief on or carve it away. Override replaces them, "
                                               "so only this layer's relief is left where it is painted. Multiply "
                                               "and Divide scale what is underneath, which turns this layer into a "
                                               "mask over it - there, Depth acts as a strength, and 1 mm leaves "
                                               "white areas untouched."),
                                      base_layer ? std::vector<TipMedia>{} :
                                                   std::vector<TipMedia>{ { "blend_add.png", from_u8(blend_add) },
                                                                          { "blend_multiply.png", from_u8(blend_multiply) } });

                    ImGui::SameLine(0.f, gap_s);
                    if (icon_toggle(842, "texture_displacement_adjust.svg", m_adjust_texture_mode, icon_sm, _L("Adjust placement"),
                                    _L("Adjust placement - drag the handle on the model to move the texture"))) {
                        if (!m_adjust_texture_mode) {
                            update_model_object(); // flush any pending strokes before anchoring
                            if (update_adjust_anchor())
                                m_adjust_texture_mode = true;
                            else
                                show_error(nullptr, _u8L("Paint something with this layer first."));
                        } else {
                            m_adjust_texture_mode = false;
                            m_adjust_drag_handle  = AdjustHandle::None;
                        }
                    }
                }
            }

            // "More settings" / "Fewer settings", drawn as a link.
            {
                const bool        expanded = m_layer_expanded[size_t(layer.slot)];
                const std::string text     = expanded ? _u8L("Fewer settings") : _u8L("More settings");
                const float       arrow    = std::round(ImGui::GetFontSize() * 0.55f);
                const ImVec2      ts       = ImGui::CalcTextSize(text.c_str());
                const ImVec2      p        = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##more", ImVec2(arrow + gap_s + ts.x, ts.y))) {
                    m_layer_expanded[size_t(layer.slot)] = !expanded;
                    // Placement is one of the settings being hidden, so it stops with them.
                    if (expanded && m_adjust_texture_mode) {
                        m_adjust_texture_mode = false;
                        m_adjust_drag_handle  = AdjustHandle::None;
                    }
                }
                const ImU32 c  = ImGui::GetColorU32(col_link);
                const float cx = p.x + 0.5f * arrow, cy = p.y + 0.5f * ts.y, hs = 0.3f * arrow;
                if (expanded)
                    dl->AddTriangleFilled(ImVec2(cx - hs, cy + 0.5f * hs), ImVec2(cx + hs, cy + 0.5f * hs), ImVec2(cx, cy - 0.5f * hs), c);
                else
                    dl->AddTriangleFilled(ImVec2(cx - hs, cy - 0.5f * hs), ImVec2(cx + hs, cy - 0.5f * hs), ImVec2(cx, cy + 0.5f * hs), c);
                const float tx = p.x + arrow + gap_s;
                dl->AddText(ImVec2(tx, p.y), c, text.c_str());
                if (ImGui::IsItemHovered())
                    dl->AddLine(ImVec2(tx, p.y + ts.y), ImVec2(tx + ts.x, p.y + ts.y), c);
            }

            ImGui::EndGroup();
            ImGui::Unindent(card_pad);
            const float card_max_y = ImGui::GetItemRectMax().y + card_pad;
            drop_target(li, layer.slot, card_min, ImVec2(content_rx, card_max_y));
            if (layer.slot == dragged_slot)
                dl->AddRectFilled(card_min, ImVec2(content_rx, card_max_y), ghost_col, rounding);
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(card_min, ImVec2(content_rx, card_max_y), col_card, rounding);
            dl->ChannelsMerge();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + card_pad);
        } else {
            // A closed layer: one outlined row - grip, chevron, thumbnail, name, depth, remove. A click
            // anywhere that is not one of its buttons opens it.
            const float  inner = std::round(m_imgui->scaled(0.15f));
            const float  row_h = frame_h + 2.f * inner;
            const ImVec2 p0    = ImGui::GetCursorScreenPos();
            const float  row_w = content_rx - p0.x;
            if (ImGui::InvisibleButton("##row", ImVec2(row_w, row_h)))
                activate_slot = layer.slot;
            const bool row_hovered = ImGui::IsItemHovered();
            ImGui::SetItemAllowOverlap(); // the row's own buttons, submitted after it, still take their clicks
            drop_target(li, layer.slot, p0, ImVec2(p0.x + row_w, p0.y + row_h));
            if (row_hovered)
                dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), col_card, rounding);
            dl->AddRect(p0, ImVec2(p0.x + row_w, p0.y + row_h), col_line, rounding);

            ImGui::SetCursorScreenPos(ImVec2(p0.x + inner, p0.y + inner));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap_s, style.ItemSpacing.y));
            grip(layer, frame_h);
            ImGui::SameLine();
            {
                const ImVec2 c  = ImGui::GetCursorScreenPos();
                const float  aw = std::round(ImGui::GetFontSize() * 0.5f), hs = 0.3f * aw;
                const float  cx = c.x + 0.5f * aw, cy = c.y + 0.5f * frame_h;
                dl->AddTriangleFilled(ImVec2(cx - 0.5f * hs, cy - hs), ImVec2(cx - 0.5f * hs, cy + hs), ImVec2(cx + 0.5f * hs, cy),
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled));
                ImGui::Dummy(ImVec2(aw, frame_h));
            }
            ImGui::SameLine();
            const float th = std::round(frame_h * 0.9f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 0.5f * (frame_h - th));
            if (GLTexture *thumb = get_layer_thumbnail(layer)) {
                if (ImGui::ImageButton((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(th, th), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), 0))
                    open_picker(layer.slot);
            } else if (ImGui::Button("?##thumb", ImVec2(th, th))) {
                open_picker(layer.slot);
            }
            hover_tip(_u8L("Pick a different image for this layer. The paint, depth and tiling stay as they are."));
            ImGui::SameLine();

            char depth_text[32];
            std::snprintf(depth_text, sizeof(depth_text), "%.3f mm", layer.depth_mm);
            const std::string summary    = painted ? std::string(depth_text) : _u8L("not painted");
            const float       sum_w      = ImGui::CalcTextSize(summary.c_str()).x;
            const float       right_edge = ImGui::GetWindowContentRegionMax().x - inner;
            const float       name_w     = std::max(0.f, right_edge - ImGui::GetCursorPosX() - 2.f * gap_s - sum_w - frame_h);
            ImGui::SetCursorPosY(p0.y - ImGui::GetWindowPos().y + ImGui::GetScrollY() + inner);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ellipsize(layer_name(layer), name_w).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right_edge - frame_h - gap_s - sum_w));
            ImGui::TextDisabled("%s", summary.c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_edge - frame_h);
            m_imgui->disabled_begin(busy);
            if (icon_button(600 + layer.slot, "texture_displacement_cross.svg", frame_h, m_desc.at("remove_layer"), _L("Remove this layer")))
                slot_to_remove = layer.slot;
            m_imgui->disabled_end();
            ImGui::PopStyleVar();
            if (layer.slot == dragged_slot)
                dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), ghost_col, rounding);
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + row_h + 0.5f * style.ItemSpacing.y));
        }

        ImGui::PopID();
    }

    // The dragged layer follows the cursor as a copy of its row, held by its grip. On the foreground draw list, so
    // it stays visible over the rest of the panel and anywhere the drag wanders.
    if (dragged_slot >= 0) {
        const auto it = std::find_if(ordered.begin(), ordered.end(), [dragged_slot](const auto *l) { return l->slot == dragged_slot; });
        if (it != ordered.end()) {
            const TextureDisplacementLayer &l      = **it;
            ImDrawList                     *fg     = ImGui::GetForegroundDrawList();
            const float                     inner  = std::round(m_imgui->scaled(0.15f));
            const float                     row_h  = frame_h + 2.f * inner;
            const float                     list_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
            const ImVec2                    mouse  = ImGui::GetIO().MousePos;
            const ImVec2                    mn(mouse.x - inner - 0.5f * grip_w, mouse.y - 0.5f * row_h);
            const ImVec2                    mx(mn.x + (content_rx - list_x), mn.y + row_h);
            fg->AddRectFilled(ImVec2(mn.x + 2.f, mn.y + 4.f), ImVec2(mx.x + 2.f, mx.y + 4.f), IM_COL32(0, 0, 0, 70), rounding);
            fg->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_WindowBg, 245.f / 255.f), rounding);
            fg->AddRect(mn, mx, ImGui::GetColorU32(orca), rounding, 0, 1.5f);

            float x = mn.x + inner;
            draw_drag_icon(fg, x, mn.y + inner, grip_w, frame_h, drag_tint(255));
            x += grip_w + gap_s;
            const float th = std::round(frame_h * 0.9f);
            if (GLTexture *thumb = get_layer_thumbnail(l)) {
                const float ty = mn.y + 0.5f * (row_h - th);
                fg->AddImage((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(x, ty), ImVec2(x + th, ty + th));
            }
            x += th + gap_s;

            char depth_text[32];
            std::snprintf(depth_text, sizeof(depth_text), "%.3f mm", l.depth_mm);
            const std::string summary = slot_painted(l.slot) ? std::string(depth_text) : _u8L("not painted");
            const float       sum_w   = ImGui::CalcTextSize(summary.c_str()).x;
            const float       text_y  = mn.y + 0.5f * (row_h - ImGui::GetFontSize());
            fg->AddText(ImVec2(mx.x - inner - sum_w, text_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), summary.c_str());
            const std::string name = ellipsize(layer_name(l), std::max(0.f, mx.x - inner - sum_w - gap_s - x));
            fg->AddText(ImVec2(x, text_y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        }
    }

    // ---- Mesh preparation: Pro only. Standard pins all of it and runs it from Bake. ----
    if (pro_mode()) {
        ImGui::Separator();
        heading(_L("Subdivision"));

        if (m_imgui->bbl_checkbox(_L("Only painted area (adaptive)"), m_subdivide_adaptive)) {
            if (m_subdivide_editing)
                rebuild_subdivide_preview(); // switch the wireframe between the uniform and adaptive result
            m_parent.set_as_dirty();
        }
        hover_tip(_u8L("Adds triangles only where you painted, instead of everywhere. On a large part with a small "
                        "decal that is the difference between thousands of triangles and millions, and your paint "
                        "survives the refinement."),
                  { { "painted_only_off.png", _L("Off") }, { "painted_only_on.png", _L("On") } });

        if (m_subdivide_adaptive) {
            if (m_subdivide_target_mm <= 0.f && mv != nullptr) {
                // Seed the target at about half the mesh's mean edge length, so the default already adds a
                // useful amount of detail rather than landing on "no change".
                const indexed_triangle_set &its = mv->mesh().its;
                double                      sum = 0.0;
                size_t                      cnt = 0;
                for (const stl_triangle_vertex_indices &tri : its.indices)
                    for (int i = 0; i < 3; ++i) {
                        sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm();
                        ++cnt;
                    }
                m_subdivide_target_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)) * 0.5f, 0.001f, 20.f) : 1.f;
            }
            // Live preview: rebuild the wireframe as the slider moves, not only on release. The rebuild is
            // bounded by the painted region, so it stays responsive.
            const auto preview_live = [this]() {
                if (m_subdivide_editing)
                    rebuild_subdivide_preview();
                m_parent.set_as_dirty();
            };

            if (m_imgui->bbl_checkbox(_L("Follow texture detail"), m_subdivide_feature))
                preview_live();
            hover_tip(_u8L("Spends the triangles where the texture actually bends - packed along ridges and edges, "
                            "sparse over flat ground - instead of spreading them evenly. The same detail for fewer "
                            "triangles on most textures."),
                      { { "adaptive_off.png", _L("Off") }, { "adaptive_on.png", _L("On") } });

            // The edge-length target is a baseline in both modes. In feature mode it is what guarantees the
            // curvature test can actually see the texture: left too coarse, a big triangle over a fine
            // pattern can sample four points that all happen to land at similar heights, report no error,
            // and stall before refinement ever starts.
            if (float_row("##subdiv_target", m_subdivide_feature ? _L("Max edge") : _L("Target edge"), &m_subdivide_target_mm,
                          0.001f, 20.f, "%.3f mm", true, 0.f,
                          m_subdivide_feature ?
                              _L("No triangle in the painted area stays larger than this, even where the texture "
                                 "is flat. Keep it near the size of the smallest feature you want to come out.") :
                              _L("Triangles in the painted area are split until none is larger than this. Smaller "
                                 "means finer detail and, fast, a lot more triangles.")))
                preview_live();

            if (m_subdivide_feature) {
                if (float_row("##subdiv_detail", _L("Detail"), &m_subdivide_detail_mm, 0.001f, 1.f, "%.3f mm", true, 0.f,
                              _L("How far the mesh may sit from the shape the texture describes, in millimetres. "
                                 "Smaller follows fine detail and costs triangles; larger only chases the big "
                                 "features.")))
                    preview_live();
                if (float_row("##subdiv_min", _L("Min edge"), &m_subdivide_min_edge_mm, 0.001f, 20.f, "%.3f mm", true, 0.f,
                              _L("A floor on triangle size, so a sharp step in the texture cannot be chased "
                                 "forever. Lower it for finer relief, raise it if the count runs away at hard "
                                 "edges.")))
                    preview_live();
            }

            // Applies in both adaptive sub-modes: it is about the step the bake puts at the paint's edge, which
            // exists whether or not "Follow texture detail" is on. 0 turns the band off.
            if (float_row("##subdiv_border", _L("Edge detail"), &m_subdivide_border_mm, 0.f, 5.f, "%.3f mm", false, 0.f,
                          _L("Triangle size along the outline of the painted area, where the relief drops back to the "
                             "bare surface. Lower it if that rim looks jagged; it only costs triangles along the "
                             "outline. 0 turns it off.")))
                preview_live();

            // Only worth showing when a layer is actually colouring: with no colour there is no boundary for it
            // to refine and the control would do nothing whatever it is set to.
            if (mv != nullptr && any_layer_colors(*mv)) {
                if (float_row("##subdiv_color", _L("Color detail"), &m_subdivide_color_mm, 0.f, 5.f, "%.3f mm", false, 0.f,
                              _L("Triangle size where two colors meet. Each triangle prints in one filament, so a "
                                 "color edge can only be as sharp as the triangles along it - and nothing else "
                                 "refines there, since the surface is flat across a change of color. 0 turns it off.")))
                    preview_live();
            }

            ImGui::TextDisabled("%s", _u8L("Triangle budget: set with Triangles, below.").c_str());
            if (m_subdivide_editing && m_subdivide_preview_tris > 0)
                m_imgui->text(Slic3r::format(into_u8(_L_PLURAL("Preview: %1% triangle", "Preview: %1% triangles", m_subdivide_preview_tris)), m_subdivide_preview_tris));
        } else {
            if (int_row("##subdiv_steps", _L("Steps"), &m_subdivide_count, 0, 5, "%d", 0.f,
                        _L("Splits every triangle of the model into four, this many times over. Each step "
                           "quadruples the count - 3 turns 100 k triangles into 6.4 M - so prefer refining "
                           "only the painted area unless you need the whole model finer."),
                        { { "subdivide.gif" } })) {
                if (m_subdivide_editing)
                    rebuild_subdivide_preview(); // a count of 0 clears the preview, it doesn't compute one
                m_parent.set_as_dirty();
            }
        }

        {
            // Subdivide is a no-op when there is nothing to commit: 0 uniform passes, or an adaptive target that
            // is not set.
            const bool  subdivide_ready = m_subdivide_adaptive ? (m_subdivide_target_mm > 0.f) : (m_subdivide_count >= 1);
            const float half            = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f);
            m_imgui->disabled_begin(busy);
            if (ImGui::Button((m_subdivide_editing ? _u8L("Hide preview") : _u8L("Preview subdivision")).c_str(), ImVec2(half, 0.f))) {
                if (m_subdivide_editing) {
                    m_subdivide_editing      = false;
                    m_subdivide_preview_tris = -1;
                    m_subdivide_preview_glmodel.reset();
                } else {
                    m_subdivide_editing = true;
                    rebuild_subdivide_preview();
                }
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            hover_tip(_u8L("Shows what the refinement would produce as a wireframe, without touching the model."));
            ImGui::SameLine();
            m_imgui->disabled_begin(!subdivide_ready || busy);
            if (ImGui::Button(_u8L("Subdivide").c_str(), ImVec2(half, 0.f))) {
                if (m_subdivide_adaptive) {
                    subdivide_model_adaptive(); // refines only the painted area, carrying the paint forward
                } else {
                    subdivide_model(); // commits m_subdivide_count passes (takes its own snapshot)
                    // Back to 0 rather than staying at the count just applied: the mesh is now up to 4^N times
                    // denser, so re-previewing the same N passes on top of it is both pointless and by far the
                    // slowest thing this panel does.
                    m_subdivide_count = 0;
                }
                if (m_subdivide_editing)
                    rebuild_subdivide_preview();
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            hover_tip(m_subdivide_adaptive ?
                          _u8L("Refines the painted area to the target edge length and carries your paint onto "
                               "the finer mesh. The rest of the model is left as it is.") :
                          _u8L("Replaces the model's geometry with the subdivided mesh and clears any not-yet-baked "
                               "paint on it (already-baked relief is unaffected)."));
        }

        // Remesh: even out uneven triangle sizes (CGAL isotropic remeshing).
        ImGui::Separator();
        heading(_L("Remeshing"));
        if (m_remesh_target_edge_mm <= 0.f && mv != nullptr) {
            // Seed the target with the model's current mean edge length, so the default is a sensible "make
            // everything about the size it already averages".
            const indexed_triangle_set &its = mv->mesh().its;
            double                      sum = 0.0;
            size_t                      cnt = 0;
            for (const stl_triangle_vertex_indices &tri : its.indices)
                for (int i = 0; i < 3; ++i) {
                    sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm();
                    ++cnt;
                }
            m_remesh_target_edge_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)), 0.1f, 20.f) : 1.f;
        }
        float_row("##remesh_edge", _L("Target edge"), &m_remesh_target_edge_mm, 0.1f, 20.f, "%.2f mm", true, 0.f,
                  _L("Triangle size the whole model is rebuilt with, in millimetres. Displacement works best on an "
                     "even mesh; this is what makes one out of an uneven import."),
                  { { "remesh_before.png", _L("Before") }, { "remesh_after.png", _L("After") } });

        m_imgui->bbl_checkbox(_L("Keep sharp edges") + "##remesh_sharp", m_remesh_keep_sharp_edges);
        hover_tip(_u8L("Holds hard edges and open borders in place while the rest is remeshed. Without it "
                       "the remesher slides vertices along the surface and rounds every crisp edge off - "
                       "a cube comes back with wobbly edges."),
                  { { "preserve_edges_off.png", _L("Off") }, { "preserve_edges_on.png", _L("On") } });
        ImGui::SameLine();
        m_imgui->disabled_begin(!m_remesh_keep_sharp_edges);
        typed_slider("##remesh_sharp_angle", -(1.f + edit_icon_room), &m_remesh_sharp_angle_deg, 5.f, 90.f, "%.0f°", false,
                     _L("How sharp a fold has to be, in degrees, before the remesher treats it as an edge worth "
                        "keeping. Lower protects more of the model's shape; higher rebuilds more of it evenly."));
        m_imgui->disabled_end();

        m_imgui->disabled_begin(mv == nullptr || busy);
        if (ImGui::Button(_u8L("Remesh").c_str()))
            remesh_model();
        m_imgui->disabled_end();
        hover_tip(_u8L("Rebuilds the whole model with triangles close to this edge length - splitting the big "
                       "ones and merging the small ones - so displacement has an even density to work with. "
                       "Replaces the geometry; your paint is carried onto the new triangles spatially, so it "
                       "survives (already-baked relief is kept too)."),
                  { { "remesh_before.png", _L("Before") }, { "remesh_after.png", _L("After") } });

        // Settings for the whole stack rather than one layer. Standard mode pins them instead of showing them.
        if (mv != nullptr) {
            TextureDisplacementOptions &opts = mv->texture_displacement_options;
            ImGui::Separator();
            heading(_L("Result"));

            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Displace up to the border"), opts.displace_border);
            hover_tip(_u8L("Lets the relief run right to the edge of the painted area. Turn it off to hold that "
                            "outer ring flat, which keeps the displacement strictly inside your paint but flattens "
                            "the pattern at the border."));

            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Smooth result"), opts.smooth_enabled);
            hover_tip(_u8L("Smooths the geometry after the texture has been applied, to take the hard steps out of a "
                            "low-resolution image. Only what the displacement moved is touched. The Smoothing slider "
                            "on a layer is a different thing: it blurs the image before it is used."),
                      { { "geometry_smoothing.gif" } });
            if (opts.smooth_enabled) {
                float percent = opts.smooth_strength * 100.f;
                if (float_row("##dispsmooth", _L_CONTEXT("Strength", "Texture Displacement"), &percent, 1.f, 100.f, "%.0f %%", false, 0.f,
                              _L("How far each pass pulls a vertex towards its neighbours. High values round the "
                                 "relief off quickly; low values need more passes but keep more of the detail."))) {
                    opts.smooth_strength   = std::clamp(percent / 100.f, 0.01f, 1.f);
                    m_preview_params_dirty = true;
                }
                if (int_row("##dispsmoothit", _L("Passes"), &opts.smooth_iterations, 1, 10, "%d", 0.f,
                            _L("How many smoothing passes to run. More passes spread the smoothing further across "
                               "the surface; Strength decides how much each one moves.")))
                    m_preview_params_dirty = true;

                m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Ignore outer ring"), opts.smooth_skip_border);
                hover_tip(_u8L("Keeps the outer ring of the painted area out of the smoothing. Its neighbours "
                                "outside the paint never move, so smoothing it drags the relief down and leaves the "
                                "pattern half-melted at the border."));

                // The settings above ride along with Preview/Bake. This button is for geometry that has *already*
                // been baked, where there is no displacement left to fold the smoothing into.
                if (m_imgui->button(_u8L("Smooth baked mesh now")))
                    smooth_model();
                hover_tip(_u8L("Applies the smoothing above to the model itself, right now, within the painted area. "
                                "For relief that is already baked in - anything not yet baked is smoothed by Bake."));
            }
        }
    }

    // Shown in both modes. The pipeline switch, the two v2 experiments and the stage debugger are
    // developer controls: kept, but off the panel unless this is flipped.
    static constexpr bool SHOW_PIPELINE_DEV_CONTROLS = false;
    if (mv != nullptr) {
        TextureDisplacementOptions &opts = mv->texture_displacement_options;
        ImGui::Separator();
        if (SHOW_PIPELINE_DEV_CONTROLS) {
            // The classic path is the opt-in: the one-run pipeline is the default, and its resolution
            // control lives in the footer next to Bake (see below).
            bool classic = !opts.pipeline_v2;
            if (m_imgui->bbl_checkbox(_L("Experimental: classic bake pipeline"), classic)) {
                opts.pipeline_v2       = !classic;
                m_preview_params_dirty = true;
            }
            hover_tip(_u8L("Bake by moving the vertices the mesh already has, after preparing it (remesh, "
                           "adaptive subdivision, and a cut along sharp steps in the texture). Keeps the "
                           "topology, which is what colors need. The default pipeline instead refines, "
                           "cleans up sliver triangles, displaces and simplifies in one run; nothing needs "
                           "preparing first, but it does not produce colors yet."));
        }
        if (SHOW_PIPELINE_DEV_CONTROLS && opts.pipeline_v2) {
            // Keeping the relief above the plate is not a checkbox: it is unconditional, in both pipelines
            // (see build_texture_displacement()).
            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Align mesh to texture edges"), opts.v2_relocate);
            hover_tip(_u8L("Slide vertices sideways onto the edges in the texture before displacing them. "
                           "Displacement can only move vertices up and down, so without this a sharp step "
                           "in the image lands wherever the triangles happen to be and comes out as a "
                           "staircase. Moving the vertices onto the step first gives a straight wall at the "
                           "same triangle count."));
            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Clean up slivers"), opts.v2_regularize);
            hover_tip(_u8L("Collapse the thin triangles refinement inherits from the model's own "
                           "tessellation, before displacement samples them. A sliver's three corners "
                           "land on three unrelated parts of the texture, which is what makes the "
                           "relief look jagged."),
                      { { "collapse_slivers_off.png", _L("Off") }, { "collapse_slivers_on.png", _L("On") } });
        }
        // Below both pipelines' own settings, because it replays whichever of them is selected.
        if (SHOW_PIPELINE_DEV_CONTROLS)
            render_debug_stage_panel(mv);
    }

    // Content height, for next frame's body size. The cursor position is scroll-compensated, so this is the
    // height of everything above, whether or not it is scrolled.
    m_panel_body_h = ImGui::GetCursorPosY() - style.ItemSpacing.y;
    ImGui::EndChild();
    pop_slim_scrollbar_style();

    // The layer-list changes deferred from the loop above.
    if (slot_to_remove >= 0)
        remove_texture_layer(slot_to_remove);
    else if (move_slot >= 0)
        move_texture_layer(move_slot, move_to);
    else if (activate_slot >= 0)
        set_active_layer(activate_slot);
    // Retexturing a layer makes it the one being painted, too.
    if (m_picker_open_request && m_picker_slot >= 0)
        set_active_layer(m_picker_slot);

    // In this window's scope, where the popup's open request and the popup itself share one ID stack.
    ImVec2 panel_rect_min, panel_rect_max;
    {
        const ImVec2 win_min = ImGui::GetWindowPos();
        const ImVec2 win_sz  = ImGui::GetWindowSize();
        render_texture_library_popup(win_min, ImVec2(win_min.x + win_sz.x, win_min.y + win_sz.y));
        // Only recorded here. The image-adjustment panel is a top-level window rather than a popup, so it
        // cannot be begun inside this one; it is drawn after GizmoImguiEnd() below.
        panel_rect_min = win_min;
        panel_rect_max = ImVec2(win_min.x + win_sz.x, win_min.y + win_sz.y);
    }

    // ImGui sliders report "changed" continuously on every frame while being dragged, not just once on
    // release - rebuilding the preview (a real CPU mesh recompute) on every one of those frames is what made
    // dragging these sliders feel slow. Only rebuild once the mouse button that's driving the drag is
    // released, i.e. once per edit instead of dozens of times per drag.
    // The feature-adaptive subdivision follows the *displaced* surface (it samples depth * texture), so depth /
    // tile-size / rotation / invert all change where it puts triangles. The heavy displacement preview below
    // only rebuilds on release, which made the subdivide wireframe look like it ignored those edits - so
    // rebuild it live here (during the drag), the same cadence its own sliders use. Cheap: it is bounded by
    // the painted region.
    if (m_preview_params_dirty && m_subdivide_editing && m_subdivide_adaptive && m_subdivide_feature)
        rebuild_subdivide_preview();
    if (m_preview_params_dirty && (m_auto_update || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        rebuild_preview();
        // Same edits (tile size, rotation, offset, a new texture) are what the projector window draws, so it
        // refreshes on the same one-per-edit cadence. No-op while it is closed.
        update_projector();
        m_preview_params_dirty = false;
    }

    // ---- Footer: pinned below the body, so Bake never scrolls away. ----
    const float footer_top = ImGui::GetCursorScreenPos().y;
    {
        TextureDisplacementOptions &opts = mv->texture_displacement_options;
        const float x0 = ImGui::GetCursorPosX();
        // Over every part the bake will act on, not just the one holding the layer stack, so the count
        // the footer quotes is the count Bake is actually working with on a multibody object. Before
        // anything is painted there is nothing to bake yet, and the whole object is the honest estimate.
        size_t base_facets = 0;
        {
            std::vector<ModelVolume *> counted = painted_parts();
            if (counted.empty())
                counted = texture_parts();
            for (const ModelVolume *part : counted)
                base_facets += part->mesh().facets_count();
        }
        const int  base_k = int((base_facets + 500) / 1000);
        const bool v2     = mv != nullptr && opts.pipeline_v2;

        ImGui::Separator();

        if (!m_show_resolution) 
            ImGui::SetNextTreeNodeOpen(false);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0); // removes extra space on tree node indentation
        bool auto_res = opts.v2_refine_mm <= 0.f;
        std::string quality_title = (auto_res ? _u8L("Quality") + " (" + _u8L("Auto") + ")" 
                                              : _u8L("Quality")) // resolution value can be shown while collapsed
                                  + "###v2quality";  // stable ID, label can change freely
        m_imgui->push_bold_font();
        const bool quality_open = ImGui::TreeNodeEx(quality_title.c_str(), flags);
        m_imgui->pop_bold_font();
        if (quality_open) {
            if (!m_show_resolution) {
                m_show_resolution = true;
                m_imgui->set_requires_extra_frame();
            } else {
                // The triangle budget is the one subdivision control Standard mode keeps: its right value depends on
                // the part rather than on the recipe (a big model, or a fine texture, simply needs more of them), and
                // raising or lowering it is safe without understanding anything else. Pro's Subdivide uses it too.
                ImGui::AlignTextToFramePadding();
                if (v2) {
                    const V2Resolution         &rec  = v2_recommendation(*mv);

                    // -1 is "auto": the row shows the recommendation, greyed; editing it makes it a fixed value.
                    const bool auto_budget = opts.v2_max_triangles_k < 0;
                    int        shown_k     = auto_budget ? v2_recommendation(*mv).budget_k : opts.v2_max_triangles_k;
                    m_imgui->disabled_begin(auto_budget);
                    // TRN Slider value: %d is the triangle budget in thousands
                    if (int_row("##v2budget", _L("Budget"), &shown_k, 0, 4000, auto_budget ? _u8L("%d k (auto)").c_str() : "%d k", 0.f,
                                _L("How many thousand triangles this bake may spend on the area you painted. "
                                   "Relief already baked into the rest of the model is kept on top of it, so a "
                                   "second bake somewhere else gets the same budget as the first. Raise it if the "
                                   "warning below Resolution says the detail will not fit; 0 keeps every triangle "
                                   "the refinement produced, however many that is."),
                                { { "subdivide.gif" } })) {
                        opts.v2_max_triangles_k = shown_k;
                        m_preview_params_dirty  = true;
                    }
                    m_imgui->disabled_end();

                    // The one control the default pipeline needs: the edge length its refinement targets, which
                    // decides how much of the texture the mesh can carry. About eight texels per edge is where
                    // fine detail (mortar joints, knurl ridges) stops being lost between vertices.
                    m_imgui->text(_L("Resolution"));
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    // Auto: the edge (and the budget) follow the texture and the model. Off: the slider holds
                    // a fixed value, seeded with the recommendation so it starts from something sensible.
                    m_imgui->disabled_begin(busy);
                    if (m_imgui->bbl_checkbox(wxString::FromUTF8("##v2auto"), auto_res)) {
                        if (auto_res) {
                            opts.v2_refine_mm       = 0.f;
                            opts.v2_max_triangles_k = -1;
                        } else {
                            opts.v2_refine_mm       = rec.edge_mm > 0.f ? rec.edge_mm : 0.3f;
                            opts.v2_max_triangles_k = rec.budget_k > 0 ? rec.budget_k : 750;
                        }
                        m_preview_params_dirty = true;
                        m_parent.set_as_dirty();
                    }
                    m_imgui->disabled_end();
                    hover_tip(_u8L("On: the resolution follows the size of the model and the budget is left at its "
                                   "default. Untick to set both yourself - worth doing for a texture much finer or "
                                   "much coarser than the part it sits on."));
                    ImGui::SameLine();
                    float shown = auto_res ? rec.edge_mm : opts.v2_refine_mm;
                    // Held in a local: the format outlives the call that reads it, and the slider and the
                    // range in its tooltip are then formatted the same way.
                    const std::string v2edge_format = auto_res ? _u8L("%.2f mm (auto)") : std::string("%.2f mm");
                    const bool        v2edge_chosen = auto_res && rec.edge_mm > 0.f;
                    const wxString    v2edge_tip =
                        v2edge_chosen ?
                            from_u8(Slic3r::format(_u8L("Chosen from the size of the model: %1$.2f mm triangles, "
                                                        "budget %2% k. Untick Auto to set them yourself."),
                                                   rec.edge_mm, rec.budget_k)) :
                            _L("How fine the mesh is made under the paint, in millimetres. It has to be "
                               "smaller than the detail you want out of the texture - a 0.5 mm groove needs "
                               "triangles well under 0.5 mm. Smaller costs triangles fast: halving it needs "
                               "four times as many, and once they no longer fit the budget the bake simplifies "
                               "back down and the detail goes with it.");
                    m_imgui->disabled_begin(busy || auto_res);
                    if (typed_slider("##v2edge", x0 + panel_w - ImGui::GetCursorPosX() - edit_icon_room, &shown, 0.02f, 2.f,
                                     v2edge_format.c_str(), true, v2edge_tip,
                                     v2edge_chosen ? std::vector<TipMedia>{} : std::vector<TipMedia>{ { "subdivide.gif" } })) {
                        opts.v2_refine_mm      = shown;
                        m_preview_params_dirty = true;
                        m_parent.set_as_dirty();
                    }
                    m_imgui->disabled_end();

                    // What this resolution costs over what is painted, against what the budget allows. Refining
                    // past the budget is not an error - the bake simplifies back down to it - but the result
                    // then carries less of the texture than the resolution asks for, and the only sign of that
                    // used to be a mesh that came out coarser than expected. Shown before the bake, so the
                    // answer is to change a number rather than to wait out a bake and redo it.
                    const float  edge_now   = auto_res ? rec.edge_mm : opts.v2_refine_mm;
                    const int    budget_k   = opts.v2_max_triangles_k < 0 ? rec.budget_k : opts.v2_max_triangles_k;
                    const size_t budget     = size_t(std::max(0, budget_k)) * 1000;
                    const size_t needed     = estimated_refined_triangles(*mv, edge_now);
                    // Only when it is clearly over: the estimate runs about 3% high where it matters and up to
                    // a third high at coarse resolutions, where the mesh's own triangles are already near the
                    // target, and a warning about a bake that would have fitted is worse than none.
                    if (budget > 0 && needed > budget * 5 / 4) {
                        // Thousands under a million: a 119 k budget shown as "0.1 M" says nothing.
                        const auto count = [](size_t n) {
                            return n >= 1000000 ? Slic3r::format("%1$.1f M", double(n) / 1000000.) :
                                                  Slic3r::format("%1% k", (n + 500) / 1000);
                        };
                        // Wrapped to the panel, like the note under the buttons: unwrapped text runs past the
                        // panel's edge and takes the window's width with it.
                        ImGui::PushTextWrapPos(x0 + panel_w);
                        m_imgui->warning_text(Slic3r::format(_u8L("Needs about %1% triangles, budget %2% - the bake "
                                                                  "will simplify back down and lose detail."),
                                                             count(needed), count(budget)));
                        ImGui::PopTextWrapPos();
                    }
                } else {
                    m_imgui->text(_L("Triangles"));
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    m_imgui->disabled_begin(busy);
                    if (typed_slider_int("##subdivbudget", x0 + panel_w - ImGui::GetCursorPosX() - edit_icon_room,
                                         &m_subdivide_budget_k, 10, 2000, "+%d k", true,
                                         _L("How many thousand triangles the refinement may add to the model. The triangle "
                                            "that fits worst is always split first, so even a run that spends the lot has "
                                            "spent it where it shows most. Raise it if the relief still looks coarse."),
                                         { { "subdivide.gif" } })) {
                        if (m_subdivide_editing)
                            rebuild_subdivide_preview();
                        m_parent.set_as_dirty();
                    }
                    m_imgui->disabled_end();
                }
            }
            ImGui::TreePop();
        } else if (m_show_resolution) {
            m_show_resolution = false;
            m_imgui->set_requires_extra_frame();
        }
        ImGui::PopStyleVar(1); // ImGuiStyleVar_IndentSpacing

        const float button_h = std::round(frame_h * 1.25f);
        const float button_w = std::max({
            ImGui::CalcTextSize(_u8L("Close").c_str()).x,
            ImGui::CalcTextSize(_u8L("Stop").c_str()).x,
            ImGui::CalcTextSize(_u8L("Preparing...").c_str()).x,
            ImGui::CalcTextSize(_u8L("Baking...").c_str()).x,
        }) + m_imgui->scaled(0.5f);
        const float row_y  = ImGui::GetCursorPosY();
        const float icon_h = 21.f * scale;
        const float row_h  = std::max(icon_h, button_h);

        const std::vector<std::pair<wxString, wxString>> shortcut = 
            m_tool_type == ToolType::BUCKET_FILL ? m_shortcuts_bucket_fill
            : m_tool_type == ToolType::SMART_FILL  ? m_shortcuts_bucket_fill 
            : m_tool_type == ToolType::BRUSH       ? m_shortcuts_brush 
            : m_tool_type == ToolType::GAP_FILL    ? m_shortcuts_gap_fill 
            : m_shortcuts_sliders; // the sliders are there whether or not a painting tool is

        ImGui::SetCursorPosY(row_y + (row_h - icon_h) * .5f); // center vertically
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(int(m_imgui->scaled(.5f)), style.ItemSpacing.y));
        GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, shortcut, x, y);
        ImGui::SameLine();
        GLGizmoUtils::render_wiki_guide_button(m_parent, scale, "https://www.orcaslicer.com/wiki/print_prepare/prepare_texture_displacement");
        ImGui::SameLine();
        GLGizmoUtils::render_video_guide_button(m_parent, scale, "https://www.youtube.com/watch?v=D7w3tG1kdvE");
        ImGui::PopStyleVar(1);

        ImGui::SameLine(x0 + panel_w - button_w * 2 - style.ItemSpacing.x);

        // Read before Close below, which releases the selection any_part_painted() finds the parts through.
        const bool        painted    = mv != nullptr && any_part_painted();
        const bool        can_bake   = !busy && painted;
        const std::string bake_label = m_prepare_in_progress ? _u8L("Preparing...") :
                                       m_bake_in_progress    ? _u8L("Baking...") :
                                                               into_u8(m_desc.at("bake"));
        GLGizmoUtils::push_orca_button_style();
        m_imgui->push_bold_font();
        m_imgui->disabled_begin(!can_bake);
        ImGui::SetCursorPosY(row_y + (row_h - button_h) * .5f); // center vertically
        if (ImGui::Button((bake_label + "##bake").c_str(), ImVec2(button_w, button_h))) {
            // Standard mode's Bake is the whole pipeline (remesh -> refine -> displace); Pro's is only the
            // displacement, because there the user has already prepared the mesh with the controls above.
            if (pro_mode())
                bake();
            else
                bake_standard();
        }
        m_imgui->disabled_end();
        m_imgui->pop_bold_font();
        GLGizmoUtils::pop_orca_button_style();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            panel_tooltip(mv != nullptr && !painted ?
                              (m_seam_edit_mode ? _u8L("Nothing is painted yet. The UV editor's seam tool is on, so "
                                                       "strokes on the model mark seams instead of painting - turn "
                                                       "it off in the pane to paint.") :
                                                  _u8L("Nothing is painted yet.")) :
                          pro_mode() ?
                              _u8L("Turn the painted height maps into real geometry, by moving the vertices that are "
                                   "already there. Use Subdivide first if the mesh is too coarse to show the detail.") :
                              _u8L("Turn the painted height maps into real geometry. The mesh is remeshed to an even "
                                   "density and refined where the texture bends first, so the detail has vertices to "
                                   "land on - all in one step."));

        ImGui::SameLine();
        if (busy) {
            if (ImGui::Button((_u8L("Stop") + "##stop").c_str(), ImVec2(button_w, button_h)))
                wxGetApp().plater()->get_ui_job_worker().cancel_all();
            hover_tip(_u8L("Stops the bake. Whatever it had already finished stays on the model, and can be undone."));
        } else {
            if (ImGui::Button((_u8L("Close") + "##close").c_str(), ImVec2(button_w, button_h)))
                m_parent.reset_all_gizmos();
            hover_tip(_u8L("Closes the tool without baking. Your paint, layers and settings stay with the model."));
        }

        // What Bake will produce, and which layers it will skip.
        if (mv != nullptr) {
            const std::string base_count = base_k > 0 ? Slic3r::format(_u8L("%1% k"), base_k) :
                                                        std::to_string(base_facets);
            const unsigned    base_n     = base_k > 0 ? unsigned(base_k) * 1000 : unsigned(base_facets);
            const unsigned    result_n   = unsigned(base_k + m_subdivide_budget_k) * 1000;
            // TRN %1% is a triangle count, either exact or rounded to thousands such as "120 k".
            std::string note = v2         ? Slic3r::format(into_u8(_L_PLURAL("Model: %1% triangle - the bake refines to the resolution above, up to its budget",
                                                                             "Model: %1% triangles - the bake refines to the resolution above, up to its budget", base_n)), base_count) :
                               // TRN %1% is a triangle count, either exact or rounded to thousands such as "120 k".
                               pro_mode() ? Slic3r::format(into_u8(_L_PLURAL("Bake moves existing vertices - the model stays at %1% triangle",
                                                                             "Bake moves existing vertices - the model stays at %1% triangles", base_n)), base_count) :
                                            Slic3r::format(into_u8(_L_PLURAL("Bake result up to ~%1% k triangle", "Bake result up to ~%1% k triangles", result_n)),
                                                           base_k + m_subdivide_budget_k);
            if (painted) {
                std::vector<std::string> skipped;
                for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
                    if (!slot_painted(l.slot))
                        skipped.push_back(layer_name(l));
                if (skipped.size() > 2)
                    note += ", " + Slic3r::format(into_u8(_L_PLURAL("%1% layer not painted, skipped", "%1% layers not painted, skipped",
                                                                    unsigned(skipped.size()))), skipped.size());
                else if (!skipped.empty())
                    // TRN %1% is one layer name, or two names separated by a comma.
                    note += ", " + Slic3r::format(into_u8(_L_PLURAL("%1% not painted, skipped", "%1% not painted, skipped",
                                                                    unsigned(skipped.size()))),
                                                   skipped.size() == 2 ? skipped[0] + ", " + skipped[1] : skipped[0]);
            }
            ImGui::PushTextWrapPos(x0 + panel_w);
            ImGui::TextDisabled("%s", note.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    m_panel_footer_h = ImGui::GetCursorScreenPos().y - footer_top;
    constexpr float resize_epsilon = 0.5f;
    const float content_bottom = ImGui::GetCursorPosY() - style.ItemSpacing.y;
    const float window_bottom  = ImGui::GetWindowContentRegionMax().y;
    const bool  outer_window_needs_fit = content_bottom < window_bottom - resize_epsilon ||
                                         (content_bottom > window_bottom + resize_epsilon &&
                                          ImGui::GetWindowHeight() < ImGui::GetMainViewport()->Size.y -
                                                                             2.f * style.DisplaySafeAreaPadding.y - resize_epsilon);
    if (std::abs(m_panel_body_h - previous_body_h) > resize_epsilon ||
        std::abs(m_panel_footer_h - previous_footer_h) > resize_epsilon || outer_window_needs_fit) {
        m_parent.request_extra_frame();
    }
    GizmoImguiEnd();

    // Outside the gizmo's window, because it is a window of its own: a floating, non-modal panel that
    // leaves the 3D view live so the relief can be watched while the sliders move. No-op when closed.
    render_image_adjust_window(panel_rect_min, panel_rect_max);

    ImGuiWrapper::pop_toolbar_style();

    // Drawn last, over everything, via the foreground draw list: the +/- add-remove sign next to the 3D
    // cursor. Still inside the gizmo's ImGui frame here, which is what render_paint_cursor_hint() needs.
    render_paint_cursor_hint();
}

} // namespace Slic3r::GUI
