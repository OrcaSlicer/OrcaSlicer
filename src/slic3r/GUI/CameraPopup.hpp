#ifndef slic3r_CameraPopup_hpp_
#define slic3r_CameraPopup_hpp_

#include "slic3r/GUI/MonitorBasePanel.h"
#include "DeviceManager.hpp"
#include "GUI.hpp"
#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/timer.h>
#include <wx/sizer.h>
#include <wx/gbsizer.h>
#include <wx/webrequest.h>
#include <wx/hyperlink.h>
#include "Widgets/SwitchButton.hpp"
#include "Widgets/RadioBox.hpp"
#include "GUI_Utils.hpp"

class Label;

namespace Slic3r {
namespace GUI {

wxDECLARE_EVENT(EVT_VCAMERA_SWITCH, wxMouseEvent);
wxDECLARE_EVENT(EVT_SDCARD_ABSENT_HINT, wxCommandEvent);
wxDECLARE_EVENT(EVT_CAM_SOURCE_CHANGE, wxCommandEvent);

class CameraPopup : public DPIDialog
{
public:
    CameraPopup(wxWindow *parent);
    virtual ~CameraPopup() {}

    void sync_vcamera_state(bool show_vcamera);
    void check_func_supported(MachineObject* obj);
    void update(bool vcamera_streaming);

    enum CameraResolution
    {
        RESOLUTION_720P = 0,
        RESOLUTION_1080P = 1,
        RESOLUTION_OPTIONS_NUM = 2
    };

    void rescale();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_switch_recording(wxCommandEvent& event);
    void on_set_resolution();
    void sdcard_absent_hint();
    void on_custom_camera_switch_toggled(wxCommandEvent& event);
    void update_custom_camera_switch();
    void on_manage_cameras_clicked(wxMouseEvent& event);

    wxWindow *  create_item_radiobox(wxString title, wxWindow *parent, wxString tooltip, int padding_left);
    void select_curr_radiobox(int btn_idx);
    void sync_resolution_setting(std::string resolution);
    void reset_resolution_setting();
    wxString to_resolution_label_string(CameraResolution resolution);
    std::string to_resolution_msg_string(CameraResolution resolution);

private:
    MachineObject* m_obj { nullptr };
    wxStaticText* m_text_recording;
    SwitchButton* m_switch_recording;
    wxStaticText* m_text_vcamera;
    SwitchButton* m_switch_vcamera;
    wxStaticText* m_text_liveview_retry;
    SwitchButton* m_switch_liveview_retry;
    wxStaticText* m_custom_camera_hint;
    SwitchButton* m_switch_custom_camera;
    Label* m_manage_cameras_link{nullptr};
    wxStaticText* m_text_resolution;
    wxWindow* m_resolution_options[RESOLUTION_OPTIONS_NUM];
    wxScrolledWindow *m_panel;
    wxBoxSizer* main_sizer;
    std::vector<RadioBox*> resolution_rbtns;
    std::vector<wxStaticText*> resolution_texts;
    CameraResolution curr_sel_resolution = RESOLUTION_1080P;
    Label* vcamera_guide_link { nullptr };
    wxPanel* link_underline{ nullptr };
    bool is_vcamera_show = false;
    bool allow_alter_resolution = false;
};


class CameraItem : public wxPanel
{
public:
    CameraItem(wxWindow *parent, std::string normal, std::string hover);
    ~CameraItem();

    MachineObject *m_obj{nullptr};
    bool     m_hover{false};
    ScalableBitmap m_bitmap_normal;
    ScalableBitmap m_bitmap_hover;

    void msw_rescale();
    void on_enter_win(wxMouseEvent &evt);
    void on_level_win(wxMouseEvent &evt);
    void paintEvent(wxPaintEvent &evt);
    void render(wxDC &dc);
    void doRender(wxDC &dc);
};

}
}
#endif
