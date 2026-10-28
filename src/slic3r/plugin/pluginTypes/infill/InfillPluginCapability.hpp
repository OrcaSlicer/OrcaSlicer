#pragma once

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace pybind11 { class module_; class object; }

namespace Slic3r {

// Snapshot of one Fill::_fill_surface_single() call, handed to a Python infill capability.
//
// This is a value copy, not a view into the slicing graph: the Python object owns its own copy for
// the whole call, so a plugin may keep or mutate it after generate() returns without touching the
// live print. The request deliberately carries no PrintConfig/PrintRegionConfig pointer; only the
// resolved values a pattern needs. A plugin that wants another config option should read it through
// the normal capability config API instead of reaching into the printer configuration.
//
// Units:
//   * expolygon, direction_origin                 - scaled integer coordinates (coord_t, 1e-6 mm
//                                                   normally, but read the live SCALING_FACTOR; use
//                                                   orca.slicing.unscale() to get millimetres)
//   * z, spacing, overlap, layer_height, anchors,
//     flow_width, flow_height, resolution         - millimetres
//   * angle, direction_angle                      - radians, CCW, 0 = +X
//   * density                                     - fraction in <0, 1>
//
// The expolygon is already offset by Fill::fill_surface() for overlap/solid spacing and translated
// into the pattern's working frame. It is the region this call must fill; the host does not clip
// the returned paths again.
struct InfillPluginRequest
{
    ExPolygon    expolygon;
    size_t       layer_id{ 0 };
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
    // Cross-section of the extrusion this fill will be converted to.
    double       flow_width{ 0. };
    double       flow_height{ 0. };

    bool         dont_adjust{ true };
    bool         monotonic{ false };
    bool         complete{ false };
    bool         use_arachne{ false };
    double       smooth_factor{ 0. };

    // Same predicates as FillParams::full_infill()/dont_connect().
    bool full_infill() const { return density > 0.9999f; }
    bool dont_connect() const { return anchor_length_max < 0.05f; }
};

// Long-lived infill provider. One instance per plugin capability; it does not itself derive from
// Fill and holds no per-surface state. Dispatch creates a Fill adapter per use through
// create_fill(), so the same provider serves every surface and layer.
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

    // sparse_infill_pattern stores the capability name itself (PluginCapabilityInterface::name()),
    // exactly like slicing_pipeline_plugin. get_name() is the only pattern key; there is no separate
    // identifier to declare, cache or validate. The loader still rejects an empty name or one that
    // shadows a built-in pattern, and the generic per-type uniqueness check rejects duplicates.

    // Python contract: synchronous, called on the slicing worker thread from Fill's narrow
    // _fill_surface_single() hook. Return one entry per open path, with vertices in scaled integer
    // coordinates (the same units as request.expolygon). An empty list is a valid "no infill here".
    // Exceptions abort this fill and are reported as a non-critical slicing error; do not call UI
    // from here.
    //
    // `request` is a value snapshot valid for the duration of the call (and owned by Python for as
    // long as the plugin keeps it). A concrete C++ provider may also implement this directly.
    virtual Polylines generate(const InfillPluginRequest& request) = 0;

    // Optional Fill semantics, forwarded by InfillPluginFill and resolved through the trampoline
    // (so a Python override runs under the audited GIL path). Defaults match Fill's C++ defaults:
    // the host may chain/sort the paths, and generate() runs in surface coordinates rather than in
    // an island frame shifted to the Fill bounding-box center.
    //
    // no_sort(): sets Fill::no_sort() on the extrusion collection Fill builds from generate()'s
    // paths, keeping the plugin's order through the G-code path planner.
    virtual bool no_sort() const { return false; }
    // aligned_to_origin(): makes Fill::fill_surface() translate each island so the Fill bounding-box
    // center is at the origin while generate() runs, then translates the returned paths back. The
    // request expolygon is already in that shifted frame when this is true.
    virtual bool aligned_to_origin() const { return false; }

    // Create a fresh Fill adapter that keeps this capability alive and routes every surface through
    // generate(). `self` is the shared_ptr the caller already owns; pass it so the adapter can
    // outlive a registry lookup without relying on enable_shared_from_this (the loader's aliasing
    // shared_ptr does not initialize it). The returned Fill is never cached or shared: one adapter
    // per fill_surface() pass, owned by the caller just like a built-in filler. The caller still
    // owns the normal Fill setup (layer_id, z, angle, spacing, bounding_box, print_config,
    // print_object_config, link_max_length, ...), exactly as for a built-in filler.
    Fill* create_fill(std::shared_ptr<InfillPluginCapability> self) const;
};

// Native Fill adapter for one InfillPluginCapability. It overrides only the narrow per-island hook,
// so Fill::fill_surface() still performs the overlap offset and Fill::fill_surface_extrusion() still
// converts the returned paths to extrusion entities, applies flow, and runs gap fill. The adapter
// owns its capability so a plugin cannot be unloaded while a Fill still references it.
class InfillPluginFill final : public Fill
{
public:
    explicit InfillPluginFill(std::shared_ptr<InfillPluginCapability> capability);

    Fill* clone() const override;

    bool is_self_crossing() override { return false; }

    // Forwarded to the capability; both affect Fill's base algorithm, not just the hook.
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
};

// Convert a Python generate() result into Polylines. Accepted per top-level path:
//   * an (N, 2) int64 ndarray in scaled coordinates (the host geometry input convention),
//   * an orca.host.Polygon (its points are taken as one open path),
//   * a sequence of orca.host.Point.
// Raises ValueError with the offending element's description on malformed input. Declared here
// because the trampoline needs it; defined in InfillPluginCapability.cpp.
Polylines infill_paths_from_python(const pybind11::object& paths);

} // namespace Slic3r
