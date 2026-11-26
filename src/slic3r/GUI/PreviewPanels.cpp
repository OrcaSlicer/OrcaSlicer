#include "PreviewPanels.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/utils.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libvgcode/include/ColorRange.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/AllPlatesStatsPanel.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendPanel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/PreviewPanelParts.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/PrintSummaryModel.hpp"
#include "slic3r/GUI/PrintSummaryPanel.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

namespace {

CanvasUi::Color ui_color(const ColorRGBA& c) { return CanvasUi::Color(c.r(), c.g(), c.b(), c.a()); }

CanvasUi::Color ui_color(const libvgcode::Color& c) { return CanvasUi::Color::from_bytes(c[0], c[1], c[2]); }

// The moves the legend lists, each with what the viewer counts it as, and whether it is shown by default.
struct LegendMove
{
    libvgcode::EOptionType option;
    libvgcode::EMoveType   move;
    bool                   shown_by_default;
};

constexpr std::array<LegendMove, 6> LEGEND_MOVES = {{{libvgcode::EOptionType::Travels, libvgcode::EMoveType::Travel, false},
                                                     {libvgcode::EOptionType::Wipes, libvgcode::EMoveType::Wipe, false},
                                                     {libvgcode::EOptionType::Retractions, libvgcode::EMoveType::Retract, false},
                                                     {libvgcode::EOptionType::Unretractions, libvgcode::EMoveType::Unretract, false},
                                                     {libvgcode::EOptionType::Seams, libvgcode::EMoveType::Seam, true},
                                                     {libvgcode::EOptionType::ToolChanges, libvgcode::EMoveType::ToolChange, false}}};

const LegendMove* legend_move(libvgcode::EOptionType option)
{
    for (const LegendMove& move : LEGEND_MOVES)
        if (move.option == option)
            return &move;
    return nullptr;
}

std::string move_name(libvgcode::EOptionType option)
{
    switch (option) {
    case libvgcode::EOptionType::Travels: return _u8L("Travel");
    case libvgcode::EOptionType::Wipes: return _u8L("Wipe");
    case libvgcode::EOptionType::Retractions: return _u8L("Retract");
    case libvgcode::EOptionType::Unretractions: return _u8L("Unretract");
    case libvgcode::EOptionType::Seams: return _u8L("Seams");
    default: return _u8L("Filament changes");
    }
}

std::string scale_unit(LegendMode mode)
{
    switch (mode) {
    case LegendMode::Speed:
    case LegendMode::Jerk: return _u8L("mm/s");
    case LegendMode::Acceleration: return _u8L("mm/s²");
    case LegendMode::Flow: return _u8L("mm³/s");
    case LegendMode::LineWidth:
    case LegendMode::LayerHeight: return _u8L("mm");
    case LegendMode::FanSpeed: return "%";
    case LegendMode::Temperature: return "°C";
    default: return {};
    }
}

bool use_inches() { return wxGetApp().app_config->get("use_inches") == "1"; }

} // namespace

LegendValues PreviewPanels::scan_legend_values(const LegendValuesKey& key, const LegendScale& scale)
{
    const libvgcode::EViewType type      = key.type;
    const size_t               time_mode = key.time_mode;
    const libvgcode::Interval  layers    = key.layers;
    const bool layer_times = type == libvgcode::EViewType::LayerTimeLinear || type == libvgcode::EViewType::LayerTimeLogarithmic;
    const ProfileLimits& limits = profile_limits();
    const bool flow = type == libvgcode::EViewType::VolumetricFlowRate || type == libvgcode::EViewType::ActualVolumetricFlowRate;
    LegendValueScan scan(scale, layer_times, layer_times ? limits.layer_time : flow ? limits.flow : std::vector<float>());
    if (layer_times) {
        const std::vector<float> times = m_gcode.m_viewer.get_layers_estimated_times();
        // A layer slows down for the filament it is printed with, taken as the one of its first extrusion.
        std::vector<int> extruder_of_layer(times.size(), -1);
        if (!limits.layer_time.empty())
            for (size_t i = 0; i < m_gcode.m_viewer.get_vertices_count(); ++i) {
                const libvgcode::PathVertex& v = m_gcode.m_viewer.get_vertex_at(i);
                if (v.is_extrusion() && v.layer_id < extruder_of_layer.size() && extruder_of_layer[v.layer_id] < 0)
                    extruder_of_layer[v.layer_id] = v.extruder_id;
            }
        for (size_t i = 0; i < times.size(); ++i)
            if (i >= layers[0] && i <= layers[1])
                scan.add(times[i], times[i], int(i) + 1, extruder_of_layer[i]);
    } else
        for (size_t i = 0; i < m_gcode.m_viewer.get_vertices_count(); ++i) {
            const libvgcode::PathVertex& v = m_gcode.m_viewer.get_vertex_at(i);
            if (v.layer_id < layers[0] || v.layer_id > layers[1])
                continue;
            if (const std::optional<float> value = m_gcode.m_viewer.get_range_value(v, type))
                scan.add(*value, v.times[time_mode], int(v.layer_id) + 1, int(v.extruder_id));
        }
    return scan.values();
}

