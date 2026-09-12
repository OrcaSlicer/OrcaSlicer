#ifndef slic3r_GUI_PrintagoBridge_hpp_
#define slic3r_GUI_PrintagoBridge_hpp_

#include <atomic>
#include <memory>
#include <string>

#include <wx/webview.h>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI {

// Shared low-level plumbing for the Printago message bridge (embedded-pages protocol v1), used by
// both the "Save to Printago" dialog and the persistent Printago app tab. Owns host->page delivery,
// the host:init payload, and the native file-upload leg (upload:begin -> bytes -> upload:progress /
// upload:done). The owner drives it from its own script-message handler. File bytes never cross the
// bridge; only the small JSON envelopes do.
class PrintagoBridge
{
public:
    explicit PrintagoBridge(wxWebView* webview);
    ~PrintagoBridge();

    // host -> page: window.__printagoBridge.receive(<json literal>). Call on the GUI thread.
    void send(const std::string& type, const nlohmann::json& payload);

    // Convenience senders shared by all Printago webviews.
    void send_host_init();      // host:init (protocol / app / versions / engine / capabilities)
    void send_theme(bool dark); // theme {dark}

    // Native upload of `file_path` per an upload:begin payload {uploadId,url,method,headers}. Emits
    // upload:progress (throttled to ~4/sec) and upload:done. Supersedes any in-flight upload; each
    // uploadId is independent (a retry from the page carries a fresh signed URL).
    void        start_upload(const nlohmann::json& upload_begin_payload, const std::string& file_path);
    void        cancel_upload();
    std::string current_upload_id() const { return m_upload_id; }

    wxWebView* webview() const { return m_webview; }

    // OrcaSlicer version as a plain semver (e.g. "2.5.0"), dropping any pre-release suffix.
    static std::string host_version();

private:
    wxWebView*                        m_webview = nullptr;
    unsigned                          m_seq     = 0;
    std::shared_ptr<std::atomic_bool> m_alive;    // guards worker-thread callbacks after destruction
    Http::Ptr                         m_http;      // active upload (its worker thread is detached)
    std::string                       m_upload_id; // id of the active upload; stale callbacks are dropped
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PrintagoBridge_hpp_
