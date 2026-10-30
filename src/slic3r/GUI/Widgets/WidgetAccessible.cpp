#include "WidgetAccessible.hpp"

#if wxUSE_ACCESSIBILITY

#include <wx/control.h>
#include <wx/window.h>
#include <utility>

WidgetAccessible::WidgetAccessible(wxWindow* win, wxAccRole role, ExtraStateGetter extra_state)
    : wxWindowAccessible(win)
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
    if (title.empty()) {
        // Many icon-only buttons never call SetName() but already have a
        // tooltip with a perfectly good label - fall back to that rather
        // than exposing no name at all. Parameter tooltips in particular are
        // a full description plus "parameter name: ..." / "Default: ..."
        // metadata (see Field::get_tooltip_text) - much too long to read
        // back as a name every time the control gets focus, so only take the
        // first paragraph; the rest is still available via GetDescription().
        title = win->GetToolTipText();
        int para_break = title.Find("\n\n");
        if (para_break != wxNOT_FOUND)
            title = title.Left(para_break);
    }
    if (title.empty())
        return wxACC_NOT_IMPLEMENTED;

    *name = title;
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::GetDescription(int childId, wxString* description)
{
    if (childId != wxACC_SELF)
        return wxACC_NOT_IMPLEMENTED;

    wxWindow* win = GetWindow();
    if (!win)
        return wxACC_FAIL;

    // Only worth exposing as a separate description when the tooltip has the
    // "name paragraph + metadata" shape and GetName() above truncated it -
    // otherwise the name already says everything the tooltip does.
    wxString tip = win->GetToolTipText();
    if (tip.empty() || tip.Find("\n\n") == wxNOT_FOUND)
        return wxACC_NOT_IMPLEMENTED;

    *description = tip;
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::GetValue(int childId, wxString* strValue)
{
    if (childId != wxACC_SELF)
        return wxACC_NOT_IMPLEMENTED;

    // A button or a tab carries no value. Left to the standard accessible, the value
    // repeats the label, and NVDA reads a tab as "Prepare Prepare".
    if (m_role != wxROLE_SYSTEM_PUSHBUTTON && m_role != wxROLE_SYSTEM_PAGETAB)
        return wxACC_NOT_IMPLEMENTED;

    strValue->clear();
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::GetRole(int childId, wxAccRole* role)
{
    if (childId != wxACC_SELF)
        return wxACC_NOT_IMPLEMENTED;

    *role = m_role;
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::GetDefaultAction(int childId, wxString* actionName)
{
    if (childId != wxACC_SELF || !m_default_action || m_default_action_name.empty())
        return wxACC_NOT_IMPLEMENTED;

    *actionName = m_default_action_name;
    return wxACC_OK;
}

wxAccStatus WidgetAccessible::DoDefaultAction(int childId)
{
    if (childId != wxACC_SELF || !m_default_action)
        return wxACC_NOT_IMPLEMENTED;

    wxWindow* win = GetWindow();
    if (!win)
        return wxACC_FAIL;
    // Assistive technology is not aware of a disabled widget being unclickable, and
    // this runs in process on the UI thread, straight into the widget's own code.
    if (!win->IsEnabled())
        return wxACC_FAIL;

    m_default_action();
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
