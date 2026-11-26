#include "Navigator3D.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NavigatorGeometry.hpp"
#include "slic3r/GUI/CanvasUi/Button.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/Icons.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Input.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Menu.hpp"
#include "slic3r/GUI/CanvasUi/MenuPopup.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/Corners.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/CanvasUi/Tooltip.hpp"
#include "slic3r/GUI/Shortcuts.hpp"

namespace Slic3r {
namespace GUI {


using CanvasUi::faded;
using CanvasUi::mix;
using CanvasUi::rgb;

namespace {

struct Palette
{
    CanvasUi::Color                face, edge, corner, line, silhouette, label, shadow;
    CanvasUi::Color                bed_top, bed_side, grid, bed_outline, bed_rim;
    std::array<CanvasUi::Color, 3> axes;
    std::array<CanvasUi::Color, 3> edges;
    CanvasUi::Color                badge_text;
    CanvasUi::Color                hover_face, hover_small, hover_line, hover_label;
};

} // namespace

static Palette palette(const CanvasUi::Theme& theme)
{
    const bool dark = theme.dark;
    Palette    p;
    p.face        = rgb(dark ? 0x4c4c55 : 0xffffff);
    p.edge        = rgb(dark ? 0x3d3d43 : 0xeceeef);
    p.corner      = rgb(dark ? 0x43434a : 0xe7e9eb);
    p.line        = rgb(dark ? 0xa0a0a6 : 0x7d8488);
    p.silhouette  = rgb(dark ? 0xb4b4ba : 0x7a8084);
    p.label       = theme.foreground;
    p.shadow      = rgb(0x000000, dark ? .45f : .20f);
    p.bed_top     = rgb(dark ? 0x38383e : 0xd4d8db);
    p.bed_side    = rgb(dark ? 0x232327 : 0xb9bfc3);
    p.grid        = dark ? rgb(0xffffff, .06f) : rgb(0x000000, .07f);
    p.bed_outline = p.silhouette;
    // In the dark theme a light rim under the light edge of the top reads as a second edge.
    p.bed_rim     = dark ? faded(p.silhouette, .35f) : p.silhouette;
    p.axes        = {rgb(dark ? 0xff7a8c : 0xc62f48), rgb(dark ? 0x86d48e : 0x2a7d39), rgb(dark ? 0x79aaff : 0x2a64c8)};
    for (size_t i = 0; i < 3; ++i)
        p.edges[i] = mix(p.edge, p.axes[i], dark ? .32f : .30f);
    p.badge_text  = rgb(dark ? 0x1c1c1f : 0xffffff);
    p.hover_face  = faded(theme.primary, dark ? .34f : .22f);
    p.hover_small = faded(theme.primary, dark ? .70f : .55f);
    p.hover_line  = theme.primary;
    // On the teal of a hovered face the accent text of the dark theme is too faint, so the label takes a light teal.
    p.hover_label = dark ? rgb(0xb2dfdb) : theme.primary_strong;
    return p;
}

namespace {

struct Sizes
{
    explicit Sizes(float scale) : m(scale) {}

