#pragma once

#include <wx/dialog.h>
#include <wx/panel.h>

#include "GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "Widgets/Label.hpp"

class wxStaticText;
class wxStaticBitmap;

namespace Slic3r {

namespace GUI {

class PurgeModeBtnPanel : public wxPanel
{
public:
    PurgeModeBtnPanel(wxWindow *parent, const wxString &label, const wxString &detail, const std::string &icon_path);
    void Select(bool selected);

protected:
    void OnPaint(wxPaintEvent &event);

private:
    void OnEnterWindow(wxMouseEvent &event);
    void OnLeaveWindow(wxMouseEvent &event);

    void UpdateStatus();

    wxBitmap icon;
    wxBitmap check_icon;

    wxStaticBitmap *m_btn;
    wxStaticBitmap *m_check_btn;
    wxStaticText   *m_label;
    Label          *m_detail;
    bool            m_hover{false};
    bool            m_selected{false};
};

enum class PurgeModeDialogType {
    MultiNozzle,
    FastMode
};

class PurgeModeDialog : public DPIDialog
{
public:
    PurgeModeDialog(wxWindow *parent, PurgeModeDialogType dialog_type = PurgeModeDialogType::MultiNozzle);

    PrimeVolumeMode get_selected_mode() const { return m_selected_mode; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void select_option(PrimeVolumeMode mode);
    void update_panel_selection();

    PurgeModeBtnPanel  *m_standard_panel;
    PurgeModeBtnPanel  *m_saving_panel;
    PrimeVolumeMode     m_selected_mode;
    PurgeModeDialogType m_dialog_type;
};

} // namespace GUI
} // namespace Slic3r
