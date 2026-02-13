#ifndef slic3r_GUI_FANCONTROL_hpp_
#define slic3r_GUI_FANCONTROL_hpp_

#include "../wxExtensions.hpp"
#include "StaticBox.hpp"
#include "StepCtrl.hpp"
#include "Button.hpp"
#include "PopupWindow.hpp"
#include "../SelectMachine.hpp"
#include "../DeviceManager.hpp"
#include "slic3r/GUI/Event.hpp"
#include <wx/simplebook.h>
#include <wx/hyperlink.h>
#include <wx/animate.h>
#include <wx/dynarray.h>
#include "../DeviceCore/DevFan.h"

namespace Slic3r {
namespace GUI {


/*************************************************
Description:Fan
**************************************************/
#define SIZE_OF_FAN_OPERATE wxSize(154, 28)

#define DRAW_TEXT_COLOUR wxColour(0x898989)
#define DRAW_HEAD_TEXT_COLOUR wxColour(0x262e30)
#define DRAW_OPERATE_LINE_COLOUR wxColour(0xDEDEDE)

enum FanControlType
{
    PART_FAN = 0,
    AUX_FAN,
    EXHAUST_FAN,
    FILTER_FAN,
    CHAMBER_FAN,
    TOP_FAN
};

struct RotateOffSet
{
    float rotate;
    wxPoint offset;
};

class Fan : public wxWindow
{
public:
    Fan(wxWindow* parent, wxWindowID id, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    ~Fan() {};
    void    post_event(wxCommandEvent&& event);
    void    paintEvent(wxPaintEvent& evt);
    void    render(wxDC& dc);
    void    doRender(wxDC& dc);
    void    msw_rescale();
    void    create(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size);
    void    set_fan_speeds(int g);

private:
    int     m_current_speeds;
    std::vector<RotateOffSet> m_rotate_offsets;

protected:
    std::vector<wxPoint> m_scale_pos_array;

    ScalableBitmap   m_bitmap_bk;
    ScalableBitmap   m_bitmap_scale_0;
    ScalableBitmap   m_bitmap_scale_1;
    ScalableBitmap   m_bitmap_scale_2;
    ScalableBitmap   m_bitmap_scale_3;
    ScalableBitmap   m_bitmap_scale_4;
    ScalableBitmap   m_bitmap_scale_5;
    ScalableBitmap   m_bitmap_scale_6;
    ScalableBitmap   m_bitmap_scale_7;
    ScalableBitmap   m_bitmap_scale_8;
    ScalableBitmap   m_bitmap_scale_9;
    ScalableBitmap   m_bitmap_scale_10;

    std::vector<ScalableBitmap> m_bitmap_scales;

    wxImage          m_img_pointer;

    virtual void     DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);
};

/*************************************************
Description:FanOperate
**************************************************/
class FanOperate : public wxWindow
{
public:
    FanOperate(wxWindow *parent, wxWindowID id, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~FanOperate() {};
    void    post_event(wxCommandEvent&& event);
    void    paintEvent(wxPaintEvent& evt);
    void    render(wxDC& dc);
    void    doRender(wxDC& dc);
    void    msw_rescale();
    void    create(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size);
    void    on_left_down(wxMouseEvent& event);

    void    set_machine_obj(MachineObject *obj);

public:
    void    set_fan_speeds(int g);
    bool    check_printing_state();
    void    add_fan_speeds();
    void    decrease_fan_speeds();
private:
    int     m_current_speeds;
    int     m_target_speed;
    int     m_min_speeds;
    int     m_max_speeds;
    ScalableBitmap   m_bitmap_add;
    ScalableBitmap   m_bitmap_decrease;

    MachineObject* m_obj;
};


/*************************************************
Description:FanControlNew
**************************************************/
class FanControlNew : public wxWindow
{
public:
    FanControlNew(wxWindow *parent, const AirDuctData& fan_data, int mode_id, int part_id, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~FanControlNew() {};

protected:
    MachineObject* m_obj;
    wxStaticText* m_static_name{ nullptr };
    int m_fan_id;

    ScalableBitmap* m_bitmap_fan{ nullptr };
    ScalableBitmap* m_bitmap_toggle_off{ nullptr };
    ScalableBitmap* m_bitmap_toggle_on{ nullptr };

    FanOperate* m_fan_operate{ nullptr };
    bool m_switch_fan{ false };
    bool m_update_already{ false };
    int  m_current_speed{ 0 };
    int  m_part_id{0};
    int  m_mode_id{0};

    wxBoxSizer *m_sizer_control_bottom{ nullptr };
    wxStaticText *m_static_status_name{nullptr};
    int           m_show_mode{0};// 0 - ctrl, 1 - auto, 2 - off

    //Fan and door may use the same mode_id, but they are different, they need to be distinguished by the m_type field
    AirDuctData m_fan_data;
    bool m_new_protocol{ false };

    std::shared_ptr<FanControlNew> token;

public:
    wxStaticBitmap* m_static_bitmap_fan { nullptr};
    wxStaticBitmap* m_switch_button{ nullptr };
    void update_obj_state(bool stat) { m_update_already = stat; };
    void update_fan_data(const AirDuctData& data) { m_fan_data = data; };
    void command_control_fan();
    bool check_printing_state();
    void set_machine_obj(MachineObject *obj);
    void set_name(wxString name);
    void set_mode_id(int id) { m_mode_id = id;}
    void set_part_id(int id){m_part_id = id;};
    void set_fan_speed(int g);
    void set_fan_speed_percent(int speed);
    void set_fan_switch(bool s);
    void post_event();
    void on_swith_fan(wxMouseEvent& evt);
    void on_swith_fan(bool on);
    void update_mode();
    void on_left_down(wxMouseEvent& event);
    void on_mode_change(wxMouseEvent& event);

    void msw_rescale();
};

wxDECLARE_EVENT(EVT_FANCTRL_SWITCH, wxCommandEvent);
class FanControlNewSwitchPanel : public wxWindow
{
    bool  switch_state_on = false;
    wxStaticBitmap* m_switch_btn{ nullptr };
    ScalableBitmap* m_bitmap_toggle_off{ nullptr };
    ScalableBitmap* m_bitmap_toggle_on{ nullptr };

public:
    FanControlNewSwitchPanel(wxWindow* parent, const wxString& title, const wxString& tips, bool on = true);

public:
    bool IsSwitchOn() const { return switch_state_on; }
    void SetSwitchOn(bool on);

private:
    void on_left_down(wxMouseEvent& event);
};


class FanControlPopupNew : public wxDialog
{
public:
    FanControlPopupNew(wxWindow* parent, MachineObject* obj, const AirDuctData& data);
    ~FanControlPopupNew() {};


private:
    wxBoxSizer* m_sizer_main{ nullptr };

