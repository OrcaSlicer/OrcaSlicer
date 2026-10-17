#pragma once

#include "../BoundingBox.hpp"
#include "../Polyline.hpp"
#include "../ExPolygon.hpp"
#include <limits>
#include <memory>
#include <utility>
#include <cstddef>
#include <array>
#include <vector>

namespace Slic3r::Voronoi {

// Geometry metadata only: no traversal or pairing decisions in the slicer.
struct Section {
    Polylines walls;
};

struct Band {
    ExPolygon polygon;
    // XY normal pointing toward the previous layer's part of this face.
    Vec2d supported_side;
    // Supported strip boundary before face/model clipping, in millimeters.
    double row_origin {std::numeric_limits<double>::quiet_NaN()};
};

struct Layer {
    Polylines walls;
    std::vector<Band> bands;
};

// Immutable 3D network for floor extraction; CGAL stays out of this header.
class CellGeometry {
public:
    explicit CellGeometry(const std::vector<Vec3d> &sites);
    ~CellGeometry();
    // Project shallow faces over twice the layer height, rounding to native rows.
    std::vector<Band> shallow_bands(const BoundingBox &bounds, double z, double width,
                                  double height, double row_spacing) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Extract the bounded planar power diagram from the 3D sites.
Section section(const std::vector<Vec3d> &sites, const BoundingBox &bounds, double z);

} // namespace Slic3r::Voronoi
