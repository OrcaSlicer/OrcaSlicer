#include "Config.hpp"
#include "Exception.hpp"
#include "Print.hpp"
#include "BoundingBox.hpp"
#include "Brim.hpp"
#include "ClipperUtils.hpp"
#include "Extruder.hpp"
#include "Flow.hpp"
#include "Geometry/ConvexHull.hpp"
#include "I18N.hpp"
#include "ShortestPath.hpp"
#include "Thread.hpp"
#include "Time.hpp"
#include "GCode.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "Utils.hpp"
#include "PrintConfig.hpp"
#include "MaterialType.hpp"
#include "Model.hpp"
#include "format.hpp"
#include <float.h>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/regex.hpp>
#include <boost/nowide/fstream.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

//BBS: add json support
#include "nlohmann/json.hpp"

#include "GCode/ConflictChecker.hpp"
#include "ParameterUtils.hpp"

#include <codecvt>

using namespace nlohmann;

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

template class PrintState<PrintStep, psCount>;
template class PrintState<PrintObjectStep, posCount>;

PrintRegion::PrintRegion(const PrintRegionConfig &config) : PrintRegion(config, config.hash()) {}
PrintRegion::PrintRegion(PrintRegionConfig &&config) : PrintRegion(std::move(config), config.hash()) {}

//BBS
// ORCA: Now this is a parameter
//float Print::min_skirt_length = 0;

struct FilamentType {
    std::string name;
    int min_temp;
    int max_temp;
    std::string temp_type;
};

void Print::clear()
{
	std::scoped_lock<std::mutex> lock(this->state_mutex());
    // The following call should stop background processing if it is running.
    this->invalidate_all_steps();
	for (PrintObject *object : m_objects)
		delete object;
	m_objects.clear();
    m_print_regions.clear();
    m_model.clear_objects();
    m_statistics_by_extruder_count.clear();
}

bool Print::has_tpu_filament() const
{
    for (unsigned int filament_id : m_wipe_tower_data.tool_ordering.all_extruders()) {
        std::string filament_name = m_config.filament_type.get_at(filament_id);
        if (filament_name == "TPU") {
            return true;
        }
    }
    return false;
}

// Called by Print::apply().
// This method only accepts PrintConfig option keys.
bool Print::invalidate_state_by_config_options(const ConfigOptionResolver & /* new_config */, const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    // Cache the plenty of parameters, which influence the G-code generator only,
    // or they are only notes not influencing the generated G-code.
    static std::unordered_set<std::string> steps_gcode = {
        //BBS
        "additional_cooling_fan_speed",
        "reduce_crossing_wall",
        "max_travel_detour_distance",
        "printable_area",
        //BBS: add bed_exclude_area
        "bed_exclude_area",
        "thumbnail_size",
        "before_layer_change_gcode",
        "enable_pressure_advance",
        "pressure_advance",
        "enable_overhang_bridge_fan",
        "overhang_fan_speed",
        "overhang_fan_threshold",
        "slow_down_for_layer_cooling",
        "default_acceleration",
        "deretraction_speed",
        "close_fan_the_first_x_layers",
        "machine_end_gcode",
        "printing_by_object_gcode",
        "filament_end_gcode",
        "post_process",
        "extruder_clearance_height_to_rod",
        "extruder_clearance_height_to_lid",
        "extruder_clearance_radius",
        "nozzle_height",
        "extruder_colour",
        "extruder_offset",
        "filament_flow_ratio",
        "reduce_fan_stop_start_freq",
        "dont_slow_down_outer_wall",
        "fan_cooling_layer_time",
        "full_fan_speed_layer",
        "fan_kickstart",
        "fan_speedup_overhangs",
        "fan_speedup_time",
        "filament_colour",
        "default_filament_colour",
        "filament_diameter",
         "volumetric_speed_coefficients",
        "filament_density",
        "filament_cost",
        "filament_notes",
        "outer_wall_acceleration",
        "inner_wall_acceleration",
        "initial_layer_acceleration",
        "top_surface_acceleration",
        "bridge_acceleration",
        "travel_acceleration",
        "sparse_infill_acceleration",
        "internal_solid_infill_acceleration",
        // BBS
        "supertack_plate_temp_initial_layer",
        "cool_plate_temp_initial_layer",
        "textured_cool_plate_temp_initial_layer",
        "eng_plate_temp_initial_layer",
        "hot_plate_temp_initial_layer",
        "textured_plate_temp_initial_layer",
        "gcode_add_line_number",
        "layer_change_gcode",
        "time_lapse_gcode",
        "wrapping_detection_gcode",
        "fan_min_speed",
        "fan_max_speed",
        "printable_height",
        "slow_down_min_speed",
        "max_volumetric_extrusion_rate_slope",
        "max_volumetric_extrusion_rate_slope_segment_length",
        "extrusion_rate_smoothing_external_perimeter_only",
        "reduce_infill_retraction",
        "filename_format",
        "retraction_minimum_travel",
        "retract_before_wipe",
        "retract_when_changing_layer",
        "retraction_length",
        "retract_length_toolchange",
        "z_hop",
        "travel_slope",
        "retract_lift_above",
        "retract_lift_below", 
        "retract_lift_enforce",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retraction_speed",
        "use_firmware_retraction",
        "slow_down_layer_time",
        "standby_temperature_delta",
        "preheat_time",
        "preheat_steps",
        "machine_start_gcode",
        "filament_start_gcode",
        "change_filament_gcode",
        "wipe",
        // BBS
        "wipe_distance",
        "curr_bed_type",
        "nozzle_volume",
        "nozzle_hrc",
        "required_nozzle_HRC",
        "upward_compatible_machine",
        "is_infill_first",
        // Orca
        "chamber_temperature",
        "thumbnails",
        "thumbnails_format",
        "seam_gap",
        "role_based_wipe_speed",
        "wipe_speed",
        "use_relative_e_distances",
        "accel_to_decel_enable",
        "accel_to_decel_factor",
        "wipe_on_loops",
        "gcode_comments",
        "gcode_label_objects", 
        "exclude_object",
        "support_material_interface_fan_speed",
        "internal_bridge_fan_speed", // ORCA: Add support for separate internal bridge fan speed control
        "ironing_fan_speed",
        "single_extruder_multi_material_priming",
        "activate_air_filtration",
        "during_print_exhaust_fan_speed",
        "complete_print_exhaust_fan_speed",
        "activate_chamber_temp_control",
        "manual_filament_change",
        "disable_m73",
        "use_firmware_retraction",
        "enable_long_retraction_when_cut",
        "long_retractions_when_cut",
        "retraction_distances_when_cut",
        "filament_long_retractions_when_cut",
        "filament_retraction_distances_when_cut",
        "grab_length",
        "bed_temperature_formula",
        "filament_notes",
        "process_notes",
        "printer_notes"
    };

    static std::unordered_set<std::string> steps_ignore;

    std::vector<PrintStep> steps;
    std::vector<PrintObjectStep> osteps;
    bool invalidated = false;

    for (const t_config_option_key &opt_key : opt_keys) {
        if (steps_gcode.find(opt_key) != steps_gcode.end()) {
            // These options only affect G-code export or they are just notes without influence on the generated G-code,
            // so there is nothing to invalidate.
            steps.emplace_back(psGCodeExport);
        } else if (steps_ignore.find(opt_key) != steps_ignore.end()) {
            // These steps have no influence on the G-code whatsoever. Just ignore them.
        } else if (
               opt_key == "skirt_type"
            || opt_key == "skirt_loops"
            || opt_key == "skirt_speed"
            || opt_key == "skirt_height"
            || opt_key == "min_skirt_length"
            || opt_key == "single_loop_draft_shield"
            || opt_key == "draft_shield"
            || opt_key == "skirt_distance"
            || opt_key == "skirt_start_angle"
            || opt_key == "ooze_prevention"
            || opt_key == "wipe_tower_x"
            || opt_key == "wipe_tower_y"
            || opt_key == "wipe_tower_rotation_angle") {
            steps.emplace_back(psSkirtBrim);
        } else if (
               opt_key == "initial_layer_print_height"
            || opt_key == "nozzle_diameter"
            || opt_key == "filament_shrink"
            || opt_key == "filament_shrinkage_compensation_z"
            || opt_key == "resolution"
            || opt_key == "precise_z_height"
            // Spiral Vase forces different kind of slicing than the normal model:
            // In Spiral Vase mode, holes are closed and only the largest area contour is kept at each layer.
            // Therefore toggling the Spiral Vase on / off requires complete reslicing.
            || opt_key == "spiral_mode") {
            osteps.emplace_back(posSlice);
        } else if (
               opt_key == "print_sequence"
            || opt_key == "filament_type"
            || opt_key == "chamber_temperature"
            || opt_key == "nozzle_temperature_initial_layer"
            || opt_key == "filament_minimal_purge_on_wipe_tower"
            || opt_key == "filament_max_volumetric_speed"
            || opt_key == "filament_adaptive_volumetric_speed"
            || opt_key == "filament_loading_speed"
            || opt_key == "filament_loading_speed_start"
            || opt_key == "filament_unloading_speed"
            || opt_key == "filament_unloading_speed_start"
            || opt_key == "filament_toolchange_delay"
            || opt_key == "filament_cooling_moves"
            || opt_key == "filament_stamping_loading_speed"
            || opt_key == "filament_stamping_distance"
            || opt_key == "filament_cooling_initial_speed"
            || opt_key == "filament_cooling_final_speed"
            || opt_key == "filament_ramming_parameters"
            || opt_key == "filament_multitool_ramming"
            || opt_key == "filament_multitool_ramming_volume"
            || opt_key == "filament_multitool_ramming_flow"
            || opt_key == "filament_max_volumetric_speed"
            || opt_key == "gcode_flavor"
            || opt_key == "single_extruder_multi_material"
            || opt_key == "nozzle_temperature"
            // BBS
            || opt_key == "supertack_plate_temp"
            || opt_key == "cool_plate_temp"
            || opt_key == "textured_cool_plate_temp"
            || opt_key == "eng_plate_temp"
            || opt_key == "hot_plate_temp"
            || opt_key == "textured_plate_temp"
            || opt_key == "enable_prime_tower"
            || opt_key == "enable_wrapping_detection"
            || opt_key == "prime_tower_enable_framework"
            || opt_key == "prime_tower_width"
            || opt_key == "prime_tower_brim_width"
            || opt_key == "prime_tower_skip_points"
            || opt_key == "prime_tower_flat_ironing"
            || opt_key == "first_layer_print_sequence"
            || opt_key == "other_layers_print_sequence"
            || opt_key == "other_layers_print_sequence_nums" 
            || opt_key == "extruder_ams_count"
            || opt_key == "filament_map_mode"
            || opt_key == "filament_map"
            || opt_key == "filament_adhesiveness_category"
            || opt_key == "wipe_tower_bridging"
            || opt_key == "wipe_tower_extra_flow"
            || opt_key == "wipe_tower_no_sparse_layers"
            || opt_key == "flush_volumes_matrix"
            || opt_key == "prime_volume"
            || opt_key == "flush_into_infill"
            || opt_key == "flush_into_support"
            || opt_key == "initial_layer_infill_speed"
            || opt_key == "travel_speed"
            || opt_key == "travel_speed_z"
            || opt_key == "initial_layer_speed"
            || opt_key == "initial_layer_travel_speed"
            || opt_key == "slow_down_layers"
            || opt_key == "idle_temperature"
            || opt_key == "wipe_tower_cone_angle"
            || opt_key == "wipe_tower_extra_spacing"
            || opt_key == "wipe_tower_max_purge_speed"
            || opt_key == "wipe_tower_wall_type"
            || opt_key == "wipe_tower_extra_rib_length"
            || opt_key == "wipe_tower_rib_width"
            || opt_key == "wipe_tower_fillet_wall"
            || opt_key == "wipe_tower_filament"
            || opt_key == "wiping_volumes_extruders"
            || opt_key == "enable_filament_ramming"
            || opt_key == "purge_in_prime_tower"
            || opt_key == "z_offset"
            || opt_key == "support_multi_bed_types"
            ) {
            steps.emplace_back(psWipeTower);
            steps.emplace_back(psSkirtBrim);
        } else if (opt_key == "filament_soluble"
                || opt_key == "filament_is_support"
                || opt_key == "filament_printable"
                || opt_key == "filament_change_length"
                || opt_key == "independent_support_layer_height") {
            steps.emplace_back(psWipeTower);
            // Soluble support interface / non-soluble base interface produces non-soluble interface layers below soluble interface layers.
            // Thus switching between soluble / non-soluble interface layer material may require recalculation of supports.
            //FIXME Killing supports on any change of "filament_soluble" is rough. We should check for each object whether that is necessary.
            osteps.emplace_back(posSupportMaterial);
            osteps.emplace_back(posSimplifySupportPath);
        } else if (
               opt_key == "initial_layer_line_width"
            || opt_key == "min_layer_height"
            || opt_key == "max_layer_height"
            //|| opt_key == "resolution"
            //BBS: when enable arc fitting, we must re-generate perimeter
            || opt_key == "enable_arc_fitting"
            || opt_key == "print_order"
            || opt_key == "wall_sequence") {
            osteps.emplace_back(posPerimeters);
            osteps.emplace_back(posEstimateCurledExtrusions);
            osteps.emplace_back(posInfill);
            osteps.emplace_back(posSupportMaterial);
			osteps.emplace_back(posSimplifyPath);
            osteps.emplace_back(posSimplifyInfill);
            osteps.emplace_back(posSimplifySupportPath);
            steps.emplace_back(psSkirtBrim);
        }
        else if (opt_key == "z_hop_types") {
            osteps.emplace_back(posDetectOverhangsForLift);
        } else {
            // for legacy, if we can't handle this option let's invalidate all steps
            //FIXME invalidate all steps of all objects as well?
            invalidated |= this->invalidate_all_steps();
            // Continue with the other opt_keys to possibly invalidate any object specific steps.
        }
    }

    sort_remove_duplicates(steps);
    for (PrintStep step : steps)
        invalidated |= this->invalidate_step(step);
    sort_remove_duplicates(osteps);
    for (PrintObjectStep ostep : osteps)
        for (PrintObject *object : m_objects)
            invalidated |= object->invalidate_step(ostep);

    return invalidated;
}

void Print::set_calib_params(const Calib_Params& params) {
    m_calib_params = params;
    m_calib_params.mode = params.mode;
}

bool Print::invalidate_step(PrintStep step)
{
	bool invalidated = Inherited::invalidate_step(step);
    // Propagate to dependent steps.
    if (step != psGCodeExport)
        invalidated |= Inherited::invalidate_step(psGCodeExport);
    return invalidated;
}

// returns true if an object step is done on all objects
// and there's at least one object
bool Print::is_step_done(PrintObjectStep step) const
{
    if (m_objects.empty())
        return false;
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (const PrintObject *object : m_objects)
        if (! object->is_step_done_unguarded(step))
            return false;
    return true;
}

// returns 0-based indices of used extruders
std::vector<unsigned int> Print::object_extruders() const
{
    std::vector<unsigned int> extruders;
    extruders.reserve(m_print_regions.size() * m_objects.size() * 3);

    //Orca: Collect extruders from all regions.
    for (const PrintObject *object : m_objects)
		for (const PrintRegion &region : object->all_regions())
        	region.collect_object_printing_extruders(*this, extruders);

    for (const PrintObject* object : m_objects) {
        const ModelObject* mo = object->model_object();
        for (const ModelVolume* mv : mo->volumes) {
            std::vector<int> volume_extruders = mv->get_extruders();
            for (int extruder : volume_extruders) {
                assert(extruder > 0);
                extruders.push_back(extruder - 1);
            }
        }

        // layer range
        for (auto layer_range : mo->layer_config_ranges) {
            if (layer_range.second.has("extruder")) {
                //BBS: actually when user doesn't change filament by height range(value is default 0), height range should not save key "extruder".
                //Don't know why height range always save key "extruder" because of no change(should only save difference)...
                //Add protection here to avoid overflow
                auto value = layer_range.second.option("extruder")->getInt();
                if (value > 0)
                    extruders.push_back(value - 1);
            }
        }
    }
    sort_remove_duplicates(extruders);
    return extruders;
}

// returns 0-based indices of used extruders
std::vector<unsigned int> Print::support_material_extruders() const
{
    std::vector<unsigned int> extruders;
    bool support_uses_current_extruder = false;
    // BBS
    auto num_extruders = (unsigned int)m_config.filament_diameter.size();

    for (PrintObject *object : m_objects) {
        if (object->has_support_material()) {
        	assert(object->config().support_filament >= 0);
            if (object->config().support_filament == 0)
                support_uses_current_extruder = true;
            else {
            	unsigned int i = (unsigned int)object->config().support_filament - 1;
                extruders.emplace_back((i >= num_extruders) ? 0 : i);
            }
        	assert(object->config().support_interface_filament >= 0);
            if (object->config().support_interface_filament == 0)
                support_uses_current_extruder = true;
            else {
            	unsigned int i = (unsigned int)object->config().support_interface_filament - 1;
                extruders.emplace_back((i >= num_extruders) ? 0 : i);
            }
        }
    }

    if (support_uses_current_extruder)
        // Add all object extruders to the support extruders as it is not know which one will be used to print supports.
        append(extruders, this->object_extruders());

    sort_remove_duplicates(extruders);
    return extruders;
}

// returns 0-based indices of used extruders
std::vector<unsigned int> Print::extruders(bool conside_custom_gcode) const
{
    std::vector<unsigned int> extruders = this->object_extruders();
    append(extruders, this->support_material_extruders());

    if (conside_custom_gcode) {
        //BBS
        int num_extruders = m_config.filament_colour.size();
        if (m_model.plates_custom_gcodes.find(m_model.curr_plate_index) != m_model.plates_custom_gcodes.end()) {
            for (auto item : m_model.plates_custom_gcodes.at(m_model.curr_plate_index).gcodes) {
                if (item.type == CustomGCode::Type::ToolChange && item.extruder <= num_extruders)
                    extruders.push_back((unsigned int)(item.extruder - 1));
            }
        }
    }

    sort_remove_duplicates(extruders);
    return extruders;
}

unsigned int Print::num_object_instances() const
{
	unsigned int instances = 0;
    for (const PrintObject *print_object : m_objects)
        instances += (unsigned int)print_object->instances().size();
    return instances;
}

double Print::max_allowed_layer_height() const
{
    double nozzle_diameter_max = 0.;
    for (unsigned int extruder_id : this->extruders())
        nozzle_diameter_max = std::max(nozzle_diameter_max, m_config.nozzle_diameter.get_at(extruder_id));
    return nozzle_diameter_max;
}

std::vector<ObjectID> Print::print_object_ids() const
{
    std::vector<ObjectID> out;
    // Reserve one more for the caller to append the ID of the Print itself.
    out.reserve(m_objects.size() + 1);
    for (const PrintObject *print_object : m_objects)
        out.emplace_back(print_object->id());
    return out;
}

bool Print::has_infinite_skirt() const
{
    // Orca: unclear why (m_config.ooze_prevention && this->extruders().size() > 1) logic is here, removed.
    // return (m_config.draft_shield == dsEnabled && m_config.skirt_loops > 0) || (m_config.ooze_prevention && this->extruders().size() > 1);

    return (m_config.draft_shield == dsEnabled && m_config.skirt_loops > 0);
}

bool Print::has_skirt() const
{
    return (m_config.skirt_height > 0);
}

bool Print::has_brim() const
{
    return std::any_of(m_objects.begin(), m_objects.end(), [](PrintObject *object) { return object->has_brim(); });
}

//BBS
std::vector<size_t> Print::layers_sorted_for_object(float start, float end, std::vector<LayerPtrs> &layers_of_objects, std::vector<BoundingBox> &boundingBox_for_objects, VecOfPoints &objects_instances_shift)
{
    std::vector<size_t> idx_of_object_sorted;
    size_t              idx = 0;
    for (const auto &object : m_objects) {
        idx_of_object_sorted.push_back(idx++);
        object->get_certain_layers(start, end, layers_of_objects, boundingBox_for_objects);
    }
    std::sort(idx_of_object_sorted.begin(), idx_of_object_sorted.end(),
              [boundingBox_for_objects](auto left, auto right) { return boundingBox_for_objects[left].area() > boundingBox_for_objects[right].area(); });

    objects_instances_shift.clear();
    objects_instances_shift.reserve(m_objects.size());
    for (const auto& object : m_objects)
        objects_instances_shift.emplace_back(object->get_instances_shift_without_plate_offset());

    return idx_of_object_sorted;
};

