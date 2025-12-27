#include "PlateSettingsDialog.hpp"
#include "MsgDialog.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {
static constexpr int MIN_LAYER_VALUE = 2;
static constexpr int MAX_LAYER_VALUE = INT_MAX - 1;

wxDEFINE_EVENT(EVT_SET_BED_TYPE_CONFIRM, wxCommandEvent);
wxDEFINE_EVENT(EVT_NEED_RESORT_LAYERS, wxCommandEvent);

bool LayerSeqInfo::operator<(const LayerSeqInfo& another) const
{
    if (this->begin_layer_number < MIN_LAYER_VALUE)
        return false;
    if (another.begin_layer_number < MIN_LAYER_VALUE)
        return true;
    if (this->begin_layer_number == another.begin_layer_number) {
        if (this->end_layer_number < MIN_LAYER_VALUE)
            return false;
        if (another.end_layer_number < MIN_LAYER_VALUE)
            return true;
        return this->end_layer_number < another.end_layer_number;
    }
    return this->begin_layer_number < another.begin_layer_number;
}

LayerNumberTextInput::LayerNumberTextInput(wxWindow* parent, int layer_number, wxSize size, Type type, ValueType value_type)
    :ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, size, 0, NULL)
    , m_layer_number(layer_number)
    , m_type(type)
    , m_value_type(value_type)
{
    GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_DIGITS));
    GetTextCtrl()->SetFont(::Label::Body_14);
    Append(_L("End"));
    Append(_L("Customize"));
    if (m_value_type == ValueType::End)
        SetSelection(0);
    if (m_value_type == ValueType::Custom) {
        SetSelection(1);
        update_label();
    }

    Bind(wxEVT_TEXT, [this](auto& evt) {
            if (m_value_type == ValueType::End) {
                // TextCtrl->SetValue() will generate a wxEVT_TEXT event
                GetTextCtrl()->ChangeValue(_L("End"));
                return;
            }
            evt.Skip();
        });

    auto validate_input_value = [this](int gui_value) {
        // value should not be less than MIN_LAYER_VALUE, and should not be greater than MAX_LAYER_VALUE
        gui_value = std::clamp(gui_value, MIN_LAYER_VALUE, MAX_LAYER_VALUE);

        int begin_value = 0;
        int end_value = 0;
        LayerNumberTextInput* end_layer_input = nullptr;
        if (this->m_type == Type::Begin) {
            begin_value = gui_value;
            end_value = m_another_layer_input->get_layer_number();
            end_layer_input = m_another_layer_input;
        }
        if (this->m_type == Type::End) {
            begin_value = m_another_layer_input->get_layer_number();
            end_value = gui_value;
            end_layer_input = this;
        }

        // end value should not be less than begin value
        if (begin_value > end_value) {
            // set new value for end_layer_input
            if (this->m_type == Type::Begin) {
                if (end_layer_input->is_layer_number_valid()) {
                    end_layer_input->set_layer_number(begin_value);
                }
            }
            if (this->m_type == Type::End) {
                if (!this->is_layer_number_valid()) {
                    this->set_layer_number(begin_value);
                    wxCommandEvent evt(EVT_NEED_RESORT_LAYERS);
                    wxPostEvent(m_parent, evt);
                }
                else {
                    // do nothing
                    // reset to the last value for end_layer_input
                }
                return;
            }
        }
        m_layer_number = gui_value;
        wxCommandEvent evt(EVT_NEED_RESORT_LAYERS);
        wxPostEvent(m_parent, evt);
    };
    auto commit_layer_number_from_gui = [this, validate_input_value]() {
        if (m_value_type == ValueType::End)
            return;

        auto gui_str = GetTextCtrl()->GetValue().ToStdString();
        if (gui_str.empty()) {
            m_layer_number = -1;
            wxCommandEvent evt(EVT_NEED_RESORT_LAYERS);
            wxPostEvent(m_parent, evt);
        }
        if (!gui_str.empty()) {
            int gui_value = atoi(gui_str.c_str());
            validate_input_value(gui_value);
        }
        update_label();
    };
    Bind(wxEVT_TEXT_ENTER, [commit_layer_number_from_gui](wxEvent& evt) {
        commit_layer_number_from_gui();
        evt.Skip();
        });
    Bind(wxEVT_KILL_FOCUS, [commit_layer_number_from_gui](wxFocusEvent& evt) {
        commit_layer_number_from_gui();
        evt.Skip();
        });

    Bind(wxEVT_COMBOBOX, [this](auto& e) {
        if (e.GetSelection() == 0) {
            m_value_type = ValueType::End;
        }
        else if (e.GetSelection() == 1) {
            m_value_type = ValueType::Custom;
            m_layer_number = -1;
            update_label();
        }
        e.Skip();
        });
}

