#ifndef slic3r_GUI_SIDETOOLS_hpp_
#define slic3r_GUI_SIDETOOLS_hpp_

#include <wx/dcgraph.h>
#include <wx/gdicmn.h>
#include <wx/dcclient.h>
#include "Button.hpp"
#include "Label.hpp"
#include "HyperLink.hpp" // ORCA
#include "../GUI/Tabbook.hpp"
#include "../DeviceManager.hpp"
#include "../wxExtensions.hpp"

#define SIDE_TOOLS_GREY900 wxColour(38, 46, 48)
#define SIDE_TOOLS_GREY600 wxColour(144, 144, 144)
#define SIDE_TOOLS_GREY400 wxColour(206, 206, 206)
#define SIDE_TOOLS_BRAND wxColour(0, 150, 136)
#define SIDE_TOOLS_LIGHT_GREEN wxColour(219, 253, 231)

enum WifiSignal {
    NONE,
    WEAK,
    MIDDLE,
    STRONG,
    WIRED,
};

enum MonitorStatus {
    MONITOR_UNKNOWN = 0,
    MONITOR_NORMAL = 1 << 1,
    MONITOR_NO_PRINTER = 1 << 2,
    MONITOR_DISCONNECTED = 1 << 3,
    MONITOR_DISCONNECTED_SERVER = 1 << 4,
    MONITOR_CONNECTING = 1 << 5,
};

#define SIDE_TOOL_CLICK_INTERVAL 20

namespace Slic3r { namespace GUI {

class SideToolsPanel : public wxPanel
{
private:
    WifiSignal      m_wifi_type{WifiSignal::NONE};
    wxString        m_dev_name;
    bool            m_hover{false};
    bool            m_click{false};
    bool            m_none_printer{true};
    int             last_printer_signal = 0;

    ScalableBitmap  m_printing_img;
    ScalableBitmap  m_arrow_img;

    ScalableBitmap  m_none_printing_img;
    ScalableBitmap  m_none_arrow_img;
    ScalableBitmap  m_none_add_img;

    ScalableBitmap  m_wifi_none_img;
    ScalableBitmap  m_wifi_weak_img;
    ScalableBitmap  m_wifi_middle_img;
    ScalableBitmap  m_wifi_strong_img;
    ScalableBitmap  m_network_wired_img;

protected:
    wxStaticBitmap *m_bitmap_info;
    wxStaticBitmap *m_bitmap_bind;
    wxTimer *       m_intetval_timer{nullptr};
    bool            m_is_in_interval {false};

public:
    SideToolsPanel(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~SideToolsPanel();

    void set_none_printer_mode();
    void on_timer(wxTimerEvent &event);
    void set_current_printer_name(std::string dev_name);
    void set_current_printer_signal(WifiSignal sign);;
    void start_interval();
    void stop_interval(wxTimerEvent &event);
    bool is_in_interval();
    void msw_rescale();

protected:
    void OnPaint(wxPaintEvent &event);
    void render(wxDC &dc);
    void doRender(wxDC &dc);
    void on_mouse_enter(wxMouseEvent &evt);
    void on_mouse_leave(wxMouseEvent &evt);
    void on_mouse_left_down(wxMouseEvent &evt);
    void on_mouse_left_up(wxMouseEvent &evt);
};

class SideTools : public wxPanel
{
public:
    SideTools(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    ~SideTools();

private:
    SideToolsPanel* m_side_tools{ nullptr };
    Tabbook*        m_tabpanel{ nullptr };
    HyperLink* m_link_network_state{ nullptr }; // ORCA
    Label* m_st_txt_error_code{ nullptr };
    Label* m_st_txt_error_desc{ nullptr };
    Label* m_st_txt_extra_info{ nullptr };
    wxWindow* m_side_error_panel{ nullptr };
    Button* m_connection_info{ nullptr };
    HyperLink* m_hyperlink{ nullptr }; // ORCA
    ScalableButton* m_more_button{ nullptr };
    ScalableBitmap      m_more_err_open;
    ScalableBitmap      m_more_err_close;
    bool                m_more_err_state{ false };

public:
    void set_table_panel(Tabbook* tb) {m_tabpanel = tb;};
    void msw_rescale();
    bool is_in_interval();
    void set_current_printer_name(std::string dev_name);
    void set_current_printer_signal(WifiSignal sign);
    void set_none_printer_mode();
    void start_interval();
    void update_status(MachineObject* obj);
    void update_connect_err_info(int code, wxString desc, wxString info);
    void show_status(int status);

public:
    SideToolsPanel* get_panel() {return m_side_tools;};
};
}} // namespace Slic3r::GUI

#endif // !slic3r_GUI_SIDETOOLS_hpp_
