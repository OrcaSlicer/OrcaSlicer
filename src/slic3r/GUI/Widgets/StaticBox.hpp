#ifndef slic3r_GUI_StaticBox_hpp_
#define slic3r_GUI_StaticBox_hpp_

#include "../wxExtensions.hpp"
#include "StateHandler.hpp"
#include "WidgetAccessible.hpp"

#include <wx/anybutton.h>
#include <wx/checklst.h>
#include <wx/gdicmn.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/colour.h>
#include <wx/peninfobase.h>
#include <wx/event.h>
#include <wx/window.h>

class StaticBox : public wxWindow
{
public:
    StaticBox();

    StaticBox(wxWindow* parent,
             wxWindowID      id        = wxID_ANY,
             const wxPoint & pos       = wxDefaultPosition,
             const wxSize &  size      = wxDefaultSize,
             long style = 0);

    bool Create(wxWindow* parent,
        wxWindowID      id        = wxID_ANY,
        const wxPoint & pos       = wxDefaultPosition,
        const wxSize &  size      = wxDefaultSize,
        long style = 0);

    void SetCornerRadius(double radius);

    void SetBorderWidth(int width);

    void SetTopMargin(int margin); // ORCA

    void SetBorderColor(StateColor const & color);

    void SetBorderColorNormal(wxColor const &color);

    void SetBorderStyle(wxPenStyle style);

    void SetBackgroundColor(StateColor const &color);

    void SetBackgroundColorNormal(wxColor const &color);

    void SetBackgroundColor2(StateColor const &color);

    static wxColor GetParentBackgroundColor(wxWindow * parent);

    void ShowBadge(bool show);

    // ORCA: give this widget a real MSAA/IAccessible role instead of the
    // generic fallback custom-drawn windows get by default. extra_state is
    // an optional callback for widget-specific state bits (e.g. selected,
    // checked, expanded) on top of the focus/enabled/visible state that's
    // always derived automatically.
#if wxUSE_ACCESSIBILITY
    void SetAccessibleRole(wxAccRole role, WidgetAccessible::ExtraStateGetter extra_state = nullptr);
#endif

protected:
#if wxUSE_ACCESSIBILITY
    wxAccessible* CreateAccessible() override;
#endif

    void eraseEvent(wxEraseEvent& evt);

    void sizeEvent(wxSizeEvent& evt);

    void paintEvent(wxPaintEvent& evt);

    void render(wxDC& dc);

    virtual void doRender(wxDC& dc);

protected:
    double radius;
    int border_width = 1;
    int top_margin = 0;
    wxPenStyle border_style = wxPENSTYLE_SOLID;
    StateHandler state_handler;
    StateColor   border_color;
    StateColor   background_color;
    StateColor   background_color2;
    ScalableBitmap badge;

#if wxUSE_ACCESSIBILITY
    wxAccRole                          m_a11y_role = wxROLE_SYSTEM_CLIENT;
    WidgetAccessible::ExtraStateGetter m_a11y_extra_state;
#endif

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_StaticBox_hpp_
