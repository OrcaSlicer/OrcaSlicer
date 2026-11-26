#include "NavigatorGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "PreviewOverlayLayout.hpp"
#include "I18N.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

static constexpr double CHAMFER = 0.25;
static constexpr double CORNER  = 0.75;
static constexpr double EPS     = 1e-9;

struct Plane
{
    Vec3d       normal;
    double      offset;
    NavZoneKind kind;
};

static std::vector<Plane> cube_planes()
{
    std::vector<Plane> planes;
    for (int axis = 0; axis < 3; ++axis)
        for (const double sign : {1., -1.}) {
            Vec3d n  = Vec3d::Zero();
            n[axis]  = sign;
            planes.push_back({n, 1., NavZoneKind::Face});
        }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            for (const double sa : {1., -1.})
                for (const double sb : {1., -1.}) {
                    Vec3d n = Vec3d::Zero();
                    n[a]    = sa;
                    n[b]    = sb;
                    planes.push_back({n, 2. - CHAMFER, NavZoneKind::Edge});
                }
    for (const double sz : {1., -1.})
        for (const double sy : {1., -1.})
            for (const double sx : {1., -1.})
                planes.push_back({Vec3d(sx, sy, sz), 3. - CORNER, NavZoneKind::Corner});
    return planes;
}

static std::vector<NavZone> build_zones()
{
    const std::vector<Plane> planes = cube_planes();
    std::vector<NavZone>     zones;
    for (size_t i = 0; i < planes.size(); ++i) {
        std::vector<Vec3d> points;
        for (size_t j = 0; j < planes.size(); ++j)
            for (size_t k = j + 1; k < planes.size(); ++k) {
                if (j == i || k == i)
                    continue;
                Eigen::Matrix3d m;
                m.row(0) = planes[i].normal;
                m.row(1) = planes[j].normal;
                m.row(2) = planes[k].normal;
                if (std::abs(m.determinant()) < EPS)
                    continue;
                const Vec3d p = m.inverse() * Vec3d(planes[i].offset, planes[j].offset, planes[k].offset);
                if (std::any_of(planes.begin(), planes.end(), [&p](const Plane& q) { return q.normal.dot(p) > q.offset + 1e-7; }))
                    continue;
                if (std::none_of(points.begin(), points.end(), [&p](const Vec3d& q) { return (q - p).squaredNorm() < 1e-12; }))
                    points.push_back(p);
            }
        const Vec3d n = planes[i].normal.normalized();
        Vec3d       centroid = Vec3d::Zero();
        for (const Vec3d& p : points)
            centroid += p;
        centroid /= double(points.size());
        const Vec3d u = (std::abs(n.z()) < 0.9 ? Vec3d::UnitZ().cross(n) : Vec3d::UnitX().cross(n)).normalized();
        const Vec3d w = n.cross(u);
        std::sort(points.begin(), points.end(), [&](const Vec3d& a, const Vec3d& b) {
            return std::atan2((a - centroid).dot(w), (a - centroid).dot(u)) < std::atan2((b - centroid).dot(w), (b - centroid).dot(u));
        });
        zones.push_back({planes[i].kind, n, std::move(points)});
    }
    return zones;
}

const std::vector<NavZone>& navigator_cube_zones()
{
    static const std::vector<NavZone> zones = build_zones();
    return zones;
}

static std::vector<Vec2d> round_corners(const std::vector<Vec2d>& sharp, const std::vector<double>& radii, int steps)
{
    std::vector<Vec2d> out;
    const size_t       n = sharp.size();
    for (size_t i = 0; i < n; ++i) {
        const double radius = radii[i];
        const Vec2d  b     = sharp[i];
        const Vec2d  u     = (sharp[(i + n - 1) % n] - b).normalized();
        const Vec2d  v     = (sharp[(i + 1) % n] - b).normalized();
        const double half  = 0.5 * std::acos(std::clamp(u.dot(v), -1., 1.));
        const double reach = radius / std::tan(half);
        const Vec2d  from  = b + u * reach;
        const Vec2d  to    = b + v * reach;
        const Vec2d  c     = b + (u + v).normalized() * (radius / std::sin(half));
        const double a0    = std::atan2(from.y() - c.y(), from.x() - c.x());
        double       a1    = std::atan2(to.y() - c.y(), to.x() - c.x());
        double       sweep = a1 - a0;
        while (sweep > PI)
            sweep -= 2. * PI;
        while (sweep < -PI)
            sweep += 2. * PI;
        for (int s = 0; s <= steps; ++s) {
            const double a = a0 + sweep * double(s) / double(steps);
            out.emplace_back(c.x() + radius * std::cos(a), c.y() + radius * std::sin(a));
        }
    }
    return out;
}

