#ifndef slic3r_GUI_DailyTips_hpp_
#define slic3r_GUI_DailyTips_hpp_

#include "HintNotification.hpp"

//#include <wx/time.h>
#include <string>
#include <vector>
#include <memory>

namespace Slic3r { namespace GUI {

enum class DailyTipsLayout{
    Horizontal,
    Vertical
};

class DailyTipsDataRenderer;
class DailyTipsPanel {
    static int uid;
public:
    DailyTipsPanel(bool can_expand = true, DailyTipsLayout layout = DailyTipsLayout::Vertical);
    void set_position(const ImVec2& pos);
    void set_size(const ImVec2& size);
    void set_can_expand(bool can_expand);
    ImVec2 get_size();
    void render();
    void retrieve_data_from_hint_database(HintDataNavigation nav);
    void expand(bool expand = true);
    void collapse();
    bool is_expanded();
    void on_change_color_mode(bool is_dark);
    void set_fade_opacity(float opacity);

protected:
    void render_controller_buttons(const ImVec2& pos, const ImVec2& size);
    void push_styles();
    void pop_styles();

private:
    std::unique_ptr<DailyTipsDataRenderer> m_dailytips_renderer;
    size_t m_page_index{ 0 };
    int m_pages_count;
    bool m_is_expanded{ true };
    bool m_can_expand{ true };
    ImVec2 m_pos;
    float m_width;
    float m_height;
    float m_content_height;
    float m_footer_height;
    int m_uid;
    bool m_first_enter{ false };
    bool m_is_dark{ false };
    DailyTipsLayout m_layout{ DailyTipsLayout::Vertical };
    float m_fade_opacity{ 1.0f };
};

class DailyTipsWindow {
public:
    DailyTipsWindow();
    void open();
    void close();
    void render();
    void on_change_color_mode(bool is_dark);

private:
    DailyTipsPanel* m_panel{ nullptr };
    bool m_show{ false };
    bool m_is_dark{ false };
};

}}

#endif