# CAD geometry and selection plugin API

`orca.host.cad` exposes native Design-tab picks and read-only OCCT topology and geometry.
It is compiled with `SLIC3R_CAD`; the experimental CAD preference controls whether Design
exists. Import STEP **in Design** to retain the CAD bodies. A STEP imported directly into
Prepare becomes a slicing mesh and does not acquire CAD selections through this API.

## Selections

| Function | Result |
| --- | --- |
| `selected_shape()` | Primary native `SelectedShape`, or `None` |
| `selected_face()` | Compatible `SelectedFace` snapshot, or `None` |
| `selected_edge()` / `selected_vertex()` | `SelectedShape` when that kind is picked, otherwise `None` |
| `on_selection_changed(callback)` | Subscription to face, edge, vertex, body and deselection events |
| `on_face_selection_changed(callback)` | Compatible subscription to face changes and deselection |

`SelectedShape` has `kind` (`face`, `edge`, `vertex`, `body`), `body_index`, `face_index`,
`edge_index`, `vertex_index`, `edge_indices`, `selection_revision`, `body_to_design` and
`shape`. Unused indices are -1. `edge_indices` describes the native Shift/Ctrl edge set;
`shape` describes its primary edge. A whole-body pick may contain a solid, shell or compound.

`SelectedFace` keeps its original fields: body/face indices, revision, centroid, optional
normal, area, planarity and transform. It adds `shape`, `local_frame` and `boundary_loops`.
The normal is evaluated at the UV-bounds midpoint, not at the area centroid. A centroid can
lie outside a concave face or in a hole; a curved face has no single global plane normal.

Indices are native document indices, not persistent topology IDs. Rebuild, replacement or
viewport destruction clears the selection. Revisions count changes to each API's published
selection, not CAD-document revisions. Subshape-list positions belong to their own snapshot;
they must not be used as native document indices. Sketches, hover and feature-tree highlights
are not converted into picks or used to clear a separately retained native solid selection.

## Read-only OCCT shapes

A `Shape` owns a detached copy of its OCCT topology **and geometry**, without display triangles.
Old snapshots and their descendants remain readable after document edits, deselection, reload
or viewport destruction. They describe the geometry at capture time, not the edited model.

`Shape.type` uses `ShapeType`: `Compound`, `CompSolid`, `Solid`, `Shell`, `Face`, `Wire`, `Edge`,
`Vertex`. `orientation` is `forward`, `reversed`, `internal` or `external`.

- `children()` returns direct oriented occurrences, preserving repeated seam edges.
- `subshapes(ShapeType.Edge)` (or any other type) enumerates unique matching topology. It
  includes the shape itself when its type matches. It is not a boundary traversal order.
- Wires, shells, solids and compounds can be reached by traversal of a selected body.
  This API does not introduce new viewport picking modes for those containers.
- Native mouse picking still requires geometry represented in the Design viewport. Standalone
  wire/point-only STEP imports do not gain a new rendering or picking implementation here.

These are read-only wrappers, not the complete OCCT Python SDK. They neither construct new
model geometry nor expose mutable kernel handles. Native OCCT failures become `GeometryError`;
wrong shape types, invalid sample counts and non-finite/out-of-range parameters raise `ValueError`.

## Faces and surfaces

On a face, `surface_type` identifies planes, cylinders, cones, spheres, tori, Bezier/B-spline,
revolution, extrusion, offset and other surfaces. These classifications do not restrict picking.
`area`, `centroid`, `uv_bounds` (`u_min, u_max, v_min, v_max`) and `periodic` (`u, v`) are readable
properties. UV parameters are native to that surface; they are not universally millimetres.

`evaluate_surface(u, v)` evaluates the underlying surface within the UV bounds and returns:

- `position`, `du`, `dv`: point and raw parameter derivatives;
- `frame`: optional right-handed frame with `origin`, `x_axis`, `y_axis`, `normal`. X follows
  increasing U; the normal accounts for face orientation, and Y is normal × X;
- `inside_trim`: whether OCCT classifies the UV point inside or on the face boundary.

UV bounds are a bounding rectangle, not the trimmed face. Check `inside_trim` to exclude holes
and trimmed-away regions. `local_frame` is the frame at the UV-bounds midpoint; its origin can
be in a hole. On a curved face it is a **local tangent frame**, not a coordinate system that
flattens the entire surface. Undefined derivatives/poles can give `frame=None`; kernel evaluation
failures raise `GeometryError`. Unbounded faces have no midpoint frame.

