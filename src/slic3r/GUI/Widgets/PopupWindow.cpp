#include "PopupWindow.hpp"

static wxWindow *GetTopParent(wxWindow *pWindow)
{
    wxWindow *pWin = pWindow;
    while (pWin->GetParent()) {
        pWin = pWin->GetParent();
        if (auto top = dynamic_cast<wxNonOwnedWindow*>(pWin))
            return top;
    }
    return pWin;
}

bool PopupWindow::Create(wxWindow *parent, int style)
{
    if (!wxPopupTransientWindow::Create(parent, style))
        return false;
#ifdef __WXGTK__
    GetTopParent(parent)->Bind(wxEVT_ACTIVATE, &PopupWindow::topWindowActiavate, this);
#endif
#ifdef __WXOSX__
    if (style & wxPU_CONTAINS_CONTROLS)
    for (auto evt : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_LEFT_DCLICK, wxEVT_MOTION, wxEVT_MOUSEWHEEL})
        Bind(evt, &PopupWindow::OnMouseEvent2, this);
#endif
    return true;
}

PopupWindow::~PopupWindow()
{
#ifdef __WXGTK__
    GetTopParent(this)->Unbind(wxEVT_ACTIVATE, &PopupWindow::topWindowActiavate, this);
#endif
#ifdef __WXMSW__
    GetTopParent(this)->Unbind(wxEVT_ACTIVATE, &PopupWindow::topWindowActivate, this);
    GetTopParent(this)->Unbind(wxEVT_ICONIZE, &PopupWindow::topWindowIconize, this);
    GetTopParent(this)->Unbind(wxEVT_SHOW, &PopupWindow::topWindowShow, this);
#endif
}

#ifdef __WXOSX__

static wxEvtHandler * HitTest(wxWindow * parent, wxMouseEvent &evt)
{
    auto pt = evt.GetPosition();
    const wxWindowList &children = parent->GetChildren();
    for (auto w : children) {
        wxRect rc { w->GetPosition(), w->GetSize() };
        if (rc.Contains(pt)) {
            evt.SetPosition(pt - rc.GetTopLeft());
            if (auto child = HitTest(w, evt))
                return child;
            return w;
        }
    }
    return nullptr;
}

void PopupWindow::OnMouseEvent2(wxMouseEvent &evt)
{
    auto child = ::HitTest(this, evt);
    if (evt.GetEventType() == wxEVT_MOTION) {
        auto h = child ? child : this;
        if (hovered != h) {
            wxMouseEvent leave(wxEVT_LEAVE_WINDOW);
            leave.SetEventObject(hovered);
            leave.SetId(static_cast<wxWindow*>(hovered)->GetId());
            hovered->ProcessEventLocally(leave);
            hovered = h;
            wxMouseEvent enter(wxEVT_ENTER_WINDOW);
            enter.SetEventObject(hovered);
            enter.SetId(static_cast<wxWindow*>(hovered)->GetId());
            hovered->ProcessEventLocally(enter);
        }
    }
    if (child) {
        child->ProcessEventLocally(evt);
    } else {
        evt.Skip();
    }
}

#endif

#ifdef __WXGTK__
void PopupWindow::topWindowActiavate(wxActivateEvent &event)
{
    event.Skip();
    if (!event.GetActive() && IsShown()) DismissAndNotify();
}
#endif

#ifdef __WXMSW__
void PopupWindow::BindUnfocusEvent()
{
    GetTopParent(this)->Bind(wxEVT_ACTIVATE, &PopupWindow::topWindowActivate, this);
    GetTopParent(this)->Bind(wxEVT_ICONIZE, &PopupWindow::topWindowIconize, this);
    GetTopParent(this)->Bind(wxEVT_SHOW, &PopupWindow::topWindowShow, this);
}

void PopupWindow::topWindowActivate(wxActivateEvent &event)
{
    if (!event.GetActive())
        Dismiss();
}

void PopupWindow::topWindowIconize(wxIconizeEvent &event)
{
    event.Skip();
    if (event.IsIconized())
        Dismiss();
}

void PopupWindow::topWindowShow(wxShowEvent &event)
{
    event.Skip();
    if (!event.IsShown())
        Dismiss();
}
#endif