void PreviewPanels::update_legend_values(LegendScale& scale)
{
    const libvgcode::EViewType type      = m_gcode.m_viewer.get_view_type();
    const size_t               time_mode = size_t(m_gcode.m_viewer.get_time_mode());
    const LegendValuesKey      key{m_gcode.m_last_result_id, m_gcode.m_viewer.get_vertices_count(), type, time_mode,
                                   m_gcode.m_viewer.is_option_visible(libvgcode::EOptionType::Travels), m_gcode.m_viewer.is_option_visible(libvgcode::EOptionType::Wipes),
                                   m_gcode.m_viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole::Custom), scale.min, scale.max,
                                   m_gcode.m_viewer.get_color_ranges_layers()};
    if (!(m_legend_values_key == key)) {
        m_legend_values_key = key;
        m_legend_values     = scan_legend_values(key, scale);
    }
    const bool flow_scale = type == libvgcode::EViewType::VolumetricFlowRate || type == libvgcode::EViewType::ActualVolumetricFlowRate;
    const bool time_scale = type == libvgcode::EViewType::LayerTimeLinear || type == libvgcode::EViewType::LayerTimeLogarithmic;
    if (flow_scale || time_scale) {
        std::vector<int> used;
        for (const uint8_t id : m_gcode.m_viewer.get_used_extruders_ids())
            used.push_back(int(id) + 1);
        scale.limits = legend_limits(flow_scale ? profile_limits().flow : profile_limits().layer_time, used);
    }
    legend_set_values(scale, m_legend_values);
    const libvgcode::ColorRange& range = m_gcode.m_viewer.get_color_range(type);
    for (LegendCategory& c : scale.categories)
        c.color = ui_color(range.get_color_at(c.value));
}

LegendModel PreviewPanels::legend_model()
{
    LegendModel                model;
    const libvgcode::EViewType type = m_gcode.m_viewer.get_view_type();
    model.imperial                  = use_inches();
    if (const std::optional<LegendModeChoice> choice = legend_mode_of(type))
        model.choice = *choice;
    model.tool_view = type == libvgcode::EViewType::Tool;

    add_legend_lines(model);
    add_mode_summaries(model);
    add_legend_scale(model);
    add_legend_filaments(model);
    return model;
}

void PreviewPanels::add_legend_lines(LegendModel& model) const
{
    const size_t                          time_mode  = size_t(m_gcode.m_viewer.get_time_mode());
    const PrintEstimatedStatistics::Mode& mode_stats = m_gcode.m_print_statistics.modes[time_mode];
    const float total_time = mode_stats.time > 0.0 ? float(mode_stats.time) : m_gcode.m_viewer.get_estimated_time();
    for (const libvgcode::EGCodeExtrusionRole role : m_gcode.m_viewer.get_extrusion_roles()) {
        if (role >= libvgcode::EGCodeExtrusionRole::COUNT)
            continue;
        LegendRow row;
        row.key   = int(role);
        row.color = ui_color(m_gcode.m_viewer.get_extrusion_role_color(role));
        row.name  = _u8L(ExtrusionEntity::role_to_string(convert(role)));
        row.time  = m_gcode.m_viewer.get_extrusion_role_estimated_time(role);
        row.share = total_time > 0.f ? row.time / total_time : 0.0;
        if (const auto it = m_gcode.m_print_statistics.used_filaments_per_role.find(convert(role)); it != m_gcode.m_print_statistics.used_filaments_per_role.end()) {
            row.filament_mm = it->second.first * 1000.0;
            row.grams       = it->second.second;
        }
        row.visible = m_gcode.m_viewer.is_extrusion_role_visible(role);
        if (!row.visible)
            ++model.hidden;
        model.rows.push_back(row);
    }
    for (const libvgcode::EOptionType option : m_gcode.m_viewer.get_options()) {
        const LegendMove* move = legend_move(option);
        if (move == nullptr)
            continue;
        LegendRow row;
        row.key     = int(option);
        row.move    = true;
        row.color   = ui_color(m_gcode.m_viewer.get_option_color(option));
        row.name    = move_name(option);
        row.visible = m_gcode.m_viewer.is_option_visible(option);
        if (move->shown_by_default && !row.visible)
            ++model.hidden;
        const size_t index = size_t(move->move);
        if (option == libvgcode::EOptionType::Travels) {
            row.time        = m_gcode.m_viewer.get_travels_estimated_time();
            row.distance_mm = m_gcode.m_print_statistics.total_travel_distance;
            row.count       = m_gcode.m_print_statistics.total_travel_moves;
        } else {
            row.time  = m_gcode.m_move_type_times[index][time_mode];
            row.count = m_gcode.m_move_type_counts[index];
            if (option == libvgcode::EOptionType::ToolChanges)
                // Toolchange delays are injected via synchronize() and are not attributed to ToolChange move vertices.
                row.time = m_gcode.m_print_statistics.total_filament_load_time + m_gcode.m_print_statistics.total_filament_unload_time + m_gcode.m_print_statistics.total_tool_change_time;
            else {
                const float seam_distance = m_gcode.m_print_statistics.total_seam_gap_distance + m_gcode.m_print_statistics.total_seam_scarf_distance;
                row.distance_mm = option == libvgcode::EOptionType::Seams && seam_distance > 0.f ? seam_distance : m_gcode.m_move_type_distances[index];
            }
        }
        row.share = total_time > 0.f ? row.time / total_time : 0.0;
        model.rows.push_back(row);
    }
    model.travel_shown = m_gcode.m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
}

