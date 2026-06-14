#pragma once

// ModelTransforms.hpp
//
// Shared transform helper for the SliceCore headless pipeline.
// Replicates the transform logic from the legacy CLI (src/OrcaSlicer.cpp)
// without any wxWidgets / event-loop dependency.
//
// Confirmed API sources:
//   Model::looks_like_saved_in_meters()       libslic3r/Model.hpp:1672
//   Model::convert_from_meters(bool)          libslic3r/Model.hpp:1673
//   Model::looks_like_imperial_units()        libslic3r/Model.hpp:1670
//   Model::convert_from_imperial_units(bool)  libslic3r/Model.hpp:1671
//   ModelObject::scale(double)                libslic3r/Model.hpp:496
//   ModelObject::rotate(double, Axis)         libslic3r/Model.hpp:502
//   ModelObject::ensure_on_bed()              libslic3r/Model.hpp:489
//   Geometry::deg2rad(double)                 libslic3r/Geometry.hpp:301
//   orientation::orient(ModelObject*)         libslic3r/Orient.hpp:153
//   get_bed_shape(DynamicPrintConfig&)        libslic3r/PrintConfig.hpp:2024
//   arrange_objects(Model&,bed,ArrangeParams) libslic3r/ModelArrange.hpp:33
//   duplicate(Model&,size_t,bed,ArrangeParams) libslic3r/ModelArrange.hpp:46
//
//   apply_object_placements — per-object placement API:
//   ModelInstance::set_offset(Vec3d)          libslic3r/Model.hpp:1303
//   ModelInstance::set_rotation(Vec3d)        libslic3r/Model.hpp:1309
//   ModelInstance::set_scaling_factor(Vec3d)  libslic3r/Model.hpp:1322
//   ModelInstance::set_mirror(Axis, double)   libslic3r/Model.hpp:1330
//   ModelObject::add_instance(Vec3d offset, Vec3d scaling_factor,
//                             Vec3d rotation, Vec3d mirror)  libslic3r/Model.hpp:438
//   ModelObject::bounding_box_approx()        libslic3r/Model.hpp:444
//   ModelObject::printable                    libslic3r/Model.hpp:375 (object-level)
//   ModelInstance::printable                  libslic3r/Model.hpp:1344 (instance-level)

#include "SliceTypes.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class Model;
class DynamicPrintConfig;

namespace SliceCore {

// Applies convert_unit, scale, rotate/rotate_x/rotate_y, orient, and
// ensure_on_bed in the legacy CLI order.
//
// Returns false (and populates err) if any parameter is invalid (e.g.
// scale <= 0).  Returns true on success.
bool apply_model_transforms(Model &model, const Transforms &t, std::string &err);

// Applies per-object placement descriptors from `objs` to the loaded model.
//
// skip_objects: list of 0-based model.objects indices whose every instance
//   should be marked printable=false.  NOTE: the headless convention keys by
//   loaded-order index (0-based into model.objects).  This deliberately differs
//   from the legacy CLI which may key by loaded_id; a comment in the
//   implementation documents the divergence.
//
// assemble: not portable in headless mode; the call pushes a non-fatal warning
//   into `warnings` and continues.
//
// Per-object logic (in order): printable flag, mirror, scale/uniform_scale,
//   rotation, position, explicit instances rebuild, orient, ensure_on_bed,
//   and bed-bounds advisory warning.
//
// Returns false (+ err) only for hard errors (invalid scale <= 0).
// Non-fatal issues are appended to `warnings`.
bool apply_object_placements(Model &model,
                             const std::vector<ObjectPlacement> &objs,
                             const std::vector<int> &skip_objects,
                             bool assemble,
                             const DynamicPrintConfig &cfg,
                             std::vector<std::string> &warnings,
                             std::string &err);

// Applies arrange (place objects on bed) or duplicate (copies_num > 1) if it
// is tractable from cfg.  A best-effort wrapper: if the bed shape cannot be
// resolved from cfg the function is a no-op (returns true) and logs a warning
// rather than failing hard — arrange/duplicate are non-critical compared to
// per-object transforms.
//
// Only acts when t.arrange == 1 or t.repetitions > 1.
bool apply_arrange_or_duplicate(Model &model, const Transforms &t,
                                const DynamicPrintConfig &cfg, std::string &err);

} // namespace SliceCore
} // namespace Slic3r
