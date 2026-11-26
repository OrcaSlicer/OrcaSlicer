#include "LegendScaleView.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/LegendAxis.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/CanvasUi/Alert.hpp"
#include "slic3r/GUI/CanvasUi/Button.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/FontMeasure.hpp"
#include "slic3r/GUI/CanvasUi/Input.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Menu.hpp"
#include "slic3r/GUI/CanvasUi/MenuPopup.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/NumberField.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/PanelBody.hpp"
#include "slic3r/GUI/CanvasUi/Popover.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Tooltip.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/CanvasUi/Zone.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {
namespace GUI {

std::string legend_filter_text(const std::string& range, double share, bool layers)
{
    const std::string percent = legend_percent(share) + "%";
    if (layers)
        // TRN Under the preview legend's scale of layer times: the values kept in colour, and their share of the layers, as "77%".
        return format(_u8L_CONTEXT("Highlight %1% · %2% of the layers", "Legend"), range, percent);
    // TRN Under the preview legend's scale: the values kept in colour, and their share of the print time, as "77%".
    return format(_u8L_CONTEXT("Highlight %1% · %2% of the time", "Legend"), range, percent);
}

std::optional<CanvasUi::AlertOptions> legend_limit_note(const LegendScale& scale, const LegendUnits& units)
{
    CanvasUi::AlertOptions note;
    if (scale.at_limit_share) {
        if (*scale.at_limit_share > 0.0) {
            note.variant = CanvasUi::AlertVariant::Warning;
            // TRN Under the preview legend's scale of flow. %1% is a share of the print time, as "23%".
            note.text = format(_u8L_CONTEXT("%1% of the print runs at the filament limit: speed there is capped by flow, not by the speed settings.", "Legend"),
                               legend_percent(*scale.at_limit_share) + "%");
        } else {
            note.variant = CanvasUi::AlertVariant::Info;
            // TRN Under the preview legend's scale of flow, when no move reaches the filament's maximum volumetric speed from its profile.
            note.text = _u8L_CONTEXT("No part of the print reaches the filament limit.", "Legend");
        }
    } else if (scale.layers_under_limit) {
        note.variant = CanvasUi::AlertVariant::Info;
        const unsigned int layers = *scale.layers_under_limit;
        if (scale.limits.size() == 1)
            note.text = format(into_u8(_L_PLURAL("%1% layer is under %2% and was slowed down for cooling.",
                                                 "%1% layers are under %2% and were slowed down for cooling.", layers)),
                               layers, legend_time(scale.limits.front().value, units));
        else
            note.text = format(into_u8(_L_PLURAL("%1% layer was slowed down for cooling.", "%1% layers were slowed down for cooling.", layers)), layers);
    } else
        return std::nullopt;
    return note;
}

std::string legend_scale_value(const LegendScale& scale, double value, int decimals, const LegendUnits& units)
{
    return scale.times ? legend_time(float(value), units) : legend_value(value, decimals);
}

std::string legend_with_unit(const LegendScale& scale, const std::string& value)
{
    return scale.times || scale.unit.empty() ? value : value + " " + scale.unit;
}

struct LegendScaleView::Sizes
{
    explicit Sizes(const CanvasUi::Metrics& metrics) : m(metrics) {}

