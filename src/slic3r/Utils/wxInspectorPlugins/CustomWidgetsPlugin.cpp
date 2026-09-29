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

void CustomWidgetsPlugin::addLabelProps(Label* lbl,
    wxVector<wxInspector::PropertyDef>& props)
{
    using namespace wxInspector;

    bool isHyperlink = (lbl->GetWindowStyleFlag() & LB_HYPERLINK) != 0;

    props.push_back({"Is Hyperlink", "Orca Label", PropertyType::Boolean,
        isHyperlink ? "true" : "false", true, {},
        [lbl]() {
            return (lbl->GetWindowStyleFlag() & LB_HYPERLINK) ? "true" : "false";
        },
        nullptr});

    props.push_back({"Font Point Size", "Orca Label", PropertyType::ReadOnly,
        wxString::Format("%d", lbl->GetFont().GetPointSize()), true, {},
        [lbl]() {
            return wxString::Format("%d", lbl->GetFont().GetPointSize());
        },
        nullptr});
}

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
