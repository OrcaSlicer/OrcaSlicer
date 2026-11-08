#ifndef slic3r_FillMeshBacked_hpp_
#define slic3r_FillMeshBacked_hpp_

#include "../Arachne/WallToolPaths.hpp"
#include "../Arachne/utils/ExtrusionLine.hpp"
#include "../ClipperUtils.hpp"
#include "../PreSliceInfillStructures.hpp"
#include "../Surface.hpp"
#include "FillGyroid.hpp"
#include "FillTpmsD.hpp"
#include "FillTpmsFK.hpp"

#include <cmath>

namespace Slic3r {

// Consumes the pre-sliced infill lattice from PreSliceInfillStructures::mesh_infill
// (PrintObject::prepare_mesh_infill_data) instead of evaluating an analytic
// pattern per layer. Always used for Custom (infill mesh); routed in place of the
// analytic Gyroid/TPMS Fillers only when UNLAYERED_MESH_INFILL=1 (FillBase.cpp).
//
// Non-template wiring interface: make_fills / the anchoring path assign the
// prepared data through this base with a single dynamic_cast, independent of
// which pattern the Filler wraps.
struct MeshBackedFill
{
    const MeshInfillSlices *mesh_slices { nullptr };
    virtual ~MeshBackedFill() = default;
};

// Deriving from the analytic Filler makes the fallback exact: any surface
// the mesh path must not touch (solid, bridge, top/bottom — the factory has
// no surface context, so the gate lives here in fill_surface), any layer
// with no mesh data, a mismatched pattern or density (per-object mesh vs
// per-region config), and any generation failure all take the analytic path
// unchanged.
//
// MVP output: Surface-mode polygon boundaries are single-wall centerlines
// (routed through the analytic connector); SolidBand outlines are closed
// traces. The region's infill rotation angle is deliberately not applied —
// the mesh is absolute object-space geometry, orientation is baked in at
// generation.
template<typename BaseFill, InfillPattern PatternTag>
class FillMeshBackedT : public BaseFill, public MeshBackedFill
{
public:
    Fill* clone() const override { return new FillMeshBackedT(*this); }

    Polylines fill_surface(const Surface *surface, const FillParams &params) override
    {
        // Surface-type / pattern / density gate (plan Phase 3 step 3 and
        // open question 9 option 4): mesh-backed output replaces only true
        // sparse internal infill of the pattern and density the mesh was
        // prepared for. Everything else falls through to the analytic base
        // with m_active_slices unset.
        m_active_slices = this->armed_slices(surface, params);
        m_active_lines  = this->armed_lines(surface, params);
        Polylines out = Fill::fill_surface(surface, params);
        m_active_slices = nullptr;
        m_active_lines  = nullptr;
        return out;
    }

    // Multi-wall (SolidBand) path: the band cross-sections are ribbon
    // regions; Arachne fills each with wall_count continuous variable-width
    // walls — the same machinery that walls perimeters, with its input
    // conditioning intact. Routed by forcing use_arachne through the base
    // fill_surface_extrusion, which then calls our fill_surface_arachne.
    void fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) override
    {
        const ExPolygons *slices = this->armed_slices(surface, params);
        if (slices != nullptr && mesh_slices->mode == ImplicitMeshMode::SolidBand && mesh_slices->wall_count >= 1) {
            m_active_slices = slices;
            FillParams arachne_params = params;
            arachne_params.use_arachne = true;
            Fill::fill_surface_extrusion(surface, arachne_params, out);
            m_active_slices = nullptr;
        } else
            BaseFill::fill_surface_extrusion(surface, params, out);
    }

