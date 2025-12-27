#include "AstroBox.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/progdlg.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "Http.hpp"


namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

AstroBox::AstroBox(DynamicPrintConfig *config) :
    host(config->opt_string("print_host")),
    apikey(config->opt_string("printhost_apikey")),
    cafile(config->opt_string("printhost_cafile"))
{}

const char* AstroBox::get_name() const { return "AstroBox"; }

bool AstroBox::test(wxString &msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure

    const char *name = get_name();

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

                if (! ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res = validate_version_text(text);
                if (! res) {
                    msg = GUI::from_u8((boost::format(_utf8(L("Mismatched type of print host: %s"))) % (text ? *text : "AstroBox")).str());
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

wxString AstroBox::get_test_ok_msg () const
{
    return _(L("Connection to AstroBox is working correctly."));
}

wxString AstroBox::get_test_failed_msg (wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s\n\n%s")
        % _utf8(L("Could not connect to AstroBox"))
        % std::string(msg.ToUTF8())
        % _utf8(L("Note: AstroBox version 1.1.0 or higher is required."))).str());
}

bool AstroBox::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
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

    auto url = make_url("api/files/local");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    auto http = Http::post(std::move(url));
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
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << "AstroBox: Upload canceled";
                res = false;
            }
        })
        .perform_sync();

    return res;
}

bool AstroBox::validate_version_text(const boost::optional<std::string> &version_text) const
{
    return version_text ? boost::starts_with(*version_text, "AstroBox") : true;
}

void AstroBox::set_auth(Http &http) const
{
    http.header("X-Api-Key", apikey);

    if (! cafile.empty()) {
        http.ca_file(cafile);
    }
}

std::string AstroBox::make_url(const std::string &path) const
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

}
