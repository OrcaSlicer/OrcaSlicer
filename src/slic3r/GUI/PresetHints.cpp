#include <cassert>

#include "libslic3r/Flow.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/libslic3r.h"

#include "orca/Config.hpp"

#include "PresetHints.hpp"

#include <wx/intl.h> 

#include "GUI.hpp"
#include "format.hpp"
#include "I18N.hpp"

namespace Slic3r {

#define MIN_BUF_LENGTH	4096
std::string PresetHints::cooling_description(const Preset &preset)
{
	std::string out;
    //BBS: don't show cooling_description now
    /*
    bool cooling              = preset.config.opt_bool("cooling", 0); // TODO(orca-types): manual migration — unknown key (not in ConfigDef)
    int  fan_cooling_layer_time = static_cast<int>(::orca::config::get_at<::orca::keys::fan_cooling_layer_time>(preset.config, 0).value_or(0.0));
    int  full_fan_speed_layer = ::orca::config::get_at<::orca::keys::full_fan_speed_layer>(preset.config, 0).value_or(0);

    if (cooling) {
		int 	slow_down_layer_time 	= static_cast<int>(::orca::config::get_at<::orca::keys::slow_down_layer_time>(preset.config, 0).value_or(0.0));
		int 	fan_min_speed 				= static_cast<int>(::orca::config::get_at<::orca::keys::fan_min_speed>(preset.config, 0).value_or(0.0));
		int 	fan_max_speed 				= static_cast<int>(::orca::config::get_at<::orca::keys::fan_max_speed>(preset.config, 0).value_or(0.0));
		int 	slow_down_min_speed				= int(::orca::config::get_at<::orca::keys::slow_down_min_speed>(preset.config, 0).value_or(0.0) + 0.5);

        out += GUI::format(_L("If estimated layer time is below ~%1%s, "
                              "fan will run at %2%%% and print speed will be reduced "
                              "so that no less than %3%s are spent on that layer "
                              "(however, speed will never be reduced below %4%mm/s)."),
                              slow_down_layer_time, fan_max_speed, slow_down_layer_time, slow_down_min_speed);
        if (fan_cooling_layer_time > slow_down_layer_time) {
            out += "\n";
            if (fan_min_speed != fan_max_speed)
                out += GUI::format(_L("If estimated layer time is greater, but still below ~%1%s, "
                               "fan will run at a proportionally decreasing speed between %2%%% and %3%%%."),
                               fan_cooling_layer_time, fan_max_speed, fan_min_speed);
            else
                out += GUI::format(_L("If estimated layer time is greater, but still below ~%1%s, "
                               "fan will run at %2%%%"),
                               fan_cooling_layer_time, fan_min_speed);
        }
        out += "\n";
    }
	if (static_cast<bool>(::orca::config::get_at<::orca::keys::reduce_fan_stop_start_freq>(preset.config, 0).value_or(0))) {
		int 	close_fan_the_first_x_layers 	= ::orca::config::get_at<::orca::keys::close_fan_the_first_x_layers>(preset.config, 0).value_or(0);
		int 	fan_min_speed 				= static_cast<int>(::orca::config::get_at<::orca::keys::fan_min_speed>(preset.config, 0).value_or(0.0));

        if (full_fan_speed_layer > close_fan_the_first_x_layers + 1)
            out += GUI::format(_L("Fan speed will be ramped from zero at layer %1% to %2%%% at layer %3%."), close_fan_the_first_x_layers, fan_min_speed, full_fan_speed_layer);
        else {
            out += GUI::format(cooling ? _L("During the other layers, fan will always run at %1%%%") : _L("Fan will always run at %1%%%"), fan_min_speed) + " ";
            if (close_fan_the_first_x_layers > 1)
                out += GUI::format(_L("except for the first %1% layers."), close_fan_the_first_x_layers);
            else if (close_fan_the_first_x_layers == 1)
            	out += GUI::format(_L("except for the first layer."));
        }
    } else
       out += cooling ? _u8L("During the other layers, fan will be turned off.") : _u8L("Fan will be turned off.");
    */
    return out;
}

static const ConfigOptionFloatOrPercent& first_positive(const ConfigOptionFloatOrPercent *v1, const ConfigOptionFloatOrPercent &v2, const ConfigOptionFloatOrPercent &v3)
{
    return (v1 != nullptr && v1->value > 0) ? *v1 : ((v2.value > 0) ? v2 : v3);
}

std::string PresetHints::maximum_volumetric_flow_description(const PresetBundle &preset_bundle)
{
    std::string out;
    //BBS: don't show maximum_volumetric_flow_description now
    /*
    // Find out, to which nozzle index is the current filament profile assigned.
    int idx_extruder  = 0;
	int num_extruders = (int)preset_bundle.filament_presets.size();
    for (; idx_extruder < num_extruders; ++ idx_extruder)
        if (preset_bundle.filament_presets[idx_extruder] == preset_bundle.filaments.get_selected_preset_name())
            break;
    if (idx_extruder == num_extruders)
        // The current filament preset is not active for any extruder.
        idx_extruder = -1;

    const DynamicPrintConfig &print_config    = preset_bundle.prints   .get_edited_preset().config;
    const DynamicPrintConfig &filament_config = preset_bundle.filaments.get_edited_preset().config;
    const DynamicPrintConfig &printer_config  = preset_bundle.printers .get_edited_preset().config;

    // Current printer values.
    float  nozzle_diameter                  = (float)::orca::config::get_at<::orca::keys::nozzle_diameter>(printer_config, idx_extruder).value_or(0.0);

    // Print config values
    double layer_height                     = ::orca::config::get<::orca::keys::layer_height>(print_config).value_or(0.0);
    double initial_layer_print_height               = ::orca::config::get<::orca::keys::initial_layer_print_height>(print_config).value_or(0.0);
    double support_speed           = ::orca::config::get<::orca::keys::support_speed>(print_config).value_or(0.0);
    double support_interface_speed = print_config.get_abs_value("support_interface_speed");
    double bridge_speed                     = ::orca::config::get<::orca::keys::bridge_speed>(print_config).value_or(0.0);
    double bridge_flow                = ::orca::config::get<::orca::keys::bridge_flow>(print_config).value_or(0.0);
    double inner_wall_speed                  = ::orca::config::get<::orca::keys::inner_wall_speed>(print_config).value_or(0.0);
    double outer_wall_speed         = print_config.get_abs_value("outer_wall_speed", inner_wall_speed);
    // double gap_infill_speed                   = print_config.opt_bool("filter_out_gap_fill") ? print_config.opt_float("gap_infill_speed") : 0.; // TODO(orca-types): manual migration — filter_out_gap_fill cataloged as Float but used as Bool (type mismatch)
    double sparse_infill_speed                     = ::orca::config::get<::orca::keys::sparse_infill_speed>(print_config).value_or(0.0);
    double small_perimeter_speed            = print_config.get_abs_value("small_perimeter_speed", inner_wall_speed);
    double internal_solid_infill_speed               = ::orca::config::get<::orca::keys::internal_solid_infill_speed>(print_config).value_or(0.0);
    double top_surface_speed           = ::orca::config::get<::orca::keys::top_surface_speed>(print_config).value_or(0.0);
    // Maximum print speed when auto-speed is enabled by setting any of the above speed values to zero.
    double max_print_speed                  = print_config.opt_float("max_print_speed"); // TODO(orca-types): manual migration — unknown key (not in ConfigDef)
    // Maximum volumetric speed allowed for the print profile.
    double max_volumetric_speed             = print_config.opt_float("max_volumetric_speed"); // TODO(orca-types): manual migration — unknown key (not in ConfigDef)

    const auto extrusion_width                     = ::orca::config::get<::orca::keys::line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto outer_wall_line_width  = ::orca::config::get<::orca::keys::outer_wall_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto initial_layer_line_width         = ::orca::config::get<::orca::keys::initial_layer_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto sparse_infill_line_width              = ::orca::config::get<::orca::keys::sparse_infill_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto inner_wall_line_width           = ::orca::config::get<::orca::keys::inner_wall_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto internal_solid_infill_line_width        = ::orca::config::get<::orca::keys::internal_solid_infill_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto support_line_width    = ::orca::config::get<::orca::keys::support_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto top_surface_line_width          = ::orca::config::get<::orca::keys::top_surface_line_width>(print_config).value_or(Slic3r::FloatOrPercent{0.0, false});
    const auto &initial_layer_speed                   = *print_config.option<ConfigOptionFloatOrPercent>("initial_layer_speed"); // TODO(orca-types): manual migration — key cataloged as Float but code uses ConfigOptionFloatOrPercent (.get_abs_value below)

    // Index of an extruder assigned to a feature. If set to 0, an active extruder will be used for a multi-material print.
    // If different from idx_extruder, it will not be taken into account for this hint.
    auto feature_extruder_active = [idx_extruder, num_extruders](int i) {
        return i <= 0 || i > num_extruders || idx_extruder == -1 || idx_extruder == i - 1;
    };
    bool perimeter_extruder_active                  = feature_extruder_active(::orca::config::get<::orca::keys::outer_wall_filament_id>(print_config).value_or(0))
                                                    && feature_extruder_active(::orca::config::get<::orca::keys::inner_wall_filament_id>(print_config).value_or(0));
    bool infill_extruder_active                     = feature_extruder_active(::orca::config::get<::orca::keys::sparse_infill_filament_id>(print_config).value_or(0));
    bool solid_infill_extruder_active               = feature_extruder_active(::orca::config::get<::orca::keys::internal_solid_filament_id>(print_config).value_or(0))
                                                    && feature_extruder_active(::orca::config::get<::orca::keys::top_surface_filament_id>(print_config).value_or(0))
                                                    && feature_extruder_active(::orca::config::get<::orca::keys::bottom_surface_filament_id>(print_config).value_or(0));
    bool support_material_extruder_active           = feature_extruder_active(::orca::config::get<::orca::keys::support_filament>(print_config).value_or(0));
    bool support_material_interface_extruder_active = feature_extruder_active(::orca::config::get<::orca::keys::support_interface_filament>(print_config).value_or(0));

    // Current filament values
    double filament_diameter                = ::orca::config::get_at<::orca::keys::filament_diameter>(filament_config, 0).value_or(0.0);
    double filament_crossection             = M_PI * 0.25 * filament_diameter * filament_diameter;
    // double filament_flow_ratio             = ::orca::config::get_at<::orca::keys::filament_flow_ratio>(filament_config, 0).value_or(0.0);
    // The following value will be annotated by this hint, so it does not take part in the calculation.
//    double filament_max_volumetric_speed    = ::orca::config::get_at<::orca::keys::filament_max_volumetric_speed>(filament_config, 0).value_or(0.0);
    for (size_t idx_type = (initial_layer_line_width.value == 0) ? 1 : 0; idx_type < 3; ++ idx_type) {
        // First test the maximum volumetric extrusion speed for non-bridging extrusions.
        bool first_layer = idx_type == 0;
        bool bridging    = idx_type == 2;
		const ConfigOptionFloatOrPercent *first_layer_extrusion_width_ptr = (first_layer && initial_layer_line_width.value > 0) ?
			&initial_layer_line_width : nullptr;
        const float                       lh  = float(first_layer ? initial_layer_print_height : layer_height);
        double                            max_flow = 0.;
        std::string                       max_flow_extrusion_type;
        auto                              limit_by_first_layer_speed = [&initial_layer_speed, first_layer](double speed_normal, double speed_max) {
            if (first_layer && initial_layer_speed.value > 0)
                // Apply the first layer limit.
                speed_normal = initial_layer_speed.get_abs_value(speed_normal);
            return (speed_normal > 0.) ? speed_normal : speed_max;
        };
        auto test_flow =
            [first_layer_extrusion_width_ptr, extrusion_width, nozzle_diameter, lh, bridging, bridge_speed, bridge_flow, limit_by_first_layer_speed, max_print_speed, &max_flow, &max_flow_extrusion_type]
            (FlowRole flow_role, const ConfigOptionFloatOrPercent &this_extrusion_width, double speed, const char *err_msg) {
            Flow flow = bridging ?
                Flow::new_from_config_width(flow_role, first_positive(first_layer_extrusion_width_ptr, this_extrusion_width, extrusion_width), nozzle_diameter, lh) :
                Flow::bridging_flow(nozzle_diameter * bridge_flow, nozzle_diameter);
            double volumetric_flow = flow.mm3_per_mm() * (bridging ? bridge_speed : limit_by_first_layer_speed(speed, max_print_speed));
            if (max_flow < volumetric_flow) {
                max_flow = volumetric_flow;
                max_flow_extrusion_type = _utf8(err_msg);
            }
        };
        if (perimeter_extruder_active) {
            test_flow(frExternalPerimeter, outer_wall_line_width, std::max(outer_wall_speed, small_perimeter_speed), L("outer wall"));
            test_flow(frPerimeter,         inner_wall_line_width,          std::max(inner_wall_speed,          small_perimeter_speed), L("inner wall"));
        }
        if (! bridging && infill_extruder_active)
            test_flow(frInfill, sparse_infill_line_width, sparse_infill_speed, L("sparse infill"));
        if (solid_infill_extruder_active) {
            test_flow(frInfill, internal_solid_infill_line_width, internal_solid_infill_speed, L("internal solid infill"));
            if (! bridging)
                test_flow(frInfill, top_surface_line_width, top_surface_speed, L("top surface"));
        }
        if (! bridging && support_material_extruder_active)
            test_flow(frSupportMaterial, support_line_width, support_speed, L("support"));
        if (support_material_interface_extruder_active)
            test_flow(frSupportMaterialInterface, support_line_width, support_interface_speed, L("support interface"));
        //FIXME handle gap_infill_speed
        if (! out.empty())
            out += "\n";
        out += (first_layer ? _utf8(L("First layer volumetric")) : (bridging ? _utf8(L("Bridge volumetric")) : _utf8(L("Volumetric"))));
        out += " " + _utf8(L("flow rate is maximized")) + " ";
        bool limited_by_max_volumetric_speed = max_volumetric_speed > 0 && max_volumetric_speed < max_flow;
        out += (limited_by_max_volumetric_speed ? 
            _utf8(L("by the print profile maximum")) :
            (_utf8(L("when printing"))+ " " + max_flow_extrusion_type))
            + " " + _utf8(L("with a volumetric rate"))+ " ";
        if (limited_by_max_volumetric_speed)
            max_flow = max_volumetric_speed;

        out += (boost::format(_utf8(L("%3.2f mm³/s at filament speed %3.2f mm/s."))) % max_flow % (max_flow / filament_crossection)).str();
    }
    */
 	return out;
}

std::string PresetHints::recommended_thin_wall_thickness(const PresetBundle &preset_bundle)
{
    std::string out;
    //BBS: don't show recommended_thin_wall_thickness description now
    /*
    const DynamicPrintConfig &print_config    = preset_bundle.prints   .get_edited_preset().config;
    const DynamicPrintConfig &printer_config  = preset_bundle.printers .get_edited_preset().config;

    float   layer_height                        = float(::orca::config::get<::orca::keys::layer_height>(print_config).value_or(0.0));
    int     num_perimeters                      = ::orca::config::get<::orca::keys::wall_loops>(print_config).value_or(0);
    bool    thin_walls                          = ::orca::config::get<::orca::keys::detect_thin_wall>(print_config).value_or(false);
    float   nozzle_diameter                     = float(::orca::config::get_at<::orca::keys::nozzle_diameter>(printer_config, 0).value_or(0.0));
    
    std::string out;
	if (layer_height <= 0.f) {
		out += _utf8(L("Recommended object thin wall thickness: Not available due to invalid layer height."));
		return out;
	}
    
    if (num_perimeters > 0) {
        int num_lines = std::min(num_perimeters * 2, 10);
        out += (boost::format(_utf8(L("Recommended object thin wall thickness for layer height %.2f and"))) % layer_height).str() + " ";
        // Start with the width of two closely spaced 
        try {
            Flow external_perimeter_flow = Flow::new_from_config_width(
                frExternalPerimeter, 
                *print_config.opt<ConfigOptionFloatOrPercent>("outer_wall_line_width"), // TODO(orca-types): manual migration — Flow::new_from_config_width expects ConfigOptionFloatOrPercent&, typed surface returns FloatOrPercent value
                nozzle_diameter, layer_height);
            Flow perimeter_flow          = Flow::new_from_config_width(
                frPerimeter,
                *print_config.opt<ConfigOptionFloatOrPercent>("inner_wall_line_width"), // TODO(orca-types): manual migration — Flow::new_from_config_width expects ConfigOptionFloatOrPercent&, typed surface returns FloatOrPercent value
                nozzle_diameter, layer_height);
	        double width = external_perimeter_flow.width() + external_perimeter_flow.spacing();
	        for (int i = 2; i <= num_lines; thin_walls ? ++ i : i += 2) {
	            if (i > 2)
	                out += ", ";
	            out += (boost::format(_utf8(L("%d lines: %.2f mm"))) % i %  width).str() + " ";
	            width += perimeter_flow.spacing() * (thin_walls ? 1.f : 2.f);
	        }
	    } catch (const FlowErrorNegativeSpacing &) {
            out = _utf8(L("Recommended object thin wall thickness: Not available due to excessively small extrusion width."));
        }
    }*/
    return out;
}


// Produce a textual explanation of the combined effects of the top/bottom_shell_layers
// versus top/bottom_min_shell_thickness. Which of the two values wins depends
// on the active layer height.
std::string PresetHints::top_bottom_shell_thickness_explanation(const PresetBundle &preset_bundle)
{
    std::string out;
    //BBS: don't show top_bottom_shell_thickness_explanation now
    /*
    const DynamicPrintConfig &print_config    = preset_bundle.prints   .get_edited_preset().config;
    const DynamicPrintConfig &printer_config  = preset_bundle.printers .get_edited_preset().config;

    int 	top_shell_layers                = ::orca::config::get<::orca::keys::top_shell_layers>(print_config).value_or(0);
    int 	bottom_shell_layers             = ::orca::config::get<::orca::keys::bottom_shell_layers>(print_config).value_or(0);
    bool    has_top_layers 					= top_shell_layers > 0;
    bool    has_bottom_layers 				= bottom_shell_layers > 0;
    double  top_shell_thickness        	= ::orca::config::get<::orca::keys::top_shell_thickness>(print_config).value_or(0.0);
    double  bottom_shell_thickness  	= ::orca::config::get<::orca::keys::bottom_shell_thickness>(print_config).value_or(0.0);
    double  layer_height                    = ::orca::config::get<::orca::keys::layer_height>(print_config).value_or(0.0);
    //FIXME the following line takes into account the 1st extruder only.
    double  min_layer_height				= Slicing::min_layer_height_from_nozzle(printer_config, 1);

	if (layer_height <= 0.f) {
		out += _utf8(L("Top / bottom shell thickness hint: Not available due to invalid layer height."));
		return out;
	}

    if (has_top_layers) {
    	double top_shell_thickness = top_shell_layers * layer_height;
    	if (top_shell_thickness < top_shell_thickness) {
    		// top_solid_min_shell_thickness triggers even in case of normal layer height. Round the top_shell_thickness up
    		// to an integer multiply of layer_height.
    		double n = ceil(top_shell_thickness / layer_height);
    		top_shell_thickness = n * layer_height;
    	}
    	double top_shell_thickness_minimum = std::max(top_shell_thickness, top_shell_layers * min_layer_height);
        out += (boost::format(_utf8(L("Top shell is %1% mm thick for layer height %2% mm."))) % top_shell_thickness % layer_height).str();
        if (top_shell_thickness_minimum < top_shell_thickness) {
        	out += " ";
	        out += (boost::format(_utf8(L("Minimum top shell thickness is %1% mm."))) % top_shell_thickness_minimum).str();        	
        }
    } else
        out += _utf8(L("Top is open."));

    out += "\n";

    if (has_bottom_layers) {
    	double bottom_shell_thickness = bottom_shell_layers * layer_height;
    	if (bottom_shell_thickness < bottom_shell_thickness) {
    		// bottom_solid_min_shell_thickness triggers even in case of normal layer height. Round the bottom_shell_thickness up
    		// to an integer multiply of layer_height.
    		double n = ceil(bottom_shell_thickness / layer_height);
    		bottom_shell_thickness = n * layer_height;
    	}
    	double bottom_shell_thickness_minimum = std::max(bottom_shell_thickness, bottom_shell_layers * min_layer_height);
        out += (boost::format(_utf8(L("Bottom shell is %1% mm thick for layer height %2% mm."))) % bottom_shell_thickness % layer_height).str();
        if (bottom_shell_thickness_minimum < bottom_shell_thickness) {
        	out += " ";
	        out += (boost::format(_utf8(L("Minimum bottom shell thickness is %1% mm."))) % bottom_shell_thickness_minimum).str();        	
        }
    } else 
        out += _utf8(L("Bottom is open."));
    */
    return out;
}

}; // namespace Slic3r
