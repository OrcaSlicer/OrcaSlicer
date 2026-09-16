#include "PrintagoBridge.hpp"

#include <chrono>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include "GUI_App.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/Widgets/WebView.hpp"

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

std::string PrintagoBridge::host_version()
{
    std::string v    = SoftFever_VERSION;
    const auto  dash = v.find('-');
    if (dash != std::string::npos)
        v = v.substr(0, dash);
    return v;
}

PrintagoBridge::PrintagoBridge(wxWebView* webview)
    : m_webview(webview), m_alive(std::make_shared<std::atomic_bool>(true))
{}

PrintagoBridge::~PrintagoBridge()
{
    if (m_alive)
        *m_alive = false;
    cancel_upload();
}

void PrintagoBridge::send(const std::string& type, const json& payload)
{
    if (!m_webview)
        return;
    json msg;
    msg["v"]       = 1;
    msg["id"]      = "host-" + std::to_string(++m_seq);
    msg["type"]    = type;
    msg["payload"] = payload;

    // Deliver as window.__printagoBridge.receive(<literal>). json(str).dump() escapes the message as
    // a safe JS string literal. Guard on the shim so an early send (before the page's JS boots) is a
    // harmless no-op rather than a JS error.
    const std::string literal = json(msg.dump()).dump();
    const wxString    script  = wxString::FromUTF8(
        "window.__printagoBridge && window.__printagoBridge.receive(" + literal + ")");
    WebView::RunScript(m_webview, script);
}

void PrintagoBridge::send_host_init()
{
    json init;
    init["protocolVersion"] = 1;
    init["hostApp"]         = "OrcaSlicer";
    init["hostVersion"]     = host_version();
    init["slicerEngine"]    = "orcaslicer";
    init["slicerVersion"]   = host_version();
    init["capabilities"]    = json::array();
    send("host:init", init);
}

void PrintagoBridge::send_theme(bool dark)
{
    json t;
    t["dark"] = dark;
    send("theme", t);
}

void PrintagoBridge::cancel_upload()
{
    if (m_http) {
        m_http->cancel(); // signals the detached worker thread to abort
        m_http.reset();
    }
    m_upload_id.clear();
}

void PrintagoBridge::start_upload(const json& payload, const std::string& file_path)
{
    if (!payload.contains("uploadId") || !payload["uploadId"].is_string() || !payload.contains("url") ||
        !payload["url"].is_string()) {
        BOOST_LOG_TRIVIAL(warning) << "Printago: upload:begin missing uploadId/url";
        return;
    }

    // Each upload:begin is independent (fresh signed URL with a ~5-minute expiry). Supersede any
    // in-flight upload before starting the new one.
    cancel_upload();

    const std::string upload_id = payload["uploadId"].get<std::string>();
    const std::string url       = payload["url"].get<std::string>();
    const std::string method    = payload.value("method", std::string("PUT"));
    m_upload_id                 = upload_id;

    const boost::filesystem::path path(file_path);
    if (!boost::filesystem::exists(path)) {
        json done;
        done["uploadId"] = upload_id;
        done["ok"]       = false;
        done["error"]    = "exported project file is missing";
        send("upload:done", done);
        return;
    }

    // PUT streams the raw file body (production GCS signed URL); POST posts the file body (dev
    // emulator). Both send the 3MF bytes verbatim as the request body.
    const bool is_put = boost::iequals(method, "PUT");
    Http       http   = is_put ? Http::put(url) : Http::post(url);
    if (is_put)
        http.set_put_body(path);
    else
        http.set_post_body(path);

    // Apply exactly the headers the page supplied in upload:begin.
    bool has_content_type = false;
    if (payload.contains("headers") && payload["headers"].is_object()) {
        for (auto it = payload["headers"].begin(); it != payload["headers"].end(); ++it) {
            if (it.value().is_string()) {
                http.header(it.key(), it.value().get<std::string>());
                if (boost::iequals(it.key(), "Content-Type"))
                    has_content_type = true;
            }
        }
    }
    // Do NOT inject a Content-Type for PUT: production GCS signed URLs are signed WITHOUT one, so any
    // Content-Type we send breaks the signature (403 SignatureDoesNotMatch). curl's PUT (CURLOPT_UPLOAD)
    // sends no Content-Type by default, which is exactly what GCS expects. The dev emulator (POST) keeps
    // an octet-stream default so curl doesn't fall back to a form content-type. A page-supplied
    // Content-Type (in upload:begin headers) always wins for either method.
    if (!has_content_type && !is_put)
        http.header("Content-Type", "application/octet-stream");

    // Signed upload endpoints are HTTPS with valid certificates; verify TLS for this request.
    http.tls_verify(true);

    BOOST_LOG_TRIVIAL(info) << "Printago: starting upload " << upload_id << " (" << method << " "
                            << url.substr(0, 120) << (url.size() > 120 ? "..." : "") << ") from " << file_path;

    auto self          = this; // safe: guarded by *alive below; the owner outlives cancellation
    auto alive         = m_alive;
    auto last_progress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());

    http.on_progress([self, alive, upload_id, last_progress](Http::Progress progress, bool& cancel) {
            if (!*alive) {
                cancel = true;
                return;
            }
            // Throttle to ~4/sec, always emitting the final 100%.
            const auto now = std::chrono::steady_clock::now();
            const bool due = progress.ulnow == progress.ultotal ||
                             std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_progress).count() >= 250;
            if (progress.ultotal == 0 || !due)
                return;
            *last_progress     = now;
            const size_t sent  = progress.ulnow;
            const size_t total = progress.ultotal;
            wxGetApp().CallAfter([self, alive, upload_id, sent, total]() {
                if (!*alive || self->m_upload_id != upload_id)
                    return;
                json p;
                p["uploadId"]   = upload_id;
                p["bytesSent"]  = sent;
                p["bytesTotal"] = total;
                self->send("upload:progress", p);
            });
        })
        .on_complete([self, alive, upload_id](std::string, unsigned status) {
            wxGetApp().CallAfter([self, alive, upload_id, status]() {
                if (!*alive || self->m_upload_id != upload_id)
                    return;
                BOOST_LOG_TRIVIAL(info) << "Printago: upload " << upload_id << " done ok (HTTP " << status << ")";
                json d;
                d["uploadId"]   = upload_id;
                d["ok"]         = true;
                d["httpStatus"] = status;
                self->send("upload:done", d);
            });
        })
        .on_error([self, alive, upload_id](std::string /*body*/, std::string error, unsigned status) {
            wxGetApp().CallAfter([self, alive, upload_id, error, status]() {
                if (!*alive || self->m_upload_id != upload_id)
                    return;
                BOOST_LOG_TRIVIAL(error) << "Printago: upload " << upload_id << " FAILED (HTTP " << status
                                         << "): " << error;
                json d;
                d["uploadId"] = upload_id;
                d["ok"]       = false;
                if (status)
                    d["httpStatus"] = status;
                d["error"] = error.empty() ? std::string("upload failed") : error;
                self->send("upload:done", d);
            });
        });

    m_http = http.perform(); // runs on the Http client's own worker thread
}

}} // namespace Slic3r::GUI
