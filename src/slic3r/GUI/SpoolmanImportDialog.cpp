#include "SpoolmanImportDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "ExtraRenderers.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Widgets/DialogButtons.hpp"
#include "oneapi/tbb/parallel_for_each.h"

#define BTN_GAP FromDIP(10)
#define BTN_SIZE wxSize(FromDIP(58), FromDIP(24))

#ifdef _WIN32
#define GET_COLUMN(dvc, idx) dvc->GetColumnAt(idx)
#else
#define GET_COLUMN(dvc, idx) dvc->GetColumn(idx)
#endif

namespace Slic3r { namespace GUI {

//-----------------------------------------
// SpoolmanViewModel
//-----------------------------------------

wxDataViewItem SpoolmanViewModel::AddFilament(const SpoolmanFilamentShrPtr& filament)
{
    m_top_children.emplace_back(std::make_unique<SpoolmanNode>(filament));
    wxDataViewItem item(m_top_children.back().get());
    ItemAdded(wxDataViewItem(nullptr), item);
    return item;
}

void SpoolmanViewModel::SetAllToggles(bool value)
{
    for (auto& item : m_top_children)
        if (item->set_checked(value))
            ItemChanged(wxDataViewItem(item.get()));
}

std::vector<SpoolmanFilamentShrPtr> SpoolmanViewModel::GetSelectedFilaments()
{
    std::vector<SpoolmanFilamentShrPtr> filaments;
    for (auto& item : m_top_children)
        if (item->get_checked())
            filaments.emplace_back(item->get_filament());
    return filaments;
}

wxString SpoolmanViewModel::GetColumnType(unsigned int col) const
{
    if (col == COL_CHECK)
        return "bool";
    if (col == COL_COLOR)
        return "wxColour";
    return "string";
}

unsigned int SpoolmanViewModel::GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const
{
    if (parent.IsOk())
        return 0;

    for (auto child : m_top_children)
        array.push_back(wxDataViewItem(child.get()));
    return m_top_children.size();
}

void SpoolmanViewModel::GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const
{
    SpoolmanNode* node = get_node(item);
    if (!node)
        return;
    switch (col) {
    case COL_CHECK: variant = node->get_checked(); break;
    case COL_ID: variant = std::to_string(node->get_id()); break;
    case COL_COLOR: variant << node->get_color(); break;
    case COL_VENDOR: variant = node->get_vendor_name(); break;
    case COL_NAME: variant = node->get_filament_name(); break;
    case COL_MATERIAL: variant = node->get_material(); break;
    case COL_PRESET_DATA: variant = wxString(node->get_has_preset_data() ? L"\u2713" : L"\u2715"); break;
    default: wxLogError("Out of bounds column call to SpoolmanViewModel::GetValue. col = %d", col);
    }
}

bool SpoolmanViewModel::SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col)
{
    if (col == COL_CHECK) {
        get_node(item)->set_checked(variant.GetBool());
        return true;
    }
    wxLogError("Out of bounds column call to SpoolmanViewModel::SetValue. Only column 0 should be set to a value. col = %d", col);
    return false;
}

//-----------------------------------------
// SpoolmanViewCtrl
//-----------------------------------------

SpoolmanViewCtrl::SpoolmanViewCtrl(wxWindow* parent) : wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES)
{
    wxGetApp().UpdateDVCDarkUI(this);
#if _WIN32
    ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
#else
    SetScrollbar(wxHORIZONTAL, 0, 0, 0);
#endif

    m_model = new SpoolmanViewModel();
    this->AssociateModel(m_model);
    m_model->SetAssociatedControl(this);

    this->AppendToggleColumn(L"\u2714", COL_CHECK, wxDATAVIEW_CELL_ACTIVATABLE, 4 * EM, wxALIGN_CENTER, 0);
    this->AppendTextColumn("ID", COL_ID, wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_SORTABLE);
    this->AppendColumn(new wxDataViewColumn("Color", new ColorRenderer(), COL_COLOR, wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, 0));
    this->AppendTextColumn("Vendor", COL_VENDOR, wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_AUTOSIZE, wxALIGN_NOT, wxCOL_SORTABLE);
    this->AppendTextColumn("Name", COL_NAME, wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_AUTOSIZE, wxALIGN_NOT, wxCOL_SORTABLE);
    this->AppendTextColumn("Material", COL_MATERIAL, wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_AUTOSIZE, wxALIGN_NOT, wxCOL_SORTABLE);
    this->AppendTextColumn("Preset Data", COL_PRESET_DATA, wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_SORTABLE);

    // fake column to put the expander in
    auto temp_col = this->AppendTextColumn("", 100);
    temp_col->SetHidden(true);
    this->SetExpanderColumn(temp_col);
}