StringObjectException Print::sequential_print_clearance_valid(const Print &print, Polygons *polygons, std::vector<std::pair<Polygon, float>>* height_polygons)
{
    StringObjectException single_object_exception;
    const auto& print_config = print.config();
    Polygons exclude_polys = get_bed_excluded_area(print_config);
    const Vec3d print_origin = print.get_plate_origin();
    std::for_each(exclude_polys.begin(), exclude_polys.end(),
                  [&print_origin](Polygon& p) { p.translate(scale_(print_origin.x()), scale_(print_origin.y())); });

    std::map<ObjectID, Polygon> map_model_object_to_convex_hull;
    struct print_instance_info
    {
        const PrintInstance *print_instance;
        BoundingBox    bounding_box;
        Polygon        hull_polygon;
        int                  object_index;
        double         arrange_score;
        double               height;
    };
    auto find_object_index = [](const Model& model, const ModelObject* obj) {
        for (int index = 0; index < model.objects.size(); index++)
        {
            if (model.objects[index] == obj)
                return index;
        }
        return -1;
    };

    auto [object_skirt_offset, _] = print.object_skirt_offset();
    std::vector<struct print_instance_info> print_instance_with_bounding_box;
    {
        // sequential_print_horizontal_clearance_valid
        Polygons convex_hulls_other;
        if (polygons != nullptr)
            polygons->clear();
        std::vector<size_t> intersecting_idxs;

        // Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
        // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
        float obj_distance = print.is_all_objects_are_short() ? scale_(std::max(0.5f * MAX_OUTER_NOZZLE_DIAMETER, object_skirt_offset) - 0.1) : scale_(0.5 * print.config().extruder_clearance_radius.value + object_skirt_offset - 0.1);

        for (const PrintObject *print_object : print.objects()) {
            assert(! print_object->model_object()->instances.empty());
            assert(! print_object->instances().empty());
            ObjectID model_object_id = print_object->model_object()->id();
            auto it_convex_hull = map_model_object_to_convex_hull.find(model_object_id);
            // Get convex hull of all printable volumes assigned to this print object.
            ModelInstance *model_instance0 = print_object->model_object()->instances.front();
            if (it_convex_hull == map_model_object_to_convex_hull.end()) {
                // Calculate the convex hull of a printable object.
                // Grow convex hull with the clearance margin.
                // FIXME: Arrangement has different parameters for offsetting (jtMiter, limit 2)
                // which causes that the warning will be showed after arrangement with the
                // appropriate object distance. Even if I set this to jtMiter the warning still shows up.
                it_convex_hull = map_model_object_to_convex_hull.emplace_hint(it_convex_hull, model_object_id,
                            print_object->model_object()->convex_hull_2d(Geometry::assemble_transform(
                            { 0.0, 0.0, model_instance0->get_offset().z() }, model_instance0->get_rotation(), model_instance0->get_scaling_factor(), model_instance0->get_mirror())));
            }
            // Make a copy, so it may be rotated for instances.
            Polygon convex_hull0 = it_convex_hull->second;
            const double z_diff = Geometry::rotation_diff_z(model_instance0->get_rotation(), print_object->instances().front().model_instance->get_rotation());
            if (std::abs(z_diff) > EPSILON)
                convex_hull0.rotate(z_diff);
            // Now we check that no instance of convex_hull intersects any of the previously checked object instances.
            for (const PrintInstance &instance : print_object->instances()) {
                Polygon convex_hull_no_offset = convex_hull0, convex_hull;
                auto tmp = offset(convex_hull_no_offset, obj_distance, jtRound, scale_(0.1));
                if (!tmp.empty()) { // tmp may be empty due to clipper's bug, see STUDIO-2452
                    convex_hull = tmp.front();
                    // instance.shift is a position of a centered object, while model object may not be centered.
                    // Convert the shift from the PrintObject's coordinates into ModelObject's coordinates by removing the centering offset.
                    convex_hull.translate(instance.shift - print_object->center_offset());
                }
                convex_hull_no_offset.translate(instance.shift - print_object->center_offset());
                //juedge the exclude area
                if (!intersection(exclude_polys, convex_hull_no_offset).empty()) {
                    if (single_object_exception.string.empty()) {
                        single_object_exception.string = (boost::format(L("%1% is too close to exclusion area, there may be collisions when printing.")) %instance.model_instance->get_object()->name).str();
                        single_object_exception.object = instance.model_instance->get_object();
                    }
                    else {
                        single_object_exception.string += "\n"+(boost::format(L("%1% is too close to exclusion area, there may be collisions when printing.")) %instance.model_instance->get_object()->name).str();
                        single_object_exception.object = nullptr;
                    }
                    //if (polygons) {
                    //    intersecting_idxs.emplace_back(convex_hulls_other.size());
                    //}
                }

                // if output needed, collect indices (inside convex_hulls_other) of intersecting hulls
                for (size_t i = 0; i < convex_hulls_other.size(); ++i) {
                    if (! intersection(convex_hulls_other[i], convex_hull).empty()) {
                        bool has_exception = false;
                        if (single_object_exception.string.empty()) {
                            single_object_exception.string = (boost::format(L("%1% is too close to others, and collisions may be caused.")) %instance.model_instance->get_object()->name).str();
                            single_object_exception.object = instance.model_instance->get_object();
                            has_exception                  = true;
                        }
                        else {
                            single_object_exception.string += "\n"+(boost::format(L("%1% is too close to others, and collisions may be caused.")) %instance.model_instance->get_object()->name).str();
                            single_object_exception.object = nullptr;
                            has_exception                  = true;
                        }

                        if (polygons) {
                            intersecting_idxs.emplace_back(i);
                            intersecting_idxs.emplace_back(convex_hulls_other.size());
                        }

                        if (has_exception) break;
                    }
                }
                struct print_instance_info print_info {&instance, convex_hull.bounding_box(), convex_hull};
                print_info.height = instance.print_object->height();
                print_info.object_index = find_object_index(print.model(), print_object->model_object());
                print_instance_with_bounding_box.push_back(std::move(print_info));
                convex_hulls_other.emplace_back(std::move(convex_hull));
            }
        }
        if (!intersecting_idxs.empty()) {
            // use collected indices (inside convex_hulls_other) to update output
            std::sort(intersecting_idxs.begin(), intersecting_idxs.end());
            intersecting_idxs.erase(std::unique(intersecting_idxs.begin(), intersecting_idxs.end()), intersecting_idxs.end());
            for (size_t i : intersecting_idxs) {
                polygons->emplace_back(std::move(convex_hulls_other[i]));
            }
        }
    }

    // calc sort order
    double hc1              = scale_(print.config().extruder_clearance_height_to_lid); // height to lid
    double hc2              = scale_(print.config().extruder_clearance_height_to_rod); // height to rod
    double printable_height = scale_(print.config().printable_height);

#if 0 //do not sort anymore, use the order in object list
    auto bed_points = get_bed_shape(print_config);
    float bed_width = bed_points[1].x() - bed_points[0].x();
    // 如果扩大以后的多边形的距离小于这个值，就需要严格保证从左到右的打印顺序，否则会撞工具头右侧
    float unsafe_dist = scale_(print_config.extruder_clearance_max_radius.value - print_config.extruder_clearance_radius.value);
    struct VecHash
    {
        size_t operator()(const Vec2i32 &n1) const
        {
            return std::hash<coord_t>()(int(n1(0) * 100 + 100)) + std::hash<coord_t>()(int(n1(1) * 100 + 100)) * 101;
        }
    };
    std::unordered_set<Vec2i32, VecHash> left_right_pair; // pairs in this vector must strictly obey the left-right order
    for (size_t i = 0; i < print_instance_with_bounding_box.size();i++) {
        auto &inst         = print_instance_with_bounding_box[i];
        inst.index         = i;
        Point pt           = inst.bounding_box.center();
        inst.arrange_score = pt.x() / 2 + pt.y(); // we prefer print row-by-row, so cost on x-direction is smaller
    }
    for (size_t i = 0; i < print_instance_with_bounding_box.size(); i++) {
        auto &inst         = print_instance_with_bounding_box[i];
        auto &l            = print_instance_with_bounding_box[i];
        for (size_t j = 0; j < print_instance_with_bounding_box.size(); j++) {
            if (j != i) {
                auto &r        = print_instance_with_bounding_box[j];
                auto ly1       = l.bounding_box.min.y();
                auto ly2       = l.bounding_box.max.y();
                auto ry1       = r.bounding_box.min.y();
                auto ry2       = r.bounding_box.max.y();
                auto lx1       = l.bounding_box.min.x();
                auto rx1       = r.bounding_box.min.x();
                auto lx2       = l.bounding_box.max.x();
                auto rx2       = r.bounding_box.max.x();
                auto inter_min = std::max(ly1, ry1);
                auto inter_max = std::min(ly2, ry2);
                auto inter_y   = inter_max - inter_min;

                // 如果y方向的重合超过轮廓的膨胀量，说明两个物体在一行，应该先打左边的物体，即先比较二者的x坐标。
                // If the overlap in the y direction exceeds the expansion of the contour, it means that the two objects are in a row and the object on the left should be hit first, that is, the x coordinates of the two should be compared first.
                if (inter_y > scale_(0.5 * print.config().extruder_clearance_radius.value)) {
                    if (std::max(rx1 - lx2, lx1 - rx2) < unsafe_dist) {
                        if (lx1 > rx1) {
                            left_right_pair.insert({j, i});
                            BOOST_LOG_TRIVIAL(debug) << "in-a-row, print_instance " << r.print_instance->model_instance->get_object()->name << "(" << r.arrange_score << ")"
                                                     << " -> " << l.print_instance->model_instance->get_object()->name << "(" << l.arrange_score << ")";
                        } else {
                            left_right_pair.insert({i, j});
                            BOOST_LOG_TRIVIAL(debug) << "in-a-row, print_instance " << l.print_instance->model_instance->get_object()->name << "(" << l.arrange_score << ")"
                                                     << " -> " << r.print_instance->model_instance->get_object()->name << "(" << r.arrange_score << ")";
                        }
                    }
                }
                if (l.height > hc1 && r.height < hc1) {
                    // 当前物体超过了顶盖高度，必须后打
                    left_right_pair.insert({j, i});
                    BOOST_LOG_TRIVIAL(debug) << "height>hc1, print_instance " << r.print_instance->model_instance->get_object()->name << "(" << r.arrange_score << ")"
                                             << " -> " << l.print_instance->model_instance->get_object()->name << "(" << l.arrange_score << ")";
                }
                else if (l.height > hc2 && l.height > r.height && l.arrange_score<r.arrange_score) {
                    // 如果当前物体的高度超过滑杆，且比r高，就给它加一点代价，尽量让高的物体后打（只有物体高度超过滑杆时才有必要按高度来）
                    if (l.arrange_score < r.arrange_score)
                        l.arrange_score = r.arrange_score + 10;
                    BOOST_LOG_TRIVIAL(debug) << "height>hc2, print_instance " << inst.print_instance->model_instance->get_object()->name
                                             << ", right=" << r.print_instance->model_instance->get_object()->name << ", l.score: " << l.arrange_score
                                             << ", r.score: " << r.arrange_score;
                }
            }
        }
    }
    // 多做几次代价传播，因为前一次有些值没有更新。
    // TODO 更好的办法是建立一颗树，一步到位。不过我暂时没精力搞，先就这样吧
    for (int k=0;k<5;k++)
    for (auto p : left_right_pair) {
        auto &l = print_instance_with_bounding_box[p(0)];
        auto &r = print_instance_with_bounding_box[p(1)];
        if(r.arrange_score<l.arrange_score)
            r.arrange_score = l.arrange_score + 10;
    }

    BOOST_LOG_TRIVIAL(debug) << "bed width: " << unscale_(bed_width) << ", unsafe_dist:" << unscale_(unsafe_dist) << ", height_to_lid: " << unscale_(hc1) << ", height_to_rod:" << unscale_(hc2) << ", final dependency:";
    for (auto p : left_right_pair) {
        auto &l         = print_instance_with_bounding_box[p(0)];
        auto &r         = print_instance_with_bounding_box[p(1)];
        BOOST_LOG_TRIVIAL(debug) << "print_instance " << I18N::translate(l.print_instance->model_instance->get_object()->name) << "(" << l.arrange_score << ")"
                                 << " -> " << I18N::translate(r.print_instance->model_instance->get_object()->name) << "(" << r.arrange_score << ")";
    }
    // sort the print instance
    std::sort(print_instance_with_bounding_box.begin(), print_instance_with_bounding_box.end(),
        [](print_instance_info& l, print_instance_info& r) {return l.arrange_score < r.arrange_score;});

    for (auto &inst : print_instance_with_bounding_box)
        BOOST_LOG_TRIVIAL(debug) << "after sorting print_instance " << inst.print_instance->model_instance->get_object()->name << ", score: " << inst.arrange_score
                                 << ", height:"<< inst.height;
#else
    // sort the print instance
    std::sort(print_instance_with_bounding_box.begin(), print_instance_with_bounding_box.end(),
        [](print_instance_info& l, print_instance_info& r) {return l.object_index < r.object_index;});

    for (auto &inst : print_instance_with_bounding_box)
        BOOST_LOG_TRIVIAL(debug) << "after sorting print_instance " << inst.print_instance->model_instance->get_object()->name << ", object_index: " << inst.object_index
                                 << ", height:"<< inst.height;

#endif
    // sequential_print_vertical_clearance_valid
    {
        // Ignore the last instance printed.
        //print_instance_with_bounding_box.pop_back();
        /*bool has_interlaced_objects = false;
        for (int k = 0; k < print_instance_count; k++)
        {
            auto inst = print_instance_with_bounding_box[k].print_instance;
            auto bbox = print_instance_with_bounding_box[k].bounding_box;
            auto iy1 = bbox.min.y();
            auto iy2 = bbox.max.y();

            for (int i = 0; i < k; i++)
            {
                auto& p = print_instance_with_bounding_box[i].print_instance;
                auto bbox2 = print_instance_with_bounding_box[i].bounding_box;
                auto py1 = bbox2.min.y();
                auto py2 = bbox2.max.y();
                auto inter_min = std::max(iy1, py1); // min y of intersection
                auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
                if (inter_max - inter_min > 0) {
                    has_interlaced_objects = true;
                    break;
                }
            }
            if (has_interlaced_objects)
                break;
        }*/

        // if objects are not overlapped on y-axis, they will not collide even if they are taller than extruder_clearance_height_to_rod
        int print_instance_count = print_instance_with_bounding_box.size();
        std::map<const PrintInstance*, std::pair<Polygon, float>> too_tall_instances;
        for (int k = 0; k < print_instance_count; k++)
        {
            auto inst = print_instance_with_bounding_box[k].print_instance;
            // 只需要考虑喷嘴到滑杆的偏移量，这个比整个工具头的碰撞半径要小得多
            // Only the offset from the nozzle to the slide bar needs to be considered, which is much smaller than the collision radius of the entire tool head.
            auto bbox = print_instance_with_bounding_box[k].bounding_box.inflated(-scale_(0.5 * print.config().extruder_clearance_radius.value + object_skirt_offset));
            auto iy1 = bbox.min.y();
            auto iy2 = bbox.max.y();
            (const_cast<ModelInstance*>(inst->model_instance))->arrange_order = k+1;
            double height = (k == (print_instance_count - 1))?printable_height:hc1;
            /*if (has_interlaced_objects) {
                if ((k < (print_instance_count - 1)) && (inst->print_object->height() > hc2)) {
                    too_tall_instances[inst] = std::make_pair(print_instance_with_bounding_box[k].hull_polygon, unscaled<double>(hc2));
                }
            }
            else {
                if ((k < (print_instance_count - 1)) && (inst->print_object->height() > hc1)) {
                    too_tall_instances[inst] = std::make_pair(print_instance_with_bounding_box[k].hull_polygon, unscaled<double>(hc1));
                }
            }*/

            for (int i = k+1; i < print_instance_count; i++)
            {
                auto& p = print_instance_with_bounding_box[i].print_instance;
                auto bbox2 = print_instance_with_bounding_box[i].bounding_box;
                auto py1 = bbox2.min.y();
                auto py2 = bbox2.max.y();
                auto inter_min = std::max(iy1, py1); // min y of intersection
                auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
                if (inter_max - inter_min > 0) {
                    height = hc2;
                    break;
                }
            }
            if (height < inst->print_object->max_z())
                too_tall_instances[inst] = std::make_pair(print_instance_with_bounding_box[k].hull_polygon, unscaled<double>(height));
        }

        if (too_tall_instances.size() > 0) {
            //return {, inst->model_instance->get_object()};
            for (auto& iter: too_tall_instances) {
                if (single_object_exception.string.empty()) {
                    single_object_exception.string = (boost::format(L("%1% is too tall, and collisions will be caused.")) %iter.first->model_instance->get_object()->name).str();
                    single_object_exception.object = iter.first->model_instance->get_object();
                }
                else {
                    single_object_exception.string += "\n" + (boost::format(L("%1% is too tall, and collisions will be caused.")) %iter.first->model_instance->get_object()->name).str();
                    single_object_exception.object = nullptr;
                }
                if (height_polygons)
                    height_polygons->emplace_back(std::move(iter.second));
            }
        }
    }

    return single_object_exception;
}

//BBS
static StringObjectException layered_print_cleareance_valid(const Print &print, StringObjectException *warning)
{
    std::vector<const PrintInstance*> print_instances_ordered = sort_object_instances_by_model_order(print, true);
    if (print_instances_ordered.size() < 1)
        return {};

    const auto& print_config = print.config();
    Polygons exclude_polys = get_bed_excluded_area(print_config);
    const Vec3d print_origin = print.get_plate_origin();
    std::for_each(exclude_polys.begin(), exclude_polys.end(),
                  [&print_origin](Polygon& p) { p.translate(scale_(print_origin.x()), scale_(print_origin.y())); });

    Pointfs wrapping_detection_area = print_config.wrapping_exclude_area.values;
    Polygon wrapping_poly;
    for (size_t i = 0; i < wrapping_detection_area.size(); ++i) {
        auto pt = wrapping_detection_area[i];
        wrapping_poly.points.emplace_back(scale_(pt.x() + print_origin.x()), scale_(pt.y() + print_origin.y()));
    }

    std::map<const ModelVolume*, Polygon> map_model_volume_to_convex_hull;
    Polygons convex_hulls_other;
    for (auto& inst : print_instances_ordered) {
        for (const ModelVolume *v : inst->print_object->model_object()->volumes) {
            if (!v->is_model_part()) continue;
            auto it_convex_hull = map_model_volume_to_convex_hull.find(v);
            if (it_convex_hull == map_model_volume_to_convex_hull.end()) {
                auto volume_hull = v->get_convex_hull_2d(Geometry::assemble_transform(Vec3d::Zero(), inst->model_instance->get_rotation(),
                                                                                      inst->model_instance->get_scaling_factor(), inst->model_instance->get_mirror()));
                volume_hull.translate(inst->shift - inst->print_object->center_offset());

                it_convex_hull = map_model_volume_to_convex_hull.emplace_hint(it_convex_hull, v, volume_hull);
            }
            Polygon &convex_hull = it_convex_hull->second;
            Polygons convex_hulls_temp;
            convex_hulls_temp.push_back(convex_hull);
            if (!intersection(exclude_polys, convex_hull).empty()) {
                return {inst->model_instance->get_object()->name + L(" is too close to exclusion area, there may be collisions when printing.") + "\n",
                        inst->model_instance->get_object()};
            }

            if (print_config.enable_wrapping_detection.value && !intersection(wrapping_poly, convex_hull).empty()) {
                return {inst->model_instance->get_object()->name + L(" is too close to clumping detection area, there may be collisions when printing.") + "\n",
                        inst->model_instance->get_object()};
            }
            convex_hulls_other.emplace_back(convex_hull);
        }
    }

    //BBS: add the wipe tower check logic
    const PrintConfig &       config   = print.config();
    int                 filaments_count = print.extruders().size();
    int                 plate_index = print.get_plate_index();
    const Vec3d         plate_origin = print.get_plate_origin();
    float               x            = config.wipe_tower_x.get_at(plate_index) + plate_origin(0);
    float               y            = config.wipe_tower_y.get_at(plate_index) + plate_origin(1);
    float               width        = config.prime_tower_width.value;
    float               a            = config.wipe_tower_rotation_angle.value;
    //float               v            = config.wiping_volume.value;

    float        depth                     = print.wipe_tower_data(filaments_count).depth;
    //float        brim_width                = print.wipe_tower_data(filaments_count).brim_width;

    if (config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib)
        width = depth;

    Polygons convex_hulls_temp;
    if (print.has_wipe_tower()) {
        if (!print.is_step_done(psWipeTower)) {
            Polygon wipe_tower_convex_hull;
            wipe_tower_convex_hull.points.emplace_back(scale_(x), scale_(y));
            wipe_tower_convex_hull.points.emplace_back(scale_(x + width), scale_(y));
            wipe_tower_convex_hull.points.emplace_back(scale_(x + width), scale_(y + depth));
            wipe_tower_convex_hull.points.emplace_back(scale_(x), scale_(y + depth));
            wipe_tower_convex_hull.rotate(a);
            convex_hulls_temp.push_back(wipe_tower_convex_hull);
        } else {
            //here, wipe_tower_polygon is not always convex.
            Polygon wipe_tower_polygon;
            if (print.wipe_tower_data().wipe_tower_mesh_data)
                wipe_tower_polygon = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
            wipe_tower_polygon.translate(Point(scale_(x), scale_(y)));
            convex_hulls_temp.push_back(wipe_tower_polygon);
        }
    }
    if (!intersection(convex_hulls_other, convex_hulls_temp).empty()) {
        if (warning) {
            warning->string += L("Prime Tower") + L(" is too close to others, and collisions may be caused.\n");
        }
    }
    if (!intersection(exclude_polys, convex_hulls_temp).empty()) {
        /*if (warning) {
            warning->string += L("Prime Tower is too close to exclusion area, there may be collisions when printing.\n");
        }*/
        return {L("Prime Tower") + L(" is too close to exclusion area, and collisions will be caused.\n")};
    }
    if (print_config.enable_wrapping_detection.value && !intersection({wrapping_poly}, convex_hulls_temp).empty()) {
        return {L("Prime Tower") + L(" is too close to clumping detection area, and collisions will be caused.\n")};
    }
    return {};
}

FilamentCompatibilityType Print::check_multi_filaments_compatibility(const std::vector<std::string>& filament_types)
{
    bool has_high_temperature_filament = false;
    bool has_low_temperature_filament = false;
    bool has_mid_temperature_filament = false;

    for (const auto& type : filament_types) {
        if (get_filament_temp_type(type) ==FilamentTempType::HighTemp)
            has_high_temperature_filament = true;
        else if (get_filament_temp_type(type) == FilamentTempType::LowTemp)
            has_low_temperature_filament = true;
        else if (get_filament_temp_type(type) == FilamentTempType::HighLowCompatible)
            has_mid_temperature_filament = true;
    }

    if (has_high_temperature_filament && has_low_temperature_filament)
        return FilamentCompatibilityType::HighLowMixed;
    else if (has_high_temperature_filament && has_mid_temperature_filament)
        return FilamentCompatibilityType::HighMidMixed;
    else if (has_low_temperature_filament && has_mid_temperature_filament)
        return FilamentCompatibilityType::LowMidMixed;
    else
        return FilamentCompatibilityType::Compatible;
}

bool Print::is_filaments_compatible(const std::vector<int>& filament_types)
{
    bool has_high_temperature_filament = false;
    bool has_low_temperature_filament = false;

    for (const auto& type : filament_types) {
        if (type == FilamentTempType::HighTemp)
            has_high_temperature_filament = true;
        else if (type == FilamentTempType::LowTemp)
            has_low_temperature_filament = true;
    }

    if (has_high_temperature_filament && has_low_temperature_filament)
        return false;

    return true;
}
int Print::get_compatible_filament_type(const std::set<int>& filament_types)
{
    bool has_high_temperature_filament = false;
    bool has_low_temperature_filament = false;

    for (const auto& type : filament_types) {
        if (type == FilamentTempType::HighTemp)
            has_high_temperature_filament = true;
        else if (type == FilamentTempType::LowTemp)
            has_low_temperature_filament = true;
    }

    if (has_high_temperature_filament && has_low_temperature_filament)
        return HighLowCompatible;
    else if (has_high_temperature_filament)
        return HighTemp;
    else if (has_low_temperature_filament)
        return LowTemp;
    return HighLowCompatible;
}

//BBS: this function is used to check whether multi filament can be printed
StringObjectException Print::check_multi_filament_valid(const Print& print)
{
    auto print_config = print.config();
    if(print_config.print_sequence == PrintSequence::ByObject) {// use ByObject valid under ByObject print sequence
        std::set<FilamentCompatibilityType> Compatibility_each_obj;
        bool enable_mix_printing = !print.need_check_multi_filaments_compatibility();

        for (const auto &objectID_t : print.print_object_ids()) {
            std::set<int> obj_used_extruder_ids;
            auto                     print_object = print.get_object(objectID_t);// current object
            if (print_object){
                auto object_extruders_t = print_object->object_extruders(); // object used extruder
                for (int extruder : object_extruders_t) {
                    assert(extruder > 0);
                    obj_used_extruder_ids.insert(extruder);
                }
            }

            if (print_object->has_support_material()) { // extruder used by supports
                auto num_extruders                 = (unsigned int) print_config.filament_diameter.size();
                assert(print_object->config().support_filament >= 0);
                if (print_object->config().support_filament >= 1 && (unsigned int)print_object->config().support_filament < num_extruders + 1)
                    obj_used_extruder_ids.insert((unsigned int) print_object->config().support_filament - 1);//0-based extruder id
                assert(print_object->config().support_interface_filament >= 0);
                if (print_object->config().support_interface_filament >= 1 && (unsigned int)print_object->config().support_interface_filament < num_extruders + 1)
                    obj_used_extruder_ids.insert((unsigned int) print_object->config().support_interface_filament - 1);
            }
            std::vector<std::string> filament_types;
            filament_types.reserve(obj_used_extruder_ids.size());
            for (const auto &extruder_idx : obj_used_extruder_ids) filament_types.push_back(print_config.filament_type.get_at(extruder_idx));

            auto                  compatibility       = check_multi_filaments_compatibility(filament_types);// check for each object
            Compatibility_each_obj.insert(compatibility);
        }
        StringObjectException ret;
        std::string           hypertext = "filament_mix_print";
        if (Compatibility_each_obj.count(FilamentCompatibilityType::HighLowMixed)){// at least one object has HighLowMixed
            if (enable_mix_printing) {
                ret.string     = L("Printing high-temp and low-temp filaments together may cause nozzle clogging or printer damage.");
                ret.is_warning = true;
                // ret.hypetext   = hypertext;
            } else
                ret.string = L("Printing high-temp and low-temp filaments together may cause nozzle clogging or printer damage. If you still want to print, you can enable the option in Preferences.");
        }else if (Compatibility_each_obj.count(FilamentCompatibilityType::LowMidMixed) || Compatibility_each_obj.count(FilamentCompatibilityType::HighMidMixed)){// at least one object has other Mixed
            ret.is_warning = true;
            // ret.hypetext   = hypertext;
            ret.string     = L("Printing different-temp filaments together may cause nozzle clogging or printer damage.");
        }
        return ret;
    }
    std::vector<unsigned int> extruders = print.extruders();
    std::vector<std::string> filament_types;
    filament_types.reserve(extruders.size());
    for (const auto& extruder_idx : extruders)
        filament_types.push_back(print_config.filament_type.get_at(extruder_idx));

    auto compatibility = check_multi_filaments_compatibility(filament_types);
    bool enable_mix_printing = !print.need_check_multi_filaments_compatibility();

    StringObjectException ret;

    if(compatibility == FilamentCompatibilityType::HighLowMixed){
        if(enable_mix_printing){
            ret.string =L("Printing high-temp and low-temp filaments together may cause nozzle clogging or printer damage.");
            ret.is_warning = true;
        }
        else{
            ret.string =L("Printing high-temp and low-temp filaments together may cause nozzle clogging or printer damage. If you still want to print, you can enable the option in Preferences.");
        }
    }
    else if (compatibility == FilamentCompatibilityType::HighMidMixed) {
        ret.is_warning = true;
        ret.string =L("Printing high-temp and mid-temp filaments together may cause nozzle clogging or printer damage.");

    }
    else if (compatibility == FilamentCompatibilityType::LowMidMixed) {
        ret.is_warning = true;
        ret.string = L("Printing mid-temp and low-temp filaments together may cause nozzle clogging or printer damage.");
    }

    return ret;
}

// Orca: this g92e0 regex is used copied from PrusaSlicer
// Matches "G92 E0" with various forms of writing the zero and with an optional comment.
boost::regex regex_g92e0 { "^[ \\t]*[gG]92[ \\t]*[eE](0(\\.0*)?|\\.0+)[ \\t]*(;.*)?$" };

