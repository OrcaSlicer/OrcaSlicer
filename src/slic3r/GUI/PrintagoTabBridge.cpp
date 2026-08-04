#include "PrintagoTabBridge.hpp"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

PrintagoTabBridge* PrintagoTabBridge::s_active = nullptr;

// Compare two filesystem paths for equality. Lexical normalization alone is not enough on macOS:
// the temp dir is /var/folders/... which is a symlink to /private/var/folders/..., and the project
// filename may come back in either spelling — so fall back to symlink-resolving canonicalization.
static bool same_path(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return false;
    if (fs::path(a).lexically_normal() == fs::path(b).lexically_normal())
        return true;
    boost::system::error_code ec_a, ec_b;
    const fs::path ca = fs::weakly_canonical(fs::path(a), ec_a);
    const fs::path cb = fs::weakly_canonical(fs::path(b), ec_b);
    return !ec_a && !ec_b && ca == cb;
}

PrintagoTabBridge::PrintagoTabBridge(wxWebView* webview, Plater* plater)
    : m_bridge(webview), m_plater(plater), m_alive(std::make_shared<std::atomic_bool>(true))
{
    // Register the printago channel via the factory's serialized, deferred path. A direct/synchronous
    // AddScriptMessageHandler is unsafe: on macOS it pumps the event loop (RunScript -> wxYield), and
    // because enable_printago_bridge() runs during MainFrame construction, that re-entrant pump can run
    // a half-initialised background-slicing timer and crash. WebView::AddScriptMessageHandler defers to
    // a CallAfter and serializes against the "wx" registration, so it lands cleanly before the page's
    // JS fires page:ready.
    if (webview)
        WebView::AddScriptMessageHandler(webview, "printago");
    s_active = this;
}

PrintagoTabBridge::~PrintagoTabBridge()
{
    if (m_alive)
        *m_alive = false;
    if (s_active == this)
        s_active = nullptr;
}

void PrintagoTabBridge::notify(const std::string& text) const
{
    if (m_plater && m_plater->get_notification_manager())
        m_plater->get_notification_manager()->push_notification(text);
}

std::string PrintagoTabBridge::current_project_path() const
{
    return m_plater ? into_u8(m_plater->get_project_filename(".3mf")) : std::string();
}

std::string PrintagoTabBridge::active_edit_part() const
{
    const std::string path = current_project_path();
    if (path.empty())
        return {};
    for (const auto& kv : m_edit_parts)
        if (same_path(kv.second.temp_path, path))
            return kv.first;
    return {};
}

void PrintagoTabBridge::on_message(const wxString& json_str)
{
    json msg;
    try {
        msg = json::parse(into_u8(json_str));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Printago tab: could not parse bridge message: " << e.what();
        return;
    }

    if (msg.contains("v") && msg["v"].is_number_integer() && msg["v"].get<int>() != 1)
        return;
    if (!msg.contains("type") || !msg["type"].is_string())
        return;

    const std::string type    = msg["type"].get<std::string>();
    const json        payload = msg.contains("payload") && msg["payload"].is_object() ? msg["payload"] : json::object();

    BOOST_LOG_TRIVIAL(info) << "Printago tab: received " << type;

    if (type == "page:ready") {
        BOOST_LOG_TRIVIAL(info) << "Printago tab: answering page:ready with host:init";
        handle_page_ready();
    } else if (type == "part:edit") {
        handle_part_edit(payload);
    } else if (type == "file:open") {
        handle_file_open(payload);
    } else if (type == "link:external") {
        if (payload.contains("url") && payload["url"].is_string())
            wxLaunchDefaultBrowser(from_u8(payload["url"].get<std::string>()));
    } else if (type == "page:authed" || type == "page:error") {
        BOOST_LOG_TRIVIAL(info) << "Printago tab event [" << type << "]: " << payload.dump();
    } else {
        BOOST_LOG_TRIVIAL(debug) << "Printago tab: ignoring message type: " << type;
    }
}

void PrintagoTabBridge::handle_page_ready()
{
    // Re-init rule: resend host:init on every page:ready. No project:info — the tab has no pending
    // project. The save-back leg lives in the /orca/replace dialog, not here.
    m_bridge.send_host_init();
    m_bridge.send_theme(wxGetApp().dark_mode());
}

