#include "PrintSummaryPanel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/PreviewPanelParts.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/PrintSummaryModel.hpp"
#include "slic3r/GUI/CanvasUi/Alert.hpp"
#include "slic3r/GUI/CanvasUi/Availability.hpp"
#include "slic3r/GUI/CanvasUi/Button.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/FontMeasure.hpp"
#include "slic3r/GUI/CanvasUi/Icons.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/PanelBody.hpp"
#include "slic3r/GUI/CanvasUi/PanelCard.hpp"
#include "slic3r/GUI/CanvasUi/ScrollArea.hpp"
#include "slic3r/GUI/CanvasUi/Swatch.hpp"
#include "slic3r/GUI/CanvasUi/Table.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/ToggleGroup.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Tooltip.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/CanvasUi/Zone.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {
namespace GUI {

namespace {

std::string event_name(SummaryEvent::Kind kind)
{
    switch (kind) {
    case SummaryEvent::Kind::Pause: return _u8L("Pause");
    case SummaryEvent::Kind::ColorChange: return _u8L("Color change");
    case SummaryEvent::Kind::ToolChange: return _u8L("Tool Change");
    case SummaryEvent::Kind::Template: return _u8L("Template");
    case SummaryEvent::Kind::Custom: return _u8L("Custom G-code");
    default: return _u8L("Unknown");
    }
}


} // namespace

struct PrintSummaryPanel::Sizes
{
    explicit Sizes(const CanvasUi::Metrics& metrics) : m(metrics) {}

    const CanvasUi::Metrics& m;
    float line{m.whole_px(CanvasUi::TextRole::body.leading)};
    float line_gap{m.whole_px(CanvasUi::space<4>())};
    float icon{m.px(CanvasUi::icon_size<14>())};
    float time_col{m.px(CanvasUi::space<52>())};
    float share_col{m.px(CanvasUi::space<36>())};
    float profile_label{m.px(CanvasUi::space<120>())};
};

// Time, weight, length and cost on one line from left to right, the weight telling the filament the model alone takes.
void draw_totals_line(const CanvasUi::Painter& paint, const PrintSummaryModel& model, float left, float top, float right, const PrintSummaryPanel::Sizes& s)
{
    const CanvasUi::Context& ui    = *CanvasUi::Context::current();
    const CanvasUi::Theme&   t     = ui.theme();
    const LegendUnits        units = translated_legend_units();
    const SummaryTotals&     f     = model.totals;
    float                    x     = left;
    paint.icon(CanvasUi::Icon::Clock, CanvasUi::Point(x + .5f * s.icon, top + .5f * s.line), s.icon, t.muted_foreground);
    x += s.icon + s.m.inline_gap;
    const CanvasUi::Font regular = ui.regular_font();
    const CanvasUi::Font bold    = ui.bold_font();
    const auto segment = [&](const CanvasUi::Font& font, const CanvasUi::Color& color, const std::string& text) {
        const std::string shown = CanvasUi::ellipsize(CanvasUi::FontMeasure(font), text, std::max(0.f, right - x));
        paint.text(font, CanvasUi::Rect(x, top, right, top + s.line), color, shown);
        const CanvasUi::Rect rect(x, top, x + font.width(shown), top + s.line);
        x += font.width(shown);
        return rect;
    };
    const std::string dot = " · ";
    segment(bold, t.foreground, legend_long_time(f.time, units));
    segment(regular, t.muted_foreground, dot);
    const CanvasUi::Rect weight = segment(regular, t.foreground, legend_mass(f.grams, model.imperial, units));
    segment(regular, t.muted_foreground, dot);
    segment(regular, t.foreground, legend_filament_length(f.mm, model.imperial, units));
    if (f.cost > 0.0) {
        segment(regular, t.muted_foreground, dot);
        // TRN The cost of the print's filament in the print summary, after its time, weight and length.
        segment(regular, t.muted_foreground, _u8L_CONTEXT("cost", "Print summary") + " ");
        segment(regular, t.foreground, legend_value(f.cost, 2));
    }
    if (CanvasUi::zone("##summary_weight_area", weight).hovered)
        // TRN The filament that goes into the model alone: the print's without supports, flushing and the prime tower.
        CanvasUi::tooltip_for("##summary_weight",
                              format(_u8L_CONTEXT("Model filament: %1%", "Print summary"), legend_mass(f.model_grams, model.imperial, units)));
}

class PrintSummaryPanel::Body : public CanvasUi::PanelBody
{
public:
    Body(PrintSummaryPanel& panel, const Sizes& s, const PrintSummaryModel& model, const CanvasUi::Painter& paint, float left, float top, float width)
        : PanelBody(paint, left, top, width), m_panel(panel), m_s(s), m_model(model), m_units(translated_legend_units())
    {}

