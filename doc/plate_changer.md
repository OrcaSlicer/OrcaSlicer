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

### Plate details table (Include / Plate / Time / Weight)

For **Print all (plate changer)**, **Send all (plate changer)**, and **Export all (plate changer)** (when the export options dialog is shown), the dialog includes a small grid:

| Column | Meaning |
|--------|---------|
| **Include** | Checkbox: whether this physical plate is part of the merged job. |
| **Plate** | Plate number (1-based). |
| **Time** / **Weight** | Estimates from slicing for that plate. |

**Behaviour:**

- **All** row: **Time** and **Weight** are totals **only for included** plates.
- **Unchecked** plates are shown with **dimmed text**; they are **not** concatenated into merged G-code and are **not** counted in those totals.
- You **cannot** uncheck every plate: the last remaining checkbox stays on if you try.
- The header shows **“Printing *n* of *m* plates”** (or **“Exporting …”** on the export options dialog), where *n* is the included count.

**Semantics for the job:**

- **First included plate** (lowest plate index that remains checked) is treated like “plate 1” for **cloud AMS JSON**, thumbnails, and other metadata that must match a single reference plate—same idea as before, but relative to inclusion, not always physical plate 1.
- **Filament / AMS UI** in **Select Machine** is rebuilt from **included** plates only when you toggle a row (so mapping matches what will be sent).

---

## Limitations

### Single vs. additional initial G-code

- **Between plates** (and at the end): OrcaSlicer always uses the printer preset’s **Plate change G-code**.
- **Start of job**: when “Start with new plate?” is enabled, OrcaSlicer first runs the optional **Additional initial plate change G-code** (if non-empty) and then runs the regular **Plate change G-code**.

This matches setups like swapmod/swaplist.app where a small, **start-only wrapper** is used to lift Z and safely load a plate when the current Z height is unknown, while the main swap routine is reused for both mid-queue and end-of-job swaps.

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
    - **Multiple sliced plates:** concatenates each plate’s G-code and inserts `plate_change_gcode` between plates; optional prepend/append when start/end toggles are on.
    - **Single plate:** if either toggle is on, builds one file: optional prepend + that plate’s G-code + optional append (normal print with swap bookends).
  - The merged result is written as a single logical plate (e.g. `Metadata/plate_1.gcode`) when merging applies.

### Export pipeline (flags)

- **`StoreParams`** (`src/libslic3r/Format/bbs_3mf.hpp`):
  - **`use_plate_changer_all`** – merge all plates with plate change G-code between them.
  - **`start_with_new_plate`** – prepend plate change G-code before the first plate.
  - **`end_with_new_plate`** – append plate change G-code after the last plate.

- These are set in **Plater::export_3mf()** (`src/slic3r/GUI/Plater.cpp`) when building `StoreParams`, and are passed through to the 3MF exporter so that **`_BBS_3MF_Exporter`** can call **`build_plate_changer_merged_gcode()`** with the correct flags.

### Per-plate inclusion (export / send mask)

- **Dialogs** keep a **`std::vector<bool> plate_included`** with one entry per physical plate (`true` = in job). Defaults are all `true`.
- **`Plater::export_3mf(..., plate_changer_plate_included)`** (optional `const std::vector<bool>*`, same length as plate count): when **`use_plate_changer_all`** is set and the pointer is non-null, **`filter_export_payload_for_plate_changer_selection()`** (in **`Plater.cpp`**) removes excluded **`PlateData`** rows and parallel thumbnail / bbox vectors, then **renumbers** **`plate_index`** to `0 … k-1` so merged G-code and **`Metadata/plate_1.*`** stay consistent.
- **`send_gcode`** / **`export_config_3mf`** take the same optional pointer; temp 3MF paths use the **first included** plate when **`export_plate_idx == PLATE_ALL_IDX`** (**`partplate_for_temp_3mf_paths()`**).
- **`export_gcode_3mf`**: after **PlateChangerExportOptionsDialog**, the returned mask is passed into **`export_3mf`**; suggested “`*_N_plates*`” filename uses the **included** count.

