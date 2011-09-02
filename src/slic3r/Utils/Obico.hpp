#ifndef slic3r_Obico_hpp_
#define slic3r_Obico_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;
class Obico : public PrintHost
{
public:
    Obico(DynamicPrintConfig* config);
    ~Obico() override = default;

    const char* get_name() const override;
    virtual bool can_test() const { return true; };
    bool has_auto_discovery() const override { return false; }
    bool is_cloud() const override { return true; }
    bool get_login_url(wxString& auth_url) const override;
    std::string  get_host() const override;

    wxString                           get_test_ok_msg() const override;
    wxString                           get_test_failed_msg(wxString& msg) const override;
    virtual bool                       test(wxString& curl_msg) const override;
    bool                               get_printers(wxArrayString& printers) const override;
    PrintHostPostUploadActions         get_post_upload_actions() const;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

protected:
    virtual void set_auth(Http& http) const;
private:
    std::string m_host;
    std::string m_port;
    std::string m_apikey;
    std::string m_cafile;
    std::string m_web_ui;
    bool        m_ssl_revoke_best_effort;

    std::string make_url(const std::string& path) const;
};
} // namespace Slic3r

#endif