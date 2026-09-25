# Printer agents

Printer agents isolate printer-specific communication from the rest of OrcaSlicer. The GUI and
`DeviceManager` operate on a shared set of printer operations and device state; a selected printer
agent implements those operations for a particular printer ecosystem. The agent boundary allows
Bambu, Moonraker-based printers, built-in integrations, and Python-provided integrations to use the
same application workflow without making the GUI understand every printer protocol.

The current boundary is an adapter boundary around the existing application contract. In particular,
some request fields and message payloads still use the Bambu-shaped representation that existing
`MachineObject` and `DeviceManager` code consumes. The printer agent is responsible for translating
that representation into the protocol spoken by its printer. This is an intentional compatibility
constraint of the current design; the interface is not yet a neutral printer protocol.

The v1 dialect migration path is deliberately narrow. `DeviceManager` currently speaks the Bambu JSON
dialect because that is the payload shape already used throughout the command and state workflow. The
v1 `OrcaPrinterAgent` also accepts that Bambu dialect. Its transport path places the small translation
needed for the target printer at `deliver_to_sink`, keeping the compatibility code at the edge rather
than spreading it through `DeviceManager` or the agent interface.

The eventual direction is for `DeviceManager` to produce an Orca JSON dialect. The Bambu agent will then
own the translation from Orca JSON to Bambu's protocol, while `OrcaPrinterAgent` can forward the Orca
payload directly to its sink. The v1 translation at `deliver_to_sink` can then be removed without
changing `DeviceManager`, the command callers, or the rest of the agent workflow.

## Components

The system has four relevant layers:

```text
GUI / DeviceManager / MachineObject
              |
        NetworkAgent
          /       \
 IPrinterAgent   ICloudServiceAgent
      |                 |
 printer protocol   authentication and cloud services
```

### `DeviceManager` and `MachineObject`

`DeviceManager` owns the application-facing printer workflow. It maintains `MachineObject` instances,
updates their state, filters devices for the active printer agent, and initiates operations such as
homing, temperature changes, printing, subscriptions, and camera playback.

`MachineObject` remains the shared state model used by the GUI. It does not contain the implementation
of a printer protocol. When a device is discovered or returned by a cloud query, the device is tagged
with the active `printer_agent_id`. Device lists and selected-machine operations use that tag to avoid
sending an operation through an agent that does not own the device.

### `NetworkAgent`

`NetworkAgent` is the façade used by the GUI and `DeviceManager`. It owns:

- the currently selected `IPrinterAgent`;
- the registered cloud-service instances, indexed by provider;
- callbacks shared by the active printer agent and the application;
- the forwarding methods for printer commands and cloud operations.

There is one active printer agent for the currently selected printer preset. Switching the preset
increments the machine-list generation, disconnects the old printer agent, removes its callbacks, and
installs the newly selected agent. The façade then forwards printer operations to that agent.

Cloud operations are selected separately using a provider key. `NetworkAgent` forwards a cloud request
to the matching `ICloudServiceAgent`, and forwards cloud camera operations with a device ID. The
printer agent receives a cloud-agent pointer through `set_cloud_agent()` when it is created, allowing
printer communication to obtain cloud tokens without depending on a concrete cloud implementation.

### `IPrinterAgent`

`IPrinterAgent` is the printer-facing contract. It covers:

- cloud-relay and direct-LAN message delivery;
- LAN connection, discovery, binding, and certificates;
- printer subscriptions and callbacks;
- print operations;
- filament synchronization;
- camera capability and local camera URL reporting;
- printer command methods.

Concrete built-in implementations include the Bambu wrapper, the native Orca/Moonraker path, and
other printer-agent implementations registered by the application. A printer agent may use either
the cloud agent, a direct LAN connection, or both.

### `ICloudServiceAgent`

`ICloudServiceAgent` owns authentication and services provided by a cloud backend. It covers login
state, tokens, user and printer lists, settings synchronization, model services, cloud messages, and
cloud camera operations.

Cloud camera operations are device-scoped:

- `get_camera_url(dev_id, callback)` obtains a stream URL for one device;
- `create_camera_signaling_channel(dev_id)` creates signaling for one device where the provider
  supports it.

This is separate from the local camera URL exposed by `IPrinterAgent`, which is currently scoped to
the active printer agent because a normal LAN agent represents one physical printer connection.

## Agent registration and selection

`NetworkAgentFactory` maintains the printer-agent registry. Each registry entry contains an agent ID,
a display name, and a factory function. Built-in agents register during application initialization.
Python printer-agent capabilities register dynamically and contribute an agent ID and factory entry.

