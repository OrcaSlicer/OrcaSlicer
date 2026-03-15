# Plate changer support

This document describes OrcaSlicer’s plate changer (multi-plate / plate swap) behaviour: how it works for users, its limitations, and where it is implemented.

## User experience

### Enabling plate change G-code

Plate changer behaviour is gated by the **Plate change G-code** setting:

- **Location:** Printer Settings → Machine G-code → **Plate change G-code**
- If this field is empty, “Print all (plate changer)”, “Send all (plate changer)”, and “Export all (plate changer)” are not offered, and the start/end plate toggles are hidden.
- When set, the same G-code block is inserted between plates and optionally at the start and end of the job (see below).

### Sending or exporting all plates (plate changer)

When the current printer has **Plate change G-code** configured, the following actions merge all sliced plates into a single job with plate-change G-code between them:

- **Print all (plate changer)** – print all plates as one job (via the current print host).
- **Send all (plate changer)** – send all plates as one job to the printer (LAN Send or Cloud Select Machine).
- **Export all (plate changer)** – export a single 3MF that contains merged G-code for all plates.

In all cases:

1. Each plate’s G-code is written as usual, then concatenated in plate order.
2. The **Plate change G-code** string is inserted between consecutive plates (after plate N, before plate N+1).
3. Optionally, the same G-code can be inserted **before the first plate** and/or **after the last plate** using the “Start with new plate?” and “End with new plate?” toggles in the Send/Print dialogs.

### Start / End with new plate toggles

In both the **Send to Printer** (LAN) and **Select Machine** (cloud) flows:

- When you choose **Send all (plate changer)** (or equivalent), the dialog shows an extra section below the main options:
  - A horizontal separator.
  - **Start with new plate?** (default off) – insert the plate change G-code once before the first plate’s G-code. Tooltip: *When enabled, the plate change G-code runs before the first plate so the printer ejects the current plate and loads a fresh one before the print starts.*
  - **End with new plate?** (default off) – insert the plate change G-code once after the last plate’s G-code. Tooltip: *When enabled, the plate change G-code runs after the last plate so the printer ejects the final plate when the job finishes.*

These toggles are **only visible** when:

- The job is “all plates” with plate changer (e.g. you opened the dialog via “Send all (plate changer)”), and  
- The **current printer preset** has a non-empty **Plate change G-code**.

Example with two plates and both toggles on:

`[plate_change] → [plate 1 gcode] → [plate_change] → [plate 2 gcode] → [plate_change]`

---

## Limitations

### Single G-code for start, between plates, and end

- **Only one** G-code string is used for every insertion: the **Plate change G-code** from the printer preset.
- The **same** block is used for:
  1. **Start** (when “Start with new plate?” is on),
  2. **Between plates** (always, when merging multiple plates),
  3. **End** (when “End with new plate?” is on).

Some setups (e.g. swaplist.app or custom workflows) use a different “initialisation” or “final” sequence at the very start or end of the job. That is **not** supported: there is no separate “start plate change” or “end plate change” field. If you need different G-code at start/middle/end, you would need to handle that outside OrcaSlicer (e.g. by post-processing the merged G-code).

### Visibility of start/end toggles

- The “Start with new plate?” and “End with new plate?” section is shown only when the **current printer preset** has non-empty **Plate change G-code**.
- It does not depend on the **selected machine** in the dialog (e.g. in Select Machine, the edited printer preset is used for this check).

---

## Implementation details

### Where merging happens

- Merged G-code for “all plates (plate changer)” is built in **`src/libslic3r/Format/bbs_3mf.cpp`**:
  - **`build_plate_changer_merged_gcode()`** – static helper that:
    - Takes the list of plate data, the print config, and two booleans: `start_with_new_plate`, `end_with_new_plate`.
    - Reads **`plate_change_gcode`** from the config.
    - Concatenates each plate’s G-code file and inserts `plate_change_gcode` between plates; if `start_with_new_plate` is true, prepends it once before the first plate; if `end_with_new_plate` is true, appends it once after the last plate.
  - The result is written as a single logical plate (e.g. `Metadata/plate_1.gcode`) so the printer sees one job.

### Export pipeline (flags)

- **`StoreParams`** (`src/libslic3r/Format/bbs_3mf.hpp`):
  - **`use_plate_changer_all`** – merge all plates with plate change G-code between them.
  - **`start_with_new_plate`** – prepend plate change G-code before the first plate.
  - **`end_with_new_plate`** – append plate change G-code after the last plate.

- These are set in **Plater::export_3mf()** (`src/slic3r/GUI/Plater.cpp`) when building `StoreParams`, and are passed through to the 3MF exporter so that **`_BBS_3MF_Exporter`** can call **`build_plate_changer_merged_gcode()`** with the correct flags.

### Plater / send APIs

- **`Plater::export_3mf(..., use_plate_changer_all, start_with_new_plate, end_with_new_plate)`** – full export; forwards the three flags into `StoreParams`.
- **`Plater::export_config_3mf(..., use_plate_changer_all, start_with_new_plate, end_with_new_plate)`** – export of the “config” 3MF used when sending; calls **`export_3mf()`** with the same flags.
- **`Plater::send_gcode(..., use_plate_changer_all, start_with_new_plate, end_with_new_plate)`** – builds the 3MF for send (via **`export_3mf()`**) then proceeds with upload; used by both LAN and cloud send flows.

### GUI: where the toggles live

- **Send to Printer (LAN):** **`src/slic3r/GUI/SendToPrinter.cpp`** / **`.hpp`**
  - **prepare(print_plate_idx, use_plate_changer_all)** – shows or hides the separator and the “Start with new plate?” / “End with new plate?” checkboxes based on `use_plate_changer_all` and whether the current printer preset has **Plate change G-code**.
  - When sending, the checkbox states are read and passed into **`send_gcode()`** and **`export_config_3mf()`**.

- **Select Machine (cloud):** **`src/slic3r/GUI/SelectMachine.cpp`** / **`.hpp`**
  - Same pattern: **prepare(print_plate_idx, use_plate_changer_all)** controls visibility of the plate-changer option section.
  - On Print, the checkbox states are passed into **`send_gcode()`** and **`export_config_3mf()`**.

### Config key

- **`plate_change_gcode`** – printer preset option (e.g. in **PrintConfig** / **PrinterSettings**). Used to decide whether plate-changer actions and start/end toggles are available, and as the exact block inserted at each position (start, between plates, end) when building merged G-code.

---

## Summary

| Item | Description |
|------|-------------|
| **Config** | Printer Settings → Machine G-code → **Plate change G-code** |
| **Merge helper** | **`build_plate_changer_merged_gcode()`** in **`bbs_3mf.cpp`** |
| **StoreParams** | **`use_plate_changer_all`**, **`start_with_new_plate`**, **`end_with_new_plate`** |
| **Send/Export** | **Plater::export_3mf**, **export_config_3mf**, **send_gcode** carry the flags |
| **Dialogs** | **SendToPrinter**, **SelectMachine**: separator + two checkboxes, shown only when plate changer is used and preset has **plate_change_gcode** |
| **Limitation** | One G-code string for start, between plates, and end (no separate start/end sequences) |
