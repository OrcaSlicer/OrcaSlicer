#ifndef SLA_PAD_HPP
#define SLA_PAD_HPP

#include <vector>
#include <functional>
#include <cmath>
#include <string>

#include <libslic3r/Point.hpp>

struct indexed_triangle_set;

namespace Slic3r {

class ExPolygon;
class Polygon;
using ExPolygons = std::vector<ExPolygon>;
using Polygons = std::vector<Polygon, PointsAllocator<Polygon>>;

namespace sla {

using ThrowOnCancel = std::function<void(void)>;

/// Calculate the polygon representing the silhouette.
void pad_blueprint(
    const indexed_triangle_set &mesh,       // input mesh
    ExPolygons &        output,     // Output will be merged with
    const std::vector<float> &,     // Exact Z levels to sample
    ThrowOnCancel thrfn = [] {}); // Function that throws if cancel was requested

void pad_blueprint(
    const indexed_triangle_set &mesh,
    ExPolygons &                output,
    float         samplingheight = 0.1f,  // The height range to sample
    float         layerheight    = 0.05f, // The sampling height
    ThrowOnCancel thrfn          = [] {});

struct PadConfig {
    double wall_thickness_mm = 1.;
    double wall_height_mm = 1.;
    double max_merge_dist_mm = 50;
    double wall_slope = std::atan(1.0);          // Universal constant for Pi/4
    double brim_size_mm = 1.6;

    struct EmbedObject {
        double object_gap_mm = 1.;
        double stick_stride_mm = 10.;
        double stick_width_mm = 0.5;
        double stick_penetration_mm = 0.1;
        bool enabled = false;
        bool everywhere = false;
        operator bool() const { return enabled; }
    } embed_object;

    inline PadConfig() = default;
    inline PadConfig(double thickness,
                     double height,
                     double mergedist,
                     double slope)
        : wall_thickness_mm(thickness)
        , wall_height_mm(height)
        , max_merge_dist_mm(mergedist)
        , wall_slope(slope)
    {}

    inline double bottom_offset() const
    {
        return (wall_thickness_mm + wall_height_mm) / std::tan(wall_slope);
    }

    inline double wing_distance() const
    {
        return wall_height_mm / std::tan(wall_slope);
    }

    inline double full_height() const
    {
        return wall_height_mm + wall_thickness_mm;
    }

    /// Returns the elevation needed for compensating the pad.
    inline double required_elevation() const { return wall_thickness_mm; }

    std::string validate() const;
};

void create_pad(
    const ExPolygons &    support_contours,
    const ExPolygons &    model_contours,
    indexed_triangle_set &output_mesh,
    const PadConfig &             = PadConfig(),
    ThrowOnCancel throw_on_cancel = [] {});

} // namespace sla
} // namespace Slic3r

#endif // SLABASEPOOL_HPP
