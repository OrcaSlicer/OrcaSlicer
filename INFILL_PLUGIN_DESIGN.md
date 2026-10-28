# Infill Plugin Design

## Identity and persistence

Each infill capability provides exactly one pattern. Its stable, nonlocalized capability name is the
`sparse_infill_pattern` value, the UI option key, and the manifest identity. There is no second
pattern-name API. Names must be nonempty and must not collide with built-in infill pattern keys;
normal capability registry uniqueness prevents duplicate names within the infill type.

The pattern setting remains a string option with plugin type `infill`. Preset/project manifests record
the full plugin capability reference, preserving local or cloud plugin identity. Built-in pattern keys
and existing saved values remain unchanged.

## Dispatch and load timing

Before infill enters parallel slicing, Orca resolves the selected infill plugin references from the
print configuration and waits for their loads with one bounded deadline. A missing, disabled, failed,
or timed-out reference fails the slice; dispatch never silently substitutes another provider.

Each Fill adapter receives the exact project capability reference. The manager verifies capability
type, name, and plugin identity before returning the provider. Plugin generation continues through the
normal Fill pipeline, which handles path conversion, flow, and gap fill.

## Errors and diagnostics

Infill plugin failures become a generic, noncritical slicing error for the user. Detailed capability
identity and exception information remain available in plugin diagnostics and logs. Diagnostics remain
until plugin reload or explicit error clearing; successful concurrent infill calls do not clear an error
from a failing call.

## Lifecycle and UI

The infill selector lists enabled infill capabilities, stores their capability names, and restores the
stored value. Capability load, unload, and enable-state changes refresh available choices, revalidate
plugin references, and invalidate the current slice when needed. GUI work is marshalled to the main
thread.

## Scope

Plugin infill is a GUI slicing feature. CLI/headless plugin loading is out of scope.
