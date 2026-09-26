# Section view — High Level Design

## Purpose and scope

Section view hides whatever lies between the camera and a plane, so the user can look inside
objects in Prepare and in the assembly view, and inside the toolpaths in Preview. It is a view
setting: it changes nothing in the model, the slice or the project file, and it does not reach
plate thumbnails.

The user controls it from the section button of the canvas toolbar in the bottom left corner of
the 3D view. The button opens a panel above it with a slider for the depth of the cut, a "Set
viewing angle" button that turns the plane to face the camera at the same depth, and a button that
resets the depth to zero. The panel is an ordinary overlay window, not a popup, so the scene keeps
taking clicks and drags while it is open; the button closes it again. The button is highlighted
while a section cuts the scene and has shortcuts of its own: the mouse wheel over it moves the
plane, a right click switches the section off and back on, and a middle click sets the viewing
angle. Alt + mouse wheel moves the plane anywhere in the 3D view, with or without a gizmo open.

## State

Prepare and Preview share one section view, so a cut made in either tab is the same cut in the
other. The assembly view, whose objects sit apart from their places on the plate, and the Design
tab keep their own. The section itself is two values.

- **Ratio**, from 0 to 1. At 0 the section is off. As the ratio grows, the plane sweeps the
  sphere around the objects, from its side facing the camera to the opposite side, so at 1
  everything is cut away.
- **Normal**, taken from the camera direction when the ratio leaves 0, and again whenever the
  user presses "Set viewing angle". The plane keeps that orientation while the camera orbits,
  so the cut face can be seen from any side.

The ratio in use when the section is switched off is kept. The right click on the button brings
the section back at that ratio and with its old normal, so it restores the same cut, while the
slider and the wheel start a new cut facing the camera. Whether the panel is open is shared along
with the section.

The sphere is recomputed every frame from the volumes of the canvas the section view belongs to:
the objects on the current plate, or every object when that plate is empty. Preview holds no
objects of its own, so it places the plane across the volumes of Prepare, which makes it cut the
toolpaths exactly where Prepare cuts the objects. Only G-code opened on its own, without objects,
is measured by its toolpaths. In the assembly view the sphere is around the whole assembly. The
ratio therefore keeps its meaning when objects move or the user switches plates. It is not a
fixed position in world space.

Only the tab on screen can change the section, and switching tabs redraws the whole scene and
closes the open gizmo, so neither tab ever shows a stale cut.

## Where the plane applies

`GLCanvas3D::_get_section_view_plane()` turns the state into a plane in the convention of
`ClippingPlane::is_point_clipped()`. Everything that draws or picks the scene reads that plane.

- **Volumes.** The plane goes to the `clipping_plane` uniform of the volume shaders. The same
  uniform serves the gouraud, phong and X-ray passes and the colour picking pass.
- **Cut faces.** Clipping only discards fragments, which would leave the cut volumes hollow.
  `_render_section_view_caps()` draws their cut faces with one `MeshClipper` per model part the
  plane passes through. A clipper recomputes its face only when the plane or the volume moves.
  Modifiers, the wipe tower and SLA auxiliaries get no face.
- **Toolpaths.** libvgcode takes the plane through `Viewer::set_clipping_plane()`. It draws each
  extrusion as only the faces of a diamond-section prism that turn towards the camera, so
  discarding the fragments on the clipped side would leave open shells. Instead, the segment
  shader follows the view ray from a fragment that is cut away to the plane. When the
  extrusion's diamond section still holds that point, the fragment is shaded as the cut face, lit
  as the plane faces; otherwise it is discarded. The cut face keeps the depth of the fragment it
  replaces, which is safe: along that ray everything else still shown lies behind the plane. The
  shader writes no `gl_FragDepth`, so early depth testing survives. Option markers are cut away
  whole, by their centres. The shadow casters share the segment shader, so what is cut away casts
  no shadow either. Preview shells are drawn by another shader and are not clipped.
- **Picking.** `get_raycaster_clipping_plane()` returns the same plane, so hover, selection and
  the perspective pan anchor ignore what the user cannot see.

## Gizmos

A gizmo that clips its object itself owns the gizmo data pool's `ObjectClipper`, and its plane
replaces the canvas section while the gizmo is open. `GLGizmosManager::get_clipping_plane()`
reports that plane, or nothing when no open gizmo has a clipper. There are two cases.

- **Painting tools and brim ears** show the canvas section on the object they edit.
  `GLGizmosManager::update_section_view()` copies the ratio and normal into their clipper
  whenever the pool is updated or the section changes. The clipper then places the plane across
  the edited instance, which is the only object shown. The painting tools keep clipping their
  own triangles, raycasts and cut face through it. Brim ears always cut horizontally from the
  top, because the ears sit on the plate. At ratio 0 the clipper holds no plane at all, so the
  raycasts are not clipped.
- **Cut and mesh boolean** use the clipper for their own purposes, so the canvas section is
  suspended while they are open.

Every other gizmo, including move, rotate and scale, leaves the canvas section in place.

## Alt + mouse wheel

The canvas handles Alt + wheel after the gizmos had their turn, so it works the same in every
tab and with any gizmo open. On Windows, releasing Alt when no key was pressed since it went down
opens the window menu, and a wheel turn does not count as a key. Under the custom title bar that
menu is invisible, yet it takes the keyboard and the next click, which looks like a frozen 3D
view. After Alt + wheel the canvas therefore consumes the Alt release instead of passing it on.

## Redraw

The button and the panel are part of the ImGui overlay, which is built after the frame's scene is
drawn. A change to the section therefore marks the scene dirty and asks for one more frame. The
cached scene is never reused across a change.
