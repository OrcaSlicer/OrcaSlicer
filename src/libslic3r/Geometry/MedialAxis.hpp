#ifndef slic3r_Geometry_MedialAxis_hpp_
#define slic3r_Geometry_MedialAxis_hpp_

#include <stddef.h>
#include <utility>
#include <vector>
#include <cstddef>
#include <array>

#include "Voronoi.hpp"
#include "../ExPolygon.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r::Geometry {

class MedialAxis {
public:
    // Geometry in scaled coordinates, independent of the Voronoi implementation.
    // Entries follow ThickPolyline::width: two measurements per polyline edge.
    struct WidthMeasurement {
        enum class Source { segment, start_vertex, end_vertex };
        Vec2d point = Vec2d::Zero();
        Line contour_segment;
        size_t contour_segment_index = 0;
        Source source = Source::segment;
    };
    using WidthMeasurements = std::vector<WidthMeasurement>;
    MedialAxis(double min_width, double max_width, const ExPolygon &expolygon);
    void build(ThickPolylines* polylines, std::vector<WidthMeasurements>* measurements = nullptr);
    void build(Polylines* polylines);
    
private:
    // Input
    const ExPolygon     &m_expolygon;
    Lines                m_lines;
    // for filtering of the skeleton edges
    double               m_min_width;
    double               m_max_width;

    // Voronoi Diagram.
    using VD = VoronoiDiagram;
    VD                   m_vd;

    // Annotations of the VD skeleton edges.
    struct EdgeData {
        bool    active      { false };
        double  width_start { 0 };
        double  width_end   { 0 };
    };
    // Returns a reference to EdgeData and a "reversed" boolean.
    std::pair<EdgeData&, bool> edge_data(const VD::edge_type &edge) {
        size_t edge_id = &edge - &m_vd.edges().front();
        return { m_edge_data[edge_id / 2], (edge_id & 1) != 0 };
    }
    std::vector<EdgeData> m_edge_data;
    std::vector<std::array<WidthMeasurement, 2>> m_width_measurements;

    void process_edge_neighbors(const VD::edge_type* edge, ThickPolyline* polyline,
                                WidthMeasurements* measurements);
    bool validate_edge(const VD::edge_type* edge);
};

} // namespace Slicer::Geometry

#endif // slic3r_Geometry_MedialAxis_hpp_