The selected printer preset contains the printer-agent choice. If no explicit choice is stored, the
application preserves the existing default behavior: Bambu presets select the Bambu agent and other
presets select the native Orca agent. When a preset is changed, `GUI_App` resolves the effective agent
ID, obtains the corresponding cloud agent, creates the printer agent through the registry, and installs
it in `NetworkAgent`.

The registry rejects conflicting agent IDs. This matters for Python plugins because an agent ID is the
stable identity used by presets and device ownership; two enabled plugin capabilities must not claim
the same ID.

## Message and command flow

There are two low-level message paths:

- `send_message()` publishes a command through the printer's cloud relay;
- `send_message_to_printer()` sends a command directly to the printer over the LAN path.

Both paths accept a JSON string, quality-of-service and flag values, and return the existing network
status code domain. The agent owns the conversion from that JSON contract to its native transport.

The typed `command_*` methods are the application-facing convenience layer. The five generic defaults
currently implemented by `IPrinterAgent` construct the existing JSON dialect and route through the
same message path:

| Method | Default operation |
| --- | --- |
| `command_xyz_abs()` | Send `G90` for absolute positioning |
| `command_auto_leveling()` | Send `G29` for bed leveling |
| `command_go_home()` | Use the supported homing operation or send `G28` |
| `command_set_bed()` | Use the supported bed control or send `M140` |
| `command_set_nozzle()` | Send `M104` for nozzle temperature |

These are compatibility defaults for common printer workflows, not a guarantee that every firmware
implements every command identically. An agent can override a method when its protocol needs another
operation. For example, a Klipper configuration may use `BED_MESH_CALIBRATE` instead of `G29`.

The remaining common command methods default to `ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED` because their
existing behavior is vendor-specific or has no portable implementation:

- AMS RFID refresh;
- AMS calibration;
- AMS tray selection;
- camera start;
- axis control.

The methods remain on the common interface so an agent that supports them can override them explicitly.
`sequence_id` remains part of the command contract because `DeviceManager` creates and tracks it as
the command ID.

## Filament and AMS synchronization

The `OrcaPrinterAgent` bridges OrcaSonar's filament/AMS state. Its capability reply
carries two independent signals: `protocol.features.fms` means a material system resolves,
and `protocol.features.filament_slots` means the connector owns a filament-slot model that
persists without any material hardware. Either one enables filament synchronization, so a
printer with slots but no AMS still exposes the slot workflow. The agent also caches
`protocol.ams_ops`, the canonical material write operations the resolved drivers implement,
and uses it to gate AMS writes; `print.ams_filament_setting` is exempt because it persists
connector state and is advertised by `filament_slots` alone. A write whose operation is not
declared is refused with `CAP_NOT_AVAILABLE` before it reaches the wire.

Printer state arrives as the pushed `ams`/`vir_slot` projection inside `push_status`; the
Orca agent does not poll the Moonraker `lane_data` namespace, which exists for
Moonraker-channel consumers. Slot presence is the user's declaration, not sensed material:
`tray_exist_bits` and the presence of a `vir_slot` entry mark a slot as present even with an
empty `tray_type`, which is the `is_exists` state the filament UI reads. A full status frame
(`msg=0`, or a LAN frame with no `msg`) updates each virtual-tray ID it contains but does not
remove IDs already observed during the current connection. An observed ID remains if omitted
from a populated `vir_slot`, if `vir_slot` is empty, or if the field is absent. Before any
virtual tray has been observed, a full frame with no virtual-tray entries clears the initial
placeholder. `MachineObject::reset()` clears retained IDs on reconnect. Delta frames update
named entries only. This retention rule is specific to the native Orca agent.

Writes follow the same edge-translation rule as the rest of the agent. The shared
`MachineObject` command builders emit Bambu-shaped `print.ams_*` payloads, and the agent's
single send funnel decodes them into OrcaSonar's canonical bodies — `selector` with a flat
lane or `ams_id`/`slot_id` coordinates for `ams_change_filament`, coordinates for
`ams_filament_setting` — before publishing. The server resolves and rejects with its own
`errno` backstop; the agent only withholds operations the device did not declare.

A filament frame can arrive before the capability reply that decides synchronization mode,
for example when the topology bootstraps after connect or Klipper restarts. While a device's
capabilities are still unknown, the agent re-requests `get_capabilities` and a full status on
a filament frame, throttled per device, and stops once an answer arrives. Dedicated
capability state is cleared when a cloud device is deselected, while an independently active
LAN session for the same device id keeps its declaration.

## Device ownership and stale responses

Printer-agent ownership is represented by `printer_agent_id` on device records and `MachineObject`
instances. The active agent ID is attached when a device is discovered, returned by a cloud list, or
reused after a preset switch. Local-machine configuration also persists the agent ID so a saved LAN
device is not silently reused by an unrelated agent.

