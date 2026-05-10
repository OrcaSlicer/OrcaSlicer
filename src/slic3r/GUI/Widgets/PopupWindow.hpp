#ifndef slic3r_GUI_PopupWindow_hpp_
#define slic3r_GUI_PopupWindow_hpp_

#include <wx/popupwin.h>
#include <wx/event.h>

class PopupWindow : public wxPopupTransientWindow
{
public:
    PopupWindow() {}

    ~PopupWindow();

    PopupWindow(wxWindow *parent, int style = wxBORDER_NONE) { Create(parent, style); }

    bool Create(wxWindow *parent, int flags = wxBORDER_NONE);
#ifdef __WXMSW__
    void BindUnfocusEvent();
#endif
protected:
    // Orca: Hook so derived classes (e.g. DropDown chains) can skip auto-dismissal
    // when the toplevel deactivates as a side effect of their own popup grab
    // (notably on Wayland, where mapping a chained xdg_popup with grab makes
    // the parent toplevel briefly inactive).
    virtual bool ShouldDismissOnTopWindowDeactivate() { return true; }
private:
#ifdef __WXOSX__
    void OnMouseEvent2(wxMouseEvent &evt);
    wxEvtHandler * hovered { this };
#endif

#ifdef __WXGTK__
    void topWindowActiavate(wxActivateEvent &event);
#endif

#ifdef __WXMSW__
    void topWindowActivate(wxActivateEvent &event);
    void topWindowIconize(wxIconizeEvent &event);
    void topWindowShow(wxShowEvent &event);
#endif
};

#endif // !slic3r_GUI_PopupWindow_hpp_
