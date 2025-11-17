#ifndef CAPSULE_BUTTON_HPP
#define CAPSULE_BUTTON_HPP

#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {
class CapsuleButton : public wxPanel
{
public:
    CapsuleButton(wxWindow *parent, wxWindowID id, const wxString &label, bool selected);
    void Select(bool selected);
    bool IsSelected() const { return m_selected; }
protected:
    void OnPaint(wxPaintEvent &event);
private:
    void OnEnterWindow(wxMouseEvent &event);
    void OnLeaveWindow(wxMouseEvent &event);
    void UpdateStatus();

    wxBitmapButton *m_btn;
    Label          *m_label;

    wxBitmap tag_on_bmp;
    wxBitmap tag_off_bmp;

    bool m_hovered;
    bool m_selected;
};
}} // namespace Slic3r::GUI

#endif