#pragma once

#include "GUI_Utils.hpp"

#include <vector>
#include <wx/gdicmn.h>
#include <wx/string.h>

class wxGrid;
class wxWindow;

namespace Slic3r { namespace GUI {

class ReorderPlatesDialog : public DPIDialog
{
public:
    ReorderPlatesDialog(wxWindow* parent, const std::vector<wxString>& plate_names);
    std::vector<int> get_order() const;

private:
    wxGrid* m_grid;

    void alphabetize();
    void update_row_numbers();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;
};

}} // namespace Slic3r::GUI
