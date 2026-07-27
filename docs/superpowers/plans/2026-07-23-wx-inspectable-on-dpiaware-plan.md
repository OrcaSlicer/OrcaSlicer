# Move `wxInspectable` into `DPIAware` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `wxInspector::wxInspectable` from individual leaf classes into the common `DPIAware<P>` template so every DPIAware widget is automatically inspectable and gets the inspector keyboard shortcut.

**Architecture:** `DPIAware<P>` gains `wxInspector::wxInspectable` as a second base class and calls `SetupInspectorAccelerator(this)` in its constructor. `DPIDialog` and `MainFrame` drop their now-redundant `wxInspectable` inheritance and `SetupInspectorAccelerator` calls.

**Tech Stack:** C++17, wxWidgets, wxInspector

## Global Constraints

- Build with `D:\VisualStudio\2026\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Use `--config RelWithDebInfo` for all builds
- Cross-platform: must compile on Windows, macOS, and Linux
- Match existing code style: PascalCase classes, `#pragma once`
- Do NOT commit files under `.superpowers/`
- Do NOT commit `task.md`

---

### Task 1: Move `wxInspectable` and `SetupInspectorAccelerator` into `DPIAware<P>`

**Files:**
- Modify: `src/slic3r/GUI/GUI_Utils.hpp:92` (DPIAware template — add wxInspectable base + SetupInspectorAccelerator call)
- Modify: `src/slic3r/GUI/GUI_Utils.hpp:276` (DPIDialog — drop wxInspectable + SetupInspectorAccelerator)
- Modify: `src/slic3r/GUI/MainFrame.hpp:96` (MainFrame — drop wxInspectable)
- Modify: `src/slic3r/GUI/MainFrame.cpp:304` (MainFrame constructor — drop SetupInspectorAccelerator)

**Interfaces:**
- Consumes: Nothing (standalone refactor)
- Produces: All DPIAware widgets automatically inherit `wxInspector::wxInspectable` and get Ctrl+Shift+I accelerator

- [ ] **Step 1: Add `wxInspectable` to `DPIAware<P>` and call `SetupInspectorAccelerator`**

In `src/slic3r/GUI/GUI_Utils.hpp`, line 92, change the base class:

```cpp
// Before:
template<class P> class DPIAware : public P
// After:
template<class P> class DPIAware : public P, public wxInspector::wxInspectable
```

In the constructor body of `DPIAware<P>`, after `this->CenterOnParent();` (currently line 110), add:

```cpp
SetupInspectorAccelerator(this);
```

(`<wx/inspector/inspector.h>` is already included at line 23.)

- [ ] **Step 2: Remove redundant `wxInspectable` and `SetupInspectorAccelerator` from `DPIDialog`**

In `src/slic3r/GUI/GUI_Utils.hpp`, line 276, change:

```cpp
// Before:
class DPIDialog : public DPIAware<wxDialog>, public wxInspector::wxInspectable
// After:
class DPIDialog : public DPIAware<wxDialog>
```

In the `DPIDialog` constructor body, remove the `SetupInspectorAccelerator(this);` line (currently line 286). The rest of the constructor stays.

- [ ] **Step 3: Remove redundant `wxInspectable` from `MainFrame`**

In `src/slic3r/GUI/MainFrame.hpp`, line 96, change:

```cpp
// Before:
class MainFrame : public DPIFrame, public wxInspector::wxInspectable
// After:
class MainFrame : public DPIFrame
```

`MainFrame` now gets `wxInspectable` through `DPIFrame` → `DPIAware<wxFrame>`.

- [ ] **Step 4: Remove redundant `SetupInspectorAccelerator` from `MainFrame` constructor**

In `src/slic3r/GUI/MainFrame.cpp`, line 304, remove the line:

```cpp
SetupInspectorAccelerator(this);
```

It is now called automatically by the `DPIAware<wxFrame>` constructor.

- [ ] **Step 5: Build to verify compilation**

```powershell
$cmakePath = "D:\VisualStudio\2026\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmakePath --build . --config RelWithDebInfo --target ALL_BUILD -- -m
```

Expected: Build succeeds with zero new errors or warnings.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/GUI_Utils.hpp src/slic3r/GUI/MainFrame.hpp src/slic3r/GUI/MainFrame.cpp
git commit -m "refactor: move wxInspectable and SetupInspectorAccelerator into DPIAware

DPIAware<P> now inherits wxInspector::wxInspectable and calls
SetupInspectorAccelerator in its constructor, making all DPIAware
widgets automatically appear in the inspector tree with the
Ctrl+Shift+I shortcut. Remove redundant wxInspectable inheritance
and SetupInspectorAccelerator calls from DPIDialog and MainFrame.

Co-Authored-By: Claude <noreply@anthropic.com>"
```
