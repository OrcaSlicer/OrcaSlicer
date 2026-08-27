# wxInspector Plugins for OrcaSlicer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build two wxInspector plugins (DPIAware + CustomWidgets) that expose OrcaSlicer custom control properties in the inspector's property grid.

**Architecture:** Two plugins in a shared folder under `src/slic3r/Utils/wxInspectorPlugins/`. DPIAwarePlugin uses `dynamic_cast<DPIFrame*>/<DPIDialog*>` for detection; CustomWidgetsPlugin uses per-type `dynamic_cast`. Both registered as static singletons via a single inline function in `Registration.hpp`, called from `MainFrame` constructor.

**Tech Stack:** C++17, wxWidgets, wxInspector plugin API (`wx/inspector/plugin.h`, `wx/inspector/inspector.h`), OrcaSlicer custom widget headers

## Global Constraints

- Plugins placed under `src/slic3r/Utils/wxInspectorPlugins/`
- Build with `D:\VisualStudio\2026\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Minimal source changes: only trivial (one-line) getters/setters added to existing classes
- Cross-platform: must compile on Windows, macOS, and Linux
- Match existing code style: PascalCase classes, snake_case functions, `#pragma once`

---

### Task 1: Add getters/setters to existing Orca widget headers

**Files:**
- Modify: `src/slic3r/GUI/GUI_Utils.hpp` (DPIAware template — add 4 methods)
- Modify: `src/slic3r/GUI/Widgets/Button.hpp` (add 3 getters)
- Modify: `src/slic3r/GUI/Widgets/CheckBox.hpp` (add 1 getter)
- Modify: `src/slic3r/GUI/Widgets/TextInput.hpp` (add 1 getter)
- Modify: `src/slic3r/GUI/Widgets/LabeledStaticBox.hpp` (add 4 getter declarations)
- Modify: `src/slic3r/GUI/Widgets/LabeledStaticBox.cpp` (add 4 getter implementations)

**Interfaces:**
- Consumes: Nothing (prerequisite for all other tasks)
- Produces:
  - `DPIAware<P>::set_scale_factor(float)`, `DPIAware<P>::set_prev_scale_factor(float)`, `DPIAware<P>::set_em_unit(int)`, `DPIAware<P>::force_rescale() const`
  - `Button::GetStyle()`, `Button::GetType()`, `Button::IsSelected()`
  - `CheckBox::IsHalfChecked()`
  - `TextInput::GetCornerRadius()`
  - `LabeledStaticBox::GetCornerRadius()`, `LabeledStaticBox::GetBorderWidth()`, `LabeledStaticBox::GetBorderColor()`, `LabeledStaticBox::GetScale()`

- [ ] **Step 1: Add DPIAware setters/getter in GUI_Utils.hpp**

After line 184 (`float prev_scale_factor() const { return m_prev_scale_factor; }`), add:

```cpp
void  set_scale_factor(float v)      { m_scale_factor = v; }
void  set_prev_scale_factor(float v) { m_prev_scale_factor = v; }
void  set_em_unit(int v)             { m_em_unit = v; }
bool  force_rescale() const          { return m_force_rescale; }
```

- [ ] **Step 2: Add Button getters in Button.hpp**

After line 79 (`void SetSelected(bool selected = true) { m_selected = selected; }`), add:

```cpp
ButtonStyle GetStyle() const { return m_style; }
ButtonType  GetType() const  { return m_type; }
bool IsSelected() const      { return m_selected; }
```

- [ ] **Step 3: Add CheckBox getter in CheckBox.hpp**

After line 16 (`void SetHalfChecked(bool value = true);`), add:

```cpp
bool IsHalfChecked() const { return m_half_checked; }
```

- [ ] **Step 4: Add TextInput getter in TextInput.hpp**

After line 44 (`void SetCornerRadius(double radius);`), add:

```cpp
int GetCornerRadius() const { return static_cast<int>(radius); }
```

(Note: `radius` is inherited from `StaticBox` which has it as a protected `double` member.)

- [ ] **Step 5: Add LabeledStaticBox getter declarations in LabeledStaticBox.hpp**

