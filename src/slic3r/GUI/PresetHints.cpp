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
    bool perimeter_extruder_active                  = feature_extruder_active(print_config.opt_int("outer_wall_filament_id"))
                                                    && feature_extruder_active(print_config.opt_int("inner_wall_filament_id"));
    bool infill_extruder_active                     = feature_extruder_active(print_config.opt_int("sparse_infill_filament_id"));
    bool solid_infill_extruder_active               = feature_extruder_active(print_config.opt_int("internal_solid_filament_id"))
                                                    && feature_extruder_active(print_config.opt_int("top_surface_filament_id"))
                                                    && feature_extruder_active(print_config.opt_int("bottom_surface_filament_id"));
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
// Auto tube sizing derives the interior width and cell spacing and shows neither, the seal
// depth is derived from the tube rather than set, and the minimum seal depth is inert at any
// immersion the geometry already needs more than. That combination once cost a full debugging
// session to discover, so the resolved numbers are printed here. Everything below resolves through
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

// A readout row appears twice: terse inline ("Seal depth:  0.550 mm") and, in the tooltip,
// with the reason attached. The inline block sits between two settings fields, so anything
// longer than the value itself pushes the page around and reads like documentation; the
// explanation is still one hover away for whoever wants it.
namespace {
struct MagmaRows {
    std::string text;      // "Label:  value"
    std::string tooltip;   // "Label:  value — why"
    void add(const std::string &label, const std::string &value, const std::string &why = {})
    {
        text += label + ":  " + value + "\n";
        tooltip += label + ":  " + value;
        if (! why.empty())
            tooltip += " \u2014 " + why;
        tooltip += "\n";
    }
    PresetHints::MagmaReadout finish() const
    {
        auto trim = [](std::string v) {
            while (! v.empty() && v.back() == '\n') v.pop_back();
            return v;
        };
        return { trim(text), trim(tooltip) };
    }
};
} // namespace

PresetHints::MagmaReadout PresetHints::magma_geometry_readout(const PresetBundle &preset_bundle)
{
    magma::MagmaResolved m;
    if (! magma_resolve(preset_bundle, m))
        return {};

    const double open_area = m.geometry->inset_open_area(m.cell_spacing, m.line_width);
    const double open_pct  = m.cell_spacing > 0.0
                                 ? 100.0 * open_area / (m.cell_spacing * m.cell_spacing) : 0.0;
    auto mm = [](double v) { return (boost::format("%1$.3f mm") % v).str(); };

    MagmaRows r;
    // The bore-scaled stand-in is a guess, and every number below is derived from it. Saying so
    // first is the difference between a readout and a readout that misleads -- slicing refuses
    // in this state, so without the line the panel looks more confident than the slicer is.
    if (m.nozzle_flat_is_estimate)
        r.add(_utf8(L("Nozzle tip flat")),
              (boost::format(_utf8(L("%1$.2f mm  (ESTIMATED)"))) % m.nozzle_flat).str(),
              _utf8(L("not measured, so this is a guess from the bore and every figure below is "
                      "derived from it; slicing will refuse until you measure the flat")));
    r.add(_utf8(L("Tube interior")), mm(m.interior_width),
          (boost::format(_utf8(L("open width of one cell; cell spacing %1$.3f mm at %2$.2f mm line width")))
           % m.cell_spacing % m.line_width).str());
    r.add(_utf8(L("Usable bore")), mm(m.bore_diameter),
          m.nozzle_flat > 0.0
              ? (boost::format(_utf8(L("largest circle that fits inside the tube, %1$.0f%% of the %2$.2f mm nozzle flat")))
                 % (100.0 * m.bore_diameter / m.nozzle_flat) % m.nozzle_flat).str()
              : _utf8(L("largest circle that fits inside the tube")));
    r.add(_utf8(L("Seal opening")), mm(m.opening_diameter),
          _utf8(L("circle the nozzle must cover to seal; wider than the bore because the corners have to be covered too")));
    r.add(_utf8(L("Open cross-section")),
          (boost::format("%1$.3f mm\u00b2") % open_area).str(),
          open_pct > 0.0
              ? (boost::format(_utf8(L("%1$.0f%% of the lattice footprint is open tube"))) % open_pct).str()
              : std::string());
    return r.finish();
}

