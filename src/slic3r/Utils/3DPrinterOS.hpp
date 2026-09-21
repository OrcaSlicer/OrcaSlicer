#ifndef slic3r_3DPrinterOS_hpp_
#define slic3r_3DPrinterOS_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/property_tree/ptree.hpp>

#include "PrintHost.hpp"
#include "slic3r/GUI/GUI.hpp"



namespace Slic3r {

class DynamicPrintConfig;
class Http;


class C3DPrinterOS : public PrintHost
{
public:
    C3DPrinterOS(DynamicPrintConfig *config);
    ~C3DPrinterOS() override = default;

    const char* get_name() const override;
    bool test(wxString &curl_msg) const override;
    bool login(wxString &msg) const;
    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    bool is_cloud() const override { return true; }
    void log_out() const override;
    bool is_logged_in() const override { return !m_apikey.empty(); }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint | PrintHostPostUploadAction::QueuePrint; }
    std::string        get_host() const override { return m_host; }
    static std::string default_host() { return "https://cloud.3dprinteros.com"; }
    
protected:
    bool validate_version_text(const boost::optional<std::string> &version_text) const;

private:
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    std::string m_username;
    std::string m_host_type;
    std::string m_preset_name;
    std::string m_api_session_file_path;

    void load_api_session();
    bool save_api_session(const std::string &session, const std::string &email) const;
    std::string parse_printer_model(const std::string& input) const;
    std::string make_url(const std::string &path) const;
    std::string get_api_auth_token(wxString &err) const;
    void login_with_token(boost::property_tree::ptree &resp, const std::string &token) const;
    bool check_session(wxString &msg) const;
    void send_form(
        const std::string &endpoint,
        const std::string &postBody,
        boost::property_tree::ptree &responseTree
    ) const;

    
    void get_cloud_projects_list(boost::property_tree::ptree &response) const;
    void get_cloud_printer_types(boost::property_tree::ptree &response, const std::string &querry) const;
    void update_file(
        boost::property_tree::ptree &response,
        const std::string &file_id,
        const std::string &ptype,
        const std::string &gtype
    ) const;

};

}

#endif
