#pragma once

// Multiplanar declarative cut planes: shared partition logic.
//
// A ModelObject's CutPlaneStack is authored non-destructively; the mesh partition it implies is
// derived here and consumed by BOTH the slicing backend (Print::apply emits one internal
// PrintObject per segment) and the GUI chunk preview (colored per-segment meshes). Keeping both
// consumers on this single module guarantees the preview can never drift from what the slicer
// will actually produce.
//
// Segment identity: sequential-cut ("peel") semantics over the plane arrangement. Planes are
// normalized so the cell containing the lowest ("closest to bed") vertex is on the near side of
// every plane, then ordered so that each occupied arrangement cell merges into the chunk of the
// plane it SITS on (largest cut-face contact). Chunk k is everything on the far side of plane k
// and the near side of every later plane, so N planes yield at most N + 1 segments -- two planes
// crossing inside the object give three sections, not four. Segments print root first, then in
// peel order; each non-root segment builds along its plane's (possibly overridden) normal.

#include "libslic3r.h"
#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "ObjectID.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;
struct CutPlane;

// Why a derivation produced no segments. The slicing backend only cares whether the vector came
// back empty; the GUI chunk preview uses this to say WHY it is showing the plain object instead of
// leaving the user to guess which of six different conditions they hit.
enum class CutSegmentationStatus
{
    Ok,             // segments were derived
    StackInactive,  // the stack is off, or no enabled plane has a usable normal
    NoGeometry,     // the object has no model-part triangles
    TooManyPlanes,  // more than the supported maximum (64) of enabled planes
    NoSplit,        // no enabled plane actually splits the mesh (fewer than two occupied cells)
    NoRootCell,     // no cell is on the near side of every plane -- no bed-resting root
    SegmentTooThin  // some segment would be thinner than min_segment_thickness
};

// Which side of a bounding plane the segment lies on, and therefore what the plane means to
// support generation. A Floor is load-bearing: the segment rests on it, it contributes to
// floor_z() and may be a surface branches root on. A Ceiling only constrains the valid support
// volume -- support must stay below it, but must never stem from it (that would rest support on
// the cut face of a segment printed later). Derived from the arrangement sign vector, not
// re-inferred from plane geometry downstream.
//
// UNLAYERED_MOD -- a Bed clip is the build plate itself (world z >= 0), added only to a non-root
// segment whose material reaches the plate. Such a segment slices along its own tilted build
// normal, so nothing else stops its angled layers at z = 0: they ran on through the plate (and,
// on a sunk import, printed the material under it). Load-bearing like a Floor, but it comes from no
// authored plane (plane_index -1), and the slicer clips against it with a margin of
// h/2 * |cos(tilt)| so no bead dips into the plate (PrintObject::slice_volumes).
enum class CutClipRole { Floor, Ceiling, Bed };

// One half-space bound of a segment, in the object's LOCAL frame:
// a point p belongs to the segment iff  normal.dot(p) - offset <= 0  for every clip.
struct CutSegmentClip
{
    Vec3d  normal { Vec3d::UnitZ() };
    double offset { 0. };
    // Index into ModelObject::cut_planes.planes -- the authored plane this clip came from.
    int         plane_index { -1 };
    // A non-root segment carries exactly one Floor -- its own (base) plane, which it rests on --
    // and a Ceiling for every LATER-peeling plane; planes peeled earlier do not clip it at all
    // (the segment may legitimately contain material on their far side). The root is all Ceilings.
    CutClipRole role        { CutClipRole::Ceiling };

    bool operator==(const CutSegmentClip &rhs) const {
        return normal == rhs.normal && offset == rhs.offset
            && plane_index == rhs.plane_index && role == rhs.role;
    }
    bool operator!=(const CutSegmentClip &rhs) const { return !(*this == rhs); }
};

