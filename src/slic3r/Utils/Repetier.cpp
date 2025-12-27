#include "Repetier.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/progdlg.h>


#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"


namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

Repetier::Repetier(DynamicPrintConfig *config) :
    host(config->opt_string("print_host")),
    apikey(config->opt_string("printhost_apikey")),
    cafile(config->opt_string("printhost_cafile")),
    port(config->opt_string("printhost_port"))
{}

const char* Repetier::get_name() const { return "Repetier"; }



static bool validate_repetier(const boost::optional<std::string>& name,
                              const boost::optional<std::string>& soft)
{
    if (soft) {
        // See https://github.com/prusa3d/PrusaSlicer/issues/7807:
        // Repetier allows "rebranding", so the "name" value is not reliable when detecting
        // server type. Newer Repetier versions send "software", which should be invariant.
        return ((*soft) == "Repetier-Server");
    } else {
        // If there is no "software" value, validate as we did before:
        return name ? boost::starts_with(*name, "Repetier") : true;
    }
}



bool Repetier::test(wxString &msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure

    const char *name = get_name();

    bool res = true;
    auto url = make_url("printer/info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: List version at: %2%") % name % url;

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

                const auto text = ptree.get_optional<std::string>("name");
                const auto soft = ptree.get_optional<std::string>("software");
                res = validate_repetier(text, soft);
                if (! res) {
                    msg = GUI::from_u8((boost::format(_utf8(L("Mismatched type of print host: %s"))) % (soft ? *soft : (text ? *text : "Repetier"))).str());
                }
            }
            catch (const std::exception &) {
                res = false;
                msg = "Could not parse server response";
            }
        })
        .perform_sync();

    return res;
}

wxString Repetier::get_test_ok_msg () const
{
    return _(L("Connection to Repetier is working correctly."));
}

wxString Repetier::get_test_failed_msg (wxString &msg) const
{
        return GUI::from_u8((boost::format("%s: %s\n\n%s")
        % _utf8(L("Could not connect to Repetier"))
        % std::string(msg.ToUTF8())
        % _utf8(L("Note: Repetier version 0.90.0 or higher is required."))).str());
}

bool Repetier::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char *name = get_name();

    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    wxString test_msg;
    if (! test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;

    auto url = upload_data.post_action == PrintHostPostUploadAction::StartPrint
        ? make_url((boost::format("printer/job/%1%") % port).str())
        : make_url((boost::format("printer/model/%1%") % port).str());

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%, group: %7%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        % upload_data.group;

    auto http = Http::post(std::move(url));
    set_auth(http);

    if (! upload_data.group.empty() && upload_data.group != _utf8(L("Default"))) {
        http.form_add("group", upload_data.group);
    }

    if(upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        http.form_add("name", upload_filename.string());
        http.form_add("autostart", "true"); // See https://github.com/prusa3d/PrusaSlicer/issues/7807#issuecomment-1235519371
    }

    http.form_add("a", "upload")
        .form_add_file("filename", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << "Repetier: Upload canceled";
                res = false;
            }
        })
        .perform_sync();

    return res;
}



void Repetier::set_auth(Http &http) const
{
    http.header("X-Api-Key", apikey);

    if (! cafile.empty()) {
        http.ca_file(cafile);
    }
}

std::string Repetier::make_url(const std::string &path) const
{
    if (host.find("http://") == 0 || host.find("https://") == 0) {
        if (host.back() == '/') {
            return (boost::format("%1%%2%") % host % path).str();
        } else {
            return (boost::format("%1%/%2%") % host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % host % path).str();
    }
}

bool Repetier::get_groups(wxArrayString& groups) const
{
    bool res = true;
    
    const char *name = get_name();
    auto url = make_url((boost::format("printer/api/%1%") % port).str());

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get groups at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.form_add("a", "listModelGroups");
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
        })
        .on_complete([&](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got groups: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                BOOST_FOREACH(boost::property_tree::ptree::value_type &v, ptree.get_child("groupNames.")) {
                    if (v.second.data() == "#") {
                        groups.push_back(_utf8(L("Default")));
                    } else {
                        // Is it safe to assume that the data are utf-8 encoded?
                        groups.push_back(GUI::from_u8(v.second.data()));
                    }
                }
            }
            catch (const std::exception &) {
                //msg = "Could not parse server response";
                res = false;
            }
        })
        .perform_sync();

    return res;
}

bool Repetier::get_printers(wxArrayString& printers) const
{
    const char *name = get_name();

    bool res = true;
    auto url = make_url("printer/list");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: List printers at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error listing printers: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
        })
        .on_complete([&](std::string body, unsigned http_status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got printers: %2%, HTTP status: %3%") % name % body % http_status;
            
            if (http_status != 200)
                throw HostNetworkError(GUI::format(_L("HTTP status: %1%\nMessage body: \"%2%\""), http_status, body));

            std::stringstream ss(body);
            pt::ptree ptree;
            try {
                pt::read_json(ss, ptree);
            } catch (const pt::ptree_error &err) {
                throw HostNetworkError(GUI::format(_L("Parsing of host response failed.\nMessage body: \"%1%\"\nError: \"%2%\""), body, err.what()));
            }
            
            const auto error = ptree.get_optional<std::string>("error");
            if (error)
                throw HostNetworkError(*error);

            try {
                BOOST_FOREACH(boost::property_tree::ptree::value_type &v, ptree.get_child("data.")) {
                    const auto port = v.second.get<std::string>("slug");
                    printers.push_back(Slic3r::GUI::from_u8(port));
                }
            } catch (const std::exception &err) {
                throw HostNetworkError(GUI::format(_L("Enumeration of host printers failed.\nMessage body: \"%1%\"\nError: \"%2%\""), body, err.what()));
            }
        })
        .perform_sync();

    return res;
}

}
