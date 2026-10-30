#pragma once

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/InfillPurpose.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <libslic3r/libslic3r.h>
#include <libslic3r/ExtrusionEntity.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pybind11 { class module_; class object; }

namespace Slic3r {

// One selectable pattern advertised by an infill capability. `id` is a stable, nonlocalized key
// persisted in the corresponding pattern setting; `display_name` is only for UI presentation.
// A capability may advertise multiple patterns, each with its own supported surface purposes.
struct InfillPluginPattern
{
    std::string                       id;
    std::string                       display_name;
    std::vector<InfillSurfacePurpose> supported_surfaces;
};

inline bool infill_pattern_supports_surface(const InfillPluginPattern& pattern, InfillSurfacePurpose purpose)
{
    return std::find(pattern.supported_surfaces.begin(), pattern.supported_surfaces.end(), purpose) !=
           pattern.supported_surfaces.end();
}

// Owned snapshot of FillParams::flow. A plugin never sees the live Flow object; the resolved scalars
// are copied into the request so it stays valid after generate() returns. mm3_per_mm is the resolved
// cross-section area and is zero when the host left the flow unset (Flow::mm3_per_mm() rejects a
// non-positive extrusion).
struct InfillPluginFlow
{
    float  width{ 0.f };
    float  height{ 0.f };
    float  spacing{ 0.f };
    float  nozzle_diameter{ 0.f };
    bool   bridge{ false };
    double mm3_per_mm{ 0. };
};

// Snapshot of one Fill::_fill_surface_single() call, handed to a Python infill capability.
//
// This is a value copy, not a view into the slicing graph: Python owns its own copy for the whole
// call, so a plugin may keep it after generate() returns without touching the live print. The
// request deliberately carries no PrintConfig/PrintRegionConfig pointer; only the resolved values a
// pattern needs. Additional resolved settings can be added here without exposing the live slicing
// graph.
//
// Apart from the internal PrintRegionConfig pointer, every FillParams value is mirrored here with
// the same meaning. `pattern` is the host pattern enum (InfillPattern.Plugin for plugin dispatch);
// the selected plugin pattern ID is the pattern_id argument of generate().
//
// Identity: `object_id`, `layer_index`, `region_id` and `region_ids` are value copies of the Fill's
// creation context. They let a plugin key a per-object cache without holding a Print/Object pointer.
// `layer_id` remains the raw Fill layer id (global in normal infill, object-relative while anchoring),
// so cache lookups should use `layer_index` instead. A grouped call fills surfaces merged from every
// region in `region_ids` at once and must not be attributed to `region_id` alone.
//
// Units:
//   * expolygon, direction_origin,
//     horiz_move, symmetric_y_axis,
//     infill_lock_depth, skin_infill_depth        - scaled integer coordinates (coord_t; read the live
//                                                   SCALING_FACTOR or use orca.slicing.unscale())
//   * z, spacing, overlap, layer_height, anchors,
//     no_extrusion_overlap, resolution,
//     flow.*                                      - millimetres (flow.mm3_per_mm is mm^3/mm)
//   * angle, direction_angle                      - radians, CCW, 0 = +X
//   * density, adaptive_infill_interior_density   - fraction in <0, 1>
//   * smooth_factor                               - factor in <0, 1>
//   * lateral_lattice_angle_1/2,
//     infill_overhang_angle                       - degrees
//
// The expolygon is already offset by Fill::fill_surface() for overlap/solid spacing and translated
// into the pattern's working frame. It is the region this call must fill; the host does not clip
// the returned paths again.
struct InfillPluginRequest
{
    ExPolygon    expolygon;
    size_t       layer_id{ 0 };
    // Object identity of the Fill's creation context, or -1 when the Fill has no PrintObject.
    // This is PrintObject::id().id, the id of the object that owns the layer (the same value Python
    // usually sees as ctx.object.id()); it is a value, not a handle, and a plugin must still treat
    // a cached state as stale when the print changes.
    int64_t object_id{ -1 };
    // Zero-based index in PrintObject::layers() of the layer this fill belongs to, or -1 when the
    // Fill has no object layer. It matches the layer order Python sees in ctx.object.layers() and
    // differs from layer_id, which is Layer::id() and starts at the raft-layer count.
    int64_t layer_index{ -1 };
    // Index into Layer::regions() of the region whose settings produced this fill, or -1 when the
    // Fill has no owning region. When a grouped surface merged several regions, this is the first
    // (representative) region whose settings the host used and the region that receives the
    // generated extrusions; region_ids lists every contributor.
    int region_id{ -1 };
    // Every region whose fill surfaces contributed to this call, in ascending region order, with
    // region_id first. A non-grouped fill contains just region_id. Empty when no region identity is
    // available. A grouped fill is generated once for all listed regions, so region_id alone must
    // not be used to attribute the result exclusively to that region.
    std::vector<int> region_ids;
    unsigned int thickness_layers{ 1 };
    // Z of the top print surface (Fill::z).
    coordf_t     z{ 0. };
    // Infill spacing and infill/perimeter overlap as set on the Fill object.
    coordf_t     spacing{ 0. };
    coordf_t     overlap{ 0. };
    // Current fill angle, and the concrete direction/origin chosen for this surface by Fill.
    float        angle{ 0.f };
    float        direction_angle{ 0.f };
    Point        direction_origin{ 0, 0 };

