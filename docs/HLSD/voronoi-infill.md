# Three-dimensional Voronoi sparse infill

The `voronoi` pattern prints layer sections of faces between nearest 3D control
points. Point generation, cell geometry, wall routing and extrusion are separate
stages, so other point distributions can reuse the same infill implementation.

## Point cloud and geometry

`PointCloudProvider` supplies reproducible XYZ points through half-open box
queries and defines the density-to-spacing mapping. The default provider uses
spatially seeded Poisson sampling. Other providers may be structured, spatially
varying or finite; queries must reproduce the same points regardless of order.
An unbounded provider must eventually supply points.

Sparse infill direction rotates the random cloud about Z through the object's
bounding-box centre; model alignment adds the object's XY rotation. The
angle stays fixed across layers; per-layer rotation templates do not rotate the
3D constellation.

`ActivePointCloud` expands its known region until nearest-site covering bounds
prove that unknown points cannot affect the working slab. Points outside the
model participate; resulting walls and floors are clipped to the fill surface.
Points sufficiently far below the slab are retired. Each infill engine owns its
active constellation and a persistent CGAL 3D Delaunay triangulation. Point-set
changes insert new vertices and batch-remove retired clusters, retaining unaffected
connectivity. Vertex handles are indexed in a sorted vector; sampled XYZ values
remain in a contiguous vector. CGAL stores vertices and tetrahedra in chunked
containers with reusable slots. Copies of a filler own independent networks.
During normal slicing, each worker processes an ascending batch of layers and
caches its engines by point provider; temporary surface fillers reuse those
engines. The cache is released at the end of the batch and is never shared between workers.

CGAL's 2D power diagram, weighted by vertical distance, supplies wall sections
and covering bounds. Its weights change with Z, so this planar diagram is rebuilt
for each section. Points are retired before updating the persistent 3D network,
which supplies shallow-face floor polygons. Geometry clipping removes overlapping
floors and wall sections within floors while retaining their supporting boundaries.

## Optional point relaxation

Relaxation iterations defaults to zero, leaving the original provider unchanged.
A nonzero count prepares a saved object-wide constellation, including exterior
neighbours, after point generation. Each iteration rebuilds its 3D Delaunay
mesh and balances edge lengths against the density-dependent preferred spacing.
Damped simultaneous steps are limited to 10% of nominal site spacing per pass;
convex-hull sites remain fixed to prevent the field shrinking at its boundary.
Three to five iterations are a useful starting range.

Relaxed points are cached and queried consistently across layers. The density
field remains unchanged; cell boundaries follow the relaxed points. Preparation checks cancellation
and reports progress. Relaxation costs additional memory and computation and
cannot eliminate every acute section of a 3D cell.

## Extrusion and routing

At one bead, faces become single wall lines when successive layers have at least half-bead
overlap. Shallower faces become floor bands, rounded outward to Orca's flow row
spacing while keeping the supported boundary fixed. At constant layer height,
continuing bands have at least 50% overlap before clipping; face ends, holes and
changing layer heights limit that guarantee.

Native Rectilinear fill prints floor rows along the face section in alternating
zigzags, advancing from the supported side. Orca's bead width, flow and spacing
are retained. Rows ending within one bead width of a previous-layer perimeter
extend into its actual extrusion footprint. Extensions remain within the fill
surface or existing perimeter material. This preserves the sweep order but does
not guarantee support at internal face ends or where the perimeter is out of reach. Boundary anchoring alone cannot
provide missing support, and floor rows retain ordinary infill flow.

Wall graph vertices snap to fixed representatives. Branch pairing favours
straight crossings; three-way junctions cycle turns across layers using their
generating points. Trails traverse each wall edge once. Wall trails and complete
floor groups are scheduled as freely reversible endpoint chords by Orca's
shared path-ordering helper. Walls follow the selected direction; floors expand
in their original supported-side-first sweep. Their actual entry and exit may
therefore differ from the scheduled chord.
Ordering is local to a fill surface, and disconnected paths still require travel.

Density is statistical: spacing calibration accounts for floor material, but
clipping and row rounding affect the result. Nominal extrusion dimensions keep
first-layer widths and adaptive layer heights from changing the point field.
Infill reports completed layers with throttled UI updates and cancellation.

## Wall proximity and smoothing

Wall-proximity decay defaults to 6 mm; zero gives a uniform base. Gaussian sigma
defaults to 8 mm; zero disables smoothing. With both zero and the Random method,
the original Poisson provider is used without a distance table.