### Plate stats table widget

- **`populate_plate_changer_time_weight_grid()`** – **`src/slic3r/GUI/PlateChangerPlateStatsTable.{hpp,cpp}`**  
  Four-column flex grid; optional callback runs after any **Include** checkbox change (e.g. refresh AMS list and totals).

### Plater / send APIs

- **`Plater::export_3mf(..., use_plate_changer_all, start_with_new_plate, end_with_new_plate, plate_changer_plate_included)`** – full export; forwards flags and optional inclusion mask.
- **`Plater::export_config_3mf(..., …, plate_changer_plate_included)`** – config 3MF for send; same optional mask.
- **`Plater::send_gcode(..., …, plate_changer_plate_included)`** – build send 3MF via **`export_3mf()`**; same optional mask.
- **`Plater::export_gcode_3mf(..., plate_changer_plate_included)`** – optional mask for callers that skip the export options dialog.

### GUI: where the toggles live

- **Send to Printer (LAN):** **`src/slic3r/GUI/SendToPrinter.cpp`** / **`.hpp`**
  - **prepare(...)** – shows the start/end toggles whenever the printer preset has non-empty **Plate change G-code** (single-plate print or print-all).
  - **Print-all (plate changer):** plate table + **`m_plate_changer_plate_included`**; **`plate_changer_included_mask_for_export()`** passed to **`send_gcode()`** / **`export_config_3mf()`**.

- **Select Machine (cloud):** **`src/slic3r/GUI/SelectMachine.cpp`** / **`.hpp`**
  - Same visibility rule as Send to Printer for start/end toggles.
  - **Print-all:** same mask vector; **`reference_plate_for_merged_cloud_job()`** returns the **first included** plate; **`reset_and_sync_ams_list()`** unions filaments only over included plates.
  - **`get_ams_mapping_result()`** must resolve **`m_filaments`** entries by **filament id**, not by the inner-loop index into **`m_ams_mapping_result`** (order differs after merge; sizes can change when the table is toggled).

- **Export options (plate changer):** **`src/slic3r/GUI/PlateChangerExportOptionsDialog.{hpp,cpp}`**  
  Same table + mask; **`plate_changer_plate_included()`** returned to **`export_gcode_3mf`** after OK.

### Config keys

- **`plate_change_gcode`** – printer preset option (e.g. in **PrintConfig** / **PrinterSettings**). Used to decide whether plate-changer actions and start/end toggles are available, and as the exact block inserted at each position (start, between plates, end) when building merged G-code.
- **`additional_initial_plate_change_gcode`** – optional; editable under **Printer settings → Machine G-code**, directly under **Plate change G-code**, in a **collapsible section** (collapsed by default). Intended to run before **`plate_change_gcode`** when **Start with new plate?** is used (merge behavior is wired separately).

---

## Summary

| Item | Description |
|------|-------------|
| **Config** | Printer Settings → Machine G-code → **Plate change G-code** |
| **Merge helper** | **`build_plate_changer_merged_gcode()`** in **`bbs_3mf.cpp`** |
| **StoreParams** | **`use_plate_changer_all`**, **`start_with_new_plate`**, **`end_with_new_plate`** |
| **Send/Export** | **Plater::export_3mf**, **export_config_3mf**, **send_gcode** carry the flags + optional **`plate_changer_plate_included`** mask |
| **Plate table** | **PlateChangerPlateStatsTable**; **Include** column + totals; **PlateChangerExportOptionsDialog** for export |
| **Dialogs** | **SendToPrinter**, **SelectMachine**: separator + start/end toggles whenever preset has **plate_change_gcode**; print-all also shows the plate grid |
| **Limitation** | One G-code string for start, between plates, and end (no separate start/end sequences) |
