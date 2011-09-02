#ifndef slic3r_PrincipalComponents2D_hpp_
#define slic3r_PrincipalComponents2D_hpp_

#include "AABBTreeLines.hpp"
#include "BoundingBox.hpp"
#include "libslic3r.h"
#include <vector>
#include "Polygon.hpp"

namespace Slic3r {

// returns triangle area, first_moment_of_area_xy, second_moment_of_area_xy, second_moment_of_area_covariance
// none of the values is divided/normalized by area.
// The function computes intgeral over the area of the triangle, with function f(x,y) = x for first moments of area (y is analogous)
// f(x,y) = x^2 for second moment of area
// and f(x,y) = x*y for second moment of area covariance
std::tuple<float, Vec2f, Vec2f, float> compute_moments_of_area_of_triangle(const Vec2f &a, const Vec2f &b, const Vec2f &c);

// returns two eigenvectors of the area covered by given polygons. The vectors are sorted by their corresponding eigenvalue, largest first
std::tuple<Vec2f, Vec2f> compute_principal_components(const Polygons &polys);

}

#endif