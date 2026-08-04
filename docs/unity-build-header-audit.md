# Unity-build header audit

This audit covers tracked C/C++/Objective-C headers under `src/`, `tests/`,
and `sandboxes/`. Generated files, build directories, dependency trees, and
vendored Catch2 headers are excluded from the header-guard scan.

An unguarded header does not necessarily fail in a normal build: each `.cpp`
file is compiled as a separate translation unit. It becomes a unity-build
risk when more than one source file that includes the header is combined into
the same generated unity translation unit.

## Purpose and context

This audit was created while preparing OrcaSlicer for an optional CMake unity
build. The intended workflow is to enable or disable the CMake
CMAKE_UNITY_BUILD configure variable from VS Code's settings.json, while
keeping the existing non-unity build available.

The motivation is to determine whether combining translation units can reduce
overall build time. The repository currently compiles normally because each
.cpp file is generally compiled as a separate translation unit. Unity builds
concatenate several .cpp files, which exposes assumptions that ordinary builds
hide: missing header guards, repeated implementation-local names, macro leaks,
and source files that include other .cpp files.

The audit therefore has two goals:

1. Identify issues that would prevent or destabilize the unity build before
   attempting a full build.
2. Separate genuine source fixes from intentional CMake exceptions, while
   preserving behavior when CMAKE_UNITY_BUILD is disabled.

The findings are based on static inspection. They are not claims that every
listed pair will be placed in the same unity batch under every CMake version or
batch-size setting.

## Headers without a recognized include guard

The following 11 tracked headers contain neither `#pragma once` nor a
recognized `#ifndef`/`#define` include guard.

| Header | Notes |
| --- | --- |
| `sandboxes/its_neighbor_index/ItsNeighborIndex.hpp` | Included by both `main.cpp` and `ItsNeighborIndex.cpp` in the same sandbox target. |
| `src/libslic3r/Format/ModelIO.hpp` | Apple-only; included by `Model.cpp` and `Format/ModelIO.mm`. |
| `src/libslic3r/QuadricEdgeCollapse.hpp` | Included by `QuadricEdgeCollapse.cpp`, `SLA/Hollowing.cpp`, and `GUI/Gizmos/GLGizmoSimplify.cpp`. |
| `src/libslic3r/clonable_ptr.hpp` | Included through `Config.hpp`; currently lower risk because that parent header is guarded. |
| `src/slic3r/GUI/BambuPlayer/BambuPlayer.h` | Objective-C header included by `wxMediaCtrl2.mm`. |
| `src/slic3r/GUI/ConfigExceptions.hpp` | Included by `OptionsGroup.cpp`. |
| `src/slic3r/GUI/InstanceCheckMac.h` | Apple-only; included by `InstanceCheckMac.mm`. |
| `src/slic3r/GUI/PartSkipDialog.hpp` | Included directly by `PartSkipDialog.cpp` and indirectly through `StatusPanel.hpp`. |
| `src/slic3r/GUI/RemovableDriveManagerMM.h` | Apple-only; included by `RemovableDriveManagerMM.mm`. |
| `src/slic3r/GUI/WebUpdatePlugin.hpp` | Empty header; no current duplicate-definition risk. |
| `src/slic3r/GUI/Widgets/TextCtrl.h` | Included by `Field.cpp`, `SpinInput.cpp`, and `TextInput.cpp`. |

The highest-priority unity-build candidates are `QuadricEdgeCollapse.hpp`,
`PartSkipDialog.hpp`, `TextCtrl.h`, and
`ItsNeighborIndex.hpp`, because each is used by multiple source files that
can be placed in one unity batch.

## Source files including `.cpp` files

The following 5 `.cpp` includes were found:

| Location | Included source | Context |
| --- | --- | --- |
| `src/libslic3r/TryCatchSignal.cpp:4` | `TryCatchSignalSEH.cpp` | MSVC-specific implementation included into one translation unit. |
| `src/libslic3r/clipper.cpp:13` | `<clipper/clipper.cpp>` | Deliberate Clipper wrapper that sets namespace and type macros before inclusion. |
| `src/slic3r/GUI/MultiTaskManagerPage.cpp:7` | `<wx/listimpl.cpp>` | No matching `WX_DEFINE_LIST` use found; likely unnecessary. |
| `src/slic3r/GUI/Preferences.cpp:14` | `<wx/listimpl.cpp>` | No matching `WX_DEFINE_LIST` use found; likely unnecessary. |
| `src/slic3r/GUI/SendMultiMachinePage.cpp:8` | `<wx/listimpl.cpp>` | Required for the `WX_DEFINE_LIST(AmsRadioSelectorList)` call at line 19. |