static std::vector<Vec3d> build_bed_outline()
{
    const double             h = NAV_BED_HALF;
    const std::vector<Vec2d> sharp = {{-h, -h},          {-0.55, -h},      {-0.34, -h - 0.21}, {0.34, -h - 0.21},
                                      {0.55, -h},        {h, -h},          {h, h},             {-h, h}};
    std::vector<Vec3d>       outline;
    constexpr double         outer = 0.16;
    constexpr double         tab   = 0.04;
    const std::vector<double> radii = {outer, tab, tab, tab, tab, outer, outer, outer};
    for (const Vec2d& p : round_corners(sharp, radii, 6))
        outline.emplace_back(p.x(), p.y(), NAV_BED_TOP);
    return outline;
}

const std::vector<Vec3d>& navigator_bed_outline()
{
    static const std::vector<Vec3d> outline = build_bed_outline();
    return outline;
}

Vec2d NavProjection::project(const Vec3d& p) const
{
    const Vec3d  v = rotation * p;
    const double f = eye_distance > 0. ? eye_distance / (eye_distance - v.z()) : 1.;
    return {center.x() + scale * f * v.x(), center.y() - scale * f * v.y()};
}

Vec3d NavProjection::to_camera() const { return rotation.row(2).transpose(); }

Vec3d NavProjection::toward_camera(const Vec3d& p) const
{
    if (eye_distance <= 0.)
        return to_camera();
    return Vec3d(to_camera() * eye_distance - p).normalized();
}

bool navigator_zone_visible(const NavZone& zone, const NavProjection& projection)
{
    return zone.normal.dot(projection.toward_camera(zone.polygon.front())) > 1e-6;
}