    const CanvasUi::Metrics& m;
    float padding{m.px(CanvasUi::space<4>())};
    float histogram_height{m.whole_px(CanvasUi::space<72>())};
    float histogram_gap{m.px(CanvasUi::space<2>())};
    float shortest_bin{m.px(CanvasUi::space<2>())};
    float narrowest_bin{m.px(CanvasUi::space<12>())};
    float widest_bin{m.px(CanvasUi::space<24>())};
    float bar_height{m.px(CanvasUi::space<8>())};
    float tick_length{m.px(CanvasUi::space<4>())};
    float tick_label_gap{m.px(CanvasUi::space<2>())};
    float brush_under_bar{m.px(CanvasUi::space<6>())};
    float mark_line{m.px(CanvasUi::Stroke::md)};
    float limit_dash{m.px(CanvasUi::space<4>())};
    float current_padding{m.px(CanvasUi::space<4>())};
    float current_radius{m.px(CanvasUi::Radius::sm)};
    float edge_grab{m.px(CanvasUi::space<12>())};
    float drag_threshold{m.px(CanvasUi::space<4>())};
    // An even width in pixels, so that a grip centred on a whole pixel has its sides on whole pixels too.
    float grip_width{2.f * std::round(.5f * m.px(CanvasUi::space<6>()))};
    float grip_height{m.px(CanvasUi::space<16>())};
    float range_field_width{m.px(CanvasUi::space<96>())};
};

class LegendScaleView::Bar
{
public:
    Bar(LegendScaleView& view, CanvasUi::PanelBody& body, const LegendModel& model, const LegendUnits& units, bool histogram)
        : m_view(view), m_body(body), m_s(body.metrics()), m_model(model), m_units(units), m_histogram(histogram)
    {}

    void render()
    {
        const LegendScale& scale  = m_model.scale;
        const float        left   = m_body.left() + metrics().table_padding_left;
        const float        right  = m_body.right() - metrics().table_padding_right;
        m_bins = scale.histogram;
        if (!scale.logarithmic)
            static_cast<LegendBins&>(m_bins) = legend_merge_bins(scale.histogram, size_t((right - left) / m_s.narrowest_bin), .005);
        const bool shown = m_histogram && !m_bins.amounts.empty();
        const bool labelled = std::any_of(scale.limits.begin(), scale.limits.end(),
                                          [&scale](const LegendLimit& limit) { return limit.value >= scale.min && limit.value <= scale.max; });
        const float                  hist   = shown ? m_s.histogram_height : 0.f;
        const float                  above  = shown || !labelled ? hist : metrics().caption_line;
        const float                  top    = m_body.y() + m_s.padding + above;
        const float                  bar    = m_s.bar_height;
        const LegendAxis axis(scale.min, scale.max, scale.logarithmic, m_bins, left, right, m_s.widest_bin);
        const CanvasUi::ZoneState zone =
            CanvasUi::zone("##brush", CanvasUi::Rect(left, top - hist, right, top + bar + m_s.brush_under_bar));
        const auto limits = limit_labels(axis, right, top - above);
        if (shown) {
            // The labels over the bars keep clear of the profile's limits and of the edges of the range.
            std::vector<CanvasUi::Rect> taken = limits.second;
            if (scale.filter)
                for (const float value : *scale.filter) {
                    const float x = std::round(axis.x_on(value));
                    taken.emplace_back(x - metrics().outline, top - hist, x + metrics().outline, top);
                }
            histogram(axis, zone, left, right, top - m_s.histogram_gap, hist - m_s.histogram_gap, taken);
        }
        gradient_bar(axis, top, bar);

        if (scale.filter) {
            const CanvasUi::Color veil(theme().card.r, theme().card.g, theme().card.b, .8f);
            const float           a = axis.x_on((*scale.filter)[0]), b = axis.x_on((*scale.filter)[1]);
            if (a > left)
                painter().fill_rect(CanvasUi::Rect(left, top, a, top + bar), veil);
            if (b < right)
                painter().fill_rect(CanvasUi::Rect(b, top, right, top + bar), veil);
        }
        brush(axis, zone, left, right, top - hist, top + bar, top + .5f * bar);
        const float label_top = scale_marks(axis, limits, left, right, top, hist);
        m_body.move_to(label_top + metrics().caption_line + m_s.padding);
        filter_row();
        limit_note();
    }

private:
    LegendScaleView&       m_view;
    CanvasUi::PanelBody&   m_body;
    const Sizes            m_s;
    const LegendModel&     m_model;
    const LegendUnits&     m_units;
    const bool             m_histogram;
    LegendHistogram        m_bins;