//-----------------------------------------
// SpoolmanImportDialog
//-----------------------------------------

SpoolmanImportDialog::SpoolmanImportDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Import from Spoolman"), wxDefaultPosition, {-1, 45 * EM}, wxDEFAULT_DIALOG_STYLE)
{
    this->SetBackgroundColour(*wxWHITE);

    if (!Spoolman::is_server_valid()) {
        show_error(parent, "Failed to get data from the Spoolman server. Make sure that the port is correct and the server is running.");
        return;
    }

    auto main_sizer = new wxBoxSizer(wxVERTICAL);

    // SpoolmanViewCtrl
    m_svc = new SpoolmanViewCtrl(this);
    main_sizer->Add(m_svc, 1, wxCENTER | wxEXPAND | wxALL, EM);

    // Base Preset Label
    auto* label = new Label(this, _L("Base Preset:"));
    wxGetApp().UpdateDarkUI(label);
    main_sizer->Add(label, 0, wxLEFT, EM);

    auto preset_sizer = new wxBoxSizer(wxHORIZONTAL);

    // PresetCombobox
    m_preset_combobox = new TabPresetComboBox(this, Preset::TYPE_FILAMENT);
    preset_sizer->Add(m_preset_combobox, 1, wxEXPAND | wxRIGHT, EM);
    m_preset_combobox->update();

    // Detach Checkbox
    auto checkbox_sizer = new wxBoxSizer(wxVERTICAL);
    m_detach_checkbox   = new wxCheckBox(this, wxID_ANY, _L("Save as Detached"));
    m_detach_checkbox->SetToolTip(_L("Save as a standalone preset"));
    checkbox_sizer->Add(m_detach_checkbox, 0, wxALIGN_CENTER_HORIZONTAL);

    m_ignore_preset_data_checkbox = new wxCheckBox(this, wxID_ANY, _L("Ignore Included Preset"));
    m_ignore_preset_data_checkbox->SetToolTip(_L("Ignore the preset data stored in Spoolman and use the selected base preset instead"));
    checkbox_sizer->Add(m_ignore_preset_data_checkbox, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, EM);

    preset_sizer->Add(checkbox_sizer, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(preset_sizer, 0, wxEXPAND | wxALL, EM);

    auto buttons = new DialogButtons(this, {"All", "None", "Import", "Cancel"}, _L("Import"), 2);

    buttons->GetButtonFromLabel(_L("All"))->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_svc->get_model()->SetAllToggles(true); });
    buttons->GetButtonFromLabel(_L("None"))->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_svc->get_model()->SetAllToggles(false); });
    buttons->GetButtonFromLabel(_L("Import"))->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_import(); });
    buttons->GetButtonFromLabel(_L("Cancel"))->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { this->EndModal(wxID_CANCEL); });

    main_sizer->Add(buttons, 0, wxCENTER | wxEXPAND | wxALL, EM);

    // Load data into SVC
    for (const auto& spoolman_filament : m_spoolman->get_spoolman_filaments())
        m_svc->get_model()->AddFilament(spoolman_filament.second);

#ifdef __LINUX__
    // Column width is not updated until shown in wxGTK
    bool adjusting_width = false;

    m_svc->Bind(wxEVT_SIZE, [&](wxSizeEvent&) {
        // A column width of 0 means the view has not fully initialized yet. Ignore events while the view is uninitialized.
        // Ignore any events caused by the adjusting the width
        if (GET_COLUMN(m_svc, 1)->GetWidth() == 0 || adjusting_width)
            return;

        int colWidth = 4 * EM; // 4 EM for checkbox (width isn't calculated right)
        for (int i = COL_ID; i < COL_COUNT; ++i)
            colWidth += GET_COLUMN(m_svc, i)->GetWidth();
        // Add buffer to ensure the scrollbars hide
        colWidth += EM / 2;

        int old_width = m_svc->GetSize().GetWidth();
        if (old_width == colWidth)
            return;

        // Start adjusting the width of the view. Ignore any size events caused by this
        adjusting_width = true;
        m_svc->SetMinSize({colWidth, -1});

        this->CenterOnParent();

        this->Fit();
        this->CallAfter([&] {
            this->Layout();
            adjusting_width = false;
        });
    });
