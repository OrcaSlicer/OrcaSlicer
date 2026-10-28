# Center of mass markers — High Level Design

## Purpose and scope

The "Center of mass" item of the canvas toolbar menu, in the bottom left corner of the 3D view, marks
where the mass of the plate, of each object instance and of each body of an assembly is centered,
in Prepare and in Preview. It helps judge how parts will rest on the plate, for instance whether a
tall or leaning part could tip. It is a view setting: it changes nothing in the model, the slice or
the project file, and it does not reach plate thumbnails. The choice is kept in the app config as
`show_center_of_mass`, off by default. The assembly view and the Design tab have no markers.

Three kinds of marker share one shape, a sphere whose octants alternate between two colors:

- each plate, black and white, for everything on it;
- each object instance, light blue and white;
- each body of an assembly, red and yellow;
- in Preview, the supports and raft of each object instance, green and black.

A click on a marker opens a box beside it with the weight and volume of what it stands for, where its
center lies in that thing's bounding box and the size of the box, and its moments of inertia about
axes through the center parallel to x, y and z.

An assembly is an object of several parts or with negative volumes. Its bodies are the connected
solids its parts make once united, the bodies the separated infills option centers its infill on
(see separated-infills.md): parts that overlap or touch are one body, parts apart are separate
bodies. An object of one body, and every object of a single part, has no body markers, as its object
marker says it all.

Mass is volume times density, the `filament_density` of the filament that prints it, or 1.245 g/cm³
(`DEFAULT_FILAMENT_DENSITY`) for a filament without one. Prepare has the model only, so it takes each
part as a solid of the density of its filament, the part's own or else its object's. It reads each
filament's density from the filament's selected preset, edits not yet saved included, as slicing does:
the plater's own config holds the values of the filament edited last only. Preview has
what will be printed, so its markers come from the toolpaths, whose mass depends on walls, infill
and flow as well. There the solid markers are for what is printed up to the top layer the layer
slider shows: for the plate with brim, raft and supports, where the weight rests at that point of
the print; for an object or a body, its own extrusions. Each has a faded twin for the same at the
end of the print, so the slider shows the weight moving toward where it ends, and at the top layer
the two meet. In Prepare the plate has the model alone, as brim, raft and supports exist only once
sliced.

## Prepare: from the meshes

An object of one part takes the mass properties of its mesh at unit density, times its density, from
`its_mass_properties()`, which handles a mesh in a single pass. Each triangle and a fixed vertex of
the mesh span a tetrahedron whose signed volume is `V = a · (b × c) / 6`, with `a`, `b`, `c` taken
relative to that vertex. By the divergence theorem these volumes add up to the volume of a closed
mesh, their volume-weighted centroids to its center of mass, and their second moments
`V (a aᵀ + b bᵀ + c cᵀ + s sᵀ) / 20`, with `s = a + b + c`, to its own. Shells facing inward, such as
a cavity, subtract themselves, and flipping every triangle changes nothing. The sums are kept in
double precision and relative to a vertex of the mesh rather than the origin, which keeps them exact
for meshes far from it. The result keeps the spread of the mass about its center,
`(x - c)(x - c)ᵀ` averaged over the mass, from which the moments of inertia follow.

The CGAL routines that look alike do not compute this. `CGAL::centroid` weighs tetrahedra by their
unsigned volume, so it fails on cavities and on any shell that is not star-shaped from the fan's
apex; over triangles it returns the centroid of the surface, and over points the average of the
vertices, which depends on the tessellation. `CGAL::barycenter` with the signed volumes as weights
gives the same answer, but only after copying every tetrahedron into a vector of weighted points,
and takes two and a half times as long.

A mesh's result is in its own coordinates. Each `GLVolume` maps it to the world with its world
matrix `M`, and weighs it by the volume times the absolute determinant of that matrix: a center of
mass moves with any affine map, and the spread becomes `L S Lᵀ` for the linear part `L` of `M`, so no
mesh is ever transformed. The `GLVolume`'s matrices, rather than the
model's, let the markers follow an object while it is dragged, before the model is updated. Results
are cached by `ModelVolume` id; a `ModelVolume` takes a new id whenever its mesh changes, which is
the rule `reload_scene()` relies on to rebuild a `GLVolume`'s geometry, so a cached result never
outlives its mesh.

An assembly's parts overlap or touch, which the mesh formula would count twice, so `solid_bodies()`
slices them instead, in the object's coordinates. It cuts the height into 500 slabs, 100 while a part
is dragged, slices every part and negative volume at the middle of each slab, unites the parts and
cuts the negative volumes away, and links the islands of neighboring slabs that overlap into bodies
with `connected_bodies()`. Each island adds a prism of the slab's thickness at the density: its
area, and its first and second moments of area, from the same sums over the outline as the area,
with the slab's height for z. Where parts of different densities overlap, the later volume of the object counts, as
slicing clips every part by the parts after it; each part then weighs the region it prints, which is
credited to the island holding it. Each body also keeps the convex hull of its islands and the height
they span, whose corners, once transformed, give its bounding box, tight while the instance turns
about z only. The object is the sum of its bodies, its box that of its parts, as the object's size
shows it, and each plate the sum of the object instances `PartPlateList::find_instance()` puts on it,
so that an instance on no plate counts in none. The bodies are cached by `ModelObject` id with the volumes, types, densities and
transformations they were sliced from.

## Preview: from the toolpaths