void PreviewPanels::add_mode_summaries(LegendModel& model) const
{
    const LegendUnits units = translated_legend_units();
    for (size_t i = 0; i < LEGEND_MODE_COUNT; ++i) {
        const LegendMode   mode    = LegendMode(i);
        LegendModeSummary& summary = model.modes[i];
        if (mode == LegendMode::LineType || mode == LegendMode::Filament)
            continue;
        const libvgcode::EViewType   mode_type = legend_view_type(mode, mode == model.choice.mode ? model.choice.variant : 0);
        const libvgcode::ColorRange& range     = m_gcode.m_viewer.get_color_range(mode_type);
        const std::vector<float>     values    = range.get_values();
        const std::string            unit      = scale_unit(mode);
        const int                    decimals  = legend_value_decimals(mode);
        const auto text = [&](float v) { return mode == LegendMode::LayerTime ? legend_time(v, units) : legend_value(v, decimals); };
        if (values.empty() || range.get_range()[0] > range.get_range()[1])
            continue;
        summary.constant = values.size() == 1;
        summary.range    = summary.constant ? text(values.front()) : text(range.get_range()[0]) + " – " + text(range.get_range()[1]);
        if (mode != LegendMode::LayerTime && !unit.empty())
            summary.range += " " + unit;
    }
}

void PreviewPanels::add_legend_scale(LegendModel& model)
{
    const libvgcode::EViewType type = m_gcode.m_viewer.get_view_type();
    const libvgcode::ColorRange& range = m_gcode.m_viewer.get_color_range(type);
    if (!model.tool_view && model.choice.mode != LegendMode::LineType && model.choice.mode != LegendMode::Filament &&
        range.get_range()[0] <= range.get_range()[1]) {
        for (const libvgcode::Color& c : range.get_palette())
            model.scale.palette.push_back(ui_color(c));
        model.scale.min         = range.get_range()[0];
        model.scale.max         = range.get_range()[1];
        model.scale.logarithmic = range.get_type() == libvgcode::EColorRangeType::Logarithmic;
        model.scale.decimals    = legend_value_decimals(model.choice.mode);
        model.scale.unit        = scale_unit(model.choice.mode);
        model.scale.times       = model.choice.mode == LegendMode::LayerTime;
        update_legend_values(model.scale);
        model.scale.filter = m_range_filter;
        const bool whole = m_gcode.m_moves_slider->GetHigherValue() == m_gcode.m_moves_slider->GetMaxValue() &&
                           m_gcode.m_layers_slider->GetLowerValue() == m_gcode.m_layers_slider->GetMinValue() &&
                           m_gcode.m_layers_slider->GetHigherValue() == m_gcode.m_layers_slider->GetMaxValue();
        if (!whole) {
            const libvgcode::PathVertex& vertex = m_gcode.m_viewer.get_current_vertex();
            if (model.scale.times) {
                const std::vector<float> times = m_gcode.m_viewer.get_layers_estimated_times();
                if (vertex.layer_id < times.size())
                    model.scale.current = times[vertex.layer_id];
            } else
                model.scale.current = m_gcode.m_viewer.get_range_value(vertex, type);
        }
    }
}

