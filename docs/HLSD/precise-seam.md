# Precise Seam — High Level Design

## Purpose and scope

Precise Seam places the seam where a helper volume intersects the external
wall. The user attaches a mesh to an object as a Precise Seam modifier, and on
every layer the seam placer reads the modifier's slice to decide where the seam
of each external perimeter may, must or must not go. The same mesh keeps
working after the model changes, so the seam does not have to be repainted
after every design revision, and a swept helper body can guide the seam along
any path.

The modifier is non-printing geometry. It does not take part in slicing, region
assignment, filament selection or brim adhesion. It affects only seam
placement, which runs during G-code export.

## Volume types and priority

Precise Seam adds six `ModelVolumeType` values after `SUPPORT_ENFORCER`. The
strong types come first and the weak types follow. `is_precise_seam()`,
`is_precise_seam_strong()` and `is_precise_seam_weak()` are range checks that
depend on this order.

| Type | Group | Effect on the perimeter |
| --- | --- | --- |
| `PRECISE_SEAM_CENTER` | strong | seam at the arc-length midpoint of the intersection |
| `PRECISE_SEAM_LEFT` | strong | seam at the first point of the intersection |
| `PRECISE_SEAM_RIGHT` | strong | seam at the last point of the intersection |
| `PRECISE_SEAM_ENFORCED` | weak | intersection marked as enforced |
| `PRECISE_SEAM_BLOCKED` | weak | intersection marked as blocked |
| `PRECISE_SEAM_NEUTRAL` | weak | intersection reset to neutral |

A strong modifier fixes one point. A weak modifier only changes the
enforced/blocked type of seam candidates, and the configured seam position then
chooses among them. First and last are taken along the perimeter made
counter-clockwise seen from above. On an outer wall seen from outside, Left is
the left end of the intersection. On the wall of a hole seen from inside the
hole, the two ends are swapped.

The order of volumes in the object is the priority order, highest first.
`ModelObject::sort_volumes()` keeps every strong modifier before every weak one
and preserves the user's order within each group. The object list lets the user
drag a modifier only within its own group. A type change that crosses a group
boundary moves the volume to the end of its new group, where it has the lowest
priority. Strong modifiers are tried in this order, and the first one that
yields a seam on a perimeter wins. Weak modifiers are applied from the lowest
priority to the highest, so the highest one overwrites any overlapping zone.

## Model storage and 3MF compatibility

Projects must stay readable by earlier releases, and the modifier must not
change a print there. Both 3MF writers therefore store a Precise Seam volume as
an ordinary parameter modifier: `modifier_part` in the Bambu-format part
subtype, and `ParameterModifier` together with the legacy `modifier` flag in
the Prusa-format volume metadata. The seam mode is written separately under
`precise_seam_type`, using the names from `ModelVolume::type_to_string()`
(`precise_seam_center` and so on).

On load, the mode applies after all other volume metadata, regardless of XML
key order, and only when the base type is a modifier. Missing or unknown modes
leave an ordinary modifier. Seam metadata on any other base type is ignored.
Files that stored the seam mode directly as the volume type still load.

A Precise Seam volume keeps any per-volume settings it had as a part or
modifier, but they are inactive and the object list shows no settings item for
it. The writers prefix these keys with `precise_seam_config:`, so an earlier
reader drops them as unknown options. The volume therefore loads there as a
modifier without settings and has no effect on the print. The current reader
restores the keys only when the volume ends up as a Precise Seam type, so the
settings return when the user changes the type back. Configuration values are
XML-escaped in both writers, for every volume type.

## Print invalidation

`Print::apply()` compares the Precise Seam volumes of each object by type, ID
and transformation. Adding, removing, moving, reordering or retyping one
cancels background processing and invalidates only `psGCodeExport`; the sliced
layers are kept. `model_volume_list_update_supports_and_seams()` then brings
the support and Precise Seam volumes of the print's model copy in line with the
new model in one pass. A volume may switch between the two families, since
neither affects slicing. A conversion to or from a part or ordinary modifier
changes the solid and modifier volume lists and reslices as before.

## Modifier slices