    float        density{ 0.f };
    int          multiline{ 1 };
    float        anchor_length{ 1000.f };
    float        anchor_length_max{ 1000.f };
    double       resolution{ 0.0125 };
    coordf_t     layer_height{ 0. };
    // Resolved extrusion this fill will be converted to.
    InfillPluginFlow flow;
    // Compatibility shortcuts equal to flow.width / flow.height, kept from the original request.
    double       flow_width{ 0. };
    double       flow_height{ 0. };

    bool         dont_adjust{ true };
    bool         monotonic{ false };
    bool         complete{ false };
    bool         use_arachne{ false };
    double       smooth_factor{ 0. };

    // Pattern-specific values, mirrored from FillParams.
    bool gyroid_optimized{ false };
    AdaptiveInfillMode adaptive_infill_mode{ AdaptiveInfillMode::Disabled };
    float adaptive_infill_interior_density{ 0.f };
    AdaptiveInfillGradient adaptive_infill_gradient{ AdaptiveInfillGradient::Linear };
    coordf_t lateral_lattice_angle_1{ 0. };
    coordf_t lateral_lattice_angle_2{ 0. };
    InfillPattern pattern{ ipRectilinear };
    float infill_overhang_angle{ 60.f };
    ExtrusionRole extrusion_role{ ExtrusionRole(0) };
    bool using_internal_flow{ false };
    float no_extrusion_overlap{ 0.f };
    bool dont_sort{ false };
    bool can_reverse{ true };
    SurfaceFillOrder fill_order{ SurfaceFillOrder::Default };
    // Scaled x offset applied to the pattern to stagger it between layers.
    float horiz_move{ 0.f };
    bool symmetric_infill_y_axis{ false };
    bool infill_complete_top{ false };
    // Scaled x center that the region was mirrored about when symmetric_infill_y_axis is set.
    coord_t symmetric_y_axis{ 0 };
    bool locked_zag{ false };
    float infill_lock_depth{ 0.f };
    float skin_infill_depth{ 0.f };
    CenterOfSurfacePattern center_of_surface_pattern{ CenterOfSurfacePattern::Each_Surface };

    // The setting category that selected the pattern for this call. Set from the per-surface
    // dispatch state; plugins must not infer it from density, extrusion role, or pattern ID.
    InfillSurfacePurpose purpose{ InfillSurfacePurpose::Sparse };

