#ifndef BBLStatusBarBind_HPP
#define BBLStatusBarBind_HPP

#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/simplebook.h>


#include <memory>
#include <string>
#include <functional>
#include <string>
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
class BBLStatusBarBind : public ProgressIndicator
{
    wxPanel *     m_self; // we cheat! It should be the base class but: perl!
    wxGauge *     m_prog;
    Button *      m_cancelbutton;
    wxStaticText *m_status_text;
    wxStaticText *m_stext_percent;
    wxBoxSizer *  m_sizer;
    wxBoxSizer *  m_sizer_eline;

public:
    BBLStatusBarBind(wxWindow *parent = nullptr, int id = -1);
    ~BBLStatusBarBind() = default;

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
    void        set_cancel_callback_fina(BBLStatusBarBind::CancelFn ccb);
    inline bool is_busy() const { return m_busy; }
    void        set_cancel_callback(CancelFn = CancelFn()) override;
    inline void reset_cancel_callback() { set_cancel_callback(); }
    wxPanel *   get_panel();
    void        set_status_text(const wxString &txt);
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

private:
    bool     m_busy = false;
    bool     m_was_cancelled = false;
    CancelFn m_cancel_cb;
    CancelFn m_cancel_cb_fina;
};

namespace GUI {
using Slic3r::BBLStatusBarBind;
}

} // namespace Slic3r

#endif // BBLSTATUSBAR_HPP
