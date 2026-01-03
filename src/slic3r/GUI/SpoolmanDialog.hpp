#ifndef ORCASLICER_SPOOLMANDIALOG_HPP
#define ORCASLICER_SPOOLMANDIALOG_HPP
#include "GUI_Utils.hpp"
#include "OptionsGroup.hpp"
#include "Spoolman.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/LoadingSpinner.hpp"

namespace Slic3r {
class SpoolmanDynamicConfig;
namespace GUI {
class SpoolInfoWidget : public wxPanel
{
public:
    SpoolInfoWidget(wxWindow* parent, const Preset* preset);

    void rescale();

    [[nodiscard]] const std::string& get_preset_name() const { return m_preset->name; }

    void set_combobox_selection(int idx) { if (m_combobox) m_combobox->SetSelection(idx); }

private:
    wxStaticBitmap* m_spool_bitmap;
    Label*          m_preset_name_label;
    ComboBox*       m_combobox;
    const Preset*   m_preset;
};

class SpoolmanDialog : DPIDialog
{
public:
    SpoolmanDialog(wxWindow* parent);
    ~SpoolmanDialog() override;
    void build_options_group() const;
    void build_spool_info();
    void show_loading(bool show = true);
    void save_spoolman_settings();
    void OnFinishLoading(wxCommandEvent& event);
    void OnRefresh(wxCommandEvent& e);
    void OnOK(wxCommandEvent& e);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void OnSpoolWidgetSelection(wxCommandEvent& e);

    bool                   m_dirty_settings{false};
    bool                   m_dirty_host{false};
    ConfigOptionsGroup*    m_optgroup;
    SpoolmanDynamicConfig* m_config;
    wxPanel*               m_main_panel;
    wxPanel*               m_loading_panel;
    wxBoxSizer*            m_info_widgets_parent_sizer;
    wxGridSizer*           m_info_widgets_grid_sizer;
    wxBoxSizer*            m_spoolman_error_label_sizer;
    Label*                 m_spoolman_error_label;
    LoadingSpinner*        m_loading_spinner;
    DialogButtons*         m_buttons;

    std::map<std::string, int> m_spool_id_updates;
};
}} // namespace Slic3r::GUI

#endif // ORCASLICER_SPOOLMANDIALOG_HPP