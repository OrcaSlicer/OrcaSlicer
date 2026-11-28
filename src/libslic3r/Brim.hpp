#ifndef slic3r_Brim_hpp_
#define slic3r_Brim_hpp_

#include "ExPolygon.hpp"
#include "ObjectID.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "libslic3r.h"

#include<map>
#include <utility>
#include<vector>

namespace Slic3r {

class Print;
class PrintObject;
class ExtrusionEntityCollection;
class PrintTryCancel;

// Produce brim lines around those objects, that have the brim enabled.
// Collect islands_area to be merged into the final 1st layer convex hull.
// Non-const Print& to store ear centroids (btEar) and bounding box.
void make_brim(Print& print, PrintTryCancel try_cancel,
    Polygons& islands_area, std::map<ObjectID, ExtrusionEntityCollection>& brimMap,
    std::map<ObjectInstanceID, ExtrusionEntityCollection>& brimMapByInstance,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec,
    std::vector<unsigned int>& printExtruders,
    std::map<ObjectInstanceID, ExPolygons>* objectBrimAreasByInstanceOut = nullptr);

ExtrusionEntityCollection makeBrimInfill(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area);
ExtrusionEntityCollection makeBrimInfillFromPlateCoordinates(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area);

// Compute the brim area (outer and/or inner, respecting brim_type) for one
// instance at the given layer. Returns ExPolygons in plate coordinates.
// Handles btEar, btPainted and btAutoBrim. Does not use the EFC outline or
// per-volume-group slices (the full layer outline is used instead); support
// exclusion is applied by the caller.
ExPolygons make_brim_area_for_layer(const Print &print, const PrintObject &object,
    size_t instance_id, size_t layer_idx);

// Whether two brim areas overlap or are within ~2 brim line widths of each other.
bool brim_areas_within_contact_distance(const Print &print, const ExPolygons &area_a, const ExPolygons &area_b);
// Union brim areas and clean up the result (dilate/erode by one resolution step).
ExPolygons merge_brim_areas(const Print &print, const ExPolygons &areas);

// BBS: automatically make brim
ExtrusionEntityCollection make_brim_auto(const Print &print, PrintTryCancel try_cancel, Polygons &islands_area);

// ORCA: Detect mouse ear positions on one closed contour. Coordinates and ear_detection_length are scaled.
// The window starts at ear_detection_length and doubles while the outline keeps curving; a corner is accepted
// once its turn saturates. Outer contours take convex corners, inner contours concave ones; candidates on the
// same corner collapse to the sharpest.
Points make_brim_ear_points(const Polygon &contour, coord_t ear_detection_length,
                            coordf_t brim_ears_max_angle, bool is_outer_brim);

} // Slic3r

#endif // slic3r_Brim_hpp_