void PrintagoTabBridge::handle_part_edit(const json& payload)
{
    if (!payload.contains("partId") || !payload["partId"].is_string() || !payload.contains("fileName") ||
        !payload["fileName"].is_string() || !payload.contains("downloadUrl") || !payload["downloadUrl"].is_string()) {
        BOOST_LOG_TRIVIAL(warning) << "Printago tab: part:edit missing partId/fileName/downloadUrl";
        return;
    }

    const std::string part_id = payload["partId"].get<std::string>();
    const std::string url     = payload["downloadUrl"].get<std::string>();
    // Strip any directory components from the supplied name to avoid path traversal.
    std::string file_name = fs::path(payload["fileName"].get<std::string>()).filename().string();
    if (file_name.empty())
        file_name = "part.3mf";

    const std::string temp_str = (fs::temp_directory_path() / "printago-edit" / part_id / file_name).string();

    auto self = this;
    download_to_temp(url, temp_str, [self, part_id, temp_str, file_name](bool ok) {
        self->finish_edit_open(part_id, temp_str, file_name, ok);
    });
}

void PrintagoTabBridge::download_to_temp(const std::string& url, const std::string& temp_path,
                                         std::function<void(bool ok)> on_gui)
{
    boost::system::error_code ec;
    fs::create_directories(fs::path(temp_path).parent_path(), ec);

    // Download the signed URL with NO auth headers (the signature is in the URL). Runs on the Http
    // worker thread; the bytes are written there, then we marshal to the GUI thread and call on_gui.
    auto alive = m_alive;
    Http::get(url)
        .size_limit(size_t(2) * 1024 * 1024 * 1024) // allow large files (default cap is only 5MB)
        .on_complete([alive, temp_path, on_gui](std::string body, unsigned /*status*/) {
            bool ok = false;
            try {
                fs::ofstream f(fs::path(temp_path), std::ios::binary | std::ios::trunc);
                f.write(body.data(), static_cast<std::streamsize>(body.size()));
                ok = f.good();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Printago tab: failed to write download: " << e.what();
            }
            wxGetApp().CallAfter([alive, on_gui, ok]() {
                if (*alive)
                    on_gui(ok);
            });
        })
        .on_error([alive, on_gui](std::string /*body*/, std::string error, unsigned /*status*/) {
            BOOST_LOG_TRIVIAL(error) << "Printago tab: download failed: " << error;
            wxGetApp().CallAfter([alive, on_gui]() {
                if (*alive)
                    on_gui(false);
            });
        })
        .perform();
}

void PrintagoTabBridge::handle_file_open(const json& payload)
{
    if (!payload.contains("url") || !payload["url"].is_string() || !payload.contains("fileName") ||
        !payload["fileName"].is_string()) {
        BOOST_LOG_TRIVIAL(warning) << "Printago tab: file:open missing url/fileName";
        return;
    }

    const std::string url = payload["url"].get<std::string>();
    std::string file_name = fs::path(payload["fileName"].get<std::string>()).filename().string();
    if (file_name.empty())
        file_name = "preview.gcode";

    const std::string temp_str = (fs::temp_directory_path() / "printago-preview" / file_name).string();
    BOOST_LOG_TRIVIAL(info) << "Printago tab: file:open -> " << temp_str;

    auto self = this;
    download_to_temp(url, temp_str, [self, temp_str](bool ok) {
        if (!self->m_plater)
            return;
        if (!ok) {
            self->notify(_u8L("Could not download the file to preview."));
            return;
        }
        // View-only preview, NOT an edit session: no partId association, no save-back. A raw .gcode
        // goes to the gcode viewer; a .gcode.3mf (or anything else) through the generic loader. Both
        // run Orca's normal unsaved-project prompt.
        if (is_gcode_file(temp_str)) {
            self->m_plater->load_gcode(from_u8(temp_str));
        } else {
            wxArrayString paths;
            paths.Add(from_u8(temp_str));
            self->m_plater->load_files(paths);
        }
    });
}

void PrintagoTabBridge::finish_edit_open(const std::string& part_id, const std::string& temp_path,
                                         const std::string& file_name, bool downloaded_ok)
{
    json r;
    r["partId"] = part_id;

    if (!downloaded_ok || !m_plater) {
        r["ok"]    = false;
        r["error"] = "Could not download the part file.";
        m_bridge.send("part:edit-opened", r);
        return;
    }

    // Open as the active project. load_project runs the normal unsaved-changes prompt; if the user
    // cancels it, nothing loads and the project filename is left unchanged — detect that by comparing.
    m_plater->load_project(from_u8(temp_path), "<loadall>");
    const bool opened = same_path(current_project_path(), temp_path);

    r["ok"] = opened;
    if (opened) {
        m_edit_parts[part_id] = EditPart{temp_path, file_name};
        notify((boost::format(_u8L("Editing Printago part \"%1%\". Use Replace in Printago to save your changes back.")) %
                file_name)
                   .str());
    } else {
        r["error"] = "Open was cancelled.";
    }
    m_bridge.send("part:edit-opened", r);
}

}} // namespace Slic3r::GUI
