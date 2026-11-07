#pragma once

#include <cstddef>
#include <string>

namespace Slic3r { namespace GUI {

// Icons that come in colour (FreeCAD's LGPL set, drawn for a light ground, and Orca's own green-and-white import glyphs) are
// decolorized to grey so a toolbar reads as one family: the hue goes, the shading stays; the coloured part of an icon is kept as one accent, Orca's teal. A rasterised icon (straight,
// non-premultiplied RGBA) keeps its alpha and its tonal structure: on a dark ground the original's near-black outlines become
// the light, the fills stay darker and the highlights stay bright but never brighter than an outline; on a light ground the
// dark outlines stay dark and the whites stay white. Used by the ImGui bars and by BitmapCache for fc_* icons.
// Features that take material away carry a red accent where the others carry the teal, so Extrude and Cut (same shape in FreeCAD's
// set) tell apart at a glance.
inline bool fc_subtractive_icon(const std::string& name)
{
    return name == "fc_cut" || name == "fc_hole" || name == "fc_bool_subtract";
}

inline void decolorize_rgba(unsigned char* px, size_t n_pixels, bool dark, bool subtractive = false)
{
    const float acc_r = subtractive ? 214.f : 0.f, acc_g = subtractive ? 72.f : 150.f, acc_b = subtractive ? 66.f : 136.f;   // red, or Orca's #009688
    for (size_t i = 0; i < n_pixels; ++i, px += 4) {
        if (px[3] == 0) continue;
        const float lum = (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.f;
        const float hi = float(px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2]) : (px[1] > px[2] ? px[1] : px[2]));
        const float lo = float(px[0] < px[1] ? (px[0] < px[2] ? px[0] : px[2]) : (px[1] < px[2] ? px[1] : px[2]));
        if (hi > 40.f && (hi - lo) / hi > 0.28f) {   // a coloured pixel: whatever its hue, it becomes Orca's UI teal (#009688), shading kept
            const float k = 0.62f + 0.75f * lum;
            const auto  c = [k](float v) { const float x = v * k; return static_cast<unsigned char>(x > 255.f ? 255.f : x + 0.5f); };
            px[0] = c(acc_r); px[1] = c(acc_g); px[2] = c(acc_b);
            continue;
        }
        float g;
        if (dark) {
            if      (lum < 0.18f) g = 0.88f;                                         // outline
            else if (lum < 0.34f) g = 0.88f + (0.46f - 0.88f) * (lum - 0.18f) / 0.16f;
            else if (lum < 0.75f) g = 0.46f + (0.64f - 0.46f) * (lum - 0.34f) / 0.41f;   // fill
            else                  g = 0.64f + (0.78f - 0.64f) * (lum - 0.75f) / 0.25f;   // highlight
        } else {
            g = 0.22f + 0.70f * lum;
        }
        const unsigned char v = static_cast<unsigned char>(g * 255.f + 0.5f);
        px[0] = px[1] = px[2] = v;
    }
}

}} // namespace Slic3r::GUI
