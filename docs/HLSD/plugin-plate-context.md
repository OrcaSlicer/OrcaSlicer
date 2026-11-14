# Plugin plate context

Plugins that associate library models with a plate need the live GUI selection
and the plate used by a print dispatch. These identify different moments:
changing the selected plate must not change a job's captured context.

## Live selection

`orca.host.plater().current_plate_index()` returns the zero-based plate selected
in the live GUI and must be called on the UI thread. It reads the plate list,
including selection restored through Undo or changed by reordering plates.
`Model.current_plate_index()` exposes the model's stored plate metadata; it is
not a live GUI selection query.

Moving a plate to the front emits `PlateSelected` after the reorder and final
selection are complete, including when the selected numeric index stays zero.
Plugins can invalidate allocations associated with the old plate order.

## Print dispatch

For `PrintJobStarted` and `PrintJobFinished`, `LifecycleEventContext.index` is
the zero-based local project plate resolved for that print's transport request.
Both events retain the same index even if GUI selection later changes. The value
is `-1` for external/SD-card prints, calibration jobs or an unresolved local
plate. Other event types retain their event-specific meaning for `index`.

The direct print job and multiple-printer task queue use the same conversion
from one-based transport plate numbers. Each task captures its request before
dispatch and uses it for both lifecycle events and the transport call.

Plugins can compare a prepared plate allocation to this job index without
guessing from the currently selected plate. The events describe dispatch:
successful `PrintJobFinished` means sending succeeded, not physical printing.

[Native GUI regression procedure](../../tests/slic3rutils/plate_context_gui.md).
