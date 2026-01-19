#include "OctoPrint.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
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

namespace {
#ifdef WIN32
std::string get_host_from_url(const std::string& url_in)
{
    std::string url = url_in;
    // add http:// if there is no scheme
    size_t double_slash = url.find("//");
    if (double_slash == std::string::npos)
        url = "http://" + url;
    std::string out = url;
    CURLU* hurl = curl_url();
    if (hurl) {
        // Parse the input URL.
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Replace the address.
            char* host;
            rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
            if (rc == CURLUE_OK) {
                char* port;
                rc = curl_url_get(hurl, CURLUPART_PORT, &port, 0);
                if (rc == CURLUE_OK && port != nullptr) {
                    out = std::string(host) + ":" + port;
                    curl_free(port);
                } else {
                    out = host;
                    curl_free(host);
                }
            }
            else
                BOOST_LOG_TRIVIAL(error) << "OctoPrint get_host_from_url: failed to get host form URL " << url;
        }
        else
            BOOST_LOG_TRIVIAL(error) << "OctoPrint get_host_from_url: failed to parse URL " << url;
        curl_url_cleanup(hurl);
    }
    else
        BOOST_LOG_TRIVIAL(error) << "OctoPrint get_host_from_url: failed to allocate curl_url";
    return out;
}

    // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
std::string substitute_host(const std::string& orig_addr, std::string sub_addr)
{
    // put ipv6 into [] brackets 
    if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[')
        sub_addr = "[" + sub_addr + "]";

#if 0
    //URI = scheme ":"["//"[userinfo "@"] host [":" port]] path["?" query]["#" fragment]
    std::string final_addr = orig_addr;
    //  http
    size_t double_dash = orig_addr.find("//");
    size_t host_start = (double_dash == std::string::npos ? 0 : double_dash + 2);
    // userinfo
    size_t at = orig_addr.find("@");
    host_start = (at != std::string::npos && at > host_start ? at + 1 : host_start);
    // end of host, could be port(:), subpath(/) (could be query(?) or fragment(#)?)
    // or it will be ']' if address is ipv6 )
    size_t potencial_host_end = orig_addr.find_first_of(":/", host_start); 
    // if there are more ':' it must be ipv6
    if (potencial_host_end != std::string::npos && orig_addr[potencial_host_end] == ':' && orig_addr.rfind(':') != potencial_host_end) {
        size_t ipv6_end = orig_addr.find(']', host_start);
        // DK: Uncomment and replace orig_addr.length() if we want to allow subpath after ipv6 without [] parentheses.
        potencial_host_end = (ipv6_end != std::string::npos ? ipv6_end + 1 : orig_addr.length()); //orig_addr.find('/', host_start));
    }
    size_t host_end = (potencial_host_end != std::string::npos ? potencial_host_end : orig_addr.length());
    // now host_start and host_end should mark where to put resolved addr
    // check host_start. if its nonsense, lets just use original addr (or  resolved addr?)
    if (host_start >= orig_addr.length()) {
        return final_addr;
    }
    final_addr.replace(host_start, host_end - host_start, sub_addr);
    return final_addr;
#else
    // Using the new CURL API for handling URL. https://everything.curl.dev/libcurl/url
    // If anything fails, return the input unchanged.
    std::string out = orig_addr;
    CURLU *hurl = curl_url();
    if (hurl) {
        // Parse the input URL.
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Replace the address.
            rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
            if (rc == CURLUE_OK) {
                // Extract a string fromt the CURL URL handle.
                char *url;
                rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
                if (rc == CURLUE_OK) {
                    out = url;
                    curl_free(url);
                } else
                    BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to extract the URL after substitution";
            } else
                BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to substitute host " << sub_addr << " in URL " << orig_addr;
        } else
            BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to parse URL " << orig_addr;
        curl_url_cleanup(hurl);
    } else
        BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to allocate curl_url";
    return out;
#endif
}
#endif // WIN32
std::string escape_string(const std::string& unescaped)
{
    std::string ret_val;
    CURL* curl = curl_easy_init();
    if (curl) {
        char* decoded = curl_easy_escape(curl, unescaped.c_str(), unescaped.size());
        if (decoded) {
            ret_val = std::string(decoded);
            curl_free(decoded);
        }
        curl_easy_cleanup(curl);
    }
    return ret_val;
}
std::string escape_path_by_element(const boost::filesystem::path& path)
{
    std::string ret_val = escape_string(path.filename().string());
    boost::filesystem::path parent(path.parent_path());
    while (!parent.empty() && parent.string() != "/") // "/" check is for case "/file.gcode" was inserted. Then boost takes "/" as parent_path.
    {
        ret_val = escape_string(parent.filename().string()) + "/" + ret_val;
        parent = parent.parent_path();
    }
    return ret_val;
}
} //namespace