    // Same predicates as FillParams::full_infill()/dont_connect().
    bool full_infill() const { return density > 0.9999f; }
    bool dont_connect() const { return anchor_length_max < 0.05f; }
};

// Assemble the owned request snapshot from one Fill/_fill_surface_single() call. This is the single
// place FillParams and Fill state become the plugin contract, so native providers and tests can use
// the same mapping as the built-in adapter.
InfillPluginRequest make_infill_plugin_request(const Fill&                    fill,
                                               const FillParams&              params,
                                               unsigned int                   thickness_layers,
                                               const std::pair<float, Point>& direction,
                                               ExPolygon                      expolygon);

// Long-lived infill provider. One instance may advertise multiple patterns through patterns(); each
// pattern ID is selected independently and may support one or more surface purposes. The provider
// does not itself derive from Fill and holds no per-surface state. Dispatch creates a Fill adapter
// per use through create_fill(), so the same provider can serve every pattern, surface and layer.
//
// Concurrency: PrintObject::infill() already runs Layer::make_fills() under tbb::parallel_for, so
// generate() is entered concurrently on several slicing threads. A Python implementation is safe by
// construction: every trampoline call acquires the GIL, so Python bodies serialize unless the plugin
// explicitly releases it. A native C++ implementation must be thread-safe on its own.
class InfillPluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::Infill; }

    // get_name() identifies this provider capability. Each item returned by patterns() has a
    // separate stable pattern ID. Pattern IDs must be unique within the provider and across every
    // loaded infill provider, and must not collide with built-in pattern keys.

    // Describe every selectable pattern this provider implements. Surface-purpose support is
    // declared per pattern because patterns from one provider may support different surfaces.
    //
    // Optional. The default keeps a plugin written before multi-pattern support working as one
    // sparse pattern whose ID is the capability name.
    virtual std::vector<InfillPluginPattern> patterns() const;

    // Python contract: synchronous, called on the slicing worker thread from Fill's narrow
    // _fill_surface_single() hook. `pattern_id` is one of the IDs returned by patterns(). Return one
    // entry per open path, with vertices in scaled integer coordinates (the same units as
    // request.expolygon). An empty list is a valid "no infill here". Exceptions abort this fill and
    // are reported as a non-critical slicing error; do not call UI from here.
    //
    // `request` is a value snapshot valid for the duration of the call (and owned by Python for as
    // long as the plugin keeps it). A concrete C++ provider may also implement this directly.
    //
    // Python compatibility: the current form is generate(self, pattern_id, request). A plugin
    // written before multi-pattern support may keep generate(self, request); the trampoline decides
    // from the callable's signature, without calling it twice. An override taking *args receives
    // the current (pattern_id, request) form.
    virtual Polylines generate(const std::string& pattern_id, const InfillPluginRequest& request) = 0;

    // Optional Fill semantics, forwarded by InfillPluginFill and resolved through the trampoline
    // (so a Python override runs under the audited GIL path). Defaults match Fill's C++ defaults:
    // the host may chain/sort the paths, and generate() runs in surface coordinates rather than in
    // an island frame shifted to the Fill bounding-box center.
    //
    // Python compatibility: no_sort(self) and aligned_to_origin(self) (no pattern ID) remain
    // accepted; *args overrides receive the current pattern_id argument.
    //
    // no_sort(): sets Fill::no_sort() on the extrusion collection Fill builds from generate()'s
    // paths, keeping this pattern's order through the G-code path planner.
    virtual bool no_sort(const std::string& pattern_id) const { (void) pattern_id; return false; }
    // aligned_to_origin(): makes Fill::fill_surface() translate each island so the Fill bounding-box
    // center is at the origin while generate() runs, then translates the returned paths back. The
    // request expolygon is already in that shifted frame when this is true.
    virtual bool aligned_to_origin(const std::string& pattern_id) const { (void) pattern_id; return false; }

    // Descriptors resolved once by the loader, under the GIL that materialized the capability.
    // Plain C++ reads: UI and lifecycle code never need to enter Python just to list patterns.
    const std::vector<InfillPluginPattern>& resolved_patterns() const { return m_patterns; }
    void set_resolved_patterns(std::vector<InfillPluginPattern> patterns) { m_patterns = std::move(patterns); }
    const InfillPluginPattern* find_resolved_pattern(const std::string& pattern_id) const;

    // Create a fresh Fill adapter that keeps this capability alive and routes every surface through
    // generate(). `self` is the shared_ptr the caller already owns; pass it so the adapter can
    // outlive a registry lookup without relying on enable_shared_from_this (the loader's aliasing
    // shared_ptr does not initialize it). The returned Fill is never cached or shared: one adapter
    // per fill_surface() pass, owned by the caller just like a built-in filler. The caller still
    // owns the normal Fill setup (layer_id, z, angle, spacing, bounding_box, print_config,
    // print_object_config, link_max_length, ...), exactly as for a built-in filler.
    Fill* create_fill(std::shared_ptr<InfillPluginCapability> self, std::string pattern_id) const;