Along flat walls, local density stays at the selected setting within 2 mm,
then decays exponentially to a 1% tail after five decay lengths. The magnitude
of the difference between original and smoothed signed distances reduces the
effective depth near both convex and concave corners. This can boost density to
approximately 1.39 times the setting. Zero decay retains the corner boost with
a 6 mm response length. A reproducibly thinned Poisson candidate field follows
this density; thinning does not constrain face slopes or ensure vaulted roofs.

The density field is prepared on the [sliced distance grid](sliced-distance-field.md).
Material density uses a logarithmic byte: zero is empty, codes 1–253 have sixteen
steps per doubling up to 100%, and 254–255 are reserved. Queries interpolate
decoded densities; raw signed distance remains available independently.

Gaussian smoothing uses separable CPU OpenCV filters with fixed-point working
values. Temporary filter borders extrapolate outward slopes; zero sigma reuses
raw distances. The fixed-point smoothing result is converted directly into
material density using wall decay and corner adjustment, without retaining a
smoothed-distance table. Fields are shared by matching material-density settings. Reslicing clears the per-object cache.
Construction and smoothing report phase progress and check cancellation.

## Structured points and boundary snapping

FCC and Cubic use lattices with their [111] body diagonal vertical and the
same candidate site intensity as Random. With decay and smoothing disabled,
every lattice site is retained. Otherwise a three-dimensional ordered dither
selects sites according to the cube of local density. FCC groups its four parity
classes on an orthogonal half-cube grid and repeats through coarser checkerboards;
Cubic uses a cubic Bayer order. This preserves repeatable structure but introduces
gaps and repeating patterns. Poisson spacing calibration is approximate for
these foams.

Sparse infill direction rotates either lattice about Z through the object's
bounding-box centre. Complete FCC lattices have vertical Voronoi faces and faces
inclined about 35 degrees above horizontal. Complete Cubic lattices have faces
inclined about 55 degrees. Thinning and relaxation can create shallower faces;
none of the distributions guarantees self-supporting infill.

Snap points to walls defaults off. After optional relaxation, points within half
of nominal spacing on either side of a wall take a bounded step towards
the raw distance field's zero contour. Flat walls are the simplest case; the
approximate table, curves and corners limit accuracy. The density field remains
unchanged; cell boundaries follow the snapped points. Queries include nearby source points before
snapping so that partitioned requests reproduce the same field.

## Wall thickness and shell bonding

Fill Multiline uses Orca's native offset outlines around routed wall paths. Odd
counts retain the central wall; even counts use half-spacing offsets. Native
boundary connectors respect the infill anchor length and maximum anchor length;
a zero maximum disables these connectors.
Floor bands cover the wider footprint and are filled once at native solid row
spacing; they bypass wall offsets and connectors to retain their sweep order.
Nominal point spacing grows with bead count to approximately retain the selected
density.

Trace model shell defaults off. It adds one native infill bead at the physical
shell interface, including hole walls. Combined layer fill outlines exclude
modifier boundaries. Touching foam walls split the boundary bead into graph
edges; three-way boundary junctions use their generating control-point pair to
cycle pairings across layers, allowing
foam trails to turn along the shell. Traces are clipped to the original fill
surface so neighbouring modifier regions meet at their shared cut. The additional
boundary bead adds material.

## Fused density modifiers

Voronoi material density blend width appears directly below sparse infill
density when editing modifiers whose parent uses Voronoi, regardless of their
local pattern overrides. The -1 mm default preserves Orca's
separate regions. Zero merges density at the grid resolution; a positive width
blends entirely inside the modifier using
`t*t*(3-2*t)`, where `t` is inward distance divided by width, clamped to [0,1].
The modifier reaches its target density at that depth. The grid resolution
and distance quantisation limit boundary accuracy. Distance includes the
modifier's sides, floor and ceiling, using its sliced geometry.

Enabled modifiers contribute their sparse-infill material density in Orca's
modifier order. Density, blend width, wall proximity decay and corner smoothing
remain editable. A modifier inherits decay and smoothing unless it overrides
them; its target density uses the physical model's wall distances. Different
smoothing widths require additional filtering during preparation. Enabled
modifiers do not split the parent into separate print regions. Other controls
are disabled and display inherited values. Their stored overrides are inactive
until blend width returns to -1, when ordinary modifier behaviour resumes.
Overlapping modifiers blend successively. Ordinary modifiers still clip regions
normally.

One constellation follows the combined field. Candidate intensity is bounded by
the parent and modifier settings, including their maximum corner boost. Optional
relaxation follows local spacing.
Density is a statistical target; removing control points does not itself clip
walls out of a region.
A conservative large-cloud warning considers object bounds and the densest
modifier setting. Increasing multiline can reduce
point count at the cost of changing cell size and wall thickness; settings are
never changed automatically.