    const CanvasUi::Context& ui() const { return m_body.ui(); }
    const CanvasUi::Theme&   theme() const { return m_body.theme(); }
    const CanvasUi::Metrics& metrics() const { return m_body.metrics(); }
    const CanvasUi::Painter& painter() const { return m_body.painter(); }

    CanvasUi::Color palette_at(float t) const
    {
        const std::vector<CanvasUi::Color>& p = m_model.scale.palette;
        if (p.empty())
            return theme().foreground;
        const float  x = std::clamp(t, 0.f, 1.f) * float(p.size() - 1);
        const size_t i = std::min(size_t(x), p.size() - 1);
        return CanvasUi::mix(p[i], p[std::min(i + 1, p.size() - 1)], x - float(i));
    }

    bool outside_filter(double lo, double hi) const
    {
        const std::optional<std::array<float, 2>>& filter = m_model.scale.filter;
        return filter && (hi < (*filter)[0] || lo > (*filter)[1]);
    }

    std::string value_label(double value, int decimals) const { return legend_scale_value(m_model.scale, value, decimals, m_units); }

    std::string amount_text(double amount) const
    {
        return m_bins.layers ? legend_count((unsigned long long) std::llround(amount)) : legend_time(float(amount), m_units);
    }

    std::string bin_tooltip(size_t bin, double total) const
    {
        const LegendScale&     scale = m_model.scale;
        const LegendHistogram& h     = m_bins;
        const std::string      lo    = value_label(h.lowest[bin], scale.decimals);
        const std::string      hi    = value_label(h.highest[bin], scale.decimals);
        const std::string      text  = legend_with_unit(scale, lo == hi ? lo : lo + " – " + hi);
        const std::string share = legend_percent(total > 0.0 ? h.amounts[bin] / total : 0.0) + "%";
        if (h.layers) {
            const auto layers = (unsigned int) std::llround(h.amounts[bin]);
            // TRN A bar of the preview legend's histogram of layer times: how many layers took that long, and their share of the layers.
            return text + "\n" + format(into_u8(_L_PLURAL("%1% layer, %2% of the layers", "%1% layers, %2% of the layers", layers)), amount_text(h.amounts[bin]), share);
        }
        // TRN A bar of the preview legend's histogram: the print time at these values, and its share.
        return text + "\n" + format(_u8L_CONTEXT("%1%, %2% of the time", "Legend"), amount_text(h.amounts[bin]), share);
    }

    // Where the profile's limits cross the axis, and their labels at the top of the histogram beside them.
    std::pair<std::vector<float>, std::vector<CanvasUi::Rect>> limit_labels(const LegendAxis& axis, float right, float top) const
    {
        const LegendScale&   scale = m_model.scale;
        const CanvasUi::Font font  = ui().caption_font();
        std::vector<float>          lines;
        std::vector<CanvasUi::Rect> labels;
        for (const LegendLimit& limit : scale.limits) {
            const bool  inside = limit.value >= scale.min && limit.value <= scale.max;
            const float x      = inside ? std::round(axis.x_on(limit.value)) : -1.f;
            const float w      = font.width(limit_label(limit, m_model.choice.mode == LegendMode::Flow));
            const float tx     = x + metrics().inline_gap + w <= right ? x + metrics().inline_gap : x - metrics().inline_gap - w;
            lines.push_back(x);
            labels.emplace_back(tx, top, tx + w, top + metrics().caption_line);
        }
        return {lines, labels};
    }

