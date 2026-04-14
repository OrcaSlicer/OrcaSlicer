#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r_version.h"
#include "libslic3r/Preset.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <boost/filesystem/path.hpp>
#include <thread>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/filedlg.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

namespace pt = boost::property_tree;
using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

namespace {

DynamicPrintConfig* get_active_printer_config()
{
  if (wxGetApp().preset_bundle == nullptr)
    return nullptr;

  return &wxGetApp().preset_bundle->printers.get_edited_preset().config;
}

std::string json_string(const json& node, const char* key)
{
  auto it = node.find(key);
  return (it != node.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string dump_json(const json& node)
{
  return node.dump(-1, ' ', false, json::error_handler_t::replace);
}

boost::filesystem::path path_from_utf8(const std::string& utf8_path)
{
#ifdef _WIN32
  const wxString wide_path = wxString::FromUTF8(utf8_path.c_str());
  return boost::filesystem::path(wide_path.ToStdWstring());
#else
  return boost::filesystem::path(utf8_path);
#endif
}

std::string filename_to_utf8(const boost::filesystem::path& path)
{
#ifdef _WIN32
  const wxString wx_filename(path.filename().c_str());
  const wxScopedCharBuffer utf8 = wx_filename.ToUTF8();
  return utf8.data() != nullptr ? std::string(utf8.data()) : std::string();
#else
  return path.filename().string();
#endif
}

}

struct PrinterWebView::ElegooImpl {
  explicit ElegooImpl(PrinterWebView& owner)
    : owner(owner)
  {
  }

  ~ElegooImpl()
  {
    stop_upload = true;
    if (upload_thread.joinable())
      upload_thread.join();
    if (sn_thread.joinable())
      sn_thread.join();
  }

  void send_ipc_message(const char* type, const std::string& request_id, const std::string& method, int code,
              const std::string& message, const std::string& data_json = "{}")
  {
    if (owner.m_browser == nullptr)
      return;

    json body = json::object();
    body["type"] = type;
    if (!request_id.empty())
      body["id"] = request_id;
    if (!method.empty())
      body["method"] = method;

    json data = json::parse(data_json, nullptr, false);
    if (data.is_discarded())
      data = json::object();
    body["data"] = std::move(data);

    if (std::string(type) == "response") {
      body["code"] = code;
      body["message"] = message;
    }

    const wxString payload = wxString::FromUTF8(dump_json(body));
    const wxString script = "if (typeof HandleStudio === 'function') { HandleStudio(" + payload + "); } else { window.postMessage(" + payload + ", '*'); }";
    wxGetApp().CallAfter([this, script]() {
      if (owner.m_browser != nullptr)
        WebView::RunScript(owner.m_browser, script);
    });
  }

  void handle_ipc_message(const wxString& message)
  {
    if (message.empty())
      return;

    json root = json::parse(message.ToUTF8().data(), nullptr, false);
    if (root.is_discarded() || !root.is_object())
      return;

    std::string request_id = json_string(root, "id");
    std::string method     = json_string(root, "method");
    json        params     = root.contains("params") && root["params"].is_object() ? root["params"] : json::object();

    if (method.empty()) {
      method = json_string(root, "command");
      if (params.empty() && root.contains("data") && root["data"].is_object())
        params = root["data"];
    }

    if (method == "open" || method == "common_openurl") {
      const std::string url = json_string(params, "url").empty() ? json_string(root, "url") : json_string(params, "url");
      if (!url.empty())
        wxLaunchDefaultBrowser(url);
      if (!request_id.empty())
        send_ipc_message("response", request_id, method, 0, "success");
      return;
    }

    if (method == "upload_file") {
      handle_upload_request(request_id, method, dump_json(params));
      return;
    }

    if (method == "open_file_dialog") {
      handle_open_file_dialog_request(request_id, method, dump_json(params));
      return;
    }

    if (method == "get_sn") {
      handle_get_sn_request(request_id, method);
      return;
    }
  }

  void handle_upload_request(const std::string& request_id, const std::string& method, const std::string& params_json)
  {
    if (upload_in_progress.exchange(true)) {
      send_ipc_message("response", request_id, method, 1, "Upload already in progress");
      return;
    }

    if (upload_thread.joinable())
      upload_thread.join();

    json params = json::parse(params_json, nullptr, false);
    if (params.is_discarded())
      params = json::object();


    std::string file_path = json_string(params, "filePath");
    std::string file_name = json_string(params, "fileName");

    if (file_path.empty()) {
      upload_in_progress = false;
      send_ipc_message("response", request_id, method, 1, "Missing filePath");
      return;
    }

    // HTML IPC passes UTF-8 strings; decode explicitly to avoid Windows codepage issues.
    boost::filesystem::path source_path = path_from_utf8(file_path);
    if (file_name.empty())
      file_name = filename_to_utf8(source_path);

    DynamicPrintConfig* config = get_active_printer_config();
    std::unique_ptr<PrintHost> print_host(config == nullptr ? nullptr : PrintHost::get_print_host(config));
    if (print_host == nullptr) {
      upload_in_progress = false;
      send_ipc_message("response", request_id, method, 1, "Could not get a valid Printer Host reference");
      return;
    }

    stop_upload = false;
    upload_thread = std::thread([this, request_id, method, file_path, file_name, source_path, print_host = std::move(print_host)]() mutable {
      std::string error_message;

      PrintHostUpload upload_data;
      upload_data.use_3mf      = false;
      upload_data.post_action  = PrintHostPostUploadAction::None;
      upload_data.source_path  = source_path;
      upload_data.upload_path  = path_from_utf8(file_name);

      const bool success = print_host->upload(
        std::move(upload_data),
        [this, request_id](Http::Progress progress, bool& cancel) {
          cancel = stop_upload.load();
          json data = {
            {"uploadedBytes", static_cast<uint64_t>(progress.ulnow)},
            {"totalBytes", static_cast<uint64_t>(progress.ultotal)}
          };
          send_ipc_message("event", request_id, "upload_progress", 0, "", dump_json(data));
        },
        [&error_message](wxString error) {
          error_message = error.ToUTF8().data();
        },
        [this, request_id](wxString tag, wxString status) {
          json data = {
            {"tag", tag.ToUTF8().data()},
            {"status", status.ToUTF8().data()}
          };
          send_ipc_message("event", request_id, "upload_info", 0, "", dump_json(data));
        });

      upload_in_progress = false;

      if (success) {
        json data = {
          {"success", true},
          {"filePath", file_path},
          {"fileName", file_name}
        };
        send_ipc_message("response", request_id, method, 0, "success", dump_json(data));
      } else {
        if (error_message.empty())
          error_message = "Upload failed";
        send_ipc_message("response", request_id, method, 1, error_message);
      }
    });
  }

  void handle_open_file_dialog_request(const std::string& request_id, const std::string& method, const std::string& params_json)
  {
    json params = json::parse(params_json, nullptr, false);
    if (params.is_discarded())
      params = json::object();

    const std::string filter = json_string(params, "filter").empty() ? "All files (*.*)|*.*" : json_string(params, "filter");

    wxWindow* parent = owner.GetParent();
    if (parent == nullptr)
      parent = wxGetApp().GetTopWindow();

    wxFileDialog open_file_dialog(parent, _L("Open File"), "", "", wxString::FromUTF8(filter), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    json data = json::object();
    data["files"] = json::array();
    if (open_file_dialog.ShowModal() != wxID_CANCEL)
      data["files"].push_back(open_file_dialog.GetPath().ToUTF8().data());

    send_ipc_message("response", request_id, method, 0, "success", dump_json(data));
  }

  void handle_get_sn_request(const std::string& request_id, const std::string& method)
  {
    if (sn_request_in_progress.exchange(true)) {
      send_ipc_message("response", request_id, method, 1, "SN request already in progress");
      return;
    }

    if (sn_thread.joinable())
      sn_thread.join();

    sn_thread = std::thread([this, request_id, method]() {
      std::string sn;

      DynamicPrintConfig* config = get_active_printer_config();
      std::unique_ptr<PrintHost> print_host(config == nullptr ? nullptr : PrintHost::get_print_host(config));
      if (print_host != nullptr)
        sn = print_host->get_sn();

      sn_request_in_progress = false; 
      json data = {
        {"sn", sn}
      };
      send_ipc_message("response", request_id, method, 0, "success", dump_json(data));
    });
  }

  PrinterWebView&         owner;
  std::atomic<bool>       upload_in_progress { false };
  std::atomic<bool>       sn_request_in_progress { false };
  std::atomic<bool>       stop_upload { false };
  std::thread             upload_thread;
  std::thread             sn_thread;
};

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_browser(nullptr)
    , m_zoomFactor(100)
    , m_apikey()
    , m_apikey_sent(false)
    , m_url_deferred()
    , m_elegooImpl(std::make_unique<ElegooImpl>(*this))
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

      // Create the webview
    m_browser = WebView::CreateWebView(this, "");
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

#ifdef __linux__
    auto cookiesPath = boost::filesystem::path(data_dir() + "/cache/cookies.db");
    auto wv = static_cast<WebKitWebView*>(m_browser->GetNativeBackend());
    auto wv_ctx = webkit_web_view_get_context(wv);
    auto cookieManager = webkit_web_context_get_cookie_manager(wv_ctx);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesPath.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#endif

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrinterWebView::OnNewWindow, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this);

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();

    // Log backend information
    /* m_browser->GetUserAgent() may lead crash
    if (wxGetApp().get_mode() == comDevelop) {
        wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("Backend: %s Version: %s", m_browser->GetClassInfo()->GetClassName(),
            wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("User Agent: %s", m_browser->GetUserAgent());
    }
    */

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    m_elegooImpl.reset();

    // Destroy the webview
    if(m_browser){
        m_browser->Destroy();
        m_browser = nullptr;
    }


    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
//    this->Show();
//    this->Raise();
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;
    m_apikey_sent = false;

    if (this->IsShown()) {
        m_url_deferred.clear();
        m_browser->LoadURL(url);
    } else {
        m_url_deferred = url;
    }
    //m_browser->SetFocus();
    UpdateState();
}

bool PrinterWebView::Show(bool show)
{
    if (show && !m_url_deferred.empty()) {
        m_browser->LoadURL(m_url_deferred);
        m_url_deferred.clear();
    }
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

void PrinterWebView::update_mode()
{
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;

    // Special handling for Elegoo Link: the URL contains a fixed id parameter that we can check to avoid sending the API key to the embedded printer web UI, 
    // which does not need it and would reject it.
    if(evt.GetURL().find("&id=elegoo") != wxString::npos){
        return;
    }
    SendAPIKey();
}

void PrinterWebView::OnNewWindow(wxWebViewEvent& evt)
{
  const wxString url = evt.GetURL();
  if (!url.empty())
    wxLaunchDefaultBrowser(url);
  evt.Veto();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt)
{
    m_elegooImpl->handle_ipc_message(evt.GetString());
}


} // GUI
} // Slic3r