void LayerNumberTextInput::update_label()
{
    if (m_value_type == ValueType::End)
        return;

    if (!is_layer_number_valid()) {
        SetLabel("");
    }
    else
        SetLabel(std::to_string(m_layer_number));
}

void LayerNumberTextInput::set_layer_number(int layer_number)
{
    m_layer_number = layer_number;
    if (layer_number == MAX_LAYER_VALUE)
        m_value_type = ValueType::End;
    else
        m_value_type = ValueType::Custom;

    if (m_value_type == ValueType::End)
        SetSelection(0);
    if (m_value_type == ValueType::Custom) {
        SetSelection(1);
        update_label();
    }
}

int LayerNumberTextInput::get_layer_number()
{
    return m_value_type == ValueType::End ? MAX_LAYER_VALUE : m_layer_number;
}

bool LayerNumberTextInput::is_layer_number_valid()
{
    if (m_value_type == ValueType::End)
        return true;
    return m_layer_number >= MIN_LAYER_VALUE;
}

OtherLayersSeqPanel::OtherLayersSeqPanel(wxWindow* parent)
    :wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    m_bmp_delete = ScalableBitmap(this, "delete_filament");
    m_bmp_add = ScalableBitmap(this, "add_filament");

    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* top_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_other_layer_print_seq_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(240), -1), 0, NULL, wxCB_READONLY);
    m_other_layer_print_seq_choice->Append(_L("Auto"));
    m_other_layer_print_seq_choice->Append(_L("Customize"));
    m_other_layer_print_seq_choice->SetSelection(0);
    wxStaticText* other_layer_txt = new wxStaticText(this, wxID_ANY, _L("Other layer filament sequence"));
    other_layer_txt->SetFont(Label::Body_14);
    title_sizer->Add(other_layer_txt, 0, wxALIGN_CENTER | wxALIGN_LEFT, 0);
    title_sizer->AddStretchSpacer();
    title_sizer->Add(m_other_layer_print_seq_choice, 0, wxALIGN_CENTER | wxALIGN_RIGHT, 0);

    wxBoxSizer* buttons_sizer = new wxBoxSizer(wxHORIZONTAL);
    ScalableButton* add_layers_btn = new ScalableButton(this, wxID_ANY, m_bmp_add);
    add_layers_btn->SetBackgroundColour(GetBackgroundColour());
    ScalableButton* delete_layers_btn = new ScalableButton(this, wxID_ANY, m_bmp_delete);
    delete_layers_btn->SetBackgroundColour(GetBackgroundColour());
    buttons_sizer->Add(add_layers_btn, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER, FromDIP(5));
    buttons_sizer->Add(delete_layers_btn, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER, FromDIP(5));
    buttons_sizer->Show(false);

    m_layer_input_panel = new wxPanel(this);
    wxBoxSizer* layer_panel_sizer = new wxBoxSizer(wxVERTICAL);
    m_layer_input_panel->SetSizer(layer_panel_sizer);
    m_layer_input_panel->Hide();
    append_layer();

    top_sizer->Add(title_sizer, 0, wxEXPAND, 0);
    top_sizer->Add(buttons_sizer, 0, wxALIGN_CENTER, 0);
    top_sizer->Add(m_layer_input_panel, 0, wxEXPAND, 0);

    SetSizer(top_sizer);
    Layout();
    top_sizer->Fit(this);


    m_other_layer_print_seq_choice->Bind(wxEVT_COMBOBOX, [this, buttons_sizer](auto& e) {
        if (e.GetSelection() == 0) {
            m_layer_input_panel->Show(false);
            buttons_sizer->Show(false);
        }
        else if (e.GetSelection() == 1) {
            m_layer_input_panel->Show(true);
            buttons_sizer->Show(true);
        }
        m_parent->Layout();
        m_parent->Fit();
        });
    add_layers_btn->Bind(wxEVT_BUTTON, [this](wxEvent&) {
        Freeze();
        append_layer();
        m_parent->Layout();
        m_parent->Fit();
        Thaw();
        });
    delete_layers_btn->Bind(wxEVT_BUTTON, [this](wxEvent&) {
        popup_layer();
        m_parent->Layout();
        m_parent->Fit();
        });
    Bind(EVT_NEED_RESORT_LAYERS, [this](auto& evt) {
        std::vector<LayerSeqInfo> result;
        for (int i = 0; i < m_layer_input_sizer_list.size(); i++) {
            int begin_layer_number = m_begin_layer_input_list[i]->get_layer_number();
            int end_layer_number = m_end_layer_input_list[i]->get_layer_number();
            result.push_back({ begin_layer_number, end_layer_number, m_drag_canvas_list[i]->get_shape_list_order() });
        }
        if (!std::is_sorted(result.begin(), result.end())) {
            std::sort(result.begin(), result.end());
            sync_layers_print_seq(1, result);
        }
        result.swap(m_layer_seq_infos);
        });
    Bind(EVT_SET_BED_TYPE_CONFIRM, [this](auto& evt) {
        std::vector<LayerSeqInfo> result;
        for (int i = 0; i < m_layer_input_sizer_list.size(); i++) {
            int begin_layer_number = m_begin_layer_input_list[i]->get_layer_number();
            int end_layer_number = m_end_layer_input_list[i]->get_layer_number();

            if (!m_begin_layer_input_list[i]->is_layer_number_valid() || !m_end_layer_input_list[i]->is_layer_number_valid()) {
                MessageDialog msg_dlg(nullptr, _L("Please input layer value (>= 2)."), wxEmptyString, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
                evt.SetString("Invalid");
                return;
            }

            result.push_back({ begin_layer_number, end_layer_number, m_drag_canvas_list[i]->get_shape_list_order() });
        }
        result.swap(m_layer_seq_infos);
        });
}

