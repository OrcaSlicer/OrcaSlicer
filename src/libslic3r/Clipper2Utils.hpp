#ifndef slic3r_Clipper2Utils_hpp_
#define slic3r_Clipper2Utils_hpp_

#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "clipper2/clipper.h"

namespace Slic3r {

Clipper2Lib::Paths64 Slic3rPolylines_to_Paths64(const Slic3r::Polylines& in);
Slic3r::Polylines  Paths64_to_polylines(const Clipper2Lib::Paths64& in);
Slic3r::Polylines  intersection_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip);
Slic3r::Polylines  diff_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip);
ExPolygons         union_ex_2(const Polygons &expolygons);
ExPolygons         union_ex_2(const ExPolygons &expolygons);
ExPolygons         offset_ex_2(const ExPolygons &expolygons, double delta);
ExPolygons         offset2_ex_2(const ExPolygons &expolygons, double delta1, double delta2);
}

#endif