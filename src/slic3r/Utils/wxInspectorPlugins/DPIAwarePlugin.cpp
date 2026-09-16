#include "DPIAwarePlugin.hpp"

#include "slic3r/GUI/GUI_Utils.hpp" // DPIFrame, DPIDialog, DPIAware<P>

#include <wx/window.h>
#include <wx/crt.h>

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

    if (auto* frame = dynamic_cast<Slic3r::GUI::DPIFrame*>(win)) {
        addDPIProps(frame, props);
    } else if (auto* dlg = dynamic_cast<Slic3r::GUI::DPIDialog*>(win)) {
        addDPIProps(dlg, props);
    }

    return props;
}
