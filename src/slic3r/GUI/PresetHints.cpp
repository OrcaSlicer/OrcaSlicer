#include <cassert>

#include <algorithm>

#include "libslic3r/Flow.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Magma/MagmaTriangleCell.hpp"
#include "libslic3r/Magma/MagmaTubeMap.hpp"
#include "libslic3r/Magma/MagmaPatterns.hpp"
#include "libslic3r/Magma/MagmaResolved.hpp"

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
    bool cooling              = preset.config.opt_bool("cooling", 0);
    int  fan_cooling_layer_time = preset.config.opt_int("fan_cooling_layer_time", 0);
    int  full_fan_speed_layer = preset.config.opt_int("full_fan_speed_layer", 0);

    if (cooling) {
		int 	slow_down_layer_time 	= preset.config.opt_int("slow_down_layer_time", 0);
		int 	fan_min_speed 				= preset.config.opt_int("fan_min_speed", 0);
		int 	fan_max_speed 				= preset.config.opt_int("fan_max_speed", 0);
		int 	slow_down_min_speed				= int(preset.config.opt_float("slow_down_min_speed", 0) + 0.5);

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
	if (preset.config.opt_bool("reduce_fan_stop_start_freq", 0)) {
		int 	close_fan_the_first_x_layers 	= preset.config.opt_int("close_fan_the_first_x_layers", 0);
		int 	fan_min_speed 				= preset.config.opt_int("fan_min_speed", 0);

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
    float  nozzle_diameter                  = (float)printer_config.opt_float("nozzle_diameter", idx_extruder);

    // Print config values
    double layer_height                     = print_config.opt_float("layer_height");
    double initial_layer_print_height               = print_config.opt_float("initial_layer_print_height");
    double support_speed           = print_config.opt_float("support_speed");
    double support_interface_speed = print_config.get_abs_value("support_interface_speed");
    double bridge_speed                     = print_config.opt_float("bridge_speed");
    double bridge_flow                = print_config.opt_float("bridge_flow");
    double inner_wall_speed                  = print_config.opt_float("inner_wall_speed");
    double outer_wall_speed         = print_config.get_abs_value("outer_wall_speed", inner_wall_speed);
    // double gap_infill_speed                   = print_config.opt_bool("filter_out_gap_fill") ? print_config.opt_float("gap_infill_speed") : 0.;
    double sparse_infill_speed                     = print_config.opt_float("sparse_infill_speed");
    double small_perimeter_speed            = print_config.get_abs_value("small_perimeter_speed", inner_wall_speed);
    double internal_solid_infill_speed               = print_config.opt_float("internal_solid_infill_speed");
    double top_surface_speed           = print_config.opt_float("top_surface_speed");
    // Maximum print speed when auto-speed is enabled by setting any of the above speed values to zero.
    double max_print_speed                  = print_config.opt_float("max_print_speed");
    // Maximum volumetric speed allowed for the print profile.
    double max_volumetric_speed             = print_config.opt_float("max_volumetric_speed");

    const auto &extrusion_width                     = *print_config.option<ConfigOptionFloatOrPercent>("line_width");
    const auto &outer_wall_line_width  = *print_config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
    const auto &initial_layer_line_width         = *print_config.option<ConfigOptionFloatOrPercent>("initial_layer_line_width");
    const auto &sparse_infill_line_width              = *print_config.option<ConfigOptionFloatOrPercent>("sparse_infill_line_width");
    const auto &inner_wall_line_width           = *print_config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
    const auto &internal_solid_infill_line_width        = *print_config.option<ConfigOptionFloatOrPercent>("internal_solid_infill_line_width");
    const auto& support_line_width    = *print_config.option<ConfigOptionFloatOrPercent>("support_line_width");
    const auto &top_surface_line_width          = *print_config.option<ConfigOptionFloatOrPercent>("top_surface_line_width");
    const auto &initial_layer_speed                   = *print_config.option<ConfigOptionFloatOrPercent>("initial_layer_speed");

    // Index of an extruder assigned to a feature. If set to 0, an active extruder will be used for a multi-material print.
    // If different from idx_extruder, it will not be taken into account for this hint.
    auto feature_extruder_active = [idx_extruder, num_extruders](int i) {
        return i <= 0 || i > num_extruders || idx_extruder == -1 || idx_extruder == i - 1;
    };
    bool perimeter_extruder_active                  = feature_extruder_active(print_config.opt_int("wall_filament"));
    bool infill_extruder_active                     = feature_extruder_active(print_config.opt_int("sparse_infill_filament"));
    bool solid_infill_extruder_active               = feature_extruder_active(print_config.opt_int("solid_infill_filament"));
    bool support_material_extruder_active           = feature_extruder_active(print_config.opt_int("support_filament"));
    bool support_material_interface_extruder_active = feature_extruder_active(print_config.opt_int("support_interface_filament"));

    // Current filament values
    double filament_diameter                = filament_config.opt_float("filament_diameter", 0);
    double filament_crossection             = M_PI * 0.25 * filament_diameter * filament_diameter;
    // double filament_flow_ratio             = filament_config.opt_float("filament_flow_ratio", 0);
    // The following value will be annotated by this hint, so it does not take part in the calculation.
//    double filament_max_volumetric_speed    = filament_config.opt_float("filament_max_volumetric_speed", 0);
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

    float   layer_height                        = float(print_config.opt_float("layer_height"));
    int     num_perimeters                      = print_config.opt_int("wall_loops");
    bool    thin_walls                          = print_config.opt_bool("detect_thin_wall");
    float   nozzle_diameter                     = float(printer_config.opt_float("nozzle_diameter", 0));
    
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
                *print_config.opt<ConfigOptionFloatOrPercent>("outer_wall_line_width"), 
                nozzle_diameter, layer_height);
            Flow perimeter_flow          = Flow::new_from_config_width(
                frPerimeter, 
                *print_config.opt<ConfigOptionFloatOrPercent>("inner_wall_line_width"), 
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

    int 	top_shell_layers                = print_config.opt_int("top_shell_layers");
    int 	bottom_shell_layers             = print_config.opt_int("bottom_shell_layers");
    bool    has_top_layers 					= top_shell_layers > 0;
    bool    has_bottom_layers 				= bottom_shell_layers > 0;
    double  top_shell_thickness        	= print_config.opt_float("top_shell_thickness");
    double  bottom_shell_thickness  	= print_config.opt_float("bottom_shell_thickness");
    double  layer_height                    = print_config.opt_float("layer_height");
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

// ============================================================================
// Magma live readouts
// ============================================================================
//
// Auto tube sizing derives the interior width and cell spacing and shows neither, and
// auto Z-slam makes the manual slam, the cone angle and (at a nonzero immersion budget)
// the contact press all inert. That combination once cost a full debugging session to
// discover, so the resolved numbers are printed here. Everything below resolves through
// the same magma:: helpers the slicer uses -- do not reimplement a formula in this file.

// Adapt the edited presets to the typed configs the one shared resolver takes. The GUI is
// the only consumer holding DynamicPrintConfigs, so the adaptation lives here rather than
// giving the resolver a second, weakly-typed entry point that could drift from the first.
static bool magma_resolve(const PresetBundle &preset_bundle, magma::MagmaResolved &out)
{
    const DynamicPrintConfig &print_config   = preset_bundle.prints  .get_edited_preset().config;
    const DynamicPrintConfig &printer_config = preset_bundle.printers.get_edited_preset().config;

    PrintRegionConfig region;  region.apply(print_config,   true);
    PrintObjectConfig object;  object.apply(print_config,   true);
    PrintConfig       printer; printer.apply(printer_config, true);
    if (printer.nozzle_diameter.values.empty())
        return false;

    return magma::resolve_magma(region, object, printer, out);
}

std::string PresetHints::magma_geometry_description(const PresetBundle &preset_bundle)
{
    magma::MagmaResolved m;
    if (! magma_resolve(preset_bundle, m))
        return std::string();

    const double open_area = m.geometry->inset_open_area(m.cell_spacing, m.line_width);
    const double open_pct  = m.cell_spacing > 0.0
                                 ? 100.0 * open_area / (m.cell_spacing * m.cell_spacing) : 0.0;

    std::string out = (boost::format(_utf8(L(
        "Tube interior %1$.3f mm, cell spacing %2$.3f mm, line width %3$.2f mm.\n"
        "Bore %4$.3f mm (the usable channel) — seal opening %5$.3f mm (what the nozzle must cover).\n"
        "Open cross-section %6$.3f mm2.")))
        % m.interior_width % m.cell_spacing % m.line_width % m.bore_diameter
        % m.opening_diameter % open_area).str();

    if (open_pct > 0.0)
        out += (boost::format(_utf8(L(" Roughly %1$.0f%% of the lattice footprint is open tube.")))
                % open_pct).str();

    // The nozzle flat is what the bore is measured against; make the ratio explicit since
    // it is the whole reason one pattern seals better than another.
    if (m.nozzle_flat > 0.0)
        out += (boost::format(_utf8(L("\nBore is %1$.0f%% of the %2$.2f mm nozzle flat.")))
                % (100.0 * m.bore_diameter / m.nozzle_flat) % m.nozzle_flat).str();
    return out;
}

std::string PresetHints::magma_injection_description(const PresetBundle &preset_bundle)
{
    magma::MagmaResolved m;
    if (! magma_resolve(preset_bundle, m))
        return std::string();

    const DynamicPrintConfig &print_config = preset_bundle.prints.get_edited_preset().config;

    std::string out = (boost::format(_utf8(L(
        "Z-slam %1$.3f mm + plunge %2$.3f mm = %3$.3f mm total nozzle intrusion.")))
        % m.slam_depth % m.plunge_depth % m.total_depth()).str();

    // Immersion is the only thing that grows a tube past the nozzle flat, and the only
    // thing that deforms one, so say where this sits against the budget.
    if (m.opening_diameter > m.nozzle_flat) {
        out += (boost::format(_utf8(L("\nThe opening is wider than the nozzle flat, so the nozzle "
                                      "enters the tube: %1$.3f mm of the %2$.3f mm immersion budget.")))
                % m.slam_depth % m.immersion_budget).str();
    } else {
        out += _utf8(L("\nThe nozzle flat covers the opening outright — it seats on the rim rather "
                       "than entering the tube."));
    }

    const double open_area   = m.geometry->inset_open_area(m.cell_spacing, m.line_width);
    const double tube_height = print_config.opt_float("magma_tube_height");
    const double fill_factor = print_config.opt_float("magma_tube_fill_factor");
    if (open_area > 0.0 && tube_height > 0.0)
        // A U-tube is a PAIR of cells sharing a window -- one injection fills both. Reporting
        // a single cell's volume understated this by ~2x. Still approximate: it ignores the
        // window cavity and any clipping at the part edge.
        out += (boost::format(_utf8(L("\nApproximately %1$.2f mm3 injected per full-height U-tube "
                                      "(2 cells x %2$.3f mm2 x %3$.1f mm x %4$.2f fill factor).")))
                % (2.0 * open_area * tube_height * fill_factor) % open_area % tube_height % fill_factor).str();
    return out;
}

}; // namespace Slic3r
