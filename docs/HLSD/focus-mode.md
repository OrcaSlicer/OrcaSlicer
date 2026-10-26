# Focus mode (hiding inactive plates) — High Level Design

## Purpose and scope

"Focus mode" is an entry of the canvas menu in the bottom left corner of Prepare. Turned on (eye
lit), the 3D view hides the inactive plates and shows the current plate alone, so a project with
many plates can be worked on one plate at a time. Turned off, every plate is shown. It is a view
setting: it changes nothing in the model, the slice or the project file, and it does not reach
plate thumbnails. The choice is kept in the app config as `focus_mode`, off by default.

Preview needs no such option, as it already draws the current plate alone and has its own plate
toolbar. The assembly view and the Design tab do not lay objects out on plates, so the entry is
disabled there.

## What is shown

The current plate is drawn with its bed, label and icons; the other plates are not drawn at all. The
world axes, which Prepare draws at the corner of the first plate, move to the corner of the current
plate, where Preview draws them.

An object instance is shown when its footprint, the XY bounding box of the convex hulls of its
volumes, overlaps the current plate, or when it is selected. The test is made per instance, so the
parts of an object are shown or hidden together, and a wipe tower follows its plate the same way.
It is geometric rather than the plate membership `PartPlateList` keeps, because an instance stays on
the plate it was first placed on: an object of another plate that reaches over this one, or one too
large for any plate, would otherwise be invisible while it takes room on the current plate. A
selected instance stays visible so that what a selection or a gizmo acts on is never hidden, for
instance an object picked in the object list on another plate.

## Hiding

A hidden object is neither drawn nor picked. `GLCanvas3D::_update_inactive_plates_visibility()`
runs at the start of each frame of Prepare. It clears `GLVolume::is_active` on the volumes of the
hidden instances and deactivates their raycasters, which every render pass, the hover and rectangle
selection, the shadow and center of mass markers already respect. It also deactivates the raycasters
of the other plates, whose icons would otherwise still take clicks where nothing is drawn. The state
persists between frames, so the raycasts made while handling the mouse, such as the anchor of a pan,
see the same scene as the frame.

Other features clear `is_active` too: gizmos hide the objects they do not edit and show them all
again when they close. `GLVolume::on_hidden_plate` records which volumes this update hid, so it only
shows again what it hid itself, and it waits for a gizmo that hides objects to close before doing
so. A volume another feature made active again is hidden again on the next frame. While Focus mode
is off and nothing is left hidden, the update returns at once.

## Switching plates

The other plates cannot be clicked while Focus mode hides them, so a compact counterpart of the Preview
plate toolbar takes their place: a column of plate thumbnails in the top left corner, below the top
toolbar, shown when the project has more than one plate and no gizmo is open. The thumbnails are the
ones the Preview toolbar shows (`PartPlate::thumbnail_data`); the canvas rebuilds its textures when
`Plater::plate_toolbar_image_version()` changes, a counter each consumer tracks on its own so that the
Preview toolbar's dirty flag is left alone. That counter moves after almost every edit, so the
thumbnail of the current plate, the only one edited while the others are hidden, is drawn again each
time, and the other plates' only once an edit to them has invalidated theirs. The current plate has an
accent border, a hovered one the gray border of the Preview toolbar, empty plates are dimmed and the
plate number sits in the corner; the tooltip gives the plate name. A click selects the plate and
clears the selection, as a click on a plate row of the object list does. The column scrolls when it
does not fit above the 3D navigator or the canvas toolbar. Its scrollbar works as the Preview
toolbar's, with which it shares its style: it shows only while the mouse is over the column or drags
the bar, and the mouse wheel scrolls the column either way.

Whatever selects a plate while the others are hidden, the camera moves its target over the new plate
and keeps its angle, distance and height (`PartPlateList::select_plate()`), so the plate comes into
view where the previous one was. A zoom to all plates, as after loading a project or adding a plate,
frames the current plate instead. Turning Focus mode on does the same, so the current plate fills the view.
