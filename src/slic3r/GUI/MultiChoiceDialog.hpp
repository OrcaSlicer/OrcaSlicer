#ifndef slic3r_GUI_MultiChoiceDialog_hpp_
#define slic3r_GUI_MultiChoiceDialog_hpp_

#include "Widgets/CheckList.hpp"
#include "Widgets/DialogButtons.hpp"

#include <wx/wx.h>
#include <vector>
#include <map>


namespace Slic3r { namespace GUI {

class MultiChoiceDialog : public DPIDialog
{
public:
    MultiChoiceDialog(
        wxWindow*            parent  = nullptr,
        const wxString&      message = wxEmptyString,
        const wxString&      caption = wxEmptyString,
        const wxArrayString& choices = wxArrayString()
    );
    ~MultiChoiceDialog();

    wxArrayInt GetSelections() const;

    void SetSelections(wxArrayInt sel_array);

protected:
    CheckList*    m_check_list;
    wxArrayInt    m_selected_indices;
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

}} // namespace Slic3r::GUI

#endif