#ifndef BBLSTATUSBARSEND_HPP
#define BBLSTATUSBARSEND_HPP

#include <wx/panel.h>
#include <wx/stattext.h>

#include <memory>
#include <string>
#include <functional>
#include <string>
#include <wx/hyperlink.h>

#include "Jobs/ProgressIndicator.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"

class wxTimer;
class wxGauge;
class wxButton;
class wxTimerEvent;
class wxStatusBar;
class wxWindow;
class wxFrame;
class wxString;
class wxFont;

namespace Slic3r {

class BBLStatusBarSend : public ProgressIndicator
{
    wxPanel *     m_self; // we cheat! It should be the base class but: perl!
    wxGauge *     m_prog;
    Label *       m_link_show_error;
    wxBoxSizer*   m_sizer_status_text;
    wxStaticBitmap* m_static_bitmap_show_error;
    wxBitmap      m_bitmap_show_error_close;
    wxBitmap      m_bitmap_show_error_open;
    Button *      m_cancelbutton;
    wxStaticText *m_status_text;
    wxStaticText *m_stext_percent;
    wxBoxSizer *  m_sizer;
    wxBoxSizer *  m_sizer_eline;
    wxWindow *    block_left;
    wxWindow *    block_right;

public:
    BBLStatusBarSend(wxWindow *parent = nullptr, int id = -1);
    ~BBLStatusBarSend() = default;

    int get_progress() const;
    // if the argument is less than 0 it shows the last state or
    // pulses if no state was set before.
    void        set_prog_block();
    void        set_progress(int) override;
    int         get_range() const override;
    void        set_range(int = 100) override;
    void        clear_percent() override;
    void        show_error_info(wxString msg, int code, wxString description, wxString extra) override;
    void        show_progress(bool);
    void        start_busy(int = 100);
    void        stop_busy();
    void        set_cancel_callback_fina(BBLStatusBarSend::CancelFn ccb);
    inline bool is_busy() const { return m_busy; }
    void        set_cancel_callback(CancelFn = CancelFn()) override;
    inline void reset_cancel_callback() { set_cancel_callback(); }
    wxPanel *   get_panel();
    bool        is_english_text(wxString str);
    bool        format_text(wxStaticText* dc, int width, const wxString& text, wxString& multiline_text);
    void        set_status_text(const wxString& txt);
    void        set_percent_text(const wxString &txt);
    void        msw_rescale();
    void        set_status_text(const std::string &txt);
    void        set_status_text(const char *txt) override;
    wxString    get_status_text() const;
    void        set_font(const wxFont &font);
    void        set_object_info(const wxString &txt);
    void        set_slice_info(const wxString &txt);
    void        show_slice_info(bool show);
    bool        is_slice_info_shown();
    bool        update_status(wxString &msg, bool &was_cancel, int percent = -1, bool yield = true);
    void        reset();
    // Temporary methods to satisfy Perl side
    void show_cancel_button();
    void hide_cancel_button();
    void change_button_label(wxString name);

    void disable_cancel_button();
    void enable_cancel_button();

    void    cancel();

private:
    bool     m_show_error_info_state = false;
    bool     m_busy = false;
    bool     m_was_cancelled = false;
    CancelFn m_cancel_cb;
    CancelFn m_cancel_cb_fina;
};

namespace GUI {
using Slic3r::BBLStatusBarSend;
}

wxDECLARE_EVENT(EVT_SHOW_ERROR_INFO_SEND, wxCommandEvent);
wxDECLARE_EVENT(EVT_SHOW_ERROR_FAIL_SEND, wxCommandEvent);
} // namespace Slic3r

#endif // BBLSTATUSBAR_HPP
