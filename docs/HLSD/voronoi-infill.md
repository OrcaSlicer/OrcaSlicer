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

`ActivePointCloud` expands its known region until nearest-site covering bounds
prove that unknown points cannot affect the working slab. Points outside the
model participate; resulting walls and floors are clipped to the fill surface.
Points sufficiently far below the slab are retired. Each filler owns its active
constellation, and geometry is rebuilt from those points.

CGAL's 2D power diagram, weighted by vertical distance, gives an exact section
of the supplied points' 3D Voronoi cells. Shallow faces also produce floor
polygons. Geometry clipping removes overlapping floors and wall sections within
floors while retaining their supporting boundaries.

## Extrusion and routing

Faces become single wall lines when successive layers have at least half-bead
overlap. Shallower faces become floor bands, rounded outward to Orca's flow row
spacing while keeping the supported boundary fixed. At constant layer height,
continuing bands have at least 50% overlap before clipping; face ends, holes and
changing layer heights limit that guarantee.

Native Rectilinear fill prints floor rows along the face section in alternating
zigzags, advancing from the supported side. Orca's bead width, flow and spacing
are retained. This does not select bridge flow or detect bridge supports.

Wall graph vertices snap to fixed representatives. Branch pairing favours
straight crossings; three-way junctions cycle turns across layers using their
generating points. Trails traverse each wall edge once. Wall trails and complete
floor groups are ordered together by nearest permitted entry, using each group's
actual exit. Walls may reverse; floors retain their supported-side-first sweep.
Ordering is local to a fill surface, and disconnected paths still require travel.

Density is statistical: spacing calibration accounts for floor material, but
clipping and row rounding affect the result. Nominal extrusion dimensions keep
first-layer widths and adaptive layer heights from changing the point field.
Infill reports completed layers with throttled UI updates and cancellation.
