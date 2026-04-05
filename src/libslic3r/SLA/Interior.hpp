#ifndef slic3r_SLA_Interior_hpp_
#define slic3r_SLA_Interior_hpp_

#include <optional>
#include <memory>

#include <libslic3r/TriangleMesh.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4146)
#endif
#include <openvdb/openvdb.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Slic3r {
namespace sla {

// Interior represents a hollowed volume with both mesh and voxel grid representations.
// Used for SLA hollowing and Magma interior shell computation.
struct Interior {
    indexed_triangle_set mesh;
    openvdb::FloatGrid::Ptr gridptr;
    mutable std::optional<openvdb::FloatGrid::ConstAccessor> accessor;

    double closing_distance = 0.;
    double thickness = 0.;
    double voxel_scale = 1.;
    double nb_in = 3.;  // narrow band width inwards
    double nb_out = 3.; // narrow band width outwards
    // Full narrow band is the sum of the two above values.

    void reset_accessor() const  // This resets the accessor and its cache
    // Not a thread safe call!
    {
        if (gridptr)
            accessor = gridptr->getConstAccessor();
    }
};

struct InteriorDeleter { void operator()(Interior *p); };
using InteriorPtr = std::unique_ptr<Interior, InteriorDeleter>;

} // namespace sla
} // namespace Slic3r

#endif // slic3r_SLA_Interior_hpp_
