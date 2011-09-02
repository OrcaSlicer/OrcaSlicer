#ifndef __BindJob_HPP__
#define __BindJob_HPP__

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include "Job.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

class BindJob : public Job
{
    wxWindow *           m_event_handle{nullptr};
    std::function<void()> m_success_fun{nullptr};
    std::string         m_dev_id;
    std::string         m_dev_ip;
    std::string         m_sec_link;
    std::string         m_ssdp_version;
    bool                m_job_finished{ false };
    int                 m_print_job_completed_id = 0;
    bool                m_improved{false};

public:
    BindJob(std::string dev_id, std::string dev_ip, std::string sec_link, std::string ssdp_version);

    int  status_range() const
    {
        return 100;
    }

    bool is_finished() { return m_job_finished;  }

    void on_success(std::function<void()> success);
    void update_status(Ctl &ctl, int st, const std::string &msg);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
    void set_event_handle(wxWindow* hanle);
    void post_fail_event(int code, std::string info);
    void set_improved(bool improved){m_improved = improved;};
};

wxDECLARE_EVENT(EVT_BIND_UPDATE_MESSAGE, wxCommandEvent);
wxDECLARE_EVENT(EVT_BIND_MACHINE_SUCCESS, wxCommandEvent);
wxDECLARE_EVENT(EVT_BIND_MACHINE_FAIL, wxCommandEvent);
}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