After line 46 (`bool Enable(bool enable) override;`), add:

```cpp
int        GetCornerRadius() const { return m_radius; }
int        GetBorderWidth() const  { return m_border_width; }
StateColor GetBorderColor() const  { return border_color; }
float      GetScale() const        { return m_scale; }
```

(Note: all of `m_radius`, `m_border_width`, `border_color`, `m_scale` are protected members, accessible to inline methods.)

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/GUI_Utils.hpp src/slic3r/GUI/Widgets/Button.hpp src/slic3r/GUI/Widgets/CheckBox.hpp src/slic3r/GUI/Widgets/TextInput.hpp src/slic3r/GUI/Widgets/LabeledStaticBox.hpp
git commit -m "feat: add getters/setters for wxInspector plugin access

Add minimal public accessors to DPIAware (set_scale_factor,
set_prev_scale_factor, set_em_unit, force_rescale), Button
(GetStyle, GetType, IsSelected), CheckBox (IsHalfChecked),
TextInput (GetCornerRadius), and LabeledStaticBox
(GetCornerRadius, GetBorderWidth, GetBorderColor, GetScale)."
```

---

### Task 2: Create Registration helper header

**Files:**
- Create: `src/slic3r/Utils/wxInspectorPlugins/Registration.hpp`

**Interfaces:**
- Consumes: Nothing (forward-declares plugin classes)
- Produces: `RegisterOrcaInspectorPlugins()`

- [ ] **Step 1: Create directory**

```bash
mkdir -p src/slic3r/Utils/wxInspectorPlugins
```

- [ ] **Step 2: Write Registration.hpp**

```cpp
#pragma once

namespace wxInspector {
class wxInspectorPlugin;
void RegisterPlugin(wxInspectorPlugin* plugin);
}

// Forward declare our plugins
class DPIAwarePlugin;
class CustomWidgetsPlugin;

inline void RegisterOrcaInspectorPlugins()
{
    static DPIAwarePlugin          dpiaware;
    static CustomWidgetsPlugin     customWidgets;
    wxInspector::RegisterPlugin(&dpiaware);
    wxInspector::RegisterPlugin(&customWidgets);
}
```

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/Utils/wxInspectorPlugins/Registration.hpp
git commit -m "feat: add wxInspector plugin registration helper

Add RegisterOrcaInspectorPlugins() inline function that creates
and registers the DPIAwarePlugin and CustomWidgetsPlugin as
static instances (matching wxInspector's built-in pattern)."
```

---

### Task 3: Create DPIAwarePlugin

**Files:**
- Create: `src/slic3r/Utils/wxInspectorPlugins/DPIAwarePlugin.hpp`
- Create: `src/slic3r/Utils/wxInspectorPlugins/DPIAwarePlugin.cpp`

**Interfaces:**
- Consumes: Task 1 (DPIAware getters/setters), Task 2 (registration pattern)
- Produces: `class DPIAwarePlugin : public wxInspector::wxInspectorPlugin`

- [ ] **Step 1: Write DPIAwarePlugin.hpp**

```cpp
#pragma once

#include <wx/inspector/plugin.h>

class DPIAwarePlugin : public wxInspector::wxInspectorPlugin
{
public:
    wxString GetName() const override;

    bool CanProvideProperties(wxClassInfo* info) override;

    wxVector<wxInspector::PropertyDef> GetProperties(
        wxInspector::InspectableObject& obj) override;
};
```

- [ ] **Step 2: Write DPIAwarePlugin.cpp**

