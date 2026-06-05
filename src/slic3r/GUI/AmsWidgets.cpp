#include "AmsWidgets.hpp"
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/dataview.h>

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "format.hpp"

#include "DeviceCore/DevFilaSystem.h"


namespace Slic3r {
namespace GUI {

TrayListModel::TrayListModel() :
    wxDataViewVirtualListModel(0)
{
    ;
}

void TrayListModel::GetValueByRow(wxVariant& variant,
    unsigned int row, unsigned int col) const
{
    switch (col) {
    case Col_TrayTitle:
        if (row >= m_titleColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_titleColValues[row];
        break;
    case Col_TrayColor:
        if (row >= m_colorColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_colorColValues[row];
        break;
    case Col_TrayMeterial:
        if (row >= m_meterialColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_meterialColValues[row];
        break;
    case Col_TrayWeight:
        if (row >= m_weightColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_weightColValues[row];
        break;
    case Col_TrayDiameter:
        if (row >= m_diameterColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_diameterColValues[row];
        break;
    case Col_TrayTime:
        if (row >= m_timeColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_timeColValues[row];
        break;
    case Col_TraySN:
        if (row >= m_snColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_snColValues[row];
        break;
    case Col_TrayManufacturer:
        if (row >= m_manufacturerColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_manufacturerColValues[row];
        break;
    case Col_TraySaturability:
        if (row >= m_saturabilityColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_saturabilityColValues[row];
        break;
    case Col_TrayTransmittance:
        if (row >= m_transmittanceColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_transmittanceColValues[row];
        break;
    case Col_TraySmooth:
        if (row >= m_smoothColValues.GetCount())
            variant = wxString::Format("N/A", row);
        else
            variant = m_smoothColValues[row];
        break;
    default:
        break;
    }
}

bool TrayListModel::GetAttrByRow(unsigned int row, unsigned int col,
    wxDataViewItemAttr& attr) const
{
    return true;
}

bool TrayListModel::SetValueByRow(const wxVariant& variant,
    unsigned int row, unsigned int col)
{
    switch (col)
    {
    case Col_TrayTitle:
    case Col_TrayColor:
    case Col_TrayMeterial:
    case Col_TrayWeight:
    case Col_TrayDiameter:
    case Col_TrayTime:
    case Col_TraySN:
    case Col_TrayManufacturer:
    case Col_TraySaturability:
    case Col_TrayTransmittance:
    case Col_TraySmooth:
        return true;
    default:
        break;
    }
    return false;
}

void TrayListModel::update(MachineObject* obj)
{
    if (!obj) return;

    m_titleColValues.clear();
    m_colorColValues.clear();
    m_meterialColValues.clear();
    m_weightColValues.clear();
    m_diameterColValues.clear();
    m_timeColValues.clear();
    m_snColValues.clear();
    m_manufacturerColValues.clear();
    m_saturabilityColValues.clear();
    m_transmittanceColValues.clear();

    std::map<std::string, DevAms*>::iterator ams_it;
    std::map<std::string, DevAmsTray*>::const_iterator tray_it;
    int tray_index = 0;

    const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
    for (auto ams_it = ams_list.begin(); ams_it != ams_list.end(); ams_it++)
    {
        if (ams_it->second) {
            for (tray_it = ams_it->second->GetTrays().cbegin(); tray_it != ams_it->second->GetTrays().cend(); tray_it++) {
                DevAmsTray* tray = tray_it->second;
                if (tray) {
                    tray_index++;
                    wxString title_text = wxString::Format("tray %s(ams %s)", tray->id, ams_it->second->GetAmsId());
                    m_titleColValues.push_back(title_text);
                    wxString color_text = wxString::Format("%s", tray->wx_color.GetAsString());
                    m_colorColValues.push_back(color_text);
                    wxString meterial_text = wxString::Format("%s", tray->m_fila_type);
                    m_meterialColValues.push_back(meterial_text);
                    wxString weight_text = wxString::Format("%sg", tray->weight);
                    m_weightColValues.push_back(weight_text);
                    wxString diameter_text = wxString::Format("%0.2f", tray->diameter);
                    m_diameterColValues.push_back(diameter_text);
                    wxString time_text = wxString::Format("%s", tray->time);
                    m_timeColValues.push_back(time_text);
                    wxString sn_text = wxString::Format("%s", tray->uuid);
                    m_snColValues.push_back(sn_text);
                    wxString manufacturer_text = wxString::Format("%s", tray->sub_brands);
                    m_manufacturerColValues.push_back(manufacturer_text);
                    // TODO: 
                    //wxString saturability_text = wxString::Format("%s", tray->saturability);
                    //m_saturabilityColValues.push_back(saturability_text);
                    //wxString transmittance_text = wxString::Format("%s", tray->transmittance);
                    //m_transmittanceColValues.push_back(transmittance_text);
                    //wxString smooth_text = wxString::Format("%s", tray->smooth);
                    //m_smoothColValues.push_back(smooth_text);
                }
            }
        }
    }

    Reset(m_titleColValues.GetCount());
}
void TrayListModel::clear_data()
{
    m_titleColValues.clear();
    m_colorColValues.clear();
    m_meterialColValues.clear();
    m_weightColValues.clear();
    m_diameterColValues.clear();
    m_timeColValues.clear();
    m_snColValues.clear();
    m_manufacturerColValues.clear();
    m_saturabilityColValues.clear();
    m_transmittanceColValues.clear();
    m_smoothColValues.clear();

    Reset(0);
}

} // GUI
} // Slic3r
