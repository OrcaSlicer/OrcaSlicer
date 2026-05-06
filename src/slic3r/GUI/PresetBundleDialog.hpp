#ifndef slic3r_PresetBundleDialog_hpp_
#define slic3r_PresetBundleDialog_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include <boost/thread/detail/thread.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <memory>
#include <slic3r/GUI/GUI.hpp>
#include <string>
#include <unordered_map>
#include <wx/dataview.h>
#include <wx/event.h>
#include <wx/language.h>
#include <wx/string.h>
#include <wx/fswatcher.h>
#include <wx/webview.h>
namespace Slic3r { namespace GUI {

#define DESIGN_GRAY900_COLOR wxColour("#363636") // Label color
#define DESIGN_GRAY600_COLOR wxColour("#ACACAC") // Dimmed text color

#define DESIGN_WINDOW_SIZE wxSize(FromDIP(640), FromDIP(640))
#define DESIGN_TITLE_SIZE wxSize(FromDIP(280), -1)
#define DESIGN_COMBOBOX_SIZE wxSize(FromDIP(120), -1)
#define DESIGN_LARGE_COMBOBOX_SIZE wxSize(FromDIP(120), -1)
#define DESIGN_INPUT_SIZE wxSize(FromDIP(120), -1)
#define DESIGN_LEFT_MARGIN 25
#define VERTICAL_GAP_SIZE FromDIP(4)
class PresetBundleDialog : public Slic3r::GUI::DPIDialog
{
public:
    PresetBundleDialog(wxWindow* parent,
                       wxWindowID id         = wxID_ANY,
                       const wxString& title = wxT(""),
                       const wxPoint& pos    = wxDefaultPosition,
                       const wxSize& size    = wxDefaultSize,
                       long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX);

    ~PresetBundleDialog();

    void create();

    bool DeleteBundleById(const wxString& id);
    bool UnsubscribeBundleById(const std::string& id);

    bool seq_top_layer_only_changed() const { return m_seq_top_layer_only_changed; }
    bool recreate_GUI() const { return m_recreate_GUI; }
    void on_dpi_changed(const wxRect& suggested_rect) override;

    // webview utilities
    void load_url(wxString& url);
    void ListBundles();
    void OpenFolder(const std::string& id);
    void DeleteBundle(const std::string& id);
    void UnsubscribeBundle(const std::string& id);
    void OpenBundleOnCloud(const std::string& id);

    void OnPresetBundlePage();

    // sends command to webview
    void RunScript(const wxString& s);

    // webview events
    void OnScriptMessage(wxWebViewEvent& e);

    void StartDialogWorker();
    void StopDialogWorker();

    void RefreshBundleMap();

    bool CheckUpdateCloud();

    void OnBundleUpdate(wxCommandEvent& evt);

    // true if b>a
    bool CompareVer(const std::string& a, const std::string& b);

protected:
    wxFileSystemWatcher* m_watcher = nullptr;

    void OnFSWatch(wxFileSystemWatcherEvent& e);

    bool m_seq_top_layer_only_changed{false};
    bool m_recreate_GUI{false};

    // Webview
    wxWebView* m_browser{nullptr};
    std::unordered_map<std::string, BundleMetadata> bundle_copy;

    boost::thread m_dialog_worker_thread;
    std::shared_ptr<int> m_dialog_worker_token;
    std::atomic<bool> m_check_update_pending{false};
    std::atomic<bool> m_update_bundles{false};

private:
    Slic3r::AppConfig* app_config;
};

}} // namespace Slic3r::GUI
#endif