#else
    int colWidth = 4 * EM; // 4 EM for checkbox (width isn't calculated right)
    for (int i = COL_ID; i < COL_COUNT; ++i)
        colWidth += GET_COLUMN(m_svc, i)->GetWidth();
    m_svc->SetMinSize({colWidth, -1});
#endif

    main_sizer->SetMinSize({-1, 45 * EM});
    this->SetSizerAndFit(main_sizer);

    wxGetApp().UpdateDlgDarkUI(this);
    this->ShowModal();
}

void SpoolmanImportDialog::EndModal(int retCode)
{
    wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
    DPIDialog::EndModal(retCode);
}

void SpoolmanImportDialog::on_dpi_changed(const wxRect& suggested_rect)
{
#ifndef __LINUX__
    int colWidth = 4 * EM; // 4 EM for checkbox (width isn't calculated right)
    for (int i = COL_ID; i < COL_COUNT; ++i)
        colWidth += GET_COLUMN(m_svc, i)->GetWidth();
    m_svc->SetMinSize({colWidth, -1});
#endif

    Fit();
    Refresh();
}

void SpoolmanImportDialog::on_import()
{
    auto&       filament_collection = wxGetApp().preset_bundle->filaments;
    const auto  current_preset      = filament_collection.find_preset(m_preset_combobox->GetStringSelection().ToUTF8().data());
    const auto& selected_filaments  = m_svc->get_model()->GetSelectedFilaments();
    if (selected_filaments.empty()) {
        show_error(this, _L("No filaments are selected"));
        return;
    }

    const bool                                                     detach             = m_detach_checkbox->GetValue();
    const bool                                                     ignore_preset_data = m_ignore_preset_data_checkbox->GetValue();
    std::vector<std::pair<SpoolmanFilamentShrPtr, SpoolmanResult>> failed_filaments;

    auto create_presets = [&](const vector<SpoolmanFilamentShrPtr>& filaments, bool force = false) {
        failed_filaments.clear();
        // Save selected preset
        const auto selected_preset_name = filament_collection.get_selected_preset_name();
        auto       edited_preset        = filament_collection.get_edited_preset();

        std::mutex failed_spools_mutex;
        auto       create_preset = [&](const SpoolmanFilamentShrPtr& filament) {
            auto res = Spoolman::create_filament_preset(filament, current_preset, !ignore_preset_data, detach, force);
            if (res.has_failed()) {
                std::lock_guard lock(failed_spools_mutex);
                failed_filaments.emplace_back(filament, res);
            }
        };

        // Calculating the hash for the internal filament id takes a bit, so using multithreading to speed it up
        wxBusyCursor busy_cursor;
        if (filaments.size() > 1)
            tbb::parallel_for_each(filaments.begin(), filaments.end(), create_preset);
        else
            create_preset(filaments[0]);

        // Restore selected preset
        filament_collection.select_preset_by_name(selected_preset_name, true);
        filament_collection.get_edited_preset() = std::move(edited_preset);
    };

    create_presets(selected_filaments);

    // Show message with any errors
    if (!failed_filaments.empty()) {
        auto build_error_msg = [&](const wxString& prefix, const wxString& postfix = "") {
            wxString error_message = prefix + ":\n\n";
            for (const auto& [filament_ptr, result] : failed_filaments) {
                error_message += wxString::FromUTF8(filament_ptr->get_preset_name()) + ":\n";
                for (const auto& msg : result.messages) {
                    error_message += " - " + msg + "\n";
                }
                error_message += "\n";
            }
            if (postfix.empty())
                error_message.erase(error_message.size() - 2);
            else
                error_message += postfix;
            return error_message;
        };

        const auto error_message = build_error_msg(_L("Errors were generated while trying to import the selected filaments"),
                                                   _L("Would you like to ignore these issues and continue?"));

        auto dlg = WarningDialog(this, error_message, wxEmptyString, wxYES | wxCANCEL);
        if (dlg.ShowModal() == wxID_YES) {
            std::vector<SpoolmanFilamentShrPtr> retry_filaments;
            for (const auto& [filament_ptr, res] : failed_filaments)
                retry_filaments.emplace_back(filament_ptr);
            create_presets(retry_filaments, true);
            if (!failed_filaments.empty())
                show_error(this, build_error_msg(_L("Errors were still generated during force import")));
            this->EndModal(wxID_OK);
        }

        // Update the combobox to display any successfully added presets
        m_preset_combobox->update();
        // Don't close the dialog so that the user may update their selections and try again
        return;
    }
    this->EndModal(wxID_OK);
}

}} // namespace Slic3r::GUI