    ThickPolylines fill_surface_arachne(const Surface *surface, const FillParams &params) override
    {
        if (m_active_slices == nullptr || m_active_slices->empty())
            return BaseFill::fill_surface_arachne(surface, params);

        // Clip the band cross-sections to the fill region and wall every
        // ribbon in ONE WallToolPaths call — Arachne handles multi-island
        // outlines, and a single call keeps its bookkeeping global.
        ExPolygons bands = intersection_ex(*m_active_slices, ExPolygons{ surface->expolygon });
        // WallToolPaths holds its outline by reference — it must outlive
        // getToolPaths(), so no temporaries here.
        Polygons band_polygons = to_polygons(bands);
        if (band_polygons.empty())
            return {};
        const coord_t bead = params.flow.scaled_spacing();
        const double  min_nozzle_diameter = *std::min_element(this->print_config->nozzle_diameter.values.begin(),
                                                              this->print_config->nozzle_diameter.values.end());
        Arachne::WallToolPathsParams input_params;
        // Lower thin-bead limits than the perimeter defaults: the band
        // pinches at TPMS saddle points, and culling those tips both loses
        // material and fragments the walls. A 0.5*nozzle bead is still
        // printable, and this is sparse infill, not a visible perimeter.
        input_params.min_bead_width                  = 0.5 * min_nozzle_diameter;
        input_params.min_feature_size                = 0.15 * min_nozzle_diameter;
        input_params.wall_transition_length          = 0.4;
        input_params.wall_transition_angle           = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count         = 1;

        Arachne::WallToolPaths wall_tool_paths(band_polygons, bead, bead, size_t(mesh_slices->wall_count),
                                               0, params.layer_height, input_params);
        std::vector<Arachne::VariableWidthLines> loops = wall_tool_paths.getToolPaths();

        struct PendingPath { ThickPolyline pl; bool closed; };
        std::vector<PendingPath> pending;
        for (Arachne::VariableWidthLines &loop : loops)
            for (const Arachne::ExtrusionLine &wall : loop) {
                if (wall.empty())
                    continue;
                ThickPolyline tp = Arachne::to_thick_polyline(wall);
                if (tp.points.size() < 2)
                    continue;
                const bool closed = wall.is_closed && tp.points.front() == tp.points.back()
                    && tp.width.front() == tp.width.back();
                pending.push_back({ std::move(tp), closed });
            }

        // Greedy nearest-endpoint chaining across ALL ribbons: without this,
        // every ribbon fragment costs a travel (measured 13x the single-wall
        // travel count). Closed loops additionally get their seam rotated to
        // the pick-up point. O(n^2) on tens of paths per layer.
        ThickPolylines thick_polylines_out;
        thick_polylines_out.reserve(pending.size());
        Point cur = surface->expolygon.contour.points.empty() ? Point(0, 0) : surface->expolygon.contour.points.front();
        std::vector<bool> used(pending.size(), false);
        for (size_t n = 0; n < pending.size(); ++n) {
            size_t best = size_t(-1); double best_d2 = std::numeric_limits<double>::max(); bool best_rev = false;
            for (size_t i = 0; i < pending.size(); ++i) {
                if (used[i])
                    continue;
                const double d_front = (pending[i].pl.points.front() - cur).template cast<double>().squaredNorm();
                if (d_front < best_d2) { best_d2 = d_front; best = i; best_rev = false; }
                if (! pending[i].closed) {
                    const double d_back = (pending[i].pl.points.back() - cur).template cast<double>().squaredNorm();
                    if (d_back < best_d2) { best_d2 = d_back; best = i; best_rev = true; }
                }
            }
            used[best] = true;
            PendingPath &pick = pending[best];
            if (pick.closed) {
                // Rotate the loop seam to the vertex nearest the pick-up point.
                pick.pl.points.pop_back();
                int nearest_idx = cur.nearest_point_index(pick.pl.points);
                std::rotate(pick.pl.points.begin(), pick.pl.points.begin() + nearest_idx, pick.pl.points.end());
                std::rotate(pick.pl.width.begin(), pick.pl.width.begin() + 2 * nearest_idx, pick.pl.width.end());
                pick.pl.points.emplace_back(pick.pl.points.front());
                // Clip so the extruder does not land exactly on the seam.
                pick.pl.clip_end(this->loop_clipping);
            } else if (best_rev)
                pick.pl.reverse();
            if (pick.pl.is_valid()) {
                cur = pick.pl.points.back();
                thick_polylines_out.emplace_back(std::move(pick.pl));
            }
        }
        return thick_polylines_out;
    }

protected:
    void _fill_surface_single(
        const FillParams                &params,
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction,
        ExPolygon                        expolygon,
        Polylines                       &polylines_out) override
    {
        if (m_active_lines != nullptr) {
            // Sheet lattice: the pre-sliced cross-sections are the lines to print, in the
            // same frame as the fill expolygons; clip, drop crumbs, and chain.
            Polylines polylines = intersection_pl(*m_active_lines, expolygon);
            const double minlength = scale_(0.5 * this->spacing);
            polylines.erase(
                std::remove_if(polylines.begin(), polylines.end(),
                               [minlength](const Polyline &pl) { return pl.length() < minlength; }),
                polylines.end());
            this->chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);
            return;
        }
        if (m_active_slices == nullptr || m_active_slices->empty()) {
            BaseFill::_fill_surface_single(params, thickness_layers, direction, expolygon, polylines_out);
            return;
        }