// Precondition: Print::validate() requires the Print::apply() to be called its invocation.
//BBS: refine seq-print validation logic.....FIXME:StringObjectException *warning can only contain one warning, but there might be many warnings, need a vector<StringObjectException>
StringObjectException Print::validate(StringObjectException *warning, Polygons* collison_polygons, std::vector<std::pair<Polygon, float>>* height_polygons) const
{
    std::vector<unsigned int> extruders = this->extruders();
    unsigned int nozzles = m_config.nozzle_diameter.size();

    if (m_objects.empty())
        return {std::string()};

    if (extruders.empty())
        return { L("No extrusions under current settings.") };

    if (nozzles < 2 && extruders.size() > 1) {
        auto ret = check_multi_filament_valid(*this);
        if (!ret.string.empty())
        {
            ret.type = STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP;
            if (ret.is_warning && warning != nullptr) {
                *warning = ret;
                //return {};
            }else
                return ret;
        }
    }

    if (m_config.print_sequence == PrintSequence::ByObject && m_objects.size() > 1) {
        if (m_config.timelapse_type == TimelapseType::tlSmooth)
            return {L("Smooth mode of timelapse is not supported when \"by object\" sequence is enabled.")};

        if (m_config.enable_wrapping_detection) {
            StringObjectException clumping_detection_setting_err;
            clumping_detection_setting_err.string = L("Clumping detection is not supported when \"by object\" sequence is enabled.");
            clumping_detection_setting_err.opt_key = "enable_wrapping_detection";
            return clumping_detection_setting_err;
        }

        //BBS: refine seq-print validation logic
        auto ret = sequential_print_clearance_valid(*this, collison_polygons, height_polygons);
        if (!ret.string.empty()) {
            ret.type = STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT;
            return ret;
        }
    }
    else {
        //BBS
        auto ret = layered_print_cleareance_valid(*this, warning);
        if (!ret.string.empty()) {
            ret.type = STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT;
            return ret;
        }
    }

    if (m_config.enable_prime_tower) {
    } else {
        if (m_config.enable_wrapping_detection && warning!=nullptr) {
            StringObjectException warningtemp;
            warningtemp.string     = L("Prime tower is required for clumping detection; otherwise, there may be flaws on the model.");
            warningtemp.opt_key    = "enable_prime_tower";
            warningtemp.is_warning = true;
            *warning               = warningtemp;
        }
    }

    if (m_config.spiral_mode) {
        size_t total_copies_count = 0;
        for (const PrintObject* object : m_objects)
            total_copies_count += object->instances().size();
        // #4043
        if (total_copies_count > 1 && m_config.print_sequence != PrintSequence::ByObject)
            return {L("Please select \"By object\" print sequence to print multiple objects in spiral vase mode."), nullptr, "spiral_mode"};
        assert(m_objects.size() == 1);
        const auto all_regions = m_objects.front()->all_regions();
        if (all_regions.size() > 1) {
            // Orca: make sure regions are not compatible
            if (std::any_of(all_regions.begin() + 1, all_regions.end(), [ra = all_regions.front()](const auto rb) {
                return !Layer::is_perimeter_compatible(ra, rb);
            })) {
                return {L("The spiral vase mode does not work when an object contains more than one materials."), nullptr, "spiral_mode"};
            }
        }
    }

    // Cache of layer height profiles for checking:
    // 1) Whether all layers are synchronized if printing with wipe tower and / or unsynchronized supports.
    // 2) Whether layer height is constant for Organic supports.
    // 3) Whether build volume Z is not violated.
    std::vector<std::vector<coordf_t>> layer_height_profiles;
    auto layer_height_profile = [this, &layer_height_profiles](const size_t print_object_idx) -> const std::vector<coordf_t>& {
        const PrintObject       &print_object = *m_objects[print_object_idx];
        if (layer_height_profiles.empty())
            layer_height_profiles.assign(m_objects.size(), std::vector<coordf_t>());
        std::vector<coordf_t>   &profile      = layer_height_profiles[print_object_idx];
        if (profile.empty())
            PrintObject::update_layer_height_profile(*print_object.model_object(), print_object.slicing_parameters(), profile);
        return profile;
    };

    // Checks that the print does not exceed the max print height
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx) {
        const PrintObject &print_object = *m_objects[print_object_idx];
        //FIXME It is quite expensive to generate object layers just to get the print height!
        if (auto layers = generate_object_layers(print_object.slicing_parameters(), layer_height_profile(print_object_idx), print_object.config().precise_z_height.value);
            !layers.empty()) {

            Vec3d test =this->shrinkage_compensation();
            const double shrinkage_compensation_z = this->shrinkage_compensation().z();
            
            if (shrinkage_compensation_z != 1. && layers.back() > (this->config().printable_height / shrinkage_compensation_z + EPSILON)) {
                // The object exceeds the maximum build volume height because of shrinkage compensation.
                return StringObjectException{
                    Slic3r::format(_u8L("While the object %1% itself fits the build volume, it exceeds the maximum build volume height because of material shrinkage compensation."), print_object.model_object()->name),
                    print_object.model_object(),
                    ""
                };
            } else if (layers.back() > this->config().printable_height + EPSILON) {
                // Test whether the last slicing plane is below or above the print volume.
                return StringObjectException{
                    0.5 * (layers[layers.size() - 2] + layers.back()) > this->config().printable_height + EPSILON ?
                    Slic3r::format(_u8L("The object %1% exceeds the maximum build volume height."), print_object.model_object()->name) :
                    Slic3r::format(_u8L("While the object %1% itself fits the build volume, its last layer exceeds the maximum build volume height."), print_object.model_object()->name) +
                    " " + _u8L("You might want to reduce the size of your model or change current print settings and retry."),
                    print_object.model_object(),
                    ""
                };
            }
        }
    }

    // Some of the objects has variable layer height applied by painting or by a table.
    bool has_custom_layering = std::find_if(m_objects.begin(), m_objects.end(), 
        [](const PrintObject *object) { return object->model_object()->has_custom_layering(); }) 
        != m_objects.end();

    // Custom layering is not allowed for tree supports as of now.
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx)
        if (const PrintObject &print_object = *m_objects[print_object_idx];
            print_object.has_support_material() && is_tree(print_object.config().support_type.value) && (print_object.config().support_style.value == smsTreeOrganic || 
                // Orca: use organic as default
                print_object.config().support_style.value == smsDefault) &&
            print_object.model_object()->has_custom_layering()) {
            if (const std::vector<coordf_t> &layers = layer_height_profile(print_object_idx); ! layers.empty())
                if (! check_object_layers_fixed(print_object.slicing_parameters(), layers))
                    return {_u8L("Variable layer height is not supported with Organic supports.") };
        }

    if (this->has_wipe_tower() && ! m_objects.empty()) {
        // Make sure all extruders use same diameter filament and have the same nozzle diameter
        // EPSILON comparison is used for nozzles and 10 % tolerance is used for filaments
        double first_nozzle_diam = m_config.nozzle_diameter.get_at(extruders.front());
        double first_filament_diam = m_config.filament_diameter.get_at(extruders.front());
        for (const auto& extruder_idx : extruders) {
            double nozzle_diam = m_config.nozzle_diameter.get_at(extruder_idx);
            double filament_diam = m_config.filament_diameter.get_at(extruder_idx);
            if (nozzle_diam - EPSILON > first_nozzle_diam || nozzle_diam + EPSILON < first_nozzle_diam
                || std::abs((filament_diam - first_filament_diam) / first_filament_diam) > 0.1) {
                // return { L("Different nozzle diameters and different filament diameters may not work well when prime tower is enabled. It's very experimental, please proceed with caucious.") };
                    warning->string = L("Different nozzle diameters and different filament diameters may not work well when the prime tower is enabled. It's very experimental, so please proceed with caution.");
                    warning->opt_key = "nozzle_diameter";
                    break;
                }
        }

        if (! m_config.use_relative_e_distances)
            return { L("The Wipe Tower is currently only supported with the relative extruder addressing (use_relative_e_distances=1).") };

        if (m_config.ooze_prevention && m_config.single_extruder_multi_material)
            return {L("Ooze prevention is only supported with the wipe tower when 'single_extruder_multi_material' is off.")};
            
#if 0
        if (m_config.gcode_flavor != gcfRepRapSprinter && m_config.gcode_flavor != gcfRepRapFirmware &&
            m_config.gcode_flavor != gcfRepetier && m_config.gcode_flavor != gcfMarlinLegacy && m_config.gcode_flavor != gcfMarlinFirmware)
            return { L("The prime tower is currently only supported for the Marlin, RepRap/Sprinter, RepRapFirmware and Repetier G-code flavors.")};

        if ((m_config.print_sequence == PrintSequence::ByObject) && extruders.size() > 1)
            return { L("The prime tower is not supported in \"By object\" print."), nullptr, "enable_prime_tower" };

        // BBS: When prime tower is on, object layer and support layer must be aligned. So support gap should be multiple of object layer height.
        for (size_t i = 0; i < m_objects.size(); i++) {
            const PrintObject* object = m_objects[i];
            const SlicingParameters& slicing_params = object->slicing_parameters();
            if (object->config().adaptive_layer_height) {
                return  { L("The prime tower is not supported when adaptive layer height is on. It requires that all objects have the same layer height."), object, "adaptive_layer_height" };
            }

            if (!object->config().enable_support)
                continue;

            double gap_layers = slicing_params.gap_object_support / slicing_params.layer_height;
            if (gap_layers - (int)gap_layers > EPSILON) {
                return {L("The prime tower requires \"support gap\" to be multiple of layer height."), object};
            }
        }
#endif

        if (m_objects.size() > 1) {
            const SlicingParameters &slicing_params0 = m_objects.front()->slicing_parameters();
            size_t                  tallest_object_idx = 0;
            for (size_t i = 1; i < m_objects.size(); ++ i) {
                const PrintObject       *object         = m_objects[i];
                const SlicingParameters &slicing_params = object->slicing_parameters();
                if (std::abs(slicing_params.first_print_layer_height - slicing_params0.first_print_layer_height) > EPSILON ||
                    std::abs(slicing_params.layer_height             - slicing_params0.layer_height            ) > EPSILON)
                    return {L("The prime tower requires that all objects have the same layer heights."), object, "initial_layer_print_height"};
                if (slicing_params.raft_layers() != slicing_params0.raft_layers())
                    return {L("The prime tower requires that all objects are printed over the same number of raft layers."), object, "raft_layers"};
                // BBS: support gap can be multiple of object layer height, remove _L()
#if 0
                if (slicing_params0.gap_object_support != slicing_params.gap_object_support ||
                    slicing_params0.gap_support_object != slicing_params.gap_support_object)
                    return {L("The prime tower is only supported for multiple objects if they are printed with the same support_top_z_distance."), object};
#endif
                if (!equal_layering(slicing_params, slicing_params0))
                    return  { L("The prime tower requires that all objects are sliced with the same layer heights."), object };
                if (has_custom_layering) {
                    auto &lh         = layer_height_profile(i);
                    auto &lh_tallest = layer_height_profile(tallest_object_idx);
                    if (*(lh.end() - 2) > *(lh_tallest.end() - 2))
                        tallest_object_idx = i;
                }
            }

            // BBS: remove obsolete logics and _L()
            if (has_custom_layering) {
                std::vector<std::vector<coordf_t>> layer_z_series;
                layer_z_series.assign(m_objects.size(), std::vector<coordf_t>());
               
                for (size_t idx_object = 0; idx_object < m_objects.size(); ++idx_object) {
                    layer_z_series[idx_object] = generate_object_layers(m_objects[idx_object]->slicing_parameters(), layer_height_profiles[idx_object], m_objects[idx_object]->config().precise_z_height.value);
                }

                for (size_t idx_object = 0; idx_object < m_objects.size(); ++idx_object) {
                    if (idx_object == tallest_object_idx) continue;
                    // Check that the layer height profiles are equal. This will happen when one object is
                    // a copy of another, or when a layer height modifier is used the same way on both objects.
                    // The latter case might create a floating point inaccuracy mismatch, so compare
                    // element-wise using an epsilon check.
                    size_t         i   = 0;
                    const coordf_t eps = 0.5 * EPSILON; // layers closer than EPSILON will be merged later. Let's make
                    // this check a bit more sensitive to make sure we never consider two different layers as one.
                    while (i < layer_height_profiles[idx_object].size() && i < layer_height_profiles[tallest_object_idx].size()) {
                        // BBS: remove the break condition, because a variable layer height object and a new object will not be checked when slicing
                        //if (i % 2 == 0 && layer_height_profiles[tallest_object_idx][i] > layer_height_profiles[idx_object][layer_height_profiles[idx_object].size() - 2])
                        //    break;
                        if (std::abs(layer_height_profiles[idx_object][i] - layer_height_profiles[tallest_object_idx][i]) > eps)
                            return {L("The prime tower is only supported if all objects have the same variable layer height.")};
                        ++i;
                    }
                }
            }
        }
    }

	{
		// Find the smallest used nozzle diameter and the number of unique nozzle diameters.
		double min_nozzle_diameter = std::numeric_limits<double>::max();
		double max_nozzle_diameter = 0;
		for (unsigned int extruder_id : extruders) {
			double dmr = m_config.nozzle_diameter.get_at(extruder_id);
			min_nozzle_diameter = std::min(min_nozzle_diameter, dmr);
			max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
		}

        // BBS: remove L()
#if 0
        // We currently allow one to assign extruders with a higher index than the number
        // of physical extruders the machine is equipped with, as the Printer::apply() clamps them.
        unsigned int total_extruders_count = m_config.nozzle_diameter.size();
        for (const auto& extruder_idx : extruders)
            if ( extruder_idx >= total_extruders_count )
                return {L("One or more object were assigned an extruder that the printer does not have.")};
#endif

        auto validate_extrusion_width = [min_nozzle_diameter, max_nozzle_diameter](const ConfigBase &config, const char *opt_key, double layer_height, std::string &err_msg) -> bool {
            double extrusion_width_min = config.get_abs_value(opt_key, min_nozzle_diameter);
            double extrusion_width_max = config.get_abs_value(opt_key, max_nozzle_diameter);
        	if (extrusion_width_min == 0) {
        		// Default "auto-generated" extrusion width is always valid.
        	} else if (extrusion_width_min <= layer_height) {
                err_msg = L("Too small line width");
				return false;
			} else if (extrusion_width_max > max_nozzle_diameter * MAX_LINE_WIDTH_MULTIPLIER) {
                err_msg = L("Too large line width");
				return false;
			}
			return true;
		};
        for (PrintObject *object : m_objects) {
            if (object->has_support_material()) {
                // BBS: remove useless logics and L()
#if 0
				if ((object->config().support_filament == 0 || object->config().support_interface_filament == 0) && max_nozzle_diameter - min_nozzle_diameter > EPSILON) {
                    // The object has some form of support and either support_filament or support_interface_filament
                    // will be printed with the current tool without a forced tool change. Play safe, assert that all object nozzles
                    // are of the same diameter.
                    return {L("Printing with multiple extruders of differing nozzle diameters. "
                           "If support is to be printed with the current filament (support_filament == 0 or support_interface_filament == 0), "
                           "all nozzles have to be of the same diameter."), object, "support_filament"};
                }
#endif

                // BBS
#if 0
                if (this->has_wipe_tower() && object->config().independent_support_layer_height) {
                    return {L("The prime tower requires that support has the same layer height with object."), object, "support_filament"};
                }
#endif

                // Prusa: Fixing crashes with invalid tip diameter or branch diameter
                // https://github.com/prusa3d/PrusaSlicer/commit/96b3ae85013ac363cd1c3e98ec6b7938aeacf46d
                if (is_tree(object->config().support_type.value) && (object->config().support_style == smsTreeOrganic ||
                    // Orca: use organic as default
                    object->config().support_style == smsDefault)) {
                    float extrusion_width = std::min(
                        support_material_flow(object).width(),
                        support_material_interface_flow(object).width());
                    if (object->config().tree_support_tip_diameter < extrusion_width - EPSILON)
                        return { L("Organic support tree tip diameter must not be smaller than support material extrusion width."), object, "tree_support_tip_diameter" };
                    if (object->config().tree_support_branch_diameter_organic < 2. * extrusion_width - EPSILON)
                        return { L("Organic support branch diameter must not be smaller than 2x support material extrusion width."), object, "tree_support_branch_diameter_organic" };
                    if (object->config().tree_support_branch_diameter_organic < object->config().tree_support_tip_diameter)
                        return { L("Organic support branch diameter must not be smaller than support tree tip diameter."), object, "tree_support_branch_diameter_organic" };
                }
            }

            // Do we have custom support data that would not be used?
            // Notify the user in that case.
            if (! object->has_support() && warning) {
                for (const ModelVolume* mv : object->model_object()->volumes) {
                    bool has_enforcers = mv->is_support_enforcer() ||
                        (mv->is_model_part() && mv->supported_facets.has_facets(*mv, EnforcerBlockerType::ENFORCER));
                    if (has_enforcers) {
                        StringObjectException warningtemp;
                        warningtemp.string = L("Support enforcers are used but support is not enabled. Please enable support.");
                        warningtemp.object  = object;
                        *warning            = warningtemp;
                        break;
                    }
                }
            }

            double initial_layer_print_height = m_config.initial_layer_print_height.value;
            double first_layer_min_nozzle_diameter;
            if (object->has_raft()) {
                // if we have raft layers, only support material extruder is used on first layer
                size_t first_layer_extruder = object->config().raft_layers == 1
                    ? object->config().support_interface_filament-1
                    : object->config().support_filament-1;
                first_layer_min_nozzle_diameter = (first_layer_extruder == size_t(-1)) ?
                    min_nozzle_diameter :
                    m_config.nozzle_diameter.get_at(first_layer_extruder);
            } else {
                // if we don't have raft layers, any nozzle diameter is potentially used in first layer
                first_layer_min_nozzle_diameter = min_nozzle_diameter;
            }
            if (initial_layer_print_height > first_layer_min_nozzle_diameter)
                return {L("Layer height cannot exceed nozzle diameter."), object, "initial_layer_print_height"};

            // validate layer_height
            double layer_height = object->config().layer_height.value;
            if (layer_height > min_nozzle_diameter)
                return {L("Layer height cannot exceed nozzle diameter."), object, "layer_height"};

            // Validate extrusion widths.
            std::string err_msg;
            if (!validate_extrusion_width(object->config(), "line_width", layer_height, err_msg))
            	return {err_msg, object, "line_width"};
            if (object->has_support() || object->has_raft()) {
                if (!validate_extrusion_width(object->config(), "support_line_width", layer_height, err_msg))
                    return {err_msg, object, "support_line_width"};
            }
            for (const char *opt_key : { "inner_wall_line_width", "outer_wall_line_width", "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width","skin_infill_line_width" ,"skeleton_infill_line_width"})
				for (const PrintRegion &region : object->all_regions())
                    if (!validate_extrusion_width(region.config(), opt_key, layer_height, err_msg))
		            	return  {err_msg, object, opt_key};
        }
    }

    // Orca: G92 E0 is not supported when using absolute extruder addressing
    // This check is copied from PrusaSlicer, the original author is Vojtech Bubnik
    if(!is_BBL_printer()) {
        bool before_layer_gcode_resets_extruder =
            boost::regex_search(m_config.before_layer_change_gcode.value, regex_g92e0);
        bool layer_gcode_resets_extruder = boost::regex_search(m_config.layer_change_gcode.value, regex_g92e0);
        if (m_config.use_relative_e_distances) {
            // See GH issues #6336 #5073
            if ((m_config.gcode_flavor == gcfMarlinLegacy || m_config.gcode_flavor == gcfMarlinFirmware) &&
                !before_layer_gcode_resets_extruder && !layer_gcode_resets_extruder)
                return {L("Relative extruder addressing requires resetting the extruder position at each layer to "
                          "prevent loss of floating point accuracy. Add \"G92 E0\" to layer_gcode."),
                        nullptr, "before_layer_change_gcode"};
        } else if (before_layer_gcode_resets_extruder)
            return {L("\"G92 E0\" was found in before_layer_gcode, which is incompatible with absolute extruder "
                      "addressing."),
                    nullptr, "before_layer_change_gcode"};
        else if (layer_gcode_resets_extruder)
            return {L("\"G92 E0\" was found in layer_gcode, which is incompatible with absolute extruder addressing."),
                    nullptr, "layer_change_gcode"};
    }

    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    assert(bed_type_def != nullptr);

	    if (is_BBL_printer()) {
	    const t_config_enum_values* bed_type_keys_map = bed_type_def->enum_keys_map;
	    for (unsigned int extruder_id : extruders) {
	        const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_key(m_config.curr_bed_type));
	        for (unsigned int extruder_id : extruders) {
	            int curr_bed_temp = bed_temp_opt->get_at(extruder_id);
	            if (curr_bed_temp == 0 && bed_type_keys_map != nullptr) {
	                std::string bed_type_name;
	                for (auto item : *bed_type_keys_map) {
	                    if (item.second == m_config.curr_bed_type) {
	                        bed_type_name = item.first;
	                        break;
	                    }
	                }

	                StringObjectException except;
	                except.string = Slic3r::format(L("Plate %d: %s does not support filament %s"), this->get_plate_index() + 1, L(bed_type_name), extruder_id + 1);
	                except.string += "\n";
	                except.type   = STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE;
	                except.params.push_back(std::to_string(this->get_plate_index() + 1));
	                except.params.push_back(L(bed_type_name));
	                except.params.push_back(std::to_string(extruder_id+1));
	                except.object = nullptr;
	                return except;
	           }
            }
        }
    }

    // check if print speed/accel/jerk is higher than the maximum speed of the printer
    if (warning) {
        try {
            auto check_motion_ability_object_setting = [&](const std::vector<std::string>& keys_to_check, double limit) -> std::string {
                std::string warning_key;
                for (const auto& key : keys_to_check) {
                    if (m_default_object_config.get_abs_value(key) > limit) {
                        warning_key = key;
                        break;
                    }
                }
                return warning_key;
            };
            auto check_motion_ability_region_setting = [&](const std::vector<std::string>& keys_to_check, double limit) -> std::string {
                std::string warning_key;
                for (const auto& key : keys_to_check) {
                    if (m_default_region_config.get_abs_value(key) > limit) {
                        warning_key = key;
                        break;
                    }
                }
                return warning_key;
            };
            std::string warning_key;

            // check jerk
            if (m_default_object_config.default_jerk == 1 || m_default_object_config.outer_wall_jerk == 1 ||
                m_default_object_config.inner_wall_jerk == 1) {
               warning->string = L("Setting the jerk speed too low could lead to artifacts on curved surfaces");
               if (m_default_object_config.outer_wall_jerk == 1)
                    warning_key = "outer_wall_jerk";
               else if (m_default_object_config.inner_wall_jerk == 1)
                    warning_key = "inner_wall_jerk";
               else
                    warning_key = "default_jerk";

               warning->opt_key = warning_key;
            }

            if (warning_key.empty() && m_default_object_config.default_jerk > 0) {
               std::vector<std::string> jerk_to_check = {"default_jerk",     "outer_wall_jerk",    "inner_wall_jerk", "infill_jerk",
                                                         "top_surface_jerk", "initial_layer_jerk", "travel_jerk"};
               const auto               max_jerk = std::min(m_config.machine_max_jerk_x.values[0], m_config.machine_max_jerk_y.values[0]);
               warning_key.clear();
               if (m_default_object_config.default_jerk > 0)
                    warning_key = check_motion_ability_object_setting(jerk_to_check, max_jerk);
               if (!warning_key.empty()) {
                    warning->string = L(
                        "The jerk setting exceeds the printer's maximum jerk (machine_max_jerk_x/machine_max_jerk_y).\nOrca will "
                        "automatically cap the jerk speed to ensure it doesn't surpass the printer's capabilities.\nYou can adjust the "
                        "maximum jerk setting in your printer's configuration to get higher speeds.");
                    warning->opt_key = warning_key;
               }
            }

            // Check junction deviation
            const auto max_junction_deviation = m_config.machine_max_junction_deviation.values[0];
            if (warning_key.empty() && m_default_object_config.default_junction_deviation.value > max_junction_deviation) {
                warning->string  = L( "Junction deviation setting exceeds the printer's maximum value "
                                      "(machine_max_junction_deviation).\nOrca will "
                                      "automatically cap the junction deviation to ensure it doesn't surpass the printer's "
                                      "capabilities.\nYou can adjust the "
                                      "machine_max_junction_deviation value in your printer's configuration to get higher limits.");
                warning->opt_key = warning_key;
            }
            
            // check acceleration
            const auto max_accel = m_config.machine_max_acceleration_extruding.values[0];
            if (warning_key.empty() && m_default_object_config.default_acceleration > 0 && max_accel > 0) {
               const bool support_travel_acc = (m_config.gcode_flavor == gcfRepetier || m_config.gcode_flavor == gcfMarlinFirmware ||
                                                m_config.gcode_flavor == gcfRepRapFirmware);

               std::vector<std::string> accel_to_check;
               if (!support_travel_acc)
                    accel_to_check = {
                        "default_acceleration",
                        "inner_wall_acceleration",
                        "outer_wall_acceleration",
                        "bridge_acceleration",
                        "initial_layer_acceleration",
                        "sparse_infill_acceleration",
                        "internal_solid_infill_acceleration",
                        "top_surface_acceleration",
                        "travel_acceleration",
                    };
               else
                    accel_to_check = {
                        "default_acceleration",
                        "inner_wall_acceleration",
                        "outer_wall_acceleration",
                        "bridge_acceleration",
                        "initial_layer_acceleration",
                        "sparse_infill_acceleration",
                        "internal_solid_infill_acceleration",
                        "top_surface_acceleration",
                    };
               warning_key = check_motion_ability_object_setting(accel_to_check, max_accel);
               if (!warning_key.empty()) {
                    warning->string  = L("The acceleration setting exceeds the printer's maximum acceleration "
                                          "(machine_max_acceleration_extruding).\nOrca will "
                                          "automatically cap the acceleration speed to ensure it doesn't surpass the printer's "
                                          "capabilities.\nYou can adjust the "
                                          "machine_max_acceleration_extruding value in your printer's configuration to get higher speeds.");
                    warning->opt_key = warning_key;
               }
               if (support_travel_acc) {
                    const auto max_travel = m_config.machine_max_acceleration_travel.values[0];
                    if (max_travel > 0) {
                        accel_to_check = {
                            "travel_acceleration",
                        };
                        warning_key = check_motion_ability_object_setting(accel_to_check, max_travel);
                        if (!warning_key.empty()) {
                            warning->string = L(
                                "The travel acceleration setting exceeds the printer's maximum travel acceleration "
                                "(machine_max_acceleration_travel).\nOrca will "
                                "automatically cap the travel acceleration speed to ensure it doesn't surpass the printer's "
                                "capabilities.\nYou can adjust the "
                                "machine_max_acceleration_travel value in your printer's configuration to get higher speeds.");
                            warning->opt_key = warning_key;
                        }
                    }
               }
            }

            // check speed
            // Orca: disable the speed check for now as we don't cap the speed
            // if (warning_key.empty()) {
            //    auto       speed_to_check = {"inner_wall_speed",  "outer_wall_speed", "sparse_infill_speed",   "internal_solid_infill_speed",
            //                                 "top_surface_speed", "bridge_speed",     "internal_bridge_speed", "gap_infill_speed"};
            //    const auto max_speed      = std::min(m_config.machine_max_speed_x.values[0], m_config.machine_max_speed_y.values[0]);
            //    warning_key.clear();
            //    warning_key = check_motion_ability_region_setting(speed_to_check, max_speed);
            //    if (warning_key.empty() && m_config.travel_speed > max_speed)
            //         warning_key = "travel_speed";
            //    if (!warning_key.empty()) {
            //         warning->string = L(
            //             "The speed setting exceeds the printer's maximum speed (machine_max_speed_x/machine_max_speed_y).\nOrca will "
            //             "automatically cap the print speed to ensure it doesn't surpass the printer's capabilities.\nYou can adjust the "
            //             "maximum speed setting in your printer's configuration to get higher speeds.");
            //         warning->opt_key = warning_key;
            //    }
            // }

            // check wall sequence and precise outer wall
            if (m_default_region_config.precise_outer_wall && m_default_region_config.wall_sequence != WallSequence::InnerOuter) {
                warning->string  = L("The precise wall option will be ignored for outer-inner or inner-outer-inner wall sequences.");
                warning->opt_key = "precise_outer_wall";
            }

        } catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Orca: validate motion ability failed: " << e.what() << std::endl;
        }
    }
    if (!this->has_same_shrinkage_compensations()){
        warning->string = L("Filament shrinkage will not be used because filament shrinkage for the used filaments differs significantly.");
        warning->opt_key = "";
    }
    return {};
}

#if 0
// the bounding box of objects placed in copies position
// (without taking skirt/brim/support material into account)
BoundingBox Print::bounding_box() const
{
    BoundingBox bb;
    for (const PrintObject *object : m_objects)
        for (const PrintInstance &instance : object->instances()) {
        	BoundingBox bb2(object->bounding_box());
        	bb.merge(bb2.min + instance.shift);
        	bb.merge(bb2.max + instance.shift);
        }
    return bb;
}

// the total bounding box of extrusions, including skirt/brim/support material
// this methods needs to be called even when no steps were processed, so it should
// only use configuration values
BoundingBox Print::total_bounding_box() const
{
    // get objects bounding box
    BoundingBox bb = this->bounding_box();

    // we need to offset the objects bounding box by at least half the perimeters extrusion width
    Flow perimeter_flow = m_objects.front()->get_layer(0)->get_region(0)->flow(frPerimeter);
    double extra = perimeter_flow.width/2;

    // consider support material
    if (this->has_support_material()) {
        extra = std::max(extra, SUPPORT_MATERIAL_MARGIN);
    }

    // consider brim and skirt
    if (m_config.brim_width.value > 0) {
        Flow brim_flow = this->brim_flow();
        extra = std::max(extra, m_config.brim_width.value + brim_flow.width/2);
    }
    if (this->has_skirt()) {
        int skirts = m_config.skirt_loops.value;
        if (skirts == 0 && this->has_infinite_skirt()) skirts = 1;
        Flow skirt_flow = this->skirt_flow();
        extra = std::max(
            extra,
            m_config.brim_width.value
                + m_config.skirt_distance.value
                + skirts * skirt_flow.spacing()
                + skirt_flow.width/2
        );
    }

    if (extra > 0)
        bb.offset(scale_(extra));

    return bb;
}
#endif

double Print::skirt_first_layer_height() const
{
    return m_config.initial_layer_print_height.value;
}