// One derived segment of a cut-plane partition. Value data, deliberately independent of any
// PrintObject identity so it survives Print::apply diffing and could later be transplanted into a
// multi-frame slicing core.
struct CutSegmentDescriptor
{
    int index      { -1 };   // print order: root == 0, then peel order; deterministic across derivations
    int parent     { -1 };   // parent segment index; -1 for the root
    int base_plane { -1 };   // index into ModelObject::cut_planes.planes; -1 for the root

    // Half-space clips confining this segment (only planes that actually split the mesh contribute).
    std::vector<CutSegmentClip> clips;
    // Object-local build direction (unit). Ignored for the root segment, which always builds +Z.
    Vec3d build_normal { Vec3d::UnitZ() };

    // Exact extent of this segment's materialized clipped piece, projected on the build axis in the
    // instance frame. Drives the slice-frame rebase (the segment slices over its own range instead
    // of the whole object's) and the thin-segment check. Both are 0 for an unpopulated descriptor.
    // For a segment carrying a Bed clip, the extent of the part ABOVE the plate, so its layer
    // schedule starts where printable material starts instead of growing empty leading layers.
    double build_z_min { 0. };
    double build_z_max { 0. };
    // How far up the build axis SUPPORT may have to reach: the highest point, along this
    // segment's build axis, of any cut face the segment sits BELOW (its ceilings). A later
    // segment resting on such a face can overhang this segment's empty space above this segment's
    // own material, and the column carrying it has to be printed in THIS segment's layers -- so
    // the segment slices up to here, with empty object layers, instead of stopping at
    // build_z_max. Never below build_z_max; equal to it when there is no ceiling.
    double support_z_max { 0. };

    // UNLAYERED_MOD -- how far this segment reaches HORIZONTALLY in the instance frame, as the
    // diagonal of its horizontal bounding box. The build-axis extent above is a 1-D projection and
    // says nothing about lateral size, but a virtual-5-axis tilt limit needs exactly that: a tilted
    // layer climbs reach*tan(tilt) from one end to the other, and that climb has to fit under the
    // gantry (virtual_tilt_limit_deg() in PrintConfig.hpp).
    //
    // The diagonal rather than the extent along the tilt azimuth, deliberately. No two points of
    // the segment are further apart than it, so it bounds the run whichever way the tilt points --
    // which means it does not have to be recomputed when the user spins a plane about Z, and the
    // limit does not move under them mid-drag. 0 for an unpopulated descriptor = "reach unknown".
    double build_reach_max { 0. };

    // Perpendicular distance from `base_plane` up to the nearest material of this segment. Zero
    // when the segment rests on its base plane (the usual case: the plane produced a cut face).
    // POSITIVE when the segment FLOATS above it -- reachable when a plane passes through empty
    // space between disconnected shells, so no cut face is generated on either piece. A floating
    // segment's first layer is suspended over the gap and is NOT detected as an overhang by the
    // stock generators (they assume layer 0 rests on the bed), so it must be seeded explicitly.
    double base_plane_distance_below_material { 0. };
    bool   touches_base_plane() const { return base_plane_distance_below_material <= EPSILON; }

    // Part segment: this segment is exactly ONE model part of a multi-part object, the one whose
    // own config carries a build direction (layer_rotation_b/c on the ModelVolume). It has no
    // clips and no parent -- it is the part's whole mesh, built along that direction -- and it is
    // never the root: it rests on nothing the partition knows about, so it gets no brim and no
    // support cascade, like a legacy per-object chunk. Root and plane segments exclude every
    // directed part (see segment_includes_volume). Invalid for plane segments and the root.
    ObjectID part_volume;

    bool is_root() const { return parent < 0 && ! part_volume.valid(); }

    // BELT_MOD -- whether the root builds along world +Z (a flat bed). On a belt it builds along the
    // tilted gantry normal, measured from the instance origin, so its build_z_min is NOT a height
    // above the belt: about half of any object projects negative without being sunk at all.
    // Meaningful for the root only; true by default so a hand-built descriptor keeps the flat-bed rule.
    bool root_builds_up { true };

