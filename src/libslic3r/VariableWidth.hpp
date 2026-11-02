#ifndef slic3r_VariableWidth_hpp_
#define slic3r_VariableWidth_hpp_

#include "ExtrusionEntity.hpp"
#include "Polyline.hpp"
#include <vector>

namespace Slic3r { class Flow; }

namespace Slic3r {
    ExtrusionMultiPath thick_polyline_to_multi_path(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance, const float merge_tolerance);
    // ORCA: one path for each run of segments whose widths all stay within width_band of each other, at their length
    // weighted mean width. See VariableWidth.cpp.
    ExtrusionMultiPath thick_polyline_to_multi_path_banded(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance, const float width_band);
    void variable_width(const ThickPolylines& polylines, ExtrusionRole role, const Flow& flow, std::vector<ExtrusionEntity*>& out);
}

#endif
