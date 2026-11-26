#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "libslic3r/Point.hpp"
#include "PreviewOverlayLayout.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

enum class NavZoneKind : unsigned char { Face, Edge, Corner };

struct NavZone
{
    NavZoneKind        kind{NavZoneKind::Face};
    // Unit normal pointing out of the cube.
    Vec3d              normal{Vec3d::Zero()};
    // Counterclockwise when seen from outside, along -normal.
    std::vector<Vec3d> polygon;
};

inline constexpr size_t NAV_ZONE_COUNT = 26;
// The faces, the first zones of the cube.
enum NavFace : size_t { NavRight, NavLeft, NavRear, NavFront, NavTop, NavBottom, NAV_FACE_COUNT };
inline constexpr double NAV_BED_HALF      = 1.30;
inline constexpr double NAV_BED_TOP       = -1.0;
inline constexpr double NAV_BED_THICKNESS = 0.10;

// The faces first (+X, -X, +Y, -Y, +Z, -Z), then the 12 edges, then the 8 corners.
const std::vector<NavZone>& navigator_cube_zones();

// The top outline of the bed plate at z = NAV_BED_TOP, counterclockwise from +Z, with the front tab
// on the -Y side and rounded corners.
const std::vector<Vec3d>& navigator_bed_outline();

struct NavProjection
{
    // World to view; the view looks along -Z of its own frame.
    Eigen::Matrix3d rotation{Eigen::Matrix3d::Identity()};
    Vec2d           center{Vec2d::Zero()};
    // Pixels per half edge in the plane through the centre of the cube.
    double          scale{1.};
    // How far the eye stands from the centre of the cube, in half edges; 0 projects orthographically.
    double          eye_distance{0.};

    Vec2d  project(const Vec3d& p) const;
    // The direction of the view, from the cube to the eye.
    Vec3d  to_camera() const;
    // The direction from p to the eye, which in perspective differs from point to point.
    Vec3d  toward_camera(const Vec3d& p) const;
};

inline constexpr double NAV_EYE_DISTANCE = 7.;

bool  navigator_zone_visible(const NavZone& zone, const NavProjection& projection);
int   navigator_zone_at(const NavProjection& projection, const Vec2d& point);
bool  navigator_point_in_polygon(const std::vector<Vec2d>& polygon, const Vec2d& point);
float navigator_label_opacity(const Vec3d& normal, const Vec3d& to_camera);
// The bed fades as the camera goes below it, so that the bottom of the cube shows through.
float navigator_bed_opacity(const Vec3d& to_camera);

struct NavView
{
    Vec3d to_camera{Vec3d::UnitZ()};
    Vec3d up{Vec3d::UnitY()};
};

NavView navigator_zone_view(size_t zone);
// The camera's rotation for a view: the camera looks back along to_camera with up at the top.
Eigen::Quaterniond navigator_view_rotation(const NavView& view);

// A zone's name, marked for translation in its context.
struct NavZoneName
{
    int         x, y, z;
    const char* text;
    const char* context;
};
const NavZoneName& navigator_zone_name(size_t zone);

// Over the bed plate as projected, top or bottom side.
bool navigator_bed_at(const NavProjection& projection, const Vec2d& point);

// The cube and the bed, in a slight perspective, fit an area of this aspect, with the cube a little above its center
// to leave room for the bed below.
NavProjection navigator_projection(const CanvasUi::Rect& area, const Eigen::Matrix3d& rotation);

// Whether a turn of the navigator has to stop short of its target: something else turned the camera away
// from the rotation the turn last set, or other canvases drew frames since its last step, as they do while
// its own is hidden. A slow frame of its own canvas does not stop it.
bool navigator_turn_interrupted(const Eigen::Quaterniond& camera, const Eigen::Quaterniond& last_set, int frames_since_last_step);

// A press on the cube or the bed, from the button going down to its release.
struct NavPress
{
    int   zone{-1};
    bool  bed{false};
    Vec2d position{Vec2d::Zero()};
    bool  dragging{false};
};

// A press turns into a drag once the mouse moves farther than threshold from where it went down.
bool                  navigator_drag_starts(const NavPress& press, const Vec2d& mouse, double threshold);
// The zone a release turns the camera to: the zone pressed, wherever the mouse is let go, unless the press was a drag.
std::optional<size_t> navigator_released_zone(const NavPress& press);

// A view rotation as the settings keep it, "w x y z"; text that is not a unit quaternion reads as none.
std::optional<Eigen::Quaterniond> navigator_view_from_string(const std::string& text);
std::string                       navigator_view_to_string(const Eigen::Quaterniond& view);

inline constexpr size_t NAV_BUTTON_COUNT = 6;

// The bottom-left corner of the scene, in canvas pixels. Without a background the toolbar is a card of
// its own with the cube over it; with one, the cube and the toolbar share a card. The section view
// button stands beside them.
struct NavLayout
{
    bool        background{false};
    CanvasUi::Rect card;
    CanvasUi::Rect toolbar;
    CanvasUi::Rect cube;
    CanvasUi::Rect section;
    std::array<CanvasUi::Rect, NAV_BUTTON_COUNT> buttons;
    CanvasUi::Rect separator;

    float top() const;
    float right() const { return section.right; }
};

NavLayout navigator_layout(bool background, float scale, float scene_bottom);

} // namespace GUI
} // namespace Slic3r
