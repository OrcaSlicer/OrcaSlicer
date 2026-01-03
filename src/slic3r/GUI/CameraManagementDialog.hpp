#ifndef slic3r_CameraManagementDialog_hpp_
#define slic3r_CameraManagementDialog_hpp_

#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/CheckBox.hpp"
#include "libslic3r/AppConfig.hpp"
#include <wx/dataview.h>

namespace Slic3r { namespace GUI {

class CameraEditDialog : public DPIDialog
{
public:
    CameraEditDialog(wxWindow* parent,
                     const std::string& dev_id = "",
                     const std::string& url = "",
                     CameraSourceType source_type = CameraSourceType::Builtin,
                     bool enabled = false);

    std::string get_dev_id() const;
    std::string get_url() const;
    CameraSourceType get_source_type() const;
    bool get_enabled() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void create_ui();
    void populate_printer_list();
    void on_ok(wxCommandEvent& event);
    void on_source_type_changed(wxCommandEvent& event);
    void update_url_field_state();

    wxComboBox* m_printer_combo{nullptr};
    wxComboBox* m_source_type_combo{nullptr};
    TextInput* m_url_input{nullptr};
    CheckBox* m_enabled_checkbox{nullptr};

    std::vector<std::pair<std::string, std::string>> m_printers;
    std::string m_initial_dev_id;
};

class CameraManagementDialog : public DPIDialog
{
public:
    CameraManagementDialog(wxWindow* parent);
    ~CameraManagementDialog();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void create_ui();
    void refresh_list();
    void cleanup_list_data();
    void on_add(wxCommandEvent& event);
    void on_edit(wxCommandEvent& event);
    void on_delete(wxCommandEvent& event);
    void on_selection_changed(wxDataViewEvent& event);
    void on_item_activated(wxDataViewEvent& event);

    std::string get_printer_name_for_dev_id(const std::string& dev_id);
    std::string truncate_serial(const std::string& dev_id);

    wxDataViewListCtrl* m_list_ctrl{nullptr};
    Button* m_btn_add{nullptr};
    Button* m_btn_edit{nullptr};
    Button* m_btn_delete{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_CameraManagementDialog_hpp_
