#include "AddNozzleSizeDialog.hpp"

#include "libslic3r/Preset.hpp"        // format_printer_variant
#include "libslic3r/LocalesUtils.hpp"  // string_to_double_decimal_point

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

// The codebase's only formal nozzle-diameter bound (PrintConfig.cpp: nozzle_diameter def->max = 100,
// no min). We additionally require a positive value, matching format_nozzle_diameter's <=0 sentinel.
static constexpr double NOZZLE_DIAMETER_MAX = 100.0;

AddNozzleSizeDialog::AddNozzleSizeDialog(wxWindow* parent, const std::string& user_model,
                                         const std::vector<std::string>& addable_sizes,
                                         const std::vector<std::string>& existing_sizes)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Add nozzle size"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_sizes(addable_sizes)
    , m_existing(existing_sizes.begin(), existing_sizes.end())
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Select the nozzle sizes to add to \"%s\":"), from_u8(user_model)));
    msg->SetFont(Label::Body_13);
    msg->Wrap(FromDIP(360));
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(12));

    wxArrayString choices;
    for (const std::string& s : m_sizes) choices.Add(from_u8(s) + " mm");
    m_check_list = new CheckList(this, choices);
    w_sizer->Add(m_check_list, 1, wxEXPAND | wxRIGHT | wxLEFT | wxTOP, FromDIP(12));

    // Custom-size entry for diameters the system model does not ship.
    wxStaticText* custom_label = new wxStaticText(this, wxID_ANY, _L("Can't find your nozzle size? Enter a custom size (mm):"));
    custom_label->SetFont(Label::Body_13);
    custom_label->Wrap(FromDIP(360));
    w_sizer->Add(custom_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_custom_input = new ::TextInput(this, wxString(), wxEmptyString, wxEmptyString, wxDefaultPosition,
        wxSize(FromDIP(120), -1), 0); // no wxTE_PROCESS_ENTER: let Enter trigger the dialog's OK button
    w_sizer->Add(m_custom_input, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // Inline validation message for the custom size, orange like SavePresetDialog.
    m_valid_label = new wxStaticText(this, wxID_ANY, "");
    m_valid_label->SetForegroundColour(wxColour(255, 111, 0));
    w_sizer->Add(m_valid_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok_btn = dlg_btns->GetOK();
    dlg_btns->GetOK()->Bind(    wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_ok_btn->IsEnabled()) EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    w_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_custom_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { update_valid(); e.Skip(); });

    SetSizer(w_sizer);
    Layout();
    w_sizer->Fit(this);
    update_valid();
    wxGetApp().UpdateDlgDarkUI(this);
}

bool AddNozzleSizeDialog::parse_custom(double& diameter_out, std::string& variant_out) const
{
    std::string s = into_u8(m_custom_input->GetTextCtrl()->GetValue());
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    if (s.empty())
        return false;
    // Locale-safe full-string parse: reject trailing garbage ("0.4x") that atof/stod would accept.
    size_t pos = 0;
    const double d = string_to_double_decimal_point(s, &pos);
    if (pos != s.size() || d <= 0.0 || d > NOZZLE_DIAMETER_MAX)
        return false;
    diameter_out = d;
    variant_out  = format_printer_variant(d);
    return true;
}

void AddNozzleSizeDialog::update_valid()
{
    const std::string raw = into_u8(m_custom_input->GetTextCtrl()->GetValue());
    // trim for the empty check
    const bool empty = raw.find_first_not_of(" \t") == std::string::npos;

    wxString info;
    bool     ok = true;
    if (!empty) {
        double d = 0.0; std::string variant;
        if (!parse_custom(d, variant)) {
            info = _L("The entered nozzle diameter is invalid, please re-enter:");
            ok = false;
        } else if (m_existing.count(variant)) {
            info = _L("This nozzle size already exists on this printer.");
            ok = false;
        }
    }
    m_valid_label->SetLabel(info);
    m_valid_label->Show(!info.IsEmpty());
    if (m_ok_btn) m_ok_btn->Enable(ok);
    Layout();
    Fit();
}

std::vector<std::string> AddNozzleSizeDialog::get_checked_sizes() const
{
    std::vector<std::string> out;
    for (int idx : m_check_list->GetSelections())
        if (idx >= 0 && idx < (int) m_sizes.size())
            out.push_back(m_sizes[idx]);
    return out;
}

std::string AddNozzleSizeDialog::get_custom_variant() const
{
    double d = 0.0; std::string variant;
    return parse_custom(d, variant) ? variant : std::string();
}

AddNozzleSizeDialog::~AddNozzleSizeDialog() {}

}} // namespace Slic3r::GUI