OctoPrint::OctoPrint(DynamicPrintConfig *config) :
    m_host(config->opt_string("print_host")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* OctoPrint::get_name() const { return "OctoPrint"; }

#ifdef WIN32
bool OctoPrint::test_with_resolved_ip(wxString &msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();
    bool res = true;
    // Msg contains ip string.
    auto url = substitute_host(make_url("api/version"), GUI::into_u8(msg));
    msg.Clear();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;

    std::string host = get_host_from_url(m_host);
    auto http = Http::get(url);//std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version at %2% : %3%, HTTP %4%, body: `%5%`") % name % url % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Got version: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res = validate_version_text(text);
                if (!res) {
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
                }
            }
            catch (const std::exception&) {
                res = false;
                msg = "Could not parse server response.";
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    return res;
}
#endif //WIN32

bool OctoPrint::test(wxString& msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char *name = get_name();

    bool res = true;
    auto url = make_url("api/version");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                if (! ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res = validate_version_text(text);
                if (! res) {
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
                }
            }
            catch (const std::exception &) {
                res = false;
                msg = "Could not parse server response";
            }
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

wxString OctoPrint::get_test_ok_msg () const
{
    return _(L("Connection to OctoPrint is working correctly."));
}

wxString OctoPrint::get_test_failed_msg (wxString &msg) const
{
    return GUI::format_wxstr("%s: %s\n\n%s"
        , _L("Could not connect to OctoPrint")
        , msg
        , _L("Note: OctoPrint version 1.1.0 or higher is required."));
}

bool OctoPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
#ifndef WIN32
    return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
#else
    std::string host = get_host_from_url(m_host);

    // decide what to do based on m_host - resolve hostname or upload to ip
    std::vector<boost::asio::ip::address> resolved_addr;
    boost::system::error_code ec;
    boost::asio::ip::address host_ip = boost::asio::ip::make_address(host, ec);
    if (!ec) {
        resolved_addr.push_back(host_ip);
    } else if ( GUI::get_app_config()->get_bool("allow_ip_resolve") && boost::algorithm::ends_with(host, ".local")){
        Bonjour("octoprint")
            .set_hostname(host)
            .set_retries(5) // number of rounds of queries send
            .set_timeout(1) // after each timeout, if there is any answer, the resolving will stop
            .on_resolve([&ra = resolved_addr](const std::vector<BonjourReply>& replies) {
                for (const auto & rpl : replies) {
                    boost::asio::ip::address ip(rpl.ip);
                    ra.emplace_back(ip);
                    BOOST_LOG_TRIVIAL(info) << "Resolved IP address: " << rpl.ip;
                }
            })
            .resolve_sync();
    }
    if (resolved_addr.empty()) {
        // no resolved addresses - try system resolving
        BOOST_LOG_TRIVIAL(error) << "PrusaSlicer failed to resolve hostname " << m_host << " into the IP address. Starting upload with system resolving.";
        return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    } else if (resolved_addr.size() == 1) {
        // one address resolved - upload there
        return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr.front());
    }  else if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
        // there are just 2 addresses and 1 is ip_v4 and other is ip_v6
        // try sending to both. (Then if both fail, show both error msg after second try)
        wxString error_message;
        if (!upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn
            , [&msg = error_message, resolved_addr](wxString error) { msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); }
            , info_fn, resolved_addr.front())
            &&
            !upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn
            , [&msg = error_message, resolved_addr](wxString error) { msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error); }
            , info_fn, resolved_addr.back())
            ) {

            error_fn(error_message);
            return false;
        }
        return true;
    } else {
        // There are multiple addresses - user needs to choose which to use.
        size_t selected_index = resolved_addr.size(); 
        IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
        if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size()) {    
            return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr[selected_index]);
        }
    }
    return false;
