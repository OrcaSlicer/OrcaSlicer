// MeshOrient.hpp — CLI orientation primitives.
//
// Implements "ground a face to the bed" operations that the GUI exposes via
// the lay-flat / face-pick gizmos but the CLI was missing. Slicer-chat
// 2026-06-22: auto-orient picks bad orientations for parts with one obvious
// flat face; without these primitives, the operator has to break the
// pipeline and round-trip through the GUI just to set orientation.
//
// All functions operate on Model in-place — they rotate the ModelInstance
// transform of every instance of every object so that the chosen face lands
// on Z=0 (normal pointing -Z). They don't translate; ensure_on_bed in the
// existing CLI pipeline handles the Z-lift afterwards.
//
// Mesh-local normals: the operator's --ground-face-normal vector is
// interpreted in the mesh's *own* coordinate system, NOT in world coords.
// This is robust against prior --rotate-* CLI flags being applied first.
#ifndef slic3r_MeshOrient_hpp_
#define slic3r_MeshOrient_hpp_

#include "libslic3r/Point.hpp"
#include <string>

namespace Slic3r {
class Model;

namespace MeshOrient {

// Find the largest planar face (cluster of coplanar triangles by mesh-local
// normal) across all volumes of all objects in `model`, then rotate every
// instance so that face points to -Z. Returns true on success; false +
// emits a BOOST_LOG error if the mesh is empty.
bool ground_largest_face(Model &model, std::string *out_chosen_normal = nullptr);

// Rotate every instance so the face whose mesh-local normal best matches
// `target_normal` (highest dot product) points to -Z. The target is
// normalized internally; (0,0,0) is rejected.
bool ground_face_normal(Model &model, const Vec3d &target_normal);

// Find the triangle that contains `point_mesh` (in mesh-local coords) and
// ground its face. If multiple triangles contain the point (edge/vertex),
// the triangle with the largest area wins.
bool ground_face_point(Model &model, const Vec3d &point_mesh);

// Translate every instance so the combined model bounding-box centroid sits
// at `bed_center` (XY only; Z is left to ensure_on_bed). Called after
// orientation when --center-on-bed is passed.
bool center_on_bed(Model &model, const Vec2d &bed_center);

} // namespace MeshOrient
} // namespace Slic3r

#endif
