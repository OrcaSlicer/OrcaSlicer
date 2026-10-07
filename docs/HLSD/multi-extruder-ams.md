# Multi-extruder and multi-AMS device control

`DeviceManager`/`MachineObject` model the toolheads, AMS units, and filament slots of a connected
printer and drive the device-control UI from that model. The model represents the counts as
device-reported runtime values rather than compile-time caps, so a printer with an arbitrary number
of toolheads, several AMS units, and a non-uniform number of slots per unit is handled by the same
code paths. The printer agent is responsible for translating its printer's state into the shared
JSON dialect this model consumes; the agent boundary is described in
[printer-agent.md](printer-agent.md), and this document describes the dialect and the model.

## Model layout

`MachineObject` owns the device subsystems that this document covers:

- `DevExtderSystem` — the toolheads (extruders) and their per-toolhead filament/temperature state.
- `DevFilaSystem` — the AMS units (`DevAms`) and their trays (`DevAmsTray`).
- `DevNozzleSystem` — the nozzles and, on rack printers, `DevNozzleRack`.
- `DevFilaSwitch` — a filament switch that can feed several AMS units to more than one extruder.

Each subsystem exposes its own count — `DevExtderSystem::GetTotalExtderCount()`,
`DevFilaSystem::GetAmsCount()`, `DevAms::GetSlotCount()` — and the rest of the application derives
its layout from those values rather than from fixed assumptions.

## Wire dialect

The dialect is the Bambu-shaped JSON that `DeviceManager` and `MachineObject` already consume. An
agent whose printer speaks something else is expected to translate into it; the agent is not free
to invent a new shape. Two representations coexist for extruders, and one for AMS/slots.

### Extruders

The generic representation is `print.device.extruder`, parsed by
`ExtderSystemParser::ParseV2_0` (`DevExtruderSystem.cpp`). It has a packed `state` integer and an
`info` array with one object per toolhead:

```text
device.extruder.state           packed integer
  bits  0..3   total extruder count
  bits  4..7   current extruder id
  bits  8..11  target extruder id
  bits 12..14  switch state
  bits 15..18  currently loading extruder id
  bit   19     busy for loading

device.extruder.info[]          one entry per toolhead
  id          extruder id
  filam_bak   array of backup-group bits
  info        bit 1 has filament, bit 2 buffer has filament, bit 3 nozzle present
  temp        bits 0..15 current temperature, bits 16..31 target temperature
  spre        slot_id bits 0..7, ams_id bits 8..15 (previous)
  snow        same packing (current)
  star        same packing (target)
  stat        bit 0..15 AMS status, bits 16..31 RFID status
  hnow        current nozzle id
```

The legacy single/dual representation is the pair of scalar fields `nozzle_temper` /
`nozzle_target_temper`, plus the `print.ams.tray_tar` / `tray_now` tray pointer, parsed by
`ExtderSystemParser::ParseV1_0`. That parser is a fallback and only fills the main extruder when the
reported toolhead count is one.

Physical extruder ids are fixed (`MAIN_EXTRUDER_ID = 0`, `DEPUTY_EXTRUDER_ID = 1`). Bambu printers
present the two toolheads as a left/right pair and invert the physical-to-logical mapping; generic
printers use the toolhead id itself as the logical id. `DevNozzle::GetLogicExtruderId()` /
`GetExtruderId()` centralise that distinction.

### AMS units and slots

AMS units arrive as `print.ams.ams[]` and are parsed by `DevFilaSystemParser::ParseV1_0`. Each unit
carries an `id` and an `info` bitfield:

```text
info bits  0..3    type: 0 dummy, 1 AMS, 2 AMS-Lite, 3 N3F, 4 N3S, 5 AMS-Lite-mixed (N9)
info bits  8..11   bound extruder id (0xE marks a switch-bound unit)
info bits 24..27   switch inlet when the unit is switch-bound
```

