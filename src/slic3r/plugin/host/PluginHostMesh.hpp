#pragma once

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/TriangleMesh.hpp>

namespace Slic3r {

// Build a BoundingBoxf3 from precomputed (float) triangle-mesh stats min/max.
// Shared by the TriangleMesh binding (PluginHostMesh.cpp) and the mesh-derived
// ModelVolume accessors (PluginHostModel.cpp).
inline BoundingBoxf3 bbox_from_stats(const TriangleMeshStats& stats)
{
    if (stats.number_of_facets == 0)
        return BoundingBoxf3();
    return BoundingBoxf3(stats.min.cast<double>(), stats.max.cast<double>());
}

} // namespace Slic3r