    // UNLAYERED_MOD -- the value subtracted from the build-axis extent to place this segment's
    // slice-frame origin, i.e. slice_z = build_axis_extent - slice_z_base(). Clamped at 0 for the
    // ROOT segment on a flat bed: it is the only one resting on the build plate and it slices along
    // +Z exactly, so its build_z_min is a real bed height, and rebasing a SUNK root onto its own bottom would
    // lift material from under the plate into positive slice z where it gets sliced and printed
    // (measured 2496 extruding moves down to -1.132 mm on a real project, and six empty leading
    // layers once the bed clip removed them again). Clamping preserves the sinking instead,
    // exactly as an ordinary object does, so the layer generator simply never slices what is
    // under the plate. Non-root segments keep the unconditional rebase: their extent is projected
    // on their own tilted build normal, and the rebase is what stops them growing leading empty
    // layers. So does a belt root (root_builds_up false): clamping there dropped every layer whose
    // gantry-normal projection was negative -- the leading wedge of the part next to the belt, up to
    // the instance origin. Material genuinely under the belt is removed by the belt clip instead
    // (bed_clip in slice_volumes), as for an ordinary belt object.
    //
    // ONE definition on purpose. update_slice_frame() builds the frame from this and
    // SegmentSupportBounds locates the build plate from it; when the clamp lived only in the
    // former, the latter still assumed -build_z_min and put the plate 1.132 mm too high on a sunk
    // root, floating every support branch off the bed.
    double slice_z_base() const { return (is_root() && root_builds_up && build_z_min < 0.) ? 0. : build_z_min; }
    bool is_part() const { return part_volume.valid(); }
    bool has_bed_clip() const {
        for (const CutSegmentClip &clip : clips)
            if (clip.role == CutClipRole::Bed)
                return true;
        return false;
    }

    bool operator==(const CutSegmentDescriptor &rhs) const {
        return index == rhs.index && parent == rhs.parent && base_plane == rhs.base_plane
            && clips == rhs.clips && build_normal == rhs.build_normal
            && build_z_min == rhs.build_z_min && build_z_max == rhs.build_z_max
            && build_reach_max == rhs.build_reach_max
            && support_z_max == rhs.support_z_max
            && base_plane_distance_below_material == rhs.base_plane_distance_below_material
            && part_volume == rhs.part_volume && root_builds_up == rhs.root_builds_up;
    }
    bool operator!=(const CutSegmentDescriptor &rhs) const { return !(*this == rhs); }
};

// Derive the segments implied by mo.cut_planes (enabled planes only). Returns an empty vector when
// the feature is inert: stack inactive, no enabled plane actually splits the mesh (fewer than two
// occupied cells), or the plane count exceeds the supported maximum (64). The first instance's
// rotation defines "down" for root selection; disconnected components within one cell stay one
// segment (same behavior as a destructive cut).
// BELT_MOD -- root_build_normal_world is the direction the ROOT builds along, world frame: +Z on a
// flat bed, the tilted gantry normal on a belt printer. Non-root segments keep their own normals.
std::vector<CutSegmentDescriptor> derive_cut_segments(
    const ModelObject &mo, const Transform3d &instance_transform = Transform3d::Identity(),
    double min_segment_thickness = 0.0,
    CutSegmentationStatus *status = nullptr,
    std::vector<indexed_triangle_set> *out_meshes = nullptr,
    const Vec3d &root_build_normal_world = Vec3d::UnitZ());

// The merged MODEL_PART mesh of `mo` in the object-local frame (each volume under its own matrix).
// The partition is always derived from exactly this mesh; exposed so a caller that already needs it
// (the GUI chunk preview) can cache it instead of keeping a second, drifting copy of the loop.
// With `exclude_directed_parts`, parts carrying their own build direction are left out -- they are
// their own segments and never take part in the plane partition.
indexed_triangle_set merged_model_part_mesh(const ModelObject &mo, bool exclude_directed_parts = false);

