# Mesh boolean — High Level Design

## Purpose and scope

The Mesh Boolean gizmo combines meshes into one: union, difference or intersection. It works in two modes, picked
from the selection when the gizmo is shown:

- **Part mode**: one object (a single full instance) with at least two volumes is selected. The lists hold volumes
  of that object, and the result is a volume of the same object.
- **Object mode**: two or more whole objects are selected, none of them cut, every instance fully on one plate. The
  lists hold objects, and the result is a new object.

The geometry lives in `MeshBoolean::ops` (`src/libslic3r/MeshBoolean.{hpp,cpp}`), built on
`MeshBoolean::mcut::make_boolean`; the gizmo is `src/slic3r/GUI/Gizmos/GLGizmoMeshBoolean.{hpp,cpp}`. The object
list's *Mesh boolean* context menu entry is a different feature, the CSG merge of one object's parts, and does not
use this gizmo.

## Lists

Clicking a part (part mode) or one of the selected objects (object mode) in the 3D view adds it to the active list
or removes it; the canvas selection never changes while the gizmo is open, so the gizmo stays in the mode it opened
in. Union and intersection use one list. Difference uses two, *Subtract from* and *Subtract with*, and the *Select*
button next to each chooses which list clicks go to. Picked entries are drawn in the list's colour through
`GLVolume::color_override`, which replaces the render colour and hides painting while it is set. Switching the
operation keeps the picks: with two picked, the first is the source and the second the tool.

Lists hold `ObjectID`s, and entries whose volume or object is gone are dropped. A selection change, or a change of
mode, clears them.

## What each operation does

The first entry of the first list is the source. For union and intersection the result replaces the source and
takes its settings; the other inputs are deleted by union, and by intersection only with *Delete input* ticked. For
difference, every *Subtract from* entry is replaced by its own result, and the *Subtract with* entries are deleted
only with *Delete input* ticked. An entry the operation leaves nothing of keeps its original.

- In part mode a difference cuts each part with the tools one after the other. The result volume keeps the
  source's name with an operation suffix, its type, config and material, and its transformation: the result mesh is
  expressed in the source volume's frame, so the part does not move.
- In object mode each object enters as the volume it prints: the union of its model parts over all its instances,
  minus the union of its negative volumes (`MeshBoolean::ops::solid`). A difference cuts each object with the union
  of the tool objects. The result is a new object with the source object's config and a copy of its first instance;
  the mesh is expressed in that instance's frame. Modifiers and support blockers and enforcers of the objects that
  make up the result are carried over, once per source instance; negative volumes are already in the mesh. Painted
  modifiers are left out, since they belong to a part of the source object. The new objects are added to the model,
  announced to the plates and added to the object list before the sources are deleted.

Inputs are taken to world space with left-handed transforms fixed and are flipped as a whole when inside out
(`MeshBoolean::ops::normalize_input`), so mirrored parts and objects work.

`MeshBoolean::ops` works shell by shell, because an MCUT dispatch only sees surfaces that cross, and MCUT leaves a
shell alone both when the surfaces don't cross and when it refuses a mesh. A union joins overlapping shells pair by
pair until no pair joins, so a mesh is never counted twice, and absorbs a shell lying inside another. A difference
drops a shell the cutter swallows whole, and an intersection keeps a shell lying wholly inside the other mesh. A shell
counts as inside only when every one of its vertices tests inside, by a ray cast from it, and lies clear of the other
surface; in doubt it is kept as it is, side by side in a union and uncut in a difference. Intersection tries the
inputs in several orders. When MCUT returns nothing for a pair whose shells come near each other, it falls back to
CGAL, but only on meshes that bound a volume and don't intersect themselves, as the CSG merge requires; otherwise, or
when CGAL fails, the operation reports a failure rather than an empty intersection.

A cavity of a hollow input is an inward shell of its own, which the shell-by-shell path keeps or drops unchanged:
that is right only while nothing else reaches into it. So when a cavity's bounding box meets a solid shell other than
the one around it, the pair (for a union of two or more inputs, all of them) goes to CGAL as whole meshes, which
handles cavities exactly, under the same conditions as the fallback above. When CGAL can't take the meshes or fails,
the shell-by-shell path runs instead.

## Painting

With *Keep painting* on (`keep_painting`), the painting of every input that ends up in a result is remapped onto
it: the source and other inputs for union and intersection, each part's own for difference. Paintings are read on
the main thread when the operation starts and transformed into the result's frame before `restore_painting`.

## Running

The operation runs as a job on the plater's worker, which shows its progress and a cancel button, and starts only
when the worker is idle. The job copies what it needs when it starts: shared mesh pointers, matrices and paintings.
While it runs, the operation button and the lists are disabled. The result is applied in the job's finalize, on the
main thread, only when the job was not cancelled, is the gizmo's latest, the gizmo is still open, and every input
is still there with the same mesh and the same volume and instance matrices; otherwise a warning says nothing was
applied. Closing the gizmo cancels the job without waiting. Cancellation is checked between MCUT dispatches, so a
long dispatch finishes before the job stops.

## Undo

Applying a result takes one *Mesh Boolean* snapshot, right before the model changes; the snapshots the deletes
would take are suppressed under it. Opening the gizmo, picking, cancelling and failing take none.
