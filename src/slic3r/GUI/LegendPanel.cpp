#include "LegendPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>


#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendScaleView.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/PreviewPanelParts.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/CanvasUi/Availability.hpp"
#include "slic3r/GUI/CanvasUi/Button.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/Collapsible.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Field.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/FontMeasure.hpp"
#include "slic3r/GUI/CanvasUi/Icons.hpp"
#include "slic3r/GUI/CanvasUi/Input.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Menu.hpp"
#include "slic3r/GUI/CanvasUi/MenuPopup.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/PanelBody.hpp"
#include "slic3r/GUI/CanvasUi/PanelCard.hpp"
#include "slic3r/GUI/CanvasUi/ScrollArea.hpp"
#include "slic3r/GUI/CanvasUi/Select.hpp"
#include "slic3r/GUI/CanvasUi/SettingsForm.hpp"
#include "slic3r/GUI/CanvasUi/Swatch.hpp"
#include "slic3r/GUI/CanvasUi/Table.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/ToggleGroup.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {
namespace GUI {

namespace {

std::string mode_name(LegendMode mode)
{
    switch (mode) {
    case LegendMode::LineType: return _u8L("Line Type");
    case LegendMode::Filament: return _u8L("Filament");
    case LegendMode::Speed: return _u8L("Speed");
    case LegendMode::Acceleration: return _u8L("Acceleration");
    case LegendMode::Jerk: return _u8L("Jerk");
    case LegendMode::Flow: return _u8L("Flow");
    case LegendMode::LineWidth: return _u8L("Line Width");
    case LegendMode::LayerHeight: return _u8L("Layer Height");
    case LegendMode::PressureAdvance: return _u8L("Pressure Advance");
    case LegendMode::FanSpeed: return _u8L("Fan Speed");
    case LegendMode::Temperature: return _u8L("Temperature");
    default: return _u8L("Layer Time");
    }
}

std::string group_name(LegendModeGroup group)
{
    switch (group) {
    // TRN A group of the preview legend's modes: line types and filaments.
    case LegendModeGroup::Categories: return _u8L_CONTEXT("Categories", "Legend modes");
    // TRN A group of the preview legend's modes: speed, acceleration and jerk.
    case LegendModeGroup::Motion: return _u8L_CONTEXT("Motion", "Legend modes");
    case LegendModeGroup::Extrusion: return _u8L_CONTEXT("Extrusion", "Legend modes");
    case LegendModeGroup::CoolingAndHeat: return _u8L_CONTEXT("Cooling and heat", "Legend modes");
    // TRN A group of the preview legend's modes: the time of layers.
    default: return _u8L_CONTEXT("Time", "Legend modes");
    }
}

std::vector<std::string> variant_names(LegendMode mode)
{
    if (mode == LegendMode::LayerTime)
        // TRN The scale of layer times in the preview legend: linear or logarithmic.
        return {_u8L_CONTEXT("Linear", "Legend"), _u8L_CONTEXT("Log", "Legend")};
    // TRN Speeds or flows in the preview legend: as the G-code asks for them, or as the printer reaches them.
    return {_u8L_CONTEXT("Planned", "Legend"), _u8L_CONTEXT("Actual", "Legend")};
}

std::string layers_text(int first, int last)
{
    if (first == last)
        // TRN Where a value of the preview legend occurs: on one layer.
        return format(_u8L_CONTEXT("layer %1%", "Legend"), first);
    // TRN Where a value of the preview legend occurs: on a range of layers.
    return format(_u8L_CONTEXT("layers %1%–%2%", "Legend"), first, last);
}

} // namespace

struct LegendPanel::Sizes
{
    explicit Sizes(const CanvasUi::Metrics& metrics) : m(metrics) {}

    const CanvasUi::Metrics& m;
    float inset{m.px(CanvasUi::space<4>())};
    float time_col{m.px(CanvasUi::space<52>())};
    float share_col{m.px(CanvasUi::space<36>())};
    float last_col{m.px(CanvasUi::space<54>())};
    float layers_col{m.px(CanvasUi::space<104>())};
    float mode_menu_width{m.px(CanvasUi::space<340>())};
};

class LegendPanel::Body : public CanvasUi::PanelBody
{
public:
    Body(LegendPanel& panel, const Sizes& s, const LegendModel& model, const CanvasUi::Painter& paint, float left, float top, float width)
        : PanelBody(paint, left, top, width), m_panel(panel), m_s(s), m_model(model), m_units(translated_legend_units())
    {}