    void render()
    {
        totals();
        if (!m_model.filaments.empty()) {
            gap();
            filaments();
        }
        changes();
        events();
        if (m_model.profiles)
            profiles();
    }

private:
    PrintSummaryPanel&       m_panel;
    const Sizes&             m_s;
    const PrintSummaryModel& m_model;
    float                    m_mass_col{0.f};
    LegendUnits              m_units;

    void totals()
    {
        const CanvasUi::Metrics& m     = metrics();
        const SummaryTotals&     f     = m_model.totals;
        const float              x     = left() + m.table_padding_left;
        const float              right = PanelBody::right() - m.table_padding_right;
        draw_totals_line(painter(), m_model, x, y(), right, m_s);
        advance(m_s.line);
        if (!(f.prepare > 0.f || f.modes_differ))
            return;
        advance(m_s.line_gap);
        float end = right;
        if (f.modes_differ) {
            // TRN The time estimate in the print summary: for the printer's normal or its silent mode.
            const std::vector<std::string> modes = {_u8L_CONTEXT("Normal", "Print summary"), _u8L_CONTEXT("Stealth", "Print summary")};
            const float                    width = CanvasUi::toggle_group_width(m, CanvasUi::ChoiceMeasure(), modes);
            int                            mode  = f.stealth ? 1 : 0;
            CanvasUi::place(CanvasUi::Point(right - width, y()));
            if (CanvasUi::toggle_group("##summary_time_mode", mode, modes) && m_panel.set_stealth)
                m_panel.set_stealth(mode == 1);
            end = right - width - m.inline_gap;
        }
        if (f.prepare > 0.f) {
            const CanvasUi::Font caption = ui().caption_font();
            // TRN In the print summary: the time the printer prepares, and the time it prints the model.
            const std::string text = format(_u8L_CONTEXT("Prepare %1% · printing %2%", "Print summary"), legend_long_time(f.prepare, m_units),
                                            legend_long_time(f.time - f.prepare, m_units));
            painter().text(caption, CanvasUi::Rect(x, y(), end, y() + m.control_height), theme().muted_foreground,
                           CanvasUi::ellipsize(CanvasUi::FontMeasure(caption), text, end - x));
        }
        advance(m.control_height);
    }

    std::string parts_text(const std::array<double, LEGEND_PART_COUNT>& grams, const std::array<double, LEGEND_PART_COUNT>& mm, bool lines) const
    {
        const std::string names[] = {
            // TRN A part of a filament's use in the print summary, with its mass.
            _u8L_CONTEXT("model %1%", "Print summary"), _u8L_CONTEXT("support %1%", "Print summary"),
            // TRN The filament flushed when changing to it, with its mass, in the print summary.
            _u8L_CONTEXT("flush %1%", "Print summary"),
            // TRN The filament the prime tower takes, with its mass, in the print summary.
            _u8L_CONTEXT("tower %1%", "Print summary")};
        std::string text;
        for (size_t part = 0; part < LEGEND_PART_COUNT; ++part) {
            if (grams[part] <= 0.0 && mm[part] <= 0.0)
                continue;
            std::string value = legend_mass(grams[part], m_model.imperial, m_units);
            if (lines)
                value += ", " + legend_filament_length(mm[part], m_model.imperial, m_units);
            if (!text.empty())
                text += lines ? "\n" : " · ";
            text += format(names[part], value);
        }
        return text;
    }

