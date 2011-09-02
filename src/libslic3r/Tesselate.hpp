#ifndef slic3r_Tesselate_hpp_
#define slic3r_Tesselate_hpp_

#include <vector>

#include "Point.hpp"

namespace Slic3r {

class ExPolygon;
typedef std::vector<ExPolygon> ExPolygons;

const bool constexpr NORMALS_UP = false;
const bool constexpr NORMALS_DOWN = true;

extern std::vector<Vec3d> triangulate_expolygon_3d (const ExPolygon  &poly,  coordf_t z = 0, bool flip = NORMALS_UP);
extern std::vector<Vec3d> triangulate_expolygons_3d(const ExPolygons &polys, coordf_t z = 0, bool flip = NORMALS_UP);
extern std::vector<Vec2d> triangulate_expolygon_2d (const ExPolygon  &poly,  bool flip = NORMALS_UP);
extern std::vector<Vec2d> triangulate_expolygons_2d(const ExPolygons &polys, bool flip = NORMALS_UP);
extern std::vector<Vec2f> triangulate_expolygon_2f (const ExPolygon  &poly,  bool flip = NORMALS_UP);
extern std::vector<Vec2f> triangulate_expolygons_2f(const ExPolygons &polys, bool flip = NORMALS_UP);

} // namespace Slic3r

#endif /* slic3r_Tesselate_hpp_ */