    void histogram(const LegendAxis& axis, const CanvasUi::ZoneState& zone, float left, float right, float bottom, float height,
                   std::vector<CanvasUi::Rect> placed)
    {
        const LegendHistogram& h     = m_bins;
        const double           total = std::accumulate(h.amounts.begin(), h.amounts.end(), 0.0);
        const double           most  = h.amounts.empty() ? 0.0 : *std::max_element(h.amounts.begin(), h.amounts.end());
        if (!(most > 0.0))
            return;
        const CanvasUi::Font font = ui().caption_font();
        const float          room = height - metrics().caption_line;
        struct Column
        {
            float x0;
            float x1;
            float top;
        };
        std::vector<Column>   columns(h.amounts.size(), Column{0.f, 0.f, bottom});
        const CanvasUi::Point mouse = CanvasUi::pointer().pos;
        for (size_t i = 0; i < h.amounts.size(); ++i) {
            const double lo = std::max<double>(h.edges[i], m_model.scale.min);
            const double hi = std::min<double>(h.edges[i + 1], m_model.scale.max);
            if (h.amounts[i] <= 0.0 || !(hi > lo))
                continue;
            const float x0  = axis.x_on(lo);
            const float x1  = axis.x_on(hi);
            // The square root keeps the order of the bars and lifts the short ones, which at full scale would vanish under a few
            // high ones; the labels and the tooltip give the amounts.
            const float len = std::max(m_s.shortest_bin, float(std::sqrt(h.amounts[i] / most)) * room);
            const float gap = x1 - x0 > 3.f * metrics().outline ? metrics().outline : 0.f;
            columns[i]      = {x0, x1, bottom - len};
            painter().fill_rect(CanvasUi::Rect(std::round(x0 + .5f * gap), bottom - len, std::round(x1 - .5f * gap), bottom),
                                outside_filter(lo, hi) ? theme().input : palette_at(.5f * (axis.t_of(lo) + axis.t_of(hi))));
            if (zone.hovered && !zone.held && mouse.x >= x0 && mouse.x < x1 && mouse.y >= bottom - height && mouse.y < bottom) {
                const CanvasUi::ScopedId id{int(i)};
                CanvasUi::tooltip_for("##bin", bin_tooltip(i, total));
            }
        }
        std::vector<size_t> order;
        for (size_t i = 0; i < h.amounts.size(); ++i)
            if (total > 0.0 && h.amounts[i] / total >= .05)
                order.push_back(i);
        std::sort(order.begin(), order.end(), [&h](size_t a, size_t b) { return h.amounts[a] > h.amounts[b]; });
        const float                 pad = metrics().inline_gap;
        for (const size_t i : order) {
            const std::string    text = amount_text(h.amounts[i]);
            const float          w    = font.width(text);
            const Column&        c    = columns[i];
            const float          x    = std::clamp(.5f * (c.x0 + c.x1 - w), left, right - w);
            const CanvasUi::Rect r(x, c.top - metrics().caption_line, x + w, c.top);
            const bool           clear_of_labels = std::all_of(placed.begin(), placed.end(), [&](const CanvasUi::Rect& o) {
                return r.max().x + pad <= o.min().x || o.max().x + pad <= r.min().x || r.max().y <= o.min().y || o.max().y <= r.min().y;
            });
            const bool           clear_of_bars   = std::all_of(columns.begin(), columns.end(),
                                                               [&](const Column& o) { return o.x1 <= r.min().x || r.max().x <= o.x0 || o.top >= r.max().y; });
            if (!clear_of_labels || !clear_of_bars)
                continue;
            placed.push_back(r);
            painter().text(font, CanvasUi::Point(x, r.min().y), theme().muted_foreground, text);
        }
    }

    void gradient_bar(const LegendAxis& axis, float top, float bar) const
    {
        const LegendScale& scale = m_model.scale;
        for (const LegendAxisPiece& p : axis.pieces()) {
            std::vector<CanvasUi::Color> stops;
            const float                  t0 = axis.t_of(p.lo), t1 = axis.t_of(p.hi);
            const size_t                 n  = std::max<size_t>(2, size_t(std::ceil((t1 - t0) * float(scale.palette.size()))) + 1);
            for (size_t k = 0; k < n; ++k)
                stops.push_back(palette_at(t0 + (t1 - t0) * float(k) / float(n - 1)));
            painter().fill_gradient(CanvasUi::Rect(p.x0, top, p.x1, top + bar), stops, CanvasUi::GradientAxis::Horizontal);
        }
    }

