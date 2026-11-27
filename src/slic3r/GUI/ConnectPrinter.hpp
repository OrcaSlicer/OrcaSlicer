#ifndef slic3r_GUI_ConnectPrinter_hpp_
#define slic3r_GUI_ConnectPrinter_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include <wx/artprov.h>
#include <wx/toplevel.h>
#include <wx/event.h>
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
#include <wx/choice.h>
#include <string>
#include <vector>
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

    // "Add printer by IP ..." form. The IP/port/agent widgets only exist after
    // enable_add_by_ip() was called (before ShowModal), so the classic access-code
    // dialog keeps its exact layout and behaviour.
    wxBoxSizer*             m_sizer_top{ nullptr };
    bool                    m_add_by_ip{ false };
    wxStaticText*           m_staticText_host{ nullptr };
    TextInput*              m_textCtrl_host{ nullptr };
    wxStaticText*           m_staticText_port{ nullptr };
    TextInput*              m_textCtrl_port{ nullptr };
    wxStaticText*           m_staticText_agent{ nullptr };
    wxChoice*               m_choice_agent{ nullptr };
    std::vector<std::string> m_agent_ids;
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
    // Extends the dialog with IP / port / printer-agent inputs, prefilled from the
    // currently edited printer preset (print_host, printhost_port, printer_agent).
    void enable_add_by_ip();
    bool add_by_ip_mode() const { return m_add_by_ip; }
    // Validates the IP form, stores it in the edited printer preset and runs the standard
    // GUI_App::select_machine() flow, which inserts/connects the printer.
    bool apply_ip_settings(const std::string& effective_code, const std::string& agent_id);
    void on_input_enter(wxCommandEvent& evt);
    void on_button_confirm(wxCommandEvent &event);
    void on_dpi_changed(const wxRect &suggested_rect) override;
};
}} // namespace Slic3r::GUI

#endif