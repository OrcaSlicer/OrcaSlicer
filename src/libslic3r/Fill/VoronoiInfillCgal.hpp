#pragma once

#include "../BoundingBox.hpp"
#include "../Polyline.hpp"
#include "../ExPolygon.hpp"
#include <limits>
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
// Sites and z are in millimeters; bounds and edges use scaled coordinates.
Section section(const std::vector<Vec3d> &sites, const BoundingBox &bounds, double z);

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

// Project shallow faces over twice the layer height to give consecutive bands
// at least 50% overlap after rounding outward to whole rows at row_spacing.
// Exactly horizontal faces belong to one half-open layer interval.
std::vector<Band> shallow_bands(const std::vector<Vec3d> &sites, const BoundingBox &bounds,
                              double z, double width, double height, double row_spacing);

} // namespace Slic3r::Voronoi