`SeamPlacer::init()` collects the Precise Seam volumes of each object once:
strong ones in priority order and weak ones reversed. It slices each volume
separately with `PrintObject::slice_single_volume()`, which shares
`slice_modifier_volumes()` with support blockers and enforcers but does not
merge volumes, so each keeps its own priority. The result is cached per volume
and indexed by object layer; `Layer::id()` includes raft layers, which are
subtracted. Seam candidates are then gathered in parallel over the layers and
read the cache without locking.

Objects without Precise Seam volumes follow the unchanged seam placement path.
For objects that have them, perimeter extraction also removes consecutive
duplicate points and the repeated closing point of each extrusion loop.
Zero-length edges at path junctions would otherwise prevent point insertion
there. Distinct visits to one point of a self-touching contour are kept.

## Finding the wall segment

The seam placer works on the external perimeter loops of each layer, both
outer contours and holes, each made counter-clockwise. For every modifier
polygon on the layer that overlaps the perimeter's bounding box, the region
enclosed by the perimeter is clipped against the modifier polygon. The boundary
of each intersection polygon alternates between runs that follow the perimeter
and runs that follow the modifier outline. The wall segment is the longest
continuous run of intersection vertices that lie on the perimeter, measured in
vertices.

The fast path first finds an intersection vertex that exactly matches a
perimeter vertex. It then walks forward and backward, expecting the adjacent
perimeter vertex and falling back to projection when Clipper has merged or
split collinear edges. A vertex counts as on the perimeter when its projection
is within about 1.6 nm, which covers Clipper's rounding. If no vertex matches
exactly, or every vertex lies on the perimeter, the general path projects all
vertices. When every vertex is on the perimeter, the edge midpoints are checked
instead: a modifier chord can join two perimeter vertices directly, and the
chords split the vertex ring into runs. If no edge leaves the perimeter, the
perimeter lies entirely inside the modifier.

`Polygon::point_projection()` optionally reports the edge that holds the
projection, and every point of the segment keeps the index of its perimeter
edge. New points are inserted on that edge. A point within 1 µm of an existing
vertex snaps to that vertex instead.

## Strong modifiers

For a strong modifier, the target is the first point, the last point or the
arc-length midpoint of the segment. The midpoint is projected back onto the
original perimeter, because Clipper may have merged several perimeter edges
into one segment edge. The target is inserted into the perimeter, and a helper
point is inserted 1 µm before and after it. Strong modifiers are tried in
priority order, the first valid intersection decides the seam, and weak
modifiers are not processed for that perimeter.

When candidates are built, the inserted point is the only enforced candidate
and becomes the central enforcer; every other candidate is blocked. The seam
position modes then pick that point: Aligned and Aligned Back prefer the central
enforcer, while Back, Random and Nearest rank enforced candidates above blocked
ones. Alignment and random placement can still move the final position along an
edge. After alignment, `restore_precise_seam_positions()` writes the exact point
and its index back into every perimeter that has a strong seam. Inner walls take
their seam from the external seam as usual, including staggering.

## Weak modifiers

Weak modifiers produce one segment per intersection polygon, so one modifier can
mark several zones on one perimeter. All segment boundaries are inserted into
the perimeter in order of decreasing arc length. Each insertion then leaves the
indices of the pending, shorter ones unchanged; a point on the closing edge is
appended rather than inserted at index zero. A helper point is added 1 µm
outside each boundary. Random placement picks a position along the edge that
follows a candidate. These helpers keep that edge 1 µm long at each boundary, so
a zone cannot extend or intrude further than that. Boundaries that coincide
share their helper points.

The zone types are then resolved in priority order, and the edges of enforced
zones are subdivided into steps of at most
`SeamPlacer::enforcer_oversampling_distance` (0.2 mm). The middle candidate of
the longest enforced patch is therefore close to the geometric middle of the
zone. That patch is measured in candidates, across the closing edge, regardless
of where the contour starts; the same rule applies to painted seams.

Candidates first receive their type from seam painting. The weak zones then
overwrite it, lowest priority first. Blocked and Enforced zones therefore take
precedence over painting, and Neutral clears painting inside its zone.