    // The ticks under the bar, the profile's limits across the histogram and the bar, and the mark of the current move; returns
    // the top of the tick labels.
    float scale_marks(const LegendAxis& axis, const std::pair<std::vector<float>, std::vector<CanvasUi::Rect>>& limits, float left, float right,
                      float top, float hist)
    {
        const LegendScale& scale = m_model.scale;
        const float        bar   = m_s.bar_height;
        const CanvasUi::Font font  = ui().caption_font();
        const auto ticks_of = [&scale](double lo, double hi, size_t count) {
            return scale.logarithmic ? legend_log_ticks(lo, hi, count) : legend_ticks(lo, hi, count);
        };
        // The coarse ticks of the whole scale first, then finer ones, then those of the part of the scale with print time, which the
        // axis widens, where they leave room.
        std::vector<std::vector<double>> passes{ticks_of(scale.min, scale.max, 5), ticks_of(scale.min, scale.max, 16)};
        const auto first = std::find_if(axis.pieces().begin(), axis.pieces().end(), [](const LegendAxisPiece& p) { return !p.empty; });
        const auto last  = std::find_if(axis.pieces().rbegin(), axis.pieces().rend(), [](const LegendAxisPiece& p) { return !p.empty; });
        if (first != axis.pieces().end())
            passes.push_back(ticks_of(first->lo, last->hi, 8));
        const float tick_bottom = top + bar + m_s.tick_length;
        const float label_top   = tick_bottom + m_s.tick_label_gap;
        std::vector<std::pair<float, float>> taken;
        const auto draw = [&](double value, const std::string& text, bool always) {
            const float x         = std::round(axis.x_on(value));
            const float w         = font.width(text);
            const float tx        = std::clamp(x - .5f * w, left, right - w);
            const float clearance = font.width("0");
            if (!always && std::any_of(taken.begin(), taken.end(), [&](const std::pair<float, float>& t) { return tx < t.second + clearance && t.first < tx + w + clearance; }))
                return;
            taken.emplace_back(tx, tx + w);
            painter().line(CanvasUi::Point(x, top + bar), CanvasUi::Point(x, tick_bottom), theme().muted_foreground, metrics().outline);
            painter().text(font, CanvasUi::Point(tx, label_top), theme().muted_foreground, text);
        };
        const bool flow_limits = m_model.choice.mode == LegendMode::Flow;
        const CanvasUi::Color limit_color = flow_limits ? theme().warning : theme().info;
        const auto& [lines, labels] = limits;
        for (size_t i = 0; i < lines.size(); ++i) {
            const float x    = lines[i];
            if (x < 0.f)
                continue;
            const float dash = m_s.limit_dash;
            for (float y = top - hist; y < top + bar; y += 2.f * dash)
                painter().line(CanvasUi::Point(x, y), CanvasUi::Point(x, std::min(y + dash, top + bar)), limit_color, m_s.mark_line);
            painter().text(font, labels[i].min(), limit_color, limit_label(scale.limits[i], flow_limits));
        }
        if (scale.current) {
            const float           x    = std::round(axis.x_on(*scale.current));
            const std::string     text = value_label(*scale.current, scale.decimals);
            const float           pad  = m_s.current_padding;
            const float           w    = font.width(text) + 2.f * pad;
            const float           bx   = std::clamp(x - .5f * w, left, right - w);
            const CanvasUi::Color ink  = theme().foreground;
            painter().line(CanvasUi::Point(x, top - hist), CanvasUi::Point(x, tick_bottom), ink, m_s.mark_line);
            painter().fill_rect(CanvasUi::Rect(bx, label_top, bx + w, label_top + metrics().caption_line), ink, m_s.current_radius);
            painter().text(font, CanvasUi::Point(bx + pad, label_top), theme().card, text);
            taken.emplace_back(bx, bx + w);
        }
        const bool ends = !scale.current;
        draw(scale.min, value_label(scale.min, scale.decimals), ends);
        draw(scale.max, legend_with_unit(scale, value_label(scale.max, scale.decimals)), ends);
        for (const std::vector<double>& pass : passes) {
            const int decimals = legend_ticks_decimals(pass);
            for (const double tick : pass)
                draw(tick, value_label(tick, decimals), false);
        }
        return label_top;
    }