void PreviewPanels::add_legend_filaments(LegendModel& model) const
{
    model.filaments = legend_filaments();
    model.isolated  = m_isolated_filament + 1;
    for (const LegendFilament& filament : model.filaments) {
        LegendTool tool;
        tool.number = filament.number;
        tool.color  = filament.color;
        tool.mm     = filament.mm[size_t(LegendPart::Model)];
        tool.grams  = filament.grams[size_t(LegendPart::Model)];
        model.tools.push_back(tool);
    }
}

std::vector<LegendFilament> PreviewPanels::legend_filaments() const
{
    std::vector<LegendFilament> filaments;
    const PresetBundle*  bundle      = wxGetApp().preset_bundle;
    const bool           from_bundle = !m_gcode.m_only_gcode_in_preview && bundle != nullptr;
    const std::vector<std::string>* types =
        m_gcode.m_only_gcode_in_preview ? nullptr : &wxGetApp().plater()->get_partplate_list().get_current_fff_print().config().filament_type.values;
    const libvgcode::PaletteWithAlpha& tool_colors = m_gcode.m_viewer.get_tool_colors();
    for (const uint8_t id : m_gcode.m_viewer.get_used_extruders_ids()) {
        if (id >= m_gcode.m_filament_diameters.size() || id >= m_gcode.m_filament_densities.size())
            continue;
        LegendFilament filament;
        filament.number      = int(id) + 1;
        const ColorRGBA color = m_gcode.shown_filament_color(libvgcode::convert(tool_colors[id]));
        filament.color       = ui_color(color);
        filament.translucent = m_gcode.m_viewer.is_translucent_filaments() && color.a() < 1.f;
        if (types != nullptr && id < types->size())
            filament.type = (*types)[id];
        if (from_bundle && id < bundle->filament_presets.size())
            filament.name = bundle->filament_presets[id];
        else if (id < m_gcode.m_settings_ids.filament.size())
            filament.name = m_gcode.m_settings_ids.filament[id];
        std::array<double, LEGEND_PART_COUNT> volumes{};
        add_filament_volumes(volumes, m_gcode.m_print_statistics, id);
        std::tie(filament.mm, filament.grams) = filament_parts(volumes, m_gcode.m_filament_diameters[id], m_gcode.m_filament_densities[id]);
        filaments.push_back(filament);
    }
    return filaments;
}

PrintSummaryModel PreviewPanels::print_summary_model() const
{
    PrintSummaryModel model;
    model.imperial         = use_inches();
    model.filaments        = legend_filaments();
    model.isolated         = m_isolated_filament + 1;
    model.filament_changes = m_gcode.m_print_statistics.total_filament_changes;
    model.tool_changes     = m_gcode.m_print_statistics.total_extruder_changes;
    add_nozzle_grouping(model);
    add_summary_events(model);
    add_file_profiles(model);
    add_summary_totals(model);
    return model;
}

void PreviewPanels::add_nozzle_grouping(PrintSummaryModel& model) const
{
    if (m_gcode.m_nozzle_nums > 1 && !m_gcode.m_only_gcode_in_preview) {
        const auto grouped = [](const std::vector<GCodeViewer::ExtruderFilament>& filaments) {
            std::vector<SummaryGroupedFilament> out;
            for (const GCodeViewer::ExtruderFilament& f : filaments) {
                ColorRGBA color;
                decode_color(f.hex_color, color);
                const bool sup = f.is_support_filament && (f.type == "PLA" || f.type == "PA" || f.type == "ABS");
                out.push_back({int(f.filament_id) + 1, ui_color(color), sup ? "Sup." + f.type : f.type});
            }
            return out;
        };
        const auto nozzle = [&](const std::string& title, const std::vector<GCodeViewer::ExtruderFilament>& filaments) {
            SummaryNozzle n;
            n.title = title;
            for (const GCodeViewer::ExtruderFilament& f : filaments) {
                n.numbers.push_back(int(f.filament_id) + 1);
                for (const LegendFilament& lf : model.filaments)
                    if (lf.number == int(f.filament_id) + 1)
                        n.grams += lf.total_grams();
            }
            return n;
        };
        model.nozzles = {nozzle(_u8L("Left nozzle"), m_gcode.m_left_extruder_filament), nozzle(_u8L("Right nozzle"), m_gcode.m_right_extruder_filament)};

        SummaryGrouping grouping;
        grouping.left  = grouped(m_gcode.m_left_extruder_filament);
        grouping.right = grouped(m_gcode.m_right_extruder_filament);
        summary_compare_grouping(grouping, wxGetApp().plater()->get_partplate_list().get_current_fff_print().statistics_by_extruder());
        model.grouping      = grouping;
    }
}