```cpp
#include "DPIAwarePlugin.hpp"

#include "slic3r/GUI/GUI_Utils.hpp" // DPIFrame, DPIDialog, DPIAware<P>

#include <wx/window.h>

namespace {

template<typename T>
void addDPIProps(T* dpi, wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    props.push_back({"Scale Factor", "DPI Scaling", PropertyType::String,
        wxString::Format("%.2f", dpi->scale_factor()), false, {},
        [dpi]() { return wxString::Format("%.2f", dpi->scale_factor()); },
        [dpi](const wxString& v) {
            double val;
            if (wxSscanf(v, "%lf", &val) != 1) return false;
            dpi->set_scale_factor((float) val);
            return true;
        }});

    props.push_back({"Prev Scale Factor", "DPI Scaling", PropertyType::String,
        wxString::Format("%.2f", dpi->prev_scale_factor()), false, {},
        [dpi]() { return wxString::Format("%.2f", dpi->prev_scale_factor()); },
        [dpi](const wxString& v) {
            double val;
            if (wxSscanf(v, "%lf", &val) != 1) return false;
            dpi->set_prev_scale_factor((float) val);
            return true;
        }});

    props.push_back({"EM Unit", "DPI Scaling", PropertyType::Integer,
        wxString::Format("%d", dpi->em_unit()), false, {},
        [dpi]() { return wxString::Format("%d", dpi->em_unit()); },
        [dpi](const wxString& v) {
            long val;
            if (!v.ToLong(&val)) return false;
            dpi->set_em_unit((int) val);
            return true;
        }});

    props.push_back({"Normal Font", "DPI Scaling", PropertyType::ReadOnly,
        dpi->normal_font().GetNativeFontInfoDesc(), true, {},
        [dpi]() { return dpi->normal_font().GetNativeFontInfoDesc(); },
        nullptr});

    props.push_back({"Force Rescale", "DPI Scaling", PropertyType::Boolean,
        dpi->force_rescale() ? "true" : "false", true, {},
        [dpi]() { return dpi->force_rescale() ? "true" : "false"; },
        nullptr});
}

} // anonymous namespace

wxString DPIAwarePlugin::GetName() const
{
    return "OrcaDPIAware";
}

bool DPIAwarePlugin::CanProvideProperties(wxClassInfo* info)
{
    return info->IsKindOf(CLASSINFO(wxWindow));
}

wxVector<wxInspector::PropertyDef> DPIAwarePlugin::GetProperties(
    wxInspector::InspectableObject& obj)
{
    wxVector<wxInspector::PropertyDef> props;
    wxWindow* win = obj.AsWindow();
    if (!win) return props;

    if (auto* frame = dynamic_cast<DPIFrame*>(win)) {
        addDPIProps(frame, props);
    } else if (auto* dlg = dynamic_cast<DPIDialog*>(win)) {
        addDPIProps(dlg, props);
    }

    return props;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/Utils/wxInspectorPlugins/DPIAwarePlugin.hpp src/slic3r/Utils/wxInspectorPlugins/DPIAwarePlugin.cpp
git commit -m "feat: add DPIAware wxInspector plugin

Exposes DPI scaling properties (scale_factor, prev_scale_factor,
em_unit, normal_font, force_rescale) on DPIFrame and DPIDialog
widgets. Uses dynamic_cast for detection and a template helper
to capture the correct static type for lambda accessors."
```

---

### Task 4: Create CustomWidgetsPlugin

**Files:**
- Create: `src/slic3r/Utils/wxInspectorPlugins/CustomWidgetsPlugin.hpp`
- Create: `src/slic3r/Utils/wxInspectorPlugins/CustomWidgetsPlugin.cpp`

**Interfaces:**
- Consumes: Task 1 (all widget getters), Task 2 (registration pattern)
- Produces: `class CustomWidgetsPlugin : public wxInspector::wxInspectorPlugin`

- [ ] **Step 1: Write CustomWidgetsPlugin.hpp**

```cpp
#pragma once

#include <wx/inspector/plugin.h>

class CustomWidgetsPlugin : public wxInspector::wxInspectorPlugin
{
public:
    wxString GetName() const override;

    bool CanProvideProperties(wxClassInfo* info) override;

    wxVector<wxInspector::PropertyDef> GetProperties(
        wxInspector::InspectableObject& obj) override;

private:
    void addButtonProps(class Button* btn,
        wxVector<wxInspector::PropertyDef>& props);
    void addCheckBoxProps(class CheckBox* cb,
        wxVector<wxInspector::PropertyDef>& props);
    void addTextInputProps(class TextInput* ti,
        wxVector<wxInspector::PropertyDef>& props);
    void addSwitchButtonProps(class SwitchButton* sb,
        wxVector<wxInspector::PropertyDef>& props);
    void addProgressBarProps(class ProgressBar* pb,
        wxVector<wxInspector::PropertyDef>& props);
    void addLabelProps(class Label* lbl,
        wxVector<wxInspector::PropertyDef>& props);
    void addLabeledStaticBoxProps(class LabeledStaticBox* lsb,
        wxVector<wxInspector::PropertyDef>& props);
};
```

