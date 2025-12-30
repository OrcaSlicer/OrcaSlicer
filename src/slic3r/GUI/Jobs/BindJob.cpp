#include "BindJob.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "slic3r/GUI/DeviceCore/DevManager.h"

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_BIND_UPDATE_MESSAGE, wxCommandEvent);
wxDEFINE_EVENT(EVT_BIND_MACHINE_SUCCESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_BIND_MACHINE_FAIL, wxCommandEvent);


static auto waiting_auth_str = _u8L("Logging in");
static auto login_failed_str = _u8L("Login failed");


BindJob::BindJob(std::string dev_id, std::string dev_ip, std::string sec_link, std::string ssdp_version)
    :
    m_dev_id(dev_id),
    m_dev_ip(dev_ip),
    m_sec_link(sec_link),
    m_ssdp_version(ssdp_version)
{
    ;
}

void BindJob::on_success(std::function<void()> success)
{
    m_success_fun = success;
}

void BindJob::update_status(Ctl &ctl, int st, const std::string &msg)
{
    ctl.update_status(st, msg);
    wxCommandEvent event(EVT_BIND_UPDATE_MESSAGE);
    event.SetString(msg);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

void BindJob::process(Ctl &ctl)
{
    int             result_code = 0;
    std::string     result_info;

    /* display info */
    auto msg = waiting_auth_str;
    int curr_percent = 0;

    NetworkAgent* m_agent = wxGetApp().getAgent();
    if (!m_agent) { return; }

    // get timezone
    wxDateTime::TimeZone tz(wxDateTime::Local);
    long offset = tz.GetOffset();
    std::string timezone = get_timezone_utc_hm(offset);

    m_agent->track_update_property("ssdp_version", m_ssdp_version, "string");
    int result = m_agent->bind(m_dev_ip, m_dev_id, m_sec_link, timezone, m_improved,
        [this, &ctl, &curr_percent, &msg, &result_code, &result_info](int stage, int code, std::string info) {

            result_code = code;
            result_info = info;

            if (stage == BBL::BindJobStage::LoginStageConnect) {
                curr_percent = 15;
                msg = _u8L("Logging in");
            } else if (stage == BBL::BindJobStage::LoginStageLogin) {
                curr_percent = 30;
                msg = _u8L("Logging in");
            } else if (stage == BBL::BindJobStage::LoginStageWaitForLogin) {
                curr_percent = 45;
                msg = _u8L("Logging in");
            } else if (stage == BBL::BindJobStage::LoginStageGetIdentify) {
                curr_percent = 60;
                msg = _u8L("Logging in");
            } else if (stage == BBL::BindJobStage::LoginStageWaitAuth) {
                curr_percent = 80;
                msg = _u8L("Logging in");
            } else if (stage == BBL::BindJobStage::LoginStageFinished) {
                curr_percent = 100;
                msg = _u8L("Logging in");
            } else {
                msg = _u8L("Logging in");
            }

            if (code != 0) {
                msg = _u8L("Login failed");
                if (code == BAMBU_NETWORK_ERR_TIMEOUT) {
                    msg += _u8L("Please check the printer network connection.");
                }
            }
            update_status(ctl, curr_percent, msg);
        }
    );

    if (result < 0) {
        BOOST_LOG_TRIVIAL(info) << "login: result = " << result;

        if (result_code == BAMBU_NETWORK_ERR_BIND_ECODE_LOGIN_REPORT_FAILED || result_code == BAMBU_NETWORK_ERR_BIND_GET_PRINTER_TICKET_TIMEOUT) {
            int         error_code;

            try
            {
                error_code = stoi(result_info);
                wxString error_msg = wxGetApp().get_hms_query()->query_print_error_msg(m_dev_id, error_code);
                result_info = error_msg.ToStdString();
            }
            catch (...) {
                ;
            }
        }

        post_fail_event(result_code, result_info);
        return;
    }

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        BOOST_LOG_TRIVIAL(error) << "login: dev is null";
        post_fail_event(result_code, result_info);
        return;
    }
    dev->update_user_machine_list_info();

     wxCommandEvent event(EVT_BIND_MACHINE_SUCCESS);
     event.SetEventObject(m_event_handle);
     wxPostEvent(m_event_handle, event);
    return;
}

void BindJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    try {
        if (eptr)
            std::rethrow_exception(eptr);
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    if (canceled || eptr)
        return;
}

void BindJob::set_event_handle(wxWindow *hanle)
{
    m_event_handle = hanle;
}

void BindJob::post_fail_event(int code, std::string info)
{
    wxCommandEvent event(EVT_BIND_MACHINE_FAIL);
    event.SetInt(code);
    event.SetString(info);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

}} // namespace Slic3r::GUI