## Unsupported geometry and warnings

Some modifier shapes cannot be resolved to one seam or one zone per crossing.
They are detected cheaply and reported rather than guessed:

- A strong modifier that crosses a perimeter in more than one place uses only
  its first valid segment. The other crossings are ignored.
- A modifier that crosses the whole region enclosed by the perimeter is
  detected when the modifier outline minus that region leaves more than one
  piece, none of them a hole. Its intersection holds two wall runs, and only
  one of them is used.
- A modifier whose slice has a hole on a layer, found as a clockwise polygon in
  the flattened slice, is skipped on that layer. The flattened slice no longer
  records which hole belongs to which contour.
- A perimeter that lies entirely inside a modifier is ignored by that modifier.

The conditions are atomic flags shared by all layers and objects. After all
objects are processed, `SeamPlacer::init()` issues at most one non-critical
warning with the ID `SlicingPreciseSeamWarning`. The warning is a single line
that lists every cause found, because the export warnings dialog shows only the
first line of each warning. Repeated warning events replace this notification
instead of appending text to it.

## User interface

- *Add Precise Seam* in the object menu creates a Center modifier from a
  primitive or a loaded mesh. Text and SVG volumes cannot become Precise Seam
  modifiers: the menu does not offer them, and `ObjectList::set_volume_type()`
  refuses the change.
- *Change Type* has a single *Precise Seam* entry. It converts other volumes to
  Center and keeps the mode of volumes that are already Precise Seam. The
  *Precise Seam Type* submenu appears only when every selected item is a
  Precise Seam volume, including settings rows that resolve to one. It sets the
  chosen mode on all selected volumes.
- Each mode has its own icon in the object list and its own color in the 3D
  view, at 60% opacity: warm oranges for the strong modes, and green, red and
  gray for Enforced, Blocked and Neutral.
- Object list drops map visible rows to volume indices while skipping hidden
  cut connectors, and they refresh the row-to-volume map of the object.
- Precise Seam volumes have no filament, block pasting into SLA, and are exposed
  to Python plugins as `ModelVolumeType` values plus the `is_precise_seam*()`
  methods.

## Implementation and verification

- [PreciseSeam.cpp](../../src/libslic3r/GCode/PreciseSeam.cpp) implements segment
  detection, point insertion, weak-zone resolution and position restoration.
  [SeamPlacer.cpp](../../src/libslic3r/GCode/SeamPlacer.cpp) integrates it into
  candidate gathering and issues the warning.
- [Model.hpp](../../src/libslic3r/Model.hpp) defines the types and their order,
  [PrintApply.cpp](../../src/libslic3r/PrintApply.cpp) handles invalidation, and
  [PrintObjectSlice.cpp](../../src/libslic3r/PrintObjectSlice.cpp) slices the
  modifiers. [bbs_3mf.cpp](../../src/libslic3r/Format/bbs_3mf.cpp) and
  [3mf.cpp](../../src/libslic3r/Format/3mf.cpp) store them.
- [GUI_Factories.cpp](../../src/slic3r/GUI/GUI_Factories.cpp) and
  [GUI_ObjectList.cpp](../../src/slic3r/GUI/GUI_ObjectList.cpp) provide the menus,
  type changes and ordering.
- [Precise Seam tests](../../tests/fff_print/test_precise_seam.cpp) cover the
  strong positions, including a midpoint on an existing vertex or the closing
  edge. They also cover shared and coincident weak boundaries, every warning,
  and the priority order.
- [Seam placer tests](../../tests/fff_print/test_seam_placer.cpp) cover
  enforced-patch selection independent of the contour start, fully painted
  contours, duplicate removal, and `Print::apply()` synchronization through
  type changes and restored model snapshots.
- [3MF tests](../../tests/libslic3r/test_precise_seam_3mf.cpp) cover the round
  trip of every mode and of inactive settings, attribute escaping, and which
  metadata combinations restore a seam mode.
  [Plugin tests](../../tests/slic3rutils/test_precise_seam_plugin.cpp) cover the
  Python bindings.
