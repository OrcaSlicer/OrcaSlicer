# Three-dimensional Voronoi sparse infill

The `voronoi` pattern prints layer sections of faces between nearest 3D control
points. Point generation, cell geometry, wall routing and extrusion are separate
stages. Density is statistical: clipping and floor-row rounding affect the
material deposited.

## Point cloud and geometry

`PointCloudProvider` supplies reproducible XYZ points through half-open box
queries. The random provider uses spatially seeded Poisson sampling, calibrated
to infill density and bead count. The random cloud is fixed in model coordinates;
sparse infill direction has no effect on it.

Exterior points participate so that the model boundary does not distort cells.
Covering bounds determine how far the known cloud must extend. Resulting walls
and floors are clipped to the fill surface. Object-wide XY coverage lets a
layer section serve multiple fill surfaces, at extra cost on sparse islands.

Each engine retains the points that can affect its working slab. Covering
radii bound ingestion above and retirement below it. A persistent CGAL 3D
Delaunay triangulation inserts new sites and batch-removes retired clusters,
retaining unaffected connectivity. Its shallow faces supply floor polygons.
A weighted 2D power diagram supplies wall sections and covering bounds;
it is rebuilt at each height because its weights depend on vertical distance.

Point ingestion advances in complete generation cells. Points remain unchanged
between batches; obsolete lower cell rows are retired before new cells are
generated. The Delaunay network removes those vertices before inserting new ones.

A worker-local registry lets successive fillers reuse an engine within an
ascending layer batch. Workers and cloned fillers own independent networks.

CGAL supplies the Delaunay and power-diagram geometry and is required at build
time. Slicing cost and memory grow with point count; large, dense objects are
more expensive. Fill Multiline thickens walls without adding more cells.

## Extrusion and routing

Steep faces produce wall lines. Shallow faces produce floor bands rounded
outward to native flow spacing while keeping the supported boundary fixed.
At constant layer height, continuing bands overlap by at least half a bead
before clipping; face ends, holes and changing layer heights limit that guarantee.

Native Rectilinear fill prints alternating floor rows along the face section,
advancing from the supported side. Reachable row ends extend into previous-layer
perimeter footprints. This does not guarantee support at both ends or select
bridge flow. Orca's bead width, spacing and flow are retained.

Wall edges form trails through paired junctions. Pairing favours straight
crossings; three-way junctions cycle turns across layers using their generating
points. Closed-trail starts are hashed by layer to scatter seams. Each wall
edge is traversed once.

Orca orders wall trails and complete floor sweeps as reversible endpoint chords.
Wall trails can reverse; floors retain their supported-side-first
order, so their actual entry and exit may differ from the scheduled chord.

Fill Multiline uses Orca's native path offsets: odd counts retain the central
line, while even counts use half-spacing offsets. Boundary connectors respect
infill anchor length and maximum anchor length. Wider floor bands are filled
once at solid row spacing, bypassing wall offsets and connectors.

Ordering is local to each fill surface. Anchoring queries and extrusion use
the same nominal region flow and cloud settings, so first-layer widths and
adaptive layer heights do not move points.
Layer completion reports throttled progress and checks cancellation.
