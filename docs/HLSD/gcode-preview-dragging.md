# G-code preview while dragging

The sliced preview draws every toolpath segment of the plate as an instanced box. On a large
plate that is tens of millions of segments, and the frame is GPU-bound: the cost is the number of
instances drawn, not anything the CPU does per frame. Dragging the camera over such a plate cannot
keep up. The `preview_reduced_detail_mode` preference (*Graphics > G-code Preview*, off by
default) lets the preview draw less while the user drags and put the full toolpaths back when they
let go.

| Preference | Values | Effect |
|---|---|---|
| `preview_reduced_detail_mode` | `off`, `solid`, `shell` | what is drawn while dragging |

libvgcode (`src/libvgcode`) builds and binds the reduced toolpath set, `GCodeViewer` maps the
preference onto it and draws the solid model, and `GLCanvas3D` decides when the user is dragging.
The OpenGL ES path keeps a single set and ignores the preference.

## Two sets, one walk

`ViewerImpl::update_enabled_entities()` walks the visible vertex range once and fills two segment
index buffers side by side: the **full** set and the **reduced** set (segments and options).
Building them together is what makes switching free: starting or ending a drag is a buffer
binding, never a rebuild. A change of mode does rebuild. Nothing is built while the mode
is off, and the reduced buffers are then uploaded empty so that the last set does not stay
allocated.

Whatever the mode leaves out, the layers at the bottom and top of the visible range are kept whole:
they are the faces the range cuts open, and the top is what the user is looking at. An end layer
with no infill of its own does not close the print there: infill combination prints the sparse
infill of a few layers in one, leaving the others walls alone, and a support layer with no object
layer at its height gets a layer of its own. Each end therefore runs from the range's end layer to
the nearest layer with sparse infill, at most eight layers deep (`Layers::get_whole_end_layers()`),
and the `Viewer` reports the two bands so that the solid model's cut can sit at their edges. The
layer table is the whole plate's, so another object's top surface on the layer says nothing about
this one's combined infill; a layer with only solid infill ends the band when no sparse infill is
within reach, as on a print with none.

The realistic view's shadow-caster pass draws whichever set is bound, so the key of the static
shadow map includes whether the reduced set is: a map built during a drag is rebuilt on release.
With the mode off the reduced flag is never raised, and a drag changes nothing in the preview.

### Modes

- `EndLayersOnly` (`solid` in the preference) keeps only the end bands and the supports.
  `GCodeViewer` then draws the sliced objects and the prime tower as opaque solids, see below.
- `ShellOnly` (`shell`) keeps every role but the sparse infill, internal solid infill, internal
  bridge infill and gap fill, which lie under the skins or between the walls. Walls, top and
  bottom surfaces, bridges, supports and the prime tower are drawn whole, so from outside the
  print looks as it does at rest. The roles come from
  the slicer, so the classification is exact wherever the slicer's is; what it cannot express is
  the inside of the prime tower and of a support, which share one role with their outside and are
  kept whole. A profile with no top or bottom shell layers, or no walls, leaves the infill on the
  surface, so `GCodeViewer` turns the hiding off for such a print when it loads it, from the
  print's default region settings; per-object overrides are not consulted.

## The solid model

The preview already loads the sliced objects as shells for its translucent ghost.
`GCodeViewer::render_solid_model()` draws those shells opaque with the
`gouraud` shader, whose z range cuts them to the visible layer range: from the top of the bottom
band to the bottom of the top band, since those layers are drawn as toolpaths and a face of the
model in either plane would flicker against them. The translucent ghost is
drawn first, as in every other mode, so the part above the cut stays visible while dragging. The
cut itself is capped, one body at a time: a pass counts, per pixel and with the depth test off,
the body's back faces less its front faces into the stencil buffer, which is zero for a closed
body and one where the cut has opened it, and a quad in each cut plane, in the cap's colour
darkened, is drawn where it is not zero. Between the lines of a sparse layer on the cut, that quad is what shows, rather than
the first layer or the bed far below moving with the camera. A view through both cut planes of
a body with neither wall in the way is not capped. The toolpath layers of the reduced set are
drawn afterwards and cap the cut with what was really printed there; the band's reach to a layer
with infill is what puts real infill lines, not a bare cap, inside the walls. The shells hold only the objects, so while this mode is on the prime tower is added from its sliced
mesh, positioned as the print placed it. It is added or removed on its own when the mode changes,
without reloading the objects, keeps its opaque color so that it never appears among the
translucent shells, and stays out of their bounding box. Supports have no mesh, so the reduced set
keeps their toolpaths whole on every layer, as the shell mode does, and the objects stand on them
as they do at rest. `load_shells()` drops every non-model-part volume, so a negative volume is not
cut out.

The solid stands in for the toolpaths, so it wears their colours. In the views that colour by
filament (Summary, Filament, Tool) each body keeps the filament colour its shell was loaded with.
In every other view `GCodeViewer::update_solid_model_colors()` samples the toolpaths through
`Viewer::get_vertex_color()`: the body takes the colour of the first outer wall vertex in the top
band's lowest layer, each cap that of the first infill vertex in the layer it is cut under, sparse
infill before solid, and the prime tower that of its own role. One vertex stands for the role, so a
view that grades the toolpaths shows the solid in the first grade printed there. The colours are
resolved when a drag starts and when the view type or the bands change, not per frame.

A plate whose shells are not loaded keeps drawing toolpaths, since the solid model would leave
only the end bands. So does a print whose layers do not rise in height from one to the next, as
when it is printed object by object: the cut is a single height range for the whole plate, which
the ends of the layer range then do not bound. And so does a preview with a section view active:
the section cuts the toolpaths open to show their inside, which a solid would cover.

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
