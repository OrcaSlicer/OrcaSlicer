/**********************************************************
* File: uiAmsHumidityPopup.h
* Description: The popup with DevAms Humidity
*
*  \n class uiAmsHumidityPopup
**********************************************************/

#pragma once
#include "slic3r/GUI/Widgets/AMSItem.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/PopupWindow.hpp"

#include "slic3r/GUI/wxExtensions.hpp"

//Previous defintions
class wxGrid;

namespace Slic3r { namespace GUI {

struct uiAmsHumidityInfo
{
    std::string ams_id;
    AMSModel ams_type;
    int humidity_display_idx = -1;
    int humidity_percent = -1;
    float current_temperature;
    int left_dry_time = -1;
};

/// </summary>
/// Note: The popup of DevAms Humidity with percentage and dry time
/// Author: xin.zhang
/// </summary>
class uiAmsPercentHumidityDryPopup : public wxDialog
{
public:
    uiAmsPercentHumidityDryPopup(wxWindow *parent);
    ~uiAmsPercentHumidityDryPopup() = default;

public:
    void Update(uiAmsHumidityInfo *info) { m_ams_id = info->ams_id; Update(info->humidity_display_idx, info->humidity_percent, info->left_dry_time, info->current_temperature); };

    std::string get_owner_ams_id() const { return m_ams_id; }

    void msw_rescale();

private:
    void Update(int humidiy_level, int humidity_percent, int left_dry_time, float current_temperature);
    void UpdateContents();

    void Create();

private:
    /*owner ams id*/
    std::string m_ams_id;

    int m_humidity_level   = 0;
    int m_humidity_percent = 0;
    int m_left_dry_time    = 0;
    float m_current_temperature = 0;

    // Bitmap
    ScalableBitmap drying_img;
    ScalableBitmap idle_img;

    // Widgets
    wxStaticBitmap* m_humidity_img;

    wxStaticBitmap* m_dry_state_img;
    Label*          m_dry_state;

    Label* m_humidity_header;
    Label* m_humidity_label;

    Label* m_temperature_header;
    Label* m_temperature_label;

    Label* left_dry_time_header;
    Label* left_dry_time_label;

    wxSizer*       m_sizer;
};

}} // namespace Slic3r::GUI