Flow Print::brim_flow() const
{
    ConfigOptionFloatOrPercent width = m_config.initial_layer_line_width;
    if (width.value <= 0)
        width = m_print_regions.front()->config().inner_wall_line_width;
    if (width.value <= 0)
        width = m_objects.front()->config().line_width;

    /* We currently use a random region's perimeter extruder.
       While this works for most cases, we should probably consider all of the perimeter
       extruders and take the one with, say, the smallest index.
       The same logic should be applied to the code that selects the extruder during G-code
       generation as well. */
    return Flow::new_from_config_width(
        frPerimeter,
        // Flow::new_from_config_width takes care of the percent to value substitution
		width,
        (float)m_config.nozzle_diameter.get_at(m_print_regions.front()->config().wall_filament-1),
		(float)this->skirt_first_layer_height());
}

Flow Print::skirt_flow() const
{
    ConfigOptionFloatOrPercent width = m_config.initial_layer_line_width;
    if (width.value <= 0)
        width = m_objects.front()->config().line_width;

    /* We currently use a random object's support material extruder.
       While this works for most cases, we should probably consider all of the support material
       extruders and take the one with, say, the smallest index;
       The same logic should be applied to the code that selects the extruder during G-code
       generation as well. */
    return Flow::new_from_config_width(
        frPerimeter,
        // Flow::new_from_config_width takes care of the percent to value substitution
		width,
		(float)m_config.nozzle_diameter.get_at(m_objects.front()->config().support_filament-1),
		(float)this->skirt_first_layer_height());
}

bool Print::has_support_material() const
{
    for (const PrintObject *object : m_objects)
        if (object->has_support_material())
            return true;
    return false;
}

/*  This method assigns extruders to the volumes having a material
    but not having extruders set in the volume config. */
void Print::auto_assign_extruders(ModelObject* model_object) const
{
    // only assign extruders if object has more than one volume
    if (model_object->volumes.size() < 2)
        return;

//    size_t extruders = m_config.nozzle_diameter.values.size();
    for (size_t volume_id = 0; volume_id < model_object->volumes.size(); ++ volume_id) {
        ModelVolume *volume = model_object->volumes[volume_id];
        //FIXME Vojtech: This assigns an extruder ID even to a modifier volume, if it has a material assigned.
        if ((volume->is_model_part() || volume->is_modifier()) && ! volume->material_id().empty() && ! volume->config.has("extruder"))
            volume->config.set("extruder", int(volume_id + 1));
    }
}

void  PrintObject::set_shared_object(PrintObject *object)
{
    m_shared_object = object;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, found shared object from %2%")%this%m_shared_object;
}

void  PrintObject::clear_shared_object()
{
    if (m_shared_object) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, clear previous shared object data %2%")%this %m_shared_object;
        m_layers.clear();
        m_support_layers.clear();

        m_shared_object = nullptr;

        invalidate_all_steps_without_cancel();
    }
}

void  PrintObject::copy_layers_from_shared_object()
{
    if (m_shared_object) {
        m_layers.clear();
        m_support_layers.clear();

        firstLayerObjSliceByVolume.clear();
        firstLayerObjSliceByGroups.clear();

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, copied layers from object %2%")%this%m_shared_object;
        m_layers = m_shared_object->layers();
        m_support_layers = m_shared_object->support_layers();

        firstLayerObjSliceByVolume = m_shared_object->firstLayerObjSlice();
        firstLayerObjSliceByGroups = m_shared_object->firstLayerObjGroups();
    }
}

void  PrintObject::copy_layers_overhang_from_shared_object()
{
    if (m_shared_object) {
        for (size_t index = 0; index <  m_layers.size() && index <  m_shared_object->m_layers.size(); index++)
        {
            Layer* layer_src = m_layers[index];
            layer_src->loverhangs = m_shared_object->m_layers[index]->loverhangs;
            layer_src->loverhangs_bbox = m_shared_object->m_layers[index]->loverhangs_bbox;
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, copied layer overhang from object %2%")%this%m_shared_object;
    }
}


// BBS
BoundingBox PrintObject::get_first_layer_bbox(float& a, float& layer_height, std::string& name)
{
    BoundingBox bbox;
    a = 0;
    name = this->model_object()->name;
    if (layer_count() > 0) {
        auto layer = get_layer(0);
        layer_height = layer->height;
        // only work for object with single instance
        auto shift = instances()[0].shift_without_plate_offset();
        for (auto bb : layer->lslices_bboxes)
        {
            bb.translate(shift.x(), shift.y());
            bbox.merge(bb);
        }
        for (auto slice : layer->lslices) {
            a += area(slice);
        }
    }
    if (has_brim())
        bbox = firstLayerObjectBrimBoundingBox;
    return bbox;
}

// BBS: map print object with its first layer's first extruder
std::map<ObjectID, unsigned int> getObjectExtruderMap(const Print& print) {
    std::map<ObjectID, unsigned int> objectExtruderMap;
    for (const PrintObject* object : print.objects()) {
        // BBS
        if (object->object_first_layer_wall_extruders.empty()){
            unsigned int objectFirstLayerFirstExtruder = print.config().filament_diameter.size();
            auto firstLayerRegions = object->layers().front()->regions();
            if (!firstLayerRegions.empty()) {
                for (const LayerRegion* regionPtr : firstLayerRegions) {
                    if (regionPtr->has_extrusions())
                        objectFirstLayerFirstExtruder = std::min(objectFirstLayerFirstExtruder,
                          regionPtr->region().extruder(frExternalPerimeter));
                }
            }
            objectExtruderMap.insert(std::make_pair(object->id(), objectFirstLayerFirstExtruder));
        }
        else {
            objectExtruderMap.insert(std::make_pair(object->id(), object->object_first_layer_wall_extruders.front()));
        }
    }
    return objectExtruderMap;
}

// Slicing process, running at a background thread.
void Print::process(long long *time_cost_with_cache, bool use_cache)
{
    long long start_time = 0, end_time = 0;
    if (time_cost_with_cache)
        *time_cost_with_cache = 0;

    name_tbb_thread_pool_threads_set_locale();

    //compute the PrintObject with the same geometries
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, enter, use_cache=%2%, object size=%3%")%this%use_cache%m_objects.size();
    if (m_objects.empty())
        return;

    for (PrintObject *obj : m_objects)
        obj->clear_shared_object();

    //add the print_object share check logic
    auto is_print_object_the_same = [this](const PrintObject* object1, const PrintObject* object2) -> bool{
        if (object1->trafo().matrix() != object2->trafo().matrix())
            return false;
        const ModelObject* model_obj1 = object1->model_object();
        const ModelObject* model_obj2 = object2->model_object();
        if (model_obj1->volumes.size() != model_obj2->volumes.size())
            return false;
        bool has_extruder1 = model_obj1->config.has("extruder");
        bool has_extruder2 = model_obj2->config.has("extruder");
        if ((has_extruder1 != has_extruder2)
            || (has_extruder1 && model_obj1->config.extruder() != model_obj2->config.extruder()))
            return false;
        for (int index = 0; index < model_obj1->volumes.size(); index++) {
            const ModelVolume &model_volume1 = *model_obj1->volumes[index];
            const ModelVolume &model_volume2 = *model_obj2->volumes[index];
            if (model_volume1.type() != model_volume2.type())
                return false;
            if (model_volume1.mesh_ptr() != model_volume2.mesh_ptr())
                return false;
            if (!(model_volume1.get_transformation() == model_volume2.get_transformation()))
                return false;
            has_extruder1 = model_volume1.config.has("extruder");
            has_extruder2 = model_volume2.config.has("extruder");
            if ((has_extruder1 != has_extruder2)
                || (has_extruder1 && model_volume1.config.extruder() != model_volume2.config.extruder()))
                return false;
            if (!model_volume1.supported_facets.equals(model_volume2.supported_facets))
                return false;
            if (!model_volume1.seam_facets.equals(model_volume2.seam_facets))
                return false;
            if (!model_volume1.mmu_segmentation_facets.equals(model_volume2.mmu_segmentation_facets))
                return false;
            if (!model_volume1.fuzzy_skin_facets.equals(model_volume2.fuzzy_skin_facets))
                return false;
            if (model_volume1.config.get() != model_volume2.config.get())
                return false;
        }
        //if (!object1->config().equals(object2->config()))
        //    return false;
        if (model_obj1->layer_height_profile.get() != model_obj2->layer_height_profile.get())
            return false;
        if (model_obj1->config.get() != model_obj2->config.get())
            return false;
        return true;
    };
    int object_count = m_objects.size();
    std::set<PrintObject*> need_slicing_objects;
    std::set<PrintObject*> re_slicing_objects;
    if (!use_cache) {
        for (int index = 0; index < object_count; index++)
        {
            PrintObject *obj =  m_objects[index];
            for (PrintObject *slicing_obj : need_slicing_objects)
            {
                if (is_print_object_the_same(obj, slicing_obj)) {
                    obj->set_shared_object(slicing_obj);
                    break;
                }
            }
            if (!obj->get_shared_object())
                need_slicing_objects.insert(obj);
        }
    }
    else {
        for (int index = 0; index < object_count; index++)
        {
            PrintObject *obj =  m_objects[index];
            if (obj->layer_count() > 0)
                need_slicing_objects.insert(obj);
        }
        for (int index = 0; index < object_count; index++)
        {
            PrintObject *obj =  m_objects[index];
            bool found_shared = false;
            if (need_slicing_objects.find(obj) == need_slicing_objects.end()) {
                for (PrintObject *slicing_obj : need_slicing_objects)
                {
                    if (is_print_object_the_same(obj, slicing_obj)) {
                        obj->set_shared_object(slicing_obj);
                        found_shared = true;
                        break;
                    }
                }
                if (!found_shared) {
                    BOOST_LOG_TRIVIAL(warning) << boost::format("Also can not find the shared object, identify_id %1%, maybe shared object is skipped")%obj->model_object()->instances[0]->loaded_id;
                    //throw Slic3r::SlicingError("Cannot find the cached data.");
                    //don't report errot, set use_cache to false, and reslice these objects
                    need_slicing_objects.insert(obj);
                    re_slicing_objects.insert(obj);
                    //use_cache = false;
                }
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": total object counts %1% in current print, need to slice %2%")%m_objects.size()%need_slicing_objects.size();
    BOOST_LOG_TRIVIAL(info) << "Starting the slicing process." << log_memory_info();
    if (!use_cache) {
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->make_perimeters();
            }
            else {
                if (obj->set_started(posSlice))
                    obj->set_done(posSlice);
                if (obj->set_started(posPerimeters))
                    obj->set_done(posPerimeters);
            }
        }
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->estimate_curled_extrusions();
            }
            else {
                if (obj->set_started(posEstimateCurledExtrusions))
                    obj->set_done(posEstimateCurledExtrusions);
            }
        }
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->infill();
            }
            else {
                if (obj->set_started(posPrepareInfill))
                    obj->set_done(posPrepareInfill);
                if (obj->set_started(posInfill))
                    obj->set_done(posInfill);
            }
        }
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->ironing();
            }
            else {
                if (obj->set_started(posIroning))
                    obj->set_done(posIroning);
            }
        }

        tbb::parallel_for(tbb::blocked_range<int>(0, int(m_objects.size())),
            [this, need_slicing_objects](const tbb::blocked_range<int>& range) {
                for (int i = range.begin(); i < range.end(); i++) {
                    PrintObject* obj = m_objects[i];
                    if (need_slicing_objects.count(obj) != 0) {
                        obj->generate_support_material();
                    }
                    else {
                        if (obj->set_started(posSupportMaterial))
                            obj->set_done(posSupportMaterial);
                    }
                }
            }
        );

        for (PrintObject* obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->detect_overhangs_for_lift();
            }
            else {
                if (obj->set_started(posDetectOverhangsForLift))
                    obj->set_done(posDetectOverhangsForLift);
            }
        }
    }
    else {
        for (PrintObject *obj : m_objects) {
            if (re_slicing_objects.count(obj) == 0) {
                if (obj->set_started(posSlice))
                    obj->set_done(posSlice);
                if (obj->set_started(posPerimeters))
                    obj->set_done(posPerimeters);
                if (obj->set_started(posPrepareInfill))
                    obj->set_done(posPrepareInfill);
                if (obj->set_started(posInfill))
                    obj->set_done(posInfill);
                if (obj->set_started(posIroning))
                    obj->set_done(posIroning);
                if (obj->set_started(posSupportMaterial))
                    obj->set_done(posSupportMaterial);
                if (obj->set_started(posDetectOverhangsForLift))
                    obj->set_done(posDetectOverhangsForLift);
            }
            else {
                obj->make_perimeters();
                obj->infill();
                obj->ironing();
                obj->generate_support_material();
                obj->detect_overhangs_for_lift();
                obj->estimate_curled_extrusions();
            }
        }
    }

    for (PrintObject *obj : m_objects)
    {
        if (need_slicing_objects.count(obj) == 0) {
            obj->copy_layers_from_shared_object();
            obj->copy_layers_overhang_from_shared_object();
        }
    }



    if (this->set_started(psWipeTower)) {
        {
            std::vector<std::set<int>> geometric_unprintables(m_config.nozzle_diameter.size());
            for (PrintObject* obj : m_objects) {
                std::vector<std::set<int>> obj_geometric_unprintables = obj->detect_extruder_geometric_unprintables();
                for (size_t idx = 0; idx < obj_geometric_unprintables.size(); ++idx) {
                    if (idx < geometric_unprintables.size()) {
                        geometric_unprintables[idx].insert(obj_geometric_unprintables[idx].begin(), obj_geometric_unprintables[idx].end());
                    }
                }
            }
            this->set_geometric_unprintable_filaments(geometric_unprintables);
        }

        m_wipe_tower_data.clear();
        m_tool_ordering.clear();
        if (this->has_wipe_tower()) {
            this->_make_wipe_tower();
        }
        else if (this->config().print_sequence != PrintSequence::ByObject) {
            // Initialize the tool ordering, so it could be used by the G-code preview slider for planning tool changes and filament switches.
            m_tool_ordering = ToolOrdering(*this, -1, false);
            m_tool_ordering.sort_and_build_data(*this, -1, false);
            if (m_tool_ordering.empty() || m_tool_ordering.last_extruder() == unsigned(-1))
                throw Slic3r::SlicingError("The print is empty. The model is not printable with current print settings.");

        }
        this->set_done(psWipeTower);
    }

    if (this->has_wipe_tower()) {
        m_fake_wipe_tower.set_pos({ m_config.wipe_tower_x.get_at(m_plate_index), m_config.wipe_tower_y.get_at(m_plate_index) });
    }

    if (this->set_started(psSkirtBrim)) {
        this->set_status(70, L("Generating skirt & brim"));

        if (time_cost_with_cache)
            start_time = (long long)Slic3r::Utils::get_current_time_utc();

        m_skirt.clear();
        m_skirt_convex_hull.clear();
        m_first_layer_convex_hull.points.clear();
        for (PrintObject *object : m_objects)  object->m_skirt.clear();

        const bool draft_shield = config().draft_shield != dsDisabled;

        if (this->has_skirt() && draft_shield) {
            // In case that draft shield is active, generate skirt first so brim
            // can be trimmed to make room for it.
            _make_skirt();
        }

        //BBS: get the objects' indices when GCodes are generated
        ToolOrdering tool_ordering;
        unsigned int initial_extruder_id = (unsigned int)-1;
        bool         has_wipe_tower = false;
        std::vector<const PrintInstance*> 					print_object_instances_ordering;
        std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
        std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> layers_to_print = GCode::collect_layers_to_print(*this);
        std::vector<unsigned int> printExtruders;
        if (this->config().print_sequence == PrintSequence::ByObject) {
            // Order object instances for sequential print.
            print_object_instances_ordering = sort_object_instances_by_model_order(*this);
            std::vector<unsigned int> first_layer_used_filaments;
            std::vector<std::vector<unsigned int>> all_filaments;
            for (print_object_instance_sequential_active = print_object_instances_ordering.begin(); print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
                tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
                for (size_t idx = 0; idx < tool_ordering.layer_tools().size(); ++idx) {
                    auto& layer_filament = tool_ordering.layer_tools()[idx].extruders;
                    all_filaments.emplace_back(layer_filament);
                    if (idx == 0)
                        first_layer_used_filaments.insert(first_layer_used_filaments.end(), layer_filament.begin(), layer_filament.end());
                }
            }
            sort_remove_duplicates(first_layer_used_filaments);
            auto used_filaments = collect_sorted_used_filaments(all_filaments);
            this->set_slice_used_filaments(first_layer_used_filaments,used_filaments);

            auto physical_unprintables = this->get_physical_unprintable_filaments(used_filaments);
            auto geometric_unprintables = this->get_geometric_unprintable_filaments();
            std::vector<int>filament_maps = this->get_filament_maps();
            auto map_mode = get_filament_map_mode();
            // get recommended filament map
            if (map_mode < FilamentMapMode::fmmManual) {
                filament_maps = ToolOrdering::get_recommended_filament_maps(all_filaments, this, map_mode, physical_unprintables, geometric_unprintables);
                std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(), [](int value) { return value + 1; });
                update_filament_maps_to_config(filament_maps);
            }
            // check map valid both in auto and mannual mode
            std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(), [](int value) {return value - 1; });

            //        print_object_instances_ordering = sort_object_instances_by_max_z(print);
            print_object_instance_sequential_active = print_object_instances_ordering.begin();
            for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
                tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
                tool_ordering.sort_and_build_data(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
                if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<unsigned int>(-1)) {
                    append(printExtruders, tool_ordering.tools_for_layer(layers_to_print.front().first).extruders);
                }
            }
        }
        else {
            tool_ordering = this->tool_ordering();
            tool_ordering.assign_custom_gcodes(*this);

            std::vector<unsigned int> first_layer_used_filaments;
            if (!tool_ordering.layer_tools().empty())
                first_layer_used_filaments = tool_ordering.layer_tools().front().extruders;

            this->set_slice_used_filaments(first_layer_used_filaments, tool_ordering.all_extruders());
            has_wipe_tower = this->has_wipe_tower() && tool_ordering.has_wipe_tower();
            initial_extruder_id = tool_ordering.first_extruder();
            print_object_instances_ordering = chain_print_object_instances(*this);
            append(printExtruders, tool_ordering.tools_for_layer(layers_to_print.front().first).extruders);
        }

        auto objectExtruderMap = getObjectExtruderMap(*this);
        std::vector<std::pair<ObjectID, unsigned int>> objPrintVec;
        for (const PrintInstance* instance : print_object_instances_ordering) {
            const ObjectID& print_object_ID = instance->print_object->id();
            bool existObject = false;
            for (auto& objIDPair : objPrintVec) {
                if (print_object_ID == objIDPair.first) existObject = true;
            }
            if (!existObject && objectExtruderMap.find(print_object_ID) != objectExtruderMap.end())
                objPrintVec.push_back(std::make_pair(print_object_ID, objectExtruderMap.at(print_object_ID)));
        }
        // BBS: m_brimMap and m_supportBrimMap are used instead of m_brim to generate brim of objs and supports seperately
        m_brimMap.clear();
        m_supportBrimMap.clear();
        m_first_layer_convex_hull.points.clear();
        if (this->has_brim()) {
            Polygons islands_area;
            make_brim(*this, this->make_try_cancel(), islands_area, m_brimMap,
                m_supportBrimMap, objPrintVec, printExtruders);
            for (Polygon& poly_ex : islands_area)
                poly_ex.douglas_peucker(SCALED_RESOLUTION);
            for (Polygon &poly : union_(this->first_layer_islands(), islands_area))
                append(m_first_layer_convex_hull.points, std::move(poly.points));
        }


        if (has_skirt() && ! draft_shield) {
            // In case that draft shield is NOT active, generate skirt now.
            // It will be placed around the brim, so brim has to be ready.
            assert(m_skirt.empty());
            _make_skirt();
        }

        this->finalize_first_layer_convex_hull();
        this->set_done(psSkirtBrim);

        if (time_cost_with_cache) {
            end_time = (long long)Slic3r::Utils::get_current_time_utc();
            *time_cost_with_cache = *time_cost_with_cache + end_time - start_time;
        }
    }
    //BBS
    for (PrintObject *obj : m_objects) {
        if (((!use_cache)&&(need_slicing_objects.count(obj) != 0))
            || (use_cache &&(re_slicing_objects.count(obj) != 0))){
            obj->simplify_extrusion_path();
        }
        else {
            if (obj->set_started(posSimplifyPath))
                obj->set_done(posSimplifyPath);
            if (obj->set_started(posSimplifyInfill))
                obj->set_done(posSimplifyInfill);
            if (obj->set_started(posSimplifySupportPath))
                obj->set_done(posSimplifySupportPath);
        }
    }

    // BBS
    bool has_adaptive_layer_height = false;
    for (PrintObject* obj : m_objects) {
        if (obj->model_object()->layer_height_profile.empty() == false) {
            has_adaptive_layer_height = true;
            break;
        }
    }
    if(!m_no_check /*&& !has_adaptive_layer_height*/)
    {
        using Clock                 = std::chrono::high_resolution_clock;
        auto            startTime   = Clock::now();
        std::optional<const FakeWipeTower *> wipe_tower_opt = {};
        if (this->has_wipe_tower()) {
            m_fake_wipe_tower.set_pos({m_config.wipe_tower_x.get_at(m_plate_index), m_config.wipe_tower_y.get_at(m_plate_index)});
            wipe_tower_opt = std::make_optional<const FakeWipeTower *>(&m_fake_wipe_tower);
        }
        auto            conflictRes = ConflictChecker::find_inter_of_lines_in_diff_objs(m_objects, wipe_tower_opt);
        auto            endTime     = Clock::now();
        volatile double seconds     = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count() / (double) 1000;
        BOOST_LOG_TRIVIAL(info) << "gcode path conflicts check takes " << seconds << " secs.";

        m_conflict_result = conflictRes;
        if (conflictRes.has_value()) {
            BOOST_LOG_TRIVIAL(error) << boost::format("gcode path conflicts found between %1% and %2%")%conflictRes.value()._objName1 %conflictRes.value()._objName2;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Slicing process finished." << log_memory_info();
}

// G-code export process, running at a background thread.
// The export_gcode may die for various reasons (fails to process filename_format,
// write error into the G-code, cannot execute post-processing scripts).
// It is up to the caller to show an error message.
std::string Print::export_gcode(const std::string& path_template, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    // output everything to a G-code file
    // The following call may die if the filename_format template substitution fails.
    std::string path = this->output_filepath(path_template);
    std::string message;
    if (!path.empty() && result == nullptr) {
        // Only show the path if preview_data is not set -> running from command line.
        message = L("Exporting G-code");
        message += " to ";
        message += path;
    } else
        message = L("Generating G-code");
    this->set_status(80, message);

    // The following line may die for multiple reasons.
    GCode gcode;
    //BBS: compute plate offset for gcode-generator
    const Vec3d origin = this->get_plate_origin();
    gcode.set_gcode_offset(origin(0), origin(1));
    gcode.do_export(this, path.c_str(), result, thumbnail_cb);
    gcode.export_layer_filaments(result);
    //BBS
    result->conflict_result = m_conflict_result;
    return path.c_str();
}

void Print::_make_skirt()
{
    // First off we need to decide how tall the skirt must be.
    // The skirt_height option from config is expressed in layers, but our
    // object might have different layer heights, so we need to find the print_z
    // of the highest layer involved.
    // Note that unless has_infinite_skirt() == true
    // the actual skirt might not reach this $skirt_height_z value since the print
    // order of objects on each layer is not guaranteed and will not generally
    // include the thickest object first. It is just guaranteed that a skirt is
    // prepended to the first 'n' layers (with 'n' = skirt_height).
    // $skirt_height_z in this case is the highest possible skirt height for safety.
    coordf_t skirt_height_z = 0.;
    for (const PrintObject *object : m_objects) {
        size_t skirt_layers = this->has_infinite_skirt() ?
            object->layer_count() :
            std::min(size_t(m_config.skirt_height.value), object->layer_count());
        skirt_height_z = std::max(skirt_height_z, object->m_layers[skirt_layers-1]->print_z);
    }

    // Collect points from all layers contained in skirt height.
    Points points;

    // BBS
    std::map<PrintObject*, Polygon> object_convex_hulls;
    for (PrintObject *object : m_objects) {
        Points object_points;
        // Get object layers up to skirt_height_z.
        for (const Layer *layer : object->m_layers) {
            if (layer->print_z > skirt_height_z)
                break;
            for (const ExPolygon &expoly : layer->lslices)
                // Collect the outer contour points only, ignore holes for the calculation of the convex hull.
                append(object_points, expoly.contour.points);
        }
        // Get support layers up to skirt_height_z.
        for (const SupportLayer *layer : object->support_layers()) {
            if (layer->print_z > skirt_height_z)
                break;
            layer->support_fills.collect_points(object_points);
        }

        object_convex_hulls.insert({ object, Slic3r::Geometry::convex_hull(object_points) });

        // Repeat points for each object copy.
        for (const PrintInstance &instance : object->instances()) {
            Points copy_points = object_points;
            for (Point &pt : copy_points)
                pt += instance.shift;
            append(points, copy_points);
        }
    }

    // Include the wipe tower.
    append(points, this->first_layer_wipe_tower_corners());

    // Unless draft shield is enabled, include all brims as well.
    if (config().draft_shield == dsDisabled)
        append(points, m_first_layer_convex_hull.points);

    if (points.size() < 3)
        // At least three points required for a convex hull.
        return;

    this->throw_if_canceled();
    Polygon convex_hull = Slic3r::Geometry::convex_hull(points);

    // Skirt may be printed on several layers, having distinct layer heights,
    // but loops must be aligned so can't vary width/spacing
    // TODO: use each extruder's own flow
    double initial_layer_print_height = this->skirt_first_layer_height();
    Flow   flow = this->skirt_flow();
    float  spacing = flow.spacing();
    double mm3_per_mm = flow.mm3_per_mm();

    std::vector<size_t> extruders;
    std::vector<double> extruders_e_per_mm;
    {
        auto set_extruders = this->extruders();
        extruders.reserve(set_extruders.size());
        extruders_e_per_mm.reserve(set_extruders.size());
        for (auto &extruder_id : set_extruders) {
            extruders.push_back(extruder_id);
            extruders_e_per_mm.push_back(Extruder((unsigned int)extruder_id, &m_config, m_config.single_extruder_multi_material).e_per_mm(mm3_per_mm));
        }
    }

    // Initial offset of the brim inner edge from the object (possible with a support & raft).
    // The skirt will touch the brim if the brim is extruded.
    auto   distance = float(scale_(m_config.skirt_distance.value - spacing/2.));
    // Draw outlines from outside to inside.
    // Loop while we have less skirts than required or any extruder hasn't reached the min length if any.
    std::vector<coordf_t> extruded_length(extruders.size(), 0.);
    if (m_config.skirt_type == stCombined) {
        for (size_t i = m_config.skirt_loops, extruder_idx = 0; i > 0; -- i) {
            this->throw_if_canceled();
            // Offset the skirt outside.
            distance += float(scale_(spacing));
            // Generate the skirt centerline.
            Polygon loop;
            {
                // BBS. skirt_distance is defined as the gap between skirt and outer most brim, so no need to add max_brim_width
                Polygons loops = offset(convex_hull, distance, ClipperLib::jtRound, float(scale_(0.1)));
                Geometry::simplify_polygons(loops, scale_(0.05), &loops);
			    if (loops.empty())
				    break;
			    loop = loops.front();
            }
            // Extrude the skirt loop.
            ExtrusionLoop eloop(elrSkirt);
            eloop.paths.emplace_back(ExtrusionPath(
                ExtrusionPath(
                    erSkirt,
                    (float)mm3_per_mm,         // this will be overridden at G-code export time
                    flow.width(),
				    (float)initial_layer_print_height  // this will be overridden at G-code export time
                )));
            eloop.paths.back().polyline = loop.split_at_first_point();
            m_skirt.append(eloop);
            if (m_config.min_skirt_length.value > 0) {
                // The skirt length is limited. Sum the total amount of filament length extruded, in mm.
                extruded_length[extruder_idx] += unscale<double>(loop.length()) * extruders_e_per_mm[extruder_idx];
                if (extruded_length[extruder_idx] < m_config.min_skirt_length.value) {
                    // Not extruded enough yet with the current extruder. Add another loop.
                    if (i == 1)
                        ++ i;
                } else {
                    assert(extruded_length[extruder_idx] >= m_config.min_skirt_length.value);
                    // Enough extruded with the current extruder. Extrude with the next one,
                    // until the prescribed number of skirt loops is extruded.
                    if (extruder_idx + 1 < extruders.size())
                        ++ extruder_idx;
                }
            } else {
                // The skirt lenght is not limited, extrude the skirt with the 1st extruder only.
            }
        }
    } else {
        m_skirt.clear();
    }
    // Brims were generated inside out, reverse to print the outmost contour first.
    m_skirt.reverse();

    // Remember the outer edge of the last skirt line extruded as m_skirt_convex_hull.
    for (Polygon &poly : offset(convex_hull, distance + 0.5f * float(scale_(spacing)), ClipperLib::jtRound, float(scale_(0.1))))
        append(m_skirt_convex_hull, std::move(poly.points));

    if (m_config.skirt_type == stPerObject) {
        // BBS
        for (auto obj_cvx_hull : object_convex_hulls) {
            double object_skirt_distance = float(scale_(m_config.skirt_distance.value - spacing/2.));
            PrintObject* object = obj_cvx_hull.first;
            object->m_skirt.clear();
            extruded_length.assign(extruded_length.size(), 0.);
            for (size_t i = m_config.skirt_loops.value, extruder_idx = 0; i > 0; -- i) {
                object_skirt_distance += float(scale_(spacing));
                Polygon loop;
                {
                    // BBS. skirt_distance is defined as the gap between skirt and outer most brim, so no need to add max_brim_width
                    Polygons loops = offset(obj_cvx_hull.second, object_skirt_distance, ClipperLib::jtRound, float(scale_(0.1)));
                    Geometry::simplify_polygons(loops, scale_(0.05), &loops);
                    if (loops.empty())
                        break;
                    loop = loops.front();
                }

                // Extrude the skirt loop.
                ExtrusionLoop eloop(elrSkirt);
                eloop.paths.emplace_back(ExtrusionPath(
                    ExtrusionPath(
                        erSkirt,
                        (float)mm3_per_mm,         // this will be overridden at G-code export time
                        flow.width(),
                        (float)initial_layer_print_height  // this will be overridden at G-code export time
                    )));
                eloop.paths.back().polyline = loop.split_at_first_point();
                object->m_skirt.append(std::move(eloop));
                if (m_config.min_skirt_length.value > 0) {
                    // The skirt length is limited. Sum the total amount of filament length extruded, in mm.
                    extruded_length[extruder_idx] += unscale<double>(loop.length()) * extruders_e_per_mm[extruder_idx];
                    if (extruded_length[extruder_idx] < m_config.min_skirt_length.value) {
                        // Not extruded enough yet with the current extruder. Add another loop.
                        if (i == 1)
                            ++ i;
                    } else {
                        assert(extruded_length[extruder_idx] >= m_config.min_skirt_length.value);
                        // Enough extruded with the current extruder. Extrude with the next one,
                        // until the prescribed number of skirt loops is extruded.
                        if (extruder_idx + 1 < extruders.size())
                            ++ extruder_idx;
                    }
                } else {
                    // The skirt lenght is not limited, extrude the skirt with the 1st extruder only.
                }

            }
            object->m_skirt.reverse();
        }
    }
}

Polygons Print::first_layer_islands() const
{
    Polygons islands;
    for (PrintObject *object : m_objects) {
        Polygons object_islands;
        for (ExPolygon &expoly : object->m_layers.front()->lslices)
            object_islands.push_back(expoly.contour);
        if (!object->support_layers().empty()) {
            if (object->support_layers().front()->support_type==stInnerNormal)
                object->support_layers().front()->support_fills.polygons_covered_by_spacing(object_islands, float(SCALED_EPSILON));
            else if(object->support_layers().front()->support_type==stInnerTree) {
                ExPolygons &expolys_first_layer = object->m_support_layers.front()->lslices;
                for (ExPolygon &expoly : expolys_first_layer) { object_islands.push_back(expoly.contour); }
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance &instance : object->instances())
            for (Polygon &poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(instance.shift);
            }
    }
    return islands;
}

Points Print::first_layer_wipe_tower_corners(bool check_wipe_tower_existance) const
{
    Points corners;
    if (check_wipe_tower_existance && (!has_wipe_tower() || m_wipe_tower_data.tool_changes.empty()))
        return corners;
    {
        double width = m_wipe_tower_data.bbx.max.x() - m_wipe_tower_data.bbx.min.x();
        double depth = m_wipe_tower_data.bbx.max.y() -m_wipe_tower_data.bbx.min.y();
        Vec2d  pt0   = m_wipe_tower_data.bbx.min + m_wipe_tower_data.rib_offset.cast<double>();
        
        // First the corners.
        std::vector<Vec2d> pts = { pt0,
                                   Vec2d(pt0.x()+width, pt0.y()),
                                   Vec2d(pt0.x()+width, pt0.y()+depth),
                                   Vec2d(pt0.x(),pt0.y()+depth)
                                 };

        // Now the stabilization cone.
        Vec2d center = (pts[0] + pts[2])/2.;
        const auto [cone_R, cone_x_scale] = WipeTower2::get_wipe_tower_cone_base(m_config.prime_tower_width, m_wipe_tower_data.height, m_wipe_tower_data.depth, m_config.wipe_tower_cone_angle);
        double r = cone_R + m_wipe_tower_data.brim_width;
        for (double alpha = 0.; alpha<2*M_PI; alpha += M_PI/20.)
            pts.emplace_back(center + r*Vec2d(std::cos(alpha)/cone_x_scale, std::sin(alpha)));

        for (Vec2d& pt : pts) {
            pt = Eigen::Rotation2Dd(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value)) * pt;
            //Orca: offset the wipe tower to the plate origin
            pt += Vec2d(m_config.wipe_tower_x.get_at(m_plate_index) + m_origin(0), m_config.wipe_tower_y.get_at(m_plate_index) + m_origin(1));
            corners.emplace_back(Point(scale_(pt.x()), scale_(pt.y())));
        }
    }
    return corners;
}

//SoftFever
Vec2d Print::translate_to_print_space(const Vec2d &point) const {
    //const BoundingBoxf bed_bbox(config().printable_area.values);
    return Vec2d(point(0) - m_origin(0), point(1) - m_origin(1));
}

Vec2d Print::translate_to_print_space(const Point &point) const {
    return Vec2d(unscaled(point.x()) - m_origin(0), unscaled(point.y()) - m_origin(1));
}

FilamentTempType Print::get_filament_temp_type(const std::string& filament_type)
{
    // FilamentTempType Temperature-based logic
    int min_temp, max_temp;
    if (MaterialType::get_temperature_range(filament_type, min_temp, max_temp)) {
        if (max_temp <= 250) return FilamentTempType::LowTemp;
        else if (max_temp < 280) return FilamentTempType::HighLowCompatible;
        else return FilamentTempType::HighTemp;
    }

    const static std::string HighTempFilamentStr = "high_temp_filament";
    const static std::string LowTempFilamentStr = "low_temp_filament";
    const static std::string HighLowCompatibleFilamentStr = "high_low_compatible_filament";
    static std::unordered_map<std::string, std::unordered_set<std::string>>filament_temp_type_map;

    if (filament_temp_type_map.empty()) {
        fs::path file_path = fs::path(resources_dir()) / "info" / "filament_info.json";
        std::ifstream in(file_path.string());
        json j;
        try{
            j = json::parse(in);
            in.close();
            auto&&high_temp_filament_arr =j[HighTempFilamentStr].get < std::vector<std::string>>();
            filament_temp_type_map[HighTempFilamentStr] = std::unordered_set<std::string>(high_temp_filament_arr.begin(), high_temp_filament_arr.end());
            auto&& low_temp_filament_arr = j[LowTempFilamentStr].get < std::vector<std::string>>();
            filament_temp_type_map[LowTempFilamentStr] = std::unordered_set<std::string>(low_temp_filament_arr.begin(), low_temp_filament_arr.end());
            auto&& high_low_compatible_filament_arr = j[HighLowCompatibleFilamentStr].get < std::vector<std::string>>();
            filament_temp_type_map[HighLowCompatibleFilamentStr] = std::unordered_set<std::string>(high_low_compatible_filament_arr.begin(), high_low_compatible_filament_arr.end());
        }
        catch (const json::parse_error& err){
            in.close();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << file_path.string() << " got a nlohmann::detail::parse_error, reason = " << err.what();
            filament_temp_type_map[HighTempFilamentStr] = {"ABS","ASA","PC","PA","PA-CF","PA-GF","PA6-CF","PET-CF", "PETG-GF","PPS","PPS-CF","PPA-GF","PPA-CF","ABS-Aero","ABS-GF"};
            filament_temp_type_map[LowTempFilamentStr] = {"PLA","TPU","PLA-CF","PLA-AERO","PVA","BVOH","SBS"};
            filament_temp_type_map[HighLowCompatibleFilamentStr] = { "HIPS","PETG","PCTG","PE","PP","EVA","PE-CF","PP-CF","PP-GF","PHA"};
        }
    }

    if (filament_temp_type_map[HighLowCompatibleFilamentStr].find(filament_type) != filament_temp_type_map[HighLowCompatibleFilamentStr].end())
        return HighLowCompatible;
    if (filament_temp_type_map[HighTempFilamentStr].find(filament_type) != filament_temp_type_map[HighTempFilamentStr].end())
        return HighTemp;
    if (filament_temp_type_map[LowTempFilamentStr].find(filament_type) != filament_temp_type_map[LowTempFilamentStr].end())
        return LowTemp;
    return Undefine;
}

int Print::get_hrc_by_nozzle_type(const NozzleType&type)
{
    static std::map<std::string, int>nozzle_type_to_hrc;
    if (nozzle_type_to_hrc.empty()) {
        fs::path file_path = fs::path(resources_dir()) / "info" / "nozzle_info.json";
        boost::nowide::ifstream in(file_path.string());
        //std::ifstream in(file_path.string());
        json j;
        try {
            j = json::parse(in);
            in.close();
            for (const auto& elem : j["nozzle_hrc"].items())
                nozzle_type_to_hrc[elem.key()] = elem.value();
        }
        catch (const json::parse_error& err) {
            in.close();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << file_path.string() << " got a nlohmann::detail::parse_error, reason = " << err.what();
            nozzle_type_to_hrc = {
                {"hardened_steel",55},
                {"stainless_steel",20},
                {"tungsten_carbide", 85},
                {"brass",2},
                {"undefine",0}
            };
        }
    }
    auto iter = nozzle_type_to_hrc.find(NozzleTypeEumnToStr[type]);
    if (iter != nozzle_type_to_hrc.end())
        return iter->second;
    //0 represents undefine
    return 0;
}

std::vector<std::string> Print::get_incompatible_filaments_by_nozzle(const float nozzle_diameter, const std::optional<NozzleVolumeType> nozzle_volume_type)
{
    static std::map<std::string, std::map<std::string, std::vector<std::string>>> incompatible_filaments;
    if(incompatible_filaments.empty()){
        fs::path file_path = fs::path(resources_dir()) / "info" / "nozzle_incompatibles.json";
        boost::nowide::ifstream in(file_path.string());
        json j;
        try {
            j = json::parse(in);
            for(auto& [volume_type, diameter_list] : j["incompatible_nozzles"].items()) {
                for(auto& [diameter, filaments]: diameter_list.items()){
                    incompatible_filaments[volume_type][diameter] = filaments.get<std::vector<std::string>>();
                }
            }
        }
        catch(const json::parse_error& err){
            in.close();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << file_path.string() << " got a nlohmann::detail::parse_error, reason = " << err.what();

            incompatible_filaments[get_nozzle_volume_type_string(NozzleVolumeType::nvtHighFlow)] = {};
            incompatible_filaments[get_nozzle_volume_type_string(NozzleVolumeType::nvtStandard)] = {};
        }
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << nozzle_diameter;
    std::string diameter_str = oss.str();

    if(nozzle_volume_type.has_value()){
        return incompatible_filaments[get_nozzle_volume_type_string(nozzle_volume_type.value())][diameter_str];
    }

    std::vector<std::string> incompatible_filaments_list;
    for(auto& [volume_type, diameter_list] : incompatible_filaments){
        auto iter = diameter_list.find(diameter_str);
        if(iter != diameter_list.end()){
            append(incompatible_filaments_list, iter->second);
        }
    }
    return incompatible_filaments_list;
}

void Print::finalize_first_layer_convex_hull()
{
    append(m_first_layer_convex_hull.points, m_skirt_convex_hull);
    if (m_first_layer_convex_hull.empty()) {
        // Neither skirt nor brim was extruded. Collect points of printed objects from 1st layer.
        for (Polygon &poly : this->first_layer_islands())
            append(m_first_layer_convex_hull.points, std::move(poly.points));
    }
    append(m_first_layer_convex_hull.points, this->first_layer_wipe_tower_corners());
    m_first_layer_convex_hull = Geometry::convex_hull(m_first_layer_convex_hull.points);
}

void Print::update_filament_maps_to_config(std::vector<int> f_maps)
{
    if (m_config.filament_map.values != f_maps)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": filament maps changed after pre-slicing.");
        m_ori_full_print_config.option<ConfigOptionInts>("filament_map", true)->values = f_maps;
        m_config.filament_map.values = f_maps;

        m_full_print_config = m_ori_full_print_config;
        m_full_print_config.update_values_to_printer_extruders_for_multiple_filaments(m_full_print_config, filament_options_with_variant,  "filament_self_index", "filament_extruder_variant");

        const std::vector<std::string> &extruder_retract_keys = print_config_def.extruder_retract_keys();
        const std::string               filament_prefix       = "filament_";
        t_config_option_keys            print_diff;
        DynamicPrintConfig              filament_overrides;
        for (auto& opt_key: extruder_retract_keys)
        {
            const ConfigOption *opt_new_filament = m_full_print_config.option(filament_prefix + opt_key);
            const ConfigOption *opt_new_machine = m_full_print_config.option(opt_key);
            const ConfigOption *opt_old_machine = m_config.option(opt_key);

            if (opt_new_filament)
                compute_filament_override_value(opt_key, opt_old_machine, opt_new_machine, opt_new_filament, m_full_print_config, print_diff, filament_overrides, f_maps);
        }

        t_config_option_keys keys(filament_options_with_variant.begin(), filament_options_with_variant.end());
        m_config.apply_only(m_full_print_config, keys, true);
        if (!print_diff.empty()) {
            m_placeholder_parser.apply_config(filament_overrides);
            m_config.apply(filament_overrides);
        }
    }
    m_has_auto_filament_map_result = true;
}

