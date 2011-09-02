#include "CalibrationWizardSavePage.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "MsgDialog.hpp"


namespace Slic3r { namespace GUI {

#define CALIBRATION_SAVE_AMS_NAME_SIZE wxSize(FromDIP(20), FromDIP(24))
#define CALIBRATION_SAVE_NUMBER_INPUT_SIZE wxSize(FromDIP(100), FromDIP(24))
#define CALIBRATION_SAVE_INPUT_SIZE     wxSize(FromDIP(240), FromDIP(24))
#define FLOW_RATE_MAX_VALUE  1.15

static wxString get_default_name(wxString filament_name, CalibMode mode){
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); it++) {
        if (filament_name.compare(it->name) == 0) {
            if (!it->alias.empty())
                filament_name = from_u8(it->alias);
            else
                filament_name = from_u8(it->name);
        }
    }

    switch (mode)
    {
    case Slic3r::CalibMode::Calib_None:
        break;
    case Slic3r::CalibMode::Calib_PA_Line:
        if (filament_name.StartsWith("Generic")) {
            filament_name.Replace("Generic", "Brand", false);
        }
        break;
    case Slic3r::CalibMode::Calib_PA_Tower:
        break;
    case Slic3r::CalibMode::Calib_Flow_Rate:
        filament_name += " Flow Rate Calibrated";
        break;
    case Slic3r::CalibMode::Calib_Temp_Tower:
        filament_name += " Temperature Calibrated";
        break;
    case Slic3r::CalibMode::Calib_Vol_speed_Tower:
        filament_name += " Max Vol Speed Calibrated";
        break;
    case Slic3r::CalibMode::Calib_VFA_Tower:
        break;
    case Slic3r::CalibMode::Calib_Retraction_tower:
        break;
    case Slic3r::CalibMode::Calib_Input_shaping_freq:
        break;
    case Slic3r::CalibMode::Calib_Input_shaping_damp:
        break;
    case Slic3r::CalibMode::Calib_Cornering:
        break;
    default:
        break;
    }
    return filament_name;
}

static wxString get_tray_name_by_tray_id(int tray_id)
{
    wxString tray_name;
    if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
        tray_name = "Ext";
    }
    else {
        int  ams_id = tray_id / 4;
        int slot_id = tray_id % 4;
        if (ams_id >= 0 && ams_id < 26) {
            char prefix = 'A' + ams_id;
            char suffix = '0' + 1 + slot_id;
            tray_name = std::string(1, prefix) + std::string(1, suffix);
        } else if (ams_id >= 128 && ams_id < 153) {
            char prefix = 'A' + ams_id - 128;
            tray_name   = std::string(1, prefix);
        }
    }
    return tray_name;
}

CalibrationCommonSavePage::CalibrationCommonSavePage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationWizardPage(parent, id, pos, size, style)
{
    ;
}

enum class GridTextInputType {
    K,
    N,
    FlowRatio,
    Name
};

class GridTextInput : public TextInput
{
public:
    GridTextInput(wxWindow* parent, wxString text, wxString label, wxSize size, int col_idx, GridTextInputType type);
    int get_col_idx() { return m_col_idx; }
    void set_col_idx(int idx) { m_col_idx = idx; }
    GridTextInputType get_type() { return m_type; }
    void set_type(GridTextInputType type) { m_type = type; }
private:
    int m_col_idx;
    GridTextInputType m_type;
};

GridTextInput::GridTextInput(wxWindow* parent, wxString text, wxString label, wxSize size, int col_idx, GridTextInputType type)
    : TextInput(parent, text, label, "", wxDefaultPosition, size, wxTE_PROCESS_ENTER)
    , m_col_idx(col_idx)
    , m_type(type)
{
}

class GridComboBox : public ComboBox {
public:
    GridComboBox(wxWindow* parent, wxSize size, int col_idx);
    int get_col_idx() { return m_col_idx; }
    void set_col_idx(int idx) { m_col_idx = idx; }
private:
    int m_col_idx;
};

GridComboBox::GridComboBox(wxWindow* parent, wxSize size, int col_idx)
    : ComboBox(parent, wxID_ANY, "", wxDefaultPosition, size, 0, nullptr)
    , m_col_idx(col_idx)
{

}

CaliPASaveAutoPanel::CaliPASaveAutoPanel(
    wxWindow* parent,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}


