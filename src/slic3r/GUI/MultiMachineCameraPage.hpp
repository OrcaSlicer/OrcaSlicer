#pragma once

#include <map>
#include <string>
#include <wx/panel.h>
#include <wx/timer.h>

class Label;
class wxGridSizer;
class wxScrolledWindow;

namespace Slic3r::GUI {

class LanCameraTile;

class MultiMachineCameraPage : public wxPanel
{
public:
    explicit MultiMachineCameraPage(wxWindow* parent);
    ~MultiMachineCameraPage() override;
    bool Show(bool show = true) override;
    void suspend();
    void resume();
    void rescale();
    void on_sys_color_changed();

private:
    void refresh();
    void layout_grid();
    wxTimer m_timer;
    wxScrolledWindow* m_scroll;
    wxGridSizer* m_grid;
    Label* m_empty;
    bool m_visible = false;
    bool m_suspended = false;
    std::map<std::string, LanCameraTile*> m_tiles;
};

} // namespace Slic3r::GUI
