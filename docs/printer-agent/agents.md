# Built-in printer agents

*Owns the per-vendor behavior of the built-in agents: what each subclass
changes and what it inherits unchanged. Defers the interface every agent
implements to [Architecture](architecture.md) and
[Python plugin agents](plugin-agents.md).*

This chapter covers the built-in Moonraker family: the general
`MoonrakerPrinterAgent` and the Qidi and Snapmaker variants. Creality
(`CrealityPrintAgent`) is also a member of this family and inherits the base
behavior, but has no section here; see the capability matrix for its
per-feature coverage. They share the same connection and status machinery.
Change the base class only when the behavior is valid for all of them.

Each subclass is thin. `MoonrakerPrinterAgent` holds the HTTP connection,
the WebSocket status subscription, the REST command worker, thumbnail
lookup, the chamber-light heuristic, and the upload-and-start path.
`QidiPrinterAgent` overrides filament discovery and adds multi-color box
mapping; `SnapmakerPrinterAgent` overrides filament discovery and camera
setup; `CrealityPrintAgent` overrides filament refresh. Each derives from
`MoonrakerPrinterAgent` and is `final`, which is why the guard rule below
must be type-based.

## Moonraker family

### Connection and commands

Moonraker-family agents use plain HTTP for the LAN connection. The connection
path deliberately ignores a TLS request because the supported printer stacks
serve Moonraker or a reverse proxy over HTTP. Restoring the caller's TLS
default can send a connection to an unavailable HTTPS endpoint.

Status is a Moonraker WebSocket subscription. Commands use REST. Command
translation happens immediately, but the resulting HTTP work runs through one
agent-owned FIFO worker. Each queued operation captures the current base URL
and API key before it is queued, so a later printer switch does not redirect
an earlier command. Keep this separation: network work on the UI path makes
controls feel stalled, and allowing a queued command to reread connection
state can send it to the wrong printer.

Pause, resume, and cancel use the dedicated Moonraker print endpoints. Do not
replace them with queued `PAUSE`, `RESUME`, or `CANCEL_PRINT` G-code. The
endpoints interrupt the print directly; a G-code command can wait behind the
active print or macro.

The request router accepts the Bambu-shaped JSON used by the native device
tab. Supply object-shaped namespaces such as `print` and `system`. A malformed
but parseable payload with a scalar where the router expects an object can
still fail before the unsupported-command fallback. The supported generic fan
status is the standard `fan` object, which represents the part fan only.
Ordinary part-fan control also works through the legacy `gcode_line` path,
which sends `M106` while `is_enable_np` is false. Auxiliary and chamber fans
are neither reported nor controlled.

Do not add `cfg`, `fun`, `aux`, and `stat` to the Moonraker status payload just
to make it look more complete. Together those fields set `is_enable_np` and
make the UI choose its structured fan and extruder commands instead. The
Moonraker agent does not translate those commands, so working controls become
unsupported no-ops. This is a UI-routing constraint, not a reason to expose
structured fan support.

### Status shown by the native device tab

The agent translates Moonraker status into the Bambu-shaped status payload the
existing Device tab understands. Some fields are necessarily synthetic:

- The virtual SD-card readiness bit and a basic software-version row make the
  native UI consider the printer ready. Each pull payload also ensures
  `m_push_count` and `m_full_msg_count` are at least one and refreshes
  `last_push_time`. Together with the normal-storage state and a placeholder
  module version, this satisfies the native `is_info_ready()` and printing
  gates. These are compatibility scaffolding, not reports of physical storage
  or OTA support.
- Current and total layers are emitted only when `print_stats.info` contains
  numeric values. Moonraker may send `null`, and many profiles do not emit the
  `SET_PRINT_STATS_INFO` data needed to populate them. Do not turn that gap
  into a JSON conversion exception.
- Remaining time is estimated from elapsed print time and virtual-SD progress.
  It is omitted below two percent progress because the early estimate is too
  unstable. Do not derive an ETA by subtracting Moonraker duration counters:
  both are elapsed counters, so their difference is overhead, not remaining
  time.
- Temperature readings are available, but nozzle diameter and nozzle type are
  not supplied in the status payload. The UI can therefore show an unknown
  nozzle. Do not make print submission depend on those missing fields.

### Camera thumbnails and lights

For a running job, the agent asks Moonraker for thumbnails and chooses the
widest usable entry, rather than assuming the first entry is useful. It accepts
both thumbnail path spellings used by Moonraker versions, encodes each path
segment, and caches the result by filename. A failed transient lookup is tried
again only a bounded number of times; a clean response without a thumbnail is
cached as a negative result. The response shape handling is source-derived,
not hardware-verified.

> **Do not perform this HTTP lookup while holding `payload_mutex`.** The
> WebSocket thread builds the status payload under that mutex and the UI
> path also needs it, so a thumbnail timeout taken under the lock would
> stall status delivery or the UI. The lookup still blocks the WebSocket
> thread briefly, so move it to a worker if that becomes measurable.