void PreviewPanels::add_summary_events(PrintSummaryModel& model) const
{
    const libvgcode::PaletteWithAlpha&    tool_colors = m_gcode.m_viewer.get_tool_colors();
    const std::vector<CustomGCode::Item>& items = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes :
                                                                           m_gcode.m_custom_gcode_per_print_z;
    std::vector<CanvasUi::Color> colors(tool_colors.size());
    for (size_t i = 0; i < tool_colors.size(); ++i)
        colors[i] = ui_color(m_gcode.shown_filament_color(libvgcode::convert(tool_colors[i])));
    model.events = summary_events(items, m_gcode.get_layers_zs(), m_gcode.m_viewer.get_layers_estimated_times(), std::move(colors),
                                  [this](const std::string& color) {
                                      ColorRGBA decoded;
                                      decode_color(color, decoded);
                                      return ui_color(m_gcode.shown_filament_color(decoded));
                                  });
}

void PreviewPanels::add_file_profiles(PrintSummaryModel& model) const
{
    if (m_gcode.m_only_gcode_in_preview) {
        SummaryProfiles profiles;
        profiles.printer = m_gcode.m_settings_ids.printer;
        profiles.print   = m_gcode.m_settings_ids.print;
        bool complete    = !m_gcode.m_settings_ids.filament.empty();
        for (const std::string& name : m_gcode.m_settings_ids.filament)
            complete &= !name.empty();
        if (complete)
            for (const uint8_t id : m_gcode.m_viewer.get_used_extruders_ids())
                if (id < m_gcode.m_settings_ids.filament.size())
                    profiles.filaments.push_back({int(id) + 1, m_gcode.m_settings_ids.filament[id]});
        if (!profiles.printer.empty() || !profiles.print.empty() || !profiles.filaments.empty())
            model.profiles = profiles;
    }
}

void PreviewPanels::add_summary_totals(PrintSummaryModel& model) const
{
    model.totals = summary_totals(model.filaments, wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics(),
                                  m_gcode.m_print_statistics, size_t(m_gcode.m_viewer.get_time_mode()), translated_legend_units());
    model.totals.stealth = m_gcode.m_viewer.get_time_mode() == libvgcode::ETimeMode::Stealth;
}

void PreviewPanels::bind()
{
    m_legend.load_settings(*wxGetApp().app_config);
    m_summary.load_settings(*wxGetApp().app_config);
    m_legend.save_settings = [](const LegendSettings& settings) { legend_save_settings(*wxGetApp().app_config, settings); };
    m_legend.collapse.save  = [](bool collapsed) { save_preview_panel_collapsed(*wxGetApp().app_config, PreviewPanel::Legend, collapsed); };
    m_summary.collapse.save = [](bool collapsed) { save_preview_panel_collapsed(*wxGetApp().app_config, PreviewPanel::Summary, collapsed); };
    bind_visibility();
    bind_focus();
    bind_summary();
}

void PreviewPanels::refresh_preview()
{
    m_gcode.update_moves_slider();
    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
    CanvasUi::Context::current()->request_frame();
}

