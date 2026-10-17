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

Each engine samples a complete, deterministic cloud over the object's height.
The exterior margin grows until covering bounds prove that unseen points cannot
change its sections. A nearest-point grid bounds the distance
throughout the domain, including the gaps between its samples. Exterior points participate;
only the resulting walls and floors are clipped to the fill surface.

An immutable CGAL 3D Delaunay triangulation supplies shallow-face floor polygons.
Workers retain their engines across a batch of layers; copied fillers can share
the immutable mesh. CGAL's 2D power diagram, weighted by vertical distance, supplies
wall sections. Its weights change with Z, so it is rebuilt
for each section. Geometry clipping removes overlapping
floors and wall sections within floors while retaining their supporting boundaries.

## Extrusion and routing

Faces become single wall lines when successive layers have at least half-bead
overlap. Shallower faces become floor bands, rounded outward to Orca's flow row
spacing while keeping the supported boundary fixed. At constant layer height,
continuing bands have at least 50% overlap before clipping; face ends, holes and
changing layer heights limit that guarantee.

Native Rectilinear fill prints floor rows along the face section in alternating
zigzags, advancing from the supported side. Orca's bead width, flow and spacing
are retained. Floor row ends extend into nearby previous-layer perimeter bead
footprints when reachable; sparse slice interiors are not treated as solid support.
This does not select bridge flow or guarantee support at both ends.

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

## Wall thickness

Fill Multiline uses Orca's native offset outlines around routed wall paths. Odd
counts retain the central wall; even counts use half-spacing offsets. Native
boundary connectors respect the infill anchor length and maximum anchor length;
a zero maximum disables these connectors.
Floor bands cover the wider footprint and are filled once at native solid row
spacing; they bypass wall offsets and connectors to retain their sweep order.
Nominal point spacing grows with bead count to approximately retain the selected
density.
