#ifndef slic3r_GUI_NetworkPluginDialog_hpp_
#define slic3r_GUI_NetworkPluginDialog_hpp_

#include "GUI_Utils.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Button.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include <wx/collpane.h>

namespace Slic3r {
namespace GUI {

class NetworkPluginDownloadDialog : public DPIDialog
{
public:
    enum class Mode {
        MissingPlugin,
        UpdateAvailable,
        CorruptedPlugin
    };

    NetworkPluginDownloadDialog(wxWindow* parent, Mode mode,
        const std::string& current_version = "",
        const std::string& error_message = "",
        const std::string& error_details = "");

    std::string get_selected_version() const;

    enum ResultCode {
        RESULT_DOWNLOAD = wxID_OK,
        RESULT_SKIP = wxID_CANCEL,
        RESULT_REMIND_LATER = wxID_APPLY,
        RESULT_SKIP_VERSION = wxID_IGNORE,
        RESULT_DONT_ASK = wxID_ABORT
    };

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void create_missing_plugin_ui();
    void create_update_available_ui(const std::string& current_version);
    void setup_version_selector();
    void on_download(wxCommandEvent& evt);
    void on_skip(wxCommandEvent& evt);
    void on_remind_later(wxCommandEvent& evt);
    void on_skip_version(wxCommandEvent& evt);
    void on_dont_ask(wxCommandEvent& evt);

    Mode m_mode;
    ComboBox* m_version_combo{nullptr};
    wxCollapsiblePane* m_details_pane{nullptr};
    std::string m_error_message;
    std::string m_error_details;
    std::vector<BBL::NetworkLibraryVersionInfo> m_available_versions;
};

class NetworkPluginRestartDialog : public DPIDialog
{
public:
    NetworkPluginRestartDialog(wxWindow* parent);

    bool should_restart_now() const { return m_restart_now; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    bool m_restart_now{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_NetworkPluginDialog_hpp_