    CanvasUi::Metrics m;
    float             drag_threshold{m.px(CanvasUi::space<4>())};
    // The turn of the camera per pixel the mouse drags.
    double            drag_turn{0.01 / m.scale};
    float             edge{m.px(CanvasUi::Stroke::sm)};
    float             bed_outline{m.px(CanvasUi::Stroke::sm)};
    float             silhouette{m.px(CanvasUi::Stroke::md)};
    float             hover_outline{m.px(CanvasUi::Stroke::md)};
    float             shadow_drop{m.px(CanvasUi::space<2>())};
    float             shadow_softness{m.px(CanvasUi::space<4>())};
    float             label{m.px(CanvasUi::TextRole::body.size)};
    float             label_min{m.px(CanvasUi::TextRole::caption.size)};
    // How far the label and the badge stand above and below the centre of the face.
    float             label_shift{m.px(CanvasUi::space<8>())};
    float             badge_half_width{.5f * m.px(CanvasUi::space<26>())};
    float             badge_half_height{.5f * m.px(CanvasUi::space<16>())};
    float             badge_radius{m.px(CanvasUi::Radius::sm)};
    float             badge_text{m.px(CanvasUi::TextRole::caption.size)};
};

} // namespace

static Vec3d bed_center() { return Vec3d(0., 0., NAV_BED_TOP); }

// Edge-on to the bed is seen from above, whatever the rounding left in a view turned to it.
static bool from_below(const NavProjection& projection) { return projection.toward_camera(bed_center()).z() < -1e-6; }

static CanvasUi::Point to_point(const Vec2d& p) { return CanvasUi::Point(float(p.x()), float(p.y())); }

static std::vector<CanvasUi::Point> project(const NavProjection& projection, const std::vector<Vec3d>& points, const Vec3d& offset = Vec3d::Zero())
{
    std::vector<CanvasUi::Point> out;
    out.reserve(points.size());
    for (const Vec3d& p : points)
        out.push_back(to_point(projection.project(p + offset)));
    return out;
}

// Wound as ImGui winds its own shapes, so that the antialiased fringe falls outside.
static std::vector<CanvasUi::Point> convex_hull(const std::vector<CanvasUi::Point>& points)
{
    Points scaled_points;
    scaled_points.reserve(points.size());
    for (const CanvasUi::Point& p : points)
        scaled_points.emplace_back(scaled(double(p.x)), scaled(double(p.y)));
    std::vector<CanvasUi::Point> hull;
    for (const Point& p : Geometry::convex_hull(std::move(scaled_points)).points)
        hull.emplace_back(float(unscaled(p.x())), float(unscaled(p.y())));
    return hull;
}

static void fill_fan(const CanvasUi::Painter& paint, const CanvasUi::Point& center, const std::vector<CanvasUi::Point>& outline, const CanvasUi::Color& col)
{
    for (size_t i = 0; i < outline.size(); ++i)
        paint.fill_convex({center, outline[i], outline[(i + 1) % outline.size()]}, col);
}

static CanvasUi::Point screen_step(const NavProjection& projection, const Vec3d& at, const Vec3d& direction, double length)
{
    const Vec2d a = projection.project(at);
    const Vec2d b = projection.project(at + direction * length);
    return to_point(b - a);
}

static std::string zone_name(size_t zone)
{
    const NavZoneName& name = navigator_zone_name(zone);
    return _u8L_CONTEXT(name.text, name.context);
}

// The shortcut of a face, by the order of the faces in navigator_cube_zones().
static constexpr std::array<Shortcut, NAV_FACE_COUNT> FACE_SHORTCUTS = {Shortcut::ViewRight, Shortcut::ViewLeft, Shortcut::ViewRear,
                                                          Shortcut::ViewFront, Shortcut::ViewTop,  Shortcut::ViewBottom};

static std::string face_shortcut(const NavigatorFrame& frame, size_t zone)
{
    return zone < FACE_SHORTCUTS.size() && frame.shortcut ? frame.shortcut(FACE_SHORTCUTS[zone]) : std::string();
}

// The default view of Orca: from the front, 45 degrees above the bed.
static Eigen::Quaterniond default_view_rotation() { return navigator_view_rotation({Vec3d(0., -1., 1.), Vec3d(0., 1., 1.)}); }

static void set_camera_rotation(Camera& camera, const Eigen::Quaterniond& rotation)
{
    Transform3d m = Transform3d::Identity();
    m.linear()    = rotation.normalized().toRotationMatrix();
    camera.set_rotation(m);
}

Navigator3D::Navigator3D()
{
    static int count = 0;
    m_card_id        = "##navigator" + std::to_string(++count);
}

void Navigator3D::animate_to(Camera& camera, const Eigen::Quaterniond& target, std::function<void()> done)
{
    const CanvasUi::Context& ui   = *CanvasUi::Context::current();
    const Eigen::Quaterniond from = camera.get_view_rotation();
    m_animation = Animation{from, target, ui.now(), std::move(done), from, ui.frame()};
}

void Navigator3D::animate_to_zone(Camera& camera, size_t zone)
{
    camera.auto_type(navigator_cube_zones()[zone].kind == NavZoneKind::Face ? Camera::EType::Ortho : Camera::EType::Perspective);
    animate_to(camera, navigator_view_rotation(navigator_zone_view(zone)));
}

void Navigator3D::rotate_about_z(Camera& camera, double angle)
{
    const Eigen::Quaterniond from = m_animation ? m_animation->to : camera.get_view_rotation();
    animate_to(camera, (from * Eigen::AngleAxisd(angle, Vec3d::UnitZ())).normalized());
}

void Navigator3D::step_animation(Camera& camera)
{
    if (!m_animation)
        return;
    const CanvasUi::Context& ui  = *CanvasUi::Context::current();
    const Clock::time_point  now = ui.now();
    if (navigator_turn_interrupted(camera.get_view_rotation(), m_animation->last, ui.frame() - m_animation->last_frame)) {
        m_animation.reset();
        return;
    }
    const double t = std::chrono::duration<double>(now - m_animation->start).count() /
                     std::chrono::duration<double>(CanvasUi::duration::turn).count();
    if (t >= 1.) {
        set_camera_rotation(camera, m_animation->to);
        const std::function<void()> done = std::move(m_animation->done);
        m_animation.reset();
        if (done)
            done();
    } else {
        const double eased = 1. - std::pow(1. - t, 3.);
        set_camera_rotation(camera, m_animation->from.slerp(eased, m_animation->to));
        m_animation->last       = camera.get_view_rotation();
        m_animation->last_frame = ui.frame();
    }
    // The frame after the last step too, to draw again what the turn held back.
    CanvasUi::Context::current()->request_frame();
}

int Navigator3D::hovered_target(const NavProjection& projection, const Vec2d& mouse, bool& bed) const
{
    bed            = false;
    const int zone = navigator_zone_at(projection, mouse);
    // From below the bed lets the clicks through to the cube.
    if (zone >= 0 || from_below(projection))
        return zone;
    bed = navigator_bed_at(projection, mouse);
    return -1;
}

void Navigator3D::handle_cube_input(Camera& camera, const NavProjection& projection, bool mouse_free, const NavigatorFrame& frame)
{
    const CanvasUi::Pointer pointer = CanvasUi::pointer();
    const Vec2d          mouse(pointer.pos.x, pointer.pos.y);
    if (!m_press) {
        bool      bed  = false;
        const int zone = mouse_free && !frame.blocked ? hovered_target(projection, mouse, bed) : -1;
        if ((zone >= 0 || bed) && pointer.pressed)
            m_press = NavPress{zone, bed, mouse, false};
        return;
    }
    if (pointer.held) {
        const Sizes s(CanvasUi::Context::current()->metrics().scale);
        if (navigator_drag_starts(*m_press, mouse, s.drag_threshold)) {
            m_press->dragging = true;
            m_animation.reset();
            camera.auto_type(Camera::EType::Perspective);
        }
        if (m_press->dragging && (pointer.delta.x != 0.f || pointer.delta.y != 0.f)) {
            const double             step = s.drag_turn;
            const Eigen::Quaterniond view = camera.get_view_rotation();
            if (m_press->bed)
                set_camera_rotation(camera, view * Eigen::AngleAxisd(pointer.delta.x * step, Vec3d::UnitZ()));
            else
                set_camera_rotation(camera, Eigen::AngleAxisd(pointer.delta.y * step, Vec3d::UnitX()) *
                                                Eigen::AngleAxisd(pointer.delta.x * step, Vec3d::UnitY()) * view);
            m_turned = true;
        }
        return;
    }
    if (const std::optional<size_t> zone = navigator_released_zone(*m_press))
        animate_to_zone(camera, *zone);
    m_press.reset();
}

static void draw_bed(const CanvasUi::Painter& paint, const NavProjection& projection, const Palette& pal, const Sizes& s)
{
    const float               opacity = navigator_bed_opacity(projection.toward_camera(bed_center()));
    const std::vector<Vec3d>& outline = navigator_bed_outline();
    const bool                below   = from_below(projection);
    const Vec3d               down(0., 0., -NAV_BED_THICKNESS);
    const std::vector<CanvasUi::Point> top     = project(projection, outline);
    const std::vector<CanvasUi::Point> bottom  = project(projection, outline, down);
    const size_t              n       = outline.size();

    std::vector<bool>   wall_visible(n);
    std::vector<size_t> walls;
    for (size_t i = 0; i < n; ++i) {
        const Vec3d edge   = outline[(i + 1) % n] - outline[i];
        const Vec3d normal = Vec3d(edge.y(), -edge.x(), 0.).normalized();
        wall_visible[i]    = normal.dot(projection.toward_camera(outline[i])) > 1e-6;
        if (wall_visible[i])
            walls.push_back(i);
    }
    std::sort(walls.begin(), walls.end(), [&](size_t a, size_t b) {
        const Vec3d eye = projection.to_camera() * projection.eye_distance;
        return (eye - .5 * (outline[a] + outline[(a + 1) % n])).norm() > (eye - .5 * (outline[b] + outline[(b + 1) % n])).norm();
    });

    const std::vector<CanvasUi::Point>& face   = below ? bottom : top;
    const std::vector<CanvasUi::Point>& rim    = below ? top : bottom;
    const CanvasUi::Point               center = to_point(projection.project(Vec3d(0., 0., NAV_BED_TOP) + (below ? down : Vec3d::Zero())));
    {
        const CanvasUi::Painter::SharpFills sharp(paint);
        for (size_t i : walls) {
            const size_t j = (i + 1) % n;
            paint.fill_convex({top[i], top[j], bottom[j], bottom[i]}, faded(pal.bed_side, opacity));
        }
        fill_fan(paint, center, face, faded(pal.bed_top, opacity));
    }
    if (!below) {
        for (const double t : {-0.65, 0., 0.65}) {
            paint.line(to_point(projection.project(Vec3d(t, -NAV_BED_HALF, NAV_BED_TOP))), to_point(projection.project(Vec3d(t, NAV_BED_HALF, NAV_BED_TOP))), pal.grid, s.edge);
            paint.line(to_point(projection.project(Vec3d(-NAV_BED_HALF, t, NAV_BED_TOP))), to_point(projection.project(Vec3d(NAV_BED_HALF, t, NAV_BED_TOP))), pal.grid, s.edge);
        }
    }

    const CanvasUi::Color line     = faded(pal.bed_outline, opacity);
    const CanvasUi::Color rim_line = faded(pal.bed_rim, opacity);
    paint.polyline(face, line, s.bed_outline, true);
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        if (wall_visible[i])
            paint.line(rim[i], rim[j], rim_line, s.bed_outline);
        if (wall_visible[i] != wall_visible[j])
            paint.line(top[j], bottom[j], rim_line, s.bed_outline);
    }
}

// The axis along the normal of a face, signed, as its badge reads.
static const char* const FACE_BADGES[] = {"+X", "\xE2\x88\x92X", "+Y", "\xE2\x88\x92Y", "+Z", "\xE2\x88\x92Z"};

static void fill_convex(const CanvasUi::Painter& paint, std::vector<CanvasUi::Point> points, const CanvasUi::Color& col)
{
    float area = 0.f;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
        area += points[j].x * points[i].y - points[i].x * points[j].y;
    // Wound as ImGui winds its own shapes, so that the antialiased fringe falls outside.
    if (area < 0.f)
        std::reverse(points.begin(), points.end());
    paint.fill_convex(points, col);
}

static void draw_face_label(const CanvasUi::Painter& paint, const NavProjection& projection, size_t zone, const CanvasUi::Color& col, const Palette& pal,
                            const CanvasUi::Font& font, const Sizes& s)
{
    const NavZone& face    = navigator_cube_zones()[zone];
    const float    opacity = navigator_label_opacity(face.normal, projection.toward_camera(face.normal));
    if (opacity <= 0.f)
        return;
    Vec3d up = Vec3d::UnitZ();
    if (std::abs(face.normal.z()) > .5)
        up = face.normal.z() > 0. ? Vec3d::UnitY() : Vec3d(-Vec3d::UnitY());
    const Vec3d       right = up.cross(face.normal);
    const std::string label = zone_name(zone);
    const float       max_w = float(1.4 * projection.scale);
    float             size  = s.label;
    const float       w     = font.resized(size).width(label);
    if (w > max_w)
        size = std::max(s.label_min, size * max_w / w);
    const double k = 1. / projection.scale;
    const CanvasUi::Point to_right = screen_step(projection, face.normal, right, k);
    const CanvasUi::Point to_down  = screen_step(projection, face.normal, -up, k);
    // The label and the badge under it, a pill of the colour of the axis along the normal, make one block
    // centred on the face.
    const double shift = s.label_shift * k;
    paint.text_in_plane(font.resized(size), label, faded(col, opacity), to_point(projection.project(face.normal + up * shift)), to_right, to_down);
    const CanvasUi::Point center = to_point(projection.project(face.normal - up * shift));
    std::vector<CanvasUi::Point> pill;
    for (const CanvasUi::Point& p : CanvasUi::rounded_rect_outline(
             CanvasUi::Rect(-s.badge_half_width, -s.badge_half_height, s.badge_half_width, s.badge_half_height), s.badge_radius,
             CanvasUi::CORNER_SMOOTHING))
        pill.push_back(CanvasUi::Point(center.x + to_right.x * p.x + to_down.x * p.y, center.y + to_right.y * p.x + to_down.y * p.y));
    const size_t axis = zone / 2;
    fill_convex(paint, pill, faded(pal.axes[axis], opacity));
    paint.text_in_plane(font.resized(s.badge_text), FACE_BADGES[zone], faded(pal.badge_text, opacity), center, to_right, to_down);
}

void Navigator3D::draw_cube(const NavProjection& projection, const NavigatorFrame& frame, int hovered, const CanvasUi::Card& card) const
{
    const CanvasUi::Painter     paint = frame.layout.background ? card.painter() : CanvasUi::Painter::background();
    const Palette               pal   = palette(CanvasUi::Context::current()->theme());
    const Sizes                 s(CanvasUi::Context::current()->metrics().scale);
    const bool                  below = from_below(projection);
    const std::vector<NavZone>& zones = navigator_cube_zones();
    const CanvasUi::Font        font  = CanvasUi::Context::current()->bold_font();

    std::vector<CanvasUi::Point> corners;
    for (const NavZone& zone : zones)
        for (const Vec3d& p : zone.polygon)
            corners.push_back(to_point(projection.project(p)));
    const std::vector<CanvasUi::Point> hull = convex_hull(corners);

    if (!below) {
        draw_bed(paint, projection, pal, s);
        std::vector<CanvasUi::Point> shadow = hull;
        for (CanvasUi::Point& p : shadow)
            p.y += s.shadow_drop;
        paint.fill_convex(shadow, pal.shadow, s.shadow_softness);
    }
    {
        // Without the antialiased fringe, so that zones sharing an edge leave no seam; the lines drawn over the edges smooth them.
        const CanvasUi::Painter::SharpFills sharp(paint);
        for (size_t i = 0; i < zones.size(); ++i) {
            if (!navigator_zone_visible(zones[i], projection))
                continue;
            const std::vector<CanvasUi::Point> poly = project(projection, zones[i].polygon);
            CanvasUi::Color fill = pal.corner;
            if (zones[i].kind == NavZoneKind::Face)
                fill = pal.face;
            else if (zones[i].kind == NavZoneKind::Edge)
                // tinted with the axis the edge runs along, the one its normal has no part of
                fill = pal.edges[std::abs(zones[i].normal.x()) < .5 ? 0 : std::abs(zones[i].normal.y()) < .5 ? 1 : 2];
            paint.fill_convex(poly, fill);
            if (int(i) == hovered)
                paint.fill_convex(poly, zones[i].kind == NavZoneKind::Face ? pal.hover_face : pal.hover_small);
        }
    }
    for (const NavZone& zone : zones)
        if (navigator_zone_visible(zone, projection)) {
            const std::vector<CanvasUi::Point> poly = project(projection, zone.polygon);
            paint.polyline(poly, pal.line, s.edge, true);
        }
    for (size_t i = 0; i < NAV_FACE_COUNT; ++i)
        if (navigator_zone_visible(zones[i], projection))
            draw_face_label(paint, projection, i, int(i) == hovered ? pal.hover_label : pal.label, pal, font, s);
    paint.polyline(hull, pal.silhouette, s.silhouette, true);
    if (hovered >= 0) {
        const std::vector<CanvasUi::Point> poly = project(projection, zones[size_t(hovered)].polygon);
        paint.polyline(poly, pal.hover_line, s.hover_outline, true);
    }

    if (below)
        draw_bed(paint, projection, pal, s);
}

void Navigator3D::cube_tooltip(const NavigatorFrame& frame, int hovered)
{
    if (hovered < 0)
        return;
    const CanvasUi::ScopedId id(hovered);
    CanvasUi::tooltip_for("##cube_zone", zone_name(size_t(hovered)), face_shortcut(frame, size_t(hovered)));
}

static constexpr const char* VIEWS_MENU   = "##navigator_views";
static constexpr const char* DISPLAY_MENU = "##navigator_display";

// Beside the navigator, its bottom on the toolbar's bottom.
static CanvasUi::Positioner menu_anchor(const NavLayout& l)
{
    const CanvasUi::Rect frame = l.background ? l.card : l.toolbar;
    return CanvasUi::Positioner::beside(frame, CanvasUi::Context::current()->metrics().popup_side_offset);
}

namespace {
// After the faces, which take their zone index.
enum ViewsMenuItem { DefaultView = NAV_FACE_COUNT, Perspective, Orthographic, AutoPerspective, SetHome, ResetHome };
} // namespace

void Navigator3D::render_views_menu(Camera& camera, const NavigatorFrame& frame)
{
    using CanvasUi::Indicator;
    using CanvasUi::MenuItem;
    const auto items = [&camera, &frame]() {
        const bool perspective = camera.get_type() == Camera::EType::Perspective;
        std::vector<MenuItem> items = {MenuItem::item(DefaultView, _u8L_CONTEXT("Default view", "Navigator"),
                                                      frame.shortcut ? frame.shortcut(Shortcut::ViewDefault) : std::string())};
        for (const size_t face : {NavTop, NavBottom, NavFront, NavRear, NavLeft, NavRight})
            items.push_back(MenuItem::item(int(face), zone_name(face), face_shortcut(frame, face)));
        MenuItem reset_home = MenuItem::item(ResetHome, _u8L_CONTEXT("Reset home view", "Navigator"));
        // TRN Why "Reset home view" is unavailable: no view was set as home.
        reset_home.availability = CanvasUi::Availability{frame.home_view.has_value(), _u8L_CONTEXT("No home view is set", "Navigator")};
        items.insert(items.end(), {
            MenuItem::separator(),
            MenuItem::radio_item(Perspective, _u8L("Perspective"), perspective),
            MenuItem::radio_item(Orthographic, _u8L("Orthographic"), !perspective),
            MenuItem::separator(),
            MenuItem::checkbox_item(AutoPerspective, _u8L_CONTEXT("Auto perspective", "Navigator"), frame.auto_perspective),
            MenuItem::separator(),
            MenuItem::item(SetHome, _u8L_CONTEXT("Set current view as home", "Navigator")),
            reset_home,
        });
        return items;
    };

    const std::optional<int> picked = CanvasUi::menu_popup(VIEWS_MENU, menu_anchor(frame.layout), items);
    if (!picked)
        return;
    switch (*picked) {
    case DefaultView:
        camera.auto_type(Camera::EType::Perspective);
        animate_to(camera, default_view_rotation(), frame.frame_bed);
        break;
    case Perspective:
    case Orthographic:
        if (frame.set_perspective)
            frame.set_perspective(*picked == Perspective);
        break;
    case AutoPerspective:
        if (frame.toggle_auto_perspective)
            frame.toggle_auto_perspective();
        break;
    case SetHome:
    case ResetHome:
        if (frame.set_home_view)
            frame.set_home_view(*picked == SetHome ? std::optional<Eigen::Quaterniond>(camera.get_view_rotation()) : std::nullopt);
        break;
    default: animate_to_zone(camera, size_t(*picked));
    }
}

void Navigator3D::render_display_menu(const NavigatorFrame& frame)
{
    std::vector<std::function<void()>> actions;
    const auto items = [&]() {
        std::vector<CanvasUi::MenuItem> items;
        for (const NavigatorToggleGroup& group : frame.display_toggles ? frame.display_toggles() : std::vector<NavigatorToggleGroup>()) {
            items.push_back(CanvasUi::MenuItem::group_label(group.title));
            for (const NavigatorToggle& toggle : group.toggles) {
                CanvasUi::MenuItem item = CanvasUi::MenuItem::checkbox_item(int(actions.size()), toggle.label, toggle.checked,
                                                                      toggle.indicator);
                item.availability = toggle.availability;
                items.push_back(std::move(item));
                actions.push_back(toggle.toggle);
            }
        }
        return items;
    };
    const std::optional<int> picked = CanvasUi::menu_popup(DISPLAY_MENU, menu_anchor(frame.layout), items);
    if (picked && actions[size_t(*picked)])
        actions[size_t(*picked)]();
}

void Navigator3D::render_toolbar(Camera& camera, const NavigatorFrame& frame, CanvasUi::Card& card)
{
    const NavLayout&    l     = frame.layout;
    const CanvasUi::Theme& theme = CanvasUi::Context::current()->theme();
    const CanvasUi::Painter paint = card.painter();
    if (l.background)
        paint.divider(l.toolbar.left, l.toolbar.right, l.toolbar.top + CanvasUi::Context::current()->metrics().outline);
    paint.fill_rect(CanvasUi::Rect(l.separator.left, l.separator.top, l.separator.right, l.separator.bottom), theme.border);

    struct Button
    {
        CanvasUi::Icon           icon;
        const char*           menu;
        std::string           tip;
        std::function<void()> action;
    };
    const std::array<Button, NAV_BUTTON_COUNT> buttons = {{
        {CanvasUi::Icon::Home, nullptr, _u8L_CONTEXT("Home view", "Navigator"),
         [&] {
             camera.auto_type(Camera::EType::Perspective);
             animate_to(camera, frame.home_view.value_or(default_view_rotation()));
         }},
        {CanvasUi::Icon::RotateLeft, nullptr, _u8L_CONTEXT("Rotate left by 90°", "Navigator"), [&] { rotate_about_z(camera, .5 * PI); }},
        {CanvasUi::Icon::RotateRight, nullptr, _u8L_CONTEXT("Rotate right by 90°", "Navigator"), [&] { rotate_about_z(camera, -.5 * PI); }},
        {CanvasUi::Icon::Cube, VIEWS_MENU, _u8L_CONTEXT("Views", "Navigator"), {}},
        {CanvasUi::Icon::Fit, nullptr, _u8L_CONTEXT("Fit view", "Navigator"), frame.fit},
        {CanvasUi::Icon::Eye, DISPLAY_MENU, _u8L_CONTEXT("Display", "Navigator"), {}},
    }};

    for (size_t i = 0; i < buttons.size(); ++i) {
        const Button&        b = buttons[i];
        CanvasUi::ButtonOptions options;
        options.size      = CanvasUi::ButtonSize::Icon;
        options.width     = l.buttons[i].width();
        options.icon      = b.icon;
        options.has_popup = b.menu != nullptr;
        options.pressed   = b.menu != nullptr && CanvasUi::popup_open(b.menu);
        options.tooltip   = b.tip;
        CanvasUi::place(CanvasUi::Point(l.buttons[i].left, l.buttons[i].top));
        bool clicked = false;
        {
            const CanvasUi::ScopedId id{int(i)};
            clicked = CanvasUi::button("##button", "", options);
        }
        if (clicked && b.menu)
            CanvasUi::open_popup(b.menu);
        else if (clicked && b.action)
            b.action();
    }

    render_views_menu(camera, frame);
    render_display_menu(frame);
}

void Navigator3D::render(Camera& camera, const NavigatorFrame& frame)
{
    step_animation(camera);

    const NavLayout&     l       = frame.layout;
    const CanvasUi::Pointer pointer = CanvasUi::pointer();
    const Vec2d          mouse(pointer.pos.x, pointer.pos.y);
    const bool           free_of_windows = !pointer.over_panel;

    const CanvasUi::Rect bounds = l.background ? l.card : l.toolbar;
    CanvasUi::CardStyle  style;
    style.padding = 0.f;
    CanvasUi::Card card(m_card_id.c_str(), CanvasUi::Point(bounds.left, bounds.top), CanvasUi::Size(bounds.width(), bounds.height()), style);

    const bool over_card  = card.hovered() && !l.toolbar.contains(CanvasUi::Point(float(mouse.x()), float(mouse.y())));
    const bool mouse_free = l.background ? over_card : free_of_windows;
    handle_cube_input(camera, navigator_projection(l.cube, camera.get_view_rotation().toRotationMatrix()), mouse_free, frame);

    const NavProjection projection = navigator_projection(l.cube, camera.get_view_rotation().toRotationMatrix());
    bool                bed        = false;
    int                 hovered    = -1;
    if (m_press && !m_press->dragging) {
        hovered = m_press->zone;
        bed     = m_press->bed;
    } else if (!m_press && mouse_free && !frame.blocked)
        hovered = hovered_target(projection, mouse, bed);
    draw_cube(projection, frame, hovered, card);
    if (m_press || hovered >= 0 || bed)
        CanvasUi::capture_mouse();
    if (!m_press)
        cube_tooltip(frame, hovered);

    render_toolbar(camera, frame, card);

    // The scene is drawn before the drag turns the camera, so the turn shows in the next frame.
    if (m_turned)
        CanvasUi::Context::current()->request_frame();
    m_turned = false;
}

} // namespace GUI
} // namespace Slic3r
