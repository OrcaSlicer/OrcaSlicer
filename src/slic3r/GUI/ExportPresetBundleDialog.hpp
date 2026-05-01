#ifndef slic3r_ExportPresetBundleDialog_hpp_
#define slic3r_ExportPresetBundleDialog_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"

#include "libslic3r/AppConfig.hpp"
#include <slic3r/GUI/GUI.hpp>
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

enum ExportCase {
    INITIALIZE_FAIL = 0,
    ADD_FILE_FAIL,
    ADD_BUNDLE_STRUCTURE_FAIL,
    FINALIZE_FAIL,
    OPEN_ZIP_WRITTEN_FILE,
    EXPORT_CANCEL,
    EXPORT_SUCCESS,
    CASE_COUNT,
};

class ExportPresetBundleDialog : public Slic3r::GUI::DPIDialog
{
public:
    ExportPresetBundleDialog(wxWindow* parent,
                             wxWindowID id         = wxID_ANY,
                             const wxString& title = wxT(""),
                             const wxPoint& pos    = wxDefaultPosition,
                             const wxSize& size    = wxDefaultSize,
                             long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX);

    ~ExportPresetBundleDialog();

    // Utilities
    bool seq_top_layer_only_changed() const { return m_seq_top_layer_only_changed; }
    bool recreate_GUI() const { return m_recreate_GUI; }
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void show_export_result(const ExportCase& e);

    void Init();
    void InitExportData();

    // Webview
    void LoadUrl(wxString& url);
    void OnScriptMessage(wxWebViewEvent& e);
    void RunScript(const wxString& s);
    void OnRequestPresets();
    void OnExportData(const wxString& path, const wxString& name, json data);

protected:
    bool m_seq_top_layer_only_changed{false};
    bool m_recreate_GUI{false};

    // Webview
    wxWebView* m_browser{nullptr};

    // Export Preset
    std::unordered_map<std::string, Preset*> m_printer_presets; // first: printer name, second: printer presets have same printer name
    std::unordered_map<std::string, std::vector<const Preset*>>
        m_filament_presets; // first: printer name, second: filament presets have same printer name
    std::unordered_map<std::string, std::vector<const Preset*>>
        m_process_presets; // first: printer name, second: filament presets have same printer name
    std::unordered_map<std::string, std::vector<std::pair<std::string, Preset*>>>
        m_filament_name_to_presets; // first: filament name, second presets have same filament name and printer name in vector
};
}} // namespace Slic3r::GUI

#endif