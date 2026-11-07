# CAD face selection plugin API

`orca.host.cad` lets a plugin read the face picked in the Design viewport and subscribe to
changes. It is compiled only with `SLIC3R_CAD`. The experimental CAD preference still controls
whether the Design tab exists. This API does not construct the tab, select geometry, change the
model, or affect slicing.

## Snapshot contract

`selected_face()` returns `SelectedFace` or `None`. Only an explicit solid-face pick counts.
An edge, vertex or whole-body pick returns `None`. Sketches, reference planes, hover and
feature-tree highlights are not converted into face picks; highlighting something else does
not itself clear a separately retained native solid-face selection. Clearing the solid
selection, replacing its shape, or destroying the viewport clears the published snapshot.

The snapshot owns ordinary values, never a document pointer or an OCCT handle:

| Property | Meaning |
| --- | --- |
| `body_index`, `face_index` | Zero-based indices into the selected body's current CAD topology. They are not persistent IDs. |
| `selection_revision` | Monotonic revision of the published selection, including changes to its geometry. Not the CAD document's topology generation. |
| `centroid` | Face area centroid in OCCT shape coordinates, in millimetres. It can lie outside a concave face or inside a hole. |
| `normal` | Orientation-aware unit normal at the face's UV midpoint, or `None` when undefined there. A curved face has no single plane normal. |
| `area` | Face area in square millimetres, before the separate display transform. |
| `is_planar` | Whether OCCT identifies the underlying surface as a plane. |
| `body_to_design` | Row-major 4×4 affine transform from shape coordinates to the Design viewport, including a pending body move. Apply it to homogeneous points; normals require the inverse transpose of its linear part. |

These are CAD coordinates, not G-code or Prepare-plate coordinates. Commit to Plate and later
arrangement require a separate association with the resulting model volumes and instances.
The API does not expose an editable B-rep or boundary loops. Old snapshots remain readable,
but their indices must not be reused after deselection or a model edit.

## Events and lifetime

`on_face_selection_changed(callback)` returns a `FaceSelectionSubscription`. Retain the
handle for as long as the subscription is needed. `close()` is idempotent; `is_active()`
reports whether it is still connected. There is no initial notification: call `selected_face()`
after subscribing to obtain the initial state.

The callback is `def callback(face_or_none)`. It receives its own snapshot of that event;
`selected_face()` may already describe a newer selection. Events are deferred to the UI
thread after native picking. Keep handlers short and synchronous. Exceptions are logged
and do not escape into wxWidgets or prevent another subscriber from receiving its event.

The snapshot cache and subscription registry are locked, so a Script worker can read or
subscribe without blocking on the UI thread. No GUI, OCCT or Python work runs under that
registry lock. Delivery acquires the Python runtime lease and GIL, restores the owning
plugin's audit context, and counts the callback as an active capability call.

Subscriptions are disconnected before plugin unload and again when capabilities/modules
are released, covering reload, shutdown, failed loads, and a subscription created during
`on_unload`. Closing or dropping a handle discards queued deliveries; an invocation that
has already started may finish. Event closures hold weak handles and copied geometry, so
they do not keep a plugin or a destroyed viewport alive.

## Manual verification

Install `sandboxes/orca_cad_face_selection_plugin_any.py` on a build with this API. Enable the
CAD feature, restart, and import a STEP box in Design. Run the watcher capability, then:

1. Pick the middle of two different faces; the window should report their different indices
   and geometry. Its initial state should also work when a face was selected before starting.
2. Pick an edge, vertex, whole body or empty space; it should report no selected face.
   With no solid face selected, picking a sketch or highlighting a feature must not invent one.
3. Pick a face and rebuild the model, undo, open another project or create a new design; the
   previous selection must clear. Re-select a face and check that its revision increased.
4. Run the capability again to stop it, pick more faces, then run it to restart. Stopping must
   suppress further deliveries.
5. Reload/unload the plugin while events are pending, then close Orca. No old handler should
   run after teardown, and no callback should touch a destroyed window.

The automated selection-state tests cover copied snapshots, deferred ordering, revision
changes, unsubscribe, owner-specific teardown, dropped handles, and reentrant handlers.
The embedded-Python test checks the binding surface and rejects an unattributed subscription.
