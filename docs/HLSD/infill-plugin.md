# Infill Plugins — High-Level Design

## Purpose

Infill plugins provide sparse-infill patterns while retaining OrcaSlicer's native fill lifecycle. A
plugin returns open paths for one surface island; the host remains responsible for extrusion conversion,
flow, gap fill, path ordering, and G-code generation.

## Capability identity and project references

One infill capability serves one pattern. Its stable, nonlocalized capability name is the
`sparse_infill_pattern` value. That shared identity aligns UI selection, preset persistence, plugin
manifest resolution, and registry lookup. Built-in pattern names remain reserved. A capability should
not derive its name from localized UI text.

The sparse infill setting is plugin-backed. Saving a preset or project records the capability's full
plugin reference, including local or cloud plugin identity. Slicing resolves only that referenced
provider; it does not substitute another loaded provider with the same pattern or silently fall back to
a built-in pattern. Existing built-in setting values remain unchanged.

## Slicing path

Before infill enters TBB parallel work, Orca resolves the selected provider references and waits for
their asynchronous loads against one bounded deadline. Missing, disabled, failed, or timed-out
references fail the slice. Fill creation receives the exact project reference and verifies its plugin
identity and infill capability type before constructing the adapter.

Each use creates a fresh `Fill` adapter that holds the provider alive. The adapter delegates per-island
path generation to the capability. Python implementations execute through the audited GIL trampoline;
native implementations must be thread-safe because separate surface generations can run concurrently.
The adapter forwards supported fill semantics such as path sorting and origin alignment.

The request is a value snapshot in the host's scaled-coordinate convention. Plugin paths pass through
the ordinary Fill conversion path, so the host applies flow and gap fill and emits the resulting G-code.
The host does not clip returned plugin paths again; they must remain inside the supplied expolygon.

## Failure and diagnostics

Plugin lookup and generation failures abort the slice as noncritical slicing errors rather than
crash-style critical errors. The user-facing message stays generic. Detailed plugin identity and
exception information are recorded in plugin diagnostics and logs, retained until plugin reload or
explicit error clearing. Cancellation remains a cancellation, not a plugin error.

## Lifecycle and UI

The sparse infill selector lists enabled infill capabilities and stores each capability name as the
setting value. Load, unload, and enable-state changes refresh live selectors, revalidate project
references, and invalidate stale slicing state. GUI updates are marshalled to the main thread.

## Scope and constraints

The API serves one pattern per capability; plugins needing multiple patterns register multiple
capabilities. CLI/headless plugin loading is not part of this integration. Project and preset values
must remain backward compatible, and plugin code must not manipulate the GUI from slicing workers.