Chamber-light control searches Moonraker objects for names that look like a
light or a standalone LED, then writes the first matching pin, LED, or macro.
The filter exists to avoid treating unrelated objects, such as a beeper, as a
lamp. It remains a heuristic. The incoming `led_node` is validated, but only
`chamber_light` is acted on; `chamber_light2` is deliberately ignored. A
printer with more than one lamp therefore has no reliable node-to-object map.

### Common maintenance limits

The same cache is reused for a selected agent ID, not per physical printer.
Qidi and Snapmaker inherit this behavior. A stateful feature added
to the base class must be reset carefully when a preset switches hosts.

> **Keep guards for this family type-based** - check whether an agent
> derives from `MoonrakerPrinterAgent` rather than comparing its ID to
> `moonraker`. An ID-based guard silently excludes Qidi, Snapmaker, and
> Creality, even though they share the base behavior.

The family has no generic implementation for firmware-specific AMS write
commands. Keep unsupported commands unsupported until the printer-side macro
or API is known. Reporting success for an untranslated command makes the
native UI claim that an action happened when it did not.

## Qidi

Qidi inherits the Moonraker connection, status, camera, and local-print path.
Its differences are Qidi filament discovery and the pre-print multi-color-box
mapping.

### Filament discovery

Discovery first reads the printer's device information to infer a Qidi series
identifier, then falls back to the configured Orca model if needed. Series
inference intentionally recognizes only a narrow set of known names. An
unknown model still produces usable generic filament data, but not a
series-specific preset identifier.

The agent reads a Qidi filament dictionary and the `save_variables` plus
slot-runout data. Failing to fetch the dictionary is non-fatal: slot discovery
continues with fallback material and colour values. Failing to fetch or parse
slot data is fatal to the refresh. A missing runout value means the agent
cannot prove filament is loaded, so it reports that slot as empty. This is an
ambiguity in the firmware data, not proof that the box is empty.

`save_variables.variables` must be an object. Qidi firmware can return `null`
there, and generic JSON value access can throw on a present null. The parser
rejects that shape without throwing. Preserve the null-slot tests whenever the
response parser changes.

### Multi-color mapping before a print

Before every Qidi print-start wrapper, the agent writes `enable_box` and, for
mapped tools, persistent `value_t<tool>` variables. These writes survive the
job. Invalid mapping JSON is checked only after `enable_box` has been written.
When the mapping is enabled, that failure can therefore leave `enable_box=1`.
There is no rollback for this or for a later per-tool write failure, so a
partial mapping can remain on the printer. An empty mapping is accepted when
the box is enabled. Single-colour jobs disable the box but leave old per-tool
assignments in place.

`enable_box` currently follows `task_use_ams`. That meaning has not been
verified against all Qidi firmware: if firmware treats it as "a box exists"
rather than "use the box for this job", this gate is wrong and needs hardware
evidence before it changes.

Only `start_local_print` reaches Moonraker's real upload-and-start path. The
other Qidi mapping wrappers currently return success stubs after applying the
mapping. Do not describe those wrappers as confirmed print paths.

Because the agent cache is keyed by agent type, a Qidi mapping can also become
stale when switching between Qidi printers. This is a generic Moonraker-family
state risk, made more consequential by Qidi's persistent firmware variables.
The configured `printer_type` can also be stale, so treat it as a fallback
hint rather than device truth.

## Snapmaker

Snapmaker uses the Moonraker base and overrides filament discovery and camera
setup. Neither path is hardware-verified in the current documentation set.

Filament information comes from parallel arrays in `print_task_config`.
`filament_exist` defines the number of slots; shorter type, subtype, colour,
vendor, or NFC arrays use safe fallback values. The agent first tries a visible
vendor, type, and colour preset, then a visible type match, and finally a
generic identifier when no preset bundle is available. An empty reported type
is changed to `PLA`, so an unknown occupied spool can look like confirmed PLA.
An unrecognized type can also reach the visible-preset fallback and be paired
with an unrelated visible preset. Treat the resulting preset as a suggestion,
not printer-ground truth.

Snapmaker U1 camera support starts the printer's monitor RPC, then serves the
still JPEG through a small local HTML page that reloads it after each load or
error. The wrapper is required because a direct still-image URL looks frozen.
The RPC is sent from a detached thread so the UI timer does not block on socket
I/O. That thread captures `this` directly, so agent destruction can race with
the camera command. Do not widen this pattern. Route future asynchronous work
through owned lifetime-managed work where possible.

## Source locations

- `src/slic3r/Utils/MoonrakerPrinterAgent.cpp`
- `src/slic3r/Utils/QidiPrinterAgent.cpp`
- `src/slic3r/Utils/SnapmakerPrinterAgent.cpp`
