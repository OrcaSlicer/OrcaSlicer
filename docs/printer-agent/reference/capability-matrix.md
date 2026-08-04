# Printer-agent capability matrix

This is a compact lookup for the built-in Moonraker family. It combines
implementation state with recorded evidence. It is not a promise that every
firmware configuration behaves the same way. Python plug-in behavior depends
on the plug-in, not on this matrix.

Use [testing and troubleshooting](../testing.md) before calling a live-printer
result complete.

For a quicker tour of the controls users actually see, open the
[annotated Device-tab view](device-tab-annotations.html). The annotations
explain the important routing constraints; this matrix remains the compact
cross-agent reference.

## Status definitions

- Supported - implemented, with a relevant live-printer result recorded.
- Partial - an important condition, limitation, or defect applies.
- Unsupported - no applicable implementation, or deliberate refusal.
- Not verified - implemented or source-inspected, but without a relevant live
  result.

`Base` means `MoonrakerPrinterAgent`. Qidi, Creality, and Snapmaker inherit
from it unless a row identifies an override.

## Connection and status

| Capability | Base | Qidi | Creality | Snapmaker |
| --- | --- | --- | --- | --- |
| Direct LAN connection with API key | Supported | Supported | Not verified | Not verified |
| WebSocket status updates | Supported | Supported | Not verified | Not verified |
| Reconnect and fresh status | Partial | Partial | Partial | Partial |
| Discovery and cloud binding | Unsupported | Unsupported | Unsupported | Unsupported |
| Device identity with a configured port | Partial | Partial | Partial | Partial |

- Reconnect completion can leave the Device tab with stale status.
- A bare IP and `host:port` can become separate device identities.
- The Base and Qidi Supported grades come from prior hardware sessions. They
  were carried into this rewrite and not rerun.

## Controls

| Capability | Base | Qidi | Creality | Snapmaker |
| --- | --- | --- | --- | --- |
| Home and arbitrary G-code | Not verified | Not verified | Not verified | Not verified |
| Bed and nozzle temperature | Not verified | Not verified | Not verified | Not verified |
| Pause, resume, and cancel | Not verified | Not verified | Not verified | Not verified |
| Configured chamber light | Partial | Partial | Partial | Partial |
| Jog and manual extrusion | Partial | Partial | Not verified | Not verified |
| Legacy part-fan speed control | Partial | Partial | Not verified | Not verified |
| Structured fan, chamber, and AI controls | Unsupported | Unsupported | Unsupported | Unsupported |
| AMS RFID, calibration, and tray control | Unsupported | Unsupported | Unsupported | Unsupported |

- Chamber light needs a recognised light object.
- Base and Qidi jog works, but can leave relative positioning active. Do not
  use it as a general safe-control test until its G-code state is restored.
- Base and Qidi part-fan control works through legacy `gcode_line` while
  `is_enable_np` is false. Adding `cfg`, `fun`, `aux`, and `stat` flips that
  flag and routes fan and extruder controls to unsupported structured commands.
- Creality and Snapmaker inherit the source path but have no separate live
  evidence for jog or fan control.

## Printing

| Capability | Base | Qidi | Creality | Snapmaker |
| --- | --- | --- | --- | --- |
| Upload G-code without starting | Not verified | Not verified | Not verified | Not verified |
| Upload and start a local print | Not verified | Not verified | Not verified | Not verified |
| Mapped multi-material print | Unsupported | Not verified | Unsupported | Unsupported |
| Cloud or SD-card start variants | Unsupported | Partial | Unsupported | Unsupported |
| Cancel during upload | Not verified | Not verified | Not verified | Not verified |
| Send with no nozzle identity | Not applicable | Not verified | Not applicable | Not applicable |

- Qidi applies mapping before it routes the real local print path.
- Some Qidi print variants can reach base success stubs after mapping.
- Qidi tolerates missing nozzle data in source, but that Send preflight is not
  hardware-verified.

## Filament

| Capability | Base | Qidi | Creality | Snapmaker |
| --- | --- | --- | --- | --- |
| Sync mode | Not verified | Not verified | Not verified | Not verified |
| Read installed material and slots | Partial | Not verified | Not verified | Not verified |
| Slot, material, and colour refresh | Unsupported | Not verified | Not verified | Not verified |
| Cleanup after removed material | Not verified | Not verified | Not verified | Not verified |
| Load, unload, or write a slot | Unsupported | Partial | Unsupported | Unsupported |
| Auto Refill | Unsupported | Unsupported | Unsupported | Unsupported |

- All built-in agents use pull-mode sync.
- Base reads Happy Hare or AFC data when present. Qidi has print-time mapping;
  Creality has CFS logic; Snapmaker reads printer arrays and NFC data.
- The Base Device-tab slot refresh uses a proprietary AMS command and has no
  generic Moonraker translation.
- Generic write-side macros stay unsupported until their printer contract is
  known and verified.

## Camera

| Capability | Base | Qidi | Creality | Snapmaker |
| --- | --- | --- | --- | --- |
| Discover a Moonraker webcam | Not verified | Not verified | Not verified | Unsupported |
| Provide a camera source | Not verified | Not verified | Not verified | Not verified |
| Live camera view | Not verified | Not verified | Not verified | Not verified |
| Snapshot-only camera start | Unsupported | Unsupported | Unsupported | Not verified |
| Camera start renewal and teardown | Unsupported | Unsupported | Unsupported | Partial |
| Print thumbnail | Not verified | Not verified | Not verified | Not verified |

- Snapmaker bypasses webcam discovery with a local snapshot-polling page.
- Its renewal and teardown path has lifetime risks without live evidence.
- Moonraker thumbnail endpoint responses and filename-cache behavior need live
  coverage.

## Python plug-ins

| Capability | Python plug-in agent |
| --- | --- |
| Registration and re-registration | Not verified - lifecycle tests cover replacement |
| Duplicate agent ID | Not verified - conflict is rejected and reported |
| Disable or unload | Not verified - deregistration is tested; session teardown needs coverage |
| Capability surface | Defined by the plug-in and exposed Python API |

## Reading the matrix safely

- `Partial` is not a softer form of `Supported`. It names a condition that must
  be checked before use. `Not verified` means the code was found, but no
  relevant live result is recorded.
- Pair each claim with the evidence grades in the testing guide. This matters
  especially for CFS, Snapmaker camera, MMU macros, Qidi Send preflight, and
  thumbnail endpoints.

## Background - deliberate exclusions

The matrix excludes Bambu-specific cloud binding, RFID, calibration, and
camera-control features from the Moonraker family. They use different protocol
contracts and are deliberately refused when no safe Klipper equivalent exists.