    //new protocol
    wxGridSizer* m_radio_btn_sizer{ nullptr };
    wxGridSizer* m_sizer_fanControl { nullptr };

    wxBoxSizer *m_mode_sizer{ nullptr };
    wxBoxSizer *m_bottom_sizer{ nullptr };

    // mode switch buttons
    std::unordered_map<int, SendModeSwitchButton*> m_mode_switch_btns; //<mode_id, SendModeSwitchButton>

    // mode text
    Label* m_mode_text;

    // submodes
    // cooling submode : filter
    wxPanel*              m_sub_mode_panel{ nullptr };
    wxBoxSizer*           m_sub_mode_sizer{ nullptr };
    FanControlNewSwitchPanel* m_cooling_filter_switch_panel{ nullptr };

    // The fan operates
    std::map<int, FanControlNew*> m_fan_control_list; //<duct_id, <fan_id, FanControl>>

    // The object
    MachineObject *m_obj{nullptr};
    AirDuctData    m_data;
    int            m_air_duct_time_out{ 0 };
    int            m_fan_set_time_out{ 0 };

    std::map<AIR_DUCT, wxString> radio_btn_name;
    std::map<AIR_DOOR, wxString> air_door_func_name;
    std::map<AIR_DUCT, wxString> label_text;

private:
    void  init_names(MachineObject* obj);
    wxString get_fan_func_name(int mode, int submode, AIR_FUN func) const;

    void  CreateDuct();
          
    void  UpdateParts();
    void  UpdatePartSubMode();
          
    void  update_fan_data(const AirDuctData& data);
    void  update_fan_data(AIR_FUN id, int speed);

    void  on_mode_changed(const wxMouseEvent& event);
    void  on_fan_changed(const wxCommandEvent& event);
    void  on_left_down(wxMouseEvent& evt);
    void  post_event(int fan_type, wxString speed);

    void  on_show(wxShowEvent& evt);
    void  paintEvent(wxPaintEvent& evt);

    void  command_control_air_duct(int mode_id, int submode = -1);

public:
    void  update_fan_data(MachineObject *obj);
    void  msw_rescale();
};

wxDECLARE_EVENT(EVT_FAN_SWITCH_ON, wxCommandEvent);
wxDECLARE_EVENT(EVT_FAN_SWITCH_OFF, wxCommandEvent);
wxDECLARE_EVENT(EVT_FAN_ADD, wxCommandEvent);
wxDECLARE_EVENT(EVT_FAN_DEC, wxCommandEvent);
wxDECLARE_EVENT(EVT_FAN_CHANGED, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif
