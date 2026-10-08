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

## Wall proximity and smoothing

Wall-proximity decay defaults to 6 mm; zero gives a uniform base. Gaussian sigma
defaults to 8 mm; zero disables smoothing. With both zero, the original Poisson
provider is used without a distance table.

Along flat walls, local density stays at the selected setting within 2 mm,
then decays exponentially to a 1% tail after five decay lengths. The magnitude
of the difference between original and smoothed signed distances reduces the
effective depth near both convex and concave corners. This can boost density to
approximately 1.39 times the setting. Zero decay retains the corner boost with
a 6 mm response length. A reproducibly thinned Poisson candidate field follows
this density; thinning does not constrain face slopes or ensure vaulted roofs.

The distance field uses Orca's completed slice outlines, including holes,
floors and ceilings. `EdgeGrid` supplies the approximate planar signed distance
for each layer slab. Two Z sweeps accumulate distance to material and air using
lower envelopes of weighted squared endpoint distances. Identical consecutive
profiles merge; thin slabs between output planes and empty gaps still contribute.
Accumulation is linear in the number of profiles and output heights per XY
column, rather than scanning the whole volume for every profile.

The immutable cubic tables normally have 1 mm spacing, logarithmic distance
bytes covering 0–1,000 mm, and sign bits. A 32-million-node limit coarsens large
objects. Sampling interpolates decoded signed distances; density saturation
happens afterwards. The field is approximate, with no geometry fallback.

Gaussian smoothing uses separable CPU OpenCV filters with fixed-point working
values. Temporary filter borders extrapolate outward slopes; zero sigma reuses
the original field. Tables are shared by smoothing width, and point providers
retain their own decay setting. Reslicing clears the per-object cache.
Construction and smoothing report phase progress and check cancellation.

## Optional inner hull

The hull threshold is a percentage of selected sparse infill density; zero
disables it. For example, 20% at 25% infill removes local targets below 5%.
The hull uses the same corner-adjusted density as the point cloud.

Orca's marching squares extracts cavity contours, preserving nested holes.
Samples at the bead's mid-plane and upper/lower faces define its swept skin
and caps. A contour bead prints the hull wall; native solid Rectilinear fill
covers remaining skin and caps using sparse-infill flow width and spacing.

The cavity and skin are subtracted from ordinary walls and floors without
changing control points. Hull paths join the existing group ordering; solid-fill
islands retain their native sweep. Hull sections are reused across clipped
regions. External perimeters and top/bottom shells are unchanged. Printable
overhangs and vaulting are not enforced.