void PreviewPanels::bind_visibility()
{
    m_legend.select_view_type = [this](libvgcode::EViewType type) {
        m_gcode.select_view_type(type);
        const std::string name = GCodeViewer::view_type_to_config_name(type);
        if (!name.empty())
            wxGetApp().app_config->set("preview_last_view_type", name);
        refresh_preview();
    };
    m_legend.toggle_row = [this](const LegendRow& row) {
        if (row.move)
            m_gcode.m_viewer.toggle_option_visibility(libvgcode::EOptionType(row.key));
        else
            m_gcode.m_viewer.toggle_extrusion_role_visibility(libvgcode::EGCodeExtrusionRole(row.key));
        refresh_preview();
    };
    m_legend.show_only_row = [this](const LegendRow& row) {
        if (!row.move)
            for (const libvgcode::EGCodeExtrusionRole role : m_gcode.m_viewer.get_extrusion_roles())
                if (m_gcode.m_viewer.is_extrusion_role_visible(role) != (int(role) == row.key))
                    m_gcode.m_viewer.toggle_extrusion_role_visibility(role);
        for (const LegendMove& move : LEGEND_MOVES)
            if (m_gcode.m_viewer.is_option_visible(move.option) != (row.move && int(move.option) == row.key))
                m_gcode.m_viewer.toggle_option_visibility(move.option);
        refresh_preview();
    };
    m_legend.show_all_line_types = [this]() {
        for (const libvgcode::EGCodeExtrusionRole role : m_gcode.m_viewer.get_extrusion_roles())
            if (!m_gcode.m_viewer.is_extrusion_role_visible(role))
                m_gcode.m_viewer.toggle_extrusion_role_visibility(role);
        refresh_preview();
    };
    m_legend.hide_all_moves = [this]() {
        for (const LegendMove& move : LEGEND_MOVES)
            if (m_gcode.m_viewer.is_option_visible(move.option))
                m_gcode.m_viewer.toggle_option_visibility(move.option);
        refresh_preview();
    };
    m_legend.show_all = [this]() {
        for (const libvgcode::EGCodeExtrusionRole role : m_gcode.m_viewer.get_extrusion_roles())
            if (!m_gcode.m_viewer.is_extrusion_role_visible(role))
                m_gcode.m_viewer.toggle_extrusion_role_visibility(role);
        for (const LegendMove& move : LEGEND_MOVES)
            if (m_gcode.m_viewer.is_option_visible(move.option) != move.shown_by_default)
                m_gcode.m_viewer.toggle_option_visibility(move.option);
        refresh_preview();
    };
    m_legend.show_travel = [this](bool shown) {
        if (m_gcode.m_viewer.is_option_visible(libvgcode::EOptionType::Travels) != shown)
            m_gcode.m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
        refresh_preview();
    };
}

void PreviewPanels::bind_focus()
{
    m_legend.hover_row = [this](const LegendRow& row) {
        m_path_hover.kind = row.move ? libvgcode::focus_kind(libvgcode::EOptionType(row.key)) : libvgcode::focus_kind(libvgcode::EGCodeExtrusionRole(row.key));
    };
    m_legend.hover_value            = [this](float lo, float hi) { m_path_hover.values = std::array<float, 2>{lo, hi}; };
    m_legend.scale_view.hover_range = [this](float lo, float hi) { m_path_hover.range = std::array<float, 2>{lo, hi}; };
    m_legend.scale_view.set_range   = [this](std::optional<std::array<float, 2>> range) {
        if (range)
            m_range_filter = range;
        else
            m_range_filter.reset();
        CanvasUi::Context::current()->request_frame();
    };
    const auto hover_filament  = [this](int number) { m_path_hover.filament = number - 1; };
    const auto isolate         = [this](int number) {
        m_isolated_filament = m_isolated_filament == number - 1 ? -1 : number - 1;
        CanvasUi::Context::current()->request_frame();
    };
    m_legend.hover_filament    = hover_filament;
    m_legend.isolate_filament  = isolate;
    m_summary.hover_filament   = hover_filament;
    m_summary.isolate_filament = isolate;
}