void Print::apply_config_for_render(const DynamicConfig &config)
{
    m_config.apply(config);
}

std::vector<int> Print::get_filament_maps() const
{
    return m_config.filament_map.values;
}

FilamentMapMode Print::get_filament_map_mode() const
{
    return m_config.filament_map_mode;
}

std::vector<std::set<int>> Print::get_physical_unprintable_filaments(const std::vector<unsigned int>& used_filaments) const
{
    int extruder_num = m_config.nozzle_diameter.size();
    std::vector<std::set<int>>physical_unprintables(extruder_num);
    if (extruder_num < 2)
        return physical_unprintables;

    auto get_unprintable_extruder_id = [&](unsigned int filament_idx) -> int {
        int status = m_config.filament_printable.values[filament_idx];
        for (int i = 0; i < extruder_num; ++i) {
            if (!(status >> i & 1)) {
                return i;
            }
        }
        return -1;
    };


    std::set<int> tpu_filaments;
    for (auto f : used_filaments) {
        if (m_config.filament_type.get_at(f) == "TPU")
            tpu_filaments.insert(f);
    }

    for (auto f : used_filaments) {
        int extruder_id = get_unprintable_extruder_id(f);
        if (extruder_id == -1)
            continue;
        physical_unprintables[extruder_id].insert(f);
    }

    return physical_unprintables;
}


std::vector<double> Print::get_extruder_printable_height() const
{
    return m_config.extruder_printable_height.values;
}

std::vector<Polygons> Print::get_extruder_printable_polygons() const
{
    std::vector<Polygons>           extruder_printable_polys;
    std::vector<std::vector<Vec2d>> extruder_printable_areas = m_config.extruder_printable_area.values;
    for (const auto &e_printable_area : extruder_printable_areas) {
        Polygons ploys = {Polygon::new_scale(e_printable_area)};
        extruder_printable_polys.emplace_back(ploys);
    }
    return std::move(extruder_printable_polys);
}

std::vector<Polygons> Print::get_extruder_unprintable_polygons() const
{
    std::vector<Vec2d>              printable_area           = m_config.printable_area.values;
    Polygon                         printable_poly           = Polygon::new_scale(printable_area);
    std::vector<std::vector<Vec2d>> extruder_printable_areas = m_config.extruder_printable_area.values;
    std::vector<Polygons>           extruder_unprintable_polys;
    for (const auto &e_printable_area : extruder_printable_areas) {
        Polygons ploys = diff(printable_poly, Polygon::new_scale(e_printable_area));
        extruder_unprintable_polys.emplace_back(ploys);
    }
    return std::move(extruder_unprintable_polys);
}

size_t Print::get_extruder_id(unsigned int filament_id) const
{
    std::vector<int> filament_map = get_filament_maps();
    if (filament_id < filament_map.size()) {
        return filament_map[filament_id] - 1;
    }
    return 0;
}

// Wipe tower support.
bool Print::has_wipe_tower() const
{
    if (m_config.enable_prime_tower.value == true) {
        if (m_config.enable_wrapping_detection.value && m_config.wrapping_exclude_area.values.size() > 2)
            return true;

        if (enable_timelapse_print())
            return true;

        return !m_config.spiral_mode.value && m_config.filament_diameter.values.size() > 1;
    }
    return false;
}

const WipeTowerData &Print::wipe_tower_data(size_t filaments_cnt) const
{
    // If the wipe tower wasn't created yet, make sure the depth and brim_width members are set to default.
    double max_height = 0;
    for (size_t obj_idx = 0; obj_idx < m_objects.size(); obj_idx++) {
        double object_z = (double) m_objects[obj_idx]->size().z();
        max_height      = std::max(unscale_(object_z), max_height);
    }
    if (max_height < EPSILON) return m_wipe_tower_data;

    double layer_height                  = 0.08f; // hard code layer height
    layer_height        = m_objects.front()->config().layer_height.value;

    auto   timelapse_type  = config().option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
    bool   need_wipe_tower = (timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false) | (m_config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib);
    double extra_spacing = config().option("prime_tower_infill_gap")->getFloat() / 100.;
    double rib_width     = config().option("wipe_tower_rib_width")->getFloat();

    double filament_change_volume = 0.;
    {
        std::vector<double> filament_change_lengths;
        auto                filament_change_lengths_opt = config().option<ConfigOptionFloats>("filament_change_length");
        if (filament_change_lengths_opt) filament_change_lengths = filament_change_lengths_opt->values;
        double              length   = filament_change_lengths.empty() ? 0 : *std::max_element(filament_change_lengths.begin(), filament_change_lengths.end());
        double              diameter = 1.75;
        std::vector<double> diameters;
        auto                filament_diameter_opt = config().option<ConfigOptionFloats>("filament_diameter");
        if (filament_diameter_opt) diameters = filament_diameter_opt->values;
        diameter               = diameters.empty() ? diameter : *std::max_element(diameters.begin(), diameters.end());
        filament_change_volume = length * PI * diameter * diameter / 4.;
    }


    if (! is_step_done(psWipeTower) && filaments_cnt !=0) {
        double wipe_volume  = m_config.prime_volume;
        int filament_depth_count = m_config.nozzle_diameter.values.size() == 2 ? filaments_cnt : filaments_cnt - 1;
        if (filaments_cnt == 1 && enable_timelapse_print()) filament_depth_count = 1;
        double volume = wipe_volume * filament_depth_count;
        if (m_config.nozzle_diameter.values.size() == 2) volume += filament_change_volume * (int) (filaments_cnt / 2);

        if (m_config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib) {
            double depth = std::sqrt(volume / layer_height * extra_spacing);
            if (need_wipe_tower || filaments_cnt > 1) {
                float min_wipe_tower_depth = WipeTower::get_limit_depth_by_height(max_height);
                depth  = std::max((double) min_wipe_tower_depth, depth);
                depth += rib_width / std::sqrt(2) + config().wipe_tower_extra_rib_length.value;
                const_cast<Print *>(this)->m_wipe_tower_data.depth = depth;
                const_cast<Print *>(this)->m_wipe_tower_data.brim_width = m_config.prime_tower_brim_width;
            }
        }
        else {
        double width        = m_config.prime_tower_width;
        if (m_config.purge_in_prime_tower && m_config.single_extruder_multi_material) {
            // Calculating depth should take into account currently set wiping volumes.
            // For a long time, the initial preview would just use 900/width per toolchange (15mm on a 60mm wide tower)
            // and it worked well enough. Let's try to do slightly better by accounting for the purging volumes.
            std::vector<std::vector<float>> wipe_volumes = WipeTower2::extract_wipe_volumes(m_config);
            std::vector<float>              max_wipe_volumes;
            for (const std::vector<float> &v : wipe_volumes)
                max_wipe_volumes.emplace_back(*std::max_element(v.begin(), v.end()));
            float maximum = std::accumulate(max_wipe_volumes.begin(), max_wipe_volumes.end(), 0.f);
            maximum       = maximum * filaments_cnt / max_wipe_volumes.size();
            
            // Orca: it's overshooting a bit, so let's reduce it a bit
            maximum *= 0.6; 
            const_cast<Print *>(this)->m_wipe_tower_data.depth = maximum / (layer_height * width);
        } else {
            double depth = volume / (layer_height * width) * extra_spacing;
            if (need_wipe_tower || m_wipe_tower_data.depth > EPSILON) {
                float min_wipe_tower_depth = WipeTower::get_limit_depth_by_height(max_height);
                depth = std::max((double) min_wipe_tower_depth, depth);
            }
            const_cast<Print *>(this)->m_wipe_tower_data.depth = depth;
        }
        const_cast<Print *>(this)->m_wipe_tower_data.brim_width = m_config.prime_tower_brim_width;
        }
        if (m_config.prime_tower_brim_width < 0) const_cast<Print *>(this)->m_wipe_tower_data.brim_width = WipeTower::get_auto_brim_by_height(max_height);
    }
    return m_wipe_tower_data;
}

bool Print::enable_timelapse_print() const
{
    return m_config.timelapse_type.value == TimelapseType::tlSmooth;
}