`boundary_loops()` returns `BoundaryLoop` objects with `is_outer`, `wire`, ordered `edges`, and
`is_complete`. Each `BoundaryEdge` has an oriented `shape`, `is_seam` and `is_degenerate`.
Inner loops delimit holes. Seams may appear twice with different traversal orientations;
degenerate edges remain present even though they have no regular 3D curve. For defective wires,
OCCT may not order every edge: `is_complete=False` exposes this instead of claiming a complete
loop. `wire.children()` still gives the direct topology. Do not generate a closed toolpath from
an incomplete traversal.

## Edges and vertices

Edges expose `curve_type` (line, circle, ellipse, hyperbola, parabola, Bezier/B-spline, offset,
other or degenerate), `is_degenerate`, and the natural increasing `parameter_range`.
`evaluate_curve(t)` returns `position`, raw increasing-parameter `derivative`, and optional
orientation-aware unit `tangent`. `sample_curve(count=33)` returns uniformly spaced **parameter**
samples in edge-traversal order, including endpoints. It does not promise equal arc length or
a chord-error tolerance. Count must be 2–100000. Degenerate/unbounded curves cannot be sampled
this way; a degenerate edge's vertices can be inspected by topology traversal.

Vertices expose `vertex_point`. Native vertex picking is resolved back to a real OCCT vertex,
including the body's display transform, rather than manufacturing a vertex from a screen hit.

## Coordinates and threading

Points, geometry and frames use OCCT shape coordinates, including OCCT locations, in mm;
areas use mm². `body_to_design` is the row-major 4×4 affine matrix that additionally applies
pending Design body moves. Apply it to homogeneous points; normals require the inverse transpose
of its linear part. Commit to Plate and later arrangement require a separate association with
Prepare model volumes and instances. This API does not provide G-code placement coordinates.

Retain a subscription handle while needed; `close()` is idempotent and `is_active()` reports
its state. There is no initial event: subscribe, then read the current selection. Callbacks
receive their event's snapshot; the current cache may already contain a newer selection.
They run later on the UI thread, under the GIL and the owning capability's audit context.
Keep handlers short. Exceptions are logged and isolated from other subscribers.

Script workers can read the snapshot cache without a GUI round trip. Geometry queries on a
snapshot and its descendants are serialized by a shared lock; they access the detached copy,
never the live document. Capturing/copying a large whole body can cost time on the UI thread.
Subscriptions are disconnected before unload and again during capability/module teardown,
covering reload, shutdown and failed loads. Selection caches are constructed before the plugin
manager so they remain alive when retained, unloaded plugin entries are destroyed at process exit.
Closing/dropping a handle suppresses queued calls;
an invocation already started may finish. As with any callback, avoid ownership cycles or
explicitly close a handle stored by an object whose bound method it retains.

## Example

```python
cad = orca.host.cad

def picked(selection):
    if selection is None:
        return
    shape = selection.shape
    if shape.type == cad.ShapeType.Face:
        print(shape.surface_type, shape.uv_bounds, shape.local_frame)
        for loop in shape.boundary_loops():
            if not loop.is_complete:
                raise ValueError("Incomplete boundary")
            for edge in loop.edges:
                if not edge.is_degenerate:
                    print(edge.shape.curve_type, edge.shape.sample_curve(33))
    elif shape.type == cad.ShapeType.Vertex:
        print(shape.vertex_point)

subscription = cad.on_selection_changed(picked)
picked(cad.selected_shape())
# subscription.close() when finished
```

## Verification

Install `sandboxes/orca_cad_face_selection_plugin_any.py`. Enable CAD, restart, and import STEP
in Design. Run the inspector capability, then test:

1. Pick planar, cylindrical and freeform faces; inspect surface type, UV bounds, frames and loops.
   Pick a face with a hole and check one outer loop plus its inner loop(s).
2. Pick an edge, vertex and whole body. Check type-specific data, and that `selected_face()`
   becomes `None`. Shift/Ctrl edge picks should update the edge set and primary edge.
3. Pick empty space, rebuild, undo, replace/open a project or start a new design. The previous
   native selection must clear; retained geometry snapshots must remain usable.
4. Move a body, re-pick it, and check its separate `body_to_design` transform.
5. Stop/restart the inspector and reload/unload the plugin with events pending. No old handler
   should run after teardown or touch a destroyed window.

Automated tests exercise topology, holes, orientation, seams, poles, freeform evaluation,
source-copy isolation, concurrent reads, a STEP round trip, Python bindings and subscription
lifecycle. Desktop picking and cross-platform application builds require separate verification.
