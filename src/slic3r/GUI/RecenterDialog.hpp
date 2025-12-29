#ifndef slic3r_GUI_RecenterDialog_hpp_
#define slic3r_GUI_RecenterDialog_hpp_

#include "GUI_Utils.hpp"
#include <wx/statbmp.h>
#include "Widgets/Button.hpp"
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {
class RecenterDialog : public DPIDialog
{
private:
    wxStaticText* m_staticText_hint;
    wxStaticBitmap* m_bitmap_home;
    ScalableBitmap  m_home_bmp;
    wxString hint1;
    wxString hint2;

    void init_bitmap();
    void OnPaint(wxPaintEvent& event);
    void render(wxDC& dc);
    void on_button_confirm(wxCommandEvent& event);
    void on_button_close(wxCommandEvent& event);
    void on_dpi_changed(const wxRect& suggested_rect) override;

public:
    RecenterDialog(wxWindow* parent,
        wxWindowID      id = wxID_ANY,
        const wxString& title = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = wxCLOSE_BOX | wxCAPTION);

    ~RecenterDialog();
};
}} // namespace Slic3r::GUI

#endif