void OtherLayersSeqPanel::append_layer(const LayerSeqInfo* layer_info)
{
    wxBoxSizer* layer_panel_sizer = static_cast<wxBoxSizer*>(m_layer_input_panel->GetSizer());

    wxStaticText* choose_layer_head_txt = new wxStaticText(m_layer_input_panel, wxID_ANY, _L("Layer"));
    choose_layer_head_txt->SetFont(Label::Body_14);

    LayerNumberTextInput* begin_layer_input = new LayerNumberTextInput(m_layer_input_panel, -1, wxSize(FromDIP(100), -1), LayerNumberTextInput::Type::Begin, LayerNumberTextInput::ValueType::Custom);

    wxStaticText* choose_layer_to_txt = new wxStaticText(m_layer_input_panel, wxID_ANY, _L("to"));
    choose_layer_to_txt->SetFont(Label::Body_14);

    LayerNumberTextInput* end_layer_input = new LayerNumberTextInput(m_layer_input_panel, -1, wxSize(FromDIP(100), -1), LayerNumberTextInput::Type::End, LayerNumberTextInput::ValueType::End);

    begin_layer_input->link(end_layer_input);
    if (m_begin_layer_input_list.size() == 0) {
        begin_layer_input->set_layer_number(MIN_LAYER_VALUE);
        end_layer_input->set_layer_number(MAX_LAYER_VALUE);
    }

    const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<int> order(extruder_colours.size());
    for (int i = 0; i < order.size(); i++) {
        order[i] = i + 1;
    }
    auto drag_canvas = new DragCanvas(m_layer_input_panel, extruder_colours, order);

    if (layer_info) {
        begin_layer_input->set_layer_number(std::max(MIN_LAYER_VALUE, layer_info->begin_layer_number));
        end_layer_input->set_layer_number(std::min(MAX_LAYER_VALUE, layer_info->end_layer_number));
        drag_canvas->set_shape_list(extruder_colours, layer_info->print_sequence);
    }

    wxBoxSizer* single_layer_input_sizer = new wxBoxSizer(wxHORIZONTAL);
    single_layer_input_sizer->Add(choose_layer_head_txt, 0, wxRIGHT | wxALIGN_CENTER, FromDIP(5));
    single_layer_input_sizer->Add(begin_layer_input, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER, FromDIP(5));
    single_layer_input_sizer->Add(choose_layer_to_txt, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER, 0);
    single_layer_input_sizer->Add(end_layer_input, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER, FromDIP(5));
    single_layer_input_sizer->AddStretchSpacer();
    single_layer_input_sizer->Add(drag_canvas, 0, wxLEFT | wxALIGN_CENTER, FromDIP(5));
    layer_panel_sizer->Add(single_layer_input_sizer, 0, wxEXPAND | wxALIGN_CENTER | wxBOTTOM, FromDIP(10));
    m_layer_input_sizer_list.push_back(single_layer_input_sizer);
    m_begin_layer_input_list.push_back(begin_layer_input);
    m_end_layer_input_list.push_back(end_layer_input);
    m_drag_canvas_list.push_back(drag_canvas);
}