void Print::_make_wipe_tower()
{
    m_wipe_tower_data.clear();

    // BBS
    const unsigned int number_of_extruders = (unsigned int)(m_config.filament_colour.values.size());

    const auto bUseWipeTower2 = is_BBL_printer() || is_QIDI_printer() ? false : true;
    // Let the ToolOrdering class know there will be initial priming extrusions at the start of the print.
    m_wipe_tower_data.tool_ordering = ToolOrdering(*this, (unsigned int) -1, bUseWipeTower2 ? true : false);
    m_wipe_tower_data.tool_ordering.sort_and_build_data(*this, (unsigned int)-1, bUseWipeTower2 ? true : false);

    if (!m_wipe_tower_data.tool_ordering.has_wipe_tower())
        // Don't generate any wipe tower.
        return;

    // Check whether there are any layers in m_tool_ordering, which are marked with has_wipe_tower,
    // they print neither object, nor support. These layers are above the raft and below the object, and they
    // shall be added to the support layers to be printed.
    // see https://github.com/prusa3d/PrusaSlicer/issues/607
    {
        size_t idx_begin = size_t(-1);
        size_t idx_end   = m_wipe_tower_data.tool_ordering.layer_tools().size();
        // Find the first wipe tower layer, which does not have a counterpart in an object or a support layer.
        for (size_t i = 0; i < idx_end; ++ i) {
            const LayerTools &lt = m_wipe_tower_data.tool_ordering.layer_tools()[i];
            if (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support) {
                idx_begin = i;
                break;
            }
        }
        if (idx_begin != size_t(-1)) {
            // Find the position in m_objects.first()->support_layers to insert these new support layers.
            double wipe_tower_new_layer_print_z_first = m_wipe_tower_data.tool_ordering.layer_tools()[idx_begin].print_z;
            auto it_layer = m_objects.front()->support_layers().begin();
            auto it_end   = m_objects.front()->support_layers().end();
            for (; it_layer != it_end && (*it_layer)->print_z - EPSILON < wipe_tower_new_layer_print_z_first; ++ it_layer);
            // Find the stopper of the sequence of wipe tower layers, which do not have a counterpart in an object or a support layer.
            for (size_t i = idx_begin; i < idx_end; ++ i) {
                LayerTools &lt = const_cast<LayerTools&>(m_wipe_tower_data.tool_ordering.layer_tools()[i]);
                if (! (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support))
                    break;
                lt.has_support = true;
                // Insert the new support layer.
                double height    = lt.print_z - (i == 0 ? 0. : m_wipe_tower_data.tool_ordering.layer_tools()[i-1].print_z);
                //FIXME the support layer ID is set to -1, as Vojtech hopes it is not being used anyway.
                it_layer = m_objects.front()->insert_support_layer(it_layer, -1, 0, height, lt.print_z, lt.print_z - 0.5 * height);
                ++ it_layer;
            }
        }
    }
    this->throw_if_canceled();

    if (!bUseWipeTower2) {
        // in BBL machine, wipe tower is only use to prime extruder. So just use a global wipe volume.
        WipeTower wipe_tower(m_config, m_plate_index, m_origin, m_wipe_tower_data.tool_ordering.first_extruder(),
                             m_wipe_tower_data.tool_ordering.empty() ? 0.f : m_wipe_tower_data.tool_ordering.back().print_z, m_wipe_tower_data.tool_ordering.all_extruders());
        wipe_tower.set_has_tpu_filament(this->has_tpu_filament());
        wipe_tower.set_filament_map(this->get_filament_maps());
        // Set the extruder & material properties at the wipe tower object.
        for (size_t i = 0; i < number_of_extruders; ++i)
            wipe_tower.set_extruder(i, m_config);

        // BBS: remove priming logic
        // m_wipe_tower_data.priming = Slic3r::make_unique<std::vector<WipeTower::ToolChangeResult>>(
        //    wipe_tower.prime((float)this->skirt_first_layer_height(), m_wipe_tower_data.tool_ordering.all_extruders(), false));

        std::set<int> used_filament_ids;

        // Lets go through the wipe tower layers and determine pairs of extruder changes for each
        // to pass to wipe_tower (so that it can use it for planning the layout of the tower)
        {
        // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
        bool               is_mutli_extruder = m_config.nozzle_diameter.values.size() > 1;
        size_t nozzle_nums = m_config.nozzle_diameter.values.size();
        using FlushMatrix = std::vector<std::vector<float>>;
        std::vector<FlushMatrix> multi_extruder_flush;
        for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
            std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, nozzle_id, nozzle_nums)));
            std::vector<std::vector<float>> wipe_volumes;
            for (unsigned int i = 0; i < number_of_extruders; ++i)
                wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * number_of_extruders, flush_matrix.begin() + (i + 1) * number_of_extruders));

            multi_extruder_flush.emplace_back(wipe_volumes);
        }

        std::vector<int>filament_maps = get_filament_maps();

        std::vector<unsigned int> nozzle_cur_filament_ids(nozzle_nums, -1);
        unsigned int current_filament_id = m_wipe_tower_data.tool_ordering.first_extruder();
        size_t cur_nozzle_id = filament_maps[current_filament_id] - 1;
        nozzle_cur_filament_ids[cur_nozzle_id] = current_filament_id;

        for (auto& layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) { // for all layers
            if (!layer_tools.has_wipe_tower) continue;
            bool first_layer = &layer_tools == &m_wipe_tower_data.tool_ordering.front();
            wipe_tower.plan_toolchange((float)layer_tools.print_z, (float)layer_tools.wipe_tower_layer_height, current_filament_id, current_filament_id);

            used_filament_ids.insert(layer_tools.extruders.begin(), layer_tools.extruders.end());

            for (const auto filament_id : layer_tools.extruders) {
                if (filament_id == current_filament_id)
                    continue;

                int          nozzle_id = filament_maps[filament_id] - 1;
                unsigned int pre_filament_id = nozzle_cur_filament_ids[nozzle_id];

                float volume_to_purge = 0;
                if (pre_filament_id != (unsigned int)(-1) && pre_filament_id != filament_id) {
                    volume_to_purge = multi_extruder_flush[nozzle_id][pre_filament_id][filament_id];
                    volume_to_purge *= m_config.flush_multiplier.get_at(nozzle_id);
                    volume_to_purge = pre_filament_id == -1 ? 0 :
                        layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_filament_id, filament_id, volume_to_purge);
                }

                //During the filament change, the extruder will extrude an extra length of grab_length for the corresponding detection, so the purge can reduce this length.
                float grab_purge_volume = m_config.grab_length.get_at(nozzle_id) * 2.4; //(diameter/2)^2*PI=2.4
                volume_to_purge = std::max(0.f, volume_to_purge - grab_purge_volume);

                wipe_tower.plan_toolchange((float)layer_tools.print_z, (float)layer_tools.wipe_tower_layer_height, current_filament_id, filament_id,
                    m_config.prime_volume, volume_to_purge);
                current_filament_id = filament_id;
                nozzle_cur_filament_ids[nozzle_id] = filament_id;
            }
            layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);

            // if enable timelapse, slice all layer
            if (m_config.enable_wrapping_detection || enable_timelapse_print()) {
                if (layer_tools.wipe_tower_partitions == 0) wipe_tower.set_last_layer_extruder_fill(false);
                continue;
            }

            if (&layer_tools == &m_wipe_tower_data.tool_ordering.back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
                break;
        }
        }

        wipe_tower.set_used_filament_ids(std::vector<int>(used_filament_ids.begin(), used_filament_ids.end()));

        std::vector<int> categories;
        for (size_t i = 0; i < m_config.filament_adhesiveness_category.values.size(); ++i) {
            categories.push_back(m_config.filament_adhesiveness_category.get_at(i));
        }
        wipe_tower.set_filament_categories(categories);

        // Generate the wipe tower layers.
        m_wipe_tower_data.tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
        wipe_tower.generate_new(m_wipe_tower_data.tool_changes);
        m_wipe_tower_data.depth      = wipe_tower.get_depth();
        m_wipe_tower_data.brim_width = wipe_tower.get_brim_width();
        m_wipe_tower_data.bbx = wipe_tower.get_bbx();
        m_wipe_tower_data.rib_offset = wipe_tower.get_rib_offset();

        // Unload the current filament over the purge tower.
        coordf_t layer_height = m_objects.front()->config().layer_height.value;
        if (m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions > 0) {
            // The wipe tower goes up to the last layer of the print.
            if (wipe_tower.layer_finished()) {
                // The wipe tower is printed to the top of the print and it has no space left for the final extruder purge.
                // Lift Z to the next layer.
                wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z + layer_height), float(layer_height), 0, false,
                                     true);
            } else {
                // There is yet enough space at this layer of the wipe tower for the final purge.
            }
        } else {
            // The wipe tower does not reach the last print layer, perform the pruge at the last print layer.
            assert(m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions == 0);
            wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z), float(layer_height), 0, false, true);
        }
        m_wipe_tower_data.final_purge = Slic3r::make_unique<WipeTower::ToolChangeResult>(wipe_tower.tool_change((unsigned int) (-1)));

        m_wipe_tower_data.used_filament         = wipe_tower.get_used_filament();
        m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
        m_wipe_tower_data.construct_mesh(wipe_tower.width(), wipe_tower.get_depth(), wipe_tower.get_height(), wipe_tower.get_brim_width(), config().wipe_tower_wall_type.value == WipeTowerWallType::wtwRib,
                                         wipe_tower.get_rib_width(), wipe_tower.get_rib_length(), config().wipe_tower_fillet_wall.value);
        const Vec3d origin                      = this->get_plate_origin();
        m_fake_wipe_tower.rib_offset = wipe_tower.get_rib_offset();
        m_fake_wipe_tower.set_fake_extrusion_data(wipe_tower.position() + m_fake_wipe_tower.rib_offset, wipe_tower.width(), wipe_tower.get_height(), wipe_tower.get_layer_height(),
                                                  m_wipe_tower_data.depth,
                                                  m_wipe_tower_data.brim_width, {scale_(origin.x()), scale_(origin.y())});
        m_fake_wipe_tower.outer_wall = wipe_tower.get_outer_wall();
    } else {
        // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
        std::vector<float> flush_matrix(cast<float>(m_config.flush_volumes_matrix.values));
        // Extract purging volumes for each extruder pair:
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i<number_of_extruders; ++i)
            wipe_volumes.push_back(std::vector<float>(flush_matrix.begin()+i*number_of_extruders, flush_matrix.begin()+(i+1)*number_of_extruders));

        // Orca: itertate over wipe_volumes and change the non-zero values to the prime_volume
        if ((!m_config.purge_in_prime_tower || !m_config.single_extruder_multi_material) && !is_BBL_printer() && !is_QIDI_printer()) {
            for (unsigned int i = 0; i < number_of_extruders; ++i) {
                for (unsigned int j = 0; j < number_of_extruders; ++j) {
                    if (wipe_volumes[i][j] > 0) {
                        wipe_volumes[i][j] = m_config.prime_volume;
                    }
                }
            }
        }
        // Initialize the wipe tower.
        WipeTower2 wipe_tower(m_config, m_default_region_config, m_plate_index, m_origin, wipe_volumes,
                              m_wipe_tower_data.tool_ordering.first_extruder());

        // wipe_tower.set_retract();
        // wipe_tower.set_zhop();

        // Set the extruder & material properties at the wipe tower object.
        for (size_t i = 0; i < number_of_extruders; ++i)
            wipe_tower.set_extruder(i, m_config);

        m_wipe_tower_data.priming = Slic3r::make_unique<std::vector<WipeTower::ToolChangeResult>>(
            wipe_tower.prime((float)this->skirt_first_layer_height(), m_wipe_tower_data.tool_ordering.all_extruders(), false));

        // Lets go through the wipe tower layers and determine pairs of extruder changes for each
        // to pass to wipe_tower (so that it can use it for planning the layout of the tower)
        {
            unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.all_extruders().back();
            for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) { // for all layers
                if (!layer_tools.has_wipe_tower)
                    continue;
                bool first_layer = &layer_tools == &m_wipe_tower_data.tool_ordering.front();
                wipe_tower.plan_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height, current_extruder_id,
                                           current_extruder_id, false);
                for (const auto extruder_id : layer_tools.extruders) {
                    if ((first_layer && extruder_id == m_wipe_tower_data.tool_ordering.all_extruders().back()) || extruder_id !=
                        current_extruder_id) {
                        float volume_to_wipe = m_config.prime_volume;
                        if (m_config.purge_in_prime_tower && m_config.single_extruder_multi_material) {
                            volume_to_wipe = wipe_volumes[current_extruder_id][extruder_id]; // total volume to wipe after this toolchange
                            volume_to_wipe *= m_config.flush_multiplier.get_at(0);
                            // Not all of that can be used for infill purging:
                            volume_to_wipe -= (float) m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);

                            // try to assign some infills/objects for the wiping:
                            volume_to_wipe = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id,
                                                                                                    volume_to_wipe);

                            // add back the minimal amount toforce on the wipe tower:
                            volume_to_wipe += (float) m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);
                        }

                        // request a toolchange at the wipe tower with at least volume_to_wipe purging amount
                        wipe_tower.plan_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height,
                                                   current_extruder_id, extruder_id, volume_to_wipe);
                        current_extruder_id = extruder_id;
                    }
                }
                layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);
                if (&layer_tools == &m_wipe_tower_data.tool_ordering.back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
                    break;
            }
        }

        // Generate the wipe tower layers.
        m_wipe_tower_data.tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
        wipe_tower.generate(m_wipe_tower_data.tool_changes);
        m_wipe_tower_data.depth             = wipe_tower.get_depth();
        m_wipe_tower_data.z_and_depth_pairs = wipe_tower.get_z_and_depth_pairs();
        m_wipe_tower_data.brim_width        = wipe_tower.get_brim_width();
        m_wipe_tower_data.height            = wipe_tower.get_wipe_tower_height();
        m_wipe_tower_data.bbx               = wipe_tower.get_bbx();
        m_wipe_tower_data.rib_offset        = wipe_tower.get_rib_offset();

        // Unload the current filament over the purge tower.
        coordf_t layer_height = m_objects.front()->config().layer_height.value;
        if (m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions > 0) {
            // The wipe tower goes up to the last layer of the print.
            if (wipe_tower.layer_finished()) {
                // The wipe tower is printed to the top of the print and it has no space left for the final extruder purge.
                // Lift Z to the next layer.
                wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z + layer_height), float(layer_height), 0, false,
                                     true);
            } else {
                // There is yet enough space at this layer of the wipe tower for the final purge.
            }
        } else {
            // The wipe tower does not reach the last print layer, perform the pruge at the last print layer.
            assert(m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions == 0);
            wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z), float(layer_height), 0, false, true);
        }
        m_wipe_tower_data.final_purge = Slic3r::make_unique<WipeTower::ToolChangeResult>(wipe_tower.tool_change((unsigned int) (-1)));

        m_wipe_tower_data.used_filament         = wipe_tower.get_used_filament();
        m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
        const Vec3d origin                      = Vec3d::Zero();
        m_fake_wipe_tower.set_fake_extrusion_data(wipe_tower.position(), wipe_tower.width(), wipe_tower.get_wipe_tower_height(),
                                                  config().initial_layer_print_height, m_wipe_tower_data.depth,
                                                  m_wipe_tower_data.z_and_depth_pairs, m_wipe_tower_data.brim_width,
                                                  config().wipe_tower_rotation_angle, config().wipe_tower_cone_angle,
                                                  {scale_(origin.x()), scale_(origin.y())});
    }
}

// Generate a recommended G-code output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters and print statistics.
// Use the final print statistics if available, or just keep the print statistics placeholders if not available yet (before G-code is finalized).
std::string Print::output_filename(const std::string &filename_base) const
{
    // Set the placeholders for the data know first after the G-code export is finished.
    // These values will be just propagated into the output file name.
    DynamicConfig config = this->finished() ? this->print_statistics().config() : this->print_statistics().placeholders();
    config.set_key_value("num_filaments", new ConfigOptionInt((int)m_config.nozzle_diameter.size()));
    config.set_key_value("num_extruders", new ConfigOptionInt((int) m_config.nozzle_diameter.size()));
    config.set_key_value("plate_name", new ConfigOptionString(get_plate_name()));
    config.set_key_value("plate_number", new ConfigOptionString(get_plate_number_formatted()));
    config.set_key_value("model_name", new ConfigOptionString(get_model_name()));

    return this->PrintBase::output_filename(m_config.filename_format.value, ".gcode", filename_base, &config);
}

std::string Print::get_model_name() const
{
    if (model().model_info != nullptr)
    {
        return model().model_info->model_name;
    } else {
        return "";
    }
}

std::string Print::get_plate_number_formatted() const
{
    std::string plate_number = std::to_string(get_plate_index() + 1);
    static const size_t n_zero = 2;

    return std::string(n_zero - std::min(n_zero, plate_number.length()), '0') + plate_number;
}

//BBS: add gcode file preload logic
void Print::set_gcode_file_ready()
{
    this->set_started(psGCodeExport);
	this->set_done(psGCodeExport);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<  boost::format(": done");
}
//BBS: add gcode file preload logic
void Print::set_gcode_file_invalidated()
{
    this->invalidate_step(psGCodeExport);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<  boost::format(": done");
}

//BBS: add gcode file preload logic
void Print::export_gcode_from_previous_file(const std::string& file, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    try {
        GCodeProcessor processor;
        GCodeProcessor::s_IsBBLPrinter = is_BBL_printer();
        const Vec3d origin = this->get_plate_origin();
        processor.set_xy_offset(origin(0), origin(1));
        //processor.enable_producers(true);
        processor.process_file(file);

        *result = std::move(processor.extract_result());
    } catch (std::exception & /* ex */) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ <<  boost::format(": found errors when process gcode file %1%") %file.c_str();
        throw Slic3r::RuntimeError(
            std::string("Failed to process the G-code file ") + file + " from previous 3mf\n");
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<  boost::format(":  process the G-code file %1% successfully")%file.c_str();
}

std::tuple<float, float> Print::object_skirt_offset(double margin_height) const
{
    if (config().skirt_loops == 0 || config().skirt_type != stPerObject)
        return std::make_tuple(0, 0);
    
    float max_nozzle_diameter = *std::max_element(m_config.nozzle_diameter.values.begin(), m_config.nozzle_diameter.values.end());
    float max_layer_height    = *std::max_element(config().max_layer_height.values.begin(), config().max_layer_height.values.end());
    float line_width = m_config.initial_layer_line_width.get_abs_value(max_nozzle_diameter);
    float object_skirt_witdh  = skirt_flow().width() + (config().skirt_loops - 1) * skirt_flow().spacing();
    float object_skirt_offset = 0;

    if (is_all_objects_are_short())
        object_skirt_offset = config().skirt_distance + object_skirt_witdh;
    else if (config().draft_shield == dsEnabled || config().skirt_height * max_layer_height > config().nozzle_height - margin_height)
        object_skirt_offset = config().skirt_distance + line_width;
    else if (config().skirt_distance + object_skirt_witdh > config().extruder_clearance_radius/2)
        object_skirt_offset = (config().skirt_distance + object_skirt_witdh - config().extruder_clearance_radius/2);
    else
        return std::make_tuple(0, 0);

    return std::make_tuple(object_skirt_offset, object_skirt_witdh);
}

DynamicConfig PrintStatistics::config() const
{
    DynamicConfig config;
    std::string normal_print_time = short_time(this->estimated_normal_print_time);
    std::string silent_print_time = short_time(this->estimated_silent_print_time);
    config.set_key_value("print_time", new ConfigOptionString(normal_print_time));
    config.set_key_value("normal_print_time", new ConfigOptionString(normal_print_time));
    config.set_key_value("silent_print_time", new ConfigOptionString(silent_print_time));
    config.set_key_value("used_filament",             new ConfigOptionFloat(this->total_used_filament / 1000.));
    config.set_key_value("extruded_volume",           new ConfigOptionFloat(this->total_extruded_volume));
    config.set_key_value("total_cost",                new ConfigOptionFloat(this->total_cost));
    config.set_key_value("total_toolchanges",         new ConfigOptionInt(this->total_toolchanges));
    config.set_key_value("total_weight",              new ConfigOptionFloat(this->total_weight));
    config.set_key_value("total_wipe_tower_cost",     new ConfigOptionFloat(this->total_wipe_tower_cost));
    config.set_key_value("total_wipe_tower_filament", new ConfigOptionFloat(this->total_wipe_tower_filament));
    config.set_key_value("initial_tool",              new ConfigOptionInt(static_cast<int>(this->initial_tool)));
    return config;
}

DynamicConfig PrintStatistics::placeholders()
{
    DynamicConfig config;
    for (const std::string key : {
        "print_time", "normal_print_time", "silent_print_time",
        "used_filament", "extruded_volume", "total_cost", "total_weight",
        "initial_tool", "total_toolchanges", "total_wipe_tower_cost", "total_wipe_tower_filament"})
        config.set_key_value(key, new ConfigOptionString(std::string("{") + key + "}"));
    return config;
}

std::string PrintStatistics::finalize_output_path(const std::string &path_in) const
{
    std::string final_path;
    try {
        boost::filesystem::path path(path_in);
        DynamicConfig cfg = this->config();
        PlaceholderParser pp;
        std::string new_stem = pp.process(path.stem().string(), 0, &cfg);
        final_path = (path.parent_path() / (new_stem + path.extension().string())).string();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to apply the print statistics to the export file name: " << ex.what();
        final_path = path_in;
    }
    return final_path;
}

// Orca: Implement prusa's filament shrink compensation approach
// Returns if all used filaments have same shrinkage compensations.
 bool Print::has_same_shrinkage_compensations() const {
     const std::vector<unsigned int> extruders = this->extruders();
     if (extruders.empty())
         return false;

     const double filament_shrinkage_compensation_xy = m_config.filament_shrink.get_at(extruders.front());
     const double filament_shrinkage_compensation_z  = m_config.filament_shrinkage_compensation_z.get_at(extruders.front());

     for (unsigned int extruder : extruders) {
         if (filament_shrinkage_compensation_xy != m_config.filament_shrink.get_at(extruder) ||
             filament_shrinkage_compensation_z  != m_config.filament_shrinkage_compensation_z.get_at(extruder)) {
             return false;
         }
     }

     return true;
 }

// Orca: Implement prusa's filament shrink compensation approach, but amended so 100% from the user is the equivalent to 0 in orca.
 // Returns scaling for each axis representing shrinkage compensations in each axis.
Vec3d Print::shrinkage_compensation() const
{
    if (!this->has_same_shrinkage_compensations())
        return Vec3d::Ones();

    const unsigned int first_extruder = this->extruders().front();

    const double xy_shrinkage_percent = m_config.filament_shrink.get_at(first_extruder);
    const double z_shrinkage_percent  = m_config.filament_shrinkage_compensation_z.get_at(first_extruder);

    const double xy_compensation = 100.0 / xy_shrinkage_percent;
    const double z_compensation  = 100.0 / z_shrinkage_percent;

    return { xy_compensation, xy_compensation, z_compensation };
}

const std::string PrintStatistics::FilamentUsedG     = "filament used [g]";
const std::string PrintStatistics::FilamentUsedGMask = "; filament used [g] =";

const std::string PrintStatistics::TotalFilamentUsedG          = "total filament used [g]";
const std::string PrintStatistics::TotalFilamentUsedGMask      = "; total filament used [g] =";
const std::string PrintStatistics::TotalFilamentUsedGValueMask = "; total filament used [g] = %.2lf\n";

const std::string PrintStatistics::FilamentUsedCm3     = "filament used [cm3]";
const std::string PrintStatistics::FilamentUsedCm3Mask = "; filament used [cm3] =";

const std::string PrintStatistics::FilamentUsedMm     = "filament used [mm]";
const std::string PrintStatistics::FilamentUsedMmMask = "; filament used [mm] =";

const std::string PrintStatistics::FilamentCost     = "filament cost";
const std::string PrintStatistics::FilamentCostMask = "; filament cost =";

const std::string PrintStatistics::TotalFilamentCost          = "total filament cost";
const std::string PrintStatistics::TotalFilamentCostMask      = "; total filament cost =";
const std::string PrintStatistics::TotalFilamentCostValueMask = "; total filament cost = %.2lf\n";

const std::string PrintStatistics::TotalFilamentUsedWipeTower     = "total filament used for wipe tower [g]";
const std::string PrintStatistics::TotalFilamentUsedWipeTowerValueMask = "; total filament used for wipe tower [g] = %.2lf\n";


/*add json export/import related functions */
#define JSON_POLYGON_CONTOUR                "contour"
#define JSON_POLYGON_HOLES                  "holes"
#define JSON_POINTS                 "points"
#define JSON_EXPOLYGON              "expolygon"
#define JSON_ARC_FITTING            "arc_fitting"
#define JSON_OBJECT_NAME            "name"
#define JSON_IDENTIFY_ID          "identify_id"


#define JSON_LAYERS                  "layers"
#define JSON_SUPPORT_LAYERS                  "support_layers"
#define JSON_TREE_SUPPORT_LAYERS                  "tree_support_layers"
#define JSON_LAYER_REGIONS                  "layer_regions"
#define JSON_FIRSTLAYER_GROUPS                  "first_layer_groups"

#define JSON_FIRSTLAYER_GROUP_ID                  "group_id"
#define JSON_FIRSTLAYER_GROUP_VOLUME_IDS          "volume_ids"
#define JSON_FIRSTLAYER_GROUP_SLICES               "slices"

#define JSON_LAYER_PRINT_Z            "print_z"
#define JSON_LAYER_SLICE_Z            "slice_z"
#define JSON_LAYER_HEIGHT             "height"
#define JSON_LAYER_ID                  "layer_id"
#define JSON_LAYER_SLICED_POLYGONS    "sliced_polygons"
#define JSON_LAYER_SLLICED_BBOXES      "sliced_bboxes"
#define JSON_LAYER_OVERHANG_POLYGONS    "overhang_polygons"
#define JSON_LAYER_OVERHANG_BBOX       "overhang_bbox"

#define JSON_SUPPORT_LAYER_ISLANDS                  "support_islands"
#define JSON_SUPPORT_LAYER_FILLS                    "support_fills"
#define JSON_SUPPORT_LAYER_INTERFACE_ID             "interface_id"
#define JSON_SUPPORT_LAYER_TYPE                     "support_type"

#define JSON_LAYER_REGION_CONFIG_HASH             "config_hash"
#define JSON_LAYER_REGION_SLICES                  "slices"
#define JSON_LAYER_REGION_RAW_SLICES              "raw_slices"
//#define JSON_LAYER_REGION_ENTITIES                "entities"
#define JSON_LAYER_REGION_THIN_FILLS                  "thin_fills"
#define JSON_LAYER_REGION_FILL_EXPOLYGONS             "fill_expolygons"
#define JSON_LAYER_REGION_FILL_SURFACES               "fill_surfaces"
#define JSON_LAYER_REGION_FILL_NO_OVERLAP             "fill_no_overlap_expolygons"
#define JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES    "unsupported_bridge_edges"
#define JSON_LAYER_REGION_PERIMETERS                  "perimeters"
#define JSON_LAYER_REGION_FILLS                  "fills"



#define JSON_SURF_TYPE              "surface_type"
#define JSON_SURF_THICKNESS         "thickness"
#define JSON_SURF_THICKNESS_LAYER   "thickness_layers"
#define JSON_SURF_BRIDGE_ANGLE       "bridge_angle"
#define JSON_SURF_EXTRA_PERIMETERS   "extra_perimeters"

#define JSON_ARC_DATA                "arc_data"
#define JSON_ARC_START_INDEX         "start_index"
#define JSON_ARC_END_INDEX           "end_index"
#define JSON_ARC_PATH_TYPE           "path_type"

#define JSON_IS_ARC                  "is_arc"
#define JSON_ARC_LENGTH              "length"
#define JSON_ARC_ANGLE_RADIUS        "angle_radians"
#define JSON_ARC_POLAY_START_THETA   "polar_start_theta"
#define JSON_ARC_POLAY_END_THETA     "polar_end_theta"
#define JSON_ARC_START_POINT          "start_point"
#define JSON_ARC_END_POINT            "end_point"
#define JSON_ARC_DIRECTION            "direction"
#define JSON_ARC_RADIUS               "radius"
#define JSON_ARC_CENTER               "center"

//extrusions
#define JSON_EXTRUSION_ENTITY_TYPE             "entity_type"
#define JSON_EXTRUSION_NO_SORT                 "no_sort"
#define JSON_EXTRUSION_PATHS                   "paths"
#define JSON_EXTRUSION_ENTITIES                "entities"
#define JSON_EXTRUSION_TYPE_PATH               "path"
#define JSON_EXTRUSION_TYPE_MULTIPATH          "multipath"
#define JSON_EXTRUSION_TYPE_LOOP               "loop"
#define JSON_EXTRUSION_TYPE_COLLECTION         "collection"
#define JSON_EXTRUSION_POLYLINE                "polyline"
#define JSON_EXTRUSION_MM3_PER_MM              "mm3_per_mm"
#define JSON_EXTRUSION_WIDTH                   "width"
#define JSON_EXTRUSION_HEIGHT                  "height"
#define JSON_EXTRUSION_ROLE                    "role"
#define JSON_EXTRUSION_NO_EXTRUSION            "no_extrusion"
#define JSON_EXTRUSION_LOOP_ROLE               "loop_role"


