#ifndef slic3r_GUI_ConnectPrinter_hpp_
#define slic3r_GUI_ConnectPrinter_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/statbmp.h>
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"
#include "DeviceManager.hpp"

namespace Slic3r { namespace GUI {
class ConnectPrinterDialog : public DPIDialog
{
private:
protected:
    wxStaticText *  m_staticText_connection_code;
    TextInput *     m_textCtrl_code;
    Button *        m_button_confirm;
    wxStaticText*   m_staticText_hints;
    wxStaticBitmap* m_bitmap_diagram;
    wxBitmap        m_diagram_bmp;
    wxImage         m_diagram_img;

    MachineObject*  m_obj{ nullptr };
    wxString        m_input_access_code;
public:
    ConnectPrinterDialog(wxWindow *      parent,
                         wxWindowID      id    = wxID_ANY,
                         const wxString &title = wxEmptyString,
                         const wxPoint & pos   = wxDefaultPosition,
                         const wxSize &  size  = wxDefaultSize,
                         long            style = wxCLOSE_BOX | wxCAPTION);

    ~ConnectPrinterDialog();

    void end_modal(wxStandardID id);
    void init_bitmap();
    void set_machine_object(MachineObject* obj);
    void on_input_enter(wxCommandEvent& evt);
    void on_button_confirm(wxCommandEvent &event);
    void on_dpi_changed(const wxRect &suggested_rect) override;
};
}} // namespace Slic3r::GUI

#endif