### Why wxWidgets uses a `.cpp` include

This is a documented wxWidgets macro pattern. `wx/list.h` defines
`WX_DECLARE_LIST()` and a list declaration, but deliberately defines
`WX_DEFINE_LIST(name)` as a diagnostic placeholder. Including `wx/listimpl.cpp`
redefines `WX_DEFINE_LIST` to emit the list's static data and deletion
function; the following `WX_DEFINE_LIST(AmsRadioSelectorList)` then provides
those definitions for the list declared in `SendMultiMachinePage.hpp`.

Only `SendMultiMachinePage.cpp` uses `WX_DEFINE_LIST` in this repository.
`MultiTaskManagerPage.cpp` and `Preferences.cpp` include the implementation
fragment without a corresponding list definition, so those two includes are
likely stale and can be removed after a targeted build check. Repeating
`wx/listimpl.cpp` in a unity translation unit mainly repeats a macro
redefinition; it is unnecessary clutter, not three separate list
implementations.

## Additional unity-build symbol collisions

Unity builds concatenate multiple `.cpp` files into one translation unit.
Consequently, namespace-scope `static`, `const`, `constexpr`, `inline`, and
anonymous-namespace definitions can still collide. Internal linkage prevents
linker collisions between ordinary translation units, but it does not permit
two definitions with the same name in the same translation unit.

The following pairs are adjacent or close enough in the current CMake source
lists to be high-priority candidates for the default unity batch size.

### WipeTower and WipeTower2

`src/libslic3r/CMakeLists.txt` lists these files together at lines 253--256:

- `src/libslic3r/GCode/WipeTower.cpp`
- `src/libslic3r/GCode/WipeTower2.cpp`

In addition to `wipe_tower_wall_infill_overlap`, both files define the
following same-scope identifiers:

- `flat_iron_speed`
- `WIPE_TOWER_RESOLUTION`
- `arc_fit_size`
- `nozzle_diameter_to_nozzle_change_width`
- `LimitFlow`
- `Segment`
- `align_round`, `align_ceil`, and `align_floor`
- `is_valid_gcode`
- `chamfer_polygon`
- `rounding_rectangle`
- `scale_polygon`, `unscale_polygon`, and `generate_rectange`

They also define the macro `SCALED_WIPE_TOWER_RESOLUTION` with different
expansions. `WT_SIMPLIFY_TOLERANCE_SCALED` is a macro in `WipeTower.cpp` but a
variable in `WipeTower2.cpp`, so its interpretation is also order-dependent.

References:

- `src/libslic3r/GCode/WipeTower.cpp:18`
- `src/libslic3r/GCode/WipeTower2.cpp:27`

### 3MF implementations

`src/libslic3r/CMakeLists.txt` places `Format/3mf.cpp` and
`Format/bbs_3mf.cpp` in the same small source group. They duplicate numerous
global names, including:

- `MODEL_FOLDER`, `MODEL_EXTENSION`, and `MODEL_FILE`
- `CONTENT_TYPES_FILE`, `RELATIONSHIPS_FILE`, and `THUMBNAIL_FILE`
- `LAYER_CONFIG_RANGES_FILE`
- `CUSTOM_GCODE_PER_PRINT_Z_FILE`
- `FDM_SUPPORTS_PAINTING_VERSION`, `SEAM_PAINTING_VERSION`, and
  `MM_PAINTING_VERSION`
- many XML tag and attribute constants
- the global `version_error` class

References:

- `src/libslic3r/Format/3mf.cpp:43`
- `src/libslic3r/Format/bbs_3mf.cpp:65`
- `src/libslic3r/Format/3mf.cpp:159`
- `src/libslic3r/Format/bbs_3mf.cpp:475`

### GUI and support source pairs

The following duplicate names were also found in source files that are close
in the target source lists:

