#ifndef _ImageDPIFrame_H_
#define _ImageDPIFrame_H_

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"

class wxStaticBitmap;
namespace Slic3r { namespace GUI {
class ImageDPIFrame : public Slic3r::GUI::DPIFrame
{
public:
    ImageDPIFrame();
    ~ImageDPIFrame() override;
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void sys_color_changed();
    void on_show();
    void on_hide();
    bool Show(bool show = true) override;

    void set_bitmap(const wxBitmap& bit_map);
    int  get_image_px() { return m_image_px; }
    void set_title(const wxString& title);

private:
    void init_timer();
    void on_timer(wxTimerEvent &event);

private:
    wxStaticBitmap *m_bitmap = nullptr;
    wxBoxSizer *m_sizer_main{nullptr};
    int             m_image_px;
    wxString        m_title_str;
    wxStaticText*   m_title;
    wxTimer *       m_refresh_timer{nullptr};
    float           m_timer_count = 0;
};
}}     // namespace Slic3r::GUI
#endif  // _STEP_MESH_DIALOG_H_
