#ifndef slic3r_GUI_SlicingProgressNotification_hpp_
#define slic3r_GUI_SlicingProgressNotification_hpp_

#include "DailyTips.hpp"
#include "NotificationManager.hpp"

namespace Slic3r { namespace GUI {


class NotificationManager::SlicingProgressNotification : public NotificationManager::PopNotification
{
public:
    // Inner state of notification, Each state changes bahaviour of the notification
    enum class SlicingProgressState
    {
        SP_NO_SLICING,        // hidden
        SP_BEGAN,             // still hidden but allows to go to SP_PROGRESS state. This prevents showing progress after slicing was canceled.
        SP_PROGRESS,          // never fades outs, no close button, has cancel button
        SP_CANCELLED,         // fades after 10 seconds, simple message
        //SP_BEFORE_COMPLETED,  // to keep displaying DailyTips for 3 seconds 
        SP_COMPLETED          // Has export hyperlink and print info, fades after 20 sec if sidebar is shown, otherwise no fade out
    };
    SlicingProgressNotification(const NotificationData& n, NotificationIDProvider& id_provider, wxEvtHandler* evt_handler, std::function<bool()> callback)
        : PopNotification(n, id_provider, evt_handler)
        , m_cancel_callback(callback)
        , m_dailytips_panel(new DailyTipsPanel(true, DailyTipsLayout::Vertical))
    {
        set_progress_state(SlicingProgressState::SP_NO_SLICING);
    }
    void                set_percentage(float percent) { m_percentage = percent; }
    DailyTipsPanel*     get_dailytips_panel() { return m_dailytips_panel; }
    SlicingProgressState get_progress_state() { return m_sp_state; }
    // sets text of notification - call after setting progress state
    void				set_status_text(const std::string& text);
    // sets cancel button callback
    void			    set_cancel_callback(std::function<bool()> callback) { m_cancel_callback = callback; }
    bool                has_cancel_callback() const { return m_cancel_callback != nullptr; }
    // sets SlicingProgressState, negative percent means canceled, returns true if state was set succesfully.
    bool				set_progress_state(float percent);
    // sets SlicingProgressState, percent is used only at progress state. Returns true if state was set succesfully.
    bool				set_progress_state(SlicingProgressState state, float percent = 0.f);
    // sets additional string of print info and puts notification into Completed state.
    void			    set_print_info(const std::string& info);
    // sets fading if in Completed state.
    void                set_sidebar_collapsed(bool collapsed);
    // Calls inherited update_state and ensures Estate goes to hidden not closing.
    bool                update_state(bool paused, const int64_t delta) override;
    // Switch between technology to provide correct text.
    void				set_fff(bool b) { m_is_fff = b; }
    void                set_export_possible(bool b) { m_export_possible = b; }
    void                on_change_color_mode(bool is_dark) override;
protected:
    void        init() override;
    void        render(GLCanvas3D& canvas, float initial_y, bool move_from_overlay, float overlay_width, float right_margin) override;
    /* PARAMS: pos is relative to screen */
    void	    render_text(const ImVec2& pos);
    void		render_bar(const ImVec2& pos, const ImVec2& size);
    void		render_cancel_button(const ImVec2& pos, const ImVec2& size);
    void		render_close_button(const ImVec2& pos, const ImVec2& size);
    void        render_dailytips_panel(const ImVec2& pos, const ImVec2& size);
    void        render_show_dailytips(const ImVec2& pos);

    void        on_show_dailytips();
    void        on_cancel_button();
    int		    get_duration() override;

protected:
    ImVec2                  m_window_pos;
    float                   m_percentage{ 0.0f };
    int64_t                 m_before_complete_start;
    // if returns false, process was already canceled
    std::function<bool()>	m_cancel_callback;
    SlicingProgressState	m_sp_state{ SlicingProgressState::SP_PROGRESS };
    bool                    m_sidebar_collapsed{ false };
    // if true, it is possible show export hyperlink in state SP_PROGRESS
    bool                    m_export_possible{ false };
    DailyTipsPanel*         m_dailytips_panel{ nullptr };

    /* currently not used */
    bool				    m_has_print_info{ false };
    std::string             m_print_info;
    bool					m_is_fff{ true };
};

}}

#endif