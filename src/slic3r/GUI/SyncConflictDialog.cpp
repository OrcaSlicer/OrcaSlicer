#include "SyncConflictDialog.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/button.h>
#include <wx/msgdlg.h>

#include <ctime>
#include <iomanip>
#include <sstream>

namespace Slic3r { namespace GUI {

SyncConflictDialog::SyncConflictDialog(wxWindow* parent, const SyncConflict& conflict)
    : DPIDialog(parent, wxID_ANY, _L("Sync Conflict"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    build_ui(conflict);
    CenterOnParent();
}

std::string SyncConflictDialog::format_time(long long timestamp)
{
    if (timestamp <= 0)
        return "Unknown";

    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm* tm   = std::localtime(&t);
    if (!tm)
        return "Unknown";

    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void SyncConflictDialog::build_ui(const SyncConflict& conflict)
{
    SetBackgroundColour(*wxWHITE);

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Title
    auto* title = new wxStaticText(this, wxID_ANY, _L("A sync conflict was detected"));
    title->SetFont(title->GetFont().Bold().Scaled(1.2f));
    main_sizer->Add(title, 0, wxALL | wxALIGN_LEFT, FromDIP(15));

    // File path
    auto* path_label = new wxStaticText(this, wxID_ANY,
        _L("File") + ": " + wxString::FromUTF8(conflict.path));
    main_sizer->Add(path_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));

    // Info grid
    auto* grid = new wxFlexGridSizer(2, 2, FromDIP(8), FromDIP(20));
    grid->AddGrowableCol(1);

    auto add_info = [&](const wxString& label, const wxString& value) {
        auto* lbl = new wxStaticText(this, wxID_ANY, label);
        lbl->SetFont(lbl->GetFont().Bold());
        grid->Add(lbl, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(this, wxID_ANY, value), 0, wxALIGN_LEFT);
    };

    add_info(_L("Local version modified"),
             wxString::FromUTF8(format_time(conflict.local_time)));
    add_info(_L("Remote version modified"),
             wxString::FromUTF8(format_time(conflict.remote_time)));

    main_sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(15));

    // Separator
    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    // Apply to all checkbox
    m_apply_all_checkbox = new wxCheckBox(this, wxID_ANY, _L("Apply to all remaining conflicts"));
    main_sizer->Add(m_apply_all_checkbox, 0, wxALL, FromDIP(15));

    // Buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto* btn_local = new wxButton(this, wxID_ANY, _L("Keep Local"));
    btn_local->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_resolution   = ConflictResolution::KeepLocal;
        m_apply_to_all = m_apply_all_checkbox->IsChecked();
        EndModal(wxID_OK);
    });

    auto* btn_remote = new wxButton(this, wxID_ANY, _L("Keep Remote"));
    btn_remote->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_resolution   = ConflictResolution::KeepRemote;
        m_apply_to_all = m_apply_all_checkbox->IsChecked();
        EndModal(wxID_OK);
    });

    auto* btn_skip = new wxButton(this, wxID_CANCEL, _L("Skip"));
    btn_skip->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_resolution   = ConflictResolution::Skip;
        m_apply_to_all = m_apply_all_checkbox->IsChecked();
        EndModal(wxID_CANCEL);
    });

    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(btn_local, 0, wxRIGHT, FromDIP(8));
    btn_sizer->Add(btn_remote, 0, wxRIGHT, FromDIP(8));
    btn_sizer->Add(btn_skip, 0);

    main_sizer->Add(btn_sizer, 0, wxALL | wxEXPAND, FromDIP(15));

    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    SetMinSize(wxSize(FromDIP(450), FromDIP(250)));
    Fit();
}

void SyncConflictDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