`DevAmsType` (`DevDefs.h`) enumerates the resulting unit kinds: `EXT_SPOOL`, `AMS`, `AMS_LITE`,
`N3F`, `N3S`, and the `AMS_LITE_MIXED` N9 variant. `AMS_LITE_MIXED` reports itself as `AMS_LITE` to
generic callers; code that needs the distinction uses `IsAmsLiteMixed()`.

A unit whose bound-extruder nibble is `0xE` is switch-bound: it is kept only when a `DevFilaSwitch`
is installed, in which case its extruder id is pinned to the main extruder and its binding set
becomes `{0, 1}`; otherwise the unit is dropped from the AMS list.

Each unit owns a map of `DevAmsTray` keyed by the tray id from the payload. The slot count is a
property of the unit kind and the reported trays:

- `AMS`, `AMS_LITE`, and `N3F` report four slots on Bambu printers, matching the firmware contract;
- `N3S` reports one slot;
- `EXT_SPOOL` and any non-Bambu unit report the number of trays actually present.

The tray ids are the payload's slot indices; they are expected to be unique, 0-based, and contiguous
within a unit, because the generic tray-index mapping below is cumulative.

Bulk/extended spools are carried by `print.vir_slot`, which `DeviceManager` parses into
`MachineObject::vt_slot`. Two ids are fixed: `VIRTUAL_TRAY_MAIN_ID = 255` (extruder 0) and
`VIRTUAL_TRAY_DEPUTY_ID = 254` (extruder 1); the id convention is `vt_id = 255 - extruder_id`, so
further extruders use 253, 252, … . The `vir_slot` parser recognises the 255/254 pair; the agent
filament descriptor path `DevFilaSystemParser::ParseAgentFilament` is the entry that stores a fuller
descending set in `vt_slot`.

## Mapping

Several subsystems relate AMS units, trays, and extruders, and more than one place derives a tray
index. The Bambu mapping path (`DevMappingUtil`) computes `ams_id * 4 + slot_id` and knows only the
Bambu unit kinds; `DevFilaSystem` provides the device model's own mapping below. The two agree for
Bambu units and diverge for a non-Bambu unit whose slot count is not four.

### AMS id to extruder id

`DevFilaSystem::GetExtruderIdByAmsId()` resolves an AMS id to the extruder it feeds:

- an AMS unit returns its bound extruder (`DevAms::GetExtruderId()`);
- a virtual tray returns `VIRTUAL_TRAY_MAIN_ID - vt_id`;
- Bambu's virtual AMS ids map to the main/deputy extruder.

A unit may be bound to more than one extruder when a `DevFilaSwitch` feeds it; the binding set
(`DevAms::m_binded_extruder_set`) and switch inlet are read by switch-aware code while the
single-extruder id stays pinned for legacy callers.

### Tray index

`DevFilaSystem::GetTrayIndexMap()` maps a global tray index to an `(ams_id, slot_id)` pair and is
used for backup-slot translation, calibration addressing, and the mapping popups.

- Bambu AMS: `ams_id * 4 + slot_id`; N3S: the AMS id; AMS-Lite-mixed (N9): `24 + slot_id`.
- Generic (non-Bambu): a running lane index — the count of trays in preceding units plus the slot
  id. This depends on each unit's tray ids being 0-based and contiguous.

### Virtual-slot recognition

`devPrinterUtil::IsVirtualSlot()` decides whether an id denotes a virtual tray. On Bambu printers it
is the fixed pair `{255, 254}`. On generic printers it is the descending range
`[255 - count + 1, 255]`, where `count` is the selected printer preset's extruder count. The preset
is expected to match the connected device; when the two disagree the virtual range is wrong, which
is why a mismatch is reported rather than silently accepted.

## Device-control UI

The device-control widgets follow the same device-reported counts.

- `AMSModel` (`AMSItem.hpp`) mirrors `DevAmsType` numerically: `EXT_AMS`, `GENERIC_AMS`, `AMS_LITE`,
  `N3F_AMS`, `N3S_AMS`. A plain `AMS` maps to `GENERIC_AMS`.
