#pragma once

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "TriangleMesh.hpp"
#include "libslic3r.h"

#include <admesh/stl.h>

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {

// The connected body of each island of each layer: islands of adjacent layers whose slices overlap are one body.
// Bodies are numbered from 0 in the order of their first island.
std::vector<std::vector<size_t>> connected_bodies(const std::vector<const ExPolygons *> &layers, size_t &count,
                                                  const std::function<void()> &throw_if_canceled = nullptr);

// Finds the island of a layer that a point lies in, testing the polygons only where the boxes of several islands hold it.
class IslandLocator
{
public:
    // The islands must outlive the locator. Their boxes are widened by the margin, for points reaching past an outline.
    IslandLocator(const ExPolygons &islands, coord_t margin);
    // Whether the island holds the point, or its box does where no other box reaches unless strict.
    bool holds(size_t island, const Point &point, bool strict = false) const;
    // The island holding the point as above, else the nearest one whose box holds it, with the squared distance to it; -1
    // for none.
    std::pair<int, double> find(const Point &point, bool strict = false) const;
    const std::vector<BoundingBox> &boxes() const { return m_boxes; }

private:
    const ExPolygons        *m_islands;
    std::vector<BoundingBox> m_boxes;
    std::vector<bool>        m_alone;
};

using MeshInPlace = std::pair<const indexed_triangle_set *, Transform3d>;

// A connected body of solids, with its outline seen from above as a convex hull and the height it spans.
struct SolidBody : MassProperties
{
    Polygon hull;
    double  z_min{ 0. };
    double  z_max{ 0. };

    // Its box once transformed, tight for a transformation that rotates about z only.
    BoundingBoxf3 bounding_box(const Transform3d &trafo) const;
};

// Each connected body of the union of the solids less the negatives, sliced into slabs, each solid weighing its density.
// Where solids overlap, the later one counts, as slicing prints it.
std::vector<SolidBody> solid_bodies(const std::vector<MeshInPlace> &solids, const std::vector<double> &densities,
                                    const std::vector<MeshInPlace> &negatives, size_t slabs);

} // namespace Slic3r
