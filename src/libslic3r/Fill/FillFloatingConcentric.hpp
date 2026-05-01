#ifndef SLIC3R_FillFloatingConcentric_HPP
#define SLIC3R_FillFloatingConcentric_HPP

#include "FillBase.hpp"
#include "FillConcentric.hpp"
#include "Arachne/WallToolPaths.hpp"

namespace Slic3r{
    struct FloatingThickline : public ThickLine
    {
        FloatingThickline(const Point& a, const Point& b, double wa, double wb, bool a_floating, bool b_floating) :ThickLine(a, b, wa, wb)
        {
            is_a_floating = a_floating;
            is_b_floating = b_floating;
        }
        bool is_a_floating;
        bool is_b_floating;
    };
    using FloatingThicklines = std::vector<FloatingThickline>;

    struct FloatingPolyline : public Polyline
    {
        std::vector<bool> is_floating;
        FloatingPolyline rebase_at(size_t idx);
    };
    using FloatingPolylines = std::vector<FloatingPolyline>;

    struct FloatingThickPolyline :public ThickPolyline
    {
        std::vector<bool> is_floating;
        FloatingThickPolyline rebase_at(size_t idx);
        FloatingThicklines floating_thicklines()const;
    };
    using FloatingThickPolylines = std::vector<FloatingThickPolyline>;

    class FillFloatingConcentric : public FillConcentric
    {
    public:
        ~FillFloatingConcentric() override = default;
        ExPolygons lower_layer_unsupport_areas;
        Polygons lower_sparse_polys;

    protected:
        Fill* clone() const override { return new FillFloatingConcentric(*this); }
#if 0
        void _fill_surface_single(
            const FillParams                &params, 
            unsigned int                     thickness_layers,
            const std::pair<float, Point>   &direction, 
            ExPolygon     		             expolygon,
            FloatingLines   &polylines_out) ;
#endif

        void _fill_surface_single(const FillParams& params,
            unsigned int                   thickness_layers,
            const std::pair<float, Point>& direction,
            ExPolygon                      expolygon,
            FloatingThickPolylines& thick_polylines_out);

        FloatingThickPolylines fill_surface_arachne_floating(const Surface* surface, const FillParams& params);

        void fill_surface_extrusion(const Surface* surface, const FillParams& params, ExtrusionEntitiesPtr& out);

        FloatingThickPolylines resplit_order_loops(Point curr_point, std::vector<const Arachne::ExtrusionLine*> all_extrusions, const ExPolygons& floating_areas, const Polygons& sparse_polys, const coord_t default_width);
#if 0
        Polylines resplit_order_loops(Point curr_point, Polygons loops, const ExPolygons& floating_areas);
#endif

        friend class Layer;
    };
}





#endif