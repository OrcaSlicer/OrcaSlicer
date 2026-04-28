// MixedFilamentColorMapPanel.cpp
// Extracted verbatim from FullSpectrum Plater.cpp:3087-3781 (Task 17).

#include "MixedFilamentColorMapPanel.hpp"

#include "libslic3r/filament_mixer.h"

#include <wx/dcbuffer.h>
#include <wx/event.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers — free functions used only by this widget.
// Copied verbatim from FullSpectrum Plater.cpp:2443-2640.
// ---------------------------------------------------------------------------
namespace {

wxColour blend_multi_filament_mixer(const std::vector<wxColour> &colors, const std::vector<double> &weights)
{
    if (colors.empty() || weights.empty())
        return wxColour("#26A69A");

    unsigned char out_r = 0;
    unsigned char out_g = 0;
    unsigned char out_b = 0;
    double accumulated_weight = 0.0;
    bool has_color = false;

    for (size_t i = 0; i < colors.size() && i < weights.size(); ++i) {
        const double weight = std::max(0.0, weights[i]);
        if (weight <= 0.0)
            continue;

        const wxColour safe = colors[i].IsOk() ? colors[i] : wxColour("#26A69A");
        const unsigned char r = static_cast<unsigned char>(safe.Red());
        const unsigned char g = static_cast<unsigned char>(safe.Green());
        const unsigned char b = static_cast<unsigned char>(safe.Blue());

        if (!has_color) {
            out_r = r;
            out_g = g;
            out_b = b;
            accumulated_weight = weight;
            has_color = true;
            continue;
        }

        const double new_total = accumulated_weight + weight;
        if (new_total <= 0.0)
            continue;
        const float t = float(weight / new_total);
        ::Slic3r::filament_mixer_lerp(out_r, out_g, out_b, r, g, b, t, &out_r, &out_g, &out_b);
        accumulated_weight = new_total;
    }

    if (!has_color)
        return wxColour("#26A69A");

    return wxColour(out_r, out_g, out_b);
}

std::vector<int> normalize_color_match_weights(const std::vector<int> &weights, size_t count)
{
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, count > 0 ? int(100 / int(count)) : 0);

    int sum = 0;
    for (int &value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned          += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

bool color_match_raw_weights_within_range(const std::vector<double> &weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const double min_allowed = double(std::clamp(min_component_percent, 0, 50));
    int active_components = 0;
    for (const double weight : weights) {
        if (weight <= 1e-4)
            continue;
        ++active_components;
        if (weight * 100.0 + 1e-6 < min_allowed)
            return false;
    }
    return active_components >= 2;
}

} // anonymous namespace

// ===========================================================================
// MixedFilamentColorMapPanel — implementation
// Verbatim from FullSpectrum Plater.cpp:3087-3781.
// ===========================================================================

MixedFilamentColorMapPanel::MixedFilamentColorMapPanel(wxWindow                        *parent,
                                                       const std::vector<unsigned int> &filament_ids,
                                                       const std::vector<wxColour>     &palette,
                                                       const std::vector<int>          &initial_weights,
                                                       const wxSize                    &min_size)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, min_size, wxBORDER_SIMPLE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(min_size);
    m_render_timer.SetOwner(this);

    m_colors.reserve(filament_ids.size());
    for (const unsigned int filament_id : filament_ids) {
        if (filament_id >= 1 && filament_id <= palette.size())
            m_colors.emplace_back(palette[filament_id - 1]);
        else
            m_colors.emplace_back(wxColour("#26A69A"));
    }
    if (m_colors.empty())
        m_colors.emplace_back(wxColour("#26A69A"));

    set_normalized_weights(initial_weights, false);

    Bind(wxEVT_PAINT,              &MixedFilamentColorMapPanel::on_paint,        this);
    Bind(wxEVT_LEFT_DOWN,          &MixedFilamentColorMapPanel::on_left_down,    this);
    Bind(wxEVT_LEFT_UP,            &MixedFilamentColorMapPanel::on_left_up,      this);
    Bind(wxEVT_MOTION,             &MixedFilamentColorMapPanel::on_mouse_move,   this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &MixedFilamentColorMapPanel::on_capture_lost, this);
    Bind(wxEVT_SIZE,               &MixedFilamentColorMapPanel::on_size,         this);
    Bind(wxEVT_TIMER,              &MixedFilamentColorMapPanel::on_render_timer, this, m_render_timer.GetId());
}