#endif // WIN32
}
#ifdef WIN32
bool OctoPrint::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
{
    info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    // Test_msg already contains resolved ip and will be cleared on start of test().
    wxString test_msg_or_host_ip = GUI::from_u8(resolved_addr.to_string());
    if (!test_with_resolved_ip(test_msg_or_host_ip)) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    std::string url = substitute_host(make_url("api/files/local"), resolved_addr.to_string());
    bool result = true;

    info_fn(L"resolve", boost::nowide::widen(url));

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    std::string host = get_host_from_url(m_host);
    auto http = Http::post(url);//std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string())      // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
  
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error % status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    return result;
}
#endif //WIN32

bool OctoPrint::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    wxString test_msg_or_host_ip;
    if (!test(test_msg_or_host_ip)) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    std::string url;
    bool res = true;

#ifdef WIN32
    // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
    if (m_host.find("https://") == 0 || test_msg_or_host_ip.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
#endif // _WIN32
    {
        // If https is entered we assume signed ceritificate is being used
        // IP resolving will not happen - it could resolve into address not being specified in cert
        url = make_url("api/files/local");
    }
#ifdef WIN32
    else {
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        // Curl uses easy_getinfo to get ip address of last successful transaction.
        // If it got the address use it instead of the stored in "host" variable.
        // This new address returns in "test_msg_or_host_ip" variable.
        // Solves troubles of uploades failing with name address.
        // in original address (m_host) replace host for resolved ip 
        info_fn(L"resolve", test_msg_or_host_ip);
        url = substitute_host(make_url("api/files/local"), GUI::into_u8(test_msg_or_host_ip));
        BOOST_LOG_TRIVIAL(info) << "Upload address after ip resolve: " << url;
    }
#endif // _WIN32

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    auto http = Http::post(std::move(url));
#ifdef WIN32
    // "Host" header is necessary here. In the workaround above (two mDNS..) we have got IP address from test connection and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    std::string host = get_host_from_url(m_host);
    http.header("Host", host);
#endif // _WIN32
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string())      // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
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

bool OctoPrint::validate_version_text(const boost::optional<std::string> &version_text) const
{
    return version_text ? boost::starts_with(*version_text, "OctoPrint") : true;
}

void OctoPrint::set_auth(Http &http) const
{
    http.header("X-Api-Key", m_apikey);

    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

std::string OctoPrint::make_url(const std::string &path) const
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

SL1Host::SL1Host(DynamicPrintConfig *config)
    : PrusaLink(config)
{
}

// SL1Host
const char* SL1Host::get_name() const { return "SL1Host"; }

wxString SL1Host::get_test_ok_msg () const
{
    return _(L("Connection to Prusa SL1 / SL1S is working correctly."));
}

wxString SL1Host::get_test_failed_msg (wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Prusa SLA"), msg);
}

bool SL1Host::validate_version_text(const boost::optional<std::string> &version_text) const
{
    return version_text ? boost::starts_with(*version_text, "Prusa SLA") : false;
}

// PrusaLink
PrusaLink::PrusaLink(DynamicPrintConfig* config, bool show_after_message) :
    OctoPrint(config),
    m_authorization_type(config->option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type")->value),
    m_username(config->opt_string("printhost_user")),
    m_password(config->opt_string("printhost_password")),
    m_show_after_message(show_after_message)
{
}

const char* PrusaLink::get_name() const { return "PrusaLink"; }

wxString PrusaLink::get_test_ok_msg() const
{
    return _(L("Connection to PrusaLink is working correctly."));
}

wxString PrusaLink::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to PrusaLink"), msg);
}

bool PrusaLink::validate_version_text(const boost::optional<std::string>& version_text) const
{
    return version_text ? (boost::starts_with(*version_text, "PrusaLink") || boost::starts_with(*version_text, "OctoPrint")) : false;
}

void PrusaLink::set_auth(Http& http) const
{
    switch (m_authorization_type) {
    case atKeyPassword:
        http.header("X-Api-Key", get_apikey());
        break;
    case atUserPassword:
        http.auth_digest(m_username, m_password);
        break;
    }

    if (!get_cafile().empty()) {
        http.ca_file(get_cafile());
    }
}

#if 0
bool PrusaLink::version_check(const boost::optional<std::string>& version_text) const
{
    // version_text is in format OctoPrint 1.2.3
    // true (= use PUT) should return: 
    // PrusaLink 0.7+
    
    try {
        if (!version_text)
            throw Slic3r::RuntimeError("no version_text was given");

        std::vector<std::string> name_and_version;
        boost::algorithm::split(name_and_version, *version_text, boost::is_any_of(" "));

        if (name_and_version.size() != 2)
            throw Slic3r::RuntimeError("invalid version_text");
        
        Semver semver(name_and_version[1]); // throws Slic3r::RuntimeError when unable to parse
        if (name_and_version.front() == "PrusaLink" && semver >= Semver(0, 7, 0))
            return true;
    } catch (const Slic3r::RuntimeError& ex) {
        BOOST_LOG_TRIVIAL(error) << std::string("Print host version check failed: ") + ex.what();
    }

    return false;
}
#endif

bool PrusaLink::test(wxString& msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();

    bool res = true;
    auto url = make_url("api/version");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
        res = false;
        msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res = validate_version_text(text);
                if (!res) {
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : "OctoPrint"));
                }
            }
            catch (const std::exception&) {
                res = false;
                msg = "Could not parse server response";
            }
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

