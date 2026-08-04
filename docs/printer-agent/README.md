# Printer agents

Printer agents let OrcaSlicer communicate with printers through a
standardized protocol. They translate between a printer's
native API and the application interfaces that the app already
uses.

This documentation explains the compatibility boundary, runtime ownership,
connection and status flow, command and feature behavior, built-in and plugin
agent implementations, and the testing evidence required for compatibility
claims.

## What printer agents do

A printer agent has two jobs:

1. Accept the app's existing commands and translate the ones its
   printer supports.
2. Convert native printer status into correctly-shaped state that
   `MachineObject` understands.

Currently, agents work at a compatibility boundary, i.e., making other vendors compatible with Bambu-shaped code, not a vendor-neutral one.
Some Bambu concepts remain part of the payload and command vocabulary.
End goal is to make the whole command and payload interfaces vendor-neutral.

## Vocabulary

Every chapter reuses these terms. The "Is not" column is the part that
causes confusion when it is left implicit.

| Term | Is | Selected by | Is not |
| --- | --- | --- | --- |
| Agent ID | Which printer agent implementation to use | `printer_agent` on the printer preset; empty is the legacy `bbl`-or-`orca` sentinel | Which printer |
| Printer agent | The live `IPrinterAgent` instance for that ID, created and cached once per ID by `NetworkAgentFactory` | Factory lookup on the agent ID | A connection, and not one object per printer |
| Device ID | One printer inside that implementation | Bind with Access Code for the Moonraker family, where the entered address becomes the ID; Bambu uses its own discovery identity | Which protocol |
| `MachineObject` | The Device tab's view of one selected printer | `DeviceManager::selected_machine`, which stores only an ID | Proof that a printer is reachable |
| Freshness | `is_connected()`, a test over the last-update time | Any reset of the update time, including one no status has followed | Proof that status arrived |
| Status-confirmed readiness | A push-status message has actually been parsed | The first real status message | The same thing as a successful `connect_printer()` |

Earlier drafts used "transport" for the printer agent instance. That term
is retired: the code selects an implementation, not a wire protocol.

## How to use this guide

- [Architecture](architecture.md) describes objects, ownership, lifetimes,
  error handling, the feature gate, and compatibility contracts for printer
  agents.
- [Connection and status](connection-and-status.md) describes how presets,
  machines, access codes, status messages, and commands fit together at
  runtime. Unlike Architecture, it follows the sequence of selecting an
  agent, connecting, receiving status, and sending commands.
- [Printing](printing.md), [filament synchronization](filament.md), and
  [camera support](camera.md) are separate chapters because they contain
  per-feature detail rather than because they are universally special:
  Printing has its send, preflight, recovery, and start contracts; Filament
  covers acquisition, mapping selection, and print-time delivery; Camera
  covers the distinct Bambu, Moonraker, and Snapmaker ownership models.
- [Built-in agents](agents.md) describes the Moonraker family and the Qidi,
  Snapmaker variants.
- [Python plugin agents](plugin-agents.md) describes the plugin bridge and
  lifecycle.
- [Testing and troubleshooting](testing.md) explains automated checks, manual
  hardware work, known defects, and the evidence required for compatibility
  claims.
- The [capability matrix](reference/capability-matrix.md) is the compact
  feature reference. The [manual checklist](reference/manual-checklist.html)
  is for a live-printer verification pass.

Treat source code as authoritative when it differs from this guide. In
particular, preserve the compatibility rules called out in each chapter:
they protect stored presets, existing profiles, and the Device tab's
assumptions.
