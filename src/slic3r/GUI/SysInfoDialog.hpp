#ifndef slic3r_GUI_SysInfoDialog_hpp_
#define slic3r_GUI_SysInfoDialog_hpp_

#include <wx/wx.h>
#include <wx/html/htmlwin.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { 
namespace GUI {

class SysInfoDialog : public DPIDialog
{
    ScalableBitmap  m_logo_bmp;
    wxStaticBitmap* m_logo;
    wxHtmlWindow*   m_opengl_info_html;
    wxHtmlWindow*   m_html;

    wxButton*       m_btn_copy_to_clipboard;

public:
    SysInfoDialog();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    
private:
    void onCopyToClipboard(wxEvent &);
    void onCloseDialog(wxEvent &);
};
} // namespace GUI
} // namespace Slic3r

#endif
