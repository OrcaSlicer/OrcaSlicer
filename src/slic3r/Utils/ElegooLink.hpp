#ifndef slic3r_ElegooLink_hpp_
#define slic3r_ElegooLink_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "OctoPrint.hpp"
#include "WebSocketClient.hpp"
namespace Slic3r {

class DynamicPrintConfig;
class Http;

class ElegooLink : public OctoPrint
{
public:
    ElegooLink(DynamicPrintConfig *config);
    ~ElegooLink() override = default;
    const char* get_name() const override;
    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
protected:
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;

#ifdef WIN32
    virtual bool test_with_resolved_ip(wxString& curl_msg) const override;
    bool elegoo_test_with_resolved_ip(wxString& curl_msg) const;
#endif

private:
    bool elegoo_test(wxString& curl_msg) const;
    bool print(WebSocketClient&  client,
               std::string       timeLapse,
               std::string       heatedBedLeveling,
               std::string       bedType,
               const std::string filename, ErrorFn error_fn) const;
    bool checkResult(WebSocketClient&  client,
               ErrorFn           error_fn) const;

    bool loopUpload(std::string url, PrintHostUpload upload_data,
                    ProgressFn        prorgess_fn,
                    ErrorFn           error_fn,
                    InfoFn            info_fn) const;

    bool uploadPart(Http &http,
                    std::string       md5,
                    std::string       uuid,
                    std::string       path,
                    std::string       filename,
                    size_t            filesize,
                    size_t            offset,
                    size_t            length,
                    ProgressFn        prorgess_fn,
                    ErrorFn           error_fn,
                    InfoFn            info_fn) const;
};
}

#endif
