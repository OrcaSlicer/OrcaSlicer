#include "BeltPurgeTower.hpp"

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "PartPlate.hpp"
#include "I18N.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/BoundingBox.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

// The classic wipe tower is disabled in belt mode (its G-code bypasses the belt mapping), so
// filament-change purging is routed into this prism via flush_into_objects (Print::_plan_belt_purge).
// The prism is a real model object: it is sliced through the normal pipeline and picks up the belt
// rotation. Width across the belt is user-set (belt_purge_tower_width); height is sized so each
// tilted slicing plane's cross-section through the prism can absorb the worst-case purge volume of
// one layer; length follows the printed objects along the belt (plus ramp compensation at both
// tilted ends).
bool ensure_belt_purge_tower(Model &model, PartPlateList &partplate_list, ObjectList *obj_list, BeltPurgeSignature &sig)
{
    auto is_prism = [](const ModelObject *mo) {
        const ConfigOption *opt = mo->config.option("belt_purge_tower_object");
        return opt != nullptr && opt->getBool();
    };

    std::vector<int> prism_idxs;
    for (int i = 0; i < (int) model.objects.size(); ++i)
        if (is_prism(model.objects[i]))
            prism_idxs.push_back(i);

    // Deletes prism objects, keeping the sidebar and the part plates in sync (the same primitives
    // as Plater::priv::remove(), minus the scene update -- the caller refreshes the scene).
    auto remove_prisms = [&](const std::vector<int> &idxs) {
        for (auto it = idxs.rbegin(); it != idxs.rend(); ++it) {
            model.delete_object(size_t(*it));
            partplate_list.notify_instance_removed(*it, -1);
            obj_list->delete_object_from_list(size_t(*it));
        }
    };

    // Cheap early-out for non-belt printers: this runs on every background-process tick, so avoid
    // the full_config() merge below unless this is a belt printer. Also tidies up a stale prism if
    // the user switched away from a belt printer.
    {
        const auto *belt_pre = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionBool>("belt_printer");
        if (belt_pre == nullptr || ! belt_pre->value) {
            sig = BeltPurgeSignature{};
            if (prism_idxs.empty())
                return false;
            remove_prisms(prism_idxs);
            return true;
        }
    }

    // Every sizing input comes from the MERGED full config, the config the backend slices with
    // (BackgroundSlicingProcess::apply uses preset_bundle->full_config()). Reading the individual
    // presets caused GUI/backend mismatches: purge_in_prime_tower is a printer option and the flush
    // matrix a project option, so the tower was sized for the small prime volume instead of the
    // real colour-change flush.
    const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();

    const bool  prime_tower_enabled = full_cfg.has("enable_belt_purge_tower") && full_cfg.opt_bool("enable_belt_purge_tower");
    const auto *seq_opt   = full_cfg.option<ConfigOptionEnum<PrintSequence>>("print_sequence");
    const bool  by_object = seq_opt != nullptr && seq_opt->value == PrintSequence::ByObject;

    // Filaments used and bounding extent of the non-prism objects on the current plate (1-based
    // filament ids; volume extruder 0 = object default).
    PartPlate    *plate = partplate_list.get_curr_plate();
    std::set<int> filaments;
    double        x_min = std::numeric_limits<double>::max();
    double        x_max = -std::numeric_limits<double>::max();
    double        y_min = std::numeric_limits<double>::max();
    double        y_max = -std::numeric_limits<double>::max();
    double        z_max = 0.;
    bool          have_objects = false;
    if (plate != nullptr) {
        for (int obj_idx = 0; obj_idx < (int) model.objects.size(); ++obj_idx) {
            const ModelObject *mo = model.objects[obj_idx];
            if (is_prism(mo))
                continue;
            int obj_extruder = 1;
            if (const ConfigOption *opt = mo->config.option("extruder"); opt != nullptr && opt->getInt() > 0)
                obj_extruder = opt->getInt();
            bool any_instance_on_plate = false;
            for (int inst_idx = 0; inst_idx < (int) mo->instances.size(); ++inst_idx) {
                if (! plate->contain_instance_totally(obj_idx, inst_idx))
                    continue;
                any_instance_on_plate = true;
                const BoundingBoxf3 bb = mo->instance_bounding_box(inst_idx);
                x_min = std::min(x_min, bb.min.x());
                x_max = std::max(x_max, bb.max.x());
                y_min = std::min(y_min, bb.min.y());
                y_max = std::max(y_max, bb.max.y());
                z_max = std::max(z_max, bb.max.z());
            }
            if (! any_instance_on_plate)
                continue;
            have_objects = true;
            for (const ModelVolume *mv : mo->volumes)
                for (int e : mv->get_extruders())
                    filaments.insert(e > 0 ? e : obj_extruder);
        }
    }

    const bool wanted = prime_tower_enabled && ! by_object && have_objects && filaments.size() > 1;
    if (! wanted) {
        sig = BeltPurgeSignature{};
        if (prism_idxs.empty())
            return false;
        remove_prisms(prism_idxs);
        return true;
    }

    // --- Sizing -----------------------------------------------------------
    const int    n_islands = std::max(1, (int) filaments.size() - 1);
    const double gap       = 1.0;
    // Every disconnected island needs at least 1 mm of printable width. Honour the configured
    // total width whenever possible, but never let the island layout grow past the footprint.
    const double min_width = n_islands + (n_islands - 1) * gap;
    const double width     = std::max(min_width, full_cfg.has("belt_purge_tower_width") ? full_cfg.opt_float("belt_purge_tower_width") : 35.);
    const double printable_width = width - (n_islands - 1) * gap;
    const double layer_h = full_cfg.has("layer_height") ? full_cfg.opt_float("layer_height") : 0.2;

    // Belt geometry. The rotation axis is the gantry tilt axis; the belt travels along the other
    // horizontal axis (X rotation -> belt along Y). The prism is a long bar along the belt, beside
    // the parts. The layer plane's angle is the print's slicing angle. No/Z rotation falls back to
    // vertical slicing geometry (theta = 90 degrees).
    const auto *axis_opt  = full_cfg.option<ConfigOptionEnum<BeltRotationAxis>>("belt_slice_rotation");
    const auto *angle_opt = full_cfg.option<ConfigOptionFloat>("belt_slice_rotation_angle");
    const BeltRotationAxis rot = axis_opt != nullptr ? axis_opt->value : BeltRotationAxis::X;
    const bool   belt_is_y = (rot != BeltRotationAxis::Y);
    double       theta     = M_PI / 2.;
    if ((rot == BeltRotationAxis::X || rot == BeltRotationAxis::Y) && angle_opt != nullptr && std::abs(angle_opt->value) > EPSILON)
        theta = std::clamp(Geometry::deg2rad(std::abs(angle_opt->value)), Geometry::deg2rad(5.), M_PI / 2.);
    const double sin_t = std::sin(theta);
    const double cot_t = std::cos(theta) / sin_t;

    const double belt_min = belt_is_y ? y_min : x_min;
    const double belt_max = belt_is_y ? y_max : x_max;
    const double lat_max  = belt_is_y ? x_max : y_max;
    const double lat_min  = belt_is_y ? x_min : y_min;

    // Worst-case purge volume of one layer: up to (filament count - 1) toolchanges, each needing the
    // worst flush matrix entry (mirrors the volume selection in Print::_plan_belt_purge()).
    const bool use_matrix = (full_cfg.has("purge_in_prime_tower") && full_cfg.opt_bool("purge_in_prime_tower"))
        && (full_cfg.has("single_extruder_multi_material") && full_cfg.opt_bool("single_extruder_multi_material"));
    double max_flush = full_cfg.has("prime_volume") ? full_cfg.opt_float("prime_volume") : 45.;
    if (use_matrix) {
        const size_t extruder_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
        const std::vector<double> matrix = get_flush_volumes_matrix(
            full_cfg.option<ConfigOptionFloats>("flush_volumes_matrix")->values, 0, extruder_nums);
        const auto * multi_opt = full_cfg.option<ConfigOptionFloats>("flush_multiplier");
        const double multiplier = multi_opt != nullptr && ! multi_opt->values.empty() ? multi_opt->get_at(0) : 1.;
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

    // Height from the per-layer purge demand: a tilted slicing plane cuts a printable_width x
    // (height/sin) rectangle out of the bars, so one layer slab absorbs printable_width * (height/sin)
    // * layer_height of purge. Solve for the height that holds the worst-case per-layer purge, with
    // eta (infill/perimeter packing) and a safety margin for the tilt ramps, plus a minimum so the
    // tower is a real printable body rather than a sliver.
    const double eta    = 0.85;
    const double safety = 1.6;
    const double printable_height = full_cfg.has("printable_height") ? full_cfg.opt_float("printable_height") : 250.;
    double       height = safety * v_layer * sin_t / (printable_width * layer_h * eta);
    height = std::clamp(height, 8.0, std::max(8.0, printable_height));

    // --- Idempotence (input-keyed) ----------------------------------------
    auto q = [](double v) { return std::lround(v * 10.0); };
    BeltPurgeSignature new_sig;
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
    if (prism_idxs.size() == 1 && model.objects[size_t(prism_idxs.front())]->instances.size() == 1 && new_sig == sig)
        return false;   // already up to date -- do not touch the model

    // --- Position ----------------------------------------------------------
    // Belt-travel axis. A point (y, z) maps to slicing-Z = y*sin(theta) + z*cos(theta). The parts
    // occupy slicing-Z in [y_min*sin, y_max*sin + z_max*cos] and the bar's full-cross-section
    // region spans slicing-Z [belt_start*sin + H*cos, belt_end*sin]. Covering the parts' whole band
    // needs belt_start <= y_min - H*cot (the bar's own leading ramp) and belt_end >= y_max + z_max*cot
    // (the parts' top features print further up the belt).
    const double margin            = 5.;
    const double ramp_compensation = height / sin_t;
    const double belt_origin  = plate_origin[belt_is_y ? 1 : 0];
    const double belt_start   = std::max(belt_origin, belt_min - ramp_compensation);
    const double belt_end     = belt_max + margin + ramp_compensation + z_max * cot_t;
    const double length       = std::max(belt_end - belt_start, 10.);
    const double belt_center  = 0.5 * (belt_start + belt_end);

    // Across-belt: flush against the bed's maximum edge, inset by half the bar width. The bed
    // (printable_area) is plate-local but model instances live in the plate's world frame.
    const double lat_origin   = plate_origin[belt_is_y ? 0 : 1];
    const double inset        = 1.;
    double       lat_center   = lat_max + 5. + 0.5 * width;   // fallback: just past the parts
    if (const auto *bed_opt = full_cfg.option<ConfigOptionPoints>("printable_area");
        bed_opt != nullptr && ! bed_opt->values.empty()) {
        const BoundingBoxf bed_ext     = get_extents(bed_opt->values);
        const double       bed_lat_max = belt_is_y ? bed_ext.max.x() : bed_ext.max.y();
        lat_center = lat_origin + bed_lat_max - inset - 0.5 * width;
    }

    const Vec3d desired_center(belt_is_y ? lat_center : belt_center,
                               belt_is_y ? belt_center : lat_center,
                               0.5 * height);

    // N DISCONNECTED sub-bars side by side across the belt, N = (filaments - 1) = the worst-case
    // number of toolchanges on one layer. mark_wiping_extrusions overrides whole extrusion-entity
    // collections, and each island slices into its own infill collection: with one solid box the
    // first toolchange on a layer grabs the entire collection and further swaps go unabsorbed (the
    // classic tower spills leftover into itself; the prism has no fallback). A 1 mm gap keeps the
    // islands distinct without gap fill bridging them.
    const double w_sub = (width - (n_islands - 1) * gap) / n_islands;

    // --- (Re)create ---------------------------------------------------------
    if (! prism_idxs.empty())
        remove_prisms(prism_idxs);

    TriangleMesh prism_mesh;
    for (int i = 0; i < n_islands; ++i) {
        const double lat_off = i * (w_sub + gap);
        TriangleMesh box = belt_is_y ? make_cube(w_sub, length, height)   // X = lateral, Y = belt
                                     : make_cube(length, w_sub, height);  // X = belt,    Y = lateral
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
    // Sacrificial solid prism: one wall, no shells, dense rectilinear infill -- every extrusion is
    // overridable, so the absorbed volume matches the cross-section x layer-height estimate above.
    cfg.set_key_value("wall_loops", new ConfigOptionInt(1));
    cfg.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    cfg.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(100));
    cfg.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    cfg.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
    cfg.set_key_value("precise_z_height", new ConfigOptionBool(false));

    // Position: drop to the bed, then translate the instance by the delta between the object's
    // actual bbox centre and the target (the fresh cube's local frame is not centred, so a direct
    // set_offset() would land the min corner on the target).
    new_object->invalidate_bounding_box();
    new_object->ensure_on_bed();
    const BoundingBoxf3 cur = new_object->bounding_box_exact();
    new_object->translate_instances(Vec3d(desired_center.x() - cur.center().x(),
                                          desired_center.y() - cur.center().y(),
                                          0.0));
    new_object->instances.front()->set_assemble_transformation(new_object->instances.front()->get_transformation());

    const size_t obj_idx = model.objects.size() - 1;
    // Registers the object in the sidebar and notifies the part plates; the selection is left alone.
    obj_list->add_object_to_list(obj_idx, /*call_selection_changed=*/false);

    sig = new_sig;
    BOOST_LOG_TRIVIAL(debug) << "belt purge tower generated: W=" << width << " L=" << length << " H=" << height
        << " v_layer=" << v_layer << " filaments=" << filaments.size();
    return true;
}

} // namespace GUI
} // namespace Slic3r
