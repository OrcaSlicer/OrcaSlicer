#ifndef slic3r_MonitorPage_hpp_
#define slic3r_MonitorPage_hpp_

#include <wx/panel.h>
#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

class MonitorPage : public wxPanel
{
private:
    wxBoxSizer* m_main_sizer;
    wxBoxSizer* m_content_sizer;
public:
    MonitorPage(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~MonitorPage();
    void msw_rescale() {}
};


}
}
#endif
