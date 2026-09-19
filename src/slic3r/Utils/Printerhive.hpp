#ifndef slic3r_Printerhive_hpp_
#define slic3r_Printerhive_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

class Printerhive : public PrintHost
{
public:
    Printerhive(DynamicPrintConfig *config);
    ~Printerhive() override = default;

    const char* get_name() const override;

    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

protected:
    std::string m_host;
    std::string m_apikey;

    virtual void set_auth(Http &http) const;
};

}

#endif