MixedFilamentColorMapPanel::~MixedFilamentColorMapPanel()
{
    if (HasCapture())
        ReleaseMouse();
    if (m_render_timer.IsRunning())
        m_render_timer.Stop();
}

std::vector<int> MixedFilamentColorMapPanel::normalized_weights() const
{
    return m_weights;
}

wxColour MixedFilamentColorMapPanel::selected_color() const
{
    std::vector<double> weights;
    weights.reserve(m_weights.size());
    for (const int weight : m_weights)
        weights.emplace_back(double(std::max(0, weight)));
    return blend_multi_filament_mixer(m_colors, weights);
}

void MixedFilamentColorMapPanel::set_normalized_weights(const std::vector<int> &weights, bool notify)
{
    m_weights = normalize_color_match_weights(weights, m_colors.size());
    initialize_cursor_from_weights();
    Refresh();
    if (notify)
        emit_changed();
}

void MixedFilamentColorMapPanel::set_min_component_percent(int min_component_percent)
{
    const int clamped = std::clamp(min_component_percent, 0, 50);
    if (m_min_component_percent == clamped)
        return;
    m_min_component_percent = clamped;
    invalidate_cached_bitmap();
    Refresh();
}

// ---------------------------------------------------------------------------
// Private: geometry helpers
// ---------------------------------------------------------------------------

MixedFilamentColorMapPanel::GeometryMode MixedFilamentColorMapPanel::geometry_mode() const
{
    if (m_colors.size() <= 1)
        return GeometryMode::Point;
    if (m_colors.size() == 2)
        return GeometryMode::Line;
    if (m_colors.size() == 3)
        return GeometryMode::Triangle;
    if (m_colors.size() == 4)
        return GeometryMode::TriangleWithCenter;
    return GeometryMode::Radial;
}

