#ifndef slic3r_GUI_CHECKLIST_hpp_
#define slic3r_GUI_CHECKLIST_hpp_

#include "../wxExtensions.hpp"

#include "Label.hpp"
#include "TextInput.hpp"

#include <wx/wx.h>
#include <vector>
#include <wx/scrolwin.h>
#include <wx/menu.h>

class CheckList : public wxWindow
{
public:
    CheckList(
        wxWindow* parent,
        const wxArrayString& choices = wxArrayString(),
        long scroll_style = wxVSCROLL
    );

    wxArrayInt GetSelections();
    void SetSelections(wxArrayInt sel_array);
    void Check(int i, bool checked);
    bool IsChecked(int i);

    void Filter(const wxString& filterText);
    void SelectAll(bool value);
    void SelectVisible(bool value);

private:
    void ShowMenu(wxMouseEvent &e);

    std::vector<wxCheckBox*> m_checks;
    int               m_list_size;
    bool              m_first_load;
    wxBoxSizer*       w_sizer;

    wxPanel*          f_bar;
    wxBoxSizer*       f_sizer;
    TextInput*        m_filter_box;
    wxTextCtrl*       m_filter_ctrl;
    wxBoxSizer*       fb_sizer;
    wxStaticBitmap*   m_menu_button;

    wxScrolledWindow* m_scroll_area;
    wxBoxSizer*       s_sizer;

    wxStaticText*     m_info;
    wxString          m_info_nonsel;
    wxString          m_info_allsel;
    wxString          m_info_empty;

    ScalableBitmap    m_search;
    ScalableBitmap    m_menu;
};

#endif // !slic3r_GUI_CheckList_hpp_
