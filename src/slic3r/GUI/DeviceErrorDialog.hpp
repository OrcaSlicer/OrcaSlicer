#pragma once

#include <unordered_set>
#include <atomic>
#include <memory>
#include <wx/statbmp.h>
#include <wx/timer.h>
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
        REFRESH_NOZZLE = 24,
        IGNORE_NO_REMINDER_NEXT_TIME = 25,
        //LOAD_FILAMENT = 26*/
        IGNORE_RESUME = 27,
        PROBLEM_SOLVED_RESUME = 28,
        TURN_OFF_FIRE_ALARM = 29,

        RETRY_PROBLEM_SOLVED = 34,
        STOP_DRYING = 35,
        CANCEL = 37,
        REMOVE_CLOSE_BTN = 39, // special case, do not show close button
        PROCEED = 41,
        OK_JUMP_RACK = 49,
        ABORT = 51,
        DISABLE_PURIFICATION = 54,
        DONT_REMIND_NEXT_TIME = 57,

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
    void on_request_timeout(wxTimerEvent& event);
    void clear_request_timer();
    wxBitmap make_placeholder_image(const wxString& text);
    bool get_fail_snapshot_from_cloud();
    bool get_fail_snapshot_from_local(const wxString& image_url);
    void on_dpi_changed(const wxRect& suggested_rect);

private:
    MachineObject* m_obj;

    int m_error_code;
    std::unordered_set<Button*> m_used_button;

    wxWebRequest web_request;
    wxTimer* m_request_timer{ nullptr };
    std::atomic<bool> m_request_cancelled{ false };
    // UI-thread-only generation counter: bumped on every update_contents() so a cloud
    // snapshot callback from a previous error cannot paint over the current one.
    int m_request_seq{ 0 };
    wxString m_local_img_url;
    // Orca: liveness token for the async cloud snapshot callback (the request has no cancel handle)
    std::shared_ptr<std::atomic_bool> m_alive{ std::make_shared<std::atomic_bool>(true) };
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
