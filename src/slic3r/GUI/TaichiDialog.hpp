#ifndef slic3r_GUI_TaichiDialog_hpp_
#define slic3r_GUI_TaichiDialog_hpp_

#include "GUI_Utils.hpp"

#include <wx/dialog.h>

class wxButton;
class wxTextCtrl;

namespace Slic3r {
class Model;

namespace GUI {

class MainFrame;

class TaichiDialog final : public DPIDialog
{
public:
    explicit TaichiDialog(MainFrame* parent);
    ~TaichiDialog() override = default;

private:
    void on_dpi_changed(const wxRect& suggested_rect) override {}

    bool prompt_backend_config();
    void on_close(wxCloseEvent& evt);

    void on_from_model(wxCommandEvent& evt);
    void on_apply_to_model(wxCommandEvent& evt);
    void on_add_new_models(wxCommandEvent& evt);
    void on_gaussian_splat_import(wxCommandEvent& evt);
    void on_config(wxCommandEvent& evt);
    void on_load(wxCommandEvent& evt);
    void on_save(wxCommandEvent& evt);

private:
    MainFrame* m_main_frame { nullptr };

    wxTextCtrl* m_text { nullptr };
    wxButton*   m_btn_from_model { nullptr };
    wxButton*   m_btn_apply_to_model { nullptr };
    wxButton*   m_btn_add_new_models { nullptr };
    wxButton*   m_btn_gaussian_splat_import { nullptr };
    wxButton*   m_btn_config { nullptr };
    wxButton*   m_btn_load { nullptr };
    wxButton*   m_btn_save { nullptr };

    wxString    m_backend_label;

    wxDECLARE_EVENT_TABLE();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_TaichiDialog_hpp_