void OtherLayersSeqPanel::popup_layer()
{
    if (m_layer_input_sizer_list.size() > 1) {
        m_layer_input_sizer_list.back()->Clear(true);
        m_layer_input_sizer_list.pop_back();
        m_begin_layer_input_list.pop_back();
        m_end_layer_input_list.pop_back();
        m_drag_canvas_list.pop_back();
    }
}

void OtherLayersSeqPanel::clear_all_layers()
{
    for (auto sizer : m_layer_input_sizer_list) {
        sizer->Clear(true);
    }
    m_layer_input_sizer_list.clear();
    m_begin_layer_input_list.clear();
    m_end_layer_input_list.clear();
    m_drag_canvas_list.clear();
}

void OtherLayersSeqPanel::sync_layers_print_seq(int selection, const std::vector<LayerSeqInfo>& seq)
{
    if (m_other_layer_print_seq_choice != nullptr) {
        if (selection == 1) {
            clear_all_layers();
            Freeze();
            for (int i = 0; i < seq.size(); i++) {
                append_layer(&seq[i]);
            }
            Thaw();
        }
        m_other_layer_print_seq_choice->SetSelection(selection);

        wxCommandEvent event(wxEVT_COMBOBOX);
        event.SetInt(selection);
        event.SetEventObject(m_other_layer_print_seq_choice);
        wxPostEvent(m_other_layer_print_seq_choice, event);
    }
}


