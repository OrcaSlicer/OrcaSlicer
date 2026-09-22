# G-code preview while dragging

The sliced preview draws every toolpath segment of the plate as an instanced box. On a large
plate that is tens of millions of segments, and the frame is GPU-bound: the cost is the number of
instances drawn, not anything the CPU does per frame. Dragging the camera over such a plate cannot
keep up. With the `preview_solid_model_while_dragging` preference (*Graphics > G-code Preview*,
off by default), the preview draws the sliced objects as solid meshes while the user drags and
puts the toolpaths back when they let go. A mesh costs its triangles once, however many layers it
has.

`GCodeViewer` draws the solid model, libvgcode (`src/libvgcode`) keeps the toolpaths that cap it,
and `GLCanvas3D` decides when the user is dragging. The OpenGL ES path ignores the preference.

## The solid model

The preview already loads the sliced objects as shells for its translucent ghost.
`GCodeViewer::render_solid_model()` draws those shells opaque, in their filament colours, with the
`gouraud` shader, whose z range cuts them to the visible layer range. The shells hold only the
objects, so while the preference is on the prime tower is added from its sliced mesh, positioned
as the print placed it. It is added or removed on its own when the preference changes, without
reloading the objects, keeps its opaque colour so that it never appears among the translucent
shells, and stays out of their bounding box. Supports have no mesh and are not shown.

A plate whose shells are not loaded keeps drawing toolpaths, since the solid model would leave
only the end layers.

## End-layer set

The cut faces of the solid model are capped with what was really printed there: the toolpaths of
the bottom and top layers of the visible range. While the preference is on,
`ViewerImpl::update_enabled_entities()` fills a second, **reduced** index buffer holding just those
two layers, in the same walk that fills the full one. Building both together is what makes
switching free: starting or ending a drag is a buffer binding, never a rebuild.

## Deciding that the user is dragging

`GLCanvas3D::_update_preview_interaction()` runs at the top of every preview frame, before the
canvas decides whether to reuse its cached scene, so that the switch lands in that frame. Dragging
is the camera, the navigator or either slider being held; a slider reports this from ImGui's active
id rather than its dirty flag, which is raised and consumed inside one frame. A wheel step has no
duration, so it holds the solid model for a 150 ms settle time instead, and the frame that restores
the toolpaths is scheduled for when that time runs out, since the render timer only wakes the idle
loop. A drag cut short by focus or capture loss is ended explicitly, and a button release wakes the
idle loop, because on some platforms nothing else would until the next input.

## Reused scene frames

`GLCanvas3D` keeps its last scene pass for frames that only rebuild the overlay (`SceneCache`). Its
key covers the canvas size, the camera and hover state, not what the toolpath sets draw, so a frame
that reuses the scene must never be one on which the solid model is switched.
`_update_preview_interaction()` therefore reports whether the bound set changed, and a frame on
which it did redraws the scene. The canvas neither captures nor reuses the scene while the user
drags, so no solid-model frame outlives a drag, and the frame that ends a wheel's settle time is
requested as a full frame.

## Per-frame lookups

The segment template draws its box from 8 corners through an index buffer, so the vertex shader
runs at most once per corner. `get_estimated_time_at()`, which the tool marker tooltip calls every
frame, starts from the running time at the first vertex of the vertex's layer, kept per layer at
load, and adds only that layer's vertices. The sum runs in vertex order, so it matches a full
accumulation exactly while costing memory per layer rather than per vertex.
