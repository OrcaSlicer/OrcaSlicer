# Cut along path — High Level Design

## Purpose and scope

The "Cut along path" gizmo splits an object along a line the user draws on it, where the planar Cut
tool can only split along a plane. It is meant for figures and organic models (sculpted, scanned or
AI-generated meshes, often with millions of triangles) that have to be printed in pieces: a head
along the hair line, an arm at the shoulder, a tail where it joins the body. Like the planar cut,
it can add connectors so the printed pieces register when glued.

The tool has two modes:

- **View path**: points picked from one view direction. The smoothed path is swept along the view
  direction into a thin wall that splits the whole object. Optional limits (start, end, depth) stop
  the wall inside the object; then it only cuts where it reaches the outside surface.
- **Loop**: points picked on the surface around the region to detach. A curved cap spanning the
  loop detaches only that region, even when the loop is not planar. With "Follow the model's edges"
  the loop snaps to the creases of the mesh and the cut runs exactly along mesh edges.

Several loops can be cut one after the other without leaving the tool; the faces already cut are
shown in orange.

## Geometry

The geometry lives in two header-only files under `src/libslic3r/`, which only depend on Eigen,
admesh, earcut and TBB, so `tests/libslic3r/test_path_cut.cpp` tests them without the rest of
libslic3r.

A general mesh boolean is far too slow on the meshes this tool is for, so neither mode uses one.
`PathCutGeometry.hpp` describes the cut as a signed field `f` over the mesh — the distance across
the wall (`ViewCutter`) or the height above the cap (`LoopCutter`) — and the slab `|f| < kerf / 2`
is the clearance left between the pieces. Triangles near the slab are refined where the field bends,
clipped against the two level sets `f = ±kerf / 2`, with each edge crossing computed once so the
result stays watertight, and the openings are closed with caps triangulated in a 2D chart of each
level set (`detail::close_cut_faces`, shared by all cutters). Cost is linear in the triangle count
and only the triangles near the slab are touched.

Rules that shape the result:

- A wall that ends inside the object does not cut there (`foreign_bands`): only the parts of the
  slab that reach the outside surface split the object, so a limited path cuts what it crosses and
  leaves the rest whole.
- A loop also cuts the walls of holes it spans, so a hole through the region comes out open.
- `cut_along_edge_loop` cuts exactly along a chain of mesh edges found by `CreaseGraph` (a
  dihedral-weighted shortest path in a corridor around the picked loop). When it fails, the gizmo
  falls back to `cut_along_loop`.

`CutResult` reports what is still connected after the cut, so the gizmo can show where the cut
does not separate the object instead of producing one piece.

## Connectors

`PathCutConnectors.hpp` places plugs or dowels (cylinder or square) on the curved cut face. All
connectors share one axis, the direction the cut face comes apart in (`pull_direction`), so they
slide out of their holes together even across a U-shaped face. A spot fits only where both pieces
have material all around it, deep enough, with a wall left, and where the face under it is flat
enough; the check casts lines parallel to the axis through a grid of the projected triangles.
Connectors are spread out automatically or placed by clicking; the user can tilt the axis, and can
force a connector where the check rejects it.

## GUI

The tool is the Curve mode of the Cut tool. It has no toolbar icon of its own: the Cut tool shows its
modes (Planar, Dovetail, Curve) as a row of icon buttons, and Curve opens `GLGizmoPathCut`, which
shows the same row to go back to the other two. It stays a separate gizmo because it works on points
picked on the mesh rather than on the cut plane the Cut tool is built around.

`GLGizmoPathCut` collects the points, runs the cut as a background job on a copy of the mesh, shows
a preview with the pieces moved apart, and replaces the object with the pieces only when the user
applies the cut, as one undo step. The job is dropped when the tool is closed or the object changes
meanwhile. The tool keeps no state in the project file.