        // The pre-sliced polygons are absolute object-space geometry in the
        // same frame as the fill expolygons; clipping is the only work left.
        // In Surface mode the polygon boundaries are the pattern's
        // centerlines (the AABB-cap segments lie outside every fill region
        // and vanish in the clip); in SolidBand mode they are the thin-band
        // outlines.
        Polylines polylines;
        for (const ExPolygon &ep : *m_active_slices)
            for (const Polygon &poly : to_polygons(ep)) {
                // Open each loop at a vertex OUTSIDE the fill region when one
                // exists: every clipped fragment then ends on the region
                // boundary, where the infill connector can hook it. An
                // arbitrary split point would leave one fragment per loop
                // with a dangling interior endpoint — unhookable, costing a
                // travel and a lost boundary link each.
                int split_idx = 0;
                for (size_t i = 0; i < poly.points.size(); ++i)
                    if (! expolygon.contains(poly.points[i])) {
                        split_idx = int(i);
                        break;
                    }
                polylines.emplace_back(poly.split_at_index(split_idx));
            }
        // Same multiline replication as the analytic path (the prepare step
        // already widened the period by the multiline factor).
        Slic3r::multiline_fill(polylines, params, float(this->spacing));
        polylines = intersection_pl(std::move(polylines), expolygon);

        // Same tiny-segment cleanup as the analytic path.
        const double minlength = scale_(0.8 * this->spacing);
        polylines.erase(
            std::remove_if(polylines.begin(), polylines.end(),
                           [minlength](const Polyline &pl) { return pl.length() < minlength; }),
            polylines.end());

        if (mesh_slices->mode == ImplicitMeshMode::Surface) {
            // Open centerlines: concatenate through the same connector the
            // analytic patterns use, so travel/retract behavior is comparable.
            this->chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);
        } else {
            // Closed band loops are already continuous traces.
            append(polylines_out, std::move(polylines));
        }
    }

private:
    bool armed(const Surface *surface, const FillParams &params) const
    {
        return surface->surface_type == stInternal && mesh_slices != nullptr
            && mesh_slices->pattern == PatternTag
            && std::abs(params.density - mesh_slices->prepared_density) < 1e-4f;
    }
    const ExPolygons *armed_slices(const Surface *surface, const FillParams &params) const
    {
        return armed(surface, params) && mesh_slices->mode != ImplicitMeshMode::Lines ? mesh_slices->slices_at(this->z) : nullptr;
    }
    const Polylines *armed_lines(const Surface *surface, const FillParams &params) const
    {
        return armed(surface, params) && mesh_slices->mode == ImplicitMeshMode::Lines ? mesh_slices->lines_at(this->z) : nullptr;
    }

    // Armed per fill_surface call; single-threaded within one Fill instance.
    const ExPolygons *m_active_slices { nullptr };
    const Polylines  *m_active_lines  { nullptr };
};

using FillMeshBackedGyroid = FillMeshBackedT<FillGyroid, ipGyroid>;
using FillMeshBackedTpmsD  = FillMeshBackedT<FillTpmsD,  ipTpmsD>;
using FillMeshBackedTpmsFK = FillMeshBackedT<FillTpmsFK, ipTpmsFK>;
// Custom = user mesh only; analytic fallback is gyroid (with a logged
// prompt from prepare when no infill-mesh volume exists).
using FillMeshBackedCustom = FillMeshBackedT<FillGyroid, ipCustomMesh>;

} // namespace Slic3r

#endif // slic3r_FillMeshBacked_hpp_
