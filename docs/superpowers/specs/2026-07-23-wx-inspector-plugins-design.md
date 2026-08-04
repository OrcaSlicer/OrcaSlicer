# wxInspector Plugins for OrcaSlicer Custom Controls — Design Spec

Date: 2026-07-23
Branch: `dev/layout-inspector`

## Overview

Create wxInspector plugins that expose OrcaSlicer's custom widget properties in the inspector's property grid. Without these plugins, the inspector shows only generic wxWidgets properties — missing all DPI-awareness data, custom styling, and Orca-specific control state.

## Goals

1. **DPIAware properties** — Inspect and update `scale_factor`, `prev_scale_factor`, `em_unit`, and `normal_font` on any DPIAware-derived widget
2. **Custom widget properties** — Surface Orca-specific properties on `Button`, `CheckBox`, `TextInput`, `SwitchButton`, `ProgressBar`, `Label`, and `LabeledStaticBox`
3. **Minimal source changes** — Only add trivial (one-line) getters/setters to existing classes; no architectural refactoring of Orca's widget hierarchy

## Non-Goals

- Custom inspector panels or AUI tabs (use the existing property grid and method invoker)
- Python-plugin integration (this is C++ wxInspector, not Orca's Python plugin system)
- Event logging customization (the built-in event logger already works)

## Architecture

### Two Plugins

| Plugin | Class | Files |
|--------|-------|-------|
| DPIAware plugin | `DPIAwarePlugin` | `DPIAwarePlugin.hpp`, `DPIAwarePlugin.cpp` |
| Custom widgets plugin | `CustomWidgetsPlugin` | `CustomWidgetsPlugin.hpp`, `CustomWidgetsPlugin.cpp` |
| Registration helper | inline function | `Registration.hpp` |

All files live under `src/slic3r/Utils/wxInspectorPlugins/`.

### Plugin Detection Strategy

**DPIAware plugin**: Uses `dynamic_cast<DPIFrame*>` and `dynamic_cast<DPIDialog*>` as detection gates. `DPIFrame` = `DPIAware<wxFrame>`, `DPIDialog` = `DPIAware<wxDialog>`. Since these are concrete typedefs, `dynamic_cast` works at runtime. This covers `MainFrame`, `SettingsDialog`, and all 8 calibration dialogs (which inherit `DPIDialog`).

**Custom widgets plugin**: Gates broadly on `CLASSINFO(wxWindow)`, then uses per-type `dynamic_cast` inside `GetProperties` to check each Orca-specific type. Only matching types append properties.

### Registration

A single `RegisterOrcaInspectorPlugins()` inline function in `Registration.hpp` creates both plugins as function-local statics (matching the wxInspector built-in provider pattern) and registers them via `wxInspector::RegisterPlugin()`.

Called once from `MainFrame::MainFrame()` after `SetupInspectorAccelerator(this)`.

### Why Separate Plugins?

- DPIAware is a C++ template concept (not a wxClassInfo-isKindOf check), so it needs its own detection logic
- Custom widgets use standard wxClassInfo-based detection, matching the built-in provider pattern
- Two focused files are easier to review and maintain than one monolithic plugin
- Compile-time failure isolation: if a widget header changes, only one plugin breaks

## DPIAware Plugin — Property Specification

### Source Changes (GUI_Utils.hpp)

Four one-liner methods added to the `DPIAware<P>` template class (public section):

```cpp
float scale_factor() const       { return m_scale_factor; }  // already exists
float prev_scale_factor() const  { return m_prev_scale_factor; }  // already exists
int   em_unit() const            { return m_em_unit; }  // already exists
void  set_scale_factor(float v)      { m_scale_factor = v; }      // NEW
void  set_prev_scale_factor(float v) { m_prev_scale_factor = v; } // NEW
void  set_em_unit(int v)             { m_em_unit = v; }           // NEW
bool  force_rescale() const          { return m_force_rescale; }  // NEW
// m_normal_font getter already exists: normal_font()
```

### Detection

```cpp
bool CanProvideProperties(wxClassInfo* info) override {
    // Gated in GetProperties via dynamic_cast on the window itself
    return info->IsKindOf(CLASSINFO(wxWindow));
}
```

In `GetProperties`:
```cpp
auto* win = obj.AsWindow();
bool isDPI = dynamic_cast<DPIFrame*>(win) || dynamic_cast<DPIDialog*>(win);
if (!isDPI) return props;
```

### Property Table (category: "DPI Scaling")

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Scale Factor | String (float) | Yes | `dpi->scale_factor()` | `dpi->set_scale_factor(v)` |
| Prev Scale Factor | String (float) | Yes | `dpi->prev_scale_factor()` | `dpi->set_prev_scale_factor(v)` |
| EM Unit | Integer | Yes | `dpi->em_unit()` | `dpi->set_em_unit(v)` |
| Normal Font | ReadOnly | No | `dpi->normal_font().GetNativeFontInfoDesc()` | — |
| Force Rescale | Boolean (ReadOnly) | No | `dpi->force_rescale()` | — |

**Note on setters**: The setters simply store values. They do NOT trigger a widget rescale/layout. To see the effect of a changed scale factor, use the inspector's Methods panel to call `Layout()` or resize the window — which triggers the DPI_CHANGED event path naturally.

## Custom Widgets Plugin — Property Specification

All properties are appended to the built-in wxWindow properties. Each widget type is independently detected via `dynamic_cast`.

### Detection gates (in `GetProperties`)

```cpp
auto* win = obj.AsWindow();
if (auto* btn = dynamic_cast<Button*>(win))          { addButtonProperties(btn, props); }
if (auto* cb  = dynamic_cast<CheckBox*>(win))         { addCheckBoxProperties(cb, props); }
if (auto* ti  = dynamic_cast<TextInput*>(win))        { addTextInputProperties(ti, props); }
if (auto* sb  = dynamic_cast<SwitchButton*>(win))     { addSwitchButtonProperties(sb, props); }
if (auto* pb  = dynamic_cast<ProgressBar*>(win))      { addProgressBarProperties(pb, props); }
if (auto* lbl = dynamic_cast<Label*>(win))            { addLabelProperties(lbl, props); }
if (auto* lsb = dynamic_cast<LabeledStaticBox*>(win)) { addLabeledStaticBoxProperties(lsb, props); }
```

### Orca Button (`Button`) — category: "Orca Button"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Button Style | Choice | Yes | enum→string | string→enum |
| Button Type | Choice | Yes | enum→string | string→enum |
| Selected | Boolean | Yes | `m_selected` (needs getter) | `SetSelected(v)` |
| Active Icon | ReadOnly | No | icon name string | — |
| Inactive Icon | ReadOnly | No | icon name string | — |

Choices for Button Style: `Regular`, `Confirm`, `Alert`, `Disabled`
Choices for Button Type: `Compact`, `Window`, `Choice`, `Parameter`, `Icon`, `Expanded`

**Source changes needed**: Button's `m_selected` is private. Add one-liner getter:
```cpp
bool IsSelected() const { return m_selected; }
```

### Orca CheckBox (`CheckBox`) — category: "Orca CheckBox"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Half Checked | Boolean | Yes | `m_half_checked` (needs getter) | `SetHalfChecked(v)` |

**Source changes needed**: `m_half_checked` is private. Add one-liner getter:
```cpp
bool IsHalfChecked() const { return m_half_checked; }
```

### Orca TextInput (`TextInput`) — category: "Orca TextInput"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Label | String | Yes | `GetLabel()` (inherited from wxWindow) | `SetLabel(v)` (exists) |
| Text Value | String | Yes | `GetTextCtrl()->GetValue()` (GetTextCtrl is public) | `GetTextCtrl()->SetValue(v)` |
| Corner Radius | Integer | Yes | `GetCornerRadius()` (NEW) | `SetCornerRadius(v)` (exists) |

**Source changes needed**: Add one getter to `TextInput`:
```cpp
int GetCornerRadius() const { return static_cast<int>(radius); }
```
(`radius` is inherited from StaticBox. `SetCornerRadius(double)` already exists. `GetTextCtrl()` is already public.)

### Orca SwitchButton (`SwitchButton`) — category: "Orca SwitchButton"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Value | Boolean | Yes | existing getter | existing setter |

### Orca ProgressBar (`ProgressBar`) — category: "Orca ProgressBar"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Proportion | Float (0-1) | Yes | `pb->m_proportion` (public member) | `pb->m_proportion = v` |
| Show Number | Boolean | Yes | `pb->m_shownumber` (public member) | `pb->m_shownumber = v` |

**No source changes needed**: `m_proportion` and `m_shownumber` are already public members. `SetValue(int)` and `SetProgress(int)` already exist as public methods.

### Orca Label (`Label`) — category: "Orca Label"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Is Hyperlink | Boolean | No | existing flag check | — |
| Font Size | ReadOnly | No | `GetFont().GetPointSize()` | — |

### LabeledStaticBox — category: "LabeledStaticBox"

| Name | Type | Editable | Getter | Setter |
|------|------|----------|--------|--------|
| Corner Radius | Integer | Yes | `GetCornerRadius()` (NEW) | `SetCornerRadius(v)` (exists) |
| Border Width | Integer | Yes | `GetBorderWidth()` (NEW) | `SetBorderWidth(v)` (exists) |
| Border Color | String (hex) | Yes | `GetBorderColor()` (NEW) | `SetBorderColor(v)` (exists) |
| Scale | Float (ReadOnly) | No | `m_scale` (protected, needs getter) | — |

**Source changes needed**: Four one-liner getters added to `LabeledStaticBox`:
```cpp
int   GetCornerRadius() const { return m_radius; }
int   GetBorderWidth() const   { return m_border_width; }
StateColor GetBorderColor() const { return border_color; }
float GetScale() const         { return m_scale; }
```

## Files Modified (Existing Code)

| File | Changes |
|------|---------|
| `src/slic3r/GUI/GUI_Utils.hpp` | +4 methods in `DPIAware<P>`: `set_scale_factor()`, `set_prev_scale_factor()`, `set_em_unit()`, `force_rescale()` |
| `src/slic3r/GUI/Widgets/LabeledStaticBox.hpp` | +4 getter declarations: `GetCornerRadius()`, `GetBorderWidth()`, `GetBorderColor()`, `GetScale()` |
| `src/slic3r/GUI/Widgets/LabeledStaticBox.cpp` | +4 getter implementations |
| `src/slic3r/GUI/Widgets/Button.hpp` | +1 getter: `IsSelected()` |
| `src/slic3r/GUI/Widgets/CheckBox.hpp` | +1 getter: `IsHalfChecked()` |
| `src/slic3r/GUI/Widgets/TextInput.hpp` | +1 getter: `GetCornerRadius()` |
| `src/slic3r/GUI/Widgets/ProgressBar.hpp` | None (public members are used directly) |
| `src/slic3r/GUI/MainFrame.cpp` | +1 `#include`, +1 call to `RegisterOrcaInspectorPlugins()` |
| `src/slic3r/CMakeLists.txt` | +4 entries in `SLIC3R_GUI_SOURCES` (the .cpp plugin files) |

## Files Created

```
src/slic3r/Utils/wxInspectorPlugins/
├── DPIAwarePlugin.hpp
├── DPIAwarePlugin.cpp
├── CustomWidgetsPlugin.hpp
├── CustomWidgetsPlugin.cpp
└── Registration.hpp
```

## Build & Linking

The `wxInspector` dependency is already wired:
- `deps/wxInspector/wxInspector.cmake` fetches and builds wxInspector
- `src/CMakeLists.txt` lines 92-93 link `wxInspector::wxInspector` into `wxWidgets_LIBRARIES`
- The plugin files only need `#include <wx/inspector/plugin.h>` and `#include <wx/inspector/inspector.h>` — both available from the installed dependency

No new CMake dependencies needed. Only the new source files need listing in `SLIC3R_GUI_SOURCES`.

## Error Handling & Edge Cases

- **Stale pointers**: Plugin lambdas capture raw pointers, regenerated on every `GetProperties` call (matching wxInspector's built-in provider pattern). Pointers live only until the next tree selection.
- **Widget destruction**: If a widget is destroyed while the inspector is showing its properties, `InspectableObject::IsValid()` returns false and properties are not displayed. The inspector won't show stale data.
- **Invalid property values**: Setters use `sscanf` / `ToLong` with validation (matching built-in patterns). Bogus input is rejected — setter returns `false`, property grid shows error state.
- **DPI drift**: Setting `scale_factor` without triggering rescale means displayed sizes don't match the new factor. This is acceptable — the inspector is a developer tool; operators know to call `Layout()` after making changes.
- **Missing widget type**: If a `dynamic_cast` fails for all types, only built-in wxWindow properties are shown. No crash, no error — just reduced info.

## Future Work (Out of Scope)

- **StateColor visualization**: `StateColor` is a multi-value type (maps bitmask states to colors). A full solution would need a custom property editor (e.g., a table showing each state→color pair). Keep it simple for now.
- **ScalableBitmap display**: Could show the bitmap as an inline thumbnail. Complex property editor work — deferred.
- **More widget types**: `SwitchBoard`, `MultiSwitchButton`, `StepCtrl`, `FanControl`, `DropDown`, `ComboBox`, `AMS*` widgets could all benefit. Add as needed.
- **Property refresh on tree selection**: Currently properties are static snapshots. A "refresh" button or auto-poll could keep values current for rapidly-changing widgets (progress bars, etc.). The built-in wxInspector already provides a tree-refresh button.