    void render()
    {
        if (m_model.tool_view)
            tools();
        else
            switch (m_model.choice.mode) {
            case LegendMode::LineType: line_types(); break;
            case LegendMode::Filament: filaments(); break;
            default: scale(); break;
            }
    }

private:
    LegendPanel&       m_panel;
    const Sizes&       m_s;
    const LegendModel& m_model;
    LegendUnits        m_units;

    std::string mass(double grams) const { return legend_mass_value(grams, m_model.imperial); }

    std::string row_tooltip(const LegendRow& row) const
    {
        std::string text = row.name;
        if (!row.move)
            // TRN The filament a line type of the preview legend takes: its length and its mass.
            text += "\n" + format(_u8L_CONTEXT("Filament: %1%, %2%", "Legend"), legend_filament_length(row.filament_mm, m_model.imperial, m_units),
                                  legend_mass(row.grams, m_model.imperial, m_units));
        if (row.time > 0.f)
            // TRN The time a line type or a move takes in the preview legend, and its share of the print time.
            text += "\n" + format(_u8L_CONTEXT("Time: %1%, %2% of the print", "Legend"), legend_time(row.time, m_units), legend_percent(row.share) + "%");
        if (row.move) {
            if (row.distance_mm)
                text += "\n" + format(_u8L_CONTEXT("Distance: %1%", "Legend"), legend_distance(*row.distance_mm, m_model.imperial, m_units));
            text += "\n" + format(_u8L_CONTEXT("Count: %1%", "Legend"), legend_count(row.count));
        }
        // TRN How a row of the preview legend's line types acts on a click.
        return text + "\n\n" + _u8L_CONTEXT("Click hides or shows it, Alt+click shows only it.", "Legend");
    }

    void line_type_row(const LegendRow& row)
    {
        CanvasUi::TableRowOptions options;
        options.width  = width();
        options.swatch = CanvasUi::Swatch{row.color};
        const LegendSettings& shown = m_panel.m_settings;
        options.cells = {{row.name, 0.f, CanvasUi::CellAlign::Left}};
        if (shown.time)
            options.cells.push_back({row.time > 0.f ? legend_time(row.time, m_units) : std::string(), m_s.time_col});
        if (shown.share)
            options.cells.push_back({row.time > 0.f ? legend_percent(row.share) : std::string(), m_s.share_col, CanvasUi::CellAlign::Right, true});
        if (shown.weight)
            options.cells.push_back({mass(row.grams), m_s.last_col});
        if (shown.length)
            options.cells.push_back({legend_length_value(row.filament_mm, m_model.imperial), m_s.last_col});
        options.dimmed  = !row.visible;
        options.tooltip = row_tooltip(row);
        place();
        const CanvasUi::ScopedId id{row.key};
        act_on_row(row, CanvasUi::table_row("##line", options));
        advance(metrics().table_row);
    }

    void act_on_row(const LegendRow& row, const CanvasUi::TableRowResult& result)
    {
        if (result.hovered && row.visible && m_panel.hover_row)
            m_panel.hover_row(row);
        if (result.alt_clicked && m_panel.show_only_row)
            m_panel.show_only_row(row);
        else if (result.clicked && m_panel.toggle_row)
            m_panel.toggle_row(row);
        if (result.context) {
            m_panel.m_menu_row     = row;
            m_panel.m_menu_pos     = CanvasUi::pointer().pos;
            m_panel.m_menu_pending = true;
        }
    }

    void line_types()
    {
        const LegendSettings& shown = m_panel.m_settings;
        const std::string     time  = _u8L_CONTEXT("Time", "Legend");
        const auto columns = [&](const std::string& title) {
            std::vector<CanvasUi::TableCell> cells{{title, 0.f, CanvasUi::CellAlign::Left}};
            if (shown.time)
                cells.push_back({time, m_s.time_col});
            if (shown.share)
                cells.push_back({"%", m_s.share_col});
            if (shown.weight)
                cells.push_back({m_model.imperial ? _u8L_CONTEXT("oz", "Legend unit") : _u8L_CONTEXT("g", "Legend unit"), m_s.last_col});
            if (shown.length)
                cells.push_back({m_model.imperial ? _u8L_CONTEXT("in", "Legend unit") : _u8L_CONTEXT("m", "Legend unit"), m_s.last_col});
            return cells;
        };
        table_header(columns(_u8L_CONTEXT("Line type", "Legend")), metrics().table_swatch);
        for (const LegendRow& row : m_model.rows)
            if (!row.move)
                line_type_row(row);
        moves();
    }

