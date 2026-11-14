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
class ExtrusionEntityCollection;
class PrintTryCancel;

// Produce brim lines around those objects, that have the brim enabled.
// Collect islands_area to be merged into the final 1st layer convex hull.
void make_brim(const Print& print, PrintTryCancel try_cancel,
    Polygons& islands_area, std::map<ObjectID, ExtrusionEntityCollection>& brimMap,
    std::map<ObjectInstanceID, ExtrusionEntityCollection>& brimMapByInstance,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec,
    std::vector<unsigned int>& printExtruders,
    std::map<ObjectInstanceID, ExPolygons>* objectBrimAreasByInstanceOut = nullptr);

ExtrusionEntityCollection makeBrimInfill(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area);
ExtrusionEntityCollection makeBrimInfillFromPlateCoordinates(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area);

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