bool PrusaLink::get_storage(wxArrayString& storage_path, wxArrayString& storage_name) const
{
    const char* name = get_name();

    bool res = true;
    auto url = make_url("api/v1/storage");
    wxString error_msg;

    struct StorageInfo {
        wxString path;
        wxString name;
        bool read_only = false;
        long long free_space = -1;
    };
    std::vector<StorageInfo> storage;

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get storage at: %2%") % name % url;

    wxString wlang = GUI::wxGetApp().current_language_code();
    std::string lang = GUI::format(wlang.SubString(0, 1));

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.header("Accept-Language", lang);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting storage: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
        error_msg = L"\n\n" + boost::nowide::widen(error);
        res = false;
        // If status is 0, the communication with the printer has failed completely (most likely a timeout), if the status is <= 400, it is an error returned by the pritner.
        // If 0, we can show error to the user now, as we know the communication has failed. (res = true will do the trick.)
        // if not 0, we must not show error, as not all printers support api/v1/storage endpoint.
        // So we must be extra careful here, or we might be showing errors on perfectly fine communication.
        if (status == 0)
            res = true;
       
    })
    .on_complete([&](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got storage: %2%") % name % body;
        try
        {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);
            
            // what if there is more structure added in the future? Enumerate all elements? 
            if (ptree.front().first != "storage_list") {
                res = false;
                return;
            }
            // each storage has own subtree of storage_list
            for (const auto& section : ptree.front().second) {
                const auto name = section.second.get_optional<std::string>("name");
                const auto path = section.second.get_optional<std::string>("path");
                const auto space = section.second.get_optional<std::string>("free_space");
                const auto read_only = section.second.get_optional<bool>("read_only");
                const auto ro = section.second.get_optional<bool>("ro"); // In PrusaLink 0.7.0RC2 "read_only" value is stored under "ro".
                const auto available = section.second.get_optional<bool>("available");
                if (path && (!available || *available)) {
                    StorageInfo si;
                    si.path = wxString(*path);
                    si.name = name ? wxString(*name) : wxString();
                    // If read_only is missing, assume it is NOT read only.
                    // si.read_only = read_only ? *read_only : false; // version without "ro"
                    si.read_only = (read_only ? *read_only : (ro ? *ro : false));
                    si.free_space = space ? std::stoll(*space) : 1;  // If free_space is missing, assume there is free space.
                    storage.emplace_back(std::move(si));
                }
            }
        }
        catch (const std::exception&)
        {
            res = false;
        }
    })
#ifdef WIN32
    .ssl_revoke_best_effort(m_ssl_revoke_best_effort)

