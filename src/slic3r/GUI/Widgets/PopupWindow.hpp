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
    // On macOS and GTK wx's Popup() pushes the popup's event handlers each time, and pushing them again while
    // they are in place links each one to itself, so the next event sent through it never returns and the app
    // hangs. There this does nothing while the popup is shown, and first removes the handlers a Hide() left in
    // place. On Windows it is wx's Popup().
    void Popup(wxWindow *focus = nullptr) override;
    // For a control that opens this popup and closes it with its next click: false while the popup is
    // hidden; true while it is shown, closing it as a click outside it does (Dismiss(), then OnDismiss()).
    // On macOS and GTK a person's click never gets here with the popup shown, since the popup's mouse
    // capture (a pointer grab on GTK) takes the click and closes it first; the MCP server's clicks do, as
    // they go straight to the control. On Windows a person's click gets here with a popup that hosts
    // controls (wxPU_CONTAINS_CONTROLS) still shown, and wx then closes it from the deactivation the click
    // caused, so there this only reports that it is shown; any other popup is already closed by then.
    bool CloseOnToggle();
#ifdef __WXGTK__
    bool Show(bool show = true) override;
#endif
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
