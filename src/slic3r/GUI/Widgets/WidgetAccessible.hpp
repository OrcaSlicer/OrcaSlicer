#ifndef slic3r_GUI_WidgetAccessible_hpp_
#define slic3r_GUI_WidgetAccessible_hpp_

#include <wx/access.h>

#if wxUSE_ACCESSIBILITY

#include <functional>

// Generic wxAccessible implementation shared by Orca's custom-drawn widgets
// (StaticBox and its subclasses, and other one-off custom wxWindows), so
// each widget only has to supply a role and, optionally, extra state bits
// instead of writing its own wxAccessible subclass.
//
// Name comes from wxWindow::SetName(); focusable/focused/enabled/visible
// state is derived generically from the window itself.
class WidgetAccessible : public wxAccessible
{
public:
    using ExtraStateGetter = std::function<long()>;

    WidgetAccessible(wxWindow* win, wxAccRole role, ExtraStateGetter extra_state = nullptr);

    wxAccStatus GetName(int childId, wxString* name) override;
    wxAccStatus GetRole(int childId, wxAccRole* role) override;
    wxAccStatus GetState(int childId, long* state) override;
    wxAccStatus GetDescription(int childId, wxString* description) override;

    void SetRole(wxAccRole role) { m_role = role; }
    void SetExtraStateGetter(ExtraStateGetter getter) { m_extra_state = std::move(getter); }

private:
    wxAccRole        m_role;
    ExtraStateGetter m_extra_state;
};

#endif // wxUSE_ACCESSIBILITY

#endif // slic3r_GUI_WidgetAccessible_hpp_