// Per-part build directions. A model part whose own config carries layer_rotation_b/c (the
// world-frame machine angles, the same keys an object uses) is sliced as its own segment along
// that direction; the remaining parts stay together in the root (and in the plane partition).
bool model_volume_has_build_direction(const ModelVolume &mv);
bool object_has_directed_parts(const ModelObject &mo);
// Does `mv` contribute material to the PrintObject carrying `segment` (nullptr = not segmented)?
// A part segment owns exactly its part; every other segment owns the undirected parts. Negative
// volumes and modifiers belong to every segment: they only ever subtract from or tag what is there.
bool segment_includes_volume(const CutSegmentDescriptor *segment, const ModelVolume &mv);
// Append one part segment per directed part of `mo` to `segments` (indices continue the sequence,
// so they print after the partition). When there is no partition at all (`segments` empty) and
// `merged` -- the undirected parts -- has geometry, a plain root over it is synthesised first, so an
// object with directed parts but no planes still slices as root + parts. `out_meshes`, when given,
// is extended in step: the merged mesh for a synthesised root, each part's object-local mesh after.
void append_part_segments(const ModelObject &mo, const indexed_triangle_set &merged,
                          const Transform3d &instance_transform,
                          std::vector<CutSegmentDescriptor> &segments,
                          std::vector<indexed_triangle_set> *out_meshes);

// Core derivation over an EXPLICIT plane list, so a caller can derive a partition that is not (yet)
// the one stored on the object -- the chunk preview uses it to include the plane currently under
// the gizmo's grabbers, which is otherwise invisible to the partition until the user commits it.
// `planes` is taken as authored: disabled entries are skipped here exactly as they are for a
// ModelObject. `user_ordered` mirrors CutPlaneStack::user_ordered: false = automatic peel order
// (seat rule), true = the list order alone decides (first plane peels/prints last and claims any
// contested wedge). When `out_meshes` is given it receives one mesh per segment, indexed by
// descriptor index, taken from the SAME materialized pieces the descriptors were derived from --
// so a preview built from them cannot disagree with the descriptors, and nothing is cut twice.
std::vector<CutSegmentDescriptor> derive_cut_segments(
    const indexed_triangle_set &merged, const std::vector<CutPlane> &planes,
    const Transform3d &instance_transform, double min_segment_thickness,
    bool user_ordered = false,
    CutSegmentationStatus *status = nullptr,
    std::vector<indexed_triangle_set> *out_meshes = nullptr,
    const std::string &object_name = std::string(),
    const Vec3d &root_build_normal_world = Vec3d::UnitZ());

// Intersect `src` with the half-plane  ab.x()*x + ab.y()*y <= c  (x/y/c in unscaled mm, `src` and
// `bbox` in scaled coordinates). `bbox` bounds the region of interest; the half-plane is realized
// as a large rectangle safely covering it. A degenerate `ab` (near zero) returns `src` unchanged —
// the caller is expected to handle the pure-horizontal-plane case by keeping or clearing the layer.
ExPolygons clip_expolygons_by_half_plane(ExPolygons &&src, const Vec2d &ab, double c, const BoundingBox &bbox);

// Cut `merged` into one mesh per segment by applying each segment's clips with cut_mesh(). This
// reconstructs the partition from scratch, cutting the whole mesh once per clip and evaluating each
// physical plane twice (once per side, in two different rotated frames), so its cut faces are only
// approximately the ones the derivation produced. Prefer derive_cut_segments()'s `out_meshes`,
// which hands back the pieces the descriptors actually came from; this remains as the standalone
// path used by the tests. Segments that end up empty yield an empty indexed_triangle_set at their
// index (callers should skip them when rendering).
std::vector<indexed_triangle_set> segment_meshes(const indexed_triangle_set &merged, const std::vector<CutSegmentDescriptor> &segs);

} // namespace Slic3r
