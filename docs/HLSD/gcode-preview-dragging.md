# G-code preview while dragging

The sliced preview draws every toolpath segment of the plate as an instanced box. On a large
plate that is tens of millions of segments, and the frame is GPU-bound: the cost is the number of
instances drawn, not anything the CPU does per frame. Dragging the camera over such a plate cannot
keep up. The `preview_reduced_detail_mode` preference (*Graphics > G-code Preview*, off by
default) lets the preview draw less while the user drags and put the full toolpaths back when they
let go.

| Preference | Values | Effect |
|---|---|---|
| `preview_reduced_detail_mode` | `off`, `solid`, `layers`, `outer_walls`, `shell` | what is drawn while dragging |
| `preview_reduced_detail_layer_stride` | 1–20 | one layer in every N is kept by the toolpath modes |

libvgcode (`src/libvgcode`) builds and binds the reduced toolpath set, `GCodeViewer` maps the
preferences onto it and draws the solid model, and `GLCanvas3D` decides when the user is dragging.
The OpenGL ES path keeps a single set and ignores the preference.

## Two sets, one walk

`ViewerImpl::update_enabled_entities()` walks the visible vertex range once and fills two segment
index buffers side by side: the **full** set and the **reduced** set (segments and options).
Building them together is what makes switching free: starting or ending a drag is a buffer
binding, never a rebuild. A change of mode or stride does rebuild. Nothing is built while the mode
is off, and the reduced buffers are then uploaded empty so that the last set does not stay
allocated.

Whatever the mode leaves out, the bottom and top layers of the visible range are kept whole: they
are the faces the range cuts open, and the top is what the user is looking at.

### Modes

- `EndLayersOnly` (`solid` in the preference) keeps only the two end layers. `GCodeViewer` then
  draws the sliced objects and the prime tower as opaque solids, see below.
- `LayersOnly` (`layers`) keeps every role of one layer in every stride.
- `OuterWallsOnly` (`outer_walls`) keeps the outer and overhang perimeters of one layer in every
  stride. The prime tower and supports have other roles and are left out.
- `ShellOnly` (`shell`) keeps what the shell extraction below marks as visible surface, of one
  layer in every stride, plus whatever a view from above or below sees of the skipped layers, so
  that a step does not vanish. It is the only mode that knows the prime tower's outside from its
  inside.

## The solid model

The preview already loads the sliced objects as shells for its translucent ghost.
`GCodeViewer::render_solid_model()` draws those shells opaque, in their filament colors, with the
`gouraud` shader, whose z range cuts them to the visible layer range. The two toolpath layers of
the reduced set are drawn afterwards and cap the cut with what was really printed there. The
shells hold only the objects, so while this mode is on the prime tower is added from its sliced
mesh, positioned as the print placed it. It is added or removed on its own when the mode changes,
without reloading the objects, keeps its opaque color so that it never appears among the
translucent shells, and stays out of their bounding box. Supports have no mesh and are not shown,
and `load_shells()` drops every non-model-part volume, so a negative volume is not cut out.

A plate whose shells are not loaded keeps drawing toolpaths, since the solid model would leave
only the end layers.

## Shell extraction

`ViewerImpl::update_shell_bitset()` classifies every extrusion segment once per load, on demand
the first time the shell mode needs it, and records the result in four bit sets. It is purely
geometric so that the wipe tower, whose every segment shares one role, works as well as the
objects.

Each layer is rasterized into a coarse 2D **occupancy grid** over the print's footprint: cells
are 0.5 mm, or coarser so that the grid is at most 1024 cells across. The footprint is then
**closed** with a radius of 2.5 mm so that sparse infill and support read as the solid area they
belong to, while holes wider than 5 mm stay open. The closing dilates each 8-connected component
separately and leaves a cell that two components both reach empty, so the gap between two objects
standing close together is never bridged and both of their facing walls stay on the shell;
fragments under eight cells do not spread and are absorbed by whatever reaches them. A separable
erosion shrinks the result back, and the raw cells are OR-ed in again so that a closing never
loses one.

A cell is a **shell cell** when it is filled and any of its six neighbours, four in the layer,
one below, one above, is not. A segment is on the shell when at least half of the cells it
crosses are shell cells: walls run along the shell, infill only touches it at the ends. The
interior infill roles and gap fill are excluded regardless, since short infill segments hugging a
wall would otherwise pass by the thousand.

Two refinements keep sloped surfaces closed:

- **Near-shell inner walls.** The step between one layer's outer wall and the next is often
  narrower than a cell. An inner wall (`Perimeter`) segment whose midpoint lies within a line and
  a half of an outer or overhang perimeter of the same layer is kept as well.
- **Top and bottom visibility.** The same pass records the highest and lowest layer occupying
  each cell over the whole print. A segment whose layer is the topmost occupant of any cell it
  crosses is visible from above, and likewise from below with the lowest. These segments are kept
  even when their layer is skipped by the stride.

The layer range is split across up to eight `std::async` workers, each owning its grids. An
allocation failure on a huge print falls back to marking every segment as shell, which leaves out
only the hidden infill roles.

## Deciding that the user is dragging

`GLCanvas3D::_update_preview_interaction()` runs at the top of every preview frame, before the
canvas decides whether to reuse its cached scene, so that the switch lands in that frame. Dragging
is `GLCanvas3D::is_user_interacting()`, the same answer the scene cache reads: the camera, the
navigator, a gizmo, the rectangle selection or either slider being held. A slider reports this from
ImGui's active id rather than its dirty flag, which is raised and consumed inside one frame. A
wheel step has no duration, so it holds the reduced set for a 150 ms settle time instead, and the
frame that restores the toolpaths is scheduled for when that time runs out, since the render timer
only wakes the idle loop. A drag cut short by focus or capture loss is ended explicitly, and a
button release wakes the idle loop, because on some platforms nothing else would until the next
input.

## Reused scene frames

`GLCanvas3D` keeps its last scene pass for frames that only rebuild the overlay (`SceneCache`). Its
key covers the canvas size, the camera and hover state, not what the toolpath sets draw, so a frame
that reuses the scene must never be one on which the set is switched.
`_update_preview_interaction()` therefore reports whether the bound set changed, and a frame on
which it did redraws the scene. The canvas neither captures nor reuses the scene while the user
drags, so no reduced frame outlives a drag, and the frame that ends a wheel's settle time is
requested as a full frame.