static void to_json(json& j, const Points& p_s) {
    for (const Point& p : p_s)
    {
        j.push_back(p.x());
        j.push_back(p.y());
    }
}

static void to_json(json& j, const BoundingBox& bb) {
    j.push_back(bb.min.x());
    j.push_back(bb.min.y());
    j.push_back(bb.max.x());
    j.push_back(bb.max.y());
}

static void to_json(json& j, const ExPolygon& polygon) {
    json contour_json = json::array(), holes_json = json::array();

    //contour
    const Polygon& slice_contour =   polygon.contour;
    contour_json = slice_contour.points;
    j[JSON_POLYGON_CONTOUR] = std::move(contour_json);

    //holes
    const Polygons& slice_holes =   polygon.holes;
    for (const Polygon& hole_polyon : slice_holes)
    {
        json hole_json = json::array();
        hole_json =  hole_polyon.points;
        holes_json.push_back(std::move(hole_json));
    }
    j[JSON_POLYGON_HOLES] = std::move(holes_json);
}

static void to_json(json& j, const Surface& surf) {
    j[JSON_EXPOLYGON] = surf.expolygon;
    j[JSON_SURF_TYPE] = surf.surface_type;
    j[JSON_SURF_THICKNESS] = surf.thickness;
    j[JSON_SURF_THICKNESS_LAYER] = surf.thickness_layers;
    j[JSON_SURF_BRIDGE_ANGLE] = surf.bridge_angle;
    j[JSON_SURF_EXTRA_PERIMETERS] = surf.extra_perimeters;
}

static void to_json(json& j, const ArcSegment& arc_seg) {
    json start_point_json = json::array(), end_point_json = json::array(), center_point_json = json::array();
    j[JSON_IS_ARC] = arc_seg.is_arc;
    j[JSON_ARC_LENGTH] = arc_seg.length;
    j[JSON_ARC_ANGLE_RADIUS] = arc_seg.angle_radians;
    j[JSON_ARC_POLAY_START_THETA] = arc_seg.polar_start_theta;
    j[JSON_ARC_POLAY_END_THETA] = arc_seg.polar_end_theta;
    start_point_json.push_back(arc_seg.start_point.x());
    start_point_json.push_back(arc_seg.start_point.y());
    j[JSON_ARC_START_POINT] = std::move(start_point_json);
    end_point_json.push_back(arc_seg.end_point.x());
    end_point_json.push_back(arc_seg.end_point.y());
    j[JSON_ARC_END_POINT] = std::move(end_point_json);
    j[JSON_ARC_DIRECTION] = arc_seg.direction;
    j[JSON_ARC_RADIUS] = arc_seg.radius;
    center_point_json.push_back(arc_seg.center.x());
    center_point_json.push_back(arc_seg.center.y());
    j[JSON_ARC_CENTER] = std::move(center_point_json);
}


static void to_json(json& j, const Polyline& poly_line) {
    json points_json = json::array(), fittings_json = json::array();
    points_json = poly_line.points;

    j[JSON_POINTS] = std::move(points_json);
    for (const PathFittingData& path_fitting : poly_line.fitting_result)
    {
        json fitting_json;
        fitting_json[JSON_ARC_START_INDEX] = path_fitting.start_point_index;
        fitting_json[JSON_ARC_END_INDEX] = path_fitting.end_point_index;
        fitting_json[JSON_ARC_PATH_TYPE] = path_fitting.path_type;
        if (path_fitting.arc_data.is_arc)
            fitting_json[JSON_ARC_DATA] = path_fitting.arc_data;

        fittings_json.push_back(std::move(fitting_json));
    }
    j[JSON_ARC_FITTING] = fittings_json;
}

static void to_json(json& j, const ExtrusionPath& extrusion_path) {
    j[JSON_EXTRUSION_POLYLINE] = extrusion_path.polyline;
    j[JSON_EXTRUSION_MM3_PER_MM] = extrusion_path.mm3_per_mm;
    j[JSON_EXTRUSION_WIDTH] = extrusion_path.width;
    j[JSON_EXTRUSION_HEIGHT] = extrusion_path.height;
    j[JSON_EXTRUSION_ROLE] = extrusion_path.role();
    j[JSON_EXTRUSION_NO_EXTRUSION] = extrusion_path.is_force_no_extrusion();
}

static bool convert_extrusion_to_json(json& entity_json, json& entity_paths_json, const ExtrusionEntity* extrusion_entity) {
    std::string path_type;
    const ExtrusionPath* path = NULL;
    const ExtrusionMultiPath* multipath = NULL;
    const ExtrusionLoop* loop = NULL;
    const ExtrusionEntityCollection* collection = dynamic_cast<const ExtrusionEntityCollection*>(extrusion_entity);

    if (!collection)
        path = dynamic_cast<const ExtrusionPath*>(extrusion_entity);

    if (!collection && !path)
        multipath = dynamic_cast<const ExtrusionMultiPath*>(extrusion_entity);

    if (!collection && !path && !multipath)
        loop = dynamic_cast<const ExtrusionLoop*>(extrusion_entity);

    path_type = path?JSON_EXTRUSION_TYPE_PATH:(multipath?JSON_EXTRUSION_TYPE_MULTIPATH:(loop?JSON_EXTRUSION_TYPE_LOOP:JSON_EXTRUSION_TYPE_COLLECTION));
    if (path_type.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":invalid extrusion path type Found");
        return false;
    }

    entity_json[JSON_EXTRUSION_ENTITY_TYPE] = path_type;

    if (path) {
        json entity_path_json = *path;
        entity_paths_json.push_back(std::move(entity_path_json));
    }
    else if (multipath) {
        for (const ExtrusionPath& extrusion_path : multipath->paths)
        {
            json entity_path_json = extrusion_path;
            entity_paths_json.push_back(std::move(entity_path_json));
        }
    }
    else if (loop) {
        entity_json[JSON_EXTRUSION_LOOP_ROLE] = loop->loop_role();
        for (const ExtrusionPath& extrusion_path : loop->paths)
        {
            json entity_path_json = extrusion_path;
            entity_paths_json.push_back(std::move(entity_path_json));
        }
    }
    else {
        //recursive collections
        entity_json[JSON_EXTRUSION_NO_SORT] = collection->no_sort;
        for (const ExtrusionEntity* recursive_extrusion_entity : collection->entities) {
            json recursive_entity_json, recursive_entity_paths_json = json::array();
            bool ret = convert_extrusion_to_json(recursive_entity_json, recursive_entity_paths_json, recursive_extrusion_entity);
            if (!ret) {
                continue;
            }
            entity_paths_json.push_back(std::move(recursive_entity_json));
        }
    }

    if (collection)
        entity_json[JSON_EXTRUSION_ENTITIES] = std::move(entity_paths_json);
    else
        entity_json[JSON_EXTRUSION_PATHS] = std::move(entity_paths_json);
    return true;
}

static void to_json(json& j, const LayerRegion& layer_region) {
    json unsupported_bridge_edges_json = json::array(), slices_surfaces_json = json::array(), raw_slices_json = json::array(), thin_fills_json, thin_fill_entities_json = json::array();
    json fill_expolygons_json = json::array(), fill_no_overlap_expolygons_json = json::array(), fill_surfaces_json = json::array(), perimeters_json, perimeter_entities_json = json::array(), fills_json, fill_entities_json = json::array();

    j[JSON_LAYER_REGION_CONFIG_HASH] = layer_region.region().config_hash();
    //slices
    for (const Surface& slice_surface : layer_region.slices.surfaces) {
        json surface_json = slice_surface;
        slices_surfaces_json.push_back(std::move(surface_json));
    }
    j.push_back({JSON_LAYER_REGION_SLICES, std::move(slices_surfaces_json)});

    //raw_slices
    for (const ExPolygon& raw_slice_explogyon : layer_region.raw_slices) {
        json raw_polygon_json = raw_slice_explogyon;

        raw_slices_json.push_back(std::move(raw_polygon_json));
    }
    j.push_back({JSON_LAYER_REGION_RAW_SLICES, std::move(raw_slices_json)});

    //thin fills
    thin_fills_json[JSON_EXTRUSION_NO_SORT] = layer_region.thin_fills.no_sort;
    thin_fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
    for (const ExtrusionEntity* extrusion_entity : layer_region.thin_fills.entities) {
        json thinfills_entity_json, thinfill_entity_paths_json = json::array();
        bool ret = convert_extrusion_to_json(thinfills_entity_json, thinfill_entity_paths_json, extrusion_entity);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":error found at print_z %1%") % layer_region.layer()->print_z;
            continue;
        }

        thin_fill_entities_json.push_back(std::move(thinfills_entity_json));
    }
    thin_fills_json[JSON_EXTRUSION_ENTITIES] = std::move(thin_fill_entities_json);
    j.push_back({JSON_LAYER_REGION_THIN_FILLS, std::move(thin_fills_json)});

    //fill_expolygons
    for (const ExPolygon& fill_expolygon : layer_region.fill_expolygons) {
        json fill_expolygon_json = fill_expolygon;

        fill_expolygons_json.push_back(std::move(fill_expolygon_json));
    }
    j.push_back({JSON_LAYER_REGION_FILL_EXPOLYGONS, std::move(fill_expolygons_json)});

    //fill_surfaces
    for (const Surface& fill_surface : layer_region.fill_surfaces.surfaces) {
        json surface_json = fill_surface;
        fill_surfaces_json.push_back(std::move(surface_json));
    }
    j.push_back({JSON_LAYER_REGION_FILL_SURFACES, std::move(fill_surfaces_json)});

    //fill_no_overlap_expolygons
    for (const ExPolygon& fill_no_overlap_expolygon : layer_region.fill_no_overlap_expolygons) {
        json fill_no_overlap_expolygon_json = fill_no_overlap_expolygon;

        fill_no_overlap_expolygons_json.push_back(std::move(fill_no_overlap_expolygon_json));
    }
    j.push_back({JSON_LAYER_REGION_FILL_NO_OVERLAP, std::move(fill_no_overlap_expolygons_json)});

    //unsupported_bridge_edges
    for (const Polyline& poly_line : layer_region.unsupported_bridge_edges)
    {
        json polyline_json = poly_line;

        unsupported_bridge_edges_json.push_back(std::move(polyline_json));
    }
    j.push_back({JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES, std::move(unsupported_bridge_edges_json)});

    //perimeters
    perimeters_json[JSON_EXTRUSION_NO_SORT] = layer_region.perimeters.no_sort;
    perimeters_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
    for (const ExtrusionEntity* extrusion_entity : layer_region.perimeters.entities) {
        json perimeters_entity_json, perimeters_entity_paths_json = json::array();
        bool ret = convert_extrusion_to_json(perimeters_entity_json, perimeters_entity_paths_json, extrusion_entity);
        if (!ret)
            continue;

        perimeter_entities_json.push_back(std::move(perimeters_entity_json));
    }
    perimeters_json[JSON_EXTRUSION_ENTITIES] = std::move(perimeter_entities_json);
    j.push_back({JSON_LAYER_REGION_PERIMETERS, std::move(perimeters_json)});

    //fills
    fills_json[JSON_EXTRUSION_NO_SORT] = layer_region.fills.no_sort;
    fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
    for (const ExtrusionEntity* extrusion_entity : layer_region.fills.entities) {
        json fill_entity_json, fill_entity_paths_json = json::array();
        bool ret = convert_extrusion_to_json(fill_entity_json, fill_entity_paths_json, extrusion_entity);
        if (!ret)
            continue;

        fill_entities_json.push_back(std::move(fill_entity_json));
    }
    fills_json[JSON_EXTRUSION_ENTITIES] = std::move(fill_entities_json);
    j.push_back({JSON_LAYER_REGION_FILLS, std::move(fills_json)});

    return;
}

static void to_json(json& j, const groupedVolumeSlices& first_layer_group) {
    json volumes_json = json::array(), slices_json = json::array();
    j[JSON_FIRSTLAYER_GROUP_ID] = first_layer_group.groupId;

    for (const ObjectID& obj_id : first_layer_group.volume_ids)
    {
        volumes_json.push_back(obj_id.id);
    }
    j[JSON_FIRSTLAYER_GROUP_VOLUME_IDS] = std::move(volumes_json);

    for (const ExPolygon& slice_expolygon : first_layer_group.slices) {
        json slice_expolygon_json = slice_expolygon;

        slices_json.push_back(std::move(slice_expolygon_json));
    }
    j[JSON_FIRSTLAYER_GROUP_SLICES] = std::move(slices_json);
}

//load apis from json
static void from_json(const json& j, Points& p_s) {
    int array_size = j.size();
    for (int index = 0; index < array_size/2; index++)
    {
        coord_t x = j[2*index], y = j[2*index+1];
        Point p(x, y);
        p_s.push_back(std::move(p));
    }
    return;
}

static void from_json(const json& j, BoundingBox& bbox) {
    bbox.min[0] = j[0];
    bbox.min[1] = j[1];
    bbox.max[0] = j[2];
    bbox.max[1] = j[3];
    bbox.defined = true;

    return;
}

static void from_json(const json& j, ExPolygon& polygon) {
    polygon.contour.points = j[JSON_POLYGON_CONTOUR];

    int holes_count = j[JSON_POLYGON_HOLES].size();
    for (int holes_index = 0; holes_index < holes_count; holes_index++)
    {
        Polygon poly;

        poly.points = j[JSON_POLYGON_HOLES][holes_index];
        polygon.holes.push_back(std::move(poly));
    }
    return;
}

static void from_json(const json& j, Surface& surf) {
    surf.expolygon = j[JSON_EXPOLYGON];
    surf.surface_type = j[JSON_SURF_TYPE];
    surf.thickness = j[JSON_SURF_THICKNESS];
    surf.thickness_layers = j[JSON_SURF_THICKNESS_LAYER];
    surf.bridge_angle = j[JSON_SURF_BRIDGE_ANGLE];
    surf.extra_perimeters = j[JSON_SURF_EXTRA_PERIMETERS];

    return;
}

static void from_json(const json& j, ArcSegment& arc_seg) {
    arc_seg.is_arc = j[JSON_IS_ARC];
    arc_seg.length = j[JSON_ARC_LENGTH];
    arc_seg.angle_radians = j[JSON_ARC_ANGLE_RADIUS];
    arc_seg.polar_start_theta = j[JSON_ARC_POLAY_START_THETA];
    arc_seg.polar_end_theta = j[JSON_ARC_POLAY_END_THETA];
    arc_seg.start_point.x() = j[JSON_ARC_START_POINT][0];
    arc_seg.start_point.y() = j[JSON_ARC_START_POINT][1];
    arc_seg.end_point.x() = j[JSON_ARC_END_POINT][0];
    arc_seg.end_point.y() = j[JSON_ARC_END_POINT][1];
    arc_seg.direction = j[JSON_ARC_DIRECTION];
    arc_seg.radius    = j[JSON_ARC_RADIUS];
    arc_seg.center.x() = j[JSON_ARC_CENTER][0];
    arc_seg.center.y() = j[JSON_ARC_CENTER][1];

    return;
}


static void from_json(const json& j, Polyline& poly_line) {
    poly_line.points = j[JSON_POINTS];

    int arc_fitting_count = j[JSON_ARC_FITTING].size();
    for (int arc_fitting_index = 0; arc_fitting_index < arc_fitting_count; arc_fitting_index++)
    {
        const json& fitting_json = j[JSON_ARC_FITTING][arc_fitting_index];
        PathFittingData path_fitting;
        path_fitting.start_point_index = fitting_json[JSON_ARC_START_INDEX];
        path_fitting.end_point_index = fitting_json[JSON_ARC_END_INDEX];
        path_fitting.path_type = fitting_json[JSON_ARC_PATH_TYPE];

        if (fitting_json.contains(JSON_ARC_DATA)) {
            path_fitting.arc_data = fitting_json[JSON_ARC_DATA];
        }

        poly_line.fitting_result.push_back(std::move(path_fitting));
    }
    return;
}

static void from_json(const json& j, ExtrusionPath& extrusion_path) {
    extrusion_path.polyline               =    j[JSON_EXTRUSION_POLYLINE];
    extrusion_path.mm3_per_mm             =    j[JSON_EXTRUSION_MM3_PER_MM];
    extrusion_path.width                  =    j[JSON_EXTRUSION_WIDTH];
    extrusion_path.height                 =    j[JSON_EXTRUSION_HEIGHT];
    extrusion_path.set_extrusion_role(j[JSON_EXTRUSION_ROLE]);
    extrusion_path.set_force_no_extrusion(j[JSON_EXTRUSION_NO_EXTRUSION]);
}

static bool convert_extrusion_from_json(const json& entity_json, ExtrusionEntityCollection& entity_collection) {
    std::string path_type = entity_json[JSON_EXTRUSION_ENTITY_TYPE];
    bool ret = false;

    if (path_type == JSON_EXTRUSION_TYPE_PATH) {
        ExtrusionPath* path = new ExtrusionPath();
        if (!path) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionPath");
            return false;
        }
        *path = entity_json[JSON_EXTRUSION_PATHS][0];
        entity_collection.entities.push_back(path);
    }
    else if (path_type == JSON_EXTRUSION_TYPE_MULTIPATH) {
        ExtrusionMultiPath* multipath = new ExtrusionMultiPath();
        if (!multipath) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionMultiPath");
            return false;
        }
        int paths_count = entity_json[JSON_EXTRUSION_PATHS].size();
        for (int path_index = 0; path_index < paths_count; path_index++)
        {
            ExtrusionPath path;
            path = entity_json[JSON_EXTRUSION_PATHS][path_index];
            multipath->paths.push_back(std::move(path));
        }
        entity_collection.entities.push_back(multipath);
    }
    else if (path_type == JSON_EXTRUSION_TYPE_LOOP) {
        ExtrusionLoop* loop = new ExtrusionLoop();
        if (!loop) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionLoop");
            return false;
        }
        loop->set_loop_role(entity_json[JSON_EXTRUSION_LOOP_ROLE]);
        int paths_count = entity_json[JSON_EXTRUSION_PATHS].size();
        for (int path_index = 0; path_index < paths_count; path_index++)
        {
            ExtrusionPath path;
            path = entity_json[JSON_EXTRUSION_PATHS][path_index];
            loop->paths.push_back(std::move(path));
        }
        entity_collection.entities.push_back(loop);
    }
    else if (path_type == JSON_EXTRUSION_TYPE_COLLECTION) {
        ExtrusionEntityCollection* collection = new ExtrusionEntityCollection();
        if (!collection) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionEntityCollection");
            return false;
        }
        collection->no_sort = entity_json[JSON_EXTRUSION_NO_SORT];
        int entities_count = entity_json[JSON_EXTRUSION_ENTITIES].size();
        for (int entity_index = 0; entity_index < entities_count; entity_index++)
        {
            const json& entity_item_json = entity_json[JSON_EXTRUSION_ENTITIES][entity_index];
            ret = convert_extrusion_from_json(entity_item_json, *collection);
            if (!ret) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": convert_extrusion_from_json failed");
                return false;
            }
        }
        entity_collection.entities.push_back(collection);
    }
    else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": unknown path type %1%")%path_type;
        return false;
    }

    return true;
}

