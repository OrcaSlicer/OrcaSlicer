#include "ReorderPlatesDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "PluginSort.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/StateColor.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/dynarray.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/toplevel.h>

namespace Slic3r { namespace GUI {

ReorderPlatesDialog::ReorderPlatesDialog(wxWindow* parent, const std::vector<wxString>& plate_names)
    : DPIDialog(parent, wxID_ANY, _L("Reorder Plates"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* instruction = new wxStaticText(this, wxID_ANY, _L("Drag the numbered row headers to reorder the plates."));
    instruction->Wrap(FromDIP(420));
    sizer->Add(instruction, 0, wxEXPAND | wxALL, FromDIP(12));

    m_grid = new wxGrid(this, wxID_ANY);
    m_grid->CreateGrid(static_cast<int>(plate_names.size()), 1);
    m_grid->SetCornerLabelValue(_L("Plate"));
    m_grid->SetColLabelValue(0, _L("Name"));
    m_grid->EnableEditing(false);
    m_grid->SetSelectionMode(wxGrid::wxGridSelectRows);
    m_grid->EnableDragRowMove();
    m_grid->DisableDragRowSize();
    m_grid->DisableDragColMove();
    m_grid->SetDefaultCellOverflow(false);
    for (int row = 0; row < static_cast<int>(plate_names.size()); ++row)
        m_grid->SetCellValue(row, 0, plate_names[row]);
    update_row_numbers();
    m_grid->Bind(wxEVT_GRID_ROW_MOVE, [this](wxGridEvent& event) {
        // Let wxGrid perform its built-in move before numbering the displayed rows.
        event.Skip();
        CallAfter(&ReorderPlatesDialog::update_row_numbers);
    });
    sizer->Add(m_grid, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    auto* buttons = new DialogButtons(this, {L("Alphabetize"), "Save", "Cancel"}, wxEmptyString, 1);
    buttons->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { alphabetize(); });
    buttons->GetButtonFromID(wxID_SAVE)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_SAVE); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    SetAffirmativeId(wxID_SAVE);
    SetEscapeId(wxID_CANCEL);

    on_sys_color_changed();
    on_dpi_changed(wxRect());
    SetSizerAndFit(sizer);
    CenterOnParent();
}

std::vector<int> ReorderPlatesDialog::get_order() const
{
    std::vector<int> order;
    order.reserve(m_grid->GetNumberRows());
    for (int pos = 0; pos < m_grid->GetNumberRows(); ++pos)
        order.push_back(m_grid->GetRowAt(pos));
    return order;
}

void ReorderPlatesDialog::alphabetize()
{
    auto order = get_order();
    std::vector<std::string> names;
    names.reserve(order.size());
    for (int row = 0; row < m_grid->GetNumberRows(); ++row)
        names.push_back(into_u8(m_grid->GetCellValue(row, 0)));
    std::stable_sort(order.begin(), order.end(), [&names](int lhs, int rhs) {
        if (names[lhs].empty() != names[rhs].empty())
            return !names[lhs].empty();
        return compare_ascii_case_insensitive_natural(names[lhs], names[rhs]) < 0;
    });
    m_grid->SetRowsOrder(wxArrayInt(order.begin(), order.end()));
    update_row_numbers();
}

void ReorderPlatesDialog::update_row_numbers()
{
    for (int pos = 0; pos < m_grid->GetNumberRows(); ++pos)
        m_grid->SetRowLabelValue(m_grid->GetRowAt(pos), wxString::Format("%d", pos + 1));
}

void ReorderPlatesDialog::on_dpi_changed(const wxRect&)
{
    m_grid->SetMinSize(FromDIP(wxSize(420, 280)));
    m_grid->SetRowLabelSize(FromDIP(60));
    m_grid->SetColSize(0, FromDIP(360));
    Layout();
}

void ReorderPlatesDialog::on_sys_color_changed()
{
    wxGetApp().UpdateDlgDarkUI(this);
    m_grid->SetDefaultCellBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_grid->SetDefaultCellTextColour(StateColor::darkModeColorFor(*wxBLACK));
    m_grid->SetLabelBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_grid->SetLabelTextColour(StateColor::darkModeColorFor(*wxBLACK));
    m_grid->SetGridLineColour(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
    m_grid->Refresh();
}

}} // namespace Slic3r::GUI
