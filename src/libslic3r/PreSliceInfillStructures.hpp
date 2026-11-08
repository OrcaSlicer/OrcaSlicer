#ifndef slic3r_PreSliceInfillStructures_hpp_
#define slic3r_PreSliceInfillStructures_hpp_

#include "ExPolygon.hpp"
#include "Polyline.hpp"
#include "Fill/FillAdaptive.hpp"
#include "Fill/FillLightning.hpp"
#include "Fill/AdaptiveInfillField.hpp"
#include "Fill/MeshGenerator.hpp"
#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace Slic3r {

// Pre-sliced mesh-backed infill (Phase 2+). The source mesh(es) are generated
// during prepare (e.g. a thin shell of the gyroid implicit surface), sliced
// once at every layer's print_z, and the routed per-layer result stored here.
//
// Keyed by print_z rather than layer index: make_fills and the anchoring path
// compute layer_id differently (raft offset), and combine_infill collapses
// layers after this data is built — print_z is the only key both call sites
// agree on and combine does not alter.
struct MeshInfillSlices
{
    InfillPattern                 pattern { ipGyroid };
    // How the slices are meant to be consumed: Surface-mode polygon
    // boundaries ARE single-wall centerlines (open after the per-region
    // clip, so they go through the infill connector); SolidBand outlines
    // are closed thin-band traces extruded as-is.
    ImplicitMeshMode              mode { ImplicitMeshMode::Surface };
    // Density fraction (0..1) the mesh was generated at. Regions whose
    // density differs fall back to the analytic pattern (open question 9,
    // option 4); per-density meshes (option 2) are the planned successor.
    float                         prepared_density { 0.f };
    // SolidBand only: number of Arachne walls to fill each band cross-section
    // with (the band was generated wall_count extrusion-widths thick).
    int                           wall_count { 1 };
    // >= 1 mesh family; multi-mesh patterns (e.g. mesh-backed rectilinear)
    // route between families per layer at prepare time. Kept after slicing
    // for the Phase 7 debug export.
    std::vector<TriangleMesh>     source_meshes;
    std::map<coord_t, ExPolygons> slices_by_print_z;
    // Lines mode: the sheet cross-sections per layer, already chained.
    std::map<coord_t, Polylines>  lines_by_print_z;

    // Both prepare and fill read the same Layer::print_z double, so rounding
    // the scaled value is stable between the two.
    static coord_t key_for_print_z(double print_z) { return coord_t(std::lround(scale_(print_z))); }

    const ExPolygons* slices_at(double print_z) const {
        auto it = slices_by_print_z.find(key_for_print_z(print_z));
        return it == slices_by_print_z.end() ? nullptr : &it->second;
    }
    const Polylines* lines_at(double print_z) const {
        auto it = lines_by_print_z.find(key_for_print_z(print_z));
        return it == lines_by_print_z.end() ? nullptr : &it->second;
    }
};

// Unified container for pre-slice 3D infill structures owned by PrintObject.
// Populated during bridge_over_infill() (where the existing prepare_*_infill_data
// functions already live), read-only during the parallel make_fills phase.
//
// Each pattern reads only the slot it cares about; no virtual dispatch.
struct PreSliceInfillStructures
{
    // Adaptive and support cubic octrees of each region's line spacing (Orca).
    FillAdaptive::RegionOctrees adaptive_fill_octrees;
    FillLightning::GeneratorPtr lightning_generator;
    // Density fields of the adaptive infill modes in use (Orca).
    AdaptiveInfillFields        adaptive_infill_fields;
    std::unique_ptr<MeshInfillSlices> mesh_infill;
};

} // namespace Slic3r

#endif // slic3r_PreSliceInfillStructures_hpp_
