#include "WidgetAccessible.hpp"

#if wxUSE_ACCESSIBILITY

#include <wx/control.h>
#include <wx/window.h>
#include <utility>

WidgetAccessible::WidgetAccessible(wxWindow* win, wxAccRole role, ExtraStateGetter extra_state)
    : wxAccessible(win)
    , m_role(role)
    , m_extra_state(std::move(extra_state))
{}

wxAccStatus WidgetAccessible::GetName(int childId, wxString* name)
{
    if (childId != wxACC_SELF)
        return wxACC_NOT_IMPLEMENTED;

    wxWindow* win = GetWindow();
    if (!win)
        return wxACC_FAIL;

    wxString title = win->GetName();
    // Real wxControls (e.g. ScalableButton) route through this class because
    // wx's stock button accessible ignores SetName() - keep their visible
    // label working as the name, exactly as the stock accessible did.
    if (title.empty())
        if (const wxControl* ctrl = wxDynamicCast(win, wxControl))
            title = ctrl->GetLabelText();
    if (title.empty())
        return wxACC_NOT_IMPLEMENTED;

    *name = title;
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::GetRole(int childId, wxAccRole* role)
{
    if (childId != wxACC_SELF)
        return wxACC_NOT_IMPLEMENTED;

    *role = m_role;
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::GetState(int childId, long* state)
{
    if (childId != wxACC_SELF)
        return wxACC_NOT_IMPLEMENTED;

    wxWindow* win = GetWindow();
    if (!win)
        return wxACC_FAIL;

    long s = 0;
    if (!win->IsShown())
        s |= wxACC_STATE_SYSTEM_INVISIBLE;
    if (!win->IsEnabled())
        s |= wxACC_STATE_SYSTEM_UNAVAILABLE;
    if (win->AcceptsFocus())
        s |= wxACC_STATE_SYSTEM_FOCUSABLE;
    if (win->HasFocus())
        s |= wxACC_STATE_SYSTEM_FOCUSED;
    if (m_extra_state)
        s |= m_extra_state();

    *state = s;
    return wxACC_OK;
}

#endif // wxUSE_ACCESSIBILITY