    void brush(const LegendAxis& axis, const CanvasUi::ZoneState& zone, float left, float right, float top, float bottom, float bar_mid)
    {
        const LegendScale&      scale = m_model.scale;
        const CanvasUi::Pointer mouse = CanvasUi::pointer();
        LegendBrushInput        input;
        input.filter           = scale.filter;
        input.x                = mouse.pos.x;
        input.hovered          = zone.hovered;
        input.pressed          = zone.pressed;
        input.held             = zone.held;
        input.double_clicked   = zone.double_clicked;
        input.pressed_anywhere = mouse.pressed;
        input.shift            = mouse.shift;
        input.left             = left;
        input.right            = right;
        input.edge_grab        = m_s.edge_grab;
        input.drag_threshold   = m_s.drag_threshold;
        const LegendBrushStep step = legend_brush_step(m_view.m_brush, input, axis);
        if (step.range_set && m_view.set_range)
            m_view.set_range(step.range);
        if (step.popover)
            open_range_popover(*step.popover, mouse.pos.x, m_s.edge_grab, top, bottom);
        if (step.hover && m_view.hover_range)
            m_view.hover_range((*step.hover)[0], (*step.hover)[1]);

        if (step.hovered_edge >= 0 || step.dragged_edge >= 0)
            CanvasUi::Context::current()->request_cursor(CanvasUi::Cursor::ResizeHorizontal);
        if (scale.filter)
            range_edges({axis.x_on((*scale.filter)[0]), axis.x_on((*scale.filter)[1])}, step.hovered_edge, step.dragged_edge, top, bottom, bar_mid);
    }

    // The edges of the range across the histogram and the bar, with a grip on the bar, wider than the line, that shows the edge
    // can be taken and dragged; the one under the pointer or dragged in the primary colour.
    void range_edges(const std::array<float, 2>& edges_x, int hovered, int dragged, float top, float bottom, float bar_mid) const
    {
        const float w = m_s.grip_width, h = m_s.grip_height;
        for (size_t i = 0; i < 2; ++i) {
            const float           x      = std::round(edges_x[i]);
            const bool            active = int(i) == hovered || int(i) == dragged;
            const CanvasUi::Color color  = active ? theme().primary : theme().foreground;
            painter().line(CanvasUi::Point(x, top), CanvasUi::Point(x, bottom), color, m_s.mark_line);
            const CanvasUi::Rect grip(x - .5f * w, bar_mid - .5f * h, x + .5f * w, bar_mid + .5f * h);
            painter().fill_rect(grip, color, .5f * w);
            painter().stroke_rect(grip, theme().card, metrics().outline, .5f * w);
        }
    }

    void open_range_popover(const std::array<float, 2>& range, float x, float grab, float top, float bottom)
    {
        m_view.m_range_anchor  = CanvasUi::Rect(x - grab, top, x + grab, bottom);
        m_view.m_range_edit    = {range[0], range[1]};
        m_view.m_range_pending = true;
    }