PlateSettingsDialog::PlateSettingsDialog(wxWindow* parent, const wxString& title, bool only_layer_seq, const wxPoint& pos, const wxSize& size, long style)
:DPIDialog(parent, wxID_ANY, title, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(650), -1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

    wxFlexGridSizer* top_sizer = new wxFlexGridSizer(0, 2, FromDIP(5), 0);
    top_sizer->AddGrowableCol(0,1);
    top_sizer->SetFlexibleDirection(wxBOTH);
    top_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    auto plate_name_txt = new wxStaticText(this, wxID_ANY, _L("Plate name"));
    plate_name_txt->SetFont(Label::Body_14);
    m_ti_plate_name = new TextInput(this, wxString::FromDouble(0.0), "", "", wxDefaultPosition, wxSize(FromDIP(240),-1), wxTE_PROCESS_ENTER);
    top_sizer->Add(plate_name_txt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxTOP | wxBOTTOM, FromDIP(5));
    top_sizer->Add(m_ti_plate_name, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxTOP | wxBOTTOM, FromDIP(5));

    m_bed_type_choice = new ComboBox( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(240),-1), 0, NULL, wxCB_READONLY );
    auto pm           = wxGetApp().plater()->get_curr_printer_model();
    if (pm) {
        m_cur_combox_bed_types.clear();
        m_bed_type_choice->AppendString(_L("Same as Global Plate Type"));
        const ConfigOptionDef *bed_type_def = print_config_def.get("curr_bed_type");
        int                    index        = 0;
        for (auto item : bed_type_def->enum_labels) {
            index++;
            bool find = std::find(pm->not_support_bed_types.begin(), pm->not_support_bed_types.end(), item) != pm->not_support_bed_types.end();
            if (!find) {
                m_bed_type_choice->AppendString(_L(item));
                m_cur_combox_bed_types.emplace_back(BedType(index));
            }
        }

    } else {
        for (BedType i = btDefault; i < btCount; i = BedType(int(i) + 1)) {
            m_bed_type_choice->Append(to_bed_type_name(i));
        }
    }

    if (!wxGetApp().preset_bundle->is_bbl_vendor())
      m_bed_type_choice->Disable();

    wxStaticText* m_bed_type_txt = new wxStaticText(this, wxID_ANY, _L("Bed type"));
    m_bed_type_txt->SetFont(Label::Body_14);
    top_sizer->Add(m_bed_type_txt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxTOP | wxBOTTOM, FromDIP(5));
    top_sizer->Add(m_bed_type_choice, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxTOP | wxBOTTOM, FromDIP(5));

    // Print Sequence
    m_print_seq_choice = new ComboBox( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(240),-1), 0, NULL, wxCB_READONLY );
    m_print_seq_choice->Append(_L("Same as Global Print Sequence"));
    for (auto i = PrintSequence::ByLayer; i < PrintSequence::ByDefault; i = PrintSequence(int(i) + 1)) {
        m_print_seq_choice->Append(to_print_sequence_name(i));
    }
    wxStaticText* m_print_seq_txt = new wxStaticText(this, wxID_ANY, _L("Print sequence"));
    m_print_seq_txt->SetFont(Label::Body_14);
    top_sizer->Add(m_print_seq_txt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxTOP | wxBOTTOM, FromDIP(5));
    top_sizer->Add(m_print_seq_choice, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxTOP | wxBOTTOM, FromDIP(5));

    // Spiral mode
    m_spiral_mode_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(240), -1), 0, NULL, wxCB_READONLY);
    m_spiral_mode_choice->Append(_L("Same as Global"));
    m_spiral_mode_choice->Append(_L("Enable"));
    m_spiral_mode_choice->Append(_L("Disable"));
    m_spiral_mode_choice->SetSelection(0);
    wxStaticText* spiral_mode_txt = new wxStaticText(this, wxID_ANY, _L("Spiral vase"));
    spiral_mode_txt->SetFont(Label::Body_14);
    top_sizer->Add(spiral_mode_txt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxTOP | wxBOTTOM, FromDIP(5));
    top_sizer->Add(m_spiral_mode_choice, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxTOP | wxBOTTOM, FromDIP(5));

    // First layer filament sequence
    m_first_layer_print_seq_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(240), -1), 0, NULL, wxCB_READONLY);
    m_first_layer_print_seq_choice->Append(_L("Auto"));
    m_first_layer_print_seq_choice->Append(_L("Customize"));
    m_first_layer_print_seq_choice->SetSelection(0);
    m_first_layer_print_seq_choice->Bind(wxEVT_COMBOBOX, [this](auto& e) {
        if (e.GetSelection() == 0) {
            m_drag_canvas->Hide();
        }
        else if (e.GetSelection() == 1) {
            m_drag_canvas->Show();
        }
        Layout();
        Fit();
        });
    wxStaticText* first_layer_txt = new wxStaticText(this, wxID_ANY, _L("First layer filament sequence"));
    first_layer_txt->SetFont(Label::Body_14);
    top_sizer->Add(first_layer_txt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxTOP | wxBOTTOM, FromDIP(5));
    top_sizer->Add(m_first_layer_print_seq_choice, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxTOP | wxBOTTOM, FromDIP(5));

    const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<int> order(extruder_colours.size());
    for (int i = 0; i < order.size(); i++) {
        order[i] = i + 1;
    }
    m_drag_canvas = new DragCanvas(this, extruder_colours, order);
    m_drag_canvas->Hide();
    top_sizer->Add(0, 0, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT, 0);
    top_sizer->Add(m_drag_canvas, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxBOTTOM, FromDIP(10));

    m_sizer_main->Add(top_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(30));

    // Other layer filament sequence
    m_other_layers_seq_panel = new OtherLayersSeqPanel(this);
    m_sizer_main->AddSpacer(FromDIP(5));
    m_sizer_main->Add(m_other_layers_seq_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](auto& e) {
        wxCommandEvent evt(EVT_SET_BED_TYPE_CONFIRM, GetId());
        static_cast<wxEvtHandler*>(m_other_layers_seq_panel)->ProcessEvent(evt);
        GetEventHandler()->ProcessEvent(evt);
        if (evt.GetString() == "Invalid")
            return;
        if (this->IsModal())
            EndModal(wxID_YES);
        else
            this->Close();
        });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](auto& e) {
        if (this->IsModal())
            EndModal(wxID_NO);
        else
            this->Close();
        });

    m_sizer_main->AddSpacer(FromDIP(20));
    m_sizer_main->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(m_sizer_main);
    Layout();
    m_sizer_main->Fit(this);

    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);

    if (only_layer_seq) {
        for (auto item : top_sizer->GetChildren()) {
            if (item->GetWindow())
                item->GetWindow()->Show(false);
        }
        first_layer_txt->Show();
        m_first_layer_print_seq_choice->Show();
        m_drag_canvas->Show();
        m_other_layers_seq_panel->Show();
        Layout();
        Fit();
    }
}

