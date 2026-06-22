#ifndef slic3r_UltimakerLink_hpp_
#define slic3r_UltimakerLink_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// REST-API PrintHost für UltiMaker Classic, S/Factor und Method-Drucker.
// Protokoll: HTTP (Port 80), Auth via X-Api-ID + X-Api-Key Header.

#include "PrintHost.hpp"
#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <wx/string.h>

namespace Slic3r {

class UltimakerLink : public PrintHost
{
public:
    explicit UltimakerLink(DynamicPrintConfig *config);
    ~UltimakerLink() override = default;

    const char *get_name() const override { return "UltiMaker"; }

    bool      test(wxString &curl_info) const override;
    wxString  get_test_ok_msg()                    const override;
    wxString  get_test_failed_msg(wxString &msg)   const override;
    bool      upload(PrintHostUpload upload_data,
                     ProgressFn prg_fn,
                     ErrorFn    err_fn,
                     InfoFn     info_fn) const override;

    bool                     has_auto_discovery()        const override { return true;  }
    bool                     can_test()                  const override { return true;  }
    PrintHostPostUploadActions get_post_upload_actions() const override
        { return PrintHostPostUploadActions(); }
    std::string              get_host()                  const override { return m_host; }

private:
    std::string m_host;
    std::string m_api_id;
    std::string m_api_key;
    int         m_port { 80 };

    // Generischer synchroner REST-Aufruf (GET / POST).
    bool rest_get (const std::string &endpoint,
                   nlohmann::json    &out,
                   std::string       &error) const;
    bool rest_post(const std::string &endpoint,
                   const std::string &body,
                   nlohmann::json    &out,
                   std::string       &error) const;

    // Prüft / beantragt Authentifizierung.
    bool check_auth(std::string &error) const;
};

} // namespace Slic3r

#endif // slic3r_UltimakerLink_hpp_
