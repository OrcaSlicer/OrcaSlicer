#pragma once

#include <unordered_set>
#include <wx/statbmp.h>
#include <wx/webrequest.h>

#include "GUI_Utils.hpp"
#include "Widgets/StateColor.hpp"
#include <nlohmann/json.hpp>

class Label;
class Button;

namespace Slic3r {

class MachineObject;//Previous definitions

namespace GUI {

class DeviceErrorDialog : public DPIDialog
{
public:
    enum ActionButton : int {
        RESUME_PRINTING = 2,
        RESUME_PRINTING_DEFECTS = 3,
        RESUME_PRINTING_PROBELM_SOLVED = 4,
        STOP_PRINTING = 5,
        CHECK_ASSISTANT = 6,
        FILAMENT_EXTRUDED = 7,
        RETRY_FILAMENT_EXTRUDED = 8,
        CONTINUE = 9,
        LOAD_VIRTUAL_TRAY = 10,
        OK_BUTTON = 11,
        FILAMENT_LOAD_RESUME = 12,
        JUMP_TO_LIVEVIEW,

        NO_REMINDER_NEXT_TIME = 23,
        IGNORE_NO_REMINDER_NEXT_TIME = 25,
        //LOAD_FILAMENT = 26*/
        IGNORE_RESUME = 27,
        PROBLEM_SOLVED_RESUME = 28,
        TURN_OFF_FIRE_ALARM = 29,

        RETRY_PROBLEM_SOLVED = 34,
        STOP_DRYING = 35,
        CANCLE = 37,
        REMOVE_CLOSE_BTN = 39, // special case, do not show close button
        PROCEED = 41,

        ERROR_BUTTON_COUNT,

        // old error code to pseudo action
        DBL_CHECK_CANCEL = 10000,
        DBL_CHECK_DONE = 10001,
        DBL_CHECK_RETRY = 10002,
        DBL_CHECK_RESUME = 10003,
        DBL_CHECK_OK = 10004,
    };
    /* action params json */
    nlohmann::json m_action_json;

public:
    DeviceErrorDialog(MachineObject* obj,
                      wxWindow* parent,
                      wxWindowID  id = wxID_ANY,
                      const wxString& title = wxEmptyString,
                      const wxPoint& pos = wxDefaultPosition,
                      const wxSize& size = wxDefaultSize,
                      long  style = wxCLOSE_BOX | wxCAPTION);
    ~DeviceErrorDialog();

public:
    wxString show_error_code(int error_code);
    void     set_action_json(const nlohmann::json &action_json) { m_action_json = action_json; }

protected:
    void init_button_list();
    void init_button(ActionButton style, wxString buton_text);

    wxString parse_error_level(int error_code);
    std::vector<int> convert_to_pseudo_buttons(std::string error_str);

    void update_contents(const wxString& title, const wxString& text, const wxString& error_code,const wxString& image_url, const std::vector<int>& btns);

    void on_button_click(ActionButton btn_id);
    void on_webrequest_state(wxWebRequestEvent& evt);
    void on_dpi_changed(const wxRect& suggested_rect);

private:
    MachineObject* m_obj;

    int m_error_code;
    std::unordered_set<Button*> m_used_button;

    wxWebRequest web_request;
    wxStaticBitmap* m_error_picture;
    Label* m_error_msg_label{ nullptr };
    Label* m_error_code_label{ nullptr };
    wxBoxSizer* m_sizer_main;
    wxBoxSizer* m_sizer_button;
    wxScrolledWindow* m_scroll_area{ nullptr };

    std::map<int, Button*> m_button_list;
    StateColor btn_bg_white;
};
}} // namespace Slic3r::GUI