PlateSettingsDialog::~PlateSettingsDialog()
{

}

void PlateSettingsDialog::sync_bed_type(BedType type)
{
    if (m_bed_type_choice != nullptr) {
        for (int i = 0; i < m_cur_combox_bed_types.size(); i++) {
            if (m_cur_combox_bed_types[i] == type) {
                m_bed_type_choice->SetSelection(i + 1);//+1 because same as global
                return;
            }
        }
        m_bed_type_choice->SetSelection(0);
    }
}

void PlateSettingsDialog::sync_print_seq(int print_seq)
{
    if (m_print_seq_choice != nullptr) {
        m_print_seq_choice->SetSelection(print_seq);
    }
}

void PlateSettingsDialog::sync_first_layer_print_seq(int selection, const std::vector<int>& seq)
{
    if (m_first_layer_print_seq_choice != nullptr) {
        if (selection == 1) {
            const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
            m_drag_canvas->set_shape_list(extruder_colours, seq);
        }
        m_first_layer_print_seq_choice->SetSelection(selection);

        wxCommandEvent event(wxEVT_COMBOBOX);
        event.SetInt(selection);
        event.SetEventObject(m_first_layer_print_seq_choice);
        wxPostEvent(m_first_layer_print_seq_choice, event);
    }
}

void PlateSettingsDialog::sync_other_layers_print_seq(int selection, const std::vector<LayerPrintSequence>& seq) {
    if (selection == 1) {
        std::vector<LayerSeqInfo> sequences;
        sequences.reserve(seq.size());
        for (int i = 0; i < seq.size(); i++) {
            LayerSeqInfo info{ seq[i].first.first, seq[i].first.second, seq[i].second };
            sequences.push_back(info);
        }
        m_other_layers_seq_panel->sync_layers_print_seq(selection, sequences);
    }
    else {
        m_other_layers_seq_panel->sync_layers_print_seq(selection, {});
    }
}