Cloud printer-list responses carry three pieces of request context added by `NetworkAgent`:

```text
provider   cloud provider used for the request
agent_id   active printer agent when the request was made
generation machine-list generation when the request was made
```

`DeviceManager` accepts the response only when those values still match the current provider, active
agent, and generation. This prevents a slow response from the previous preset or provider from
repopulating the current device list.

The provider mapping is currently selected by `GUI_App`: the Bambu agent maps to the Bambu cloud
provider and other agents map to the Orca cloud provider. The generation check protects that existing
selection from races; it does not make cloud-provider ownership intrinsic to an agent. Cloud-printer
ownership and the broader Orca cloud services are therefore still separate architectural concerns.

## Python printer agents

`PrinterAgentPluginCapability` implements `IPrinterAgent` directly. The live capability object is
registered with `NetworkAgentFactory` and handed out as the printer agent when its agent ID is selected.
The plugin receives the selected `ICloudServiceAgent` through `set_cloud_agent()` just like a built-in
printer agent.

Python plugins must implement the core communication and lifecycle methods required by the interface,
including agent metadata, printer connection, discovery callbacks, and the two message-send methods.
Methods that are meaningful only to a particular printer are optional overrides where the C++ base
class provides a default.

All ten `command_*` methods are available in the Python binding and in the trampoline. Their override
status is intentionally optional:

- the five generic commands use the C++ default when Python does not override them;
- the five vendor-specific commands return `NOT_SUPPORTED` unless Python supplies an implementation;
- a Python implementation can replace either behavior for its own protocol.

The Python camera binding exposes HTTP, HTTPS, RTSP, and HTTP-snapshot modes. WebRTC remains a
built-in C++ camera mode, but is not exposed as a Python mode because the current Python capability
does not provide the corresponding cloud signaling-channel contract.

## Camera playback boundary

The camera stream mode describes how a stream is obtained; it does not by itself define ownership of
the wxWidgets view that renders it. `MediaPlayCtrl` selects and tears down the active backend, while
the wx parent owns the child window or renderer. This is important because a web view, native media
control, and frame-based/WebRTC renderer have different wx window-lifetime requirements.

Cloud URL and signaling requests are routed through `NetworkAgent` to the cloud provider selected for
the device. Local URL requests are routed to the active printer agent. The distinction keeps cloud
account services device-scoped while preserving the current one-LAN-agent/one-printer model.

## Compatibility constraints

The printer-agent boundary intentionally preserves several existing application contracts:

- Bambu-shaped JSON is still the shared command representation;
- existing network status codes are reused, with Orca-specific unsupported/capability errors added
  in the Orca-reserved range;
- `MachineObject` remains the shared device-state model;
- preset and local-machine data retain compatibility with the existing agent-selection behavior;
- Python plugins use the existing capability and pybind11 registration system.

The agent abstraction is therefore responsible for containing vendor differences, not for pretending
that all vendor protocols are identical. The planned Orca JSON dialect is the protocol-neutral command
model for the `DeviceManager`/agent boundary. Once it is introduced, Bambu-specific translation remains
inside the Bambu agent and the Orca agent's v1 sink adapter can be removed as a self-contained cleanup.

## Main implementation locations

- [`IPrinterAgent`](../../src/slic3r/Utils/IPrinterAgent.hpp) — printer-agent contract and generic command defaults
- [`ICloudServiceAgent`](../../src/slic3r/Utils/ICloudServiceAgent.hpp) — cloud service and per-device
  cloud camera contract
- [`NetworkAgent`](../../src/slic3r/Utils/NetworkAgent.hpp) — façade and dispatch between active agents
- [`NetworkAgentFactory`](../../src/slic3r/Utils/NetworkAgentFactory.hpp) — built-in and Python agent registry
- [`OrcaPrinterAgent`](../../src/slic3r/Utils/OrcaPrinterAgent.hpp) — OrcaSonar transport, capability
  discovery, and Bambu-to-canonical command translation
- [`AmsPayload`](../../src/slic3r/Utils/AmsPayload.hpp) — shared filament payload rendering and the
  per-device AMS capability cache
- [`DeviceManager`](../../src/slic3r/GUI/DeviceCore/DevManager.cpp) — device ownership, filtering, and
  stale-response checks
- [`PrinterAgentPluginCapability`](../../src/slic3r/plugin/pluginTypes/printerAgent/PrinterAgentPluginCapability.cpp)
  — Python bindings
- [`MediaPlayCtrl`](../../src/slic3r/GUI/MediaPlayCtrl.cpp) — camera backend selection and playback lifecycle