#endif // WIN32
    .perform_sync();

    for (const auto& si : storage) {
        if (!si.read_only && si.free_space > 0) {
            storage_path.push_back(si.path);
            storage_name.push_back(si.name);
        }
    }

    if (res && storage_path.empty()) {
        if (!storage.empty()) { // otherwise error_msg is already filled 
            error_msg = L"\n\n" + _L("Storages found") + L": \n";
            for (const auto& si : storage) {
                error_msg += GUI::format_wxstr(si.read_only ?
                                                                // TRN %1% = storage path
                                                                _L("%1% : read only") : 
                                                                // TRN %1% = storage path
                                                                _L("%1% : no free space"), si.path) + L"\n";
            }
        }
        // TRN %1% = host
        std::string message = GUI::format(_L("Upload has failed. There is no suitable storage found at %1%."), m_host) + GUI::into_u8(error_msg);
        BOOST_LOG_TRIVIAL(error) << message;
        throw Slic3r::IOError(message);
    }

    return res;
}

bool PrusaLink::test_with_method_check(wxString& msg, bool& use_put) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure

    const char* name = get_name();

    bool res = true;
    auto url = make_url("api/version");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
        res = false;
        msg = format_error(body, error, status);
    })
    .on_complete([&, this](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;

        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);

            if (!ptree.get_optional<std::string>("api")) {
                res = false;
                return;
            }

            const auto text = ptree.get_optional<std::string>("text");
            res = validate_version_text(text);
            if (!res) {
                msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : "OctoPrint"));
                use_put = false;
                return;
            }

            //  find capabilities subtree and read upload-by-put
            for (const auto& section : ptree) {
                if (section.first == "capabilities") {
                    const auto put_upload = section.second.get_optional<bool>("upload-by-put");
                    if (put_upload)
                        use_put = *put_upload;
                    break;
                }
            }

        }
        catch (const std::exception&) {
            res = false;
            msg = "Could not parse server response";
        }
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



#ifdef WIN32
bool PrusaLink::test_with_resolved_ip_and_method_check(wxString& msg, bool& use_put) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();
    bool res = true;
    // Msg contains ip string.
    auto url = substitute_host(make_url("api/version"), GUI::into_u8(msg));
    msg.Clear();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;

    std::string host = get_host_from_url(m_host);
    auto http = Http::get(url);//std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http
        .on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version at %2% : %3%, HTTP %4%, body: `%5%`") % name % url % error % status % body;
        res = false;
        msg = format_error(body, error, status);
            })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Got version: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res = validate_version_text(text);
                if (!res) {
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : "OctoPrint"));
                    use_put = false;
                    return;
                }

                //  find capabilities subtree and read upload-by-put
                for (const auto& section : ptree) {
                    if (section.first == "capabilities") {
                        const auto put_upload = section.second.get_optional<bool>("upload-by-put");
                        if (put_upload)
                            use_put = *put_upload;
                        break;
                    }
                }

            }
            catch (const std::exception&) {
                res = false;
                msg = "Could not parse server response";
            }

        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    return res;
}

bool PrusaLink::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
{
    info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    // Test_msg already contains resolved ip and will be cleared on start of test().
    wxString test_msg_or_host_ip = GUI::from_u8(resolved_addr.to_string());
    bool use_put = false;
    if (!test_with_resolved_ip_and_method_check(test_msg_or_host_ip, use_put)) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    std::string storage_path = (use_put ? "api/v1/files" : "api/files");
    storage_path += (upload_data.storage.empty() ? "/local" : upload_data.storage);
    std::string url = substitute_host(make_url(storage_path), resolved_addr.to_string());
    bool result = true;
    info_fn(L"resolve", boost::nowide::widen(url));

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%, method: %7%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        % (use_put ? "PUT" : "POST");

    if (use_put)
        return put_inner(std::move(upload_data), std::move(url), name, prorgess_fn, error_fn, info_fn);
    return post_inner(std::move(upload_data), std::move(url), name, prorgess_fn, error_fn, info_fn);
}

#endif //WIN32

bool PrusaLink::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    wxString test_msg_or_host_ip;
    bool use_put = false;
    if (!test_with_method_check(test_msg_or_host_ip, use_put)) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    std::string url;
    std::string storage_path = (use_put ? "api/v1/files" : "api/files");
    storage_path += (upload_data.storage.empty() ? "/local" : upload_data.storage);
