#ifndef slic3r_OctoPrint_hpp_
#define slic3r_OctoPrint_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

class OctoPrint : public PrintHost
{
public:
    OctoPrint(DynamicPrintConfig *config);
    ~OctoPrint() override = default;

    const char* get_name() const override;

    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }

protected:
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;

    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    virtual void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;

#ifdef WIN32
    virtual bool test_with_resolved_ip(wxString& curl_msg) const;
#endif
};


class PrusaLink : public OctoPrint
{
public:
    PrusaLink(DynamicPrintConfig* config) : PrusaLink(config, false) {}
    PrusaLink(DynamicPrintConfig* config, bool show_after_message);
    ~PrusaLink() override = default;

    const char* get_name() const override;

    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    virtual PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }

    // gets possible storage to be uploaded to. This allows different printer to have different storage. F.e. local vs sdcard vs usb.
    bool get_storage(wxArrayString& storage_path, wxArrayString& storage_name) const override;
protected:
    bool test(wxString& curl_msg) const override;
    bool validate_version_text(const boost::optional<std::string>& version_text) const override;
    bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    void set_auth(Http& http) const override;
    virtual void set_http_post_header_args(Http& http, PrintHostPostUploadAction post_action) const;
#ifdef WIN32
    bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const override;
#endif

    // Host authorization type.
    AuthorizationType m_authorization_type;
    // username and password for HTTP Digest Authentization (RFC RFC2617)
    std::string m_username;
    std::string m_password;

private:
    bool test_with_method_check(wxString& curl_msg, bool& use_put) const;
    bool put_inner(PrintHostUpload upload_data, std::string url, const std::string& name, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;
    bool post_inner(PrintHostUpload upload_data, std::string url, const std::string& name, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;
#ifdef WIN32
    bool test_with_resolved_ip_and_method_check(wxString& curl_msg, bool& use_put) const;
#endif

    bool m_show_after_message;

#if 0
    bool version_check(const boost::optional<std::string>& version_text) const;
#endif
};

class PrusaConnect : public PrusaLink
{
public:
    PrusaConnect(DynamicPrintConfig* config);
    ~PrusaConnect() override = default;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint | PrintHostPostUploadAction::QueuePrint; }
    const char* get_name() const override { return "PrusaConnect"; }
    bool get_storage(wxArrayString& storage_path, wxArrayString& storage_name) const override { return false; }
protected:
    void set_http_post_header_args(Http& http, PrintHostPostUploadAction post_action) const override;
};

class SL1Host : public PrusaLink
{
public:
    SL1Host(DynamicPrintConfig* config);
    ~SL1Host() override = default;

    const char* get_name() const override;

    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return {}; }

protected:
    bool validate_version_text(const boost::optional<std::string>& version_text) const override;
};

}

#endif