    void filter_row()
    {
        const LegendScale& scale = m_model.scale;
        if (!scale.filter)
            return;
        const float row   = metrics().control_height;
        const std::string label = _u8L_CONTEXT("Reset", "Legend");
        CanvasUi::ButtonOptions reset;
        reset.variant      = CanvasUi::ButtonVariant::Ghost;
        reset.size         = CanvasUi::ButtonSize::Sm;
        const float reset_w = CanvasUi::button_width(metrics(), CanvasUi::FontMeasure(), label, CanvasUi::ButtonSize::Sm);
        const std::string range = legend_with_unit(scale, value_label((*scale.filter)[0], scale.decimals) + "–" + value_label((*scale.filter)[1], scale.decimals));
        const std::string text = legend_filter_text(range, legend_filter_share(scale.histogram, *scale.filter), scale.histogram.layers);
        const float       x    = m_body.left() + metrics().table_padding_left;
        const float       room = m_body.right() - metrics().table_padding_right - reset_w - metrics().inline_gap - x;
        // The share is what the row is for, so a text too long for the row wraps rather than losing its end.
        const CanvasUi::FontMeasure measure;
        const float                 first = m_body.y() + .5f * (row - measure.line_height());
        painter().text_wrapped(ui().regular_font(), CanvasUi::Point(x, first), room, theme().foreground, text);
        CanvasUi::place(CanvasUi::Point(m_body.right() - metrics().table_padding_right - reset_w,
                                        m_body.y() + .5f * (row - CanvasUi::button_height(metrics(), CanvasUi::ButtonSize::Sm))));
        if (CanvasUi::button("##range_reset", label, reset) && m_view.set_range)
            m_view.set_range(std::nullopt);
        m_body.move_to(std::max(m_body.y() + row, first + measure.height(text, room) + .5f * (row - measure.line_height())) + metrics().item_gap);
    }

    std::string limit_label(const LegendLimit& limit, bool flow) const
    {
        const auto        digits = [](double v) { return std::abs(v - std::round(v)) < 1e-6 ? 0 : std::abs(v * 10.0 - std::round(v * 10.0)) < 1e-5 ? 1 : 2; };
        const std::string value  = flow ? legend_value(limit.value, digits(limit.value)) : legend_time(limit.value, m_units);
        // TRN A line on the preview legend's scale of flow: the filament's maximum volumetric speed from its profile.
        const std::string label = flow ? format(_u8L_CONTEXT("Max volumetric speed %1%", "Legend"), value)
                                       // TRN A line on the preview legend's scale of layer times: the time under which a layer is slowed down for cooling.
                                       : format(_u8L_CONTEXT("Layer time %1%", "Legend"), value);
        if (m_model.scale.limits.size() < 2)
            return label;
        std::string numbers;
        for (const int number : limit.filaments)
            numbers += (numbers.empty() ? "" : ", ") + std::to_string(number);
        const std::string filaments = limit.filaments.size() == 1 ?
                                          // TRN The filament a limit of the profile belongs to, after the limit on the preview legend's scale, as "filament 2".
                                          format(_u8L_CONTEXT("filament %1%", "Legend"), numbers) :
                                          // TRN The filaments a limit of the profile belongs to, after the limit on the preview legend's scale, as "filaments 1, 3".
                                          format(_u8L_CONTEXT("filaments %1%", "Legend"), numbers);
        // TRN A limit on the preview legend's scale and the filaments it belongs to, as "Max volumetric speed 12 · filaments 1, 3".
        return format(_u8L_CONTEXT("%1% · %2%", "Legend"), label, filaments);
    }

