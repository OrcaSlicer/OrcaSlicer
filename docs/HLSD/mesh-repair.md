# Mesh repair — High Level Design

## Purpose and scope

Mesh repair turns a broken triangle mesh into closed, outward oriented shells that bound a solid, so that the
mesh slices predictably and the other mesh tools (booleans, cut, painting) get valid input. It runs on demand:
the *Fix Model* command of the context menu of an object or part and of the warning icon in the object list, *Fix
All* and *Fix All Plates* in the context menu of a plate, the prompt the Cut tool shows when a cut leaves open edges,
the *Repair and import* choice of texture import, and the `--repair` option of the command line. It is built on CGAL
and is the only repair the application has, on every platform.

`repair_mesh()` in `src/libslic3r/MeshRepair.cpp` does the work on one `TriangleMesh`, and `ModelVolume::repair()`
applies it to a part: it replaces the mesh only when the repair changed it, and remaps or drops the painting of a
replaced mesh. The GUI side, `fix_model_with_cgal_gui()` in `src/slic3r/Utils/FixModelByCgal.cpp`, runs it on a
worker thread for each volume of an object, or for one volume, behind a progress dialog. `repair_mesh()` reports its
progress between stages through a callback, which cancels the repair by throwing.

## What the repaired mesh is

The repair keeps what the slicer would print from the input. The slicer fills a layer where the winding number of
the input surface is not zero (`MeshSlicingParams::SlicingMode::Regular` uses Clipper's nonzero fill rule), so:

- Shells crossing each other print as their union. The repair keeps them as separate shells, as the parts of an
  object are kept, and makes each of them an outward oriented solid.
- A shell inside a solid that is oriented like it adds nothing, and the repair removes it.
- A shell inside a solid that is oriented inward is a cavity, and stays one.
- A whole mesh turned inside out prints as a solid, and the repair turns it outward.

On top of that, it closes holes and cracks, gives inconsistently oriented triangles the orientation of their
neighbours, drops degenerate, duplicate and stray flat geometry, and removes small self-intersections inside a shell,
such as slivers and folds. Where none of this applies, it reports the mesh as unmodified and the caller keeps the
original mesh, its painting and its object id.

A volume is repaired in place and is never split into several parts: splitting by connectivity would turn a cavity
into a separate solid part and fill it. Each solid of the result is a shell of its own, so *Split to parts* still
separates them. Open pieces of the input whose borders meet exactly, which are pieces of one surface rather than
solids, come out as the one shell they form.

## Pipeline

The input is treated as a triangle soup. Every stage after the first only runs on what the previous ones could
not settle, so a valid mesh passes through quickly.

### Valid parts first

Parts of the input that are connected by vertex indices and are already closed, consistently oriented 2-manifolds
skip the soup stages and go straight to the shell stages. Merging their points with coincident points of other
parts would only make non-manifold junctions out of solids that touch, for example parts of an assembly exported
together or voxels written as separate cubes.

### Soup clean-up

The rest of the soup has its coincident points merged. Triangles with repeated corners are dropped. Coincident
triangles of opposite orientation are a wall between two solids touching face to face, so they cancel in pairs;
of the remaining copies of a triangle one is kept. A connected region that would vanish entirely through
cancelling is a two-sided export instead (every face written in both orientations), and keeps one copy of each
triangle.

Open edges then get their vertices welded within a tolerance of 1e-4 of the bounding box diagonal, which closes the
cracks some exporters leave between triangles that should share corners. A vertex is never welded further than a
quarter of its shortest edge, so welding cannot collapse fine detail. A soup that stays mostly open after welding has
cracks wider than that tolerance, and is welded again with ten and then a hundred times the tolerance.

### Work units

The soup is cut into units repaired independently and in parallel: its connected parts, except that open parts whose
bounding boxes touch share a unit, so that cracks and T-junctions between separately exported patches still close.

### Building the mesh

A unit whose edges are each shared by at most two triangles, running in opposite directions, and whose vertices each
have a single fan of triangles, becomes a `Surface_mesh` directly. Any other unit is oriented first. Triangles are
flipped so that neighbours across an edge shared by two of them agree, each part keeping the orientation the
majority of its area had, which keeps inward oriented cavities inward. Edges where that is impossible or that more
than two triangles share are cut, and each fan of triangles around a vertex gets its own copy of the vertex. If that
still leaves an invalid mesh, CGAL's `orient_polygon_soup()` orients the unit instead, and the parts it flipped
against their majority are reversed back. The linear pass exists because `orient_polygon_soup()` is several times
slower on large soups that need only a few fixes.

### Closing

Compatible borders are stitched, parts that can be stitched once reversed are merged, and borders still open are
snapped onto each other within the weld tolerance, which closes T-junctions. Exactly degenerate faces are removed.

Holes are filled last. Planar border loops that face each other in one plane, one inside the other, bound a single
region, such as a missing annulus at the end of a tube or a cap with features standing on it. Such a group is
triangulated at once with a 2D constrained Delaunay triangulation that fills only the faces at odd nesting depth,
so the openings inside the region stay open. The patch is used only if it closes every loop with the orientation
of the triangles around it. Every other loop is filled on its own with `triangulate_hole()`, which uses a 2D
constrained Delaunay triangulation when the loop is planar. Otherwise it searches the best triangulation among all
triangles over the loop for loops of up to 100 edges, and among the triangles of a 3D Delaunay triangulation for
longer ones, as the full search is cubic in the length of the loop. Beyond 200 edges the full search is not even
used as a fallback, and a fan around the loop's centroid closes a loop nothing else could.

The closed unit is split into its shells. The largest stays in place and the others are copied out, as converting
the whole mesh again costs seconds on large models.

### Shells

The shells of all units, together with the valid parts set aside at the start, are handled independently and in
parallel. Shells enclosing no volume, which hole filling makes out of stray flat patches, are dropped.

Self-intersections inside a shell are removed locally with `experimental::remove_self_intersections()`, which
remeshes the region around them. It is used only for local defects: a shell whose intersections involve more than
5 % of its faces is left as it is, and a result that changes the enclosed volume by more than 0.1 % is undone. On
heavily overlapping ornamental models the remeshing otherwise cuts parts down to slivers, while on genuine defects
it changes the volume by thousandths of a percent. Overlaps left in place slice correctly. Remeshing can cut
small pieces off a shell, so a shell it changed is split again and its pieces enclosing no volume are dropped.

Shells crossing others are found through their bounding boxes and an AABB tree per shell, and are made outward
oriented solids. For every other shell the winding number just outside it is summed from the orientation of the
shells containing it, tested from a point on its largest face. The shell bounds the solid only if the winding
number is zero on exactly one of its sides; it is then oriented so that the solid is inside, or dropped otherwise.

Crossing shells are not united. Slicing unites them anyway, uniting them exactly costs tens of seconds on models
made of many overlapping pieces, and it would fuse parts that *Split to parts* should keep apart.

## Robustness

The repair avoids `experimental::autorefine_and_remove_self_intersections()` and corefinement, which can crash on
messy input, and uses only steps whose failures it can detect. A step that fails keeps its input. CGAL precondition
failures surface as `repair_mesh()` returning false with the message as the error. The output is written in float,
as every mesh in the application is.

The result does not depend on thread scheduling. `libslic3r_cgal` is built with `CGAL_LINKED_WITH_TBB`. Units and
shells are processed in parallel, but each on its own data, and their results are gathered in a fixed order;
sorts compare complete keys, and the detected self-intersections are put in order before they are used. Snapping
stays sequential, as its parallel version gives different results from run to run. Holes are filled with the full
search wherever it is affordable, because the search among Delaunay triangles can choose between equally good fills
differently from run to run, so only non-planar holes of more than 100 edges may still vary. On some inputs
`experimental::remove_self_intersections()` itself places a few vertices differently from run to run.

## Verification

`tests/libslic3r/test_mesh_repair.cpp` covers each kind of defect above on small meshes with known volumes.
