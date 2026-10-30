#ifndef slic3r_GUI_WidgetAccessible_hpp_
#define slic3r_GUI_WidgetAccessible_hpp_

#include <wx/access.h>
#include <wx/window.h>

#if wxUSE_ACCESSIBILITY

#include <functional>

// Generic accessible implementation shared by Orca's custom-drawn widgets
// (StaticBox and its subclasses, and other one-off custom wxWindows), so
// each widget only has to supply a role and, optionally, extra state bits
// instead of writing its own wxAccessible subclass.
//
// Name comes from wxWindow::SetName(); focusable/focused/enabled/visible
// state is derived generically from the window itself.
//
// The base class must be wxWindowAccessible, not wxAccessible: only
// wxWindowAccessible implements GetChildCount()/GetChild()/GetParent()/
// Navigate() in terms of the window's child windows. wxAccessible answers
// wxACC_NOT_IMPLEMENTED to all of them, which makes wxIAccessible fall back
// to the control's standard MSAA object - and that object reports a child
// count taken from the child HWNDs while failing every get_accChild() for
// them, so a screen reader sees a strip of nameless, role-less phantom
// children and cannot reach the real ones (the tabs of a page tab list, for
// instance).
//
// The overrides below answer for wxACC_SELF only and return
// wxACC_NOT_IMPLEMENTED for a child id, which is wx's own convention (see
// wxWindowAccessible::GetName): wxIAccessible then resolves the child id
// through GetChild() and asks the child's own accessible instead.
class WidgetAccessible : public wxWindowAccessible
{
public:
    using ExtraStateGetter = std::function<long()>;
    // Runs the widget's default action - what MSAA's accDoDefaultAction performs.
    // Windows' UIA proxy builds its ExpandCollapse and Invoke patterns on top of it,
    // so a widget that reports an expandable state needs one to be operable at all.
    using DefaultActionHandler = std::function<void()>;

    WidgetAccessible(wxWindow* win, wxAccRole role, ExtraStateGetter extra_state = nullptr);

    wxAccStatus GetName(int childId, wxString* name) override;
    wxAccStatus GetRole(int childId, wxAccRole* role) override;
    wxAccStatus GetState(int childId, long* state) override;
    wxAccStatus GetDescription(int childId, wxString* description) override;
    wxAccStatus GetValue(int childId, wxString* strValue) override;
    wxAccStatus GetDefaultAction(int childId, wxString* actionName) override;
    wxAccStatus DoDefaultAction(int childId) override;

    void SetRole(wxAccRole role) { m_role = role; }
    void SetExtraStateGetter(ExtraStateGetter getter) { m_extra_state = std::move(getter); }
    // name may be empty, which leaves the action's name to the standard accessible.
    void SetDefaultAction(wxString name, DefaultActionHandler handler)
    {
        m_default_action_name = std::move(name);
        m_default_action      = std::move(handler);
    }

private:
    wxAccRole            m_role;
    ExtraStateGetter     m_extra_state;
    wxString             m_default_action_name;
    DefaultActionHandler m_default_action;
};

#endif // wxUSE_ACCESSIBILITY

#endif // slic3r_GUI_WidgetAccessible_hpp_
