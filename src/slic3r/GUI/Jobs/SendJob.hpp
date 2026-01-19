#ifndef SendJOB_HPP
#define SendJOB_HPP

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include "slic3r/GUI/DeviceCore/DevStorage.h" 
#include "Job.hpp"
#include "PrintJob.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

class Plater;

typedef std::function<void(int status, int code, std::string msg)> OnUpdateStatusFn;
typedef std::function<bool()>                       WasCancelledFn;

class SendJob : public Job
{
    PrintPrepareData    job_data;
    std::string         m_dev_id;
    bool                m_job_finished{ false };
    int                 m_print_job_completed_id = 0;
    bool                m_is_check_mode{false};
    bool                m_check_and_continue{false};
    std::function<void()> m_success_fun{nullptr};
    std::function<void(int)> m_enter_ip_address_fun_fail{nullptr};
    std::function<void()> m_enter_ip_address_fun_success{nullptr};
    Plater *m_plater;

public:
    void prepare();
    SendJob(std::string dev_id = "");

    std::string m_project_name;
    std::string m_dev_ip;
    std::string m_access_code;
    std::string task_bed_type;
	std::string task_ams_mapping;
	std::string connection_type;

    bool        m_local_use_ssl_for_ftp{true};
    bool        m_local_use_ssl_for_mqtt{true};
    bool        cloud_print_only { false };
    bool        has_sdcard { false };
    bool        task_use_ams { true };

    DevStorage::SdcardState sdcard_state = DevStorage::SdcardState::NO_SDCARD;

    wxWindow*   m_parent{nullptr};

    int  status_range() const
    {
        return 100;
    }

    wxString get_http_error_msg(unsigned int status, std::string body);
    void set_check_mode() {m_is_check_mode = true;};
    void check_and_continue() {m_check_and_continue = true;};
    bool is_finished() { return m_job_finished;  }
    void process(Ctl &ctl) override;
    void on_success(std::function<void()> success);
    void on_check_ip_address_fail(std::function<void(int)> func);
    void on_check_ip_address_success(std::function<void()> func);
    void finalize(bool canceled, std::exception_ptr &) override;
    void set_project_name(std::string name);
};

}}

#endif
