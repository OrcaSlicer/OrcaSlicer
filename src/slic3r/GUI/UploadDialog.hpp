#pragma once

#include <wx/dialog.h>
#include <wx/checklst.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class UploadDialog : public wxDialog
{
public:
    UploadDialog(wxWindow* parent, const std::string& username, const std::string& password,
                 const std::string& gcode_path, const std::string& filename);
    ~UploadDialog();

private:
    // Workers only own this state; closing the dialog cannot leave callbacks using destroyed widgets.
    struct RequestResult {
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        std::string body;
        std::string error;
    };
    void get_online_printers();
    void send_file(bool cloud_only);
    void start_request(bool upload, std::vector<std::string> printers = {});
    void finish_request(wxTimerEvent&);
    void notify(const std::string& message, bool error = false);

    std::string m_username, m_password, m_gcode_path, m_filename;
    std::vector<std::string> m_printer_ids;
    wxCheckListBox* m_printers;
    wxButton* m_refresh;
    wxButton* m_send;
    wxButton* m_cloud_only;
    wxTimer m_timer;
    std::shared_ptr<RequestResult> m_result;
    bool m_uploading = false;
};

}} // namespace Slic3r::GUI
