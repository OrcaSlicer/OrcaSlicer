# Move `wxInspectable` into `DPIAware` — Design Spec

Date: 2026-07-23
Branch: `dev/layout-inspector`

## Overview

Move the `wxInspector::wxInspectable` base class from individual leaf classes (`DPIDialog`, `MainFrame`) into the common `DPIAware<P>` template. This makes every DPIAware widget automatically visible in the inspector tree without requiring each subclass to opt in.

## Motivation

Currently, only `DPIDialog` and `MainFrame` explicitly inherit `wxInspectable`. `DPIFrame` (which `MainFrame` inherits from) does not — `MainFrame` adds it manually. This means:

- Any `DPIAware<T>` widget that isn't `DPIDialog` or `MainFrame` is invisible in the inspector tree
- `DPIFrame` subclasses (`BaseTransparentDPIFrame`, `ImageDPIFrame`, `ModelMallDialog`, `MediaFileFrame`, `SecondaryCheckDialog`, `PrintErrorDialog`, etc.) don't appear
- Adding a new DPIAware widget type requires remembering to also inherit `wxInspectable`

Moving `wxInspectable` to `DPIAware` fixes this for all current and future DPIAware widgets at once.

## Design

### Change 1: `GUI_Utils.hpp` — `DPIAware<P>`

Add `wxInspector::wxInspectable` as a second base class, and call `SetupInspectorAccelerator(this)` in the constructor (after `this->CenterOnParent()`):

```cpp
// Before:
template<class P> class DPIAware : public P

// After:
template<class P> class DPIAware : public P, public wxInspector::wxInspectable
```

Add in the constructor body (after `this->CenterOnParent()` at line 110):
```cpp
SetupInspectorAccelerator(this);
```

This gives every `DPIAware<T>` widget both inspectability and the Ctrl+Shift+I keyboard shortcut automatically. `#include <wx/inspector/inspector.h>` is already present in the file.

### Change 2: `GUI_Utils.hpp` — `DPIDialog`

Remove the now-redundant `wxInspector::wxInspectable` and the `SetupInspectorAccelerator(this)` call:

```cpp
// Before:
class DPIDialog : public DPIAware<wxDialog>, public wxInspector::wxInspectable
// ...
    SetupInspectorAccelerator(this);

// After:
class DPIDialog : public DPIAware<wxDialog>
// (SetupInspectorAccelerator call removed — now done in DPIAware constructor)
```

`DPIDialog` gets `wxInspectable` and the accelerator through `DPIAware<wxDialog>` now.

### Change 3: `MainFrame.hpp` — `MainFrame`

Remove the now-redundant `wxInspector::wxInspectable`:

```cpp
// Before:
class MainFrame : public DPIFrame, public wxInspector::wxInspectable

// After:
class MainFrame : public DPIFrame
```

`MainFrame` gets `wxInspectable` through `DPIFrame` → `DPIAware<wxFrame>`.

### Change 4: `MainFrame.cpp` — `MainFrame` constructor

Remove the now-redundant `SetupInspectorAccelerator(this)` call (line 304). It will be called automatically by the `DPIAware` constructor.

## Impact

| Widget | Before | After |
|--------|--------|-------|
| `DPIDialog` subclasses (~80) | ✓ inspectable | ✓ inspectable (transitive) |
| `MainFrame` | ✓ inspectable | ✓ inspectable (transitive) |
| `DPIFrame` subclasses (8 others) | ✗ invisible | ✓ inspectable |
| Future `DPIAware<T>` | ✗ invisible | ✓ inspectable |

## Files Modified

| File | Change |
|------|--------|
| `src/slic3r/GUI/GUI_Utils.hpp` | `DPIAware<P>` gains `wxInspector::wxInspectable` + `SetupInspectorAccelerator(this)` call; `DPIDialog` drops redundant `wxInspector::wxInspectable` and `SetupInspectorAccelerator(this)` |
| `src/slic3r/GUI/MainFrame.hpp` | `MainFrame` drops redundant `wxInspector::wxInspectable` |
| `src/slic3r/GUI/MainFrame.cpp` | Remove redundant `SetupInspectorAccelerator(this)` from MainFrame constructor |

## Non-Goals

- The `DPIAwarePlugin` detection logic (`dynamic_cast<DPIFrame*>` / `dynamic_cast<DPIDialog*>`) is unchanged
- No new DPI properties — this is purely about tree visibility and accelerator setup

## Risk Assessment

- **Multiple inheritance**: `DPIAware<P>` already has a vtable (virtual destructor). Adding `wxInspectable` adds a second base but no additional data members. The `wxInspector::wxInspectable` class is expected to be a lightweight marker interface.
- **Build**: No new includes needed; `<wx/inspector/inspector.h>` is already included in `GUI_Utils.hpp`.
- **Cross-platform**: The change is standard C++ multiple inheritance — no platform-specific concerns.
