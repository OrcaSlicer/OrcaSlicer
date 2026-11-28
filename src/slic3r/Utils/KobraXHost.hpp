#ifndef slic3r_KobraXHost_hpp_
#define slic3r_KobraXHost_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Print host for the Anycubic Kobra X (see KobraX.hpp). Without the printer-agents preference, Send and Print
// go through here: the upload address is announced over MQTT, so each upload opens a short MQTT session.
class KobraXHost : public PrintHost
{
public:
    explicit KobraXHost(DynamicPrintConfig* config);
    ~KobraXHost() override = default;

    const char* get_name() const override;

    bool                       test(wxString& msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return false; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }

private:
    std::string m_host;
    int         m_http_port;
};

} // namespace Slic3r

#endif
