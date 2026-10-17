#pragma once

#include "../BoundingBox.hpp"
#include "../Polyline.hpp"
#include "../ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include <limits>
#include <memory>
#include <utility>
#include <cstddef>
#include <array>
#include <vector>

namespace Slic3r::Voronoi {

// Geometry metadata only: no traversal or pairing decisions in the slicer.
struct Junction {
    Point point;
    std::array<Vec3d, 3> sites;
};
struct Section {
    Polylines walls;
    std::vector<Junction> junctions;
    // Maximum nearest-site distance over the bounded XY section, including
    // clipped boundary vertices. Infinity means that no site is known.
    double covering_radius { std::numeric_limits<double>::infinity() };
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

// Retains the 3D Delaunay network between point-cloud updates. Copies own an
// independent network, as required by Fill::clone(). CGAL stays out of this header.
class CellGeometry {
public:
    CellGeometry();
    ~CellGeometry();
    CellGeometry(const CellGeometry &);
    CellGeometry &operator=(const CellGeometry &);
    CellGeometry(CellGeometry &&) noexcept;
    CellGeometry &operator=(CellGeometry &&) noexcept;
    // Returns counts of inserted and removed vertices.
    std::pair<size_t, size_t> update(const std::vector<Vec3d> &sites);
    size_t remove_below(double z);
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
