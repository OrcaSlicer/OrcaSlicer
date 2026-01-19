#include "MonitorPage.hpp"

namespace Slic3r {
namespace GUI {

MonitorPage::MonitorPage(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    :wxPanel(parent, id, pos, size, style)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_content_sizer = new wxBoxSizer(wxVERTICAL);

    m_main_sizer->Add(m_content_sizer, 1, wxEXPAND);
    SetSizerAndFit(m_main_sizer);
}

MonitorPage::~MonitorPage()
{
}


}
}