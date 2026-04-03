#include "PlateChangerExportOptionsDialog.hpp"
#include "PlateChangerPlateStatsTable.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <cstring>

namespace Slic3r { namespace GUI {

namespace {

enum class ExportProjectNameValid { Ok, Bad };

} // namespace

PlateChangerExportOptionsDialog::PlateChangerExportOptionsDialog(wxWindow* parent, Plater* plater)
    : DPIDialog(parent, wxID_ANY, _L("Plate changer export"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
{
    SetBackgroundColour(*wxWHITE);

    if (m_plater) {
        m_current_project_name = from_u8(filter_characters(m_plater->get_project_name().ToUTF8().data(), "<>[]:/\\|?*\""));
    }

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

    if (m_plater) {
        m_plate_stats_panel = new wxPanel(panel, wxID_ANY);
        m_plate_stats_panel->SetBackgroundColour(*wxWHITE);
        auto* stats_vsizer = new wxBoxSizer(wxVERTICAL);

        m_project_rename_book = new wxSimplebook(m_plate_stats_panel, wxID_ANY);
        m_project_rename_book->SetMinSize(wxSize(FromDIP(420), FromDIP(28)));
        m_project_rename_book->SetMaxSize(wxSize(FromDIP(520), FromDIP(28)));

        m_project_rename_normal_panel = new wxPanel(m_project_rename_book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
        m_project_rename_normal_panel->SetBackgroundColour(*wxWHITE);
        auto* rename_sizer_v = new wxBoxSizer(wxVERTICAL);
        auto* rename_sizer_h = new wxBoxSizer(wxHORIZONTAL);

        m_export_project_rename_text = new wxStaticText(m_project_rename_normal_panel, wxID_ANY, m_current_project_name, wxDefaultPosition,
                                                        wxDefaultSize, wxST_ELLIPSIZE_END);
        m_export_project_rename_text->SetFont(Label::Head_14);
        m_export_project_rename_text->SetBackgroundColour(*wxWHITE);
        m_export_project_rename_text->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        m_export_project_rename_text->SetMaxSize(wxSize(FromDIP(390), -1));

        m_export_project_rename_btn = new Button(m_project_rename_normal_panel, "", "rename_edit", wxBORDER_NONE, FromDIP(13));
        m_export_project_rename_btn->SetBackgroundColor(*wxWHITE);
        m_export_project_rename_btn->SetBackgroundColour(*wxWHITE);
        m_export_project_rename_btn->Bind(wxEVT_BUTTON, &PlateChangerExportOptionsDialog::on_export_project_rename_click, this);

        rename_sizer_h->Add(m_export_project_rename_text, 0, wxALIGN_CENTER_VERTICAL, 0);
        rename_sizer_h->Add(m_export_project_rename_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
        rename_sizer_v->Add(rename_sizer_h, 1, wxALIGN_CENTER_VERTICAL, 0);
        m_project_rename_normal_panel->SetSizer(rename_sizer_v);
        m_project_rename_normal_panel->Layout();

        auto* rename_edit_panel = new wxPanel(m_project_rename_book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
        rename_edit_panel->SetBackgroundColour(*wxWHITE);
        auto* rename_edit_sizer_v = new wxBoxSizer(wxVERTICAL);

        m_export_project_rename_input = new TextInput(rename_edit_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition,
                                                      wxDefaultSize, wxTE_PROCESS_ENTER);
        m_export_project_rename_input->GetTextCtrl()->SetFont(Label::Body_13);
        m_export_project_rename_input->SetSize(wxSize(FromDIP(400), FromDIP(24)));
        m_export_project_rename_input->SetMinSize(wxSize(FromDIP(400), FromDIP(24)));
        m_export_project_rename_input->SetMaxSize(wxSize(FromDIP(400), FromDIP(24)));
        m_export_project_rename_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { on_export_project_rename_enter(); });
        m_export_project_rename_input->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
            if (!m_export_project_rename_input->HasFocus() && m_export_project_rename_text && !m_export_project_rename_text->HasFocus())
                on_export_project_rename_enter();
            else
                e.Skip();
        });
        rename_edit_sizer_v->Add(m_export_project_rename_input, 1, wxALIGN_CENTER_VERTICAL, 0);
        rename_edit_panel->SetSizer(rename_edit_sizer_v);
        rename_edit_panel->Layout();

        m_project_rename_book->AddPage(m_project_rename_normal_panel, wxEmptyString, true);
        m_project_rename_book->AddPage(rename_edit_panel, wxEmptyString, false);

        const int n_plates         = m_plater->get_partplate_list().get_plate_count();
        m_stext_export_plate_count = new wxStaticText(m_plate_stats_panel, wxID_ANY, wxString::Format(_L("Exporting %d plates."), n_plates),
                                                      wxDefaultPosition, wxDefaultSize);
        m_stext_export_plate_count->SetFont(Label::Body_13);

        m_plate_stats_grid_sizer = new wxFlexGridSizer(0, 3, FromDIP(1), FromDIP(1));
        stats_vsizer->Add(m_project_rename_book, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        stats_vsizer->Add(m_stext_export_plate_count, 0, wxBOTTOM, FromDIP(8));
        stats_vsizer->Add(m_plate_stats_grid_sizer, 0, wxEXPAND);

        m_plate_stats_panel->SetSizer(stats_vsizer);
        populate_plate_changer_time_weight_grid(m_plate_stats_grid_sizer, m_plate_stats_panel, this, m_plater->get_partplate_list());

        panel_sizer->Add(m_plate_stats_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(16));
    }

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

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE && m_project_rename_book && m_project_rename_book->GetSelection() != 0) {
            m_is_rename_mode = false;
            m_project_rename_book->SetSelection(0);
            refresh_export_project_rename_labels();
            m_project_rename_normal_panel->Layout();
        } else
            e.Skip();
    });

    wxGetApp().UpdateDlgDarkUI(this);
}

void PlateChangerExportOptionsDialog::refresh_export_project_rename_labels()
{
    if (m_export_project_rename_text)
        m_export_project_rename_text->SetLabel(m_current_project_name);
}

void PlateChangerExportOptionsDialog::on_export_project_rename_click(wxCommandEvent&)
{
    if (!m_export_project_rename_input || !m_project_rename_book)
        return;
    m_is_rename_mode = true;
    m_export_project_rename_input->GetTextCtrl()->SetValue(m_current_project_name);
    m_project_rename_book->SetSelection(1);
    m_export_project_rename_input->GetTextCtrl()->SetFocus();
    m_export_project_rename_input->GetTextCtrl()->SetInsertionPointEnd();
}

void PlateChangerExportOptionsDialog::on_export_project_rename_enter()
{
    if (!m_is_rename_mode || !m_export_project_rename_input || !m_project_rename_book || !m_plater)
        return;
    m_is_rename_mode = false;

    wxString new_file_name = m_export_project_rename_input->GetTextCtrl()->GetValue();
    wxString temp;
    int      num = 0;
    for (auto t : new_file_name) {
        if (t == wxString::FromUTF8("\x20")) {
            num++;
            if (num == 1)
                temp += t;
        } else {
            num = 0;
            temp += t;
        }
    }
    new_file_name = temp;

    auto              validity = ExportProjectNameValid::Ok;
    wxString          info_line;
    const char*       unusable_symbols = "<>[]:/\\|?*\"";
    const std::string unusable_suffix  = PresetCollection::get_suffix_modified();
    for (size_t i = 0; i < std::strlen(unusable_symbols); i++) {
        if (new_file_name.find_first_of(unusable_symbols[i]) != std::string::npos) {
            info_line = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
            validity  = ExportProjectNameValid::Bad;
            break;
        }
    }

    if (validity == ExportProjectNameValid::Ok && new_file_name.find(unusable_suffix) != std::string::npos) {
        info_line = _L("Name is invalid;") + "\n" + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        validity  = ExportProjectNameValid::Bad;
    }

    if (validity == ExportProjectNameValid::Ok && new_file_name.empty()) {
        info_line = _L("The name is not allowed to be empty.");
        validity  = ExportProjectNameValid::Bad;
    }

    if (validity == ExportProjectNameValid::Ok && new_file_name.find_first_of(' ') == 0) {
        info_line = _L("The name is not allowed to start with space character.");
        validity  = ExportProjectNameValid::Bad;
    }

    if (validity == ExportProjectNameValid::Ok && new_file_name.find_last_of(' ') == new_file_name.length() - 1) {
        info_line = _L("The name is not allowed to end with space character.");
        validity  = ExportProjectNameValid::Bad;
    }

    if (validity == ExportProjectNameValid::Ok && new_file_name.size() >= 100) {
        info_line = _L("The name length exceeds the limit.");
        validity  = ExportProjectNameValid::Bad;
    }

    if (validity != ExportProjectNameValid::Ok) {
        MessageDialog msg_window(nullptr, info_line, "", wxICON_WARNING | wxOK);
        if (msg_window.ShowModal() == wxID_OK) {
            m_project_rename_book->SetSelection(0);
            refresh_export_project_rename_labels();
            m_project_rename_normal_panel->Layout();
            return;
        }
    }

    m_current_project_name = new_file_name;
    m_plater->set_project_name(m_current_project_name);
    m_project_rename_book->SetSelection(0);
    refresh_export_project_rename_labels();
    m_project_rename_normal_panel->Layout();
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

void PlateChangerExportOptionsDialog::rebuild_plate_stats_grid()
{
    if (!m_plater || !m_plate_stats_grid_sizer || !m_plate_stats_panel)
        return;
    const int n_plates = m_plater->get_partplate_list().get_plate_count();
    if (m_stext_export_plate_count)
        m_stext_export_plate_count->SetLabel(wxString::Format(_L("Exporting %d plates."), n_plates));
    if (m_export_project_rename_text && (!m_project_rename_book || m_project_rename_book->GetSelection() == 0))
        m_export_project_rename_text->SetLabel(m_current_project_name);
    populate_plate_changer_time_weight_grid(m_plate_stats_grid_sizer, m_plate_stats_panel, this, m_plater->get_partplate_list());
}

void PlateChangerExportOptionsDialog::on_ok(wxCommandEvent&)
{
    if (m_is_rename_mode)
        on_export_project_rename_enter();
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
    if (m_export_project_rename_text)
        m_export_project_rename_text->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    rebuild_plate_stats_grid();
    Layout();
    Fit();
}

}} // namespace Slic3r::GUI