- [ ] **Step 2: Write CustomWidgetsPlugin.cpp — includes and GetName/CanProvideProperties**

```cpp
#include "CustomWidgetsPlugin.hpp"

#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "slic3r/GUI/Widgets/SwitchButton.hpp"
#include "slic3r/GUI/Widgets/ProgressBar.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/LabeledStaticBox.hpp"

#include <wx/window.h>
#include <wx/tglbtn.h>

wxString CustomWidgetsPlugin::GetName() const
{
    return "OrcaCustomWidgets";
}

bool CustomWidgetsPlugin::CanProvideProperties(wxClassInfo* info)
{
    return info->IsKindOf(CLASSINFO(wxWindow));
}

wxVector<wxInspector::PropertyDef> CustomWidgetsPlugin::GetProperties(
    wxInspector::InspectableObject& obj)
{
    wxVector<wxInspector::PropertyDef> props;
    wxWindow* win = obj.AsWindow();
    if (!win) return props;

    if (auto* btn = dynamic_cast<Button*>(win))
        addButtonProps(btn, props);
    if (auto* cb = dynamic_cast<CheckBox*>(win))
        addCheckBoxProps(cb, props);
    if (auto* ti = dynamic_cast<TextInput*>(win))
        addTextInputProps(ti, props);
    if (auto* sb = dynamic_cast<SwitchButton*>(win))
        addSwitchButtonProps(sb, props);
    if (auto* pb = dynamic_cast<ProgressBar*>(win))
        addProgressBarProps(pb, props);
    if (auto* lbl = dynamic_cast<Label*>(win))
        addLabelProps(lbl, props);
    if (auto* lsb = dynamic_cast<LabeledStaticBox*>(win))
        addLabeledStaticBoxProps(lsb, props);

    return props;
}
```

- [ ] **Step 3: Write CustomWidgetsPlugin.cpp — addButtonProps**

```cpp
void CustomWidgetsPlugin::addButtonProps(Button* btn,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    wxVector<wxString> styleChoices;
    styleChoices.push_back("Regular");
    styleChoices.push_back("Confirm");
    styleChoices.push_back("Alert");
    styleChoices.push_back("Disabled");

    auto styleToStr = [](ButtonStyle s) -> wxString {
        switch (s) {
            case ButtonStyle::Regular:  return "Regular";
            case ButtonStyle::Confirm:  return "Confirm";
            case ButtonStyle::Alert:    return "Alert";
            case ButtonStyle::Disabled: return "Disabled";
        }
        return "Regular";
    };

    props.push_back({"Button Style", "Orca Button", PropertyType::Choice,
        styleToStr(btn->GetStyle()), false, styleChoices,
        [btn, styleToStr]() { return styleToStr(btn->GetStyle()); },
        [btn](const wxString& v) {
            ButtonStyle s = ButtonStyle::Regular;
            if (v == "Confirm")       s = ButtonStyle::Confirm;
            else if (v == "Alert")    s = ButtonStyle::Alert;
            else if (v == "Disabled") s = ButtonStyle::Disabled;
            btn->SetStyle(s, btn->GetType());
            return true;
        }});

    wxVector<wxString> typeChoices;
    typeChoices.push_back("Compact");
    typeChoices.push_back("Window");
    typeChoices.push_back("Choice");
    typeChoices.push_back("Parameter");
    typeChoices.push_back("Icon");
    typeChoices.push_back("Expanded");

    auto typeToStr = [](ButtonType t) -> wxString {
        switch (t) {
            case ButtonType::Compact:   return "Compact";
            case ButtonType::Window:    return "Window";
            case ButtonType::Choice:    return "Choice";
            case ButtonType::Parameter: return "Parameter";
            case ButtonType::Icon:      return "Icon";
            case ButtonType::Expanded:  return "Expanded";
        }
        return "Compact";
    };

    props.push_back({"Button Type", "Orca Button", PropertyType::Choice,
        typeToStr(btn->GetType()), false, typeChoices,
        [btn, typeToStr]() { return typeToStr(btn->GetType()); },
        [btn](const wxString& v) {
            ButtonType t = ButtonType::Compact;
            if (v == "Window")       t = ButtonType::Window;
            else if (v == "Choice")    t = ButtonType::Choice;
            else if (v == "Parameter") t = ButtonType::Parameter;
            else if (v == "Icon")      t = ButtonType::Icon;
            else if (v == "Expanded")  t = ButtonType::Expanded;
            btn->SetStyle(btn->GetStyle(), t);
            return true;
        }});

    props.push_back({"Selected", "Orca Button", PropertyType::Boolean,
        btn->IsSelected() ? "true" : "false", false, {},
        [btn]() { return btn->IsSelected() ? "true" : "false"; },
        [btn](const wxString& v) {
            btn->SetSelected(v == "true");
            btn->Refresh();
            return true;
        }});
}
```

