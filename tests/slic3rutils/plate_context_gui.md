# Native plugin plate context regression

Run in the built desktop app through an enabled plugin's Page message callback
and lifecycle observer. Prepare two plates with visibly different models using
ordinary Orca controls. Use a disposable printer or test transport for dispatch.

- Query `plater.current_plate_index()` from the plugin's UI callback after
  selecting each plate, reordering it and undoing/re-doing plate creation or
  imports. It must match the visibly selected plate's zero-based position.
  Do not use the model's stored plate metadata as the live selection oracle.
- With an already-dirty project, move a different plate to the front while the
  first plate is selected. Record exactly one `PlateSelected` event after the
  reorder; its index and the live getter must both be zero, with the moved
  plate's models present. Repeat with the moved plate already selected. Any
  allocation prepared before reordering must be invalidated.
- Record the index on actual `PrintJobStarted` and `PrintJobFinished` callbacks
  for a disposable local project print dispatch. It must match the resolved
  plate sent to the transport, with the same value in both events. Changing
  GUI selection after dispatch must not change the finished event's index.
- Exercise both the direct PrintJob and the multiple-printer task queue where
  their transports are available. SD-card/external and generated calibration
  jobs must report `-1`; they must not borrow the current GUI plate's index.
- Query the live getter from a worker thread. It must raise a clear error
  without accessing GUI state.
- Sending success alone does not prove that a printer completed the print.
