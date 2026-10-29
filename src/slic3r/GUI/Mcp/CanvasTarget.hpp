#pragma once

#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class AABBMesh;
namespace GUI {
class ClippingPlane;
namespace Mcp {

// ui_mouse aims at points of the model instead of pixels guessed from a screenshot: a point is
// projected through the camera the canvas was last drawn with, and the ray a click there casts is
// checked to land on that point's mesh. Pure functions of the camera and the meshes, so they are
// tested without the app.

// The camera a canvas was last drawn with.
struct CanvasView
{
    Transform3d        view       = Transform3d::Identity();
    Transform3d        projection = Transform3d::Identity();
    std::array<int, 4> viewport{0, 0, 0, 0}; // physical pixels
    double             scale = 1.;           // physical pixels per logical pixel
};

// The logical canvas pixel that shows `point` (scene coordinates), or nullopt when the point is
// outside what the camera shows: beside the canvas, behind the camera, or before its near or beyond
// its far plane.
std::optional<Vec2d> project_point(const CanvasView& view, const Vec3d& point);

// The ray a click at logical pixel `pixel` casts: its origin on the near plane and unit direction.
std::pair<Vec3d, Vec3d> pixel_ray(const CanvasView& view, const Vec2d& pixel);

// Where a ray from outside the mesh's displayed bounding box, along the axis `spot` names and
// through the box's centre, first meets the mesh: "top" looks down, "bottom" up, "front" from -Y,
// "back" from +Y, "left" from -X, "right" from +X. Nullopt when the ray misses the mesh or `spot`
// names no side.
std::optional<Vec3d> spot_point(const AABBMesh& mesh, const Transform3d& world, const std::string& spot);

// A mesh as the scene shows it, for check_surface().
struct SurfaceMesh
{
    const AABBMesh* mesh    = nullptr;
    Transform3d     world   = Transform3d::Identity();
    bool            target  = false;
    bool            clipped = false; // the clipping plane cuts it (the open gizmo's object)
    std::string     name;            // what a reason calls it, "object 1 part 0"
};

// Why a click along `ray`, the ray through the pixel the click goes to, would not land on the target
// mesh at `point`, or empty when it would. `point` must lie on a target mesh, above the bed (the plane
// z = 0 inside one of the `beds`, each plate's outline, scaled) and on the side of `clip` it keeps
// when that mesh is clipped. The first thing the ray meets among `meshes` (clipped ones only where
// `clip` keeps them) and on the beds (only looking down: from below the GUI draws and picks through
// them) must be a target mesh near `point`: within `tolerance` plus twice the ray's distance from
// `point`, which a whole pixel adds, on a triangle whose plane holds `point` within `tolerance`. So on
// a slope the click may meet the point's surface a little nearer to the camera or beyond, but not
// another surface.
std::string check_surface(const Vec3d&                     point,
                          const std::pair<Vec3d, Vec3d>&   ray,
                          const std::vector<SurfaceMesh>&  meshes,
                          const ClippingPlane*             clip,
                          const std::vector<Polygon>&      beds,
                          double                           tolerance);

}}} // namespace Slic3r::GUI::Mcp
