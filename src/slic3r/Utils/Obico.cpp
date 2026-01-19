#include "Obico.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

Obico::Obico(DynamicPrintConfig* config) : 
    m_host(config->opt_string("print_host")), 
    m_web_ui(config->opt_string("print_host_webui")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_port(config->opt_string("printhost_port")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* Obico::get_name() const { return "Obico"; }

std::string Obico::get_host() const {
    return m_host;
}
void  Obico::set_auth(Http& http) const
{
    http.header("Authorization", "Bearer " + m_apikey);
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

bool Obico::get_login_url(wxString& auth_url) const
{
    auth_url = make_url("o/authorize?response_type=token&client_id=OrcaSlicer&hide_navbar=true");
    return true;
}

wxString Obico::get_test_ok_msg() const { return _(L("Connected to Obico successfully!")); }

wxString Obico::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Obico"), msg.Truncate(256));
}

bool Obico::test(wxString& msg) const
{ 
    if (m_apikey.empty()) {
        return false;
    }

    bool res = true;
    const char* name = get_name();
    auto url = make_url("api/v1/version/");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
}

bool Obico::get_printers(wxArrayString& printers) const
{
    const char* name = get_name();
    bool        res  = false;
    auto        url  = make_url("api/v1/printers/");
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: List printers at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);

    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error listing printers: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
        })
        .on_complete([&](std::string body, unsigned http_status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got printers: %2%, HTTP status: %3%") % name % body % http_status;
            if (http_status != 200)
                throw HostNetworkError(GUI::format(_L("HTTP status: %1%\nMessage body: \"%2%\""), http_status, body));

            std::stringstream ss(body);
            pt::ptree         ptree;
            try {
                pt::read_json(ss, ptree);
            } catch (const pt::ptree_error& err) {
                throw HostNetworkError(
                    GUI::format(_L("Parsing of host response failed.\nMessage body: \"%1%\"\nError: \"%2%\""), body, err.what()));
            }

            const auto error = ptree.get_optional<std::string>("error");
            if (error)
                throw HostNetworkError(*error);

            try {
                BOOST_FOREACH (boost::property_tree::ptree::value_type& v, ptree) {
                    const auto name = v.second.get<std::string>("name");
                    const auto port = v.second.get<std::string>("id");
                    printers.push_back(Slic3r::GUI::from_u8(name + " [" + port + "]"));
                }
            } catch (const std::exception& err) {
                throw HostNetworkError(
                    GUI::format(_L("Enumeration of host printers failed.\nMessage body: \"%1%\"\nError: \"%2%\""), body, err.what()));
            }
            res = true;
        })
        .perform_sync();

    return res;
}

PrintHostPostUploadActions Obico::get_post_upload_actions() const {
    return PrintHostPostUploadAction::StartPrint; 
}

bool Obico::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{   
    const char* name = get_name();
    const auto  upload_filename    = upload_data.upload_path.filename();
    const auto  upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;
    auto url = make_url("api/v1/g_code_files/");

    auto  http = Http::post(url); // std::move(url));
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add("printer_id", m_port)
        .form_add("filename", upload_filename.string()) 
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return res;
}

std::string Obico::make_url(const std::string& path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}
}
