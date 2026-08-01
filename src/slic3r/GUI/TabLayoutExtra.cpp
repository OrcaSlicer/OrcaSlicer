// TabLayoutExtra.cpp
//
// TabFilament::build_extra_layout() - member function implementation.
//
// Supplements TabLayout_generated.cpp with UI sections that require custom
// widget factories (create_line_with_widget) and therefore cannot be
// auto-generated from the proto schema.
//
// Called from TabFilament::build() after TabFilament_build_main_layout():
//   1. Toolchange + ramming parameters sections (single + multi extruder)
//   2. Setting Overrides page  (checkbox-driven nullable retraction overrides)
//   3. Dependencies page  (compatible_printers and compatible_prints widgets)
//   4. Notes page

#include "Tab.hpp"
#include "GUI_App.hpp"                   // dots, wxGetApp()
#include "libslic3r/FlushVolCalc.hpp"   // g_max_flush_volume (needed by WipeTowerDialog)
#include "WipeTowerDialog.hpp"           // RammingDialog
#include "MsgDialog.hpp"                 // InfoDialog
#include "libslic3r/GCode/Thumbnails.hpp"
#include "format.hpp"

namespace Slic3r { namespace GUI {

void TabFilament::build_extra_layout()
{
    constexpr int notes_field_height = 25;

    // -- Continue the Multimaterial page ----------------------------------
    // TabFilament_build_main_layout() ended after the Multi Filament optgroup.
    // We continue on the same page by accessing m_pages.back().
    {
        PageShp page = m_pages.back();

        // Tool change parameters - single extruder MM printers
        // (includes filament_ramming_parameters which needs a custom button widget)
        {
            auto optgroup = page->new_optgroup(L("Tool change parameters with single extruder MM printers"), L"param_toolchange");
            optgroup->append_single_option_line("filament_loading_speed_start", "material_multimaterial#loading-speed-at-the-start");
            optgroup->append_single_option_line("filament_loading_speed", "material_multimaterial#loading-speed");
            optgroup->append_single_option_line("filament_unloading_speed_start", "material_multimaterial#unloading-speed-at-the-start");
            optgroup->append_single_option_line("filament_unloading_speed", "material_multimaterial#unloading-speed");
            optgroup->append_single_option_line("filament_toolchange_delay", "material_multimaterial#delay-after-unloading");
            optgroup->append_single_option_line("filament_cooling_moves", "material_multimaterial#number-of-cooling-moves");
            optgroup->append_single_option_line("filament_cooling_initial_speed", "material_multimaterial#speed-of-the-first-cooling-move");
            optgroup->append_single_option_line("filament_cooling_final_speed", "material_multimaterial#speed-of-the-last-cooling-move");
            optgroup->append_single_option_line("filament_stamping_loading_speed", "material_multimaterial#stamping-loading-speed");
            optgroup->append_single_option_line("filament_stamping_distance", "material_multimaterial#stamping-distance");
            create_line_with_widget(optgroup.get(), "filament_ramming_parameters",
                "material_multimaterial#ramming-parameters",
                [this](wxWindow* parent) {
                    Button* btn = new Button(parent, _(L("Set")) + " " + dots);
                    btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
                    auto sizer = new wxBoxSizer(wxHORIZONTAL);
                    sizer->Add(btn);
                    btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                        RammingDialog dlg(this,
                            (m_config->option<ConfigOptionStrings>("filament_ramming_parameters"))->get_at(0));
                        if (dlg.ShowModal() == wxID_OK) {
                            load_key_value("filament_ramming_parameters", dlg.get_parameters());
                            update_changed_ui();
                        }
                    });
                    return sizer;
                });
        }

