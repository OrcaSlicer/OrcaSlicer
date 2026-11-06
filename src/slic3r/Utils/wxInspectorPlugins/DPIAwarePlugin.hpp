#pragma once

#include <wx/inspector/object.h>
#include <wx/inspector/plugin.h>
#include <wx/string.h>
#include <wx/rtti.h>
#include <wx/vector.h>

class wxClassInfo;
namespace wxInspector { class InspectableObject; }

class DPIAwarePlugin : public wxInspector::wxInspectorPlugin
{
public:
    wxString GetName() const override;

    bool CanProvideProperties(wxClassInfo* info) override;

    wxVector<wxInspector::PropertyDef> GetProperties(
        wxInspector::InspectableObject& obj) override;
};