wxRect MixedFilamentColorMapPanel::canvas_rect() const
{
    const wxSize size = GetClientSize();
    return wxRect(0, 0, std::max(1, size.GetWidth()), std::max(1, size.GetHeight()));
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::make_vec(double x, double y)
{
    return Vec2 { x, y };
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::add_vec(const Vec2 &lhs, const Vec2 &rhs)
{
    return Vec2 { lhs.x + rhs.x, lhs.y + rhs.y };
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::sub_vec(const Vec2 &lhs, const Vec2 &rhs)
{
    return Vec2 { lhs.x - rhs.x, lhs.y - rhs.y };
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::scale_vec(const Vec2 &value, double factor)
{
    return Vec2 { value.x * factor, value.y * factor };
}

double MixedFilamentColorMapPanel::dot_vec(const Vec2 &lhs, const Vec2 &rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y;
}

double MixedFilamentColorMapPanel::length_sq(const Vec2 &value)
{
    return dot_vec(value, value);
}

double MixedFilamentColorMapPanel::dist_sq(const Vec2 &lhs, const Vec2 &rhs)
{
    return length_sq(sub_vec(lhs, rhs));
}

std::array<MixedFilamentColorMapPanel::Vec2, 3> MixedFilamentColorMapPanel::simplex_vertices() const
{
    return { make_vec(0.50, 0.05), make_vec(0.08, 0.94), make_vec(0.92, 0.94) };
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::simplex_center() const
{
    const auto vertices = simplex_vertices();
    return make_vec((vertices[0].x + vertices[1].x + vertices[2].x) / 3.0,
                    (vertices[0].y + vertices[1].y + vertices[2].y) / 3.0);
}

std::vector<MixedFilamentColorMapPanel::AnchorPoint> MixedFilamentColorMapPanel::radial_anchor_points() const
{
    std::vector<AnchorPoint> anchors;
    const size_t count = m_colors.size();
    anchors.reserve(count);
    if (count == 0)
        return anchors;
    if (count == 1) {
        anchors.emplace_back(AnchorPoint { 0.5, 0.5 });
        return anchors;
    }
    if (count == 2) {
        anchors.emplace_back(AnchorPoint { 0.0, 0.5 });
        anchors.emplace_back(AnchorPoint { 1.0, 0.5 });
        return anchors;
    }
    if (count == 3) {
        anchors.emplace_back(AnchorPoint { 0.0, 0.5 });
        anchors.emplace_back(AnchorPoint { 1.0, 0.0 });
        anchors.emplace_back(AnchorPoint { 1.0, 1.0 });
        return anchors;
    }
    if (count == 4) {
        anchors.emplace_back(AnchorPoint { 0.0, 0.0 });
        anchors.emplace_back(AnchorPoint { 1.0, 0.0 });
        anchors.emplace_back(AnchorPoint { 1.0, 1.0 });
        anchors.emplace_back(AnchorPoint { 0.0, 1.0 });
        return anchors;
    }

    constexpr double k_pi = 3.14159265358979323846;
    const double center_x = 0.5;
    const double center_y = 0.5;
    const double radius = 0.45;
    for (size_t idx = 0; idx < count; ++idx) {
        const double angle = (2.0 * k_pi * double(idx)) / double(count);
        anchors.emplace_back(AnchorPoint { center_x + radius * std::cos(angle), center_y + radius * std::sin(angle) });
    }
    return anchors;
}

std::vector<MixedFilamentColorMapPanel::AnchorPoint> MixedFilamentColorMapPanel::anchor_points() const
{
    std::vector<AnchorPoint> anchors;
    switch (geometry_mode()) {
    case GeometryMode::Point:
        anchors.emplace_back(AnchorPoint { 0.5, 0.5 });
        break;
    case GeometryMode::Line:
        anchors.emplace_back(AnchorPoint { 0.06, 0.5 });
        anchors.emplace_back(AnchorPoint { 0.94, 0.5 });
        break;
    case GeometryMode::Triangle: {
        const auto vertices = simplex_vertices();
        for (const Vec2 &vertex : vertices)
            anchors.emplace_back(AnchorPoint { vertex.x, vertex.y });
        break;
    }
    case GeometryMode::TriangleWithCenter: {
        const auto vertices = simplex_vertices();
        for (const Vec2 &vertex : vertices)
            anchors.emplace_back(AnchorPoint { vertex.x, vertex.y });
        const Vec2 center = simplex_center();
        anchors.emplace_back(AnchorPoint { center.x, center.y });
        break;
    }
    case GeometryMode::Radial:
        anchors = radial_anchor_points();
        break;
    }
    return anchors;
}

std::array<double, 3> MixedFilamentColorMapPanel::triangle_barycentric(const Vec2 &point, const std::array<Vec2, 3> &triangle)
{
    const Vec2 &a = triangle[0];
    const Vec2 &b = triangle[1];
    const Vec2 &c = triangle[2];
    const double denom = ((b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y));
    if (std::abs(denom) <= 1e-9)
        return { 1.0, 0.0, 0.0 };
    const double w0 = ((b.y - c.y) * (point.x - c.x) + (c.x - b.x) * (point.y - c.y)) / denom;
    const double w1 = ((c.y - a.y) * (point.x - c.x) + (a.x - c.x) * (point.y - c.y)) / denom;
    const double w2 = 1.0 - w0 - w1;
    return { w0, w1, w2 };
}

bool MixedFilamentColorMapPanel::point_in_triangle(const Vec2 &point, const std::array<Vec2, 3> &triangle)
{
    const auto barycentric = triangle_barycentric(point, triangle);
    constexpr double eps = 1e-6;
    return barycentric[0] >= -eps && barycentric[1] >= -eps && barycentric[2] >= -eps;
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::closest_point_on_segment(const Vec2 &point, const Vec2 &start, const Vec2 &end)
{
    const Vec2 edge = sub_vec(end, start);
    const double edge_len_sq = length_sq(edge);
    if (edge_len_sq <= 1e-9)
        return start;
    const double t = std::clamp(dot_vec(sub_vec(point, start), edge) / edge_len_sq, 0.0, 1.0);
    return add_vec(start, scale_vec(edge, t));
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::closest_point_on_triangle(const Vec2 &point, const std::array<Vec2, 3> &triangle)
{
    if (point_in_triangle(point, triangle))
        return point;

    Vec2 best = triangle[0];
    double best_dist = std::numeric_limits<double>::max();
    for (int edge_idx = 0; edge_idx < 3; ++edge_idx) {
        const Vec2 candidate = closest_point_on_segment(point, triangle[edge_idx], triangle[(edge_idx + 1) % 3]);
        const double candidate_dist = dist_sq(point, candidate);
        if (candidate_dist < best_dist) {
            best_dist = candidate_dist;
            best = candidate;
        }
    }
    return best;
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::normalized_point_from_mouse(const wxMouseEvent &evt) const
{
    const wxRect rect = canvas_rect();
    const int width = std::max(1, rect.GetWidth() - 1);
    const int height = std::max(1, rect.GetHeight() - 1);
    return make_vec(
        std::clamp(double(evt.GetX() - rect.GetLeft()) / double(width), 0.0, 1.0),
        std::clamp(double(evt.GetY() - rect.GetTop()) / double(height), 0.0, 1.0));
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::clamp_point_to_geometry(const Vec2 &point) const
{
    switch (geometry_mode()) {
    case GeometryMode::Point:
        return make_vec(0.5, 0.5);
    case GeometryMode::Line:
        return make_vec(std::clamp(point.x, 0.0, 1.0), 0.5);
    case GeometryMode::Triangle:
    case GeometryMode::TriangleWithCenter:
        return closest_point_on_triangle(point, simplex_vertices());
    case GeometryMode::Radial:
        return make_vec(std::clamp(point.x, 0.0, 1.0), std::clamp(point.y, 0.0, 1.0));
    }
    return point;
}

std::vector<double> MixedFilamentColorMapPanel::simplex_weights_from_pos(const Vec2 &point) const
{
    const auto triangle = simplex_vertices();
    const Vec2 clamped = closest_point_on_triangle(point, triangle);
    const auto barycentric = triangle_barycentric(clamped, triangle);

    if (geometry_mode() == GeometryMode::Triangle)
        return { std::max(0.0, barycentric[0]), std::max(0.0, barycentric[1]), std::max(0.0, barycentric[2]) };

    const double shared = std::max(0.0, std::min({ barycentric[0], barycentric[1], barycentric[2] }));
    return {
        std::max(0.0, barycentric[0] - shared),
        std::max(0.0, barycentric[1] - shared),
        std::max(0.0, barycentric[2] - shared),
        std::max(0.0, shared * 3.0)
    };
}

MixedFilamentColorMapPanel::Vec2 MixedFilamentColorMapPanel::triangle_point_from_weights() const
{
    const auto vertices = simplex_vertices();
    double total = 0.0;
    for (size_t idx = 0; idx < 3 && idx < m_weights.size(); ++idx)
        total += std::max(0, m_weights[idx]);
    if (total <= 0.0)
        return simplex_center();

    Vec2 out = make_vec(0.0, 0.0);
    for (size_t idx = 0; idx < 3 && idx < m_weights.size(); ++idx) {
        const double weight = double(std::max(0, m_weights[idx])) / total;
        out = add_vec(out, scale_vec(vertices[idx], weight));
    }
    return out;
}

void MixedFilamentColorMapPanel::initialize_cursor_from_grid_search()
{
    double best_x = 0.5;
    double best_y = 0.5;
    double best_error = std::numeric_limits<double>::max();
    constexpr int grid = 96;
    for (int y_idx = 0; y_idx <= grid; ++y_idx) {
        for (int x_idx = 0; x_idx <= grid; ++x_idx) {
            const Vec2 point = clamp_point_to_geometry(make_vec(double(x_idx) / double(grid), double(y_idx) / double(grid)));
            const std::vector<int> probe = normalized_weights_from_pos(point.x, point.y);
            if (probe.size() != m_weights.size())
                continue;
            double error = 0.0;
            for (size_t idx = 0; idx < probe.size(); ++idx) {
                const double delta = double(probe[idx] - m_weights[idx]);
                error += delta * delta;
            }
            if (error < best_error) {
                best_error = error;
                best_x = point.x;
                best_y = point.y;
            }
        }
    }
    m_cursor_x = best_x;
    m_cursor_y = best_y;
    m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
}

std::vector<double> MixedFilamentColorMapPanel::raw_weights_from_pos(double normalized_x, double normalized_y) const
{
    switch (geometry_mode()) {
    case GeometryMode::Point:
        return { 1.0 };
    case GeometryMode::Line: {
        const double t = std::clamp(normalized_x, 0.0, 1.0);
        return { 1.0 - t, t };
    }
    case GeometryMode::Triangle:
    case GeometryMode::TriangleWithCenter:
        return simplex_weights_from_pos(make_vec(normalized_x, normalized_y));
    case GeometryMode::Radial:
        break;
    }

    const std::vector<AnchorPoint> anchors = radial_anchor_points();
    std::vector<double> out(anchors.size(), 0.0);
    if (anchors.empty())
        return out;

    constexpr double eps = 1e-8;
    size_t exact_idx = size_t(-1);
    for (size_t idx = 0; idx < anchors.size(); ++idx) {
        const double dx = normalized_x - anchors[idx].x;
        const double dy = normalized_y - anchors[idx].y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= eps) {
            exact_idx = idx;
            break;
        }
        out[idx] = 1.0 / std::max(1e-6, d2);
    }
    if (exact_idx != size_t(-1)) {
        std::fill(out.begin(), out.end(), 0.0);
        out[exact_idx] = 1.0;
        return out;
    }

    double sum = 0.0;
    for (const double value : out)
        sum += value;
    if (sum <= 0.0) {
        out.assign(out.size(), 0.0);
        out[0] = 1.0;
        return out;
    }
    for (double &value : out)
        value /= sum;
    return out;
}

std::vector<int> MixedFilamentColorMapPanel::normalized_weights_from_pos(double normalized_x, double normalized_y) const
{
    std::vector<int> raw_weights;
    const std::vector<double> raw = raw_weights_from_pos(normalized_x, normalized_y);
    raw_weights.reserve(raw.size());
    for (const double value : raw)
        raw_weights.emplace_back(std::max(0, int(std::lround(value * 100.0))));
    return normalize_color_match_weights(raw_weights, raw.size());
}

void MixedFilamentColorMapPanel::initialize_cursor_from_weights()
{
    if (m_weights.empty()) {
        m_cursor_x = 0.5;
        m_cursor_y = 0.5;
        return;
    }

    switch (geometry_mode()) {
    case GeometryMode::Point:
        m_cursor_x = 0.5;
        m_cursor_y = 0.5;
        break;
    case GeometryMode::Line: {
        const int total = std::accumulate(m_weights.begin(), m_weights.end(), 0);
        const double t = total > 0 && m_weights.size() >= 2 ? double(std::max(0, m_weights[1])) / double(total) : 0.5;
        m_cursor_x = std::clamp(t, 0.0, 1.0);
        m_cursor_y = 0.5;
        m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
        break;
    }
    case GeometryMode::Triangle: {
        const Vec2 point = triangle_point_from_weights();
        m_cursor_x = point.x;
        m_cursor_y = point.y;
        m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
        break;
    }
    case GeometryMode::TriangleWithCenter:
    case GeometryMode::Radial:
        initialize_cursor_from_grid_search();
        break;
    }
}

// ---------------------------------------------------------------------------
// Private: interaction + rendering
// ---------------------------------------------------------------------------

void MixedFilamentColorMapPanel::emit_changed()
{
    wxCommandEvent evt(wxEVT_SLIDER, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

void MixedFilamentColorMapPanel::update_from_mouse(const wxMouseEvent &evt, bool notify)
{
    const Vec2 point = clamp_point_to_geometry(normalized_point_from_mouse(evt));
    m_cursor_x = point.x;
    m_cursor_y = point.y;
    m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
    Refresh();
    if (notify)
        emit_changed();
}

wxColour MixedFilamentColorMapPanel::canvas_background_color() const
{
    return GetBackgroundColour().IsOk() ? GetBackgroundColour() : wxColour(245, 245, 245);
}

bool MixedFilamentColorMapPanel::cached_bitmap_matches(const wxSize &size, const wxColour &background) const
{
    return m_cached_bitmap.IsOk() && m_cached_bitmap_size == size && m_cached_background == background;
}

void MixedFilamentColorMapPanel::schedule_cached_bitmap_render()
{
    if (!m_render_timer.IsRunning())
        m_render_timer.StartOnce(80);
}

void MixedFilamentColorMapPanel::invalidate_cached_bitmap()
{
    m_cached_bitmap = wxBitmap();
    m_cached_bitmap_size = wxSize();
    m_cached_background = wxColour();
}

void MixedFilamentColorMapPanel::render_cached_bitmap(const wxSize &size, const wxColour &background)
{
    const int width = size.GetWidth();
    const int height = size.GetHeight();
    if (width <= 0 || height <= 0)
        return;

    wxImage image(width, height);
    unsigned char *data = image.GetData();
    if (data != nullptr) {
        for (int y = 0; y < height; ++y) {
            const double normalized_y = (height > 1) ? double(y) / double(height - 1) : 0.5;
            for (int x = 0; x < width; ++x) {
                const double normalized_x = (width > 1) ? double(x) / double(width - 1) : 0.5;
                const int data_idx = (y * width + x) * 3;
                bool paint_pixel = true;
                if (geometry_mode() == GeometryMode::Triangle || geometry_mode() == GeometryMode::TriangleWithCenter)
                    paint_pixel = point_in_triangle(make_vec(normalized_x, normalized_y), simplex_vertices());

                const std::vector<double> raw_weights = raw_weights_from_pos(normalized_x, normalized_y);
                wxColour color = paint_pixel ? blend_multi_filament_mixer(m_colors, raw_weights) : background;
                if (paint_pixel && m_min_component_percent > 0 &&
                    !color_match_raw_weights_within_range(raw_weights, m_min_component_percent)) {
                    const bool stripe = (((x + y) / 8) % 2) == 0;
                    const double factor = stripe ? 0.12 : 0.38;
                    color = wxColour(
                        static_cast<unsigned char>(std::clamp(int(std::lround(double(color.Red()) * factor)), 0, 255)),
                        static_cast<unsigned char>(std::clamp(int(std::lround(double(color.Green()) * factor)), 0, 255)),
                        static_cast<unsigned char>(std::clamp(int(std::lround(double(color.Blue()) * factor)), 0, 255)));
                }
                data[data_idx + 0] = color.Red();
                data[data_idx + 1] = color.Green();
                data[data_idx + 2] = color.Blue();
            }
        }
    }

    m_cached_bitmap = wxBitmap(image);
    m_cached_bitmap_size = size;
    m_cached_background = background;
}

void MixedFilamentColorMapPanel::draw_cached_bitmap(wxAutoBufferedPaintDC &dc, const wxRect &rect)
{
    if (!m_cached_bitmap.IsOk())
        return;

    if (m_cached_bitmap_size == rect.GetSize()) {
        dc.DrawBitmap(m_cached_bitmap, rect.GetLeft(), rect.GetTop(), false);
        return;
    }

    wxMemoryDC memdc;
    memdc.SelectObject(m_cached_bitmap);
    dc.StretchBlit(rect.GetLeft(), rect.GetTop(), rect.GetWidth(), rect.GetHeight(),
                   &memdc, 0, 0, m_cached_bitmap_size.GetWidth(), m_cached_bitmap_size.GetHeight());
    memdc.SelectObject(wxNullBitmap);
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void MixedFilamentColorMapPanel::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const wxRect rect = canvas_rect();
    const int width = rect.GetWidth();
    const int height = rect.GetHeight();
    if (width <= 0 || height <= 0)
        return;

    const wxColour background = canvas_background_color();
    if (!cached_bitmap_matches(rect.GetSize(), background)) {
        if (!m_cached_bitmap.IsOk())
            render_cached_bitmap(rect.GetSize(), background);
        else
            schedule_cached_bitmap_render();
    }
    draw_cached_bitmap(dc, rect);

    if (geometry_mode() == GeometryMode::Triangle || geometry_mode() == GeometryMode::TriangleWithCenter) {
        const auto triangle = simplex_vertices();
        wxPoint points[3] = {
            wxPoint(rect.GetLeft() + int(std::lround(triangle[0].x * double(std::max(1, width - 1)))),
                    rect.GetTop()  + int(std::lround(triangle[0].y * double(std::max(1, height - 1))))),
            wxPoint(rect.GetLeft() + int(std::lround(triangle[1].x * double(std::max(1, width - 1)))),
                    rect.GetTop()  + int(std::lround(triangle[1].y * double(std::max(1, height - 1))))),
            wxPoint(rect.GetLeft() + int(std::lround(triangle[2].x * double(std::max(1, width - 1)))),
                    rect.GetTop()  + int(std::lround(triangle[2].y * double(std::max(1, height - 1)))))
        };
        dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawPolygon(3, points);
        if (geometry_mode() == GeometryMode::TriangleWithCenter) {
            const Vec2 center = simplex_center();
            const wxPoint center_pt(rect.GetLeft() + int(std::lround(center.x * double(std::max(1, width - 1)))),
                                    rect.GetTop()  + int(std::lround(center.y * double(std::max(1, height - 1)))));
            dc.SetPen(wxPen(wxColour(180, 180, 180), 1, wxPENSTYLE_DOT));
            for (const wxPoint &vertex : points)
                dc.DrawLine(center_pt, vertex);
        }
    } else {
        dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);
    }

    dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    const auto anchors = anchor_points();
    for (size_t idx = 0; idx < anchors.size() && idx < m_colors.size(); ++idx) {
        const int anchor_x = rect.GetLeft() + int(std::lround(anchors[idx].x * double(std::max(1, width - 1))));
        const int anchor_y = rect.GetTop() + int(std::lround(anchors[idx].y * double(std::max(1, height - 1))));
        dc.SetPen(wxPen(wxColour(30, 30, 30), 1));
        dc.SetBrush(wxBrush(m_colors[idx]));
        dc.DrawCircle(wxPoint(anchor_x, anchor_y), FromDIP(4));
    }

    const int cursor_x = rect.GetLeft() + int(std::lround(m_cursor_x * double(std::max(1, width - 1))));
    const int cursor_y = rect.GetTop() + int(std::lround(m_cursor_y * double(std::max(1, height - 1))));
    dc.SetPen(wxPen(wxColour(255, 255, 255), 3));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawCircle(wxPoint(cursor_x, cursor_y), FromDIP(7));
    dc.SetPen(wxPen(wxColour(30, 30, 30), 1));
    dc.DrawCircle(wxPoint(cursor_x, cursor_y), FromDIP(7));
}

void MixedFilamentColorMapPanel::on_left_down(wxMouseEvent &evt)
{
    if (!HasCapture())
        CaptureMouse();
    m_dragging = true;
    update_from_mouse(evt, true);
}

void MixedFilamentColorMapPanel::on_left_up(wxMouseEvent &evt)
{
    if (m_dragging)
        update_from_mouse(evt, true);
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
}

void MixedFilamentColorMapPanel::on_mouse_move(wxMouseEvent &evt)
{
    if (m_dragging && evt.LeftIsDown())
        update_from_mouse(evt, true);
}

void MixedFilamentColorMapPanel::on_capture_lost(wxMouseCaptureLostEvent &)
{
    m_dragging = false;
}

void MixedFilamentColorMapPanel::on_size(wxSizeEvent &evt)
{
    if (m_cached_bitmap.IsOk())
        schedule_cached_bitmap_render();
    Refresh(false);
    evt.Skip();
}

void MixedFilamentColorMapPanel::on_render_timer(wxTimerEvent &)
{
    const wxRect rect = canvas_rect();
    render_cached_bitmap(rect.GetSize(), canvas_background_color());
    Refresh(false);
}

} } // namespace Slic3r::GUI
