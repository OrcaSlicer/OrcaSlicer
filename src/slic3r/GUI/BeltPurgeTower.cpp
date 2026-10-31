#include "BeltPurgeTower.hpp"

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "PartPlate.hpp"
#include "I18N.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

#include <boost/log/trivial.hpp>
#include <cstddef>
#include <math.h>
#include <string>
#include <utility>

namespace Slic3r {
namespace GUI {

// The classic wipe tower is disabled in belt mode (its G-code bypasses the belt
// transform), so filament-change purging is routed into this prism via
// flush_into_objects (see Print::_plan_belt_purge()). The prism is a real model
// object so it is sliced through the normal pipeline and picks up the belt
// rotation. Width across the belt is user-set (belt_purge_tower_width); height
// is sized so each tilted slicing plane's cross-section through the prism can
// absorb the worst-case purge volume of one layer; length follows the printed
// objects along the belt (plus ramp/height compensation at both tilted ends).
bool ensure_belt_purge_tower(Model &model, PartPlateList &partplate_list, ObjectList *obj_list, std::vector<BeltPurgeSignature> &sigs)
{
    auto is_prism = [](const ModelObject *mo) {
        const ConfigOption *opt = mo->config.option("belt_purge_tower_object");
        return opt != nullptr && opt->getBool();
    };

    // Every plate gets its own prism. A prism belongs to the plate it lies on; one
    // that lies on no plate is stale.
    const int plate_count = partplate_list.get_plate_count();
    sigs.resize(size_t(plate_count));
    std::vector<std::vector<int>> prisms_by_plate(static_cast<size_t>(plate_count));
    std::vector<int>              stale_prisms;
    for (int i = 0; i < (int) model.objects.size(); ++i)
        if (is_prism(model.objects[i])) {
            const int plate_idx = model.objects[i]->instances.empty() ? -1 : partplate_list.find_instance(i, 0);
            if (plate_idx >= 0 && plate_idx < plate_count)
                prisms_by_plate[size_t(plate_idx)].push_back(i);
            else
                stale_prisms.push_back(i);
        }

    // Deletes prism objects, keeping the sidebar and part plates in sync
    // (same primitives as Plater::priv::remove(), minus scene update — the
    // caller refreshes the scene). Highest index first, so the others stay valid.
    auto remove_prisms = [&](std::vector<int> idxs) {
        std::sort(idxs.begin(), idxs.end());
        for (auto it = idxs.rbegin(); it != idxs.rend(); ++it) {
            model.delete_object(size_t(*it));
            partplate_list.notify_instance_removed(*it, -1);
            obj_list->delete_object_from_list(size_t(*it));
        }
    };
    auto all_prisms = [&]() {
        std::vector<int> all = stale_prisms;
        for (const auto &v : prisms_by_plate)
            all.insert(all.end(), v.begin(), v.end());
        return all;
    };

    // Cheap early-out for non-belt printers: only belt printers ever get a belt
    // purge tower, and this runs on every background-process tick — so avoid the
    // full_config() merge below unless this is actually a belt printer. (Also
    // tidies up a stale prism if the user switched away from a belt printer.)
    {
        const auto *belt_pre = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionBool>("belt_printer");
        if (belt_pre == nullptr || !belt_pre->value) {
            std::fill(sigs.begin(), sigs.end(), BeltPurgeSignature{});
            const std::vector<int> all = all_prisms();
            if (all.empty())
                return false;
            remove_prisms(all);
            return true;
        }
    }

    // Read every sizing input from the MERGED full config — the exact same
    // config the backend slices with (BackgroundSlicingProcess::apply uses
    // preset_bundle->full_config()). Reading from the individual presets caused
    // GUI/backend mismatches: e.g. purge_in_prime_tower or a populated
    // flush_volumes_matrix present in the merged config but false/empty in the
    // preset the GUI happened to read, so the tower was sized for the small
    // prime_volume instead of the real color-change flush and could not absorb
    // it. The three names below alias the one merged config so the rest of the
    // function is unchanged.
    const DynamicPrintConfig  full_cfg       = wxGetApp().preset_bundle->full_config();
    const DynamicPrintConfig &printer_config = full_cfg;
    const DynamicPrintConfig &print_config   = full_cfg;
    const DynamicPrintConfig &project_config = full_cfg;

    const auto *belt_opt = printer_config.option<ConfigOptionBool>("belt_printer");
    const bool  belt     = belt_opt != nullptr && belt_opt->value;
    // Belt purge tower is its own type, gated by the belt-only printer option
    // enable_belt_purge_tower (not the classic process enable_prime_tower).
    const bool  prime_tower_enabled = printer_config.has("enable_belt_purge_tower") && printer_config.opt_bool("enable_belt_purge_tower");
    const auto *seq_opt   = print_config.option<ConfigOptionEnum<PrintSequence>>("print_sequence");
    const bool  by_object = seq_opt != nullptr && seq_opt->value == PrintSequence::ByObject;

    // Mixed filament slots are expanded to their physical components below.
    std::vector<unsigned char> is_mixed;
    std::vector<std::string>   comp_strs;
    if (const auto *o = full_cfg.option<ConfigOptionBools>("filament_is_mixed"))
        is_mixed = o->values;
    if (const auto *o = full_cfg.option<ConfigOptionStrings>("filament_mixed_components"))
        comp_strs = o->values;

    const double gap     = 1.0;
    const double layer_h = print_config.has("layer_height") ? print_config.opt_float("layer_height") : 0.2;

    // Belt geometry. The rotation axis is the gantry tilt axis; the belt
    // travels along the *other* horizontal axis (X-rotation -> belt along Y,
    // the CR-30 default). The purge prism is a long bar laid along the belt
    // travel direction, beside the parts. For no/Z rotation we fall back to
    // vertical slicing geometry (theta = 90 deg).
    const auto *axis_opt  = printer_config.option<ConfigOptionEnum<BeltRotationAxis>>("belt_slice_rotation");
    const auto *angle_opt = printer_config.option<ConfigOptionFloat>("belt_slice_rotation_angle");
    const BeltRotationAxis rot = axis_opt != nullptr ? axis_opt->value : BeltRotationAxis::X;
    const bool   belt_is_y = (rot != BeltRotationAxis::Y); // X / None / Z -> belt along Y
    double       theta     = M_PI / 2.;
    if ((rot == BeltRotationAxis::X || rot == BeltRotationAxis::Y) && angle_opt != nullptr && std::abs(angle_opt->value) > EPSILON)
        theta = std::clamp(Geometry::deg2rad(std::abs(angle_opt->value)), Geometry::deg2rad(5.), M_PI / 2.);
    const double sin_t = std::sin(theta);
    const double cot_t = std::cos(theta) / sin_t;

    // NOTE: both purge_in_prime_tower and single_extruder_multi_material are
    // PRINTER options (Preset.cpp s_Preset_printer_options) — read them from the
    // printer preset. Reading purge_in_prime_tower from the print preset returns
    // has()==false, collapsing use_matrix to false and sizing the tower for the
    // small prime_volume instead of the real color-change flush. This must match
    // the backend Print::_plan_belt_purge() which reads both from the merged config.
    const bool use_matrix = (printer_config.has("purge_in_prime_tower") && printer_config.opt_bool("purge_in_prime_tower"))
        && (printer_config.has("single_extruder_multi_material") && printer_config.opt_bool("single_extruder_multi_material"));
    const double prime_volume     = print_config.has("prime_volume") ? print_config.opt_float("prime_volume") : 45.;
    const double printable_height = printer_config.has("printable_height") ? printer_config.opt_float("printable_height") : 250.;

    // The bed (printable_area) is plate-local but model instances live in the
    // plate's world frame, so the plate origin is added to every bed coordinate.
    const double inset = 1.;
    BoundingBoxf bed_ext;
    if (const auto *bed_opt = printer_config.option<ConfigOptionPoints>("printable_area");
        bed_opt != nullptr && !bed_opt->values.empty())
        bed_ext = get_extents(bed_opt->values);

    // What each plate needs, decided before anything is deleted or created.
    struct Plan
    {
        bool               wanted = false;
        BeltPurgeSignature sig;
        int                n_islands = 1;
        double             w_sub = 0., length = 0., height = 0.;
        Vec3d              center = Vec3d::Zero();
    };
    std::vector<Plan> plans(static_cast<size_t>(plate_count));

    for (int plate_idx = 0; plate_idx < plate_count; ++plate_idx) {
        Plan      &plan  = plans[size_t(plate_idx)];
        PartPlate *plate = partplate_list.get_plate(plate_idx);

        // Filaments used and bounding extent of the non-prism objects on this
        // plate (1-based filament ids; volume extruder 0 = object default).
        std::set<int> filaments;
        double        x_min = std::numeric_limits<double>::max();
        double        x_max = -std::numeric_limits<double>::max();
        double        y_min = std::numeric_limits<double>::max();
        double        y_max = -std::numeric_limits<double>::max();
        double        z_max = 0.;
        bool          have_objects = false;
        if (belt && plate != nullptr) {
            for (int obj_idx = 0; obj_idx < (int) model.objects.size(); ++obj_idx) {
                const ModelObject *mo = model.objects[obj_idx];
                if (is_prism(mo))
                    continue;
                int obj_extruder = 1;
                if (const ConfigOption *opt = mo->config.option("extruder"); opt != nullptr && opt->getInt() > 0)
                    obj_extruder = opt->getInt();
                bool any_instance_on_plate = false;
                for (int inst_idx = 0; inst_idx < (int) mo->instances.size(); ++inst_idx) {
                    if (!plate->contain_instance_totally(obj_idx, inst_idx))
                        continue;
                    any_instance_on_plate = true;
                    const BoundingBoxf3 bb = mo->instance_bounding_box(inst_idx);
                    x_min = std::min(x_min, bb.min.x());
                    x_max = std::max(x_max, bb.max.x());
                    y_min = std::min(y_min, bb.min.y());
                    y_max = std::max(y_max, bb.max.y());
                    z_max = std::max(z_max, bb.max.z());
                }
                if (!any_instance_on_plate)
                    continue;
                have_objects = true;
                for (const ModelVolume *mv : mo->volumes)
                    for (int e : mv->get_extruders())
                        filaments.insert(e > 0 ? e : obj_extruder);
            }
        }

        // A mixed filament slot is VIRTUAL: it never reaches a nozzle. At slice time
        // ToolOrdering::resolve_mixed_filaments() replaces it with its physical
        // components, so the toolchanges the prism has to absorb are between those
        // components, not to the mixed slot itself. Counting the slot as a filament
        // of its own therefore over-provisions the prism by one island per mixed slot
        // -- the "extra purge tower" -- and, when every component is already used by
        // another object, by an island that can never be reached at all.
        //
        // Expand here with the same helper the backend uses (Print.cpp's sequential
        // path), so the GUI sizes the prism against the same filament set the slicer
        // will actually produce. No-op when no filament is mixed.
        if (has_any_mixed_filament(is_mixed)) {
            std::vector<unsigned int> zero_based;
            zero_based.reserve(filaments.size());
            for (int f : filaments)
                if (f > 0)
                    zero_based.push_back((unsigned int) (f - 1));
            zero_based = expand_mixed_filaments(zero_based, is_mixed, comp_strs);
            filaments.clear();
            for (unsigned int f : zero_based)
                filaments.insert((int) f + 1);
        }

        plan.wanted = belt && prime_tower_enabled && !by_object && have_objects && filaments.size() > 1;
        if (!plan.wanted)
            continue;

        // --- Sizing -----------------------------------------------------------
        const int    n_islands = std::max(1, (int) filaments.size() - 1);
        // Every disconnected island needs at least 1 mm of printable width. Honor
        // the configured total width whenever possible, but never let the island
        // layout silently grow past the footprint used for placement.
        const double min_width = n_islands + (n_islands - 1) * gap;
        const double width     = std::max(min_width,
            print_config.has("belt_purge_tower_width") ? print_config.opt_float("belt_purge_tower_width") : 35.);
        const double printable_width = width - (n_islands - 1) * gap;

        // Parts' extent along the belt-travel axis and the lateral (across-belt) axis.
        const double belt_min = belt_is_y ? y_min : x_min;
        const double belt_max = belt_is_y ? y_max : x_max;
        const double lat_min  = belt_is_y ? x_min : y_min;
        const double lat_max  = belt_is_y ? x_max : y_max;

        // Worst-case purge volume of one layer: up to (filament count - 1)
        // toolchanges, each needing the worst flush matrix entry (mirrors the
        // volume selection in Print::_plan_belt_purge()).
        double max_flush = prime_volume;
        if (use_matrix) {
            const size_t extruder_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
            const std::vector<double> matrix = get_flush_volumes_matrix(
                project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values, 0, extruder_nums);
            const auto * multi_opt = project_config.option<ConfigOptionFloats>("flush_multiplier");
            const double multiplier = multi_opt != nullptr && !multi_opt->values.empty() ? multi_opt->get_at(0) : 1.;
            const int    n_total    = (int) (std::sqrt(double(matrix.size())) + 0.5);
            double       m          = 0.;
            for (int i : filaments)
                for (int j : filaments)
                    if (i != j && i <= n_total && j <= n_total)
                        m = std::max(m, matrix[size_t(i - 1) * n_total + size_t(j - 1)]);
            if (m > 0.)
                max_flush = m * multiplier;
        }
        const double v_layer = double(filaments.size() - 1) * max_flush;

        // Height from the per-layer purge demand. A tilted slicing plane cuts a
        // printable_width x (height/sin) rectangle out of the bars, so one layer
        // slab absorbs printable_width * (height/sin) * layer_height of purge. Solve for the height that
        // holds the worst-case per-layer purge, with eta (infill/perimeter packing)
        // and a safety margin for the tilt ramps / grid-alignment slop, plus a
        // minimum so the tower is a real printable body rather than a sliver.
        const double eta    = 0.85;
        const double safety = 1.6;
        double       height = safety * v_layer * sin_t / (printable_width * layer_h * eta);
        height = std::clamp(height, 8.0, std::max(8.0, printable_height));

        // --- Idempotence (input-keyed) ----------------------------------------
        auto q = [](double v) { return std::lround(v * 10.0); }; // 0.1 mm quantization
        BeltPurgeSignature &new_sig = plan.sig;
        new_sig.valid          = true;
        new_sig.filament_count = (int) filaments.size();
        new_sig.key[0] = q(width);
        new_sig.key[1] = q(layer_h);
        new_sig.key[2] = q(height);
        new_sig.key[3] = q(belt_min);
        new_sig.key[4] = q(belt_max);
        new_sig.key[5] = q(lat_min);
        new_sig.key[6] = q(z_max);
        new_sig.key[7] = static_cast<long>(rot);
        new_sig.key[8] = std::lround(theta * 10000.0);
        new_sig.key[9] = q(lat_max);
        const Vec3d plate_origin = plate->get_origin();
        new_sig.key[10] = q(plate_origin.x());
        new_sig.key[11] = q(plate_origin.y());
        if (bed_ext.defined) {
            new_sig.key[12] = q(bed_ext.max.x());
            new_sig.key[13] = q(bed_ext.max.y());
        }

        // --- Position ----------------------------------------------------------
        // Belt-travel axis. With the mesh rotated by theta before slicing, a machine
        // point (y,z) maps to slicing-Z = y*sin(theta) + z*cos(theta). The parts
        // occupy slicing-Z in [y_min*sin, y_max*sin + z_max*cos], and the bar's
        // FULL-cross-section region (the part not in a triangular end ramp) spans
        // slicing-Z [belt_start*sin + H*cos, belt_end*sin]. Covering the parts'
        // whole band needs belt_start <= y_min - H*cot (bar's own leading ramp) and
        // belt_end >= y_max + z_max*cot (parts' top features print further up the
        // belt). The trailing z_max*cot term dominates the bar's own ramp.
        //
        // Along the belt the bar stops at the end of the plate: a longer bar cannot be
        // printed, and the cross-sections it loses there are reported by the purge
        // planner when the parts' last layers then purge more than the bar holds.
        const double margin            = 5.;
        const double ramp_compensation = height / sin_t;
        const double belt_origin  = plate_origin[belt_is_y ? 1 : 0];
        double       belt_end     = belt_max + margin + ramp_compensation + z_max * cot_t;   // + parts' top-feature belt reach
        if (bed_ext.defined)
            belt_end = std::min(belt_end, belt_origin + (belt_is_y ? bed_ext.max.y() : bed_ext.max.x()) - inset);
        const double belt_start   = std::max(belt_origin, std::min(belt_min - ramp_compensation, belt_end - 10.)); // leading ramp, toward belt origin
        const double length       = std::max(belt_end - belt_start, 10.);
        belt_end                  = belt_start + length;
        const double belt_center  = 0.5 * (belt_start + belt_end);

        // Across-belt: flush against the bed's maximum edge, inset by half the bar
        // width so the bar's far edge sits on the boundary and the whole bar stays
        // on the bed. lat_min/lat_max come from instance_bounding_box (world frame).
        const double lat_origin   = plate_origin[belt_is_y ? 0 : 1];
        double       lat_center   = lat_max + 5. + 0.5 * width; // fallback: just past the parts
        if (bed_ext.defined) {
            const double bed_lat_max = belt_is_y ? bed_ext.max.x() : bed_ext.max.y();
            lat_center = lat_origin + bed_lat_max - inset - 0.5 * width;
        }

        plan.n_islands = n_islands;
        plan.w_sub     = (width - (n_islands - 1) * gap) / n_islands;
        plan.length    = length;
        plan.height    = height;
        plan.center    = Vec3d(belt_is_y ? lat_center : belt_center,
                               belt_is_y ? belt_center : lat_center,
                               0.5 * height);
    }

    // --- Decide ---------------------------------------------------------------
    // A plate whose prism exists and matches its recorded inputs is left alone, so
    // subsequent ticks are no-ops until the parts/config actually change.
    std::vector<int> to_delete = stale_prisms;
    std::vector<int> to_create;
    for (int plate_idx = 0; plate_idx < plate_count; ++plate_idx) {
        const Plan             &plan     = plans[size_t(plate_idx)];
        const std::vector<int> &existing = prisms_by_plate[size_t(plate_idx)];
        BeltPurgeSignature     &sig      = sigs[size_t(plate_idx)];
        if (!plan.wanted) {
            sig = BeltPurgeSignature{};
            to_delete.insert(to_delete.end(), existing.begin(), existing.end());
        } else if (existing.size() == 1 && model.objects[size_t(existing.front())]->instances.size() == 1 && plan.sig == sig) {
            continue;
        } else {
            to_delete.insert(to_delete.end(), existing.begin(), existing.end());
            to_create.push_back(plate_idx);
        }
    }
    if (to_delete.empty() && to_create.empty())
        return false;

    remove_prisms(to_delete);

    // --- (Re)create -----------------------------------------------------------
    // Build the prism as N DISCONNECTED sub-bars side by side across the belt,
    // N = (filaments - 1) = the worst-case number of toolchanges on one layer.
    // Why: mark_wiping_extrusions overrides whole extrusion-entity COLLECTIONS,
    // and each disconnected island slices into its own infill collection. With a
    // single solid box there is one collection per layer, so the FIRST toolchange
    // on a layer grabs the entire collection (consuming all of it for one swap)
    // and any further swaps on that layer find nothing left and go unabsorbed
    // (the classic wipe tower hides this by spilling leftover into the real
    // tower; the belt prism has no fallback). One island per simultaneous swap
    // lets each swap claim its own island. Total lateral footprint stays `width`
    // (each island width/N wide, separated by a small gap), so per-layer capacity
    // per island ~= max_flush, matching the height sizing.
    // Minimal gap between sub-bars: they must stay just-separated so the slicer
    // keeps them as distinct islands (hence distinct infill collections, one per
    // simultaneous swap). Zero gap would union them into one collection and
    // reintroduce the multi-swap-per-layer absorption bug; a hair over ~2 line
    // widths also keeps gap-fill from bridging them. 1 mm is about as close as
    // they can butt up while staying individually purgeable.
    for (int plate_idx : to_create) {
        const Plan &plan = plans[size_t(plate_idx)];

        TriangleMesh prism_mesh;
        for (int i = 0; i < plan.n_islands; ++i) {
            const double lat_off = i * (plan.w_sub + gap);
            // Box dims: lateral = w_sub, along-belt = length, vertical = height.
            TriangleMesh box = belt_is_y ? make_cube(plan.w_sub, plan.length, plan.height)   // X = lateral, Y = belt
                                         : make_cube(plan.length, plan.w_sub, plan.height);  // X = belt,    Y = lateral
            box.translate(belt_is_y ? Vec3f((float) lat_off, 0.f, 0.f) : Vec3f(0.f, (float) lat_off, 0.f));
            prism_mesh.merge(box);
        }

        ModelObject *new_object = model.add_object();
        new_object->name        = _u8L("Belt Purge Tower");
        new_object->add_instance();
        ModelVolume *new_volume = new_object->add_volume(std::move(prism_mesh));
        new_volume->name        = new_object->name;

        auto &cfg = new_object->config;
        cfg.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
        cfg.set_key_value("flush_into_objects", new ConfigOptionBool(true));
        cfg.set_key_value("extruder", new ConfigOptionInt(1));
        // Sacrificial solid prism: one wall, no shells, dense rectilinear infill —
        // every extrusion is overriddable, so the absorbed volume matches the
        // cross-section x layer-height estimate used for the height above.
        cfg.set_key_value("wall_loops", new ConfigOptionInt(1));
        cfg.set_key_value("top_shell_layers", new ConfigOptionInt(0));
        cfg.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
        cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(100));
        cfg.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        cfg.set_key_value("enable_support", new ConfigOptionBool(false));
        cfg.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
        cfg.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
        cfg.set_key_value("precise_z_height", new ConfigOptionBool(false));

        // Position by the belt-calibration pattern: drop to the bed, then translate
        // the instance by the delta between the object's ACTUAL bbox center and the
        // target. Setting the instance offset directly is unreliable here — the
        // freshly added cube's local frame is not centered, so set_offset() lands
        // the min corner (not the center) on the target, leaving the bar centered
        // on the bed edge with half of it hanging off.
        new_object->invalidate_bounding_box();
        new_object->ensure_on_bed();
        const BoundingBoxf3 cur = new_object->bounding_box_exact();
        new_object->translate_instances(Vec3d(plan.center.x() - cur.center().x(),
                                              plan.center.y() - cur.center().y(),
                                              0.0));
        new_object->instances.front()->set_assemble_transformation(new_object->instances.front()->get_transformation());

        const size_t obj_idx = model.objects.size() - 1;
        // Registers the object in the sidebar and notifies the part plates;
        // selection is left untouched (auto-managed object).
        obj_list->add_object_to_list(obj_idx, /*call_selection_changed=*/false);

        sigs[size_t(plate_idx)] = plan.sig;
    }

    return true;
}

} // namespace GUI
} // namespace Slic3r
