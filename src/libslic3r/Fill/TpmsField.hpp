#pragma once

#include <cmath>

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"

namespace Slic3r {

// Implicit TPMS equation with a period of 2 PI, with its cells stretched by scale along the axes of the pattern, then
// turned by rotation_x around its X axis.
struct TpmsEquation
{
    TpmsEquation(float (*equation)(float x, float y, float z), const Vec3d &scale, double rotation_x)
        : equation(equation), scale(scale), cos_x(std::cos(rotation_x)), sin_x(std::sin(rotation_x))
    {}

    float operator()(double x, double y, double z) const
    {
        return equation(float(x / scale.x()), float((cos_x * y + sin_x * z) / scale.y()), float((cos_x * z - sin_x * y) / scale.z()));
    }

    // Every layer is a layer of the regular cells, which the parametric waves of Gyroid and TPMS-D follow.
    bool regular_layers() const { return scale.x() == 1. && scale.y() == 1. && sin_x == 0.; }

    float (*equation)(float x, float y, float z);
    Vec3d  scale;
    double cos_x;
    double sin_x;
};

// Sampling grid cell in mm, roughly the length of the line segments.
constexpr double TpmsGridCell = 0.4;

// Lines of the TPMS on the layer at z, sampled over bbox on a grid of cell, with the frequency along each axis in
// radians per mm.
Polylines make_tpms_polylines(const TpmsEquation &tpms, const Vec3d &frequency, const BoundingBox &bbox, coordf_t z,
                              double cell = TpmsGridCell);

// bbox with a cell of margin for the lines closed along its border, on a sampling grid shared by every region.
BoundingBox tpms_sampling_bbox(BoundingBox bbox, double cell = TpmsGridCell);

} // namespace Slic3r