private:
    std::vector<InfillPluginPattern> m_patterns;
};

// Native Fill adapter for one InfillPluginCapability. It overrides only the narrow per-island hook,
// so Fill::fill_surface() still performs the overlap offset and Fill::fill_surface_extrusion() still
// converts the returned paths to extrusion entities, applies flow, and runs gap fill. The adapter
// owns its capability so a plugin cannot be unloaded while a Fill still references it.
class InfillPluginFill final : public Fill
{
public:
    explicit InfillPluginFill(std::shared_ptr<InfillPluginCapability> capability,
                              std::string pattern_id);

    Fill* clone() const override;

    bool is_self_crossing() override { return false; }

    // Forwarded to the selected pattern; both affect Fill's base algorithm, not just the hook.
    bool no_sort() const override;
    bool aligned_to_origin() const override;

protected:
    void _fill_surface_single(const FillParams& params,
                              unsigned int      thickness_layers,
                              const std::pair<float, Point>& direction,
                              ExPolygon         expolygon,
                              Polylines&        polylines_out) override;

    // Plugin infill emits plain centre lines only; the Arachne variable-width path has no
    // equivalent in the Python contract, so reject it instead of silently producing no infill.
    void _fill_surface_single(const FillParams& params,
                              unsigned int      thickness_layers,
                              const std::pair<float, Point>& direction,
                              ExPolygon         expolygon,
                              ThickPolylines&   thick_polylines_out) override;

private:
    std::shared_ptr<InfillPluginCapability> m_capability;
    std::string                            m_pattern_id;
};

// Register every pattern this capability advertises in the host's InfillFactory. Called by
// PluginManager while the capability is loaded and enabled. Each pattern's declared surface
// purposes become its registry metadata, so a dropdown and the project preflight can restrict it to
// the settings it was written for. Registration is all-or-nothing: a rejected ID rolls back the
// entries added for this capability.
//
// Each registration also publishes one provider lease that keeps this capability (and therefore its
// Python module) alive while the patterns are registered and, through a slicing job's selection,
// until that job stops creating Fills. That is what lets an unload wait for the job instead of
// tearing the module down under it.
bool register_infill_patterns(const std::shared_ptr<InfillPluginCapability>& capability, std::string& error);

// Remove every registry entry owned by this exact capability identity. An entry pinned by a
// slicing job is retired rather than erased, so the in-flight job keeps its provider identity and
// its provider lease.
void unregister_infill_patterns(const std::string& plugin_key, const std::string& capability_name);

// Convert a Python generate() result into Polylines. Accepted per top-level path:
//   * an (N, 2) int64 ndarray in scaled coordinates (the host geometry input convention),
//   * an orca.host.Polygon (its points are taken as one open path),
//   * a sequence of orca.host.Point.
// Raises ValueError with the offending element's description on malformed input. Declared here
// because the trampoline needs it; defined in InfillPluginCapability.cpp.
Polylines infill_paths_from_python(const pybind11::object& paths);

} // namespace Slic3r