    // The mass column as wide as its widest entry, in bold as a selected row draws it, so that the names end alike on every row.
    void filament_columns()
    {
        const CanvasUi::Font bold = ui().bold_font();
        m_mass_col                = 0.f;
        for (const LegendFilament& filament : m_model.filaments)
            m_mass_col = std::max(m_mass_col, std::ceil(bold.width(legend_mass_value(filament.total_grams(), m_model.imperial))));
    }

    void text_row(const std::string& label, const std::string& value, bool bold = false)
    {
        const float          row   = metrics().table_row;
        const float          x     = left() + metrics().table_padding_left;
        const float          right = PanelBody::right() - metrics().table_padding_right;
        const CanvasUi::Font font  = bold ? ui().bold_font() : ui().regular_font();
        painter().text(font, CanvasUi::Rect(x, y(), right, y() + row), theme().foreground,
                       CanvasUi::ellipsize(CanvasUi::FontMeasure(font), label, right - x - font.width(value) - metrics().table_cell_gap));
        painter().text(font, CanvasUi::Rect(right - font.width(value), y(), right, y() + row), theme().foreground, value);
        advance(row);
    }

    void filaments()
    {
        // TRN The heading of the filaments in the print summary, with how many there are.
        section_title(format(_u8L_CONTEXT("Filaments · %1%", "Print summary"), m_model.filaments.size()),
                      m_model.imperial ? _u8L_CONTEXT("oz", "Legend unit") : _u8L_CONTEXT("g", "Legend unit"));
        filament_columns();
        FilamentRowOptions row;
        row.isolated       = m_model.isolated;
        row.several        = m_model.filaments.size() > 1;
        const auto rows_of = [&](const std::vector<int>* numbers) {
            for (const LegendFilament& filament : m_model.filaments)
                if (numbers == nullptr || std::find(numbers->begin(), numbers->end(), filament.number) != numbers->end()) {
                    row.mass    = {legend_mass_value(filament.total_grams(), m_model.imperial), m_mass_col, CanvasUi::CellAlign::Right, false, true};
                    row.details = parts_text(filament.grams, filament.mm, true);
                    filament_row(*this, filament, row, m_panel.hover_filament, m_panel.isolate_filament);
                }
        };
        if (m_model.nozzles.empty())
            rows_of(nullptr);
        else
            for (const SummaryNozzle& nozzle : m_model.nozzles) {
                section_title(nozzle.title, legend_mass(nozzle.grams, m_model.imperial, m_units));
                rows_of(&nozzle.numbers);
            }
        if (m_model.filaments.size() > 1) {
            std::array<double, LEGEND_PART_COUNT> grams{}, mm{};
            for (const LegendFilament& filament : m_model.filaments)
                for (size_t part = 0; part < LEGEND_PART_COUNT; ++part) {
                    grams[part] += filament.grams[part];
                    mm[part] += filament.mm[part];
                }
            double total = 0.0;
            for (const double g : grams)
                total += g;
            text_row(_u8L("Total"), legend_mass(total, m_model.imperial, m_units), true);
            const CanvasUi::Font caption = ui().caption_font();
            painter().text(caption, CanvasUi::Point(left() + metrics().table_padding_left, y()), theme().muted_foreground, parts_text(grams, mm, false));
            advance(metrics().caption_line);
        }
        if (m_model.grouping)
            grouping();
    }

    void changes()
    {
        if (m_model.filament_changes > 0 || m_model.tool_changes > 0) {
            gap();
            section_title(_u8L_CONTEXT("Changes", "Print summary"));
            if (m_model.filament_changes > 0)
                text_row(_u8L("Filament changes"), legend_count(m_model.filament_changes));
            if (m_model.tool_changes > 0)
                text_row(_u8L("Tool changes"), legend_count(m_model.tool_changes));
        }
    }