    void variant_row()
    {
        const LegendMode mode       = m_model.choice.mode;
        const float      row_height = metrics().control_height;
        const bool       variants   = legend_mode_has_variants(mode);
        const bool       travel     = mode == LegendMode::Speed || mode == LegendMode::Acceleration || mode == LegendMode::Jerk;
        const std::vector<std::string> names = variants ? variant_names(mode) : std::vector<std::string>();
        if (variants) {
            int selected = m_model.choice.variant;
            place();
            if (CanvasUi::toggle_group("##variant", selected, names) && m_panel.select_view_type)
                m_panel.select_view_type(legend_view_type(mode, selected));
        }
        if (!travel) {
            if (variants)
                advance(row_height + metrics().item_gap);
            return;
        }
        bool                   shown = m_model.travel_shown;
        CanvasUi::FieldOptions field;
        // TRN A switch of the preview legend: whether travel moves are drawn and count in the scale.
        field.label = _u8L_CONTEXT("Include travel", "Legend");
        const float right    = PanelBody::right() - metrics().table_padding_right;
        const float switch_w = CanvasUi::field_switch_width(metrics(), CanvasUi::FontMeasure(), field.label);
        const float group_w  = variants ? CanvasUi::toggle_group_width(metrics(), CanvasUi::ChoiceMeasure(), names) + metrics().field_gap : 0.f;
        if (m_model.scale.spread == LegendSpread::Scale && left() + group_w + switch_w <= right) {
            field.width = switch_w;
            CanvasUi::place(CanvasUi::Point(right - switch_w, y() + .5f * (row_height - metrics().field_height)));
            if (CanvasUi::field_switch("##travel", shown, field) && m_panel.show_travel)
                m_panel.show_travel(shown);
            advance(row_height + metrics().item_gap);
            return;
        }
        if (variants)
            advance(row_height + metrics().item_gap);
        const float inner = width() - metrics().table_padding_left - metrics().table_padding_right;
        CanvasUi::place(CanvasUi::Point(left() + metrics().table_padding_left, y()));
        field.width = inner;
        if (CanvasUi::field_switch("##travel", shown, field) && m_panel.show_travel)
            m_panel.show_travel(shown);
        advance(CanvasUi::field_switch_height(metrics(), CanvasUi::FontMeasure(), field.label, inner) + metrics().item_gap);
    }

    std::string value_text(float value) const
    {
        const LegendScale& scale = m_model.scale;
        return legend_with_unit(scale, legend_scale_value(scale, value, scale.decimals, m_units));
    }

    void one_value()
    {
        const LegendScale& scale = m_model.scale;
        const float        row   = metrics().table_row;
        const float        side  = metrics().table_swatch;
        const float        x     = left() + metrics().table_padding_left;
        const float        sy    = std::round(y() + .5f * (row - side));
        const CanvasUi::Color color = scale.categories.empty() ? (scale.palette.empty() ? theme().foreground : scale.palette.front())
                                                                : scale.categories.front().color;
        CanvasUi::draw_swatch(painter(), CanvasUi::Rect(x, sy, x + side, sy + side), CanvasUi::Swatch{color}, theme().card);
        const float       text_x = x + side + metrics().table_swatch_gap;
        const std::string value  = value_text(scale.categories.empty() ? scale.min : scale.categories.front().value);
        const CanvasUi::Font bold = ui().bold_font();
        painter().text(bold, CanvasUi::Rect(text_x, y(), right(), y() + row), theme().foreground, value);
        // TRN After the only value of a mode in the preview legend, as "9.0 mm/s on every move".
        painter().text(ui().regular_font(), CanvasUi::Rect(text_x + bold.width(value) + metrics().inline_gap, y(), right(), y() + row),
                       theme().foreground, _u8L_CONTEXT("on every move", "Legend"));
        advance(row);
        const CanvasUi::Font caption = ui().caption_font();
        const std::string note = m_panel.m_settings.range == LegendRange::VisibleLayers ?
                                     // TRN Under the only value of a mode in the preview legend, when its range is the visible layers.
                                     _u8L_CONTEXT("Constant in the visible layers, so there is no scale", "Legend") :
                                     _u8L_CONTEXT("Constant in this file, so there is no scale", "Legend");
        painter().text(caption, CanvasUi::Rect(text_x, y(), right(), y() + metrics().caption_line), theme().muted_foreground, note);
        advance(metrics().caption_line);
    }