void PlateSettingsDialog::sync_spiral_mode(bool spiral_mode, bool as_global)
{
    if (m_spiral_mode_choice) {
        if (as_global) {
            m_spiral_mode_choice->SetSelection(0);
        }
        else {
            if (spiral_mode)
                m_spiral_mode_choice->SetSelection(1);
            else
                m_spiral_mode_choice->SetSelection(2);
        }
    }
}

wxString PlateSettingsDialog::to_bed_type_name(BedType bed_type) {
    switch (bed_type) {
    case btDefault:
        return _L("Same as Global Plate Type");
    default: {
        const ConfigOptionDef *bed_type_def = print_config_def.get("curr_bed_type");
        return _(bed_type_def->enum_labels[size_t(bed_type) - 1]);
        }
    }
    return _L("Same as Global Bed Type");
}

wxString PlateSettingsDialog::to_print_sequence_name(PrintSequence print_seq) {
    switch (print_seq) {
    case PrintSequence::ByLayer:
        return _L("By Layer");
    case PrintSequence::ByObject:
        return _L("By Object");
    default:
        return _L("By Layer");
    }
    return _L("By Layer");
}

void PlateSettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
}

wxString PlateSettingsDialog::get_plate_name() const {
    return m_ti_plate_name->GetTextCtrl()->GetValue(); 
}

void PlateSettingsDialog::set_plate_name(const wxString &name) { m_ti_plate_name->GetTextCtrl()->SetValue(name); }

BedType PlateSettingsDialog::get_bed_type_choice()
{
    if (m_bed_type_choice != nullptr) {
        int choice = m_bed_type_choice->GetSelection();
        if (choice > 0) {
            return m_cur_combox_bed_types[choice - 1];//-1 because same as globlal
        }
    }
    return BedType::btDefault;
};

std::vector<int> PlateSettingsDialog::get_first_layer_print_seq()
{
    return m_drag_canvas->get_shape_list_order();
}


//PlateNameEditDialog
PlateNameEditDialog::PlateNameEditDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, title, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), -1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxFlexGridSizer *top_sizer = new wxFlexGridSizer(0, 2, FromDIP(5), 0);
    top_sizer->AddGrowableCol(0, 1);
    top_sizer->SetFlexibleDirection(wxBOTH);
    top_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    auto plate_name_txt = new wxStaticText(this, wxID_ANY, _L("Plate name"));
    plate_name_txt->SetFont(Label::Body_14);
    m_ti_plate_name = new TextInput(this, wxString::FromDouble(0.0), "", "", wxDefaultPosition, wxSize(FromDIP(240), -1), wxTE_PROCESS_ENTER);
    m_ti_plate_name->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &e) {
        if (this->IsModal())
            EndModal(wxID_YES);
        else
            this->Close();
    });
    top_sizer->Add(plate_name_txt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
    top_sizer->Add(m_ti_plate_name, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, FromDIP(5));
    m_ti_plate_name->GetTextCtrl()->SetMaxLength(250);

    m_sizer_main->Add(top_sizer, 0, wxEXPAND | wxALL, FromDIP(30));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (this->IsModal())
            EndModal(wxID_YES);
        else
            this->Close();
    });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (this->IsModal())
            EndModal(wxID_NO);
        else
            this->Close();
    });

    m_sizer_main->Add(dlg_btns, 0, wxEXPAND, FromDIP(20));

    SetSizer(m_sizer_main);
    Layout();
    m_sizer_main->Fit(this);

    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

PlateNameEditDialog::~PlateNameEditDialog() {}

void PlateNameEditDialog::on_dpi_changed(const wxRect &suggested_rect)
{
}


wxString PlateNameEditDialog::get_plate_name() const { return m_ti_plate_name->GetTextCtrl()->GetValue(); }

void PlateNameEditDialog::set_plate_name(const wxString &name) {
    m_ti_plate_name->GetTextCtrl()->SetValue(name);
    m_ti_plate_name->GetTextCtrl()->SetFocus();
    m_ti_plate_name->GetTextCtrl()->SetInsertionPointEnd();
}


}
} // namespace Slic3r::GUI