    void events()
    {
        if (m_model.events.empty())
            return;
        gap();
        table_header({{_u8L_CONTEXT("Events", "Print summary"), 0.f, CanvasUi::CellAlign::Left},
                      {_u8L_CONTEXT("Layer", "Print summary"), m_s.share_col},
                      {_u8L_CONTEXT("Time", "Print summary"), m_s.time_col}},
                     0.f);
        int index = 0;
        for (const SummaryEvent& event : m_model.events) {
            const std::string         name = event_name(event.kind);
            CanvasUi::TableRowOptions row;
            row.width = width();
            row.cells = {{name, 0.f, CanvasUi::CellAlign::Left}, {std::to_string(event.layer), m_s.share_col}, {legend_time(event.time, m_units), m_s.time_col}};
            // TRN Clicking an event of the print summary moves the layer slider to its layer.
            row.tooltip = _u8L_CONTEXT("Go to its layer", "Print summary");
            place();
            CanvasUi::ScopedId id(index++);
            if (CanvasUi::table_row("##event", row).clicked && m_panel.go_to_layer)
                m_panel.go_to_layer(event.layer);
            if (event.kind == SummaryEvent::Kind::ColorChange) {
                const std::vector<CanvasUi::RowSpan> spans = CanvasUi::table_columns(metrics(), left(), width(), row.cells, 0.f);
                const float side = metrics().table_swatch;
                float       x    = spans.front().left + ui().regular_font().width(name) + metrics().table_cell_gap;
                const float sy   = std::round(y() + .5f * (metrics().table_row - side));
                CanvasUi::draw_swatch(painter(), CanvasUi::Rect(x, sy, x + side, sy + side), CanvasUi::Swatch{event.before}, theme().card);
                x += side + metrics().inline_gap;
                const float arrow = metrics().px(CanvasUi::icon_size<12>());
                painter().icon(CanvasUi::Icon::ArrowRight, CanvasUi::Point(x + .5f * arrow, y() + .5f * metrics().table_row), arrow,
                               theme().muted_foreground);
                x += arrow + metrics().inline_gap;
                CanvasUi::draw_swatch(painter(), CanvasUi::Rect(x, sy, x + side, sy + side), CanvasUi::Swatch{event.after}, theme().card);
            }
            advance(metrics().table_row);
        }
    }

    void grouping()
    {
        const SummaryGrouping& g = *m_model.grouping;
        gap();
        const CanvasUi::Metrics& m       = metrics();
        const CanvasUi::Font     caption = ui().caption_font();
        const float              x       = left() + m.table_padding_left;
        const float              wrap    = width() - m.table_padding_left - m.table_padding_right;
        const std::string        hint    = _u8L("Please place filaments on the printer based on grouping result.");
        painter().text_wrapped(caption, CanvasUi::Point(x, y()), wrap, theme().muted_foreground, hint);
        advance(CanvasUi::FontMeasure(caption).height(hint, wrap) + m.item_gap);

        CanvasUi::ButtonOptions link;
        link.variant = CanvasUi::ButtonVariant::Ghost;
        link.size    = CanvasUi::ButtonSize::Sm;
        CanvasUi::place(CanvasUi::Point(x, y()));
        // TRN Opens the wiki page about grouping filaments over the two nozzles.
        if (CanvasUi::button("##grouping_wiki", _u8L_CONTEXT("How grouping works", "Print summary"), link) && m_panel.open_grouping_wiki)
            m_panel.open_grouping_wiki();
        advance(CanvasUi::button_height(m, CanvasUi::ButtonSize::Sm) + m.item_gap);

        CanvasUi::AlertOptions alert;
        alert.width   = width();
        alert.variant = g.optimal ? CanvasUi::AlertVariant::Info : CanvasUi::AlertVariant::Warning;
        if (!g.optimal)
            alert.title = _u8L("Current grouping of slice result is not optimal.");
        alert.text = g.comparison;
        if (!g.optimal)
            alert.actions.push_back(_u8L("Set to Optimal"));
        alert.actions.push_back(_u8L("Regroup filament"));
        if (alert.title.empty() && alert.text.empty()) {
            CanvasUi::ButtonOptions regroup;
            regroup.size = CanvasUi::ButtonSize::Sm;
            CanvasUi::place(CanvasUi::Point(x, y()));
            if (CanvasUi::button("##regroup", _u8L("Regroup filament"), regroup) && m_panel.regroup_filaments)
                m_panel.regroup_filaments();
            advance(CanvasUi::button_height(m, CanvasUi::ButtonSize::Sm));
            return;
        }
        place();
        if (const std::optional<size_t> action = CanvasUi::alert("##grouping", alert)) {
            const bool optimal_action = !g.optimal && *action == 0;
            if (optimal_action && m_panel.set_optimal_grouping)
                m_panel.set_optimal_grouping();
            else if (!optimal_action && m_panel.regroup_filaments)
                m_panel.regroup_filaments();
        }
        advance(CanvasUi::alert_height(m, CanvasUi::FontMeasure(), CanvasUi::FontMeasure(ui().bold_font()), alert));
    }


