#ifndef slic3r_GUI_DialogButtons_hpp_
#define slic3r_GUI_DialogButtons_hpp_

#include "wx/wx.h"
#include "wx/sizer.h"
#include "map"
#include "set"

#include "Button.hpp"
#include "Label.hpp"

#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace GUI {

class DialogButtons  : public wxPanel{
public:

    DialogButtons(wxWindow* parent, std::vector<wxString> non_translated_labels, const wxString& primary_btn_label = "",  const int left_aligned_buttons_count = 0);

    wxBoxSizer* GetSizer() const { return m_sizer; }

    Button* GetButtonFromID(wxStandardID id);

    Button* GetButtonFromLabel(wxString label);

    Button* GetButtonFromIndex(int index);

    Button* GetOK();
    Button* GetYES();
    Button* GetAPPLY();
    Button* GetCONFIRM();
    Button* GetNO();
    Button* GetCANCEL();
    Button* GetRETURN();
    Button* GetNEXT();
    Button* GetFIRST();
    Button* GetLAST();

    void SetPrimaryButton(wxString label);

    void SetAlertButton(wxString label);

    void SetLeftAlignedButtonsCount(int left_aligned_buttons_count);

    void UpdateButtons();

    ~DialogButtons();

private:
    wxWindow*            m_parent;
    wxBoxSizer*          m_sizer;
    std::vector<Button*> m_buttons;
    wxString             m_primary;
    wxString             m_alert;
    int                  m_left_aligned_buttons_count;

    // missing ones Transfer / Update / Create
    const std::map<wxString, wxStandardID> m_standardIDs = {
        // Choice
        {"ok"         , wxID_OK},
        {"yes"        , wxID_YES},
        {"apply"      , wxID_APPLY},
        {"confirm"    , wxID_APPLY}, // no id for confirm, reusing wxID_APPLY
        {"no"         , wxID_NO},
        {"cancel"     , wxID_CANCEL},
        // Action
        {"open"       , wxID_PRINT},
        {"open"       , wxID_OPEN},
        {"add"        , wxID_ADD},
        {"copy"       , wxID_COPY},
        {"new"        , wxID_NEW},
        {"save"       , wxID_SAVE},
        {"save as"    , wxID_SAVEAS},
        {"refresh"    , wxID_REFRESH},
        {"retry"      , wxID_RETRY},
        {"ignore"     , wxID_IGNORE},
        {"help"       , wxID_HELP},
        {"clone"      , wxID_DUPLICATE},
        {"duplicate"  , wxID_DUPLICATE},
        {"select all" , wxID_SELECTALL},
        {"replace"    , wxID_REPLACE},
        {"replace all", wxID_REPLACE_ALL},
        // Navigation
        {"return"     , wxID_BACKWARD}, // use return instead back. back mostly used as side of object in translations
        {"next"       , wxID_FORWARD},
        // Alert / Negative
        {"remove"     , wxID_REMOVE},
        {"delete"     , wxID_DELETE},
        {"abort"      , wxID_ABORT},
        {"stop"       , wxID_STOP},
        {"reset"      , wxID_RESET},
        {"clear"      , wxID_CLEAR},
        {"exit"       , wxID_EXIT},
        {"quit"       , wxID_EXIT}
    };

    std::set<wxStandardID> m_primaryIDs {
        wxID_OK,
        wxID_YES,
        wxID_APPLY,
        wxID_SAVE,
        wxID_PRINT
    };

    std::set<wxStandardID> m_alertIDs {
        wxID_REMOVE,
        wxID_DELETE,
        wxID_ABORT,
        wxID_STOP,
        wxID_RESET,
        wxID_CLEAR,
        wxID_EXIT
    };

    Button* PickFromList(std::set<wxStandardID> ID_list);

    int  FromDIP(int d);

    void on_dpi_changed(wxDPIChangedEvent& event);

    void on_keydown(wxKeyEvent& event);
};

}} // namespace Slic3r::GUI
#endif // !slic3r_GUI_DialogButtons_hpp_