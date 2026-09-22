# G-code preview while dragging

The sliced preview draws every toolpath segment of the plate as an instanced box. On a large
plate that is tens of millions of segments, and the frame is GPU-bound: the cost is the number of
instances drawn, not anything the CPU does per frame. Dragging the camera over such a plate cannot
keep up. The `preview_reduced_detail_mode` preference (*Graphics > G-code Preview*, off by
default) lets the preview draw less while the user drags and put the full toolpaths back when they
let go.

| Preference | Values | Effect |
|---|---|---|
| `preview_reduced_detail_mode` | `off`, `solid`, `layers`, `outer_walls` | what is drawn while dragging |
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