void CaliPASaveAutoPanel::create_panel(wxWindow* parent)
{
    m_complete_text_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_complete_text_panel->SetBackgroundColour(*wxWHITE);
    m_complete_text_panel->Hide();
    wxBoxSizer* complete_text_sizer = new wxBoxSizer(wxVERTICAL);
    auto complete_text = new Label(m_complete_text_panel, _L("We found the best Flow Dynamics Calibration Factor"));
    complete_text->SetFont(Label::Head_14);
    complete_text_sizer->Add(complete_text, 0, wxEXPAND);
    m_complete_text_panel->SetSizer(complete_text_sizer);

    m_part_failed_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_part_failed_panel->SetBackgroundColour(wxColour(238, 238, 238));
    wxBoxSizer* part_failed_sizer = new wxBoxSizer(wxVERTICAL);
    m_part_failed_panel->SetSizer(part_failed_sizer);
    part_failed_sizer->AddSpacer(FromDIP(10));
    auto part_failed_text = new Label(m_part_failed_panel, _L("Part of the calibration failed! You may clean the plate and retry. The failed test result would be dropped."));
    part_failed_text->SetFont(Label::Body_14);
    part_failed_sizer->Add(part_failed_text, 0, wxLEFT | wxRIGHT, FromDIP(20));
    part_failed_sizer->AddSpacer(FromDIP(10));

    m_top_sizer->Add(m_part_failed_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_top_sizer->Add(m_complete_text_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_grid_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_grid_panel->SetBackgroundColour(*wxWHITE);
    m_top_sizer->Add(m_grid_panel, 0, wxALIGN_CENTER);

    m_multi_extruder_grid_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_multi_extruder_grid_panel->SetBackgroundColour(*wxWHITE);
    m_top_sizer->Add(m_multi_extruder_grid_panel, 0, wxALIGN_CENTER);

    m_top_sizer->AddSpacer(FromDIP(10));

    auto naming_hints = new Label(parent, _L("*We recommend you to add brand, materia, type, and even humidity level in the Name"));
    naming_hints->SetFont(Label::Body_14);
    naming_hints->SetForegroundColour(wxColour(157, 157, 157));
    m_top_sizer->Add(naming_hints, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));
}

std::vector<std::pair<int, std::string>> CaliPASaveAutoPanel::default_naming(std::vector<std::pair<int, std::string>> preset_names)
{
    std::unordered_set<std::string> set;
    int i = 1;
    for (auto& item : preset_names) {
        if (!set.insert(item.second).second) {
            item.second = get_default_name(item.second, CalibMode::Calib_PA_Line).ToUTF8().data();
            item.second += "_" + std::to_string(i);
            i++;
        }
        else {
            item.second = get_default_name(item.second, CalibMode::Calib_PA_Line).ToUTF8().data();
        }
    }
    return preset_names;
}

void CaliPASaveAutoPanel::sync_cali_result(const std::vector<PACalibResult>& cali_result, const std::vector<PACalibResult>& history_result)
{
    if (m_obj && m_obj->is_multi_extruders()) {
        m_grid_panel->Hide();
        m_multi_extruder_grid_panel->Show();
        sync_cali_result_for_multi_extruder(cali_result, history_result);
        return;
    }

    m_grid_panel->Show();
    m_multi_extruder_grid_panel->Hide();

    m_history_results = history_result;
    m_calib_results.clear();
    for (auto& item : cali_result) {
        if (item.confidence == 0)
            m_calib_results[item.tray_id] = item;
    }
    m_grid_panel->DestroyChildren();
    auto grid_sizer = new wxBoxSizer(wxHORIZONTAL);
    const int COLUMN_GAP = FromDIP(20);
    const int ROW_GAP = FromDIP(30);
    wxBoxSizer* left_title_sizer = new wxBoxSizer(wxVERTICAL);
    left_title_sizer->AddSpacer(FromDIP(52));
    auto k_title = new Label(m_grid_panel, _L("Factor K"));
    k_title->SetFont(Label::Head_14);
    left_title_sizer->Add(k_title, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
    auto n_title = new Label(m_grid_panel, _L("Factor N"));
    n_title->SetFont(Label::Head_14);
    // hide n value
    n_title->Hide();
    left_title_sizer->Add(n_title, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
    auto brand_title = new Label(m_grid_panel, _L("Name"));
    brand_title->SetFont(Label::Head_14);
    left_title_sizer->Add(brand_title, 0, wxALIGN_CENTER);
    grid_sizer->Add(left_title_sizer);
    grid_sizer->AddSpacer(COLUMN_GAP);

    m_is_all_failed = true;
    bool part_failed = false;
    if (cali_result.empty())
        part_failed = true;

    std::vector<std::pair<int, std::string>> preset_names;
    for (auto& info : m_obj->selected_cali_preset) {
        preset_names.push_back({ info.tray_id, info.name });
    }
    preset_names = default_naming(preset_names);

    std::vector<PACalibResult> sorted_cali_result = cali_result;
    std::sort(sorted_cali_result.begin(), sorted_cali_result.end(), [this](const PACalibResult &left, const PACalibResult& right) {
        return left.tray_id < right.tray_id;
    });

    for (auto &item : sorted_cali_result) {
        bool result_failed = false;
        if (item.confidence != 0) {
            result_failed = true;
            part_failed = true;
        }
        else {
            m_is_all_failed = false;
        }

        wxBoxSizer* column_data_sizer = new wxBoxSizer(wxVERTICAL);
        auto tray_title = new Label(m_grid_panel, "");
        tray_title->SetFont(Label::Head_14);
        wxString tray_name = get_tray_name_by_tray_id(item.tray_id);
        tray_title->SetLabel(tray_name);

        auto k_value = new GridTextInput(m_grid_panel, "", "", CALIBRATION_SAVE_INPUT_SIZE, item.tray_id, GridTextInputType::K);
        auto n_value = new GridTextInput(m_grid_panel, "", "", CALIBRATION_SAVE_INPUT_SIZE, item.tray_id, GridTextInputType::N);
        k_value->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        n_value->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        auto k_value_failed = new Label(m_grid_panel, _L("Failed"));
        auto n_value_failed = new Label(m_grid_panel, _L("Failed"));

        auto comboBox_tray_name = new GridComboBox(m_grid_panel, CALIBRATION_SAVE_INPUT_SIZE, item.tray_id);
        auto tray_name_failed = new Label(m_grid_panel, " - ");
        wxArrayString selections;
        static std::vector<PACalibResult> filtered_results;
        filtered_results.clear();
        for (auto history : history_result) {
            if (history.filament_id == item.filament_id) {
                filtered_results.push_back(history);
                selections.push_back(from_u8(history.name));
            }
        }
        comboBox_tray_name->Set(selections);

        column_data_sizer->Add(tray_title, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(k_value, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(n_value, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(k_value_failed, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(n_value_failed, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(comboBox_tray_name, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(tray_name_failed, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);

        auto set_edit_mode = [this, k_value, n_value, k_value_failed, n_value_failed, comboBox_tray_name, tray_name_failed](std::string str) {
            if (str == "normal") {
                comboBox_tray_name->Show();
                tray_name_failed->Show(false);
                k_value->Show();
                n_value->Show();
                k_value_failed->Show(false);
                n_value_failed->Show(false);
            }
            if (str == "failed") {
                comboBox_tray_name->Show(false);
                tray_name_failed->Show();
                k_value->Show(false);
                n_value->Show(false);
                k_value_failed->Show();
                n_value_failed->Show();
            }

            // hide n value
            n_value->Hide();
            n_value_failed->Hide();

            m_grid_panel->Layout();
            m_grid_panel->Update();
        };

        if (!result_failed) {
            set_edit_mode("normal");

            auto k_str = wxString::Format("%.3f", item.k_value);
            auto n_str = wxString::Format("%.3f", item.n_coef);
            k_value->GetTextCtrl()->SetValue(k_str);
            n_value->GetTextCtrl()->SetValue(n_str);

            for (auto& name : preset_names) {
                int tray_id = item.tray_id;
                /* upgrade single extruder printer tray_id from 254 to 255 */
                if (!m_obj->is_multi_extruders() && tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
                    tray_id = VIRTUAL_TRAY_MAIN_ID;
                }

                if (tray_id == name.first) {
                    comboBox_tray_name->SetValue(from_u8(name.second));
                }
            }

            comboBox_tray_name->Bind(wxEVT_COMBOBOX, [this, comboBox_tray_name, k_value, n_value](auto& e) {
                int selection = comboBox_tray_name->GetSelection();
                auto history = filtered_results[selection];
                });
        }
        else {
            set_edit_mode("failed");
        }

        grid_sizer->Add(column_data_sizer);
        grid_sizer->AddSpacer(COLUMN_GAP);
    }

    m_grid_panel->SetSizer(grid_sizer, true);
    m_grid_panel->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        SetFocusIgnoringChildren();
        });

    if (part_failed) {
        m_part_failed_panel->Show();
        m_complete_text_panel->Show();
        if (m_is_all_failed) {
            m_complete_text_panel->Hide();
        }
    }
    else {
        m_complete_text_panel->Show();
        m_part_failed_panel->Hide();
    }

    wxGetApp().UpdateDarkUIWin(this);

    Layout();
}

void CaliPASaveAutoPanel::save_to_result_from_widgets(wxWindow* window, bool* out_is_valid, wxString* out_msg) {
    if (!window)
        return;

    //operate
    auto input = dynamic_cast<GridTextInput*>(window);
    if (input && input->IsShown()) {
        int tray_id = input->get_col_idx();
        if (input->get_type() == GridTextInputType::K) {
            float k = 0.0f;
            if (!CalibUtils::validate_input_k_value(input->GetTextCtrl()->GetValue(), &k)) {
                *out_msg = wxString::Format(_L("Please input a valid value (K in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE);
                *out_is_valid = false;
            }
            else
                m_calib_results[tray_id].k_value = k;
        }
        else if (input->get_type() == GridTextInputType::N) {
        }
    }

    auto comboBox = dynamic_cast<GridComboBox*>(window);
    if (comboBox && comboBox->IsShown()) {
        int tray_id = comboBox->get_col_idx();
        wxString name = comboBox->GetTextCtrl()->GetValue().ToStdString();
        if (name.IsEmpty()) {
            *out_msg = _L("Please enter the name you want to save to printer.");
            *out_is_valid = false;
        }
        else if (name.Length() > 40) {
            *out_msg = _L("The name cannot exceed 40 characters.");
            *out_is_valid = false;
        }
        m_calib_results[tray_id].name = into_u8(name);
    }

    auto childern = window->GetChildren();
    for (auto child : childern) {
        save_to_result_from_widgets(child, out_is_valid, out_msg);
    }
};

bool CaliPASaveAutoPanel::get_result(std::vector<PACalibResult>& out_result) {
    bool is_valid = true;
    wxString err_msg;
    // Check if the input value is valid and save to m_calib_results
    if (m_obj && m_obj->is_multi_extruders())
        save_to_result_from_widgets(m_multi_extruder_grid_panel, &is_valid, &err_msg);
    else
        save_to_result_from_widgets(m_grid_panel, &is_valid, &err_msg);
    if (is_valid) {
        /*
        std::vector<PACalibResult> to_save_result;
        for (auto &result : m_calib_results) {
            auto iter = std::find_if(to_save_result.begin(), to_save_result.end(), [this, &result](const PACalibResult &item) {
                bool has_same_name = (item.name == result.second.name && item.filament_id == result.second.filament_id);
                if (m_obj && m_obj->is_multi_extruders()) {
                    has_same_name &= (item.extruder_id == result.second.extruder_id && item.nozzle_volume_type == result.second.nozzle_volume_type);
                }
                return has_same_name;
            });

            if (iter != to_save_result.end()) {
                MessageDialog msg_dlg(nullptr, _L("Only one of the results with the same name will be saved. Are you sure you want to overwrite the other results?"),
                                      wxEmptyString, wxICON_WARNING | wxYES_NO);
                if (msg_dlg.ShowModal() != wxID_YES) {
                    return false;
                } else {
                    break;
                }
            }
        }

        for (auto &result : m_history_results) {
            auto iter = std::find_if(m_history_results.begin(), m_history_results.end(), [this, &result](const PACalibResult &item) {
                bool has_same_name = (item.name == result.name && item.filament_id == result.filament_id);
                if (m_obj && m_obj->is_multi_extruders()) {
                    has_same_name &= (item.extruder_id == result.extruder_id && item.nozzle_volume_type == result.nozzle_volume_type);
                }
                return has_same_name;
            });

            if (iter != m_history_results.end()) {
                 MessageDialog msg_dlg(nullptr,
                                      wxString::Format(_L("There is already a historical calibration result with the same name: %s. Are you sure you want to override the historical result?"),
                                                       result.name),
                                      wxEmptyString, wxICON_WARNING | wxYES_NO);
                if (msg_dlg.ShowModal() != wxID_YES) {
                    return false;
                }
            }
        }
        */

        for (auto& result : m_calib_results) {
            out_result.push_back(result.second);
        }
        return true;
    }
    else {
        MessageDialog msg_dlg(nullptr, err_msg, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
}

void CaliPASaveAutoPanel::sync_cali_result_for_multi_extruder(const std::vector<PACalibResult>& cali_result, const std::vector<PACalibResult>& history_result)
{
    if (!m_obj)
        return;

    std::map<int, DynamicPrintConfig> old_full_filament_ams_list = wxGetApp().sidebar().build_filament_ams_list(m_obj);
    std::map<int, DynamicPrintConfig> full_filament_ams_list;
    for (auto ams_item : old_full_filament_ams_list) {
        int key = ams_item.first & 0x0FFFF;
        full_filament_ams_list[key] = std::move(ams_item.second);
    }

    m_is_all_failed  = true;
    bool part_failed = false;
    if (cali_result.empty())
        part_failed = true;

    m_history_results = history_result;
    m_calib_results.clear();
    for (auto &item : cali_result) {
        if (item.confidence == 0) {
            int tray_id = 4 * item.ams_id + item.slot_id;
            if (item.ams_id == VIRTUAL_TRAY_MAIN_ID || item.ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
                tray_id = item.ams_id;
            }
            m_calib_results[tray_id] = item;
        }
    }
    m_multi_extruder_grid_panel->DestroyChildren();
    auto        grid_sizer       = new wxBoxSizer(wxHORIZONTAL);
    const int   COLUMN_GAP       = FromDIP(10);
    const int   ROW_GAP          = FromDIP(10);

    m_multi_extruder_grid_panel->SetSizer(grid_sizer, true);
    m_multi_extruder_grid_panel->Bind(wxEVT_LEFT_DOWN, [this](auto &e) { SetFocusIgnoringChildren(); });

    wxStaticBoxSizer *left_sizer  = new wxStaticBoxSizer(wxVERTICAL, m_multi_extruder_grid_panel, _L("Left extruder"));
    wxStaticBoxSizer *right_sizer = new wxStaticBoxSizer(wxVERTICAL, m_multi_extruder_grid_panel, _L("Right extruder"));
    grid_sizer->Add(left_sizer);
    grid_sizer->AddSpacer(COLUMN_GAP);
    grid_sizer->Add(right_sizer);

    wxFlexGridSizer *left_grid_sizer  = new wxFlexGridSizer(3, COLUMN_GAP, ROW_GAP);
    wxFlexGridSizer *right_grid_sizer = new wxFlexGridSizer(3, COLUMN_GAP, ROW_GAP);
    left_sizer->Add(left_grid_sizer);
    right_sizer->Add(right_grid_sizer);

    // main extruder
    {
        left_grid_sizer->Add(new wxStaticText(m_multi_extruder_grid_panel, wxID_ANY, ""), 1, wxEXPAND); // fill empty space

        auto brand_title = new Label(m_multi_extruder_grid_panel, _L("Name"), 0, CALIBRATION_SAVE_INPUT_SIZE);
        brand_title->SetFont(Label::Head_14);
        left_grid_sizer->Add(brand_title, 1, wxALIGN_CENTER);

        auto k_title = new Label(m_multi_extruder_grid_panel, _L("Factor K"), 0, CALIBRATION_SAVE_NUMBER_INPUT_SIZE);
        k_title->SetFont(Label::Head_14);
        left_grid_sizer->Add(k_title, 1, wxALIGN_CENTER);
    }

    // deputy extruder
    {
        right_grid_sizer->Add(new wxStaticText(m_multi_extruder_grid_panel, wxID_ANY, ""), 1, wxEXPAND); // fill empty space

        auto brand_title = new Label(m_multi_extruder_grid_panel, _L("Name"), 0, CALIBRATION_SAVE_INPUT_SIZE);
        brand_title->SetFont(Label::Head_14);
        right_grid_sizer->Add(brand_title, 1, wxALIGN_CENTER);

        auto k_title = new Label(m_multi_extruder_grid_panel, _L("Factor K"), 0, CALIBRATION_SAVE_NUMBER_INPUT_SIZE);
        k_title->SetFont(Label::Head_14);
        right_grid_sizer->Add(k_title, 1, wxALIGN_CENTER);
    }

    std::vector<std::pair<int, std::string>> preset_names;
    int i = 1;
    std::unordered_set<std::string> set;
    for (auto &info : m_obj->selected_cali_preset) {
        std::string default_name;
        // extruder _id
        {
            int extruder_id = 0;
            if (info.tray_id == VIRTUAL_TRAY_MAIN_ID) {
                extruder_id = 0;
            } else if (info.tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
                extruder_id = 1;
            } else {
                int ams_id  = info.tray_id / 4;
                extruder_id = m_obj->get_extruder_id_by_ams_id(std::to_string(ams_id));
            }

            if (extruder_id == 0) {
                default_name += L("Right Nozzle");
            } else if (extruder_id == 1){
                default_name += L("Left Nozzle");
            }
        }

        // nozzle_volume_type
        {
            default_name += "_";
            if (info.nozzle_volume_type == NozzleVolumeType::nvtStandard) {
                default_name += L("Standard");
            }
            else if (info.nozzle_volume_type == NozzleVolumeType::nvtHighFlow) {
                default_name += L("High Flow");
            }
        }

        // filament_id
        {
            default_name += "_";
            default_name += get_default_name(info.name, CalibMode::Calib_PA_Line).ToUTF8().data();
            if (!set.insert(default_name).second) {
                default_name += "_" + std::to_string(i);
                i++;
            }
        }

        preset_names.push_back({info.tray_id, default_name});
    }

    bool left_first_add_item = true;
    bool right_first_add_item = true;
    std::vector<PACalibResult> sorted_cali_result   = cali_result;
    if (m_obj && m_obj->is_support_new_auto_cali_method) {
        for (auto &res : sorted_cali_result) {
            if (res.ams_id == VIRTUAL_TRAY_MAIN_ID || res.ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
                res.tray_id = res.ams_id;
            } else {
                res.tray_id = res.ams_id * 4 + res.slot_id;
            }
        }
    }

    std::sort(sorted_cali_result.begin(), sorted_cali_result.end(), [](const PACalibResult &left, const PACalibResult &right) {
        return left.tray_id < right.tray_id;
    });
    for (auto &item : sorted_cali_result) {
        bool result_failed = false;
        if (item.confidence != 0) {
            result_failed = true;
            part_failed   = true;
        } else {
            m_is_all_failed = false;
        }

        wxString tray_name = get_tray_name_by_tray_id(item.tray_id);
        wxButton *tray_title = new wxButton(m_multi_extruder_grid_panel, wxID_ANY, {}, wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)), wxBU_EXACTFIT | wxBU_AUTODRAW | wxBORDER_NONE);
        tray_title->SetBackgroundColour(*wxWHITE);
        tray_title->SetBitmap(*get_extruder_color_icon(full_filament_ams_list[item.tray_id].opt_string("filament_colour", 0u), tray_name.ToStdString(), FromDIP(20), FromDIP(20)));
        tray_title->SetToolTip("");

        auto k_value = new GridTextInput(m_multi_extruder_grid_panel, "", "", CALIBRATION_SAVE_NUMBER_INPUT_SIZE, item.tray_id, GridTextInputType::K);
        auto n_value = new GridTextInput(m_multi_extruder_grid_panel, "", "", CALIBRATION_SAVE_NUMBER_INPUT_SIZE, item.tray_id, GridTextInputType::N);
        k_value->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        n_value->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        auto k_value_failed = new Label(m_multi_extruder_grid_panel, _L("Failed"));
        auto n_value_failed = new Label(m_multi_extruder_grid_panel, _L("Failed"));

        auto                              comboBox_tray_name = new GridComboBox(m_multi_extruder_grid_panel, CALIBRATION_SAVE_INPUT_SIZE, item.tray_id);
        auto                              tray_name_failed   = new Label(m_multi_extruder_grid_panel, " - ");
        wxArrayString                     selections;
        static std::vector<PACalibResult> filtered_results;
        filtered_results.clear();
        for (auto history : history_result) {
            if (history.filament_id == item.filament_id
                && history.extruder_id == item.extruder_id
                && history.nozzle_volume_type == item.nozzle_volume_type
                && history.nozzle_diameter == item.nozzle_diameter) {
                filtered_results.push_back(history);
                selections.push_back(from_u8(history.name));
            }
        }
        comboBox_tray_name->Set(selections);

        auto set_edit_mode = [this, k_value, n_value, k_value_failed, n_value_failed, comboBox_tray_name, tray_name_failed](std::string str) {
            if (str == "normal") {
                comboBox_tray_name->Show();
                tray_name_failed->Show(false);
                k_value->Show();
                n_value->Show();
                k_value_failed->Show(false);
                n_value_failed->Show(false);
            }
            if (str == "failed") {
                comboBox_tray_name->Show(false);
                tray_name_failed->Show();
                k_value->Show(false);
                n_value->Show(false);
                k_value_failed->Show();
                n_value_failed->Show();
            }

            // hide n value
            n_value->Hide();
            n_value_failed->Hide();

            m_multi_extruder_grid_panel->Layout();
            m_multi_extruder_grid_panel->Update();
        };

        if (!result_failed) {
            set_edit_mode("normal");

            auto k_str = wxString::Format("%.3f", item.k_value);
            auto n_str = wxString::Format("%.3f", item.n_coef);
            k_value->GetTextCtrl()->SetValue(k_str);
            n_value->GetTextCtrl()->SetValue(n_str);

            for (auto &name : preset_names) {
                if (item.tray_id == name.first) { comboBox_tray_name->SetValue(from_u8(name.second)); }
            }

            comboBox_tray_name->Bind(wxEVT_COMBOBOX, [this, comboBox_tray_name, k_value, n_value](auto &e) {
                int  selection = comboBox_tray_name->GetSelection();
                auto history   = filtered_results[selection];
            });
        } else {
            set_edit_mode("failed");
        }

        if ((m_obj->is_main_extruder_on_left() && item.extruder_id == 0)
            || (!m_obj->is_main_extruder_on_left() && item.extruder_id == 1)) {
            if (left_first_add_item) {
                wxString title_name = left_sizer->GetStaticBox()->GetLabel();
                title_name += " - ";
                title_name += get_nozzle_volume_type_name(item.nozzle_volume_type);
                left_sizer->GetStaticBox()->SetLabel(title_name);
                left_first_add_item = false;
            }

            left_grid_sizer->Add(tray_title, 1, wxEXPAND);

            if (comboBox_tray_name->IsShown()) {
                left_grid_sizer->Add(comboBox_tray_name, 1, wxEXPAND);
            } else {
                left_grid_sizer->Add(tray_name_failed, 1, wxEXPAND);
            }

            if (k_value->IsShown()) {
                left_grid_sizer->Add(k_value, 1, wxEXPAND);
            } else {
                left_grid_sizer->Add(k_value_failed, 1, wxEXPAND);
            }
        }
        else {
            if (right_first_add_item) {
                wxString title_name = right_sizer->GetStaticBox()->GetLabel();
                title_name += " - ";
                title_name += get_nozzle_volume_type_name(item.nozzle_volume_type);
                right_sizer->GetStaticBox()->SetLabel(title_name);
                right_first_add_item = false;
            }
            right_grid_sizer->Add(tray_title, 1, wxEXPAND);

            if (comboBox_tray_name->IsShown()) {
                right_grid_sizer->Add(comboBox_tray_name, 1, wxEXPAND);
            } else {
                right_grid_sizer->Add(tray_name_failed, 1, wxEXPAND);
            }

            if (k_value->IsShown()) {
                right_grid_sizer->Add(k_value, 1, wxEXPAND);
            } else {
                right_grid_sizer->Add(k_value_failed, 1, wxEXPAND);
            }
        }
    }

    if (left_first_add_item)
        left_sizer->Show(false);
    if (right_first_add_item)
        right_sizer->Show(false);

    if (part_failed) {
        m_part_failed_panel->Show();
        m_complete_text_panel->Show();
        if (m_is_all_failed) {
            m_complete_text_panel->Hide();
        }
    } else {
        m_complete_text_panel->Show();
        m_part_failed_panel->Hide();
    }

    wxGetApp().UpdateDarkUIWin(this);

    Layout();
}

CaliPASaveManualPanel::CaliPASaveManualPanel(
    wxWindow* parent,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliPASaveManualPanel::create_panel(wxWindow* parent)
{
    auto complete_text_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    complete_text_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* complete_text_sizer = new wxBoxSizer(wxVERTICAL);
    m_complete_text = new Label(complete_text_panel, _L("Please find the best line on your plate"));
    m_complete_text->SetFont(Label::Head_14);
    m_complete_text->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    complete_text_sizer->Add(m_complete_text, 0);
    complete_text_panel->SetSizer(complete_text_sizer);
    m_top_sizer->Add(complete_text_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_picture_panel = new CaliPagePicture(parent);
    set_save_img();
    m_top_sizer->Add(m_picture_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    auto k_value_text = new Label(parent, _L("Factor K"));
    k_value_text->SetFont(::Label::Head_14);
    k_value_text->Wrap(-1);
    auto n_value_text = new Label(parent, _L("Factor N"));
    n_value_text->SetFont(::Label::Head_14);
    n_value_text->Wrap(-1);
    n_value_text->Hide();
    m_k_val = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, 0);
    m_k_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_n_val = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, 0);
    m_n_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_n_val->Hide();
    m_top_sizer->Add(k_value_text, 0);
    m_top_sizer->Add(m_k_val, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    auto save_text = new Label(parent, _L("Name"));
    save_text->SetFont(Label::Head_14);
    m_top_sizer->Add(save_text, 0, 0, 0);

    m_save_name_input = new TextInput(parent, "", "", "", wxDefaultPosition, { CALIBRATION_TEXT_MAX_LENGTH, FromDIP(24) }, 0);
    m_top_sizer->Add(m_save_name_input, 0, 0, 0);

    m_top_sizer->AddSpacer(FromDIP(10));

    auto naming_hints = new Label(parent, _L("*We recommend you to add brand, materia, type, and even humidity level in the Name"));
    naming_hints->SetFont(Label::Body_14);
    naming_hints->SetForegroundColour(wxColour(157, 157, 157));
    m_top_sizer->Add(naming_hints, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        SetFocusIgnoringChildren();
        });
}

void CaliPASaveManualPanel::set_save_img() {
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        m_picture_panel->set_bmp(ScalableBitmap(this, "fd_calibration_manual_result_CN", 330));
    } else {
        m_picture_panel->set_bmp(ScalableBitmap(this, "fd_calibration_manual_result", 330));
    }
}

void CaliPASaveManualPanel::set_pa_cali_method(ManualPaCaliMethod method)
{
    if (method == ManualPaCaliMethod::PA_LINE) {
        m_complete_text->SetLabel(_L("Please find the best line on your plate"));
        set_save_img();
    } else if (method == ManualPaCaliMethod::PA_PATTERN) {
        m_complete_text->SetLabel(_L("Please find the corner with perfect degree of extrusion"));
        if (wxGetApp().app_config->get_language_code() == "zh-cn") {
            m_picture_panel->set_bmp(ScalableBitmap(this, "fd_pattern_manual_result_CN", 350));
        } else {
            m_picture_panel->set_bmp(ScalableBitmap(this, "fd_pattern_manual_result", 350));
        }
    }
}

void CaliPASaveManualPanel::set_default_name(const wxString& name) {
    m_save_name_input->GetTextCtrl()->SetValue(name);
}

bool CaliPASaveManualPanel::get_result(PACalibResult& out_result) {
    // Check if the value is valid
    float k;
    if (!CalibUtils::validate_input_k_value(m_k_val->GetTextCtrl()->GetValue(), &k)) {
        MessageDialog msg_dlg(nullptr, wxString::Format(_L("Please input a valid value (K in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }

    wxString name = m_save_name_input->GetTextCtrl()->GetValue();
    if (!CalibUtils::validate_input_name(name))
        return false;

    out_result.k_value = k;
    out_result.name = into_u8(name);
    if (m_obj) {
        assert(m_obj->selected_cali_preset.size() <= 1);
        if (!m_obj->selected_cali_preset.empty()) {
            out_result.tray_id = m_obj->selected_cali_preset[0].tray_id;
            out_result.nozzle_diameter = m_obj->selected_cali_preset[0].nozzle_diameter;
            out_result.filament_id = m_obj->selected_cali_preset[0].filament_id;
            out_result.setting_id = m_obj->selected_cali_preset[0].setting_id;
            out_result.extruder_id = m_obj->selected_cali_preset[0].extruder_id;
            out_result.nozzle_volume_type    = m_obj->selected_cali_preset[0].nozzle_volume_type;
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "CaliPASaveManual: obj->selected_cali_preset is empty";
            return false;
        }
    }
    else {
        BOOST_LOG_TRIVIAL(trace) << "CaliPASaveManual::get_result(): obj is nullptr";
        return false;
    }

    return true;
}

bool CaliPASaveManualPanel::Show(bool show) {
    if (show) {
        if (m_obj) {
            if (!m_obj->selected_cali_preset.empty()) {
                wxString default_name = get_default_name(m_obj->selected_cali_preset[0].name, CalibMode::Calib_PA_Line);
                if (m_obj->is_multi_extruders()) {
                    wxString recommend_name;
                    CaliPresetInfo info = m_obj->selected_cali_preset[0];
                    // extruder _id
                    {
                        int extruder_id = 0;
                        if (info.tray_id == VIRTUAL_TRAY_MAIN_ID) {
                            extruder_id = 0;
                        } else if (info.tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
                            extruder_id = 1;
                        } else {
                            int ams_id  = info.tray_id / 4;
                            extruder_id = m_obj->get_extruder_id_by_ams_id(std::to_string(ams_id));
                        }

                        if (extruder_id == 0) {
                            recommend_name += L("Right");
                        } else if (extruder_id == 1) {
                            recommend_name += L("Left");
                        }
                    }

                    // nozzle_volume_type
                    {
                        recommend_name += "_";
                        if (info.nozzle_volume_type == NozzleVolumeType::nvtStandard) {
                            recommend_name += L("Standard");
                        } else if (info.nozzle_volume_type == NozzleVolumeType::nvtHighFlow) {
                            recommend_name += L("High Flow");
                        }
                    }

                    default_name = recommend_name + "_" + default_name;
                }
                set_default_name(default_name);
                m_k_val->GetTextCtrl()->SetLabel("");
                m_n_val->GetTextCtrl()->SetLabel("");
            }
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "CaliPASaveManual::Show(): obj is nullptr";
        }
    }
    return wxPanel::Show(show);
}

void CaliPASaveManualPanel::msw_rescale()
{
    m_picture_panel->msw_rescale();
}

CaliPASaveP1PPanel::CaliPASaveP1PPanel(
    wxWindow* parent,
    wxWindowID id,
    const wxPoint& pos,
    const wxSize& size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliPASaveP1PPanel::create_panel(wxWindow* parent)
{
    auto complete_text_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    complete_text_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* complete_text_sizer = new wxBoxSizer(wxVERTICAL);
    m_complete_text = new Label(complete_text_panel, _L("Please find the best line on your plate"));
    m_complete_text->SetFont(Label::Head_14);
    m_complete_text->Wrap(CALIBRATION_TEXT_MAX_LENGTH);
    complete_text_sizer->Add(m_complete_text, 0, wxEXPAND);
    complete_text_panel->SetSizer(complete_text_sizer);
    m_top_sizer->Add(complete_text_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_picture_panel = new CaliPagePicture(parent);
    set_save_img();
    m_top_sizer->Add(m_picture_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    auto value_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto k_value_text = new Label(parent, _L("Factor K"));
    k_value_text->Wrap(-1);
    k_value_text->SetFont(::Label::Head_14);
    auto n_value_text = new Label(parent, _L("Factor N"));
    n_value_text->Wrap(-1);
    n_value_text->SetFont(::Label::Head_14);
    m_k_val = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, 0);
    m_k_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_n_val = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, 0);
    m_n_val->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    n_value_text->Hide();
    m_n_val->Hide();
    value_sizer->Add(k_value_text, 0, wxALIGN_CENTER_VERTICAL, 0);
    value_sizer->AddSpacer(FromDIP(10));
    value_sizer->Add(m_k_val, 0);
    value_sizer->AddSpacer(FromDIP(50));
    value_sizer->Add(n_value_text, 0, wxALIGN_CENTER_VERTICAL, 0);
    value_sizer->AddSpacer(FromDIP(10));
    value_sizer->Add(m_n_val, 0);
    m_top_sizer->Add(value_sizer, 0, wxALIGN_CENTER);

    m_top_sizer->AddSpacer(FromDIP(20));

    Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        SetFocusIgnoringChildren();
        });
}

void CaliPASaveP1PPanel::set_save_img() {
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        m_picture_panel->set_bmp(ScalableBitmap(this, "fd_calibration_manual_result_CN", 350));
    } else {
        m_picture_panel->set_bmp(ScalableBitmap(this, "fd_calibration_manual_result", 350));
    }
}

void CaliPASaveP1PPanel::set_pa_cali_method(ManualPaCaliMethod method)
{
    if (method == ManualPaCaliMethod::PA_LINE) {
        m_complete_text->SetLabel(_L("Please find the best line on your plate"));
        set_save_img();
    }
    else if (method == ManualPaCaliMethod::PA_PATTERN) {
        m_complete_text->SetLabel(_L("Please find the corner with perfect degree of extrusion"));
        if (wxGetApp().app_config->get_language_code() == "zh-cn") {
            m_picture_panel->set_bmp(ScalableBitmap(this, "fd_pattern_manual_result_CN", 350));
        } else {
            m_picture_panel->set_bmp(ScalableBitmap(this, "fd_pattern_manual_result", 350));
        }
    }
}

bool CaliPASaveP1PPanel::get_result(float* out_k, float* out_n){
    // Check if the value is valid
    if (!CalibUtils::validate_input_k_value(m_k_val->GetTextCtrl()->GetValue(), out_k)) {
        MessageDialog msg_dlg(nullptr, wxString::Format(_L("Please input a valid value (K in %.1f~%.1f)"), MIN_PA_K_VALUE, MAX_PA_K_VALUE), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    return true;
}

bool CaliPASaveP1PPanel::Show(bool show) {
    if (show) {
        m_k_val->GetTextCtrl()->SetLabel("");
        m_n_val->GetTextCtrl()->SetLabel("");
    }
    return wxPanel::Show(show);
}

void CaliPASaveP1PPanel::msw_rescale()
{
    m_picture_panel->msw_rescale();
}

CaliSavePresetValuePanel::CaliSavePresetValuePanel(
    wxWindow *parent,
    wxWindowID id,
    const wxPoint &pos,
    const wxSize &size,
    long style)
    : wxPanel(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_panel(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CaliSavePresetValuePanel::create_panel(wxWindow *parent)
{
    m_picture_panel = new CaliPagePicture(parent);

    m_value_title = new Label(parent, _L("Input Value"));
    m_value_title->SetFont(Label::Head_14);
    m_value_title->Wrap(-1);
    m_input_value = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, wxTE_PROCESS_ENTER);
    m_input_value->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    m_save_name_title = new Label(parent, _L("Save to Filament Preset"));
    m_save_name_title->Wrap(-1);
    m_save_name_title->SetFont(Label::Head_14);

    m_input_name = new TextInput(parent, wxEmptyString, "", "", wxDefaultPosition, {CALIBRATION_TEXT_MAX_LENGTH, FromDIP(24)}, 0);

    m_top_sizer->Add(m_picture_panel, 0, wxEXPAND, 0);
    m_top_sizer->AddSpacer(FromDIP(20));
    m_top_sizer->Add(m_value_title, 0);
    m_top_sizer->AddSpacer(FromDIP(10));
    m_top_sizer->Add(m_input_value, 0);
    m_top_sizer->AddSpacer(FromDIP(20));
    m_top_sizer->Add(m_save_name_title, 0);
    m_top_sizer->AddSpacer(FromDIP(10));
    m_top_sizer->Add(m_input_name, 0);
    m_top_sizer->AddSpacer(FromDIP(20));
}

void CaliSavePresetValuePanel::set_img(const std::string& bmp_name_in)
{
    m_picture_panel->set_bmp(ScalableBitmap(this, bmp_name_in, 400));
}

void CaliSavePresetValuePanel::set_value_title(const wxString& title) {
    m_value_title->SetLabel(title);
}

void CaliSavePresetValuePanel::set_save_name_title(const wxString& title) {
    m_save_name_title->SetLabel(title);
}

void CaliSavePresetValuePanel::get_value(double& value)
{
    m_input_value->GetTextCtrl()->GetValue().ToDouble(&value);
}

void CaliSavePresetValuePanel::get_save_name(std::string& name)
{
    name = into_u8(m_input_name->GetTextCtrl()->GetValue());
}

void CaliSavePresetValuePanel::set_save_name(const std::string& name)
{
    m_input_name->GetTextCtrl()->SetValue(name);
}

void CaliSavePresetValuePanel::msw_rescale()
{
    m_picture_panel->msw_rescale();
}

CalibrationPASavePage::CalibrationPASavePage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationCommonSavePage(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_cali_mode = CalibMode::Calib_PA_Line;

    m_page_type = CaliPageType::CALI_PAGE_PA_SAVE;

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationPASavePage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    wxArrayString steps;
    steps.Add(_L("Preset"));
    steps.Add(_L("Calibration"));
    steps.Add(_L("Record Factor"));
    m_step_panel = new CaliPageStepGuide(parent, steps);
    m_step_panel->set_steps(2);
    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    m_manual_panel = new CaliPASaveManualPanel(parent, wxID_ANY);
    m_auto_panel = new CaliPASaveAutoPanel(parent, wxID_ANY);
    m_p1p_panel = new CaliPASaveP1PPanel(parent, wxID_ANY);
    m_help_panel = new PAPageHelpPanel(parent);
    m_manual_panel->Hide();
    m_p1p_panel->Hide();

    m_top_sizer->Add(m_manual_panel, 0, wxEXPAND);
    m_top_sizer->Add(m_auto_panel, 0, wxEXPAND);
    m_top_sizer->Add(m_p1p_panel, 0, wxEXPAND);
    m_top_sizer->Add(m_help_panel, 0, wxEXPAND);
    m_top_sizer->AddSpacer(FromDIP(20));

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_PA_SAVE);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);
}

void CalibrationPASavePage::sync_cali_result(MachineObject* obj)
{
    // only auto need sync cali_result
    if (obj && (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO || m_cali_method == CalibrationMethod::CALI_METHOD_NEW_AUTO)) {
        m_auto_panel->sync_cali_result(obj->pa_calib_results, obj->pa_calib_tab);
    } else {
        std::vector<PACalibResult> empty_result;
        m_auto_panel->sync_cali_result(empty_result, empty_result);
    }
}

void CalibrationPASavePage::show_panels(CalibrationMethod method, const PrinterSeries printer_ser) {
    if (printer_ser == PrinterSeries::SERIES_X1) {
        if (method == CalibrationMethod::CALI_METHOD_MANUAL) {
            m_manual_panel->set_pa_cali_method(curr_obj->manual_pa_cali_method);
            m_manual_panel->Show();
            m_auto_panel->Show(false);
        }
        else {
            m_auto_panel->Show();
            m_manual_panel->Show(false);
        }
        m_p1p_panel->Show(false);
    }
    else if (curr_obj->cali_version >= 0) {
        m_auto_panel->Show(false);
        m_manual_panel->set_pa_cali_method(curr_obj->manual_pa_cali_method);
        m_manual_panel->Show();
        m_p1p_panel->Show(false);
    } else {
        m_auto_panel->Show(false);
        m_manual_panel->Show(false);
        m_p1p_panel->set_pa_cali_method(curr_obj->manual_pa_cali_method);
        m_p1p_panel->Show();
        assert(false);
    }
    Layout();
}

void CalibrationPASavePage::set_cali_method(CalibrationMethod method)
{
    CalibrationWizardPage::set_cali_method(method);
    if (curr_obj) {
        show_panels(method, curr_obj->get_printer_series());
    }
}

void CalibrationPASavePage::on_device_connected(MachineObject* obj)
{
    curr_obj = obj;
    m_auto_panel->set_machine_obj(curr_obj);
    m_manual_panel->set_machine_obj(curr_obj);
    if (curr_obj)
        show_panels(m_cali_method, curr_obj->get_printer_series());
}

void CalibrationPASavePage::update(MachineObject* obj)
{
    CalibrationWizardPage::update(obj);

    if (m_auto_panel && m_auto_panel->IsShown())
        m_auto_panel->set_machine_obj(obj);
    if (m_manual_panel && m_manual_panel->IsShown())
        m_manual_panel->set_machine_obj(obj);
}

bool CalibrationPASavePage::Show(bool show) {
    if (show) {
        if (curr_obj) {
            show_panels(m_cali_method, curr_obj->get_printer_series());
            sync_cali_result(curr_obj);
        }
    }
    return wxPanel::Show(show);
}

void CalibrationPASavePage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_manual_panel->msw_rescale();
    m_p1p_panel->msw_rescale();
    m_help_panel->msw_rescale();
}

CalibrationFlowX1SavePage::CalibrationFlowX1SavePage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationCommonSavePage(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_cali_mode = CalibMode::Calib_Flow_Rate;

    m_page_type = CaliPageType::CALI_PAGE_FLOW_SAVE;

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationFlowX1SavePage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    wxArrayString steps;
    steps.Add(_L("Preset"));
    steps.Add(_L("Calibration"));
    steps.Add(_L("Record Factor"));
    m_step_panel = new CaliPageStepGuide(parent, steps);
    m_step_panel->set_steps(2);
    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    m_complete_text_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_complete_text_panel->SetBackgroundColour(*wxWHITE);
    m_complete_text_panel->Hide();
    wxBoxSizer* complete_text_sizer = new wxBoxSizer(wxVERTICAL);
    auto complete_text = new Label(m_complete_text_panel, _L("We found the best flow ratio for you"));
    complete_text->SetFont(Label::Head_14);
    complete_text_sizer->Add(complete_text, 0, wxEXPAND);
    m_complete_text_panel->SetSizer(complete_text_sizer);

    m_part_failed_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_part_failed_panel->SetBackgroundColour(wxColour(238, 238, 238));
    wxBoxSizer* part_failed_sizer = new wxBoxSizer(wxVERTICAL);
    m_part_failed_panel->SetSizer(part_failed_sizer);
    part_failed_sizer->AddSpacer(FromDIP(10));
    auto part_failed_text = new Label(m_part_failed_panel, _L("Part of the calibration failed! You may clean the plate and retry. The failed test result would be dropped."));
    part_failed_text->SetFont(Label::Body_14);
    part_failed_sizer->Add(part_failed_text, 0, wxLEFT | wxRIGHT, FromDIP(20));
    part_failed_sizer->AddSpacer(FromDIP(10));

    m_top_sizer->Add(m_part_failed_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_top_sizer->Add(m_complete_text_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_grid_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_grid_panel->SetBackgroundColour(*wxWHITE);
    m_top_sizer->Add(m_grid_panel, 0, wxALIGN_CENTER);

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_FLOW_SAVE);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);
}

void CalibrationFlowX1SavePage::sync_cali_result(const std::vector<FlowRatioCalibResult>& cali_result)
{
    m_save_results.clear();
    m_grid_panel->DestroyChildren();
    wxBoxSizer* grid_sizer = new wxBoxSizer(wxHORIZONTAL);
    const int COLUMN_GAP = FromDIP(20);
    const int ROW_GAP = FromDIP(30);
    wxBoxSizer* left_title_sizer = new wxBoxSizer(wxVERTICAL);
    left_title_sizer->AddSpacer(FromDIP(49));
    auto flow_ratio_title = new Label(m_grid_panel, _L("Flow Ratio"));
    flow_ratio_title->SetFont(Label::Head_14);
    left_title_sizer->Add(flow_ratio_title, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP + FromDIP(10));
    auto brand_title = new Label(m_grid_panel, _L("Save to Filament Preset"));
    brand_title->SetFont(Label::Head_14);
    left_title_sizer->Add(brand_title, 0, wxALIGN_CENTER);
    grid_sizer->Add(left_title_sizer);
    grid_sizer->AddSpacer(COLUMN_GAP);

    m_is_all_failed = true;
    bool part_failed = false;
    if (cali_result.empty())
        part_failed = true;
    for (auto& item : cali_result) {
        bool result_failed = false;
        if (item.confidence != 0 || item.flow_ratio < 1e-3 || item.flow_ratio > FLOW_RATE_MAX_VALUE) {
            result_failed = true;
            part_failed = true;
        }
        else {
            m_is_all_failed = false;
        }

        wxBoxSizer* column_data_sizer = new wxBoxSizer(wxVERTICAL);
        auto tray_title = new Label(m_grid_panel, "");
        tray_title->SetFont(Label::Head_14);
        wxString tray_name = get_tray_name_by_tray_id(item.tray_id);
        tray_title->SetLabel(tray_name);

        auto flow_ratio_value = new GridTextInput(m_grid_panel, "", "", CALIBRATION_SAVE_INPUT_SIZE, item.tray_id, GridTextInputType::FlowRatio);
        flow_ratio_value->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        auto flow_ratio_value_failed = new Label(m_grid_panel, _L("Failed"));

        auto save_name_input = new GridTextInput(m_grid_panel, "", "", { CALIBRATION_TEXT_MAX_LENGTH, FromDIP(24) }, item.tray_id, GridTextInputType::Name);
        auto save_name_input_failed = new Label(m_grid_panel, " - ");

        column_data_sizer->Add(tray_title, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(flow_ratio_value, 0, wxALIGN_LEFT | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(flow_ratio_value_failed, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(save_name_input, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);
        column_data_sizer->Add(save_name_input_failed, 0, wxALIGN_CENTER | wxBOTTOM, ROW_GAP);

        auto set_edit_mode = [this, flow_ratio_value, flow_ratio_value_failed, save_name_input, save_name_input_failed](std::string str) {
            if (str == "normal") {
                save_name_input->Show();
                save_name_input_failed->Show(false);
                flow_ratio_value->Show();
                flow_ratio_value_failed->Show(false);
            }
            if (str == "failed") {
                save_name_input->Show(false);
                save_name_input_failed->Show();
                flow_ratio_value->Show(false);
                flow_ratio_value_failed->Show();
            }
            m_grid_panel->Layout();
            m_grid_panel->Update();
        };

        if (!result_failed) {
            set_edit_mode("normal");

            auto flow_ratio_str = wxString::Format("%.3f", item.flow_ratio);
            flow_ratio_value->GetTextCtrl()->SetValue(flow_ratio_str);
            for (auto& info : curr_obj->selected_cali_preset) {
                if (item.tray_id == info.tray_id) {
                    save_name_input->GetTextCtrl()->SetValue(get_default_name(info.name, CalibMode::Calib_Flow_Rate) + "_" + tray_name);
                    break;
                }
                else {
                    BOOST_LOG_TRIVIAL(trace) << "CalibrationFlowX1Save : obj->selected_cali_preset doesn't contain correct tray_id";
                }
            }
        }
        else {
            set_edit_mode("failed");
        }

        grid_sizer->Add(column_data_sizer);
        grid_sizer->AddSpacer(COLUMN_GAP);
    }
    m_grid_panel->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        m_grid_panel->SetFocusIgnoringChildren();
        });
    m_grid_panel->SetSizer(grid_sizer, true);

    if (part_failed) {
        m_part_failed_panel->Show();
        m_complete_text_panel->Show();
        if (m_is_all_failed) {
            m_complete_text_panel->Hide();
        }
    }
    else {
        m_complete_text_panel->Show();
        m_part_failed_panel->Hide();
    }

    wxGetApp().UpdateDarkUIWin(this);

    Layout();
}

void CalibrationFlowX1SavePage::save_to_result_from_widgets(wxWindow* window, bool* out_is_valid, wxString* out_msg)
{
    if (!window)
        return;

    //operate
    auto input = dynamic_cast<GridTextInput*>(window);
    if (input && input->IsShown()) {
        int tray_id = input->get_col_idx();
        if (input->get_type() == GridTextInputType::FlowRatio) {
            float flow_ratio = 0.0f;
            if (!CalibUtils::validate_input_flow_ratio(input->GetTextCtrl()->GetValue(), &flow_ratio)) {
                *out_msg = _L("Please input a valid value (0.0 < flow ratio < 2.0)");
                *out_is_valid = false;
            }
            m_save_results[tray_id].second = flow_ratio;
        }
        else if (input->get_type() == GridTextInputType::Name) {
            if (input->GetTextCtrl()->GetValue().IsEmpty()) {
                *out_msg = _L("Please enter the name of the preset you want to save.");
                *out_is_valid = false;
            }
            m_save_results[tray_id].first = input->GetTextCtrl()->GetValue().ToStdString();
        }
    }

    auto childern = window->GetChildren();
    for (auto child : childern) {
        save_to_result_from_widgets(child, out_is_valid, out_msg);
    }
}

bool CalibrationFlowX1SavePage::get_result(std::vector<std::pair<wxString, float>>& out_results)
{
    bool is_valid = true;
    wxString err_msg;
    // Check if the value is valid and save to m_calib_results
    save_to_result_from_widgets(m_grid_panel, &is_valid, &err_msg);
    if (is_valid) {
        // obj->cali_result contain failure results, so use m_save_results to record value
        for (auto& item : m_save_results) {
            out_results.push_back(item.second);
        }
        return true;
    }
    else {
        MessageDialog msg_dlg(nullptr, err_msg, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
}

bool CalibrationFlowX1SavePage::Show(bool show) {
    if (show) {
        if (curr_obj) {
            sync_cali_result(curr_obj->flow_ratio_results);
        }
    }
    return wxPanel::Show(show);
}

void CalibrationFlowX1SavePage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
}

CalibrationFlowCoarseSavePage::CalibrationFlowCoarseSavePage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationCommonSavePage(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_cali_mode = CalibMode::Calib_Flow_Rate;

    m_page_type = CaliPageType::CALI_PAGE_COARSE_SAVE;

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationFlowCoarseSavePage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    wxArrayString steps;
    steps.Add(_L("Preset"));
    steps.Add(_L("Calibration1"));
    steps.Add(_L("Calibration2"));
    steps.Add(_L("Record Factor"));
    m_step_panel = new CaliPageStepGuide(parent, steps);
    m_step_panel->set_steps(1);
    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    auto complete_text = new Label(parent, _L("Please find the best object on your plate"));
    complete_text->SetFont(Label::Head_14);
    complete_text->Wrap(-1);
    m_top_sizer->Add(complete_text, 0, wxEXPAND, 0);
    m_top_sizer->AddSpacer(FromDIP(20));

    m_picture_panel = new CaliPagePicture(parent);
    set_save_img();
    m_top_sizer->Add(m_picture_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    auto coarse_value_sizer = new wxBoxSizer(wxVERTICAL);
    auto coarse_value_text = new Label(parent, _L("Fill in the value above the block with smoothest top surface"));
    coarse_value_text->SetFont(Label::Head_14);
    coarse_value_text->Wrap(-1);
    m_optimal_block_coarse = new ComboBox(parent, wxID_ANY, "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, 0, nullptr, wxCB_READONLY);
    wxArrayString coarse_block_items;
    for (int i = 0; i < 9; i++) {
        coarse_block_items.Add(std::to_string(-20 + (i * 5)));
    }
    m_optimal_block_coarse->Set(coarse_block_items);
    m_coarse_calc_result_text = new Label(parent, "");
    coarse_value_sizer->Add(coarse_value_text, 0, 0);
    coarse_value_sizer->Add(m_optimal_block_coarse, 0, 0);
    coarse_value_sizer->Add(m_coarse_calc_result_text, 0);
    m_top_sizer->Add(coarse_value_sizer, 0, 0, 0);
    m_top_sizer->AddSpacer(FromDIP(20));

    auto checkBox_panel = new wxPanel(parent);
    checkBox_panel->SetBackgroundColour(*wxWHITE);
    auto cb_sizer = new wxBoxSizer(wxHORIZONTAL);
    checkBox_panel->SetSizer(cb_sizer);
    m_checkBox_skip_calibration = new CheckBox(checkBox_panel);
    cb_sizer->Add(m_checkBox_skip_calibration);

    auto cb_text = new Label(checkBox_panel, _L("Skip Calibration2"));
    cb_sizer->Add(cb_text);
    cb_text->Bind(wxEVT_LEFT_DOWN, [this](auto&) {
        m_checkBox_skip_calibration->SetValue(!m_checkBox_skip_calibration->GetValue());
        wxCommandEvent event(wxEVT_TOGGLEBUTTON);
        event.SetEventObject(m_checkBox_skip_calibration);
        m_checkBox_skip_calibration->GetEventHandler()->ProcessEvent(event);
        });

    m_top_sizer->Add(checkBox_panel, 0, 0, 0);

    auto save_panel = new wxPanel(parent);
    save_panel->SetBackgroundColour(*wxWHITE);
    auto save_sizer = new wxBoxSizer(wxVERTICAL);
    save_panel->SetSizer(save_sizer);

    auto save_text = new Label(save_panel, _L("Save to Filament Preset"));
    save_text->Wrap(-1);
    save_text->SetFont(Label::Head_14);
    save_sizer->Add(save_text, 0, 0, 0);

    m_save_name_input = new TextInput(save_panel, "", "", "", wxDefaultPosition, {CALIBRATION_TEXT_MAX_LENGTH, FromDIP(24)}, 0);
    save_sizer->Add(m_save_name_input, 0, 0, 0);

    m_top_sizer->Add(save_panel, 0, 0, 0);
    save_panel->Hide();

    m_top_sizer->AddSpacer(FromDIP(20));

    m_checkBox_skip_calibration->Bind(wxEVT_TOGGLEBUTTON, [this, save_panel](wxCommandEvent &e) {
        if (m_checkBox_skip_calibration->GetValue()) {
            m_skip_fine_calibration = true;
            save_panel->Show();
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_COARSE_SAVE);
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, false);
        }
        else {
            m_skip_fine_calibration = false;
            save_panel->Hide();
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_COARSE_SAVE, false);
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2);
        }
        Layout();
        Fit();
        e.Skip();
        });

    m_optimal_block_coarse->Bind(wxEVT_COMBOBOX, [this](auto& e) {
        m_coarse_flow_ratio = m_curr_flow_ratio * (100.0f + stof(m_optimal_block_coarse->GetValue().ToStdString())) / 100.0f;
        m_coarse_calc_result_text->SetLabel(wxString::Format(_L("flow ratio : %s "), std::to_string(m_coarse_flow_ratio)));
        });

    m_sending_panel = new CaliPageSendingPanel(parent);
    m_sending_panel->get_sending_progress_bar()->set_cancel_callback_fina([this]() {
        on_cali_cancel_job();
        });
    m_sending_panel->Hide();
    m_top_sizer->Add(m_sending_panel, 0, wxALIGN_CENTER);

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_COARSE_SAVE);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_COARSE_SAVE, false);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);
}

void CalibrationFlowCoarseSavePage::set_save_img() {
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_coarse_result_CN", 350));
    } else {
        m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_coarse_result", 350));
    }
}

void CalibrationFlowCoarseSavePage::set_default_options(const wxString& name) {
    m_optimal_block_coarse->SetSelection(-1);
    m_coarse_calc_result_text->SetLabelText("");
    m_checkBox_skip_calibration->SetValue(false);
    m_save_name_input->GetTextCtrl()->SetValue(name);

    wxCommandEvent event(wxEVT_TOGGLEBUTTON);
    event.SetEventObject(m_checkBox_skip_calibration);
    m_checkBox_skip_calibration->GetEventHandler()->ProcessEvent(event);
}

bool CalibrationFlowCoarseSavePage::is_skip_fine_calibration() {
    return m_skip_fine_calibration;
}

void CalibrationFlowCoarseSavePage::set_curr_flow_ratio(const float value) {
    m_curr_flow_ratio = value;
}

bool CalibrationFlowCoarseSavePage::get_result(float* out_value, wxString* out_name) {
    // Check if the value is valid
    if (m_optimal_block_coarse->GetSelection() == -1 || m_coarse_flow_ratio <= 0.0 || m_coarse_flow_ratio >= 2.0) {
        MessageDialog msg_dlg(nullptr, _L("Please choose a block with smoothest top surface."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    if (m_save_name_input->GetTextCtrl()->GetValue().IsEmpty()) {
        MessageDialog msg_dlg(nullptr, _L("Please enter the name of the preset you want to save."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    *out_value = m_coarse_flow_ratio;
    *out_name = m_save_name_input->GetTextCtrl()->GetValue();
    return true;
}

bool CalibrationFlowCoarseSavePage::Show(bool show) {
    if (show) {
        if (curr_obj) {
            assert(curr_obj->selected_cali_preset.size() <= 1);
            if (!curr_obj->selected_cali_preset.empty()) {
                wxString default_name = get_default_name(curr_obj->selected_cali_preset[0].name, CalibMode::Calib_Flow_Rate);
                set_default_options(default_name);
                set_curr_flow_ratio(curr_obj->cache_flow_ratio);
            }
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "CalibrationFlowCoarseSave::Show(): obj is nullptr";
        }
    }
    return wxPanel::Show(show);
}

void CalibrationFlowCoarseSavePage::on_cali_start_job()
{
    m_sending_panel->reset();
    m_sending_panel->Show();
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, false);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, false);
    Layout();
    Fit();
}

void CalibrationFlowCoarseSavePage::on_cali_finished_job()
{
    m_sending_panel->reset();
    m_sending_panel->Show(false);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, true);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, true);
    Layout();
    Fit();
}

void CalibrationFlowCoarseSavePage::on_cali_cancel_job()
{
    BOOST_LOG_TRIVIAL(info) << "CalibrationWizard::print_job: enter canceled";
    if (CalibUtils::print_worker) {
        BOOST_LOG_TRIVIAL(info) << "calibration_print_job: canceled";
        CalibUtils::print_worker->cancel_all();
        CalibUtils::print_worker->wait_for_idle();
    }

    m_sending_panel->reset();
    m_sending_panel->Show(false);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, true);
    m_action_panel->show_button(CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2, true);
    Layout();
    Fit();
}

void CalibrationFlowCoarseSavePage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_picture_panel->msw_rescale();
}

CalibrationFlowFineSavePage::CalibrationFlowFineSavePage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationCommonSavePage(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_cali_mode = CalibMode::Calib_Flow_Rate;

    m_page_type = CaliPageType::CALI_PAGE_FINE_SAVE;

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationFlowFineSavePage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    wxArrayString steps;
    steps.Add(_L("Preset"));
    steps.Add(_L("Calibration1"));
    steps.Add(_L("Calibration2"));
    steps.Add(_L("Record Factor"));
    m_step_panel = new CaliPageStepGuide(parent, steps);
    m_step_panel->set_steps(3);
    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    auto complete_text = new Label(parent, _L("Please find the best object on your plate"));
    complete_text->SetFont(Label::Head_14);
    complete_text->Wrap(-1);
    m_top_sizer->Add(complete_text, 0, wxEXPAND, 0);
    m_top_sizer->AddSpacer(FromDIP(20));

    m_picture_panel = new CaliPagePicture(parent);
    set_save_img();
    m_top_sizer->Add(m_picture_panel, 0, wxEXPAND, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    auto fine_value_sizer = new wxBoxSizer(wxVERTICAL);
    auto fine_value_text = new Label(parent, _L("Fill in the value above the block with smoothest top surface"));
    fine_value_text->Wrap(-1);
    fine_value_text->SetFont(::Label::Head_14);
    m_optimal_block_fine = new ComboBox(parent, wxID_ANY, "", wxDefaultPosition, CALIBRATION_OPTIMAL_INPUT_SIZE, 0, nullptr, wxCB_READONLY);
    wxArrayString fine_block_items;
    for (int i = 0; i < 10; i++) {
        fine_block_items.Add(std::to_string(-9 + (i)));
    }
    m_optimal_block_fine->Set(fine_block_items);
    m_fine_calc_result_text = new Label(parent, "");
    fine_value_sizer->Add(fine_value_text, 0, 0);
    fine_value_sizer->Add(m_optimal_block_fine, 0, 0);
    fine_value_sizer->Add(m_fine_calc_result_text, 0);
    m_top_sizer->Add(fine_value_sizer, 0, 0, 0);
    m_top_sizer->AddSpacer(FromDIP(20));

    auto save_text = new Label(parent, _L("Save to Filament Preset"));
    save_text->Wrap(-1);
    save_text->SetFont(Label::Head_14);
    m_top_sizer->Add(save_text, 0, 0, 0);

    m_save_name_input = new TextInput(parent, "", "", "", wxDefaultPosition, {CALIBRATION_TEXT_MAX_LENGTH, FromDIP(24)}, 0);
    m_top_sizer->Add(m_save_name_input, 0, 0, 0);

    m_top_sizer->AddSpacer(FromDIP(20));

    m_optimal_block_fine->Bind(wxEVT_COMBOBOX, [this](auto& e) {
        m_fine_flow_ratio = m_curr_flow_ratio * (100.0f + stof(m_optimal_block_fine->GetValue().ToStdString())) / 100.0f;
        m_fine_calc_result_text->SetLabel(wxString::Format(_L("flow ratio : %s "), std::to_string(m_fine_flow_ratio)));
        });

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_FINE_SAVE);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);
}

void CalibrationFlowFineSavePage::set_save_img() {
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_fine_result_CN", 350));
    } else {
        m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_fine_result", 350));
    }
}

void CalibrationFlowFineSavePage::set_default_options(const wxString &name) {
    m_optimal_block_fine->SetSelection(-1);
    m_fine_calc_result_text->SetLabelText("");
    m_save_name_input->GetTextCtrl()->SetValue(name);
}

void CalibrationFlowFineSavePage::set_curr_flow_ratio(const float value) {
    m_curr_flow_ratio = value;
}

bool CalibrationFlowFineSavePage::get_result(float* out_value, wxString* out_name) {
    // Check if the value is valid
    if (m_optimal_block_fine->GetSelection() == -1 || m_fine_flow_ratio <= 0.0 || m_fine_flow_ratio >= 2.0) {
        MessageDialog msg_dlg(nullptr, _L("Please choose a block with smoothest top surface."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    if (m_save_name_input->GetTextCtrl()->GetValue().IsEmpty()) {
        MessageDialog msg_dlg(nullptr, _L("Please enter the name of the preset you want to save."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    *out_value = m_fine_flow_ratio;
    *out_name = m_save_name_input->GetTextCtrl()->GetValue();
    return true;
}

bool CalibrationFlowFineSavePage::Show(bool show) {
    if (show) {
        if (curr_obj) {
            assert(curr_obj->selected_cali_preset.size() <= 1);
            if (!curr_obj->selected_cali_preset.empty()) {
                wxString default_name = get_default_name(curr_obj->selected_cali_preset[0].name, CalibMode::Calib_Flow_Rate);
                set_default_options(default_name);
                set_curr_flow_ratio(curr_obj->cache_flow_ratio);
            }
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "CalibrationFlowFineSave::Show(): obj is nullptr";
        }
    }
    return wxPanel::Show(show);
}

void CalibrationFlowFineSavePage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_picture_panel->msw_rescale();
}

CalibrationMaxVolumetricSpeedSavePage::CalibrationMaxVolumetricSpeedSavePage(
    wxWindow *parent,
    wxWindowID id,
    const wxPoint &pos,
    const wxSize &size,
    long style)
    : CalibrationCommonSavePage(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    m_cali_mode = CalibMode::Calib_Vol_speed_Tower;

    m_page_type = CaliPageType::CALI_PAGE_COMMON_SAVE;

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    m_top_sizer->Fit(this);
}

void CalibrationMaxVolumetricSpeedSavePage::create_page(wxWindow *parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    wxArrayString steps;
    steps.Add(_L("Preset"));
    steps.Add(_L("Calibration"));
    steps.Add(_L("Record Factor"));
    m_step_panel = new CaliPageStepGuide(parent, steps);
    m_step_panel->set_steps(2);
    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    m_save_preset_panel = new CaliSavePresetValuePanel(parent, wxID_ANY);

    set_save_img();

    m_top_sizer->Add(m_save_preset_panel, 0, wxEXPAND);

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_COMMON_SAVE);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);
}

void CalibrationMaxVolumetricSpeedSavePage::set_save_img() {
    m_save_preset_panel->set_img("max_volumetric_speed_calibration");
}

bool CalibrationMaxVolumetricSpeedSavePage::get_save_result(double& value, std::string& name) {
    // Check if the value is valid
    m_save_preset_panel->get_save_name(name);
    m_save_preset_panel->get_value(value);
    if (value < 0 || value > 60) {
        MessageDialog msg_dlg(nullptr, _L("Please input a valid value (0 <= Max Volumetric Speed <= 60)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    if (name.empty()) {
        MessageDialog msg_dlg(nullptr, _L("Please enter the name of the preset you want to save."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    return true;
}

bool CalibrationMaxVolumetricSpeedSavePage::Show(bool show) {
    if (show) {
        if (curr_obj) {
            assert(curr_obj->selected_cali_preset.size() <= 1);
            if (!curr_obj->selected_cali_preset.empty()) {
                wxString default_name = get_default_name(curr_obj->selected_cali_preset[0].name, CalibMode::Calib_Vol_speed_Tower);
                m_save_preset_panel->set_save_name(default_name.ToStdString());
            }
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "CalibrationMaxVolumetricSpeedSave::Show(): obj is nullptr";
        }
    }
    return wxPanel::Show(show);
}


}}