#pragma once

#include "BoundingBox.hpp"
#include "Color.hpp"
#include "MixedFilament.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class ModelVolume;

struct SpectrumPicPrintPlan
{
    bool        valid = false;
    std::string error;
    std::string mixed_filament_definitions; // enabled mix rows; empty if all Physical
    size_t      mix_count     = 0;
    size_t      cluster_count = 0;
    int         width         = 0; // buffer used for sampling (after downsample)
    int         height        = 0;
    std::vector<unsigned> dest_id; // size width*height, 1-based filament IDs (physical or Mix)
};

// rgb: packed RGB bytes, length w*h*3. max_clusters default SPECTRUM_PAINT_ID_PERSIST_CAP (32).
// existing_mix_defs: project mix rows to keep (append unique PicPrint recipes; do not
// reassign Mix IDs already used by other objects). Fails if mix_base + merged > persist 32.
SpectrumPicPrintPlan plan_spectrum_picprint(
    const std::uint8_t *rgb, int w, int h,
    const std::vector<ColorRGB> &physicals,
    size_t mix_base,
    size_t max_clusters = SPECTRUM_PAINT_ID_PERSIST_CAP,
    const std::string &existing_mix_defs = {});

// u,v in [0,1]. pixel_x = clamp(int(u*(w-1))); pixel_y = clamp(int((1.0-v)*(h-1))).
unsigned spectrum_picprint_sample_facet(const SpectrumPicPrintPlan &plan, double u, double v);

// Planar XY: u,v vs the passed bbox min/max (not x/extent with origin at 0).
// Returns false when X or Y extent < 1e-6.
bool spectrum_picprint_world_to_uv(const Vec3d &world, const BoundingBoxf3 &xy_bbox, double &u, double &v);

// Over-cap Catch2: collapse unique mix recipe strings until mix_base + count <= persist 32.
// Duplicate the 0048 ΔE00 merge loop (mixer_delta_e00 + predicted_swatch_for_mix). Do NOT change PaintBake algorithm.
// Bypass median-cut: this helper takes already-built recipe rows.
std::string spectrum_collapse_mix_recipe_rows(
    std::vector<std::string> recipe_rows,
    const std::vector<ColorRGB> &physicals,
    size_t mix_base);

// Original-triangle planar XY paint. World = instance matrix * volume matrix.
// Refuses when XY extent < 1e-6. Does not select_patch / subdivide.
bool spectrum_picprint_apply_to_volume(ModelVolume &vol,
                                       const Transform3d &world,
                                       const BoundingBoxf3 &xy_bbox,
                                       const SpectrumPicPrintPlan &plan);

} // namespace Slic3r