| Sources | Duplicate identifiers |
| --- | --- |
| `GUI/Jobs/PrintJob.cpp`, `GUI/Jobs/SendJob.cpp` | `check_gcode_failed_str`, `printjob_cancel_str`, `timeout_to_upload_str`, `failed_in_cloud_service_str`, `file_is_not_exists_str`, `file_over_size_str`, `print_canceled_str`, `send_print_failed_str`, `upload_ftp_failed_str`, `desc_network_error`, `desc_file_too_large`, `desc_fail_not_exist`, `desc_upload_ftp_failed`, `sending_over_lan_str`, `sending_over_cloud_str` |
| `GUI/Tabbook.cpp`, `GUI/TabButton.cpp` | `TAB_BUTTON_BG`, `TAB_BUTTON_SEL` |
| `GUI/CapsuleButton.cpp`, `GUI/FilamentMapPanel.cpp` | `BgNormalColor`, `BgSelectColor`, `BorderNormalColor` |
| `Support/SupportCommon.cpp`, `Support/SupportMaterial.cpp` | `support_types_interface` |
| `GUI/CalibrationWizard.cpp`, `GUI/CalibrationWizardCaliPage.cpp` | `NA_STR` |
| `GUI/OG_CustomCtrl.cpp`, `GUI/OptionsGroup.cpp` | `titleWidth` |
| `GUI/DeviceTab/wgtDeviceNozzleRack.cpp`, `GUI/DeviceTab/wgtDeviceNozzleSelect.cpp` | `a_nozzle_seq` |
| `GUI/Widgets/AMSControl.cpp`, `GUI/Widgets/AMSItem.cpp` | `AMS_CANS_WINDOW_SIZE` macro, with different replacements |

### Batch-size-dependent candidates

These are not necessarily in the same default batch, but become collisions
if the unity batch size or source grouping puts them together:

- `GUI/Gizmos/GLGizmoEmboss.cpp` and `GUI/Gizmos/GLGizmoSVG.cpp`: duplicate
  anonymous-namespace names including `rotation_snapshot_name`,
  `move_snapshot_name`, `IconType`, `IconState`, `GuiCfg`, `get_icon`, and
  `create_gui_configuration`.
- `GUI/3DBed.cpp` and `GUI/PartPlate.cpp`: `GROUND_Z`.
- `GUI/PrintOptionsDialog.cpp`, `GUI/SafetyOptionsDialog.cpp`, and
  `GUI/StatusPanel.cpp`: `STATIC_BOX_LINE_COL`; the first two also share
  `STATIC_TEXT_CAPTION_COL` and `STATIC_TEXT_EXPLAIN_COL`.
- `GUI/MediaPlayCtrl.cpp` and `GUI/Printer/PrinterFileSystem.cpp`:
  `error_messages`.
- `GUI/Auxiliary.cpp` and `GUI/Project.cpp`: `license_list`.
- `GUI/StatusPanel.cpp` and `GUI/Widgets/AxisCtrlButton.cpp`:
  `BUTTON_PRESS_COL`.

## Macros are a separate unity-build hazard

Namespaces do not isolate preprocessor macros. Besides the WipeTower macro,
the scan found differing definitions of names such as:

- `MATERIAL_ITEM_SIZE`
- `MAPPING_ITEM_REAL_SIZE`
- `BORDER_W`
- `BTN_GAP`
- `SUPPORT_MATERIAL_MARGIN`
- `AMS_CANS_WINDOW_SIZE`

If the defining files share a unity translation unit, the redefinition can
produce a diagnostic or change how later source text is preprocessed. Renaming
these macros, limiting their scope with `#undef`, or excluding the affected
source from unity is safer than relying on namespaces.

## Anonymous namespace assessment

Wrapping each source file's definitions in an anonymous namespace is not a
general fix. In a normal build, each source file has a separate anonymous
namespace. In a unity build, the files are concatenated into one translation
unit, so their anonymous namespaces are the same unnamed namespace and the
identifiers can still be redefined.

A unique named namespace per implementation can separate C++ identifiers, but
it requires adjusting references and does not affect macros, header guards, or
included `.cpp` files. For legacy implementation pairs such as WipeTower and
the two 3MF implementations, the proper source fix is to put private helpers, constants, and types in unique named detail namespaces, rename implementation-local symbols where appropriate, and replace or undefine macros. The CMake exclusion below is a temporary or intentional build-system exception, not the preferred source fix; to
exclude one or both files from unity:

```cmake
set_source_files_properties(
    GCode/WipeTower2.cpp
    Format/bbs_3mf.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
)
```

The paths must be relative to the CMake directory containing the command.

## Verification

This is a static scan only. No build or unity-build configuration was run.
