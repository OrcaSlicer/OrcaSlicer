# Plugin examples

`ScanlineInfill/` is a working example of a Python infill plugin. A plug-in package is one
directory containing exactly one `.py` entry file; here the directory and entry file are both
named `ScanlineInfill`.

```
examples/plugins/
├── README.md
└── ScanlineInfill/
    └── ScanlineInfill.py
```

The entry file carries PEP 723 metadata (`requires-python`, `dependencies`) and plugin package
metadata under `[tool.orcaslicer.plugin]`. It needs Python 3.12 or newer, has no third-party
dependencies, and registers its capability with `@orca.plugin` + `orca.register_capability`.

## Install

**Local installer (recommended).** In OrcaSlicer open the Plugins dialog, choose **Install local
plugin**, and select `ScanlineInfill/ScanlineInfill.py`. The installer copies the entry file into
its own directory under the data directory.

**Manual install.** Put the entry file in a directory of its own under the data directory's
`orca_plugins` folder:

```
<data-dir>/orca_plugins/ScanlineInfill/ScanlineInfill.py
```

Find `<data-dir>` with **Help → Show Configuration Folder**, or pass `--datadir` when starting
OrcaSlicer. The directory must contain exactly one `.py` file. Then rescan or restart so the
plugin is discovered, and make sure it is enabled in the Plugins dialog.

## Use

1. Enable **Scanline Hatch Infill** in the Plugins dialog.
2. In the print settings, choose `scanline-hatch` as the **Sparse infill pattern**.
3. Slice. The pattern follows the usual sparse-infill spacing and direction settings; the phase
   is anchored at the fill direction origin.

The capability's canonical `get_name()` value (`scanline-hatch`) is the key stored in
`sparse_infill_pattern`, so it is also the value that appears in a saved preset or project.

## Implementation notes

- `generate(request)` returns open paths as `orca.host.Point` sequences in scaled integer
  coordinates. The host converts them to extrusions and applies flow and gap fill.
- The hatch is clipped to `request.expolygon.contour` and its holes with even-odd intersections,
  because the host does not clip plugin paths again.
- Line spacing follows Fill's sparse-line convention (`request.spacing / request.density`), falling
  back to `request.flow_width / request.density`; millimetres are converted with
  `orca.slicing.unscale(1)` rather than a hard-coded scale.
- Orientation follows `request.direction_angle` using Fill's built-in convention (the actual scanlines
  run 90° to it, which is the selected infill direction); the grid phase comes from
  `request.direction_origin`. Consecutive lines alternate direction, and `no_sort()` keeps that
  order.
- Empty or degenerate rings produce no paths. The plugin makes no GUI calls: `generate()` runs on
  a slicing worker thread.
- This is an example, not a production-grade infill implementation. It has no Arachne /
  variable-width support, and it does not attempt pattern-specific optimizations beyond the
  scanline ordering described above.
