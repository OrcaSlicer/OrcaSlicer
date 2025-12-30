#include "Printerhive.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "nlohmann/json.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "Http.hpp"

using json = nlohmann::json;

namespace Slic3r {

Printerhive::Printerhive(DynamicPrintConfig *config) :
    m_host(config->opt_string("print_host")),
    m_apikey(config->opt_string("printhost_apikey"))
{}

const char* Printerhive::get_name() const { return "Printerhive"; }

bool Printerhive::test(wxString& msg) const
{
    const char *name = get_name();
    bool res = true;

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Testing connection at: %2%") % name % m_host;

    auto http = Http::get(m_host);
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error testing connection: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Connection test successful: HTTP %2%") % name % status;
            // Success if HTTP 200
            if (status != 200) {
                res = false;
                msg = format_error(body, "Unexpected status code", status);
            }
        })
        .perform_sync();

    return res;
}

wxString Printerhive::get_test_ok_msg () const
{
    return _(L("Connection to Printerhive is working correctly."));
}

wxString Printerhive::get_test_failed_msg (wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Printerhive"), msg);
}

bool Printerhive::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    bool res = true;

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, print: %5%")
        % name
        % upload_data.source_path
        % m_host
        % upload_filename.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    auto http = Http::post(m_host);
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
            
            // If "Switch to Device tab after upload" is enabled, load Printerhive URL in Device tab
            auto& app = GUI::wxGetApp();
            if (app.app_config->get_bool("open_device_tab_post_upload")) {
                const auto mainframe = app.mainframe;
                if (mainframe) {
                    // Parse response to get file_uuid
                    std::string file_uuid;
                    try {
                        const auto j = json::parse(body, nullptr, false, true);
                        if (j.is_object() && j.contains("file_uuid")) {
                            file_uuid = j["file_uuid"].get<std::string>();
                        }
                    } catch (const std::exception& e) {
                        BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Failed to parse response JSON: %2%") % name % e.what();
                    }
                    
                    // Build URL from host: replace /orcaslicer/ with /orcaslicer-print/ and append /file_uuid
                    std::string display_url = m_host;
                    boost::algorithm::replace_first(display_url, "/orcaslicer/", "/orcaslicer-print/");
                    if (!file_uuid.empty()) {
                        // Remove trailing space if present
                        display_url.erase(display_url.find_last_not_of(" \t") + 1);
                        display_url += "/" + file_uuid;
                    }
                    
                    // Add API key as query parameter if not empty
                    if (!m_apikey.empty()) {
                        display_url += (display_url.find('?') == std::string::npos ? "?" : "&");
                        display_url += "api_key=" + m_apikey;
                    }
                    
                    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Loading URL in Device tab: %2%") % name % display_url;
                    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Using API key: %2%") % name % (m_apikey.empty() ? "(empty)" : m_apikey);
                    
                    // Use CallAfter to safely call GUI functions from HTTP callback thread
                    app.CallAfter([mainframe, display_url]() {
                        // Switch Device tab to PrinterWebView (if it's currently showing MonitorPanel)
                        mainframe->show_device(false);
                        mainframe->request_select_tab(MainFrame::TabPosition::tpMonitor);
                        mainframe->load_printer_url(display_url, ""); // API key is now in URL
                    });
                }
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
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
        .perform_sync();

    return res;
}

void Printerhive::set_auth(Http &http) const
{
    http.header("X-Api-Key", m_apikey);
}

} // namespace Slic3r