void PreviewPanels::bind_summary()
{
    m_summary.go_to_layer = [this](int layer) {
        const int value = std::max(0, layer - 1);
        if (m_gcode.m_layers_slider->GetLowerValue() > value)
            m_gcode.m_layers_slider->SetLowerValue(value);
        m_gcode.m_layers_slider->SetHigherValue(value);
        m_gcode.m_layers_slider->set_as_dirty();
        CanvasUi::Context::current()->request_frame();
    };
    m_summary.set_stealth = [this](bool stealth) {
        m_gcode.m_viewer.set_time_mode(stealth ? libvgcode::ETimeMode::Stealth : libvgcode::ETimeMode::Normal);
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
        CanvasUi::Context::current()->request_frame();
    };
    // The dialog runs its own event loop, which may paint this canvas, so it waits until the frame is over.
    m_summary.set_optimal_grouping = []() {
        wxGetApp().CallAfter([]() {
            MessageDialog msg_dlg(nullptr, _L("Automatically re-slice according to the optimal filament grouping, and the grouping results will be displayed after slicing."),
                                  wxEmptyString, wxOK | wxCANCEL);
            if (msg_dlg.ShowModal() == wxID_OK) {
                wxGetApp().plater()->get_partplate_list().get_curr_plate()->set_filament_map_mode(FilamentMapMode::fmmAutoForFlush);
                wxPostEvent(wxGetApp().plater(), SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
            }
        });
    };
    m_summary.regroup_filaments = []() {
        Plater*        plater = wxGetApp().plater();
        wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
        evt.SetEventObject(plater);
        evt.SetInt(1); // 1 means from gcode viewer
        wxPostEvent(plater, evt);
    };
    m_summary.open_grouping_wiki = []() { wxLaunchDefaultBrowser(wxString::FromUTF8("https://e.bambulab.com/t?c=mOkvsXkJ9pldGYp9")); };
}

const PreviewPanels::ProfileLimits& PreviewPanels::profile_limits()
{
    if (m_profile_limits.read && m_profile_limits.result == m_gcode.m_last_result_id)
        return m_profile_limits;
    m_profile_limits      = ProfileLimits();
    m_profile_limits.read = true;
    m_profile_limits.result = m_gcode.m_last_result_id;
    const auto floats = [](const ConfigOptionFloats* option) {
        return option == nullptr ? std::vector<float>() : std::vector<float>(option->values.begin(), option->values.end());
    };
    // A sliced plate has the settings it was sliced with; a G-code file opened alone loads its own into the presets.
    const Print& print = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    if (print.finished()) {
        m_profile_limits.flow       = floats(&print.config().filament_max_volumetric_speed);
        m_profile_limits.layer_time = floats(&print.config().slow_down_layer_time);
    } else {
        const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
        m_profile_limits.flow       = floats(config.option<ConfigOptionFloats>("filament_max_volumetric_speed"));
        m_profile_limits.layer_time = floats(config.option<ConfigOptionFloats>("slow_down_layer_time"));
    }
    return m_profile_limits;
}

void PreviewPanels::apply_palette()
{
    if (m_legend.settings().palette == m_palette)
        return;
    m_palette = m_legend.settings().palette;
    const libvgcode::Palette& palette = m_palette == LegendPalette::Perceptual ? libvgcode::PERCEPTUAL_RANGES_COLORS : libvgcode::DEFAULT_RANGES_COLORS;
    using libvgcode::EViewType;
    for (const EViewType type : {EViewType::Height, EViewType::Width, EViewType::Speed, EViewType::ActualSpeed, EViewType::FanSpeed, EViewType::Temperature,
                                 EViewType::PressureAdvance, EViewType::Acceleration, EViewType::Jerk, EViewType::VolumetricFlowRate,
                                 EViewType::ActualVolumetricFlowRate, EViewType::LayerTimeLinear, EViewType::LayerTimeLogarithmic})
        m_gcode.m_viewer.set_color_range_palette(type, palette);
    m_gcode.m_solid_model_colors.valid = false;
    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
    CanvasUi::Context::current()->request_frame();
}

void PreviewPanels::apply_ranges_layers()
{
    const libvgcode::Interval whole{0, std::numeric_limits<size_t>::max()};
    const libvgcode::Interval wanted = m_legend.settings().range == LegendRange::VisibleLayers ? m_gcode.m_viewer.get_layers_view_range() : whole;
    if (wanted == m_gcode.m_viewer.get_color_ranges_layers())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (wanted != m_ranges_layers) {
        m_ranges_layers       = wanted;
        m_ranges_layers_since = now;
    }
    // While the slider moves, the scales wait for it to rest: each change recolours every vertex. A new print takes its own at once.
    const auto settle     = std::chrono::milliseconds(150);
    const bool new_result = m_ranges_layers_result != m_gcode.m_last_result_id;
    if (!new_result && wanted != whole && now - m_ranges_layers_since < settle) {
        CanvasUi::Context::current()->request_frame_at(m_ranges_layers_since + settle);
        return;
    }
    m_gcode.m_viewer.set_color_ranges_layers(wanted);
    m_ranges_layers_result     = m_gcode.m_last_result_id;
    m_gcode.m_solid_model_colors.valid = false;
    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
    CanvasUi::Context::current()->request_frame();
}

void PreviewPanels::drop_path_focus()
{
    m_range_filter.reset();
    m_isolated_filament = -1;
    m_path_hover        = {};
}

void PreviewPanels::apply_path_focus()
{
    apply_palette();
    apply_ranges_layers();
    const std::vector<uint8_t> used = m_gcode.m_viewer.get_used_extruders_ids();
    if (!m_range_filter_shown)
        m_range_filter.reset();
    if ((!m_gcode.is_legend_shown() && !m_gcode.is_summary_shown()) ||
        (m_isolated_filament >= 0 && (used.size() < 2 || std::find(used.begin(), used.end(), uint8_t(m_isolated_filament)) == used.end())))
        m_isolated_filament = -1;
    if (!m_legend.settings().hover)
        m_path_hover = {};
    libvgcode::Focus focus;
    focus.kind     = m_path_hover.kind;
    focus.extruder = m_path_hover.filament >= 0 ? m_path_hover.filament : m_isolated_filament;
    const libvgcode::ColorRange& range = m_gcode.m_viewer.get_color_range(m_gcode.m_viewer.get_view_type());
    if (const std::optional<std::array<float, 2>> positions = legend_focus_positions(m_path_hover.values, m_path_hover.range, m_range_filter, range.get_range(),
                                                                                     [&range](float value) { return range.get_position_at(value); }))
        focus.range = *positions;
    if (focus == m_gcode.m_viewer.get_focus())
        return;
    m_gcode.m_viewer.set_focus(focus);
    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
    CanvasUi::Context::current()->request_frame();
}

void PreviewPanels::render_legend_panel(const PreviewOverlayLayout& layout)
{
    m_range_filter_shown = false;
    if (!m_gcode.is_legend_shown()) {
        m_legend.hide();
        m_gcode.panel_drawn(PreviewPanel::Legend, CanvasUi::Rect());
        return;
    }
    const PreviewPanelPlacement place = m_gcode.panel_place(PreviewPanel::Legend, layout);
    const LegendModel           model = legend_model();
    m_legend.render(place, place.bottom, model);
    m_range_filter_shown = legend_range_filter_shown(m_legend.collapse.collapsed(), model.scale);
    m_gcode.panel_drawn(PreviewPanel::Legend, m_legend.last_rect());
}

void PreviewPanels::render_summary_panel(const PreviewOverlayLayout& layout)
{
    if (!m_gcode.is_summary_shown()) {
        m_summary.hide();
        m_gcode.panel_drawn(PreviewPanel::Summary, CanvasUi::Rect());
        return;
    }
    const PreviewPanelPlacement place = m_gcode.panel_place(PreviewPanel::Summary, layout);
    m_summary.render(place, place.bottom, print_summary_model());
    m_gcode.panel_drawn(PreviewPanel::Summary, m_summary.last_rect());
}

void PreviewPanels::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show) const
{
    if (!show || gcode_result_list.empty())
        return;
    for (const GCodeProcessorResult* result : gcode_result_list)
        if (result->moves.empty())
            return;
    const std::vector<float>& diameters = gcode_result_list.front()->filament_diameters;
    const std::vector<float>& densities = gcode_result_list.front()->filament_densities;
    std::vector<ColorRGBA>    colors;
    decode_colors(wxGetApp().plater()->get_extruder_colors_from_plater_config(gcode_result_list.back()), colors);

    AllPlatesStats stats;
    stats.imperial = use_inches();
    std::map<size_t, std::array<double, LEGEND_PART_COUNT>> volumes;
    const size_t max_extruders_colors = wxGetApp().plater()->get_extruders_colors().size();
    for (PartPlate* plate : wxGetApp().plater()->get_partplate_list().get_nonempty_plate_list()) {
        const PrintEstimatedStatistics& ps = plate->get_slice_result()->print_statistics;
        for (size_t extruder_id : plate->get_extruders(true)) {
            extruder_id -= 1;
            // Skip stale/overflow extruder indices (e.g. from object assignments that outlived a
            // filament-count change) so downstream per-extruder lookups stay in range. Ported
            // from BambuStudio (STUDIO-15763).
            if (extruder_id >= max_extruders_colors)
                continue;
            add_filament_volumes(volumes[extruder_id], ps, extruder_id);
        }
        stats.time += float(ps.modes[size_t(m_gcode.m_viewer.get_time_mode())].time);
        Print* print = nullptr;
        plate->get_print((PrintBase**) &print, nullptr, nullptr);
        stats.cost += print->print_statistics().total_cost;
    }
    for (const auto& [id, v] : volumes) {
        if (id >= diameters.size() || id >= densities.size())
            continue;
        LegendFilament row;
        row.number = int(id) + 1;
        if (id < colors.size())
            row.color = ui_color(adjust_color_for_rendering(colors[id]));
        std::tie(row.mm, row.grams) = filament_parts(v, diameters[id], densities[id]);
        stats.rows.push_back(row);
    }
    const Size canvas = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
    GUI::render_all_plates_stats(stats, CanvasUi::Point(.5f * float(canvas.get_width()), .5f * float(canvas.get_height())));
}

PreviewPanels::PreviewPanels(GCodeViewer& preview) : m_gcode(preview) {}

void PreviewPanels::render(const PreviewOverlayLayout& layout)
{
    m_path_hover = {};
    render_legend_panel(layout);
    render_summary_panel(layout);
    apply_path_focus();
}

} // namespace GUI
} // namespace Slic3r