bool navigator_point_in_polygon(const std::vector<Vec2d>& polygon, const Vec2d& point)
{
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2d& a = polygon[i];
        const Vec2d& b = polygon[j];
        if ((a.y() > point.y()) != (b.y() > point.y()) && point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

int navigator_zone_at(const NavProjection& projection, const Vec2d& point)
{
    const std::vector<NavZone>& zones = navigator_cube_zones();
    for (size_t i = 0; i < zones.size(); ++i) {
        if (!navigator_zone_visible(zones[i], projection))
            continue;
        std::vector<Vec2d> polygon;
        for (const Vec3d& p : zones[i].polygon)
            polygon.push_back(projection.project(p));
        if (navigator_point_in_polygon(polygon, point))
            return int(i);
    }
    return -1;
}

float navigator_label_opacity(const Vec3d& normal, const Vec3d& to_camera)
{
    return float(std::clamp((normal.dot(to_camera) - 0.30) / 0.30, 0., 1.));
}

float navigator_bed_opacity(const Vec3d& to_camera)
{
    const double below = -to_camera.normalized().z();
    return float(1. - .75 * std::clamp(below / std::sin(25. * PI / 180.), 0., 1.));
}

NavView navigator_zone_view(size_t zone)
{
    const Vec3d n = navigator_cube_zones()[zone].normal;
    NavView     view;
    view.to_camera = n;
    if (std::abs(n.z()) > 1. - 1e-9)
        view.up = n.z() > 0. ? Vec3d::UnitY() : Vec3d(-Vec3d::UnitY());
    else
        view.up = (Vec3d::UnitZ() - n * n.z()).normalized();
    return view;
}

bool navigator_bed_at(const NavProjection& projection, const Vec2d& point)
{
    std::vector<Vec2d> top;
    std::vector<Vec2d> bottom;
    for (const Vec3d& v : navigator_bed_outline()) {
        top.push_back(projection.project(v));
        bottom.push_back(projection.project(v - Vec3d(0., 0., NAV_BED_THICKNESS)));
    }
    return navigator_point_in_polygon(top, point) || navigator_point_in_polygon(bottom, point);
}

static const NavZoneName ZONE_NAMES[] = {
    {1, 0, 0, L_CONTEXT("Right", "Camera View"), "Camera View"},
    {-1, 0, 0, L_CONTEXT("Left", "Camera View"), "Camera View"},
    {0, 1, 0, L_CONTEXT("Rear", "Camera View"), "Camera View"},
    {0, -1, 0, L_CONTEXT("Front", "Camera View"), "Camera View"},
    {0, 0, 1, L_CONTEXT("Top", "Camera View"), "Camera View"},
    {0, 0, -1, L_CONTEXT("Bottom", "Camera View"), "Camera View"},
    {1, -1, 0, L_CONTEXT("Front right", "Navigator"), "Navigator"},
    {-1, -1, 0, L_CONTEXT("Front left", "Navigator"), "Navigator"},
    {1, 1, 0, L_CONTEXT("Rear right", "Navigator"), "Navigator"},
    {-1, 1, 0, L_CONTEXT("Rear left", "Navigator"), "Navigator"},
    {0, -1, 1, L_CONTEXT("Top front", "Navigator"), "Navigator"},
    {0, 1, 1, L_CONTEXT("Top rear", "Navigator"), "Navigator"},
    {-1, 0, 1, L_CONTEXT("Top left", "Navigator"), "Navigator"},
    {1, 0, 1, L_CONTEXT("Top right", "Navigator"), "Navigator"},
    {0, -1, -1, L_CONTEXT("Bottom front", "Navigator"), "Navigator"},
    {0, 1, -1, L_CONTEXT("Bottom rear", "Navigator"), "Navigator"},
    {-1, 0, -1, L_CONTEXT("Bottom left", "Navigator"), "Navigator"},
    {1, 0, -1, L_CONTEXT("Bottom right", "Navigator"), "Navigator"},
    {1, -1, 1, L_CONTEXT("Top front right", "Navigator"), "Navigator"},
    {-1, -1, 1, L_CONTEXT("Top front left", "Navigator"), "Navigator"},
    {1, 1, 1, L_CONTEXT("Top rear right", "Navigator"), "Navigator"},
    {-1, 1, 1, L_CONTEXT("Top rear left", "Navigator"), "Navigator"},
    {1, -1, -1, L_CONTEXT("Bottom front right", "Navigator"), "Navigator"},
    {-1, -1, -1, L_CONTEXT("Bottom front left", "Navigator"), "Navigator"},
    {1, 1, -1, L_CONTEXT("Bottom rear right", "Navigator"), "Navigator"},
    {-1, 1, -1, L_CONTEXT("Bottom rear left", "Navigator"), "Navigator"},
};

static int sign(double v) { return v > 1e-6 ? 1 : v < -1e-6 ? -1 : 0; }

const NavZoneName& navigator_zone_name(size_t zone)
{
    const Vec3d& n = navigator_cube_zones()[zone].normal;
    for (const NavZoneName& name : ZONE_NAMES)
        if (name.x == sign(n.x()) && name.y == sign(n.y()) && name.z == sign(n.z()))
            return name;
    return ZONE_NAMES[0];
}

Eigen::Quaterniond navigator_view_rotation(const NavView& view)
{
    Eigen::Matrix3d r;
    r.row(1) = view.up.normalized();
    r.row(2) = view.to_camera.normalized();
    r.row(0) = r.row(1).cross(r.row(2));
    return Eigen::Quaterniond(r).normalized();
}

NavProjection navigator_projection(const CanvasUi::Rect& area, const Eigen::Matrix3d& rotation)
{
    NavProjection projection;
    projection.rotation     = rotation;
    projection.eye_distance = NAV_EYE_DISTANCE;
    projection.scale    = std::min(area.width() / 4.0, area.height() / 3.35);
    projection.center   = Vec2d(0.5 * (area.left + area.right), 0.5 * (area.top + area.bottom));
    // Centred on a point a little under the cube, toward the bed, wherever the view puts the bed.
    projection.center -= projection.project(Vec3d(0., 0., -0.55)) - projection.center;
    return projection;
}

static constexpr auto MARGIN          = CanvasUi::space<16>();
static constexpr auto TOOLBAR_PADDING = CanvasUi::space<4>();
static constexpr auto TOOLBAR_HEIGHT  = CanvasUi::space<40>();
static constexpr auto CUBE_WIDTH      = CanvasUi::space<200>();
static constexpr auto CUBE_HEIGHT     = CanvasUi::space<160>();
static constexpr auto CUBE_GAP        = CanvasUi::space<8>();
static constexpr auto CARD_WIDTH      = CanvasUi::space<232>();
static constexpr auto CARD_HEIGHT     = CanvasUi::space<228>();
static constexpr auto CARD_SIDE       = CanvasUi::space<14>();
static constexpr auto CARD_TOP        = CanvasUi::space<10>();
static constexpr auto SECTION_SIZE    = CanvasUi::space<40>();
static constexpr auto SECTION_GAP     = CanvasUi::space<8>();
static constexpr auto SEPARATOR_SIDE  = CanvasUi::space<4>();
static constexpr auto SEPARATOR       = CanvasUi::space<20>();

float NavLayout::top() const
{
    float t = section.top;
    for (const CanvasUi::Rect& part : {toolbar, card, cube})
        if (part.width() > 0.f)
            t = std::min(t, part.top);
    return t;
}

NavLayout navigator_layout(bool background, float scale, float scene_bottom)
{
    const CanvasUi::Metrics m(scale);
    const float             left   = m.px(MARGIN);
    const float             bottom = scene_bottom - m.px(MARGIN);
    NavLayout               l;
    // The buttons, the separator before the last one.
    std::vector<CanvasUi::RowSlot> slots(NAV_BUTTON_COUNT, CanvasUi::RowSlot{m.icon_button});
    slots.insert(slots.end() - 1, CanvasUi::RowSlot{2.f * m.px(SEPARATOR_SIDE) + m.outline});
    const float row = CanvasUi::row_width(m.icon_button_gap, slots);

    l.background = background;
    if (l.background) {
        l.card  = {left, bottom - m.px(CARD_HEIGHT), left + m.px(CARD_WIDTH), bottom};
        l.toolbar = {l.card.left, bottom - m.px(TOOLBAR_HEIGHT), l.card.right, bottom};
        l.cube  = {l.card.left + m.px(CARD_SIDE), l.card.top + m.px(CARD_TOP), l.card.right - m.px(CARD_SIDE), l.toolbar.top - m.px(CARD_TOP)};
    } else {
        l.toolbar = {left, bottom - m.px(TOOLBAR_HEIGHT), left + row + 2.f * m.px(TOOLBAR_PADDING), bottom};
        const float center = 0.5f * (l.toolbar.left + l.toolbar.right);
        const float base   = l.toolbar.top - m.px(CUBE_GAP);
        l.cube             = {center - 0.5f * m.px(CUBE_WIDTH), base - m.px(CUBE_HEIGHT), center + 0.5f * m.px(CUBE_WIDTH), base};
    }
    const float frame_right = l.background ? l.card.right : l.toolbar.right;
    l.section               = {frame_right + m.px(SECTION_GAP), bottom - m.px(SECTION_SIZE), frame_right + (m.px(SECTION_GAP) + m.px(SECTION_SIZE)), bottom};

    const float toolbar_left = std::round(0.5f * (l.toolbar.left + l.toolbar.right - row));
    const std::vector<CanvasUi::RowSpan> spans = CanvasUi::row_layout(toolbar_left, toolbar_left + row, m.icon_button_gap, slots);
    const float y     = std::round(0.5f * (l.toolbar.top + l.toolbar.bottom - m.icon_button));
    for (size_t i = 0; i < NAV_BUTTON_COUNT; ++i) {
        const CanvasUi::RowSpan& span = spans[i < NAV_BUTTON_COUNT - 1 ? i : i + 1];
        l.buttons[i]               = {span.left, y, span.right(), y + m.icon_button};
    }
    const CanvasUi::RowSpan& separator = spans[NAV_BUTTON_COUNT - 1];
    const float           mid       = 0.5f * (l.toolbar.top + l.toolbar.bottom);
    const float           line_left = std::round(separator.left + m.px(SEPARATOR_SIDE));
    const float           line_top  = std::round(mid - 0.5f * m.px(SEPARATOR));
    l.separator = {line_left, line_top, line_left + m.outline, line_top + std::round(m.px(SEPARATOR))};
    return l;
}

bool navigator_turn_interrupted(const Eigen::Quaterniond& camera, const Eigen::Quaterniond& last_set, int frames_since_last_step)
{
    return camera.angularDistance(last_set) > 1e-4 || frames_since_last_step > 1;
}

bool navigator_drag_starts(const NavPress& press, const Vec2d& mouse, double threshold)
{
    return !press.dragging && (mouse - press.position).norm() > threshold;
}

std::optional<size_t> navigator_released_zone(const NavPress& press)
{
    if (press.dragging || press.zone < 0)
        return std::nullopt;
    return size_t(press.zone);
}

std::optional<Eigen::Quaterniond> navigator_view_from_string(const std::string& text)
{
    std::istringstream stream(text);
    double             w, x, y, z;
    if (!(stream >> w >> x >> y >> z))
        return std::nullopt;
    const Eigen::Quaterniond q(w, x, y, z);
    if (std::abs(q.norm() - 1.) >= 1e-3)
        return std::nullopt;
    return q.normalized();
}

std::string navigator_view_to_string(const Eigen::Quaterniond& view)
{
    std::ostringstream stream;
    stream.precision(9);
    stream << view.w() << ' ' << view.x() << ' ' << view.y() << ' ' << view.z();
    return stream.str();
}

} // namespace GUI
} // namespace Slic3r