- [ ] **Step 4: Write CustomWidgetsPlugin.cpp — addCheckBoxProps**

```cpp
void CustomWidgetsPlugin::addCheckBoxProps(CheckBox* cb,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    props.push_back({"Half Checked", "Orca CheckBox", PropertyType::Boolean,
        cb->IsHalfChecked() ? "true" : "false", false, {},
        [cb]() { return cb->IsHalfChecked() ? "true" : "false"; },
        [cb](const wxString& v) {
            cb->SetHalfChecked(v == "true");
            return true;
        }});
}
```

- [ ] **Step 5: Write CustomWidgetsPlugin.cpp — addTextInputProps**

```cpp
void CustomWidgetsPlugin::addTextInputProps(TextInput* ti,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    props.push_back({"Label", "Orca TextInput", PropertyType::String,
        ti->GetLabel(), false, {},
        [ti]() { return ti->GetLabel(); },
        [ti](const wxString& v) { ti->SetLabel(v); return true; }});

    props.push_back({"Text Value", "Orca TextInput", PropertyType::String,
        ti->GetTextCtrl()->GetValue(), false, {},
        [ti]() { return ti->GetTextCtrl()->GetValue(); },
        [ti](const wxString& v) { ti->GetTextCtrl()->SetValue(v); return true; }});

    props.push_back({"Corner Radius", "Orca TextInput", PropertyType::Integer,
        wxString::Format("%d", ti->GetCornerRadius()), false, {},
        [ti]() { return wxString::Format("%d", ti->GetCornerRadius()); },
        [ti](const wxString& v) {
            long val;
            if (!v.ToLong(&val)) return false;
            ti->SetCornerRadius((double) val);
            ti->Refresh();
            return true;
        }});
}
```

- [ ] **Step 6: Write CustomWidgetsPlugin.cpp — addSwitchButtonProps**

```cpp
void CustomWidgetsPlugin::addSwitchButtonProps(SwitchButton* sb,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    props.push_back({"Value", "Orca SwitchButton", PropertyType::Boolean,
        sb->GetValue() ? "true" : "false", false, {},
        [sb]() { return sb->GetValue() ? "true" : "false"; },
        [sb](const wxString& v) {
            sb->SetValue(v == "true");
            return true;
        }});
}
```

(Note: `GetValue()` and `SetValue()` are inherited from `wxBitmapToggleButton` → `wxToggleButton`.)

- [ ] **Step 7: Write CustomWidgetsPlugin.cpp — addProgressBarProps**