    void limit_note()
    {
        std::optional<CanvasUi::AlertOptions> note = legend_limit_note(m_model.scale, m_units);
        if (!note)
            return;
        note->width = m_body.width();
        m_body.place();
        CanvasUi::alert("##limit", *note);
        m_body.advance(CanvasUi::alert_height(metrics(), CanvasUi::FontMeasure(), CanvasUi::FontMeasure(ui().bold_font()), *note) + metrics().item_gap);
    }
};

void LegendScaleView::render(CanvasUi::PanelBody& body, const LegendModel& model, const LegendUnits& units, bool histogram)
{
    Bar(*this, body, model, units, histogram).render();
}

void LegendScaleView::render_range_popover(const LegendScale& scale)
{
    // Opened here, in the scope the popover is drawn in: the edge asking for it lies in the body's scroll area.
    if (m_range_pending) {
        m_range_pending = false;
        CanvasUi::open_popup("##legend_range");
    }
    const CanvasUi::Metrics& m     = CanvasUi::Context::current()->metrics();
    const Sizes              s(m);
    const CanvasUi::FontMeasure measure;
    const std::string from = _u8L_CONTEXT("From", "Legend"), to = _u8L_CONTEXT("To", "Legend");
    const std::string reset = _u8L_CONTEXT("Reset", "Legend"), apply = _u8L_CONTEXT("Apply", "Legend");
    const float label_w = std::max(measure.width(from), measure.width(to)) + m.field_gap;
    const float field_w = s.range_field_width;
    const float unit_w  = scale.unit.empty() || scale.times ? 0.f : measure.width(scale.unit) + m.inline_gap;
    const float width   = label_w + field_w + unit_w;
    CanvasUi::PopoverOptions options;
    // TRN The title of the box with the exact values the preview legend keeps in colour.
    options.title   = _u8L_CONTEXT("Highlight range", "Legend");
    options.content = CanvasUi::Size(width, 3.f * m.control_height + 3.f * m.item_gap);
    CanvasUi::Popover popover("##legend_range", CanvasUi::Positioner::below(m_range_anchor, m.popup_side_offset), options);
    if (!popover.open())
        return;
    const CanvasUi::Rect      box = popover.content();
    const CanvasUi::Painter   paint = CanvasUi::Painter::current();
    const CanvasUi::Context&  ui    = *CanvasUi::Context::current();
    const int                 decimals = std::max(scale.decimals, 1);
    float y = box.min().y;
    const auto field_row = [&](const char* id, const std::string& label, double& value) {
        paint.text(ui.regular_font(), CanvasUi::Rect(box.min().x, y, box.min().x + label_w, y + m.control_height), ui.theme().foreground, label);
        CanvasUi::place(CanvasUi::Point(box.min().x + label_w, y));
        CanvasUi::NumberFieldOptions field;
        field.width = field_w;
        CanvasUi::number_field(id, value, decimals, field);
        if (unit_w > 0.f)
            paint.text(ui.regular_font(), CanvasUi::Rect(box.min().x + label_w + field_w + m.inline_gap, y, box.max().x, y + m.control_height),
                       ui.theme().muted_foreground, scale.unit);
        y += m.control_height + m.item_gap;
    };
    field_row("##from", from, m_range_edit[0]);
    field_row("##to", to, m_range_edit[1]);
    std::array<float, 2> typed{float(std::min(m_range_edit[0], m_range_edit[1])), float(std::max(m_range_edit[0], m_range_edit[1]))};
    typed = {std::clamp(typed[0], scale.min, scale.max), std::clamp(typed[1], scale.min, scale.max)};
    y += m.item_gap;
    CanvasUi::ButtonOptions secondary;
    secondary.size = CanvasUi::ButtonSize::Sm;
    CanvasUi::ButtonOptions primary = secondary;
    primary.variant                 = CanvasUi::ButtonVariant::Default;
    const float apply_w = CanvasUi::button_width(m, measure, apply, CanvasUi::ButtonSize::Sm);
    const float reset_w = CanvasUi::button_width(m, measure, reset, CanvasUi::ButtonSize::Sm);
    CanvasUi::place(CanvasUi::Point(box.max().x - apply_w - m.inline_gap - reset_w, y));
    if (CanvasUi::button("##popover_reset", reset, secondary)) {
        if (set_range)
            set_range(std::nullopt);
        popover.close();
    }
    CanvasUi::place(CanvasUi::Point(box.max().x - apply_w, y));
    if (CanvasUi::button("##popover_apply", apply, primary)) {
        if (set_range)
            set_range(legend_range_whole(typed, scale.min, scale.max) ? std::nullopt : std::optional<std::array<float, 2>>(typed));
        popover.close();
    }
}

} // namespace GUI
} // namespace Slic3r
