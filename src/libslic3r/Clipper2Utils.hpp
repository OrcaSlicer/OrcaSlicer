#ifndef slic3r_Clipper2Utils_hpp_
#define slic3r_Clipper2Utils_hpp_

#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "clipper2/clipper.h"

namespace Slic3r {

// Constants for Clipper2
static constexpr const double                         Clipper2SafetyOffset     = 10.0;
static constexpr const Clipper2Lib::JoinType         DefaultJoinType2         = Clipper2Lib::JoinType::Miter;
static constexpr const Clipper2Lib::EndType          DefaultEndType2          = Clipper2Lib::EndType::Polygon;
static constexpr const double                         DefaultMiterLimit2       = 3.0;
static constexpr const Clipper2Lib::JoinType         DefaultLineJoinType2     = Clipper2Lib::JoinType::Square;
static constexpr const double                         DefaultLineMiterLimit2   = 0.0;

// Polylines conversion and operations
Clipper2Lib::Paths64 Slic3rPolylines_to_Paths64(const Slic3r::Polylines& in);
Slic3r::Polylines    Paths64_to_polylines(const Clipper2Lib::Paths64& in);
Slic3r::Polylines    intersection_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip);
Slic3r::Polylines    diff_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip);

// Polygon conversion functions
Clipper2Lib::Path64   Slic3rPolygon_to_Path64(const Polygon &polygon);
Clipper2Lib::Paths64  Slic3rPolygon_to_Paths64(const Polygon &polygon);
Clipper2Lib::Paths64  Slic3rPolygons_to_Paths64(const Polygons &polygons);
Clipper2Lib::Paths64  Slic3rExPolygon_to_Paths64(const ExPolygon &expolygon);
Clipper2Lib::Paths64  Slic3rExPolygons_to_Paths64(const ExPolygons &expolygons);

// Reverse conversion functions
Polygon         Path64_to_Slic3rPolygon(const Clipper2Lib::Path64 &path);
Polygons        Paths64_to_Slic3rPolygons(const Clipper2Lib::Paths64 &paths);
Points          Path64ToPoints(const Clipper2Lib::Path64& path64);

// Union operations
ExPolygons union_ex_2(const Polygons &polygons);
ExPolygons union_ex_2(const ExPolygons &expolygons);

// Offset operations
ExPolygons offset_ex_2(const ExPolygons &expolygons, double delta);
ExPolygons offset2_ex_2(const ExPolygons &expolygons, double delta1, double delta2);

// Offset functions for ExPolygon (returns Polygons)
Polygons offset_2(const ExPolygon &expolygon, double delta, 
                  Clipper2Lib::JoinType joinType = DefaultJoinType2, 
                  double miterLimit = DefaultMiterLimit2);

Polygons union_pt_chained_outside_in_2(const Polygons &subject);

// PolyTree utilities
void SimplifyPolyTree(const Clipper2Lib::PolyPath64 *polytree, double epsilon, Clipper2Lib::PolyPath64 *result);

} // namespace Slic3r

#endif // slic3r_Clipper2Utils_hpp_