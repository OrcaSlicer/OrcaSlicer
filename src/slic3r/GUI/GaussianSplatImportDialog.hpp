#ifndef slic3r_GUI_GaussianSplatImportDialog_hpp_
#define slic3r_GUI_GaussianSplatImportDialog_hpp_

#include "GUI_Utils.hpp"

#include <string>
#include <vector>

class wxButton;
class wxListBox;
class wxTextCtrl;
class wxSpinCtrlDouble;

namespace Slic3r {

struct GaussianSplatImportSpec;

namespace GUI {

class GaussianSplatImportDialog final : public DPIDialog
{
public:
    explicit GaussianSplatImportDialog(wxWindow* parent);
    ~GaussianSplatImportDialog() override = default;

    GaussianSplatImportSpec get_spec() const;

private:
    void on_dpi_changed(const wxRect& suggested_rect) override {}

    void on_add_images(wxCommandEvent& evt);
    void on_add_folder(wxCommandEvent& evt);
    void on_remove_selected(wxCommandEvent& evt);
    void on_clear(wxCommandEvent& evt);

    void add_paths(const wxArrayString& paths);
    void refresh_buttons();

private:
    wxTextCtrl*       m_object_name { nullptr };
    wxListBox*        m_images { nullptr };
    wxSpinCtrlDouble* m_preview_cube_mm { nullptr };

    wxButton* m_btn_add_images { nullptr };
    wxButton* m_btn_add_folder { nullptr };
    wxButton* m_btn_remove { nullptr };
    wxButton* m_btn_clear { nullptr };

    wxDECLARE_EVENT_TABLE();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_GaussianSplatImportDialog_hpp_