    void few_values()
    {
        const std::string& unit  = m_model.scale.unit;
        const std::string  value = unit.empty() || m_model.scale.times ? _u8L_CONTEXT("Value", "Legend") :
                                                                          // TRN The heading of the values column of the preview legend with their unit, as "Value, °C".
                                                                          format(_u8L_CONTEXT("Value, %1%", "Legend"), unit);
        table_header({{value, 0.f, CanvasUi::CellAlign::Left},
                {_u8L_CONTEXT("Layers", "Legend"), m_s.layers_col},
                {_u8L_CONTEXT("Time", "Legend"), m_s.time_col},
                {"%", m_s.share_col}},
               metrics().table_swatch);
        int index = 0;
        for (const LegendCategory& category : m_model.scale.categories) {
            CanvasUi::TableRowOptions row;
            row.width       = width();
            row.swatch      = CanvasUi::Swatch{category.color};
            row.cells       = {{legend_scale_value(m_model.scale, category.value, m_model.scale.category_decimals, m_units), 0.f,
                                CanvasUi::CellAlign::Left},
                               {layers_text(category.first_layer, category.last_layer), m_s.layers_col},
                               {legend_time(category.time, m_units), m_s.time_col},
                               {legend_percent(category.share), m_s.share_col, CanvasUi::CellAlign::Right, true}};
            row.interactive = false;
            place();
            CanvasUi::ScopedId id(index++);
            if (CanvasUi::table_row("##value", row).hovered && m_panel.hover_value)
                m_panel.hover_value(category.lowest, category.highest);
            advance(metrics().table_row);
        }
    }

    void scale()
    {
        if (m_model.scale.palette.empty())
            return;
        variant_row();
        switch (m_model.scale.spread) {
        case LegendSpread::One: one_value(); break;
        case LegendSpread::Few: few_values(); break;
        default: m_panel.scale_view.render(*this, m_model, m_units, m_panel.m_settings.histogram); break;
        }
    }

    void filaments()
    {
        table_header({{_u8L("Filament"), 0.f, CanvasUi::CellAlign::Left},
                      {m_model.imperial ? _u8L_CONTEXT("oz", "Legend unit") : _u8L_CONTEXT("g", "Legend unit"), m_s.last_col}},
                     metrics().table_swatch_numbered);
        FilamentRowOptions row;
        row.isolated = m_model.isolated;
        row.several  = m_model.filaments.size() > 1;
        for (const LegendFilament& filament : m_model.filaments) {
            row.mass    = {mass(filament.total_grams()), m_s.last_col};
            row.details = legend_mass(filament.total_grams(), m_model.imperial, m_units) + ", " +
                          legend_filament_length(filament.total_mm(), m_model.imperial, m_units);
            filament_row(*this, filament, row, m_panel.hover_filament, m_panel.isolate_filament);
        }
        moves();
    }

    // The moves as toggles in a flow, as a chart's legend lists its series: they are mostly shown or hidden, and their time and
    // count are in the tooltip.
    void moves()
    {
        gap();
        section_title(_u8L_CONTEXT("Moves", "Legend"));
        const CanvasUi::FontMeasure measure;
        const float                 row_height = metrics().table_row;
        const float                 right      = PanelBody::right();
        float                       x          = left();
        bool                        any        = false;
        for (const LegendRow& row : m_model.rows) {
            if (!row.move)
                continue;
            const float w = metrics().table_padding_left + metrics().table_swatch + metrics().table_swatch_gap + std::ceil(measure.width(row.name)) +
                            metrics().table_padding_right;
            if (any && x + w > right) {
                x = left();
                advance(row_height);
            }
            CanvasUi::TableRowOptions toggle;
            toggle.width   = w;
            toggle.swatch  = CanvasUi::Swatch{row.color};
            toggle.cells   = {{row.name, 0.f, CanvasUi::CellAlign::Left}};
            toggle.dimmed  = !row.visible;
            toggle.tooltip = row_tooltip(row);
            CanvasUi::place(CanvasUi::Point(x, y()));
            const CanvasUi::ScopedId id{row.key + 1000};
            act_on_row(row, CanvasUi::table_row("##move", toggle));
            x += w + metrics().inline_gap;
            any = true;
        }
        if (any)
            advance(row_height);
    }