```cpp
void CustomWidgetsPlugin::addProgressBarProps(ProgressBar* pb,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    props.push_back({"Proportion", "Orca ProgressBar", PropertyType::String,
        wxString::Format("%.2f", pb->m_proportion), false, {},
        [pb]() { return wxString::Format("%.2f", pb->m_proportion); },
        [pb](const wxString& v) {
            double val;
            if (wxSscanf(v, "%lf", &val) != 1) return false;
            pb->m_proportion = val;
            pb->Refresh();
            return true;
        }});

    props.push_back({"Show Number", "Orca ProgressBar", PropertyType::Boolean,
        pb->m_shownumber ? "true" : "false", false, {},
        [pb]() { return pb->m_shownumber ? "true" : "false"; },
        [pb](const wxString& v) {
            pb->m_shownumber = (v == "true");
            pb->Refresh();
            return true;
        }});
}
```

(Note: `m_proportion` and `m_shownumber` are public members on `ProgressBar`.)

- [ ] **Step 8: Write CustomWidgetsPlugin.cpp — addLabelProps**

```cpp
void CustomWidgetsPlugin::addLabelProps(Label* lbl,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    bool isHyperlink = (lbl->GetWindowStyleFlag() & 0x0020) != 0; // LB_HYPERLINK

    props.push_back({"Is Hyperlink", "Orca Label", PropertyType::Boolean,
        isHyperlink ? "true" : "false", true, {},
        [lbl]() {
            return (lbl->GetWindowStyleFlag() & 0x0020) ? "true" : "false";
        },
        nullptr});

    props.push_back({"Font Point Size", "Orca Label", PropertyType::ReadOnly,
        wxString::Format("%d", lbl->GetFont().GetPointSize()), true, {},
        [lbl]() {
            return wxString::Format("%d", lbl->GetFont().GetPointSize());
        },
        nullptr});
}
```

- [ ] **Step 9: Write CustomWidgetsPlugin.cpp — addLabeledStaticBoxProps**

```cpp
void CustomWidgetsPlugin::addLabeledStaticBoxProps(LabeledStaticBox* lsb,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    props.push_back({"Corner Radius", "LabeledStaticBox", PropertyType::Integer,
        wxString::Format("%d", lsb->GetCornerRadius()), false, {},
        [lsb]() { return wxString::Format("%d", lsb->GetCornerRadius()); },
        [lsb](const wxString& v) {
            long val;
            if (!v.ToLong(&val)) return false;
            lsb->SetCornerRadius((int) val);
            return true;
        }});

    props.push_back({"Border Width", "LabeledStaticBox", PropertyType::Integer,
        wxString::Format("%d", lsb->GetBorderWidth()), false, {},
        [lsb]() { return wxString::Format("%d", lsb->GetBorderWidth()); },
        [lsb](const wxString& v) {
            long val;
            if (!v.ToLong(&val)) return false;
            lsb->SetBorderWidth((int) val);
            return true;
        }});

    // Border Color: display as hex string
    wxColour bc = lsb->GetBorderColor().colorForStates(0);
    props.push_back({"Border Color", "LabeledStaticBox", PropertyType::String,
        bc.GetAsString(wxC2S_HTML_SYNTAX), false, {},
        [lsb]() {
            return lsb->GetBorderColor()
                .colorForStates(0)
                .GetAsString(wxC2S_HTML_SYNTAX);
        },
        [lsb](const wxString& v) {
            wxColour c(v);
            if (!c.IsOk()) return false;
            lsb->SetBorderColor(StateColor(c));
            return true;
        }});

    props.push_back({"Scale", "LabeledStaticBox", PropertyType::ReadOnly,
        wxString::Format("%.2f", lsb->GetScale()), true, {},
        [lsb]() { return wxString::Format("%.2f", lsb->GetScale()); },
        nullptr});
}
```

- [ ] **Step 10: Commit**