- On non-Bambu printers `use_generic_ams_layout()` routes `GENERIC_AMS` units through a variable-lane
  rendering that draws one lane per reported tray; Bambu keeps its fixed four-slot rendering.
- `AMSControl` picks its layout by vendor: Bambu keeps the left/right presentation
  (`CreateAmsSingleNozzle` for one toolhead, `CreateAmsDoubleNozzle` for two), while non-Bambu
  printers use `CreateAmsMultiNozzle`, which builds one preview and one AMS simplebook per toolhead
  for any count.
- The status panel's `ExtruderImage` renders one per-toolhead state per nozzle and is resized to
  `GetTotalExtderCount()`. Non-Bambu printers choose the active toolhead through
  `m_generic_nozzle_selector` (shown when more than one toolhead exists); Bambu keeps its left/right
  `m_nozzle_btn_panel`. Nozzle temperature controls are created on demand per toolhead.

## Constraints

- **Single dialect.** The agent translates its protocol into this JSON; the model does not learn
  vendor protocols. Extending the model with a new unit kind or field means extending the dialect.
- **Agent-produced counts.** Toolhead, AMS, and slot counts come from the device payload. Nothing
  caps them at two toolheads or four slots except the Bambu contract described above.
- **Extruder dialect invariants.** `device.extruder.state`'s count field must equal the length of
  `info[]`, and `info[]` must be ordered by extruder id with `id` equal to the array index: the
  parser places toolheads by array position while `GetExtderById()` indexes by id.
- **Preset/device agreement.** The generic layout and virtual-slot range derive their count from the
  selected printer preset. A printer preset must therefore declare the same number of
  `nozzle_diameter` entries as the device has toolheads; a mismatch is surfaced because the sidebar
  and extruder logic would otherwise disagree.
- **Bambu compatibility.** Bambu-specific presentation (left/right pair, fixed 4-slot units, fixed
  virtual ids, physical/logical inversion) is preserved behind the vendor check. Generic paths do
  not alter it.
- **Tray-index invariant.** The generic tray-index map is cumulative, so payload tray ids must be
  unique, 0-based, and contiguous per unit. A malformed id is a producer error.
- **Protocol markers.** The dialect has no neutral equivalent for some Bambu state, so agents that
  lack it send empty `print.cfg`, `print.fun`, `print.aux`, and `print.stat`. Their presence selects
  the new-protocol parse path; they are compatibility markers, not feature switches.

## Main implementation locations

- [`DevExtruderSystem`](../../src/slic3r/GUI/DeviceCore/DevExtruderSystem.h) — toolhead model and the
  `device.extruder` parser
- [`DevFilaSystem`](../../src/slic3r/GUI/DeviceCore/DevFilaSystem.h) — AMS/tray model, the AMS parser,
  and the mapping helpers
- [`DevDefs.h`](../../src/slic3r/GUI/DeviceCore/DevDefs.h) — `DevAmsType`, extruder ids, and virtual
  tray ids
- [`DeviceManager.cpp`](../../src/slic3r/GUI/DeviceManager.cpp) — message dispatch, `vir_slot`
  parsing, and device state assembly
- [`AMSControl.cpp`](../../src/slic3r/GUI/Widgets/AMSControl.cpp) — per-toolhead AMS panes
  (`CreateAmsMultiNozzle`) and the generic layout
- [`AMSItem.hpp`](../../src/slic3r/GUI/Widgets/AMSItem.hpp) — `AMSModel` and the variable-lane AMS
  rendering
- [`StatusPanel.cpp`](../../src/slic3r/GUI/StatusPanel.cpp) — `ExtruderImage`, the toolhead selector,
  and per-toolhead temperature controls
- [`MoonrakerPrinterAgent.cpp`](../../src/slic3r/Utils/MoonrakerPrinterAgent.cpp) — producer example
  (AMS payload and the `device.extruder` encoding)