    void tools()
    {
        section_title(_u8L("Filament"), _u8L_CONTEXT("weight · length", "Legend"));
        int index = 0;
        for (const LegendTool& tool : m_model.tools) {
            CanvasUi::TableRowOptions row;
            row.width       = width();
            row.swatch      = CanvasUi::Swatch{tool.color};
            row.cells       = {{format(_u8L_CONTEXT("Extruder %1%", "Legend"), tool.number), 0.f, CanvasUi::CellAlign::Left},
                               {legend_mass(tool.grams, m_model.imperial, m_units), m_s.last_col + m_s.share_col},
                               {legend_filament_length(tool.mm, m_model.imperial, m_units), m_s.last_col + m_s.share_col}};
            row.interactive = false;
            place();
            CanvasUi::ScopedId id(index++);
            CanvasUi::table_row("##tool", row);
            advance(metrics().table_row);
        }
    }

};

void LegendPanel::load_settings(const AppConfig& config)
{
    collapse.load(load_preview_panel_collapsed(config, PreviewPanel::Legend));
    m_settings = legend_load_settings(config);
}

CanvasUi::SettingsFormOptions LegendPanel::settings_form(float width)
{
    CanvasUi::SettingsFormOptions form;
    form.width        = width;
    const auto column = [&](const char* id, const std::string& label, bool& value) {
        CanvasUi::SettingsField field = CanvasUi::settings_switch(id, label, value);
        field.availability = [this, &value]() {
            const int columns = int(m_settings.time) + int(m_settings.share) + int(m_settings.weight) + int(m_settings.length);
            if (columns == 1 && value)
                // TRN Why the last column of the preview legend's line types cannot be turned off.
                return CanvasUi::Availability{false, _u8L_CONTEXT("The line types keep at least one column", "Legend")};
            return CanvasUi::Availability();
        };
        form.fields.push_back(std::move(field));
    };
    column("##col_time", _u8L_CONTEXT("Time column", "Legend"), m_settings.time);
    column("##col_share", _u8L_CONTEXT("Share column", "Legend"), m_settings.share);
    column("##col_weight", _u8L_CONTEXT("Weight column", "Legend"), m_settings.weight);
    column("##col_length", _u8L_CONTEXT("Length column", "Legend"), m_settings.length);
    // TRN A setting of the preview legend: bars of the print time at each value above the bar of a scale.
    form.fields.push_back(CanvasUi::settings_switch("##histogram", _u8L_CONTEXT("Histogram on scales", "Legend"), m_settings.histogram));
    // TRN A setting of the preview legend: a row under the mouse keeps its paths in colour in the 3D view and hides the rest.
    form.fields.push_back(CanvasUi::settings_switch("##hover", _u8L_CONTEXT("Highlight in 3D on hover", "Legend"), m_settings.hover));
    CanvasUi::SettingsField range;
    range.id = "##range_setting";
    // TRN A setting of the preview legend: what the scale of a mode spans, the whole print or the layers the slider shows.
    range.label   = _u8L_CONTEXT("Scale range", "Legend");
    range.options = {_u8L_CONTEXT("Whole print", "Legend"), _u8L_CONTEXT("Visible layers", "Legend")};
    range.get     = [this]() { return int(m_settings.range); };
    range.set     = [this](int option) { m_settings.range = LegendRange(option); };
    form.fields.push_back(std::move(range));
    CanvasUi::SettingsField palette;
    palette.id = "##palette";
    // TRN A setting of the preview legend: the colours of the scales; perceptual ones read evenly, also to colour-blind eyes.
    palette.label   = _u8L_CONTEXT("Palette", "Legend");
    palette.options = {_u8L_CONTEXT("Classic", "Legend"), _u8L_CONTEXT("Perceptual", "Legend")};
    palette.get     = [this]() { return int(m_settings.palette); };
    palette.set     = [this](int option) { m_settings.palette = LegendPalette(option); };
    form.fields.push_back(std::move(palette));
    form.changed = legend_changed_settings(m_settings);
    // TRN The number of settings of the preview legend that differ from their defaults.
    form.changed_count = format(_u8L_CONTEXT("Changed: %1%", "Legend"), form.changed);
    form.reset_label   = _u8L_CONTEXT("Reset", "Legend");
    form.reset         = [this]() { m_settings = LegendSettings(); };
    return form;
}

void LegendPanel::render_header(const CanvasUi::PanelCard& frame, const Sizes& s, const LegendModel& model)
{
    const CanvasUi::Context& ui     = *CanvasUi::Context::current();
    const CanvasUi::Metrics& m      = ui.metrics();
    const CanvasUi::Card&    card   = frame.card();
    const CanvasUi::Point    origin = card.rect().min();
    const float              header = frame.header();
    const float              button = m.panel_header_button;
    const std::vector<CanvasUi::RowSpan> slots =
        CanvasUi::row_layout(origin.x + m.inline_gap, origin.x + frame.width() - m.inline_gap, m.inline_gap,
                             {{button}, {0.f, true}, {CanvasUi::button_width(m, CanvasUi::FontMeasure(), "88", CanvasUi::ButtonSize::Sm, true)}, {button}});
    const float button_y = origin.y + .5f * (header - button);

    CanvasUi::ButtonOptions icon;
    icon.size  = CanvasUi::ButtonSize::Icon;
    icon.width = button;

    if (frame.collapse_button("##legend_collapse", collapse.collapsed(), collapse.collapsed() ? _u8L_CONTEXT("Expand", "Legend") : _u8L_CONTEXT("Collapse", "Legend")))
        collapse.toggle_next_frame();

    std::vector<CanvasUi::MenuItem> items;
    std::optional<LegendModeGroup>  group;
    for (size_t i = 0; i < LEGEND_MODE_COUNT; ++i) {
        const LegendMode mode = LegendMode(i);
        if (group != legend_mode_group(mode)) {
            group = legend_mode_group(mode);
            items.push_back(CanvasUi::MenuItem::group_label(group_name(*group)));
        }
        const LegendModeSummary& summary = model.modes[i];
        CanvasUi::MenuItem       item    = CanvasUi::MenuItem::item(int(i), mode_name(mode));
        item.value                       = summary.range;
        item.muted                       = summary.constant;
        item.selected                    = !model.tool_view && model.choice.mode == mode;
        items.push_back(std::move(item));
    }
    CanvasUi::SelectOptions select;
    select.width      = slots[1].width;
    select.menu_width = s.mode_menu_width;
    const std::string current = model.tool_view ? _u8L("Tool") : mode_name(model.choice.mode);
    CanvasUi::place(CanvasUi::Point(slots[1].left, origin.y + .5f * (header - m.control_height)));
    if (const std::optional<int> picked = CanvasUi::select("##legend_mode", current, items, select)) {
        const LegendMode mode    = LegendMode(*picked);
        const int        variant = mode == model.choice.mode ? model.choice.variant : 0;
        collapse.set(false);
        if (select_view_type)
            select_view_type(legend_view_type(mode, variant));
    }

    // The count of what is hidden stays in the header, so that hiding a line type moves nothing under it.
    CanvasUi::ButtonOptions hidden;
    hidden.variant = CanvasUi::ButtonVariant::Ghost;
    hidden.size    = CanvasUi::ButtonSize::Sm;
    hidden.width   = slots[2].width;
    hidden.icon    = CanvasUi::Icon::EyeOff;
    if (model.hidden > 0)
        // TRN The tooltip of the preview legend's count of line types and moves hidden; a click shows them.
        hidden.tooltip = format(_u8L_CONTEXT("Hidden: %1%", "Legend"), model.hidden) + "\n" + _u8L_CONTEXT("Click shows them all again.", "Legend");
    else
        // TRN Why the preview legend's count of hidden line types cannot be pressed.
        hidden.availability = CanvasUi::Availability{false, _u8L_CONTEXT("Nothing is hidden", "Legend")};
    CanvasUi::place(CanvasUi::Point(slots[2].left, origin.y + .5f * (header - CanvasUi::button_height(m, CanvasUi::ButtonSize::Sm))));
    if (CanvasUi::button("##legend_hidden", model.hidden > 0 ? std::to_string(model.hidden) : std::string(), hidden) && show_all)
        show_all();

    CanvasUi::ButtonOptions settings = icon;
    settings.icon                    = CanvasUi::Icon::Sliders;
    settings.tooltip                 = _u8L_CONTEXT("Legend settings", "Legend");
    CanvasUi::place(CanvasUi::Point(slots[3].left, button_y));
    if (CanvasUi::collapsible_trigger("##legend_settings", "", m_settings_open, settings) && m_settings_open)
        collapse.set(false);
}

void LegendPanel::render_row_menu(const LegendModel& model)
{
    enum Item { ShowOnly, Toggle, ShowLineTypes, HideMoves, Copy };
    const LegendRow row   = m_menu_row;
    const auto      items = [&row]() {
        return std::vector<CanvasUi::MenuItem>{
            // TRN In the menu of a row of the preview legend: hide every other line type.
            CanvasUi::MenuItem::item(ShowOnly, format(_u8L_CONTEXT("Show only %1%", "Legend"), row.name), _u8L_CONTEXT("Alt+click", "Legend")),
            CanvasUi::MenuItem::item(Toggle, row.visible ? _u8L_CONTEXT("Hide", "Legend") : _u8L_CONTEXT("Show", "Legend"), _u8L_CONTEXT("Click", "Legend")),
            CanvasUi::MenuItem::separator(),
            CanvasUi::MenuItem::item(ShowLineTypes, _u8L_CONTEXT("Show all line types", "Legend")),
            CanvasUi::MenuItem::item(HideMoves, _u8L_CONTEXT("Hide all moves", "Legend")),
            CanvasUi::MenuItem::separator(),
            CanvasUi::MenuItem::item(Copy, _u8L_CONTEXT("Copy statistics", "Legend"))};
    };
    // Opened here, in the scope the menu is drawn in: the row asking for it lies in the body's scroll area.
    if (m_menu_pending) {
        m_menu_pending = false;
        CanvasUi::open_popup("##legend_line");
    }
    const std::optional<int> picked = CanvasUi::menu_popup("##legend_line", CanvasUi::Positioner::point(m_menu_pos.x, m_menu_pos.y), items);
    if (!picked)
        return;
    switch (*picked) {
    case ShowOnly:
        if (show_only_row)
            show_only_row(row);
        break;
    case Toggle:
        if (toggle_row)
            toggle_row(row);
        break;
    case ShowLineTypes:
        if (show_all_line_types)
            show_all_line_types();
        break;
    case HideMoves:
        if (hide_all_moves)
            hide_all_moves();
        break;
    default: {
        CanvasUi::copy_to_clipboard(legend_statistics_text(model.rows, model.imperial, translated_legend_units()));
        break;
    }
    }
}

void LegendPanel::render(const PreviewPanelPlacement& place, float bottom, const LegendModel& model)
{
    m_last_rect = CanvasUi::Rect();
    collapse.take_pending();
    const CanvasUi::Metrics& m = CanvasUi::Context::current()->metrics();
    const Sizes              s(m);
    CanvasUi::collapsible_close_on_escape(m_settings_open);
    CanvasUi::PanelCardOptions options;
    options.at        = CanvasUi::Point(place.x, place.top);
    options.right     = place.is_right();
    options.room      = bottom - place.top;
    options.collapsed = collapse.collapsed();
    std::optional<CanvasUi::SettingsFormOptions> form;
    if (m_settings_open && !collapse.collapsed())
        form = settings_form(options.card_width(m));
    options.extra = form ? std::min(CanvasUi::settings_form_height(m, CanvasUi::FontMeasure(), CanvasUi::ChoiceMeasure(), *form),
                                    std::max(0.f, options.room - m.panel_header)) :
                           0.f;
    const CanvasUi::PanelCard frame("Legend", options, m_body_content);
    const CanvasUi::Point      origin = frame.origin();

    render_header(frame, s, model);
    frame.body("##legend_body", s.inset, [&](const CanvasUi::Painter& paint, float left, float top, float width) {
        Body content(*this, s, model, paint, left, top, width);
        content.render();
        return content.height();
    });
    if (!collapse.collapsed() && options.extra >= 1.f) {
        form->height = options.extra;
        form->below  = frame.rect().height() - frame.header() - options.extra;
        if (CanvasUi::settings_form("##legend_settings_panel", CanvasUi::Point(origin.x, origin.y + frame.header()), *form)) {
            if (save_settings)
                save_settings(m_settings);
            CanvasUi::Context::current()->request_frame();
        }
    }
    render_row_menu(model);
    scale_view.render_range_popover(model.scale);
    m_last_rect = frame.rect();
}

} // namespace GUI
} // namespace Slic3r