```bash
git add src/slic3r/Utils/wxInspectorPlugins/CustomWidgetsPlugin.hpp src/slic3r/Utils/wxInspectorPlugins/CustomWidgetsPlugin.cpp
git commit -m "feat: add OrcaCustomWidgets wxInspector plugin

Exposes Orca-specific properties on 7 widget types:
- Button: Style, Type, Selected
- CheckBox: Half Checked
- TextInput: Label, Text Value, Corner Radius
- SwitchButton: Value
- ProgressBar: Proportion, Show Number
- Label: Is Hyperlink, Font Point Size
- LabeledStaticBox: Corner Radius, Border Width, Border Color, Scale

Each widget type uses dynamic_cast for safe detection."
```

---

### Task 5: Wire plugins into MainFrame and CMakeLists

**Files:**
- Modify: `src/slic3r/GUI/MainFrame.cpp` (add include + registration call)
- Modify: `src/slic3r/CMakeLists.txt` (add 4 source files)

**Interfaces:**
- Consumes: Tasks 1-4 (all plugins and registration helper)
- Produces: Registered plugins available at runtime, buildable project

- [ ] **Step 1: Add include in MainFrame.cpp**

After the existing includes (around line 30, near the other Utils includes), add:

```cpp
#include "slic3r/Utils/wxInspectorPlugins/Registration.hpp"
```

- [ ] **Step 2: Add registration call in MainFrame constructor**

After `SetupInspectorAccelerator(this);` (currently line ~303), add:

```cpp
RegisterOrcaInspectorPlugins();
```

- [ ] **Step 3: Add source files to CMakeLists.txt**

Find the `SLIC3R_GUI_SOURCES` list in `src/slic3r/CMakeLists.txt`. After the existing `Utils/*.cpp` entries (around line 650-754), add:

```cmake
Utils/wxInspectorPlugins/DPIAwarePlugin.hpp
Utils/wxInspectorPlugins/DPIAwarePlugin.cpp
Utils/wxInspectorPlugins/CustomWidgetsPlugin.hpp
Utils/wxInspectorPlugins/CustomWidgetsPlugin.cpp
Utils/wxInspectorPlugins/Registration.hpp
```

(Note: Add all 5 files — 2 .hpp + 2 .cpp + 1 Registration.hpp. wxWidgets cmake needs headers listed too for the resource system.)

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/MainFrame.cpp src/slic3r/CMakeLists.txt
git commit -m "feat: wire wxInspector plugins into MainFrame and build

- Call RegisterOrcaInspectorPlugins() after SetupInspectorAccelerator
- Add all plugin source files to SLIC3R_GUI_SOURCES"
```

---

### Task 6: Build and verify

**Files:**
- None modified (verification only)

- [ ] **Step 1: Configure the build**

```powershell
$cmakePath = "D:\VisualStudio\2026\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmakePath --build . --config Debug --target ALL_BUILD -- -m
```

Expected: Build succeeds with zero errors and zero warnings from our new files.

- [ ] **Step 2: Fix any compilation errors**

If the build fails:
- Check that `#include` paths resolve (the `slic3r/GUI/…` relative paths use `src/` as the include root — verify this is set up in CMake via `include_directories`)
- Check that `ButtonStyle` and `ButtonType` enums are visible (they're defined in `Button.hpp`)
- Check that `StateColor` constructor from `wxColour` is valid (it has `StateColor(wxColour const&)`)
- Check that `LabeledStaticBox::GetBorderColor()` returns by value (StateColor copy is fine)
- On macOS: static box margin removal call needs `#ifdef __WXOSX__` guard

- [ ] **Step 3: Launch OrcaSlicer and verify inspector**

Launch the built OrcaSlicer, press Ctrl+Shift+I to open the inspector:
1. Select the MainFrame in the tree — verify "DPI Scaling" category appears with Scale Factor, Prev Scale Factor, EM Unit, Normal Font, Force Rescale
2. Select an Orca Button — verify "Orca Button" category appears
3. Select an Orca CheckBox — verify "Orca CheckBox" category appears
4. Edit a property value (e.g., Scale Factor) — verify the setter applies correctly
5. Select a LabeledStaticBox — verify corner radius, border width, border color, scale appear

- [ ] **Step 5: Commit (if fixes were needed) or mark complete**

```bash
git status
```

If clean: verification complete. If changes were made: `git add` and commit with fix message.
