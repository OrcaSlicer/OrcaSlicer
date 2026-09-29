#pragma once

#include "Point.hpp"

#include <vector>

namespace Slic3r {

class ModelObject;

// A face of an object's convex hull that the object can rest on. These are the faces the
// "Lay on Face" gizmo offers and the ones the CLI --ground-* options choose from.
//
// Frames: "object" coordinates have the volume transformations applied but not the instance
// transformation. "Instance" coordinates additionally have the instance rotation, scale and
// mirror applied, but not its offset.
struct LayOnFacePlane
{
    Vec3d       normal;         // outward unit normal, object coordinates
    Vec3d       center;         // centroid of the outline, object coordinates; on the face's mean plane
    float       area;           // mm², instance coordinates
    Pointf3s    outline;        // convex outline in the plane frame, where the face is horizontal
    Transform3d to_plane_frame; // rotation from instance coordinates to the plane frame
};

// Candidate faces of the object's model parts, largest first. The instance transformation
// (without offset) is applied before measuring, so faces too small to rest on are dropped
// by their printed size: under 5 mm², a side under 1 mm, or an inner angle under 1°.
std::vector<LayOnFacePlane> lay_on_face_planes(const ModelObject &object, const Transform3d &instance_matrix_no_offset);

// Index of the largest plane, or -1 if `planes` is empty. Of planes with the same area, such as
// the top and bottom of a box, the one already facing down the most wins, so flat parts stay put.
int find_largest_plane(const std::vector<LayOnFacePlane> &planes);

// Index of the plane whose normal is closest to `direction` (object coordinates),
// or -1 if `planes` is empty.
int find_plane_by_normal(const std::vector<LayOnFacePlane> &planes, const Vec3d &direction);

// Index of the plane whose face contains `point` (object coordinates) within `tolerance` mm, or -1
// if there is none. `instance_matrix_no_offset` is the one the planes were computed with.
int find_plane_at_point(const std::vector<LayOnFacePlane> &planes, const Transform3d &instance_matrix_no_offset,
                        const Vec3d &point, double tolerance);

// Rotates the instance so that `normal` (object coordinates) points down, the same rotation as
// the gizmo applies, then drops the instance so its lowest point is at z = 0.
void lay_on_face(ModelObject &object, size_t instance_idx, const Vec3d &normal);

} // namespace Slic3r
