#include "PlateChangerExportOptionsDialog.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/DialogButtons.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

PlateChangerExportOptionsDialog::PlateChangerExportOptionsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Plate changer export"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);

    auto* main  = new wxBoxSizer(wxVERTICAL);
    auto* panel = new wxPanel(this, wxID_ANY);
    panel->SetBackgroundColour(*wxWHITE);

    auto* hint = new wxStaticText(panel, wxID_ANY,
                                  _L("These options control how plate-change G-code is inserted into the merged file for this export."));
    hint->Wrap(FromDIP(420));

    auto*               grid = new wxGridSizer(0, 2, FromDIP(5), FromDIP(10));
    std::vector<POItem> ops_on_off;
    ops_on_off.push_back(POItem{"on", _L("On")});
    ops_on_off.push_back(POItem{"off", _L("Off")});

    m_opt_start_with_new_plate = new PrintOption(panel, _L("Start with new plate?"),
                                                 _L("When enabled, the plate change G-code runs before the first plate so the printer "
                                                    "ejects the current plate and loads a fresh one before the print starts."),
                                                 ops_on_off, "start_with_new_plate");
    m_opt_end_with_new_plate   = new PrintOption(panel, _L("End with new plate?"),
                                                 _L("When enabled, the plate change G-code runs after the last plate so the printer ejects "
                                                      "the final plate and loads a fresh one when the job finishes."),
                                                 ops_on_off, "end_with_new_plate");

    bool start = false, end = false;
    Plater::plate_changer_prefs_load_from_appconfig(start, end);
    m_opt_start_with_new_plate->setValue(start ? "on" : "off");
    m_opt_end_with_new_plate->setValue(end ? "on" : "off");

    m_opt_start_with_new_plate->Bind(EVT_SWITCH_PRINT_OPTION, [this](wxCommandEvent&) { persist_plate_changer_prefs_to_appconfig(); });
    m_opt_end_with_new_plate->Bind(EVT_SWITCH_PRINT_OPTION, [this](wxCommandEvent&) { persist_plate_changer_prefs_to_appconfig(); });

    grid->Add(m_opt_start_with_new_plate, 0, wxEXPAND);
    grid->Add(m_opt_end_with_new_plate, 0, wxEXPAND);

    auto* panel_sizer = new wxBoxSizer(wxVERTICAL);
    panel_sizer->Add(hint, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    panel_sizer->Add(grid, 0, wxEXPAND);
    panel->SetSizer(panel_sizer);

    auto* btns = new DialogButtons(this, {"OK", "Cancel"});

    main->Add(panel, 1, wxEXPAND | wxALL, FromDIP(15));
    main->Add(btns, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));
    SetSizer(main);
    Fit();
    SetMinSize(GetSize());
    CenterOnParent();

    btns->GetOK()->Bind(wxEVT_BUTTON, &PlateChangerExportOptionsDialog::on_ok, this);
    btns->GetCANCEL()->Bind(wxEVT_BUTTON, &PlateChangerExportOptionsDialog::on_cancel, this);

    wxGetApp().UpdateDlgDarkUI(this);
}

bool PlateChangerExportOptionsDialog::start_with_new_plate() const
{
    return m_opt_start_with_new_plate && m_opt_start_with_new_plate->getValue() == "on";
}

bool PlateChangerExportOptionsDialog::end_with_new_plate() const
{
    return m_opt_end_with_new_plate && m_opt_end_with_new_plate->getValue() == "on";
}

void PlateChangerExportOptionsDialog::persist_plate_changer_prefs_to_appconfig()
{
    if (!m_opt_start_with_new_plate || !m_opt_end_with_new_plate)
        return;
    Plater::plate_changer_prefs_save_to_appconfig(start_with_new_plate(), end_with_new_plate());
}

void PlateChangerExportOptionsDialog::on_ok(wxCommandEvent&)
{
    persist_plate_changer_prefs_to_appconfig();
    EndModal(wxID_OK);
}

void PlateChangerExportOptionsDialog::on_cancel(wxCommandEvent&) { EndModal(wxID_CANCEL); }

void PlateChangerExportOptionsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    (void) suggested_rect;
    if (m_opt_start_with_new_plate)
        m_opt_start_with_new_plate->msw_rescale();
    if (m_opt_end_with_new_plate)
        m_opt_end_with_new_plate->msw_rescale();
    Layout();
    Fit();
}

}} // namespace Slic3r::GUI
