#ifndef libslic3r_Timer_hpp_
#define libslic3r_Timer_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {
class TriangleMeshDeal
{
public:
    static TriangleMesh smooth_triangle_mesh(const TriangleMesh &mesh,bool& ok);
};
} // namespace Slic3r

#endif // libslic3r_Timer_hpp_