        // Tool change parameters - multi extruder MM printers
        {
            auto optgroup = page->new_optgroup(L("Tool change parameters with multi extruder MM printers"), L"param_toolchange_multi_extruder");
            optgroup->append_single_option_line("filament_multitool_ramming", "material_multimaterial#tool-change-parameters-with-multi-extruder");
            optgroup->append_single_option_line("filament_multitool_ramming_volume", "material_multimaterial#multi-tool-ramming-volume");
            optgroup->append_single_option_line("filament_multitool_ramming_flow", "material_multimaterial#multi-tool-ramming-flow");
        }
    }

    // -- Setting Overrides page --------------------------------------------
    // Checkbox-driven nullable retraction overrides - complex runtime logic,
    // cannot be expressed in the proto schema.
    add_filament_overrides_page();

    // -- Dependencies page -------------------------------------------------
    // compatible_printers and compatible_prints require create_line_with_widget
    // and must come BEFORE the text condition inputs in each optgroup.
    {
        auto page = add_options_page(L("Dependencies"), "advanced");
        {
            auto optgroup = page->new_optgroup(L("Compatible printers"), L"param_dependencies_printers");
            create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
                return compatible_widget_create(parent, m_compatible_printers);
            });
            Option option = optgroup->get_option("compatible_printers_condition");
            option.opt.full_width = true;
            optgroup->append_single_option_line(option, "material_dependencies#compatible-printers");
        }
        {
            auto optgroup = page->new_optgroup(L("Compatible process profiles"), L"param_dependencies_presets");
            create_line_with_widget(optgroup.get(), "compatible_prints", "", [this](wxWindow* parent) {
                return compatible_widget_create(parent, m_compatible_prints);
            });
            Option option = optgroup->get_option("compatible_prints_condition");
            option.opt.full_width = true;
            optgroup->append_single_option_line(option, "material_dependencies#compatible-process-profiles");
        }
    }

    // -- Notes page --------------------------------------------------------
    {
        auto page = add_options_page(L("Notes"), "custom-gcode_note");
        auto optgroup = page->new_optgroup(L("Notes"), L"note", 0);
        Option option = optgroup->get_option("filament_notes");
        option.opt.full_width = true;
        option.opt.height = notes_field_height;
        optgroup->append_single_option_line(option);
    }
}

// -----------------------------------------------------------------------------
// TabPrinter::build_fff_extra_layout()
//
// Builds the "Basic information" page for the Printer/FFF tab.
// Kept here (not generated) because it contains:
//   - create_line_with_widget for printable_area (bed shape editor)
//   - m_on_change for the thumbnails field (thumbnail format sync logic)
//   - append_line for Cooling Fan multi-option line (fan_speedup_time + fan_speedup_overhangs)
//
// Called from TabPrinter::build_fff() before TabPrinter_build_gcode_layout().
// -----------------------------------------------------------------------------
// ── TabPrinter hook methods called by TabPrinter_build_basic_information_layout ──
// These implement the optgroups that cannot be auto-generated from yaml because they
// require custom widget factories, special m_on_change callbacks, or multi-option lines.

void TabPrinter::layout_hook_printable_space(ConfigOptionsGroup* optgroup)
{
    // Bed shape widget must be FIRST in the group
    create_line_with_widget(optgroup, "printable_area", "custom-svg-and-png-bed-textures_124612", [this](wxWindow* parent) {
        return create_bed_shape_widget(parent);
    });
    Option option = optgroup->get_option("bed_exclude_area");
    option.opt.full_width = true;
    optgroup->append_single_option_line(option, "printer_basic_information_printable_space#excluded-bed-area");
    optgroup->append_single_option_line("printable_height", "printer_basic_information_printable_space#printable-height");
    optgroup->append_single_option_line("support_multi_bed_types", "printer_basic_information_printable_space#support-multi-bed-types");
    optgroup->append_single_option_line("best_object_pos", "printer_basic_information_printable_space#best-object-position");
    optgroup->append_single_option_line("z_offset", "printer_basic_information_printable_space#z-offset");
    optgroup->append_single_option_line("preferred_orientation", "printer_basic_information_printable_space#preferred-orientation");
}

