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
