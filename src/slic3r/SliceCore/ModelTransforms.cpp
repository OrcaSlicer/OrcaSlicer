// ModelTransforms.cpp
//
// Implementation of apply_model_transforms(), apply_object_placements(), and
// apply_arrange_or_duplicate().
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

#include <cmath>   // std::isfinite

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
// apply_object_placements
// ---------------------------------------------------------------------------
//
// Headless per-object placement.  Processes two categories in order:
//
//  A) skip_objects  — mark entire objects non-printable by 0-based index.
//     NOTE: Headless convention uses 0-based index into model.objects[], unlike
//     the legacy CLI which may key by loaded_id.  Callers of the legacy CLI that
//     relied on loaded_id semantics must re-map to 0-based indices before calling
//     this function.
//
//  B) ObjectPlacement list — for each descriptor, resolve the target object,
//     then apply printable, mirror, scale, rotation, position, explicit
//     instances, orient, and ensure_on_bed in that order.

bool apply_object_placements(Model &model,
                             const std::vector<ObjectPlacement> &objs,
                             const std::vector<int> &skip_objects,
                             bool assemble,
                             const DynamicPrintConfig &cfg,
                             std::vector<std::string> &warnings,
                             std::string &err)
{
    const int obj_count = static_cast<int>(model.objects.size());

    // --- assemble best-effort no-op ----------------------------------------
    // Assembly state (relative positions in a multi-body sub-object grouping)
    // is GUI/Plater state not portable through the headless pipeline.  We warn
    // and continue — slicing will use the individual object placements as-is.
    if (assemble) {
        warnings.push_back("assemble not supported in headless mode; "
                           "objects will be sliced at their individual placements");
        BOOST_LOG_TRIVIAL(warning)
            << "[SliceCore] apply_object_placements: assemble flag ignored";
    }

    // --- A: skip_objects ----------------------------------------------------
    for (const int id : skip_objects) {
        if (id < 0 || id >= obj_count) {
            warnings.push_back("skip_objects: index " + std::to_string(id) +
                                " out of range (model has " +
                                std::to_string(obj_count) + " objects)");
            BOOST_LOG_TRIVIAL(warning)
                << "[SliceCore] skip_objects: index " << id << " out of range";
            continue;
        }
        ModelObject *o = model.objects[id];
        // Mark every instance non-printable.  The object-level printable flag
        // (ModelObject::printable) is left true so the object still participates
        // in arrangement bounding-box queries; only instance-level printable is
        // cleared, consistent with OrcaSlicer GUI "hide/show" semantics.
        for (ModelInstance *inst : o->instances)
            inst->printable = false;
        BOOST_LOG_TRIVIAL(info)
            << "[SliceCore] skip_objects: marked object[" << id << "] \""
            << o->name << "\" non-printable";
    }

    // --- B: ObjectPlacement list --------------------------------------------
    // Build bed extents once for bounds-check advisory warnings.
    // get_bed_shape returns Points (scaled integer coordinates, 1 unit = SCALING_FACTOR mm).
    // We convert to mm via unscale<double>() (libslic3r.h:125).
    BoundingBoxf bed_bbox;
    {
        Points bed_pts = get_bed_shape(cfg);
        if (!bed_pts.empty()) {
            for (const Point &p : bed_pts)
                bed_bbox.merge(Vec2d(unscale<double>(p.x()), unscale<double>(p.y())));
        }
    }
    const bool have_bed_bbox = bed_bbox.defined;

    for (const ObjectPlacement &op : objs) {
        // --- resolve target object ---
        ModelObject *target = nullptr;

        if (op.index.has_value()) {
            const int idx = *op.index;
            if (idx >= 0 && idx < obj_count) {
                target = model.objects[idx];
            } else {
                warnings.push_back(
                    "placement: index " + std::to_string(idx) +
                    " out of range (model has " + std::to_string(obj_count) +
                    " objects); skipping");
                continue;
            }
        } else if (op.name.has_value()) {
            for (ModelObject *o : model.objects) {
                if (o->name == *op.name) {
                    target = o;
                    break;
                }
            }
            if (target == nullptr) {
                warnings.push_back(
                    "placement: no object named \"" + *op.name +
                    "\" found; skipping");
                continue;
            }
        } else {
            warnings.push_back(
                "placement not matched: ObjectPlacement has neither index nor "
                "name; skipping");
            continue;
        }

        BOOST_LOG_TRIVIAL(info)
            << "[SliceCore] apply_object_placements: processing object \""
            << target->name << "\"";

        // --- printable=false short-circuit -----------------------------------
        if (op.printable.has_value() && !*op.printable) {
            for (ModelInstance *inst : target->instances)
                inst->printable = false;
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   printable=false — all instances marked non-printable";
            continue;   // no further transforms needed for a skipped object
        }

        // --- ensure at least one instance ------------------------------------
        if (target->instances.empty()) {
            // add_instance(offset, scaling_factor, rotation, mirror)
            // Confirmed signature: ModelObject::add_instance(const Vec3d&,
            //   const Vec3d&, const Vec3d&, const Vec3d&) — Model.hpp:438.
            // Identity: offset=(0,0,0), scale=(1,1,1), rot=(0,0,0), mirror=(1,1,1).
            target->add_instance(Vec3d::Zero(), Vec3d::Ones(),
                                 Vec3d::Zero(), Vec3d::Ones());
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   added default instance (object had none)";
        }

        // --- mirror -----------------------------------------------------------
        if (op.mirror.has_value()) {
            const auto &m = *op.mirror;
            for (ModelInstance *inst : target->instances) {
                // set_mirror(Axis, double): Axis enum from libslic3r.h (X=0,Y=1,Z=2).
                // Mirrored axis: -1.0; normal axis: 1.0.
                inst->set_mirror(X, m[0] ? -1.0 : 1.0);
                inst->set_mirror(Y, m[1] ? -1.0 : 1.0);
                inst->set_mirror(Z, m[2] ? -1.0 : 1.0);
            }
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   mirror applied";
        }

        // --- scale -----------------------------------------------------------
        if (op.scale.has_value()) {
            const auto &s = *op.scale;
            if (s[0] <= 0.0 || s[1] <= 0.0 || s[2] <= 0.0) {
                err = "invalid per-axis scale for object \"" + target->name +
                      "\": all components must be > 0";
                return false;
            }
            for (ModelInstance *inst : target->instances)
                inst->set_scaling_factor(Vec3d(s[0], s[1], s[2]));
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   per-axis scale applied";
        } else if (op.uniform_scale.has_value()) {
            const double u = *op.uniform_scale;
            if (u <= 0.0) {
                err = "invalid uniform_scale for object \"" + target->name +
                      "\": must be > 0";
                return false;
            }
            for (ModelInstance *inst : target->instances)
                inst->set_scaling_factor(Vec3d(u, u, u));
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   uniform_scale=" << u << " applied";
        }

        // --- rotation --------------------------------------------------------
        if (op.rotation.has_value()) {
            const auto &r = *op.rotation;
            for (ModelInstance *inst : target->instances)
                inst->set_rotation(Vec3d(Geometry::deg2rad(r[0]),
                                         Geometry::deg2rad(r[1]),
                                         Geometry::deg2rad(r[2])));
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   rotation applied";
        }

        // --- position --------------------------------------------------------
        if (op.position.has_value() && op.instances.empty()) {
            // Global position applied to all instances; only when no explicit
            // per-instance list is provided (explicit instances carry their own
            // positions).
            const auto &p = *op.position;
            for (ModelInstance *inst : target->instances)
                inst->set_offset(Vec3d(p[0], p[1], p[2]));
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   position applied";
        }

        // --- explicit instance rebuild ----------------------------------------
        // When the caller provides an explicit instance list the object's
        // ModelInstance list is completely replaced.  One ModelInstance is
        // created per InstancePlacement entry using add_instance().
        //
        // IMPORTANT: After this call the object's instance positions are
        // authoritative; the global arrange / repetitions step in SliceService
        // must skip objects with explicit instances to avoid clobbering them.
        // The guard is implemented in SliceService::run() by checking whether
        // any ObjectPlacement has a non-empty instances vector.
        if (!op.instances.empty()) {
            target->clear_instances();
            for (const InstancePlacement &ip : op.instances) {
                // Defaults: identity transform.
                Vec3d offset(0, 0, 0);
                Vec3d scaling(1, 1, 1);
                Vec3d rotation(0, 0, 0);
                Vec3d mirror(1, 1, 1);   // add_instance takes mirror as Vec3d

                if (ip.position.has_value()) {
                    const auto &p = *ip.position;
                    offset = Vec3d(p[0], p[1], p[2]);
                }
                if (ip.scale.has_value()) {
                    const double s = *ip.scale;
                    if (s <= 0.0) {
                        err = "invalid instance scale for object \"" +
                              target->name + "\": must be > 0";
                        return false;
                    }
                    scaling = Vec3d(s, s, s);
                }
                if (ip.rotation_z.has_value()) {
                    rotation.z() = Geometry::deg2rad(*ip.rotation_z);
                }

                target->add_instance(offset, scaling, rotation, mirror);
            }
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   rebuilt " << op.instances.size()
                << " explicit instance(s)";
        }

        // --- orient ----------------------------------------------------------
        if (op.orient.has_value() && *op.orient == 1) {
            orientation::orient(target);
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   orient applied";
        }

        // --- ensure_on_bed ---------------------------------------------------
        if (op.ensure_on_bed.has_value() && *op.ensure_on_bed) {
            target->ensure_on_bed();
            BOOST_LOG_TRIVIAL(info)
                << "[SliceCore]   ensure_on_bed applied";
        }

        // --- bed bounds advisory warning -------------------------------------
        // After placement, check whether the object's approximate bounding box
        // still intersects the bed footprint.  This is advisory only — the
        // object is NOT dropped.  The renderer / print validate() will produce
        // the actionable error at slice time.
        if (have_bed_bbox) {
            const BoundingBoxf3 bb = target->bounding_box_approx();
            const Vec2d lo(bb.min.x(), bb.min.y());
            const Vec2d hi(bb.max.x(), bb.max.y());
            // Object is fully outside when its 2-D footprint doesn't overlap.
            if (hi.x() < bed_bbox.min.x() || lo.x() > bed_bbox.max.x() ||
                hi.y() < bed_bbox.min.y() || lo.y() > bed_bbox.max.y()) {
                warnings.push_back(
                    "object \"" + target->name +
                    "\" placement appears to be outside the bed footprint "
                    "(advisory only — slicing continues)");
                BOOST_LOG_TRIVIAL(warning)
                    << "[SliceCore]   object \"" << target->name
                    << "\" outside bed bounds (advisory)";
            }
        }
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
