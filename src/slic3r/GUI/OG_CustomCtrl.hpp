#ifndef slic3r_OG_CustomCtrl_hpp_
#define slic3r_OG_CustomCtrl_hpp_

#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <wx/dc.h>
#include <wx/colour.h>
#include <cstddef>
#include <vector>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/settings.h>

#include <map>
#include <functional>
#include <wx/types.h>
#include <wx/string.h>
#include <wx/validate.h>

#include "libslic3r/Config.hpp"

#include "slic3r/GUI/wxExtensions.hpp"

#include "OptionsGroup.hpp"

class wxColour;
class wxDC;
class wxMouseEvent;
class wxPaintEvent;
class wxSizer;
class wxWindow;
namespace Slic3r::GUI { class Field; }

// Translate the ifdef 
#ifdef __WXOSX__
    #define wxOSX true
#else
    #define wxOSX false
#endif

namespace Slic3r { namespace GUI {

class OptionStateRegistry;

//  Static text shown among the options.
class OG_CustomCtrl :public wxPanel
{
    wxFont  m_font;
    int     m_v_gap;
    int     m_v_gap2;
    int     m_h_gap;
    int     m_em_unit;

    wxSize  m_bmp_mode_sz;
    wxSize  m_bmp_blinking_sz;

    ScalableBitmap m_bmp_multi_extruder; // ORCA multi extruder icon drawn on left margin for variant options
    // Breathing room between the row toggle and the neighbour it shares a row with, derived from
    // the same em unit as every other gap here.
    int     toggle_gap() const { return m_em_unit * 3 / 5; }
    // Row toggle bitmaps, the same ones ::CheckBox paints; built on first use.
    ScalableBitmap m_bmp_check_on;
    ScalableBitmap m_bmp_check_off;
    ScalableBitmap m_bmp_check_on_focused;
    ScalableBitmap m_bmp_check_off_focused;
    ScalableBitmap m_bmp_check_on_disabled;
    ScalableBitmap m_bmp_check_off_disabled;
    // Set once the six above have been constructed: every paint path calls
    // init_row_toggle_bitmaps(), and msw_rescale() rescales the loaded ones in place.
    bool m_row_toggle_bitmaps_ready{false};

    // Whether the row draws the per-extruder icon: it is keyed by a variant column and the group
    // shows more than one. Same rule render() and get_pos() must agree on.
    bool line_has_multi_extruder_icon(const Line& line) const;
    // The icon's logical width, resolved on first use (0 when the asset is missing).
    wxCoord multi_extruder_width();
    void    init_multi_extruder_bitmap();

    int     m_max_win_width{0};

    struct CtrlLine {
        wxCoord           width{ wxDefaultCoord };
        wxCoord           height{ wxDefaultCoord };
        OG_CustomCtrl*    ctrl    { nullptr };
        const Line&       og_line;

        bool draw_just_act_buttons  { false };
        bool draw_mode_bitmap       { true };
        bool is_visible             { true };
        bool is_focused             { false };

        CtrlLine(   wxCoord         height,
                    OG_CustomCtrl*  ctrl,
                    const Line&     og_line,
                    bool            draw_just_act_buttons = false,
                    bool            draw_mode_bitmap = true);
        ~CtrlLine() { ctrl = nullptr; }

        int     get_max_win_width();
        void    correct_items_positions();
        void    msw_rescale();
        void    update_visibility(ConfigOptionMode mode);

        void render_separator(wxDC& dc, wxCoord v_pos);

        void    render(wxDC& dc, wxCoord h_pos, wxCoord v_pos);
        wxCoord draw_text      (wxDC& dc, wxPoint pos, const wxString& text, const wxColour* color, int width, bool is_url = false, bool is_main = false);
        wxPoint draw_blinking_bmp(wxDC& dc, wxPoint pos, bool is_blinking);
        wxPoint draw_act_bmps(wxDC& dc, wxPoint pos, const wxBitmap& bmp_undo_to_sys, const wxBitmap& bmp_undo, bool is_blinking, size_t rect_id = 0, bool is_main = false);
        // Draws the line's opt-in row toggle in the left-hand column, before the label. Returns
        // the x the following columns continue from.
        wxPoint draw_row_toggle(wxDC& dc, wxPoint pos);
        wxCoord draw_edit_bmp(wxDC& dc, wxPoint pos, const wxBitmap& bmp_edit);
        bool    launch_browser() const;
        bool    is_separator() const { return og_line.is_separator(); }
        // Mutable view of the line's toggle; the control holds the line by reference.
        RowToggle* row_toggle() const {
            return og_line.row_toggle ? &const_cast<RowToggle&>(*og_line.row_toggle) : nullptr;
        }

        std::vector<wxRect> rects_undo_icon;
        std::vector<wxRect> rects_undo_to_sys_icon;
        std::vector<wxRect> rects_edit_icon;
        wxRect              rect_label;
        wxRect              rect_row_toggle;
        bool                is_row_toggle_hover{ false };
    };

    std::vector<CtrlLine> ctrl_lines;
    const OptionStateRegistry* m_option_state_registry{nullptr};

public:
    OG_CustomCtrl(  wxWindow* parent,
                    OptionsGroup* og,
                    const wxPoint& pos = wxDefaultPosition,
                    const wxSize& size = wxDefaultSize,
                    const wxValidator& val = wxDefaultValidator,
                    const wxString& name = wxEmptyString);
    ~OG_CustomCtrl() {}

    void set_option_state_registry(const OptionStateRegistry* registry) { m_option_state_registry = registry; }

    void    OnPaint(wxPaintEvent&);
    void    OnMotion(wxMouseEvent& event);
    void    OnLeftDown(wxMouseEvent& event);
    void    OnLeaveWin(wxMouseEvent& event);

    void    init_ctrl_lines();
    bool    update_visibility(ConfigOptionMode mode);
    void    correct_window_position(wxWindow* win, const Line& line, Field* field = nullptr);
    void    correct_widgets_position(wxSizer* widget, const Line& line, Field* field = nullptr);
    void    init_max_win_width();
    // Builds the row-toggle bitmaps on first use; they are the bitmaps ::CheckBox paints.
    void    init_row_toggle_bitmaps();
    // The toggle box's width, reserved by get_pos() and advanced by draw_row_toggle().
    wxCoord row_toggle_width();
    void    set_max_win_width(int max_win_width);
    int     get_max_win_width() { return m_max_win_width; }

    //BBS
    int    get_title_width();
    // BBS
    void fixup_items_positions();

    void    msw_rescale();
    void    sys_color_changed();

    wxPoint get_pos(const Line& line, Field* field = nullptr);
    int     get_height(const Line& line);

    OptionsGroup*  opt_group;

};

}}

#endif /* slic3r_OG_CustomCtrl_hpp_ */
