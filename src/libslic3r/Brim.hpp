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

//ORCA: A brim ear for the outline point `pt`. Without a sprue the ear is centred on `pt`. With one,
// the ear sits `sprue_length` out from the outline, joined to it by a strip `sprue_width` wide that
// runs away from the material: along the edge normal, or the corner bisector at a vertex.
// `outline` follows the ExPolygon orientation, material on the left of each edge.
// The slicer and the brim ears gizmo both place ears with it, so the preview matches the print.
struct BrimEar
{
    Point      base;      // where the ear attaches to the outline
    Point      center;    // centre of the round ear
    Vec2d      dir;       // unit vector from base to center, zero without a sprue
    double     back = 0.; // how far the strip reaches behind base into the material, scaled
    coord_t    sprue_width = 0;
    ExPolygons footprint; // round ear and strip
};
BrimEar place_brim_ear(const Polygons &outline, const Point &pt, coord_t radius, coord_t sprue_length, coord_t sprue_width);

} // Slic3r

#endif // slic3r_Brim_hpp_
