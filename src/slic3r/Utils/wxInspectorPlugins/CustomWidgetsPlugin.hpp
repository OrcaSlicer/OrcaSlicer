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