    void profiles()
    {
        const SummaryProfiles& p = *m_model.profiles;
        gap();
        section_title(_u8L_CONTEXT("Profiles in this file", "Print summary"));
        const CanvasUi::Font font  = ui().regular_font();
        const float          x     = left() + metrics().table_padding_left;
        const float          right = PanelBody::right() - metrics().table_padding_right;
        const auto           row   = [&](const std::string& label, const std::string& value) {
            const float h = metrics().table_row;
            painter().text(font, CanvasUi::Rect(x, y(), x + m_s.profile_label, y() + h), theme().muted_foreground,
                           CanvasUi::ellipsize(CanvasUi::FontMeasure(font), label, m_s.profile_label - metrics().table_cell_gap));
            painter().text(font, CanvasUi::Rect(x + m_s.profile_label, y(), right, y() + h), theme().foreground,
                           CanvasUi::ellipsize(CanvasUi::FontMeasure(font), value, right - x - m_s.profile_label));
            advance(h);
        };
        if (!p.printer.empty())
            row(_u8L("Printer"), p.printer);
        if (!p.print.empty())
            row(_u8L("Print settings"), p.print);
        for (const auto& [number, name] : p.filaments)
            row(p.filaments.size() == 1 ? _u8L("Filament") : format(_u8L_CONTEXT("Filament %1%", "Print summary"), number), name);
    }

};

void PrintSummaryPanel::load_settings(const AppConfig& config) { collapse.load(load_preview_panel_collapsed(config, PreviewPanel::Summary)); }

void PrintSummaryPanel::render_header(const CanvasUi::PanelCard& frame, const Sizes& s, const PrintSummaryModel& model)
{
    const CanvasUi::Context& ui     = *CanvasUi::Context::current();
    const CanvasUi::Point    origin = frame.origin();
    if (frame.collapse_button("##summary_collapse", collapse.collapsed(),
                          collapse.collapsed() ? _u8L_CONTEXT("Expand", "Print summary") : _u8L_CONTEXT("Collapse", "Print summary")))
        collapse.toggle_next_frame();
    const float text_left = frame.title_left();
    const float right     = origin.x + frame.width() - ui.metrics().card_padding;
    if (collapse.collapsed())
        draw_totals_line(frame.card().painter(), model, text_left, origin.y + .5f * (frame.header() - s.line), right, s);
    else
        // TRN The title of the preview panel with the print's totals, filament use, changes and events.
        frame.card().painter().text(ui.bold_font(), CanvasUi::Rect(text_left, origin.y, right, origin.y + frame.header()), ui.theme().foreground,
                                    _u8L_CONTEXT("Print summary", "Print summary"));
}

void PrintSummaryPanel::render(const PreviewPanelPlacement& place, float bottom, const PrintSummaryModel& model)
{
    m_last_rect = CanvasUi::Rect();
    collapse.take_pending();
    const CanvasUi::Metrics& m = CanvasUi::Context::current()->metrics();
    const Sizes              s(m);
    CanvasUi::PanelCardOptions options;
    options.at        = CanvasUi::Point(place.x, place.top);
    options.right     = place.is_right();
    options.room      = bottom - place.top;
    options.collapsed = collapse.collapsed();
    const CanvasUi::PanelCard frame("Print summary", options, m_body_content);
    render_header(frame, s, model);
    frame.body("##summary_body", m.inline_gap, [&](const CanvasUi::Painter& paint, float left, float top, float width) {
        Body content(*this, s, model, paint, left, top, width);
        content.render();
        return content.height();
    });
    m_last_rect = frame.rect();
}

} // namespace GUI
} // namespace Slic3r