#ifdef WIN32
    // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
    if (m_host.find("https://") == 0 || test_msg_or_host_ip.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
#endif // _WIN32
    {
        // If https is entered we assume signed ceritificate is being used
        // IP resolving will not happen - it could resolve into address not being specified in cert
        url = make_url(storage_path);
    }
#ifdef WIN32
    else {
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        // Curl uses easy_getinfo to get ip address of last successful transaction.
        // If it got the address use it instead of the stored in "host" variable.
        // This new address returns in "test_msg_or_host_ip" variable.
        // Solves troubles of uploades failing with name address.
        // in original address (m_host) replace host for resolved ip 
        info_fn(L"resolve", test_msg_or_host_ip);
        url = substitute_host(make_url(storage_path), GUI::into_u8(test_msg_or_host_ip));
        BOOST_LOG_TRIVIAL(info) << "Upload address after ip resolve: " << url;
    }
#endif // _WIN32

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%, method: %7%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        % (use_put ? "PUT" : "POST");

    if (use_put)
        return put_inner(std::move(upload_data), std::move(url), name, prorgess_fn, error_fn, info_fn);
    return post_inner(std::move(upload_data), std::move(url), name, prorgess_fn, error_fn, info_fn);
}

bool PrusaLink::put_inner(PrintHostUpload upload_data, std::string url, const std::string& name, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    info_fn(L"set_complete_off", wxString());

    bool res = true;
    // Percent escape all filenames in on path and add it to the url. This is different from POST.
    url += "/" + escape_path_by_element(upload_data.upload_path);
    Http http = Http::put(std::move(url));
#ifdef WIN32
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    std::string host = get_host_from_url(m_host);
    http.header("Host", host);
#endif // _WIN32
    set_auth(http);
    // This is ugly, but works. There was an error at PrusaLink side that accepts any string at Print-After-Upload as true, thus False was also triggering print after upload.
    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint)
        http.header("Print-After-Upload", "?1");
    http.set_put_body(upload_data.source_path)
        .header("Content-Type", "text/x.gcode")
        .header("Overwrite", "?1")
        .on_complete([&](std::string body, unsigned status) {
            wxString widebody = wxString::FromUTF8(body);
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % widebody;
            std::string message = m_show_after_message ? (boost::format("%1%") % widebody).str() : std::string();
            info_fn(L"complete", message);
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << "Octoprint: Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}
bool PrusaLink::post_inner(PrintHostUpload upload_data, std::string url, const std::string& name, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    info_fn(L"set_complete_off", wxString());
    bool res = true;
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    Http http = Http::post(std::move(url));
#ifdef WIN32
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    std::string host = get_host_from_url(m_host);
    http.header("Host", host);
#endif // _WIN32
    set_auth(http);
    set_http_post_header_args(http, upload_data.post_action);
    http.form_add("path", upload_parent_path.string())      // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            if (m_show_after_message) {
                // PrusaConnect message
                wxString widebody = wxString::FromUTF8(body);
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % widebody;
                std::string message = m_show_after_message ? (boost::format("%1%") % widebody).str() : std::string();
                if (status == 202)
                    info_fn(L"complete_with_warning", message);
                else 
                    info_fn(L"complete", message);
            } else {
                // PrusaLink
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%") % name % status;
                info_fn(L"complete", wxString());
            }
           
           
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << "Octoprint: Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

void PrusaLink::set_http_post_header_args(Http& http, PrintHostPostUploadAction post_action) const
{
    http.form_add("print", post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");
}

PrusaConnect::PrusaConnect(DynamicPrintConfig* config)
    : PrusaLink(config, true)
{
}

void PrusaConnect::set_http_post_header_args(Http& http, PrintHostPostUploadAction post_action) const
{
    // Language for accept message
    wxString wlang = GUI::wxGetApp().current_language_code();
    std::string lang = GUI::format(wlang.SubString(0, 1));
    http.header("Accept-Language", lang);
    // Post action
    if (post_action == PrintHostPostUploadAction::StartPrint) {
        http.form_add("to_print", "True");
    } else if (post_action == PrintHostPostUploadAction::QueuePrint) {
        http.form_add("to_queue", "True");
    }
   
}

wxString PrusaConnect::get_test_ok_msg() const
{
    return _(L("Connection to Prusa Connect is working correctly."));
}

wxString PrusaConnect::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Prusa Connect"), msg);
}
}
