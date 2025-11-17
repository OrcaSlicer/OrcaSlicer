#ifndef slic3r_GUI_PopupWindow_hpp_
#define slic3r_GUI_PopupWindow_hpp_

#include <wx/popupwin.h>

class PopupWindow : public wxPopupTransientWindow
{
public:
    PopupWindow() {}

    ~PopupWindow();

    PopupWindow(wxWindow *parent, int style = wxBORDER_NONE)
        { Create(parent, style); }

    bool Create(wxWindow *parent, int flags = wxBORDER_NONE);

private:
#ifdef __WXOSX__
    void OnMouseEvent2(wxMouseEvent &evt);
    wxEvtHandler * hovered { this };
#endif

#ifdef __WXGTK__
    void topWindowActiavate(wxActivateEvent &event);
#endif
};

#endif // !slic3r_GUI_PopupWindow_hpp_
