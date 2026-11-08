#ifndef slic3r_Fill_InfillMeshTiling_hpp_
#define slic3r_Fill_InfillMeshTiling_hpp_

#include "../BoundingBox.hpp"
#include "../Point.hpp"
#include "../TriangleMesh.hpp"

#include "../Polyline.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

class ModelObject;

// Tiling of a user-supplied infill cell. The cell is a triangle mesh in its OWN frame -- the
// frame of the Infill Mesh volume -- and is repeated on the pitch of its own axis-aligned
// bounding box, so a rectangular-prism cell tiles as a rectangular lattice and a rotated volume
// rotates the whole lattice (the cell's axes are assumed to be its tiling axes).

struct InfillCellTiling
{
    Vec3i32 lo { 0, 0, 0 };
    Vec3i32 hi { -1, -1, -1 };
    bool    truncated { false }; // the tile budget was exceeded and only the cell itself was placed

    size_t count() const {
        return (hi.x() < lo.x() || hi.y() < lo.y() || hi.z() < lo.z()) ? 0
            : size_t(hi.x() - lo.x() + 1) * size_t(hi.y() - lo.y() + 1) * size_t(hi.z() - lo.z() + 1);
    }
};

// Tile indices whose copies of `cell_bbox` touch `target` (both in the cell frame): tile (i,j,k)
// is the cell translated by (i,j,k) * pitch with pitch = cell_bbox.size(). An axis with zero
// pitch (a flat cell) is not tiled. `max_tiles` is a runaway guard for tiny cells in huge parts.
InfillCellTiling infill_cell_tiling(const BoundingBoxf3 &cell_bbox, const BoundingBoxf3 &target, size_t max_tiles);

// The lattice: every tile of `cell` covering `target_in_cell_frame`, merged, still in the cell
// frame. Empty for an empty cell. When the tile budget is exceeded the cell is returned as is
// (one tile) and `tiling->truncated` is set.
indexed_triangle_set tile_infill_cell(const indexed_triangle_set &cell, const BoundingBoxf3 &target_in_cell_frame,
                                      size_t max_tiles, InfillCellTiling *tiling = nullptr);

// Convenience over the two above: the lattice covering `target` (an AABB in the target frame),
// returned IN the target frame. `cell_to_target` maps cell coordinates to target coordinates.
indexed_triangle_set build_infill_lattice(const indexed_triangle_set &cell, const Transform3d &cell_to_target,
                                          const BoundingBoxf3 &target, size_t max_tiles, InfillCellTiling *tiling = nullptr);

// The merged Infill Mesh volumes of `mo` as one cell. With several volumes the first one's frame
// is the cell frame and the others are merged into it, so the returned `cell_to_object` is exactly
// the first volume's matrix. Without an infill-mesh volume, `fallback_path` (the infill_mesh_path
// setting) is loaded instead, in the file's own frame at the object's origin (identity). Empty when
// neither exists.
indexed_triangle_set infill_mesh_cell(const ModelObject &mo, const std::string &fallback_path, Transform3d &cell_to_object);
// The mesh file's geometry as one cell (all parts merged under their own placement). Cached per
// path and modification time; empty (with a logged warning) when the file cannot be read.
indexed_triangle_set infill_mesh_cell_from_file(const std::string &path);

// The default runaway guard.
constexpr size_t kMaxInfillTiles = 4096;

// True when the mesh has boundary edges (an open surface: sheets, ribbons) rather than being a
// closed solid. Such a cell prints as single lines along the sheets' cross-sections.
bool its_is_open_surface(const indexed_triangle_set &its);

// Slice an open-surface mesh: at every z the intersection of each facet with the plane is a
// segment, and segments meeting at their ends (within `join_tolerance`, scaled) are chained
// into polylines; a sheet that closes on itself yields a closed polyline. Vertices exactly on a
// plane count as above it, so no z has to be nudged. Output is aligned 1:1 with `zs`.
std::vector<Polylines> slice_sheet_mesh(const indexed_triangle_set &its, const std::vector<float> &zs, coord_t join_tolerance);

// Merge polylines whose ends meet within `tolerance` (scaled) into continuous polylines; the
// coincident joint is kept once. Linear-time endpoint hashing; the order of the result is not
// meaningful.
void join_polylines_at_endpoints(Polylines &polylines, coord_t tolerance);

} // namespace Slic3r

#endif // slic3r_Fill_InfillMeshTiling_hpp_
