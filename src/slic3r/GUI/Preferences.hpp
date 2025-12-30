#ifndef slic3r_Preferences_hpp_
#define slic3r_Preferences_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"

#include <wx/dialog.h>
#include <wx/timer.h>
#include <vector>
#include <list>
#include <map>
#include "Widgets/ComboBox.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/TabCtrl.hpp"

namespace Slic3r { namespace GUI {

#define DESIGN_GRAY900_COLOR wxColour("#363636") // Label color
#define DESIGN_GRAY600_COLOR wxColour("#ACACAC") // Dimmed text color

#define DESIGN_WINDOW_SIZE wxSize(FromDIP(640), FromDIP(640))
#define DESIGN_TITLE_SIZE wxSize(FromDIP(280), -1)
#define DESIGN_COMBOBOX_SIZE wxSize(FromDIP(120), -1)
#define DESIGN_LARGE_COMBOBOX_SIZE wxSize(FromDIP(120), -1)
#define DESIGN_INPUT_SIZE wxSize(FromDIP(120), -1)
#define DESIGN_LEFT_MARGIN 25

class CheckBox;
class TextInput;

class PreferencesDialog : public DPIDialog
{
private:
    AppConfig *app_config;

protected:
    wxBoxSizer *  m_sizer_body;
    wxScrolledWindow* m_parent;
    TabCtrl* m_pref_tabs;

    // bool								m_settings_layout_changed {false};
    bool m_seq_top_layer_only_changed{false};
    bool m_recreate_GUI{false};

public:
    bool seq_top_layer_only_changed() const { return m_seq_top_layer_only_changed; }
    bool recreate_GUI() const { return m_recreate_GUI; }
    void on_dpi_changed(const wxRect &suggested_rect) override;

public:
    PreferencesDialog(wxWindow *      parent,
                      wxWindowID      id    = wxID_ANY,
                      const wxString &title = wxT(""),
                      const wxPoint & pos   = wxDefaultPosition,
                      const wxSize &  size  = wxDefaultSize,
                      long            style = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX);

    ~PreferencesDialog();

    wxString m_backup_interval_time;

    void      create();

    // debug mode
    ::CheckBox * m_developer_mode_ckeckbox   = {nullptr};
    ::CheckBox * m_internal_developer_mode_ckeckbox = {nullptr};
    ::CheckBox * m_dark_mode_ckeckbox        = {nullptr};
    ::TextInput *m_backup_interval_textinput = {nullptr};
    ::CheckBox * m_legacy_networking_ckeckbox     = {nullptr};

    wxString m_developer_mode_def;
    wxString m_internal_developer_mode_def;
    wxString m_backup_interval_def;
    wxString m_iot_environment_def;

    std::vector<wxFlexGridSizer*> f_sizers;

    wxBoxSizer *create_item_title(wxString title);
    wxBoxSizer *create_item_combobox(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, std::function<void(wxString)> onchange = {});
    wxBoxSizer *create_item_combobox(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, std::vector<std::string> config_name_index);
    wxBoxSizer *create_item_region_combobox(wxString title, wxString tooltip);
    wxBoxSizer *create_item_language_combobox(wxString title, wxString tooltip);
    wxBoxSizer *create_item_loglevel_combobox(wxString title, wxString tooltip, std::vector<wxString> vlist);
    wxBoxSizer *create_item_checkbox(wxString title, wxString tooltip, std::string param, const wxString secondary_title = "");
    wxBoxSizer *create_item_darkmode(wxString title,wxString tooltip, std::string param);
    void set_dark_mode();
    wxBoxSizer *create_item_button(wxString title, wxString title2, wxString tooltip, wxString tooltip2, std::function<void()> onclick);
    wxBoxSizer *create_item_downloads(wxString title, wxString tooltip);
    wxBoxSizer *create_item_input(wxString title, wxString title2, wxString tooltip, std::string param, std::function<void(wxString)> onchange = {});
    wxBoxSizer *create_camera_orbit_mult_input(wxString title, wxString tooltip);
    wxBoxSizer *create_item_backup(wxString title, wxString tooltip);
    wxBoxSizer *create_item_auto_reslice(wxString title, wxString checkbox_tooltip, wxString delay_tooltip);
    wxBoxSizer *create_item_multiple_combobox(wxString title, wxString tooltip, std::string parama, std::vector<wxString> vlista, std::vector<wxString> vlistb);
#ifdef WIN32
    wxBoxSizer *create_item_link_association(wxString url_prefix, wxString website_name);
#endif // WIN32

    void create_items();
    void create_sync_page();
    void create_shortcuts_page();
    wxBoxSizer* create_debug_page();

    // BBS
    void create_select_domain_widget();

    void Split(const std::string &src, const std::string &separator, std::vector<wxString> &dest);
    int m_current_language_selected = {0};

private:
    std::tuple<wxBoxSizer*, ComboBox*> create_item_combobox_base(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, unsigned int current_index);
};

}} // namespace Slic3r::GUI

#endif /* slic3r_Preferences_hpp_ */