void TabPrinter::layout_hook_advanced(ConfigOptionsGroup* optgroup)
{
    optgroup->append_single_option_line("printer_structure", "printer_basic_information_advanced#printer-structure");
    optgroup->append_single_option_line("gcode_flavor", "printer_basic_information_advanced#g-code-flavor");
    optgroup->append_single_option_line("pellet_modded_printer", "printer_basic_information_advanced#pellet-modded-printer");
    optgroup->append_single_option_line("bbl_use_printhost", "printer_basic_information_advanced#use-3rd-party-print-host");
    optgroup->append_single_option_line("scan_first_layer", "printer_basic_information_advanced#scan-first-layer");
    optgroup->append_single_option_line("enable_power_loss_recovery", "printer_basic_information_advanced#power-loss-recovery");
    optgroup->append_single_option_line("disable_m73", "printer_basic_information_advanced#disable-set-remaining-print-time");
    {
        Option option = optgroup->get_option("thumbnails");
        option.opt.full_width = true;
        optgroup->append_single_option_line(option, "printer_basic_information_advanced#g-code-thumbnails");
    }
    // Thumbnail format sync — cannot be expressed in proto schema
    optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        wxTheApp->CallAfter([this, opt_key, value]() {
            if (opt_key == "thumbnails" && m_config->has("thumbnails_format")) {
                const std::string val = boost::any_cast<std::string>(value);
                if (!value.empty()) {
                    auto [thumbnails_list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(val);
                    if (errors != enum_bitmask<ThumbnailError>()) {
                        std::string error_str = format(_u8L("Invalid value provided for parameter %1%: %2%"), "thumbnails", val);
                        error_str += GCodeThumbnails::get_error_string(errors);
                        InfoDialog(parent(), _L("G-code flavor is switched"), from_u8(error_str)).ShowModal();
                    }
                    if (!thumbnails_list.empty()) {
                        GCodeThumbnailsFormat old_format = GCodeThumbnailsFormat(m_config->option("thumbnails_format")->getInt());
                        GCodeThumbnailsFormat new_format = thumbnails_list.begin()->first;
                        if (old_format != new_format) {
                            DynamicPrintConfig new_conf = *m_config;
                            auto* opt = m_config->option("thumbnails_format")->clone();
                            opt->setInt(int(new_format));
                            new_conf.set_key_value("thumbnails_format", opt);
                            load_config(new_conf);
                        }
                    }
                }
            }
            update_dirty();
            on_value_change(opt_key, value);
        });
    };
    optgroup->append_single_option_line("use_relative_e_distances", "printer_basic_information_advanced#use-relative-e-distances");
    optgroup->append_single_option_line("use_firmware_retraction", "printer_basic_information_advanced#use-firmware-retraction");
    optgroup->append_single_option_line("time_cost", "printer_basic_information_advanced#time-cost");
}

void TabPrinter::layout_hook_cooling_fan(ConfigOptionsGroup* optgroup)
{
    Line line = Line{ L("Fan speed-up time"), optgroup->get_option("fan_speedup_time").opt.tooltip };
    line.label_path = "printer_basic_information_cooling_fan#fan-speed-up-time";
    line.append_option(optgroup->get_option("fan_speedup_time"));
    line.append_option(optgroup->get_option("fan_speedup_overhangs"));
    optgroup->append_line(line);
    optgroup->append_single_option_line("fan_kickstart", "printer_basic_information_cooling_fan#fan-kick-start-time");
    optgroup->append_single_option_line("part_cooling_fan_min_pwm", "printer_basic_information_cooling_fan#minimum-non-zero-part-cooling-fan-speed");
}

// build_fff_extra_layout is kept for backward compatibility.
// It now delegates to the yaml-generated TabPrinter_build_basic_info_layout
// which calls the hook methods above.
void TabPrinter::build_fff_extra_layout()
{
    // Superseded: Tab.cpp now calls TabPrinter_build_basic_info_layout(*this) directly.
    // Hook methods below are called by that generated function.
}

} } // namespace Slic3r::GUI