static void convert_layer_region_from_json(const json& j, LayerRegion& layer_region) {
    //slices
    int slices_count = j[JSON_LAYER_REGION_SLICES].size();
    for (int slices_index = 0; slices_index < slices_count; slices_index++)
    {
        Surface surface;

        surface = j[JSON_LAYER_REGION_SLICES][slices_index];
        layer_region.slices.surfaces.push_back(std::move(surface));
    }

    //raw_slices
    int raw_slices_count = j[JSON_LAYER_REGION_RAW_SLICES].size();
    for (int raw_slices_index = 0; raw_slices_index < raw_slices_count; raw_slices_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_LAYER_REGION_RAW_SLICES][raw_slices_index];
        layer_region.raw_slices.push_back(std::move(polygon));
    }

    //thin fills
    layer_region.thin_fills.no_sort = j[JSON_LAYER_REGION_THIN_FILLS][JSON_EXTRUSION_NO_SORT];
    int thinfills_entities_count = j[JSON_LAYER_REGION_THIN_FILLS][JSON_EXTRUSION_ENTITIES].size();
    for (int thinfills_entities_index = 0; thinfills_entities_index < thinfills_entities_count; thinfills_entities_index++)
    {
        const json& extrusion_entity_json =  j[JSON_LAYER_REGION_THIN_FILLS][JSON_EXTRUSION_ENTITIES][thinfills_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, layer_region.thin_fills);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":error parsing thin_fills found at layer %1%, print_z %2%") %layer_region.layer()->id() %layer_region.layer()->print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing thin_fills at layer %zu, print_z %f", layer_region.layer()->id(), layer_region.layer()->print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    //fill_expolygons
    int fill_expolygons_count = j[JSON_LAYER_REGION_FILL_EXPOLYGONS].size();
    for (int fill_expolygons_index = 0; fill_expolygons_index < fill_expolygons_count; fill_expolygons_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_LAYER_REGION_FILL_EXPOLYGONS][fill_expolygons_index];
        layer_region.fill_expolygons.push_back(std::move(polygon));
    }

    //fill_surfaces
    int fill_surfaces_count = j[JSON_LAYER_REGION_FILL_SURFACES].size();
    for (int fill_surfaces_index = 0; fill_surfaces_index < fill_surfaces_count; fill_surfaces_index++)
    {
        Surface surface;

        surface = j[JSON_LAYER_REGION_FILL_SURFACES][fill_surfaces_index];
        layer_region.fill_surfaces.surfaces.push_back(std::move(surface));
    }

    //fill_no_overlap_expolygons
    int fill_no_overlap_expolygons_count = j[JSON_LAYER_REGION_FILL_NO_OVERLAP].size();
    for (int fill_no_overlap_expolygons_index = 0; fill_no_overlap_expolygons_index < fill_no_overlap_expolygons_count; fill_no_overlap_expolygons_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_LAYER_REGION_FILL_NO_OVERLAP][fill_no_overlap_expolygons_index];
        layer_region.fill_no_overlap_expolygons.push_back(std::move(polygon));
    }

    //unsupported_bridge_edges
    int unsupported_bridge_edges_count = j[JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES].size();
    for (int unsupported_bridge_edges_index = 0; unsupported_bridge_edges_index < unsupported_bridge_edges_count; unsupported_bridge_edges_index++)
    {
        Polyline polyline;

        polyline = j[JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES][unsupported_bridge_edges_index];
        layer_region.unsupported_bridge_edges.push_back(std::move(polyline));
    }

    //perimeters
    layer_region.perimeters.no_sort = j[JSON_LAYER_REGION_PERIMETERS][JSON_EXTRUSION_NO_SORT];
    int perimeters_entities_count = j[JSON_LAYER_REGION_PERIMETERS][JSON_EXTRUSION_ENTITIES].size();
    for (int perimeters_entities_index = 0; perimeters_entities_index < perimeters_entities_count; perimeters_entities_index++)
    {
        const json& extrusion_entity_json =  j[JSON_LAYER_REGION_PERIMETERS][JSON_EXTRUSION_ENTITIES][perimeters_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, layer_region.perimeters);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error parsing perimeters found at layer %1%, print_z %2%") %layer_region.layer()->id() %layer_region.layer()->print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing perimeters at layer %zu, print_z %f", layer_region.layer()->id(), layer_region.layer()->print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    //fills
    layer_region.fills.no_sort = j[JSON_LAYER_REGION_FILLS][JSON_EXTRUSION_NO_SORT];
    int fills_entities_count = j[JSON_LAYER_REGION_FILLS][JSON_EXTRUSION_ENTITIES].size();
    for (int fills_entities_index = 0; fills_entities_index < fills_entities_count; fills_entities_index++)
    {
        const json& extrusion_entity_json =  j[JSON_LAYER_REGION_FILLS][JSON_EXTRUSION_ENTITIES][fills_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, layer_region.fills);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error parsing fills found at layer %1%, print_z %2%") %layer_region.layer()->id() %layer_region.layer()->print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing fills at layer %zu, print_z %f", layer_region.layer()->id(), layer_region.layer()->print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    return;
}


void extract_layer(const json& layer_json, Layer& layer) {
    //slice_polygons
    int slice_polygons_count = layer_json[JSON_LAYER_SLICED_POLYGONS].size();
    for (int polygon_index = 0; polygon_index < slice_polygons_count; polygon_index++)
    {
        ExPolygon polygon;

        polygon = layer_json[JSON_LAYER_SLICED_POLYGONS][polygon_index];
        layer.lslices.push_back(std::move(polygon));
    }

    //slice_bboxes
    int sliced_bboxes_count = layer_json[JSON_LAYER_SLLICED_BBOXES].size();
    for (int bbox_index = 0; bbox_index < sliced_bboxes_count; bbox_index++)
    {
        BoundingBox bbox;

        bbox = layer_json[JSON_LAYER_SLLICED_BBOXES][bbox_index];
        layer.lslices_bboxes.push_back(std::move(bbox));
    }

    //overhang_polygons
    int overhang_polygons_count = layer_json[JSON_LAYER_OVERHANG_POLYGONS].size();
    for (int polygon_index = 0; polygon_index < overhang_polygons_count; polygon_index++)
    {
        ExPolygon polygon;

        polygon = layer_json[JSON_LAYER_OVERHANG_POLYGONS][polygon_index];
        layer.loverhangs.push_back(std::move(polygon));
    }

    //overhang_box
    layer.loverhangs_bbox = layer_json[JSON_LAYER_OVERHANG_BBOX];

    //layer_regions
    int layer_region_count = layer.region_count();
    for (int layer_region_index = 0; layer_region_index < layer_region_count; layer_region_index++)
    {
        LayerRegion* layer_region = layer.get_region(layer_region_index);
        const json& layer_region_json = layer_json[JSON_LAYER_REGIONS][layer_region_index];
        convert_layer_region_from_json(layer_region_json, *layer_region);

        //LayerRegion layer_region = layer_json[JSON_LAYER_REGIONS][layer_region_index];
    }

    return;
}

void extract_support_layer(const json& support_layer_json, SupportLayer& support_layer) {
    extract_layer(support_layer_json, support_layer);

    support_layer.support_type = support_layer_json[JSON_SUPPORT_LAYER_TYPE];
    //support_islands
    int islands_count = support_layer_json[JSON_SUPPORT_LAYER_ISLANDS].size();
    for (int islands_index = 0; islands_index < islands_count; islands_index++)
    {
        ExPolygon polygon;

        polygon = support_layer_json[JSON_SUPPORT_LAYER_ISLANDS][islands_index];
        support_layer.support_islands.push_back(std::move(polygon));
    }

    //support_fills
    support_layer.support_fills.no_sort = support_layer_json[JSON_SUPPORT_LAYER_FILLS][JSON_EXTRUSION_NO_SORT];
    int support_fills_entities_count = support_layer_json[JSON_SUPPORT_LAYER_FILLS][JSON_EXTRUSION_ENTITIES].size();
    for (int support_fills_entities_index = 0; support_fills_entities_index < support_fills_entities_count; support_fills_entities_index++)
    {
        const json& extrusion_entity_json =  support_layer_json[JSON_SUPPORT_LAYER_FILLS][JSON_EXTRUSION_ENTITIES][support_fills_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, support_layer.support_fills);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error parsing fills found at support_layer %1%, print_z %2%")%support_layer.id() %support_layer.print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing fills at support_layer %zu, print_z %f", support_layer.id(), support_layer.print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    return;
}

static void from_json(const json& j, groupedVolumeSlices& firstlayer_group)
{
    firstlayer_group.groupId               =   j[JSON_FIRSTLAYER_GROUP_ID];

    int volume_count = j[JSON_FIRSTLAYER_GROUP_VOLUME_IDS].size();
    for (int volume_index = 0; volume_index < volume_count; volume_index++)
    {
        ObjectID obj_id;

        obj_id.id = j[JSON_FIRSTLAYER_GROUP_VOLUME_IDS][volume_index];
        firstlayer_group.volume_ids.push_back(std::move(obj_id));
    }

    int slices_count = j[JSON_FIRSTLAYER_GROUP_SLICES].size();
    for (int slice_index = 0; slice_index < slices_count; slice_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_FIRSTLAYER_GROUP_SLICES][slice_index];
        firstlayer_group.slices.push_back(std::move(polygon));
    }
}

int Print::export_cached_data(const std::string& directory, bool with_space)
{
    int ret = 0;
    boost::filesystem::path directory_path(directory);

    auto convert_layer_to_json = [](json& layer_json, const Layer* layer) {
        json slice_polygons_json = json::array(), slice_bboxs_json = json::array(), overhang_polygons_json = json::array(), layer_regions_json = json::array();
        layer_json[JSON_LAYER_PRINT_Z] = layer->print_z;
        layer_json[JSON_LAYER_HEIGHT] = layer->height;
        layer_json[JSON_LAYER_SLICE_Z] = layer->slice_z;
        layer_json[JSON_LAYER_ID] = layer->id();
        //layer_json["slicing_errors"] = layer->slicing_errors;

        //sliced_polygons
        for (const ExPolygon& slice_polygon : layer->lslices) {
            json slice_polygon_json = slice_polygon;
            slice_polygons_json.push_back(std::move(slice_polygon_json));
        }
        layer_json[JSON_LAYER_SLICED_POLYGONS] = std::move(slice_polygons_json);

        //sliced_bbox
        for (const BoundingBox& slice_bbox : layer->lslices_bboxes) {
            json bbox_json = json::array();

            bbox_json = slice_bbox;
            slice_bboxs_json.push_back(std::move(bbox_json));
        }
        layer_json[JSON_LAYER_SLLICED_BBOXES] = std::move(slice_bboxs_json);

        //overhang_polygons
        for (const ExPolygon& overhang_polygon : layer->loverhangs) {
            json overhang_polygon_json = overhang_polygon;
            overhang_polygons_json.push_back(std::move(overhang_polygon_json));
        }
        layer_json[JSON_LAYER_OVERHANG_POLYGONS] = std::move(overhang_polygons_json);

        //overhang_box
        layer_json[JSON_LAYER_OVERHANG_BBOX] = layer->loverhangs_bbox;

        for (const LayerRegion *layer_region : layer->regions()) {
            json region_json = *layer_region;

            layer_regions_json.push_back(std::move(region_json));
        }
        layer_json[JSON_LAYER_REGIONS] = std::move(layer_regions_json);

        return;
    };

    //firstly clear this directory
    if (fs::exists(directory_path)) {
        fs::remove_all(directory_path);
    }
    try {
        if (!fs::create_directory(directory_path)) {
            BOOST_LOG_TRIVIAL(error) << boost::format("create directory %1% failed")%directory;
            return CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED;
        }
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << boost::format("create directory %1% failed")%directory;
        return CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED;
    }

    int count = 0;
    std::vector<std::string> filename_vector;
    std::vector<json> json_vector;
    for (PrintObject *obj : m_objects) {
        const ModelObject* model_obj = obj->model_object();
        if (obj->get_shared_object()) {
            BOOST_LOG_TRIVIAL(info) << boost::format("shared object %1%, skip directly")%model_obj->name;
            continue;
        }

        const PrintInstance &print_instance = obj->instances()[0];
        const ModelInstance *model_instance = print_instance.model_instance;
        size_t identify_id = (model_instance->loaded_id > 0)?model_instance->loaded_id: model_instance->id().id;
        std::string file_name = directory +"/obj_"+std::to_string(identify_id)+".json";

        BOOST_LOG_TRIVIAL(info) << boost::format("begin to dump object %1%, identify_id %2% to %3%")%model_obj->name %identify_id %file_name;

        try {
            json root_json, layers_json = json::array(), support_layers_json = json::array(), first_layer_groups = json::array();

            root_json[JSON_OBJECT_NAME] = model_obj->name;
            root_json[JSON_IDENTIFY_ID] = identify_id;

            //export the layers
            std::vector<json> layers_json_vector(obj->layer_count());
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->layer_count()),
                [&layers_json_vector, obj, convert_layer_to_json](const tbb::blocked_range<size_t>& layer_range) {
                    for (size_t layer_index = layer_range.begin(); layer_index < layer_range.end(); ++ layer_index) {
                        const Layer *layer = obj->get_layer(layer_index);
                        json layer_json;
                        convert_layer_to_json(layer_json, layer);
                        layers_json_vector[layer_index] = std::move(layer_json);
                    }
                }
            );
            for (int l_index = 0; l_index < layers_json_vector.size(); l_index++) {
                layers_json.push_back(std::move(layers_json_vector[l_index]));
            }
            layers_json_vector.clear();
            /*for (const Layer *layer : obj->layers()) {
                // for each layer
                json layer_json;

                convert_layer_to_json(layer_json, layer);

                layers_json.push_back(std::move(layer_json));
            }*/

            root_json[JSON_LAYERS] = std::move(layers_json);

            //export the support layers
            std::vector<json> support_layers_json_vector(obj->support_layer_count());
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->support_layer_count()),
                [&support_layers_json_vector, obj, convert_layer_to_json](const tbb::blocked_range<size_t>& support_layer_range) {
                    for (size_t s_layer_index = support_layer_range.begin(); s_layer_index < support_layer_range.end(); ++ s_layer_index) {
                        const SupportLayer *support_layer = obj->get_support_layer(s_layer_index);
                        json support_layer_json, support_islands_json = json::array(), support_fills_json, supportfills_entities_json = json::array();

                        convert_layer_to_json(support_layer_json, support_layer);

                        support_layer_json[JSON_SUPPORT_LAYER_INTERFACE_ID] = support_layer->interface_id();
                        support_layer_json[JSON_SUPPORT_LAYER_TYPE] = support_layer->support_type;

                        //support_islands
                        for (const ExPolygon& support_island : support_layer->support_islands) {
                            json support_island_json = support_island;
                            support_islands_json.push_back(std::move(support_island_json));
                        }
                        support_layer_json[JSON_SUPPORT_LAYER_ISLANDS] = std::move(support_islands_json);

                        //support_fills
                        support_fills_json[JSON_EXTRUSION_NO_SORT] = support_layer->support_fills.no_sort;
                        support_fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
                        for (const ExtrusionEntity* extrusion_entity : support_layer->support_fills.entities) {
                            json supportfill_entity_json, supportfill_entity_paths_json = json::array();
                            bool ret = convert_extrusion_to_json(supportfill_entity_json, supportfill_entity_paths_json, extrusion_entity);
                            if (!ret)
                                continue;

                            supportfills_entities_json.push_back(std::move(supportfill_entity_json));
                        }
                        support_fills_json[JSON_EXTRUSION_ENTITIES] = std::move(supportfills_entities_json);
                        support_layer_json[JSON_SUPPORT_LAYER_FILLS] = std::move(support_fills_json);

                        support_layers_json_vector[s_layer_index] = std::move(support_layer_json);
                    }
                }
            );
            for (int s_index = 0; s_index < support_layers_json_vector.size(); s_index++) {
                support_layers_json.push_back(std::move(support_layers_json_vector[s_index]));
            }
            support_layers_json_vector.clear();

            /*for (const SupportLayer *support_layer : obj->support_layers()) {
                json support_layer_json, support_islands_json = json::array(), support_fills_json, supportfills_entities_json = json::array();

                convert_layer_to_json(support_layer_json, support_layer);

                support_layer_json[JSON_SUPPORT_LAYER_INTERFACE_ID] = support_layer->interface_id();

                //support_islands
                for (const ExPolygon& support_island : support_layer->support_islands.expolygons) {
                    json support_island_json = support_island;
                    support_islands_json.push_back(std::move(support_island_json));
                }
                support_layer_json[JSON_SUPPORT_LAYER_ISLANDS] = std::move(support_islands_json);

                //support_fills
                support_fills_json[JSON_EXTRUSION_NO_SORT] = support_layer->support_fills.no_sort;
                support_fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
                for (const ExtrusionEntity* extrusion_entity : support_layer->support_fills.entities) {
                    json supportfill_entity_json, supportfill_entity_paths_json = json::array();
                    bool ret = convert_extrusion_to_json(supportfill_entity_json, supportfill_entity_paths_json, extrusion_entity);
                    if (!ret)
                        continue;

                    supportfills_entities_json.push_back(std::move(supportfill_entity_json));
                }
                support_fills_json[JSON_EXTRUSION_ENTITIES] = std::move(supportfills_entities_json);
                support_layer_json[JSON_SUPPORT_LAYER_FILLS] = std::move(support_fills_json);

                support_layers_json.push_back(std::move(support_layer_json));
            } // for each layer*/
            root_json[JSON_SUPPORT_LAYERS] = std::move(support_layers_json);

            const std::vector<groupedVolumeSlices> &first_layer_obj_groups =  obj->firstLayerObjGroups();
            for (size_t s_group_index = 0; s_group_index < first_layer_obj_groups.size(); ++ s_group_index) {
                groupedVolumeSlices group = first_layer_obj_groups[s_group_index];

                //convert the id
                for (ObjectID& obj_id : group.volume_ids)
                {
                    const ModelVolume* currentModelVolumePtr = nullptr;
                    //BBS: support shared object logic
                    const PrintObject* shared_object = obj->get_shared_object();
                    if (!shared_object)
                        shared_object = obj;
                    const ModelVolumePtrs& volumes_ptr = shared_object->model_object()->volumes;
                    size_t volume_count = volumes_ptr.size();
                    for (size_t index = 0; index < volume_count; index ++) {
                        currentModelVolumePtr = volumes_ptr[index];
                        if (currentModelVolumePtr->id() == obj_id) {
                            obj_id.id = index;
                            break;
                        }
                    }
                }

                json first_layer_group_json;

                first_layer_group_json = group;
                first_layer_groups.push_back(std::move(first_layer_group_json));
            }
            root_json[JSON_FIRSTLAYER_GROUPS] = std::move(first_layer_groups);

            filename_vector.push_back(file_name);
            json_vector.push_back(std::move(root_json));
            /*boost::nowide::ofstream c;
            c.open(file_name, std::ios::out | std::ios::trunc);
            if (with_space)
                c << std::setw(4) << root_json << std::endl;
            else
                c << root_json.dump(0) << std::endl;
            c.close();*/
            count ++;
            BOOST_LOG_TRIVIAL(info) << boost::format("will dump object %1%'s json to %2%.")%model_obj->name%file_name;
        }
        catch(std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": save to "<<file_name<<" got a generic exception, reason = " << err.what();
            ret = CLI_EXPORT_CACHE_WRITE_FAILED;
        }
    }

    boost::mutex mutex;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, filename_vector.size()),
        [filename_vector, &json_vector, with_space, &ret, &mutex](const tbb::blocked_range<size_t>& output_range) {
            for (size_t object_index = output_range.begin(); object_index < output_range.end(); ++ object_index) {
                try {
                    boost::nowide::ofstream c;
                    c.open(filename_vector[object_index], std::ios::out | std::ios::trunc);
                    if (with_space)
                        c << std::setw(4) << json_vector[object_index] << std::endl;
                    else
                        c << json_vector[object_index].dump(0) << std::endl;
                    c.close();
                }
                catch(std::exception &err) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": save to "<<filename_vector[object_index]<<" got a generic exception, reason = " << err.what();
                    boost::unique_lock l(mutex);
                    ret = CLI_EXPORT_CACHE_WRITE_FAILED;
                }
            }
        }
    );

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": total printobject count %1%, saved %2%, ret=%3%")%m_objects.size() %count %ret;
    return ret;
}


int Print::load_cached_data(const std::string& directory)
{
    int ret = 0;
    boost::filesystem::path directory_path(directory);

    if (!fs::exists(directory_path)) {
        BOOST_LOG_TRIVIAL(info) << boost::format("directory %1% not exist.")%directory;
        return CLI_IMPORT_CACHE_NOT_FOUND;
    }

    auto find_region = [this](PrintObject* object, size_t config_hash) -> const PrintRegion* {
        int regions_count = object->num_printing_regions();
        for (int index = 0; index < regions_count; index++ )
        {
            const PrintRegion&  print_region = object->printing_region(index);
            if (print_region.config_hash() == config_hash ) {
                return &print_region;
            }
        }
        return NULL;
    };

    int count = 0;
    std::vector<std::pair<std::string, PrintObject*>> object_filenames;
    for (PrintObject *obj : m_objects) {
        const ModelObject* model_obj = obj->model_object();
        const PrintInstance &print_instance = obj->instances()[0];
        const ModelInstance *model_instance = print_instance.model_instance;

        obj->clear_layers();
        obj->clear_support_layers();

        int identify_id = model_instance->loaded_id;
        if (identify_id <= 0) {
            //for old 3mf
            identify_id = model_instance->id().id;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": object %1%'s loaded_id is 0, need to use the instance_id %2%")%model_obj->name %identify_id;
            //continue;
        }
        std::string file_name = directory +"/obj_"+std::to_string(identify_id)+".json";

        if (!fs::exists(file_name)) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": file %1% not exist, maybe a shared object, skip it")%file_name;
            continue;
        }
        object_filenames.push_back({file_name, obj});
    }

    boost::mutex mutex;
    std::vector<json> object_jsons(object_filenames.size());
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, object_filenames.size()),
        [object_filenames, &ret, &object_jsons, &mutex](const tbb::blocked_range<size_t>& filename_range) {
            for (size_t filename_index = filename_range.begin(); filename_index < filename_range.end(); ++ filename_index) {
                try {
                    json root_json;
                    boost::nowide::ifstream ifs(object_filenames[filename_index].first);
                    ifs >> root_json;
                    object_jsons[filename_index] = std::move(root_json);
                }
                catch(std::exception &err) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": load from "<<object_filenames[filename_index].first<<" got a generic exception, reason = " << err.what();
                    boost::unique_lock l(mutex);
                    ret = CLI_IMPORT_CACHE_LOAD_FAILED;
                }
            }
        }
    );

    if (ret) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< boost::format(": load json failed.");
        return ret;
    }

    for (int obj_index = 0; obj_index < object_jsons.size(); obj_index++) {
        json& root_json = object_jsons[obj_index];
        PrintObject *obj = object_filenames[obj_index].second;

        try {
            //boost::nowide::ifstream ifs(file_name);
            //ifs >> root_json;

            std::string name = root_json.at(JSON_OBJECT_NAME);
            int identify_id = root_json.at(JSON_IDENTIFY_ID);
            int layer_count = 0, support_layer_count = 0, firstlayer_group_count = 0;

            layer_count = root_json[JSON_LAYERS].size();
            support_layer_count = root_json[JSON_SUPPORT_LAYERS].size();
            firstlayer_group_count = root_json[JSON_FIRSTLAYER_GROUPS].size();

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(":will load %1%, identify_id %2%, layer_count %3%, support_layer_count %4%, firstlayer_group_count %5%")
                %name %identify_id %layer_count %support_layer_count %firstlayer_group_count;

            Layer* previous_layer = NULL;
            //create layer and layer regions
            for (int index = 0; index < layer_count; index++)
            {
                json& layer_json = root_json[JSON_LAYERS][index];
                Layer* new_layer = obj->add_layer(layer_json[JSON_LAYER_ID], layer_json[JSON_LAYER_HEIGHT], layer_json[JSON_LAYER_PRINT_Z], layer_json[JSON_LAYER_SLICE_Z]);
                if (!new_layer) {
                    BOOST_LOG_TRIVIAL(error) <<__FUNCTION__<< boost::format(":create_layer failed, out of memory");
                    return CLI_OUT_OF_MEMORY;
                }
                if (previous_layer) {
                    previous_layer->upper_layer = new_layer;
                    new_layer->lower_layer = previous_layer;
                }
                previous_layer = new_layer;

                //layer regions
                int layer_regions_count = layer_json[JSON_LAYER_REGIONS].size();
                for (int region_index = 0; region_index < layer_regions_count; region_index++)
                {
                    json& region_json = layer_json[JSON_LAYER_REGIONS][region_index];
                    size_t config_hash = region_json[JSON_LAYER_REGION_CONFIG_HASH];
                    const PrintRegion *print_region = find_region(obj, config_hash);

                    if (!print_region){
                        BOOST_LOG_TRIVIAL(error) <<__FUNCTION__<< boost::format(":can not find print region of object %1%, layer %2%, print_z %3%, layer_region %4%")
                            %name % index %new_layer->print_z %region_index;
                        //delete new_layer;
                        return CLI_IMPORT_CACHE_DATA_CAN_NOT_USE;
                    }

                    new_layer->add_region(print_region);
                }

            }

            //load the layer data parallel
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": load the layers in parallel");
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->layer_count()),
                [&root_json, &obj](const tbb::blocked_range<size_t>& layer_range) {
                    for (size_t layer_index = layer_range.begin(); layer_index < layer_range.end(); ++ layer_index) {
                        const json& layer_json = root_json[JSON_LAYERS][layer_index];
                        Layer* layer = obj->get_layer(layer_index);
                        extract_layer(layer_json, *layer);
                    }
                }
            );

            //support layers
            Layer* previous_support_layer = NULL;
            //create support_layers
            for (int index = 0; index < support_layer_count; index++)
            {
                json& layer_json = root_json[JSON_SUPPORT_LAYERS][index];
                SupportLayer* new_support_layer = obj->add_support_layer(layer_json[JSON_LAYER_ID], layer_json[JSON_SUPPORT_LAYER_INTERFACE_ID], layer_json[JSON_LAYER_HEIGHT], layer_json[JSON_LAYER_PRINT_Z]);
                if (!new_support_layer) {
                    BOOST_LOG_TRIVIAL(error) <<__FUNCTION__<< boost::format(":add_support_layer failed, out of memory");
                    return CLI_OUT_OF_MEMORY;
                }
                if (previous_support_layer) {
                    previous_support_layer->upper_layer = new_support_layer;
                    new_support_layer->lower_layer = previous_support_layer;
                }
                previous_support_layer = new_support_layer;
            }

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": finished load layers, start to load support_layers.");
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->support_layer_count()),
                [&root_json, &obj](const tbb::blocked_range<size_t>& support_layer_range) {
                    for (size_t layer_index = support_layer_range.begin(); layer_index < support_layer_range.end(); ++ layer_index) {
                        const json& layer_json = root_json[JSON_SUPPORT_LAYERS][layer_index];
                        SupportLayer* support_layer = obj->get_support_layer(layer_index);
                        extract_support_layer(layer_json, *support_layer);
                    }
                }
            );

            //load first group volumes
            std::vector<groupedVolumeSlices>& firstlayer_objgroups = obj->firstLayerObjGroupsMod();
            for (int index = 0; index < firstlayer_group_count; index++)
            {
                json& firstlayer_group_json = root_json[JSON_FIRSTLAYER_GROUPS][index];
                groupedVolumeSlices firstlayer_group = firstlayer_group_json;
                //convert the id
                for (ObjectID& obj_id : firstlayer_group.volume_ids)
                {
                    ModelVolume* currentModelVolumePtr = nullptr;
                    ModelVolumePtrs& volumes_ptr = obj->model_object()->volumes;
                    size_t volume_count = volumes_ptr.size();
                    if (obj_id.id < volume_count) {
                        currentModelVolumePtr = volumes_ptr[obj_id.id];
                        obj_id = currentModelVolumePtr->id();
                    }
                    else {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< boost::format(": can not find volume_id %1% from object file %2% in firstlayer groups, volume_count %3%!")
                            %obj_id.id %object_filenames[obj_index].first %volume_count;
                        return CLI_IMPORT_CACHE_LOAD_FAILED;
                    }
                }
                firstlayer_objgroups.push_back(std::move(firstlayer_group));
            }

            count ++;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": load object %1% from %2% successfully.")%count%object_filenames[obj_index].first;
        }
        catch(nlohmann::detail::parse_error &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<object_filenames[obj_index].first<<" got a nlohmann::detail::parse_error, reason = " << err.what();
            return CLI_IMPORT_CACHE_LOAD_FAILED;
        }
        catch(std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": load from "<<object_filenames[obj_index].first<<" got a generic exception, reason = " << err.what();
            ret = CLI_IMPORT_CACHE_LOAD_FAILED;
        }
    }

    object_jsons.clear();
    object_filenames.clear();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": total printobject count %1%, loaded %2%, ret=%3%")%m_objects.size() %count %ret;
    return ret;
}

BoundingBoxf3 PrintInstance::get_bounding_box() const {
    return print_object->model_object()->instance_bounding_box(*model_instance, false);
}

Polygon PrintInstance::get_convex_hull_2d() {
    Polygon poly = print_object->model_object()->convex_hull_2d(model_instance->get_matrix());
    poly.douglas_peucker(0.1);
    return poly;
}

//BBS: instance_shift is too large because of multi-plate, apply without plate offset.
Point PrintInstance::shift_without_plate_offset() const
{
    const Print* print = print_object->print();
    const Vec3d plate_offset = print->get_plate_origin();
    return shift - Point(scaled(plate_offset.x()), scaled(plate_offset.y()));
}

PrintRegion *PrintObjectRegions::FuzzySkinPaintedRegion::parent_print_object_region(const LayerRangeRegions &layer_range) const
{
    using FuzzySkinParentType = PrintObjectRegions::FuzzySkinPaintedRegion::ParentType;

    if (this->parent_type == FuzzySkinParentType::PaintedRegion) {
        return layer_range.painted_regions[this->parent].region;
    }

    assert(this->parent_type == FuzzySkinParentType::VolumeRegion);
    return layer_range.volume_regions[this->parent].region;
}

int PrintObjectRegions::FuzzySkinPaintedRegion::parent_print_object_region_id(const LayerRangeRegions &layer_range) const
{
    return this->parent_print_object_region(layer_range)->print_object_region_id();
}

ExtrusionLayers FakeWipeTower::getTrueExtrusionLayersFromWipeTower() const
{
    ExtrusionLayers wtels;
    wtels.type = ExtrusionLayersType::WIPE_TOWER;
    std::vector<float> layer_heights;
    layer_heights.reserve(outer_wall.size());
    auto pre = outer_wall.begin();
    for (auto it = outer_wall.begin(); it != outer_wall.end(); ++it) {
        if (it == outer_wall.begin())
            layer_heights.push_back(it->first);
        else {
            layer_heights.push_back(it->first - pre->first);
            ++pre;
        }
    }
    Point trans = {scale_(pos.x()), scale_(pos.y())};
    for (auto it = outer_wall.begin(); it != outer_wall.end(); ++it) {
        int            index = std::distance(outer_wall.begin(), it);
        ExtrusionLayer el;
        ExtrusionPaths paths;
        paths.reserve(it->second.size());
        for (auto &polyline : it->second) {
            ExtrusionPath path(ExtrusionRole::erWipeTower, 0.0, 0.0, layer_heights[index]);
            path.polyline = polyline;
            for (auto &p : path.polyline.points) p += trans;
            paths.push_back(path);
        }
        el.paths    = std::move(paths);
        el.bottom_z = it->first - layer_heights[index];
        el.layer    = nullptr;
        wtels.push_back(el);
    }
    return wtels;
}
void WipeTowerData::construct_mesh(float width, float depth, float height, float brim_width, bool is_rib_wipe_tower, float rib_width, float rib_length,bool fillet_wall)
{
    wipe_tower_mesh_data = WipeTowerMeshData{};
    float first_layer_height=0.08; //brim height
    if (width < EPSILON || depth < EPSILON || height < EPSILON) return;
    if (!is_rib_wipe_tower || rib_length < EPSILON) {
        wipe_tower_mesh_data->real_wipe_tower_mesh = make_cube(width, depth, height);
        wipe_tower_mesh_data->real_brim_mesh       = make_cube(width + 2 * brim_width, depth + 2 * brim_width, first_layer_height);
        wipe_tower_mesh_data->real_brim_mesh.translate({-brim_width, -brim_width, 0});
        wipe_tower_mesh_data->bottom = {scaled(Vec2f{-brim_width, -brim_width}), scaled(Vec2f{width + brim_width, 0}), scaled(Vec2f{width + brim_width, depth + brim_width}),
                                        scaled(Vec2f{0, depth})};
    } else {
        wipe_tower_mesh_data->real_wipe_tower_mesh = WipeTower::its_make_rib_tower(width, depth, height, rib_length, rib_width, fillet_wall);
        wipe_tower_mesh_data->bottom               = WipeTower::rib_section(width, depth, rib_length, rib_width, fillet_wall);
        auto brim_bottom                           = offset(wipe_tower_mesh_data->bottom, scaled(brim_width));
        if (!brim_bottom.empty())
            wipe_tower_mesh_data->bottom               = brim_bottom.front();
        wipe_tower_mesh_data->real_brim_mesh       = WipeTower::its_make_rib_brim(wipe_tower_mesh_data->bottom, first_layer_height);
        wipe_tower_mesh_data->real_wipe_tower_mesh.translate(Vec3f(rib_offset[0], rib_offset[1],0));
        wipe_tower_mesh_data->real_brim_mesh.translate(Vec3f(rib_offset[0], rib_offset[1], 0));
        wipe_tower_mesh_data->bottom.translate(scaled(Vec2f(rib_offset[0], rib_offset[1])));
    }
    //wipe_tower_mesh_data->real_wipe_tower_mesh.write_ascii("../wipe_tower_mesh.obj");
   //wipe_tower_mesh_data->real_brim_mesh.write_ascii("../wipe_tower_brim_mesh.obj");
}

} // namespace Slic3r
