#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include "libslic3r.h"
#include <vector>
#include "Layer.hpp"
#include "Flow.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "SurfaceCollection.hpp"

namespace Slic3r {

class PerimeterGenerator {
public:
    // Inputs:
    const SurfaceCollection     *slices;
    const LayerRegionPtrs       *compatible_regions;
    const ExPolygons            *upper_slices;
    const SurfaceCollection     *upper_slices_same_region;
    const ExPolygons            *lower_slices;
    double                       layer_height;
    int                          layer_id;
    coordf_t                     slice_z;
    Flow                         perimeter_flow;
    Flow                         ext_perimeter_flow;
    Flow                         overhang_flow;
    Flow                         solid_infill_flow;
    const PrintRegionConfig     *config;
    const PrintObjectConfig     *object_config;
    const PrintConfig           *print_config;
    // Outputs:
    ExtrusionEntityCollection   *loops;
    ExtrusionEntityCollection   *gap_fill;
    SurfaceCollection           *fill_surfaces;
    //BBS
    ExPolygons                  *fill_no_overlap;

    //BBS
    Flow                        smaller_ext_perimeter_flow;
    std::vector<Polygons>       m_lower_polygons_series;
    std::vector<Polygons>       m_external_lower_polygons_series;
    std::vector<Polygons>       m_smaller_external_lower_polygons_series;

    bool                                            has_fuzzy_skin = false;
    bool                                            has_fuzzy_hole = false;
    std::unordered_map<FuzzySkinConfig, ExPolygons> regions_by_fuzzify;
    
    PerimeterGenerator(
        // Input:
        const SurfaceCollection*    slices,
        const LayerRegionPtrs       *compatible_regions,
        double                      layer_height,
        coordf_t                    slice_z,
        Flow                        flow,
        const PrintRegionConfig*    config,
        const PrintObjectConfig*    object_config,
        const PrintConfig*          print_config,
        const bool                  spiral_mode,
        // Output:
        // Loops with the external thin walls
        ExtrusionEntityCollection*  loops,
        // Gaps without the thin walls
        ExtrusionEntityCollection*  gap_fill,
        // Infills without the gap fills
        SurfaceCollection*          fill_surfaces,
        //BBS
        ExPolygons*                 fill_no_overlap)
        : slices(slices), compatible_regions(compatible_regions), upper_slices(nullptr), lower_slices(nullptr), layer_height(layer_height),
            slice_z(slice_z), layer_id(-1), perimeter_flow(flow), ext_perimeter_flow(flow),
            overhang_flow(flow), solid_infill_flow(flow),
            config(config), object_config(object_config), print_config(print_config),
            m_spiral_vase(spiral_mode),
            m_scaled_resolution(scaled<double>(print_config->resolution.value > EPSILON ? print_config->resolution.value : EPSILON)),
            loops(loops), gap_fill(gap_fill), fill_surfaces(fill_surfaces), fill_no_overlap(fill_no_overlap),
            m_ext_mm3_per_mm(-1), m_mm3_per_mm(-1), m_mm3_per_mm_overhang(-1), m_ext_mm3_per_mm_smaller_width(-1)
        {}

    void        process_classic();
    void        process_arachne();

    void        add_infill_contour_for_arachne( ExPolygons infill_contour, int loops, coord_t ext_perimeter_spacing, coord_t perimeter_spacing, coord_t min_perimeter_infill_spacing, coord_t spacing, bool is_inner_part );

    double      ext_mm3_per_mm()        const { return m_ext_mm3_per_mm; }
    double      mm3_per_mm()            const { return m_mm3_per_mm; }
    double      mm3_per_mm_overhang()   const { return m_mm3_per_mm_overhang; }
    //BBS
    double      smaller_width_ext_mm3_per_mm()   const { return m_ext_mm3_per_mm_smaller_width; }
    Polygons    lower_slices_polygons() const { return m_lower_slices_polygons; }

private:
    std::vector<Polygons>     generate_lower_polygons_series(float width);
    void split_top_surfaces(const ExPolygons &orig_polygons, ExPolygons &top_fills, ExPolygons &non_top_polygons, ExPolygons &fill_clip) const;
    void apply_extra_perimeters(ExPolygons& infill_area);
    void process_no_bridge(Surfaces& all_surfaces, coord_t perimeter_spacing, coord_t ext_perimeter_width);

private:
    bool        m_spiral_vase;
    double      m_scaled_resolution;
    double      m_ext_mm3_per_mm;
    double      m_mm3_per_mm;
    double      m_mm3_per_mm_overhang;
    //BBS
    double      m_ext_mm3_per_mm_smaller_width;
    Polygons    m_lower_slices_polygons;
};

}

#endif
