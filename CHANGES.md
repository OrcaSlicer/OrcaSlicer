# Changelog

All notable changes to this fork are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added

- **Volumetric Temperature Compensation (VTC)** — predictive nozzle temperature
  control that follows future volumetric flow. When flow changes during a print
  (a shrinking vase circumference, or any region slowed to satisfy minimum layer
  time) the filament dwells a different time in the melt zone and its effective
  melt temperature drifts, producing visible gloss/sheen variation on silk and
  matte filaments. VTC looks ahead in the tool-path, maps volumetric flow to a
  target temperature and injects non-blocking `M104` commands early enough that
  the hotend reaches the target exactly when the new flow begins. It never emits
  `M109` and clamps every setpoint to a safety band around the nominal
  temperature. Implemented as a whole-buffer post-processing pass
  (`src/libslic3r/GCode/VolTempCompensation.{hpp,cpp}`), gated by `vtc_enabled`
  (off by default — no effect on existing profiles or output when disabled).

  - Per-filament settings (Filament tab → *Volumetric temperature
    compensation*): simple slope mapping or a custom monotonic PCHIP
    flow→temperature curve, plus look-ahead, smoothing and command-rate
    controls.
  - Per-hotend thermal model (Printer tab): heating rate, passive/active cooling
    rates, PID-overshoot compensation and settling margin — calibrated once per
    machine.
  - Example values pre-filled in the *Generic PLA @System* profile (disabled by
    default; flip the enable toggle to use it).
  - Unit tests: `tests/libslic3r/test_vol_temp_compensation.cpp` (4 cases /
    2681 assertions).

  Concept & original idea: **Nadir @ CN3D** (`n.nadir@gmail.com`), MIT licensed
  with attribution. Native C++ port of the reference `vtc_postprocess.py`.