PresetHints::MagmaReadout PresetHints::magma_injection_readout(const PresetBundle &preset_bundle)
{
    magma::MagmaResolved m;
    if (! magma_resolve(preset_bundle, m))
        return {};

    const DynamicPrintConfig &print_config = preset_bundle.prints.get_edited_preset().config;
    auto mm = [](double v) { return (boost::format("%1$.3f mm") % v).str(); };

    // Every number here comes from magma::resolve_magma -- the same resolver MagmaTubeMap::build
    // and Print::validate use -- so this cannot describe a tube the slicer will not print.
    MagmaRows r;
    // A value the budget cut down is shown as what will happen, with what was asked for
    // beside it. Printing only the clamped number leaves the field and the readout quietly
    // disagreeing, which is how a setting that is not in effect reads as one that is.
    auto mm_clamped = [&mm](double effective, double requested) {
        return mm(effective) + (boost::format(_utf8(L("  (asked %1$.3f, capped)"))) % requested).str();
    };

    r.add(_utf8(L("Seal depth")), mm(m.seal_depth),
          m.opening_diameter > m.nozzle_flat
              ? _utf8(L("reached in one fast move before any filament flows: the opening is wider than the nozzle flat, so the nozzle descends until the cone covers it"))
              : _utf8(L("the flat already covers the opening, so the nozzle seats on the rim without entering the tube")));
    r.add(_utf8(L("Corner grip")), mm(m.grip),
          _utf8(L("what actually holds the seal shut: (press + plunge) x tan(cone angle). Both are depth past first contact -- one before the injection, one during it. The corner is the last part of the opening the cone covers, so it is the least pressed contact in the cell and the first to let go")));
    r.add(_utf8(L("Plunge")),
          m.plunge_clamped() ? mm_clamped(m.plunge_depth, m.plunge_requested) : mm(m.plunge_depth),
          ! print_config.opt_bool("magma_injection_plunge")
              ? _utf8(L("disabled"))
              : (m.plunge_clamped()
                     ? _utf8(L("the seal depth already spent most of the budget, leaving only this much room to sink further"))
                     : _utf8(L("the nozzle sinks this much further WHILE the tube fills, paced to the extrusion, keeping the seal shut as pressure builds"))));
    r.add(_utf8(L("Total immersion")), mm(m.total_depth()),
          _utf8(L("seal depth plus plunge -- the deepest the nozzle gets, reached as the last filament goes in and held through the dwell. A consequence of the two settings above, not a budget they are spent from")));
    r.add(_utf8(L("Nozzle vs cell pitch")),
          (boost::format("%1$.0f%%") % (100.0 * m.pitch_ratio())).str(),
          m.pitch_ratio() > magma::MAGMA_PITCH_ABSURD_RATIO
              ? _utf8(L("the cone covers an entire neighbouring cell before it touches the one it is sealing"))
              : _utf8(L("how much of the cell pitch the cone spans at seal depth. A geometry figure, not a quality one -- prints have been clean well past 100%; what actually damages the lattice is how LONG each injection takes")));

    const double open_area   = m.geometry->inset_open_area(m.cell_spacing, m.line_width);
    const double tube_height = print_config.opt_float("magma_tube_height");
    const double fill_factor = print_config.opt_float("magma_tube_fill_factor");
    if (open_area > 0.0 && tube_height > 0.0)
        // A U-tube is a PAIR of cells sharing a window -- one injection fills both. Estimate:
        // ignores the window cavity and part-edge clipping. The slicer measures the real cavity
        // from the deposited toolpath.
        r.add(_utf8(L("Injected per U-tube")),
              (boost::format("%1$.2f mm\u00b3") % (2.0 * open_area * tube_height * fill_factor)).str(),
              (boost::format(_utf8(L("estimate: 2 cells x %1$.3f mm\u00b2 x %2$.1f mm x %3$.2f fill factor. The slicer measures each tube's real cavity from the printed toolpath")))
               % open_area % tube_height % fill_factor).str());

    // The binding constraint, so it goes last where the eye lands.
    // filament_max_volumetric_speed lives in the FILAMENT preset, not the print preset.
    const double max_vol =
        preset_bundle.filaments.get_edited_preset().config.opt_float("filament_max_volumetric_speed", 0);
    const double secs    = magma::injection_seconds(open_area, tube_height, fill_factor, max_vol);
    if (secs > 0.0)
        r.add(_utf8(L("Injection time")),
              (boost::format("%1$.2f s") % secs).str(),
              secs > magma::MAGMA_MAX_INJECTION_SECONDS
                  ? (boost::format(_utf8(L("past the ~%1$.1f s the lattice survives -- the nozzle softens the surrounding walls and the cells deform. Reduce tube height, then width")))
                     % magma::MAGMA_MAX_INJECTION_SECONDS).str()
                  : (boost::format(_utf8(L("how long the nozzle stays sealed in each cell, at the filament's %1$.1f mm\u00b3/s. THE number that decides whether a print is clean: keep it under ~%2$.1f s")))
                     % max_vol % magma::MAGMA_MAX_INJECTION_SECONDS).str());
    return r.finish();
}

}; // namespace Slic3r