`GCodeProcessor` sums the masses while it processes the G-code, in the same pass that builds the
moves, and leaves them in `GCodeProcessorResult`; `GCodeViewer` keeps a copy of them when it loads a
result. Nothing is stored per move.

Each extrusion weighs the volume of filament its E extrudes times the density of the filament that
extrudes it, so a print of several materials weighs each as it is. Flow ratio, line widths, ironing
and purging into infill all count
as printed. Its mass spreads evenly along the segment the bead's center runs, half the layer height
below the nozzle, in the frame of the stored moves: plate offset added, Z offset removed. Such a
segment from `a` to `b` adds `m (a + b) / 2` to the moments and `m (a² + a b + b²) / 3` to the second
moments along each axis, and its box widened by half the bead's height to the bounding box; not by
half its width, which the processor only estimates, so that a box runs along the walls' center lines. Arcs are already split into segments by the processor. Walls, infill, top and bottom surfaces, ironing
and gap fill make the parts. The brim and the support roles, the raft among them, count only in what
the plate prints. The skirt, the prime tower and custom G-code count nowhere.

The plate takes every extrusion, so it needs nothing more. The objects and bodies need the sliced
objects, which the G-code does not describe, so the G-code export hands the processor a locator
built from the `Print`; G-code opened from a file, or from a project sliced earlier, has no `Print`
behind it, and so shows the plate alone. Object labels would not do: profiles turn the four kinds
Orca writes on and off in every combination, and none of them tells the bodies apart.

The locator numbers the object instances and, for each assembly, the bodies of every instance. It
takes the bodies `PrintObject::prepare_infill()` found for separated infills, or, when that option
did not need them, links the islands (`Layer::lslices`) of neighboring layers into bodies with the
same `connected_bodies()`. For each extrusion of a part, it finds the first layer printed at or above
its height, as spiral vase rises through each layer, and the island holding it with an
`IslandLocator`, the one `solid_bodies()` credits its regions with: by the island's box, widened by
1 mm for walls reaching past it, with a polygon test only where boxes overlap, and the nearest
outline where none holds the point. The boxes of one layer's islands say nothing of the other
instances, so an instance whose widened box reaches another's, as copies placed side by side do,
tests the outlines alone, and outside them the nearest outline of all such instances wins. The island
gives both the instance and the body. The island found last is tried first, as extrusions mostly follow each other on one
island. Brim, raft and supports lie outside the islands. The brim counts in the plate only; a support
or raft extrusion goes to the instance whose footprint, the box of its widened islands, holds it, the
one whose center is nearest among several, or else the nearest footprint, as supports stand below and
around their object.

Each mass holds, for each layer id, the running total of what is printed up to that layer, the last
of which is the faded marker's, so the solid marker for any slider position is a single lookup. The layer ids are those
the moves carry, which are also the layers of libvgcode and of the slider; in a print by object they
follow the order of printing, so the solid markers show the objects printed so far as they are.

## Drawing

`smooth_sphere()` with a resolution divisible by four leaves every triangle within one octant, so it
splits into two models drawn with the `gouraud_light` shader in each kind's two colors. The radius is
9 pixels for the plate, 7 for the objects, 6 for the supports and 5 for the bodies, scaled like the canvas toolbar for the
display's DPI and kept constant on screen through the camera's inverse zoom. They are drawn in that
order, so that markers at one place show as rings. The faded markers are the same spheres at 40%
opacity, drawn before all the solid ones, which show over them where both meet.

The centers usually lie inside the objects, so the markers are drawn without the depth test and show
through the objects and anything in front of them. Back face culling keeps the far half of a sphere
from covering the near one. They are drawn after the ambient occlusion pass, which would otherwise
darken them as the surface behind them, and before FXAA, which smooths their edges.

The markers are part of the cached scene, so toggling them, or changing a filament's density while
they are shown, marks the scene dirty, and moving the layer slider redraws the scene with the solid
markers where they belong. In Prepare they are hidden while any gizmo other than Move, Rotate, Scale
and Lay on face is open, since the others work on the surface a marker would cover, and a hidden
object has no markers.

## Details

The markers drawn last are kept, and a left click is tested against them before it selects: each
center and a point a radius to its right are projected to the screen, and the click hits a marker
within that distance. The solid markers are tested before the faded ones and the smaller kinds
before the larger, the order in which they cover each other. A hit opens the details of that marker
and keeps the click from changing the selection; a click anywhere else closes them. The box is an
ImGui window beside the marker, redrawn with the overlay from the markers of the last scene, so it
follows a dragged object, and in Preview the layer slider. It closes when its marker is gone, or when
the number of markers of its kind changes, as then it may stand for something else.

Its title says what the marker stands for: the plate, an object, an assembly or a part, the body of
an assembly. The G-code export lists the object instances for the processor, marking assemblies, as
it hands it the locator.

Each marker carries its sums: mass, volume, first moments and the second moments about the origin
along each axis, `Σ m x²`, `Σ m y²` and `Σ m z²`, which add up from parts to objects to plates. The
moment of inertia about the axis through the center parallel to x is then
`m (σy² + σz²)`, with `σ² = Σ m x² / m - c²` along each axis, and likewise for y and z. Masses are
kept in mg, volume times density in g/cm³, and shown in g, volumes in cm³ and moments of inertia in
g·mm². In Preview the box tells the finished print from what is printed up to the layer shown, the
two weighing differently, and both are placed in the bounding box of everything the marker holds
by the end.
