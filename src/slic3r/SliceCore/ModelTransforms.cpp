// ModelTransforms.cpp
//
// Implementation of apply_model_transforms() and apply_arrange_or_duplicate().
//
// Transform order mirrors the legacy CLI (src/OrcaSlicer.cpp):
//   1. convert_unit  (~4357-4368)
//   2. scale         (~4480-4490)
//   3. rotate Z/X/Y  (~4465-4479)
//   4. orient        (~4598-4615)
//   5. ensure_on_bed (~5565-5574)
//
// Arrange / duplicate order comes after per-object transforms and before the
// PartPlateList is populated (~4400-4422 for "copy", ~4731-4855 for full
// arrange).

#include "ModelTransforms.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"      // Geometry::deg2rad
#include "libslic3r/libslic3r.h"       // Axis enum (X, Y, Z)
#include "libslic3r/Orient.hpp"        // orientation::orient(ModelObject*)
#include "libslic3r/ModelArrange.hpp"  // arrange_objects, duplicate
#include "libslic3r/PrintConfig.hpp"   // get_bed_shape(DynamicPrintConfig&)

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace SliceCore {

// ---------------------------------------------------------------------------
// apply_model_transforms
// ---------------------------------------------------------------------------

bool apply_model_transforms(Model &model, const Transforms &t, std::string &err)
{
    // 1. convert_unit — mirrors OrcaSlicer.cpp:4357-4368
    //    Only acts when t.convert_unit == true (the field is a plain bool).
    if (t.convert_unit) {
        if (model.looks_like_saved_in_meters()) {
            BOOST_LOG_TRIVIAL(info) << "[SliceCore] convert_unit: model looks like meters, converting to mm";
            model.convert_from_meters(true);
        } else if (model.looks_like_imperial_units()) {
            BOOST_LOG_TRIVIAL(info) << "[SliceCore] convert_unit: model looks like imperial units, converting to mm";
            model.convert_from_imperial_units(true);
        }
    }

    // 2. scale — mirrors OrcaSlicer.cpp:4480-4490
    //    scale == 1.0 is the identity; skip to avoid useless work.
    //    scale <= 0 is invalid (CLI exits with CLI_INVALID_PARAMS).
    if (t.scale != 1.0) {
        if (t.scale <= 0.0) {
            err = "invalid scale ratio " + std::to_string(t.scale) +
                  " (must be > 0)";
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "[SliceCore] scale: " << t.scale;
        for (ModelObject *o : model.objects)
            o->scale(t.scale);
    }

    // 3. rotate Z, X, Y — mirrors OrcaSlicer.cpp:4465-4479
    //    0.0 degrees is the identity; skip to avoid dirty transforms.
    if (t.rotate != 0.0) {
        BOOST_LOG_TRIVIAL(info) << "[SliceCore] rotate Z: " << t.rotate << " deg";
        for (ModelObject *o : model.objects)
            o->rotate(Geometry::deg2rad(t.rotate), Z);
    }
    if (t.rotate_x != 0.0) {
        BOOST_LOG_TRIVIAL(info) << "[SliceCore] rotate X: " << t.rotate_x << " deg";
        for (ModelObject *o : model.objects)
            o->rotate(Geometry::deg2rad(t.rotate_x), X);
    }
    if (t.rotate_y != 0.0) {
        BOOST_LOG_TRIVIAL(info) << "[SliceCore] rotate Y: " << t.rotate_y << " deg";
        for (ModelObject *o : model.objects)
            o->rotate(Geometry::deg2rad(t.rotate_y), Y);
    }

    // 4. orient — mirrors OrcaSlicer.cpp:4369-4398 / 4598-4615
    //    orient == 1 means "force orient all objects".
    //    orient == 0 means disabled; any other value is "auto" (leave as-is).
    if (t.orient == 1) {
        BOOST_LOG_TRIVIAL(info) << "[SliceCore] orient: applying orientation::orient() to all objects";
        for (ModelObject *o : model.objects)
            orientation::orient(o);
    }

    // 5. ensure_on_bed — mirrors OrcaSlicer.cpp:5565-5574
    if (t.ensure_on_bed) {
        BOOST_LOG_TRIVIAL(info) << "[SliceCore] ensure_on_bed: lifting all objects to z=0";
        for (ModelObject *o : model.objects)
            o->ensure_on_bed();
    }

    return true;
}

// ---------------------------------------------------------------------------
// apply_arrange_or_duplicate
// ---------------------------------------------------------------------------

bool apply_arrange_or_duplicate(Model &model, const Transforms &t,
                                const DynamicPrintConfig &cfg, std::string &err)
{
    // Nothing to do when arrange is disabled and repetitions == 1.
    const bool want_arrange    = (t.arrange == 1);
    const bool want_duplicate  = (t.repetitions > 1);

    if (!want_arrange && !want_duplicate)
        return true;

    // Build the bed polygon from the config.  get_bed_shape() returns an empty
    // Points vector if the config carries no bed shape; in that case we fall
    // back to a no-op to avoid crashing.
    Points bed_pts = get_bed_shape(cfg);
    if (bed_pts.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "[SliceCore] apply_arrange_or_duplicate: bed shape not available "
               "in config — arrange/duplicate skipped (best-effort no-op)";
        return true;   // non-fatal; caller can proceed without arrange
    }

    // Basic arrange params — mirrors OrcaSlicer.cpp:4300 / 4731 (reset).
    // The full param setup used in the full CLI arrange pass pulls many more
    // printer-capability fields (height_to_rod, clearance_radius, etc.) from
    // the config and the partplate_list; replicating the entire setup here
    // would be deeply entangled with GUI state.  We use the safe defaults
    // (min_obj_distance = 0, no rotation, no seq-print) which are sufficient
    // for simple headless jobs.
    ArrangeParams arrange_cfg;
    arrange_cfg.min_obj_distance = 0;

    try {
        if (want_duplicate) {
            // duplicate() copies the whole model t.repetitions times and arranges.
            // matches CLI "copy" path: OrcaSlicer.cpp:4410-4413.
            const size_t copies = static_cast<size_t>(t.repetitions);
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore] duplicate: " << copies << " copies";
            model.add_default_instances();
            duplicate(model, copies, bed_pts, arrange_cfg);
        } else {
            // arrange only.
            BOOST_LOG_TRIVIAL(info) << "[SliceCore] arrange_objects";
            model.add_default_instances();
            arrange_objects(model, bed_pts, arrange_cfg);
        }
    } catch (const std::exception &ex) {
        // arrange can throw if objects don't fit; treat as non-fatal warning
        // consistent with CLI best-effort strategy.
        BOOST_LOG_TRIVIAL(warning)
            << "[SliceCore] arrange/duplicate failed (non-fatal): " << ex.what();
        // do NOT set err / return false — slicing can still proceed with
        // whatever placement the model has.
    }

    return true;
}

} // namespace SliceCore
} // namespace Slic3r
