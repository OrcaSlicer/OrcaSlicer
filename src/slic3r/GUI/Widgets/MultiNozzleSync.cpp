#include "MultiNozzleSync.hpp"
#include "../GUI_App.hpp"
#include "../DeviceCore/DevManager.h"
#include "libslic3r/PresetBundle.hpp"
#include <wx/sizer.h>


namespace Slic3r::GUI{

wxDEFINE_EVENT(EVT_NOZZLE_SELECTED, wxCommandEvent);

static const int LeftExtruderIdx = 0;
static const int RightExtruderIdx = 1;

ManualNozzleCountDialog::ManualNozzleCountDialog(wxWindow *parent, NozzleVolumeType volume_type, int standard_count, int highflow_count, int max_nozzle_count, bool force_no_zero)
    : GUI::DPIDialog(parent, wxID_ANY, "Set nozzle count", wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX), m_volume_type(volume_type)
{
    this->SetBackgroundColour(*wxWHITE);
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    wxPanel *content = new wxPanel(this);
    content->SetBackgroundColour(*wxWHITE);

    wxBitmap icon        = create_scaled_bitmap("hotend_thumbnail", nullptr, FromDIP(60));
    auto    *nozzle_icon = new wxStaticBitmap(content, wxID_ANY, icon);

    wxBoxSizer *content_sizer = new wxBoxSizer(wxHORIZONTAL);
    content->SetSizer(content_sizer);

    wxBoxSizer *choice_sizer = new wxBoxSizer(wxVERTICAL);

    auto* label = new wxStaticText(content, wxID_ANY, _L("Please set nozzle count"));
    choice_sizer->Add(label, 0, wxALL | wxALIGN_LEFT, FromDIP(10));

    wxArrayString nozzle_choices;
    for (int i = 0; i <= max_nozzle_count; ++i) nozzle_choices.Add(wxString::Format("%d", i));

    bool show_standard = (volume_type == nvtStandard || volume_type == nvtHybrid);
    bool show_highflow = (volume_type == nvtHighFlow || volume_type == nvtHybrid);

    if (show_standard) {
        // Standard nozzle choice
        auto *standard_label = new wxStaticText(content, wxID_ANY, _L(get_nozzle_volume_type_string(nvtStandard)));
        choice_sizer->Add(standard_label, 0, wxALL | wxALIGN_LEFT, FromDIP(5));

        m_standard_choice = new wxChoice(content, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(100), -1), nozzle_choices);
        m_standard_choice->SetSelection(standard_count);
        m_standard_choice->SetBackgroundColour(*wxWHITE);
        choice_sizer->Add(m_standard_choice, 0, wxLEFT | wxBOTTOM | wxRIGHT, FromDIP(10));
    }

    if (show_highflow) {
        // Highflow nozzle choice
        auto *highflow_label = new wxStaticText(content, wxID_ANY, _L(get_nozzle_volume_type_string(nvtHighFlow)));
        choice_sizer->Add(highflow_label, 0, wxALL | wxALIGN_LEFT, FromDIP(5));

        m_highflow_choice = new wxChoice(content, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(100), -1), nozzle_choices);
        m_highflow_choice->SetSelection(highflow_count);
        m_highflow_choice->SetBackgroundColour(*wxWHITE);
        choice_sizer->Add(m_highflow_choice, 0, wxLEFT | wxBOTTOM | wxRIGHT, FromDIP(10));
    }

    m_error_label = new Label(this, "");
    m_error_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#E14747")));
    m_error_label->SetFont(Label::Body_12);
    m_error_label->Hide();

    auto update_nozzle_error = [this, force_no_zero, content, max_nozzle_count](int standard_count, int highflow_count) {
        int total_count = standard_count + highflow_count;
        if (0 < total_count && total_count <= max_nozzle_count&& m_error_label->IsShown()) {
            m_error_label->Hide();
            m_confirm_btn->Enable();
            content->Freeze();
            this->Layout();
            this->Fit();
            content->Thaw();
        } else if (total_count == 0 && force_no_zero || total_count > max_nozzle_count) {
            wxString error_tip;
            if (total_count == 0)
                error_tip = _L("Error: Can not set both nozzle count to zero.");
            else
                error_tip = wxString::Format(_L("Error: Nozzle count can not exceed %d."), max_nozzle_count);

            m_error_label->SetLabel(error_tip);
            m_error_label->Wrap(content->GetSize().x);
            m_error_label->Show();
            m_confirm_btn->Disable();
            content->Freeze();
            this->Layout();
            this->Fit();
            content->Thaw();
        }
    };

    if (m_standard_choice) {
        m_standard_choice->Bind(wxEVT_CHOICE, [this, update_nozzle_error](wxCommandEvent &e) {
            int standard_count = m_standard_choice->GetSelection();
            int highflow_count = m_highflow_choice ? m_highflow_choice->GetSelection() : 0;
            update_nozzle_error(standard_count, highflow_count);
            e.Skip();
        });
    }

    if (m_highflow_choice) {
        m_highflow_choice->Bind(wxEVT_CHOICE, [this, update_nozzle_error](wxCommandEvent &e) {
            int standard_count = m_standard_choice ? m_standard_choice->GetSelection() : 0;
            int highflow_count = m_highflow_choice->GetSelection();
            update_nozzle_error(standard_count, highflow_count);
            e.Skip();
        });
    }

    content_sizer->Add(nozzle_icon, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(15));
    content_sizer->Add(choice_sizer, 0, wxALIGN_CENTRE_VERTICAL);

    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered), std::pair<wxColour, int>(wxColour(0, 174, 66), StateColor::Normal));

    m_confirm_btn = new Button(this, _L("Confirm"));
    m_confirm_btn->SetBackgroundColor(btn_bg_green);
    m_confirm_btn->SetTextColor(StateColor::darkModeColorFor("#FFFFFE"));
    m_confirm_btn->SetMinSize(wxSize(FromDIP(68), FromDIP(23)));
    m_confirm_btn->SetMinSize(wxSize(FromDIP(68), FromDIP(23)));
    m_confirm_btn->SetCornerRadius(12);
    m_confirm_btn->Bind(wxEVT_LEFT_DOWN, [this](auto &e) { EndModal(wxID_OK); });

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(content, 1, wxEXPAND);
    mainSizer->Add(m_error_label, 0, wxALL, FromDIP(5));
    mainSizer->Add(m_confirm_btn, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(20));

    SetSizerAndFit(mainSizer);
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

int ManualNozzleCountDialog::GetNozzleCount(NozzleVolumeType volume_type) const
{
    if(volume_type == nvtStandard)
        return m_standard_choice ? m_standard_choice->GetSelection() : 0;
    else if(volume_type == nvtHighFlow)
        return m_highflow_choice ? m_highflow_choice->GetSelection() : 0;
    else if(volume_type == nvtHybrid)
        return (m_standard_choice ? m_standard_choice->GetSelection() : 0) + (m_highflow_choice ? m_highflow_choice->GetSelection() : 0);
    return 0;
}

void manuallySetNozzleCount(int extruder_id)
{
    auto& preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle)
        return;

    auto full_config = GUI::wxGetApp().preset_bundle->full_config();
    auto extruder_max_nozzle_count = full_config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    if (!extruder_max_nozzle_count || !std::any_of(extruder_max_nozzle_count->values.begin(), extruder_max_nozzle_count->values.end(), [](int val) {return val > 1; }))
        return;

    ConfigOptionEnumsGeneric* nozzle_volume_type_opt = full_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    NozzleVolumeType volume_type = NozzleVolumeType(nozzle_volume_type_opt->values[extruder_id]);
    double nozzle_diameter = full_config.option<ConfigOptionFloatsNullable>("nozzle_diameter")->values[extruder_id];
    int standard_count = preset_bundle->extruder_nozzle_stat.get_extruder_nozzle_count(extruder_id, nvtStandard);
    int highflow_count = preset_bundle->extruder_nozzle_stat.get_extruder_nozzle_count(extruder_id, nvtHighFlow);


    bool force_no_zero = volume_type == nvtHybrid;
    if(nozzle_volume_type_opt->values.size() > 1){
        int other_nozzle_count = preset_bundle->extruder_nozzle_stat.get_extruder_nozzle_count(1 - extruder_id);
        force_no_zero |= (other_nozzle_count == 0);
    }

    ManualNozzleCountDialog dialog(GUI::wxGetApp().plater(), volume_type, standard_count, highflow_count, extruder_max_nozzle_count->values[extruder_id], force_no_zero);

    if (dialog.ShowModal() == wxID_OK) {
        int nozzle_count = dialog.GetNozzleCount(volume_type);
        if (volume_type == nvtHybrid) {
            setExtruderNozzleCount(preset_bundle, extruder_id, nvtStandard, dialog.GetNozzleCount(nvtStandard), true);
            setExtruderNozzleCount(preset_bundle, extruder_id, nvtHighFlow, dialog.GetNozzleCount(nvtHighFlow), false);
        }
        else {
            setExtruderNozzleCount(preset_bundle, extruder_id, volume_type, nozzle_count, true);
        }
        updateNozzleCountDisplay(preset_bundle, extruder_id, volume_type);
        wxGetApp().plater()->update();
    }
}

ExtruderBadge::ExtruderBadge(wxWindow* parent) : wxPanel(parent)
{
    wxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour("#F8F8F8");
    wxBitmap icon = create_scaled_bitmap("extruder_badge_none_selected", nullptr, FromDIP(90));

    auto extruder_label = new Label(this, _L("Extruder"));

    badget = new wxStaticBitmap(this, wxID_ANY, icon);

    m_diameter_list = { "0.4","0.4" };
    m_volume_type_list = { NozzleVolumeType::nvtStandard,NozzleVolumeType::nvtStandard };

    left_diameter_desp = new Label(this, _L(m_diameter_list[LeftExtruderIdx]));
    right_diameter_desp = new Label(this, _L(m_diameter_list[RightExtruderIdx]));
    left_flow_desp = new Label(this, _L(get_nozzle_volume_type_string(m_volume_type_list[LeftExtruderIdx])));
    right_flow_desp = new Label(this, _L(get_nozzle_volume_type_string(m_volume_type_list[RightExtruderIdx])));


    wxBoxSizer* top_h = new wxBoxSizer(wxVERTICAL);
    top_h->Add(extruder_label, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(10));

    main_sizer->Add(top_h, 0, wxEXPAND | wxTOP, FromDIP(5));
    main_sizer->Add(badget, 0, wxALIGN_CENTER | wxTOP, FromDIP(5));

    wxBoxSizer* left_extruder = new wxBoxSizer(wxVERTICAL);

    left_extruder->Add(left_diameter_desp, 0, wxALIGN_CENTER | wxLEFT, FromDIP(12));
    left_extruder->Add(left_flow_desp, 0, wxALIGN_CENTER | wxLEFT, FromDIP(12));

    wxBoxSizer* right_extruder = new wxBoxSizer(wxVERTICAL);
    right_extruder->Add(right_diameter_desp, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(12));
    right_extruder->Add(right_flow_desp, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(12));

    wxBoxSizer* info_sizer = new wxBoxSizer(wxHORIZONTAL);
    info_sizer->Add(left_extruder, 0);
    info_sizer->AddStretchSpacer();
    info_sizer->Add(right_extruder, 0);

    main_sizer->Add(info_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));

    SetSizer(main_sizer);
    Layout();
    Fit();
    wxGetApp().UpdateDarkUIWin(this);
}

void ExtruderBadge::SetExtruderInfo(int extruder_id, const std::string& diameter, const NozzleVolumeType& volume_type)
{
    m_diameter_list[extruder_id] = diameter;
    m_volume_type_list[extruder_id] = volume_type;

    if (extruder_id == LeftExtruderIdx) {
        left_diameter_desp->SetLabel(diameter + " mm");
        left_flow_desp->SetLabel(_L(get_nozzle_volume_type_string(volume_type)));
    }
    else if (extruder_id == RightExtruderIdx) {
        right_diameter_desp->SetLabel(diameter + " mm");
        right_flow_desp->SetLabel(_L(get_nozzle_volume_type_string(volume_type)));
    }
}

void ExtruderBadge::SetExtruderValid(bool right_on)
{
    if (!right_on) {
        right_diameter_desp->SetLabel("");
        right_flow_desp->SetLabel("");
    }
    std::string badge_name;
    if (m_right_on)
        badge_name = "extruder_badge_none_selected";
    else
        badge_name = "extruder_badge_none_selected_single";
    wxBitmap icon = create_scaled_bitmap(badge_name, nullptr, FromDIP(90));
    badget->SetBitmap(icon);

    m_right_on = right_on;
}

void ExtruderBadge::SetExtruderStatus(bool left_selected, bool right_selected)
{
    std::string badge_name;
    if (m_right_on) {
        if (left_selected && right_selected)
            badge_name = "extruder_badge_both_selected";
        else if (left_selected)
            badge_name = "extruder_badge_left_selected";
        else if (right_selected)
            badge_name = "extruder_badge_right_selected";
        else
            badge_name = "extruder_badge_none_selected";
    }
    else if (left_selected) {
        badge_name = "extruder_badge_left_selected_single";
    }
    else {
        badge_name = "extruder_badge_none_selected_single";
    }

    wxBitmap icon = create_scaled_bitmap(badge_name, nullptr, FromDIP(90));
    badget->SetBitmap(icon);
    Layout();
}


void ExtruderBadge::UnMarkRelatedItems(const NozzleOption& option)
{
    bool left_selected = true, right_selected = true;

    if (m_diameter_list[LeftExtruderIdx] == option.diameter && option.extruder_nozzle_stats.count(LeftExtruderIdx)
        && option.extruder_nozzle_stats.at(LeftExtruderIdx).count(m_volume_type_list[LeftExtruderIdx])
        && option.extruder_nozzle_stats.at(LeftExtruderIdx).at(m_volume_type_list[LeftExtruderIdx])>0)
        left_selected = false;

    if (m_diameter_list[RightExtruderIdx] == option.diameter && option.extruder_nozzle_stats.count(RightExtruderIdx)
        && option.extruder_nozzle_stats.at(RightExtruderIdx).count(m_volume_type_list[RightExtruderIdx])
        && option.extruder_nozzle_stats.at(RightExtruderIdx).at(m_volume_type_list[RightExtruderIdx])>0)
        right_selected = false;

    SetExtruderStatus(left_selected, right_selected);
}

void ExtruderBadge::MarkRelatedItems(const NozzleOption& option)
{
    bool left_selected = false, right_selected = false;

    if (m_diameter_list[LeftExtruderIdx] == option.diameter && option.extruder_nozzle_stats.count(LeftExtruderIdx)
        && option.extruder_nozzle_stats.at(LeftExtruderIdx).count(m_volume_type_list[LeftExtruderIdx])
        && option.extruder_nozzle_stats.at(LeftExtruderIdx).at(m_volume_type_list[LeftExtruderIdx]) > 0)
        left_selected = true;

    if (m_diameter_list[RightExtruderIdx] == option.diameter && option.extruder_nozzle_stats.count(RightExtruderIdx)
        && option.extruder_nozzle_stats.at(RightExtruderIdx).count(m_volume_type_list[RightExtruderIdx])
        && option.extruder_nozzle_stats.at(RightExtruderIdx).at(m_volume_type_list[RightExtruderIdx]) > 0)
        right_selected = true;

    SetExtruderStatus(left_selected, right_selected);
}

HotEndTable::HotEndTable(wxWindow* parent) :  wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,wxBORDER_NONE)
{
    Bind(wxEVT_PAINT, &HotEndTable::OnPaint, this);
    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    auto label = new Label(this, _L("Induction Hotend Rack"));
    label->SetBackgroundColour("#F8F8F8");

    m_arow_nozzle_box = CreateNozzleBox({ 0,2,4 });
    m_brow_nozzle_box = CreateNozzleBox({ 1,3,5 });
    main_sizer->Add(label, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(5));
    main_sizer->Add(m_arow_nozzle_box, 0, wxLEFT | wxRIGHT, FromDIP(5));
    main_sizer->Add(m_brow_nozzle_box, 0, wxLEFT | wxRIGHT, FromDIP(5));
    SetBackgroundColour("#F8F8F8");

    SetSizer(main_sizer);
    Layout();
    Fit();
    wxGetApp().UpdateDarkUIWin(this);
}

void HotEndTable::UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    const auto& nozzle_rack = rack.lock();
    if (nozzle_rack) {
        UpdateNozzleItems(m_nozzle_items, nozzle_rack);
    }
}

std::vector<int> HotEndTable::FilterHotEnds(const NozzleOption& option)
{
    auto rack = m_nozzle_rack.lock();
    if (!rack)
        return {};

    std::vector<HotEndAttr> nozzles_to_search;

    for (auto& item : option.extruder_nozzle_stats) {
        for (auto& nozzle : item.second) {
            HotEndAttr info;
            info.diameter = option.diameter;
            info.extruder_id = item.first;
            info.volume_type = nozzle.first;
            nozzles_to_search.emplace_back(info);
        }
    }

    std::vector<int> filtered_nozzles;

    for (auto& info : nozzles_to_search) {

        float diameter = atof(info.diameter.c_str());
        NozzleFlowType flow = DevNozzle::ToNozzleFlowType(info.volume_type);
        int extruder_id = 1 - info.extruder_id; //physical

        auto nozzles = rack->GetNozzleSystem()->CollectNozzles(extruder_id, flow, diameter);

        for (auto& nozzle : nozzles) {
            if (nozzle.IsOnRack())
                filtered_nozzles.emplace_back(nozzle.GetNozzleId());
        }
    }

    return filtered_nozzles;
}

void HotEndTable::MarkRelatedItems(const NozzleOption& option)
{
    const static StateColor bg_green(
        std::pair<wxColour, int>(wxColour("#DBFDE7"), StateColor::Normal)
    );

    const static StateColor bd_green(
        std::pair<wxColour, int>(wxColour("#00AE42"), StateColor::Normal)
    );
    auto filtered_nozzles = FilterHotEnds(option);
    for (auto nozzle_id : filtered_nozzles) {
        auto iter = m_nozzle_items.find(nozzle_id);
        if (iter == m_nozzle_items.end())
            continue;
        auto& item = iter->second;
        item->SetBackgroundColor(bg_green);
        item->SetBorderColor(bd_green);
        for (auto child : item->GetChildren()) {
            child->SetBackgroundColour("#DBFDE7");
        }
    }
    wxGetApp().UpdateDarkUIWin(this);
}

void HotEndTable::UnMarkRelatedItems(const NozzleOption& option)
{
    static const wxColour bg_color("#EEEEEE");
    static const wxColour bd_color("#CECECE");
    const static StateColor bg_green(
        std::pair<wxColour, int>(bg_color, StateColor::Normal)
    );

    const static StateColor bd_green(
        std::pair<wxColour, int>(bd_color, StateColor::Normal)
    );
    auto filtered_nozzles = FilterHotEnds(option);
    for (auto nozzle_id : filtered_nozzles) {
        auto iter = m_nozzle_items.find(nozzle_id);
        if (iter == m_nozzle_items.end())
            continue;
        auto& item = iter->second;
        item->SetBackgroundColor(bg_green);
        item->SetBorderColor(bd_green);
        for (auto child : item->GetChildren()) {
            child->SetBackgroundColour(bg_color);
        }
    }
    wxGetApp().UpdateDarkUIWin(this);
}



StaticBox* HotEndTable::CreateNozzleBox(const std::vector<int>& nozzle_indices)
{
    StaticBox* nozzle_box = new StaticBox(this);
    nozzle_box->SetBackgroundColour("#F8F8F8");
    nozzle_box->SetBorderColorNormal("#F8F8F8");
    nozzle_box->SetCornerRadius(0);

    wxSizer* h_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (auto idx : nozzle_indices) {
        wgtDeviceNozzleRackNozzleItem* nozzle_item = new wgtDeviceNozzleRackNozzleItem(nozzle_box, idx);
        nozzle_item->SetBackgroundColorNormal("#EEEEEE");
        for (auto& child : nozzle_item->GetChildren())
            child->SetBackgroundColour("#EEEEEE");
        m_nozzle_items[idx] = nozzle_item;
        h_sizer->Add(nozzle_item, 0, wxALL, FromDIP(8));
    }

    nozzle_box->SetSizer(h_sizer);

    return nozzle_box;
}

void HotEndTable::UpdateNozzleItems(const std::unordered_map<int, wgtDeviceNozzleRackNozzleItem*>& nozzle_items, std::shared_ptr<DevNozzleRack> nozzle_rack)
{
    for (auto& item : nozzle_items)
        item.second->Update(nozzle_rack);
}

void HotEndTable::OnPaint(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    wxSize size = GetClientSize();

    dc.SetPen(wxPen(wxColour("#EEEEEE"), 2));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, size.GetWidth()-2, size.GetHeight()-2, 5);
}

NozzleListTable::NozzleListTable(wxWindow* parent) : wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize ,wxNO_BORDER)
{
    m_web_view = wxWebView::New(this, wxID_ANY, wxEmptyString, wxDefaultPosition,wxDefaultSize,wxString::FromAscii(wxWebViewBackendDefault),wxNO_BORDER);
    m_web_view->AddScriptMessageHandler("nozzleListTable");
    m_web_view->EnableContextMenu(false);
    fs::path filepath = fs::path(resources_dir()) / "web/flush/NozzleListTable.html";
    wxFileName fn(wxString::FromUTF8(filepath.string()));
    wxString url = wxFileSystem::FileNameToURL(fn);
    m_web_view->LoadURL(url);

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer(0);
    sizer->Add(m_web_view, 1, wxEXPAND);
    sizer->AddStretchSpacer(0);
    SetSizer(sizer);
    Layout();

    m_web_view->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this,sizer](wxWebViewEvent& evt) {
        std::string message = evt.GetString().ToStdString();
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << "Received message: " << message;
        try {
            json j = json::parse(message);
            if (j["msg"].get<std::string>() == "init") {
                auto table_obj_str = BuildTableObjStr();
                auto text_obj_str = BuildTextObjStr();
                CallAfter([table_obj_str, text_obj_str, this] {
                    wxString script1 = wxString::Format("initText(%s)", text_obj_str);
                    m_web_view->RunScript(script1);
                    wxString script2 = wxString::Format("initTable(%s)", table_obj_str);
                    m_web_view->RunScript(script2);
                    });
            }
            else if (j["msg"].get<std::string>() == "updateList") {
                auto table_obj_str = BuildTableObjStr();

                CallAfter([table_obj_str, this] {
                    wxString script1 = wxString::Format("updateTable(%s)", table_obj_str);
                    m_web_view->RunScript(script1);
                    });

            }
            else if (j["msg"].get<std::string>() == "onSelect") {
                int idx = j["index"].get<int>();
                m_selected_idx = idx;
                SendSelectionChangedEvent();
            }
            else if (j["msg"].get<std::string>() == "layout") {
                int height = j["height"].get<int>();
                int width = j["width"].get<int>();
                wxSize table_size = wxSize(-1, FromDIP(height));
                m_web_view->SetSize(table_size);
                m_web_view->SetMaxSize(table_size);
                m_web_view->SetMinSize(table_size);
                this->Layout();
                this->GetParent()->Layout();
                this->GetParent()->Fit();
            }
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Failed to parse json message: " << message;
        }
        });

    wxGetApp().UpdateDarkUIWin(this);
}

wxString NozzleListTable::BuildTableObjStr()
{
    json obj;
    obj["darkmode"] = wxGetApp().dark_mode();
    obj["data"] = json::array();
    for(size_t idx = 0; idx < m_nozzle_options.size(); ++idx){
        const auto& option = m_nozzle_options[idx];
        json json_opt;
        json_opt["diameter"] = option.diameter;
        json_opt["is_selected"] = idx == m_selected_idx;
        json_opt["darkmode"] = wxGetApp().dark_mode();

        json extruders = json::object();
        for (const auto& [extruderId, nozzleInfo] : option.extruder_nozzle_stats) {
            nlohmann::json nozzleData;
            nozzleData["type"] = "";
            nozzleData["count"] = std::accumulate(nozzleInfo.begin(), nozzleInfo.end(), 0, [](int val, auto elem) {return val + elem.second; });
            extruders[std::to_string(extruderId)] = nozzleData;
        }
        json_opt["extruders"] = extruders;
        obj["data"].push_back(json_opt);
    }
    return wxString::FromUTF8(obj.dump().c_str());
}

void NozzleListTable::SendSelectionChangedEvent()
{
    wxCommandEvent event(EVT_NOZZLE_SELECTED, GetId());
    event.SetInt(m_selected_idx);
    event.SetEventObject(this);
    ProcessWindowEvent(event);
}
wxString NozzleListTable::BuildTextObjStr()
{
    wxString nozzle_selection = _L("Nozzle Selection");
    wxString nozzle_list = _L("Available Nozzles");
    wxString Left = _L("Left");
    wxString Right = _L("Right");
    wxString highflow = _L(get_nozzle_volume_type_string(nvtHighFlow));
    wxString standard = _L(get_nozzle_volume_type_string(nvtStandard));

    wxString text_obj = "{";
    text_obj += wxString::Format("\"nozzle_selection_label\":\"%s\",", nozzle_selection);
    text_obj += wxString::Format("\"nozzle_list_label\":\"%s\",", nozzle_list);
    text_obj += wxString::Format("\"left_label\":\"%s\",", Left);
    text_obj += wxString::Format("\"right_label\":\"%s\",", Right);
    text_obj += wxString::Format("\"highflow_label\":\"%s\",", highflow);
    text_obj += wxString::Format("\"standard_label\":\"%s\",", standard);
    text_obj += wxString::Format("\"language\":\"%s\"", wxGetApp().app_config->get_language_code());
    text_obj += "}";

    return text_obj;
}

void NozzleListTable::SetOptions(const std::vector<NozzleOption>& options,int default_select)
{
    m_nozzle_options = options;
    m_selected_idx = default_select;
    auto table_obj_str = BuildTableObjStr();
    wxString script1 = wxString::Format("updateTable(%s)", table_obj_str);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "update table " << script1;

#if 1
    m_web_view->RunScript(script1);
#else
    CallAfter([script1, this]() {
        m_web_view->RunScript(script1);
        });
#endif
}

MultiNozzleStatusTable::MultiNozzleStatusTable(wxWindow* parent): wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    m_badge = new ExtruderBadge(this);

    SetBackgroundColour(wxColour("#F8F8F8"));

    auto main_sizer    = new wxBoxSizer(wxVERTICAL);

    auto title_panel = new wxPanel(this);
    title_panel->SetBackgroundColour(0xEEEEEE);
    auto title_sizer = new wxBoxSizer(wxHORIZONTAL);
    title_panel->SetSizer(title_sizer);

    Label* static_text = new Label(this, _L("Nozzle Info"));
    static_text->SetFont(Label::Head_13);
    static_text->SetBackgroundColour(0xEEEEEE);

    title_sizer->Add(static_text, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

    main_sizer->Add(title_panel, 0, wxEXPAND);
    main_sizer->AddSpacer(10);

    wxSizer* nozzle_area_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_table = new HotEndTable(this);

    nozzle_area_sizer->Add(m_badge, 0, wxLEFT | wxRIGHT, FromDIP(20));
    nozzle_area_sizer->Add(m_table, 0, wxRIGHT, FromDIP(10));

    main_sizer->Add(nozzle_area_sizer);
    main_sizer->AddSpacer(FromDIP(5));

    SetSizer(main_sizer);
    Layout();
    Fit();
    wxGetApp().UpdateDarkUIWin(this);
}

void MultiNozzleStatusTable::MarkRelatedItems(const NozzleOption& option)
{
    m_table->MarkRelatedItems(option);

    m_badge->MarkRelatedItems(option);
}

void MultiNozzleStatusTable::UnMarkRelatedItems(const NozzleOption& option)
{
    m_table->UnMarkRelatedItems(option);

    m_badge->UnMarkRelatedItems(option);
}

void MultiNozzleStatusTable::UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack)
{
    if (m_table)
        m_table->UpdateRackInfo(rack);
    if (m_badge) {
        auto nozzle_rack = rack.lock();
        if (!nozzle_rack)
            return;
        auto nozzles_in_extruder = nozzle_rack->GetNozzleSystem()->GetExtNozzles();
        bool has_right = false;
        for (auto& elem : nozzles_in_extruder) {
            auto& nozzle = elem.second;
            int extruder_id = nozzle.AtLeftExtruder() ? 0 : 1;
            if (nozzle.AtRightExtruder())
                has_right = true;

            NozzleVolumeType volume_type = DevNozzle::ToNozzleVolumeType(nozzle.m_nozzle_flow);

            m_badge->SetExtruderInfo(extruder_id, format_diameter_to_str(nozzle.GetNozzleDiameter()), volume_type);
        }
        m_badge->SetExtruderValid(has_right);
    }
}


Slic3r::GUI::MultiNozzleSyncDialog::MultiNozzleSyncDialog(wxWindow* parent,std::weak_ptr<DevNozzleRack> rack) : DPIDialog(parent, wxID_ANY, _L("Sync Nozzle status"),wxDefaultPosition, wxDefaultSize,wxDEFAULT_DIALOG_STYLE)
{
    m_nozzle_rack = rack;
    wxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(*wxWHITE);

    m_tips = new Label(this, "");
    wxBoxSizer* label_sizer = new wxBoxSizer(wxHORIZONTAL);
    label_sizer->Add(m_tips, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(label_sizer, 0, wxTOP | wxBOTTOM, FromDIP(15));

    m_list_table = new NozzleListTable(this);

    m_list_table->Bind(EVT_NOZZLE_SELECTED, [this](wxCommandEvent& evt) {
        int idx = evt.GetInt();
        this->OnSelectRadio(idx);
        });

    main_sizer->Add(m_list_table, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(25));

    m_nozzle_table = new MultiNozzleStatusTable(this);
    wxBoxSizer* table_sizer = new wxBoxSizer(wxHORIZONTAL);
    table_sizer->Add(m_nozzle_table, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(table_sizer, 0, wxTOP | wxBOTTOM, FromDIP(15));

    wxBoxSizer* button_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_cancel_btn = new Button(this, _L("Cancel"), "", 0, 0, wxID_OK);
    m_confirm_btn = new Button(this, _L("Confirm"), "", 0, 0, wxID_CANCEL);

    m_caution = new Label(this, _L("Caution: Mixing nozzle diameters in one print is not supported. If the selected size is only on one extruder, single-extruder printing will be enforced."));
    m_caution->SetForegroundColour("#909090");
    main_sizer->Add(m_caution, 0, wxLEFT | wxRIGHT, FromDIP(25));

    StateColor btn_bg_green(
        std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 174, 66), StateColor::Normal)
    );

    StateColor btn_text_green(
        std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal)
    );


    m_confirm_btn->SetBackgroundColor(btn_bg_green);
    m_confirm_btn->SetMinSize(wxSize(FromDIP(55), FromDIP(24)));
    m_confirm_btn->SetCornerRadius(FromDIP(12));
    m_confirm_btn->SetBackgroundColor(btn_bg_green);
    m_confirm_btn->SetTextColor(btn_text_green);
    m_confirm_btn->SetFocus();

    m_cancel_btn->SetMinSize(wxSize(FromDIP(55), FromDIP(24)));
    m_cancel_btn->SetCornerRadius(FromDIP(12));

    button_sizer->AddStretchSpacer();
    button_sizer->Add(m_cancel_btn, 0, wxALL, FromDIP(10));
    button_sizer->Add(m_confirm_btn, 0, wxALL, FromDIP(10));

    main_sizer->Add(button_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    SetSizer(main_sizer);

    //main_sizer->SetSizeHints(this);
    main_sizer->Fit(this);

    int table_width = m_nozzle_table->GetBestSize().GetWidth();
    m_tips->Wrap(table_width);
    m_tips->SetMaxSize(wxSize(table_width, -1));

    m_caution->Wrap(table_width);
    m_caution->SetMaxSize(wxSize(table_width, -1));

    Layout();

    m_refresh_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &MultiNozzleSyncDialog::OnRefreshTimer, this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}


void MultiNozzleSyncDialog::OnRackStatusReadingFinished(wxEvent& evt) {
    if (!IsShown()) return;

    m_refreshing = false;
    if (m_refresh_timer)
        m_refresh_timer->Stop();
#if 1
    if (!UpdateUi(m_nozzle_rack))
        EndModal(wxID_OK);
#else
    if(!UpdateUoi(m_nozzle_rack)){
        CallAfter([this]() {
            EndModal(wxID_OK);
            });
    }
#endif
}

void MultiNozzleSyncDialog::OnRefreshTimer(wxTimerEvent& evt){
    if(!m_refreshing)
        return;
    auto nozzle_rack = m_nozzle_rack.lock();
    if(!nozzle_rack)
        return;

    int reading_count = nozzle_rack->GetReadingCount();
    int reading_idx = nozzle_rack->GetReadingIdx();

    wxString tip = wxString::Format(_L("Refresh %d/%d..."), reading_idx, reading_count);
    m_confirm_btn->SetLabel(tip);
    m_confirm_btn->Fit();
    if (m_confirm_btn->GetParent())
        m_confirm_btn->GetParent()->Layout();

    m_confirm_btn->Disable();
    m_cancel_btn->Disable();
}

void MultiNozzleSyncDialog::UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    UpdateUi(rack);
}

void MultiNozzleSyncDialog::OnSelectRadio(int select_idx)
{
    if (m_nozzle_option_idx != -1)
        m_nozzle_table->UnMarkRelatedItems(m_nozzle_option_values[m_nozzle_option_idx]);
    m_nozzle_option_idx = select_idx;
    m_nozzle_table->MarkRelatedItems(m_nozzle_option_values[m_nozzle_option_idx]);
}

bool MultiNozzleSyncDialog::hasMultiDiameters(const std::vector<MultiNozzleUtils::NozzleGroupInfo>& group_infos)
{
    if (group_infos.empty())
        return false;
    return !std::all_of(group_infos.begin(), group_infos.end(), [val = group_infos.front()](auto& elem) {return val.diameter == elem.diameter; });
}


std::vector<NozzleOption> MultiNozzleSyncDialog::GetNozzleOptions(const std::vector<MultiNozzleUtils::NozzleGroupInfo>& nozzle_groups)
{
    std::vector<NozzleOption> options;

    std::set<std::string> diameters;
    std::multimap<std::string, MultiNozzleUtils::NozzleGroupInfo> groups_mapped_for_diameter;
    for (auto& nozzle_group : nozzle_groups) {
        groups_mapped_for_diameter.insert({ nozzle_group.diameter,nozzle_group });
    }

#if ENABLE_MIX_FLOW_PRINT
    for (auto it = groups_mapped_for_diameter.begin(); it != groups_mapped_for_diameter.end(); ) {
        NozzleOption option;
        const auto& diameter = it->first;
        auto range = groups_mapped_for_diameter.equal_range(diameter);

        option.diameter = diameter;
        for (auto val_it = range.first; val_it != range.second; ++val_it) {
            const auto& elem = val_it->second;
            option.extruder_nozzle_stats[elem.extruder_id][elem.volume_type] += elem.nozzle_count;
        }
        options.emplace_back(std::move(option));
        it = range.second;
    }
#else
    for (auto it = groups_mapped_for_diameter.begin(); it != groups_mapped_for_diameter.end(); ) {
        const auto& diameter = it->first;
        auto range = groups_mapped_for_diameter.equal_range(diameter);

        std::vector<std::pair<NozzleVolumeType, int>> left_nozzles;
        std::vector<std::pair<NozzleVolumeType, int>> right_nozzles;

        for (auto val_it = range.first; val_it != range.second; ++val_it) {
            const auto& elem = val_it->second;
            if (elem.extruder_id == 0)
                left_nozzles.emplace_back(elem.volume_type, elem.nozzle_count);
            else
                right_nozzles.emplace_back(elem.volume_type, elem.nozzle_count);
        }

        for (const auto& left_nozzle : left_nozzles.empty() ? std::vector<std::pair<NozzleVolumeType, int>>{{}} : left_nozzles) {
            for (const auto& right_nozzle : right_nozzles.empty() ? std::vector<std::pair<NozzleVolumeType, int>>{{}} : right_nozzles) {
                NozzleOption option;
                option.diameter = diameter;

                if (!left_nozzles.empty()) {
                    option.extruder_nozzle_stats[0] = { left_nozzle.first, left_nozzle.second };
                }

                if (!right_nozzles.empty()) {
                    option.extruder_nozzle_stats[1] = { right_nozzle.first, right_nozzle.second };
                }

                options.emplace_back(std::move(option));
            }
        }

        it = range.second;
    }
#endif
    return options;
}

bool MultiNozzleSyncDialog::UpdateOptionList(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable)
{
    const auto& nozzle_rack = rack.lock();
    if (!nozzle_rack)
        return true;
    bool has_unknown = nozzle_rack->HasUnknownNozzles() && !ignore_unknown;
    bool has_unreliable = nozzle_rack->HasUnreliableNozzles() && !ignore_unreliable;

    if (has_unknown || has_unreliable) {
        m_list_table->Hide();
        return true;
    }

    m_list_table->Show();

    m_nozzle_option_values.clear();

    auto options = GetNozzleOptions(nozzle_rack->GetNozzleSystem()->GetNozzleGroups());

    int recommend_idx = std::max_element(options.begin(), options.end(), [](const NozzleOption& opt1, const NozzleOption& opt2) {
        int count1 = 0, count2 = 0;
        for (auto elem : opt1.extruder_nozzle_stats) {
            for (auto& stats : elem.second)
                count1 += stats.second;
        }
        for (auto elem : opt2.extruder_nozzle_stats) {
            for (auto& stats : elem.second)
                count2 += stats.second;
        }
        return count1 < count2;
        }) - options.begin();

    m_nozzle_option_values = options;
    OnSelectRadio(recommend_idx);
    m_list_table->SetOptions(options,recommend_idx);

        if (!has_unknown && !has_unreliable && options.size() == 1) {
            return false;
        }
        return true;
}

void MultiNozzleSyncDialog::UpdateTip(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable)
{
    const auto& nozzle_rack = rack.lock();
    if (!nozzle_rack)
        return;

    bool has_unknown = nozzle_rack->HasUnknownNozzles() && !ignore_unknown;
    bool has_unreliable = nozzle_rack->HasUnreliableNozzles() && !ignore_unreliable;

    if (has_unknown && has_unreliable) {
        m_tips->SetLabel(_L("Unknown nozzle detected. Refresh to update info (unrefreshed nozzles will be excluded during slicing). Verify nozzle diameter & flow rate against displayed values."));
        m_caution->Hide();
    }
    else if (has_unknown) {
        m_tips->SetLabel(_L("Unknown nozzle detected. Refresh to update (unrefreshed nozzles will be skipped in slicing)."));
        m_caution->Hide();
    }
    else if (has_unreliable) {
        m_tips->SetLabel(_L("Please confirm whether the required nozzle diameter and flow rate match the currently displayed values."));
        m_caution->Hide();
    }
    else {
        m_tips->SetLabel(_L("Your printer has different nozzles installed. Please select a nozzle for this print."));
        m_caution->Show();
    }
}

int MultiNozzleSyncDialog::ShowModal()
{
    bool res = UpdateUi(m_nozzle_rack);
    if (!res)
        return wxID_OK;

    return DPIDialog::ShowModal();

}

MultiNozzleSyncDialog::~MultiNozzleSyncDialog()
{
    if (auto rack = m_nozzle_rack.lock()) {
        rack->Unbind(DEV_RACK_EVENT_READING_FINISHED, &MultiNozzleSyncDialog::OnRackStatusReadingFinished, this);
    }
    if (m_refresh_timer)
        m_refresh_timer->Stop();
}

void MultiNozzleSyncDialog::UpdateButton(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable)
{
    const auto& nozzle_rack = rack.lock();
    if (!nozzle_rack)
        return;

    if (m_refreshing)
        return;

    bool has_unknown = nozzle_rack->HasUnknownNozzles() && !ignore_unknown;
    bool has_unreliable = nozzle_rack->HasUnreliableNozzles() && !ignore_unreliable;

    auto refresh_cmd = [rack, this]() {
        auto nozzle_rack = m_nozzle_rack.lock();
        if (!nozzle_rack)
            return;
        nozzle_rack->CtrlRackReadAll();
        m_refreshing = true;
        if (m_refresh_timer)
            m_refresh_timer->Start(500);
        nozzle_rack->Bind(DEV_RACK_EVENT_READING_FINISHED, &MultiNozzleSyncDialog::OnRackStatusReadingFinished, this);
        };

    auto trust_cmd = [rack, this]() {
        auto nozzle_rack = rack.lock();
        if (!nozzle_rack)
            return;
        nozzle_rack->CtrlRackConfirmAll();
        UpdateUi(rack, true, false);
    };

    auto ignore_opt = [rack,this]() {
        if (!UpdateUi(rack, true, true))
            EndModal(wxID_OK);
        };

    m_cancel_btn->Enable();
    m_confirm_btn->Enable();

    if (has_unknown && has_unreliable) {
        m_cancel_btn->Show();
        m_confirm_btn->Show();

        m_cancel_btn->SetLabel(_L("Ignore"));
        m_confirm_btn->SetLabel(_L("Refresh"));

        m_cancel_btn->Bind(wxEVT_LEFT_DOWN, [this, rack, ignore_opt](auto& e) {ignore_opt(); });
        m_confirm_btn->Bind(wxEVT_LEFT_DOWN, [this, rack, refresh_cmd](auto& e) {refresh_cmd(); });
    }
    else if (has_unknown) {
        m_cancel_btn->Show();
        m_confirm_btn->Show();

        m_cancel_btn->SetLabel(_L("Ignore"));
        m_confirm_btn->SetLabel(_L("Refresh"));

        m_cancel_btn->Bind(wxEVT_LEFT_DOWN, [this, rack, ignore_opt](auto& e) {ignore_opt(); });
        m_confirm_btn->Bind(wxEVT_LEFT_DOWN, [this, rack, refresh_cmd](auto& e) {refresh_cmd(); });
    }
    else if (has_unreliable) {
        m_cancel_btn->Show();
        m_confirm_btn->Show();

        m_cancel_btn->SetLabel(_L("Refresh"));
        m_confirm_btn->SetLabel(_L("Confirm"));

        m_cancel_btn->Bind(wxEVT_LEFT_DOWN, [this, rack, refresh_cmd](auto& e) {refresh_cmd(); });
        m_confirm_btn->Bind(wxEVT_LEFT_DOWN, [this, rack, trust_cmd](auto& e) {trust_cmd(); });

    }
    else {
        m_cancel_btn->Hide();
        m_confirm_btn->Show();
        m_confirm_btn->SetLabel(_L("OK"));
        m_confirm_btn->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {EndModal(wxID_OK); });
    }

}

bool MultiNozzleSyncDialog::UpdateUi(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable)
{
    m_nozzle_table->UpdateRackInfo(rack);
    bool res = UpdateOptionList(rack, ignore_unknown, ignore_unreliable);
    if (!res)
        return false;

    UpdateTip(rack, ignore_unknown, ignore_unreliable);
    UpdateButton(rack, ignore_unknown, ignore_unreliable);

    Layout();

    int table_width = m_nozzle_table->GetBestSize().GetWidth();
    m_tips->Wrap(table_width);
    m_tips->SetMaxSize(wxSize(table_width, -1));
    m_caution->Wrap(table_width);
    m_caution->SetMaxSize(wxSize(table_width, -1));
    Fit();
    return true;
}


std::optional<NozzleOption> tryPopUpMultiNozzleDialog(MachineObject* obj)
{
    if (!obj)
        return std::nullopt;
    auto rack = obj->GetNozzleSystem()->GetNozzleRack();
    if (!rack || !rack->IsSupported())
        return std::nullopt;
    MultiNozzleSyncDialog dialog(wxGetApp().plater_,rack);

    bool has_unreliable = rack->HasUnreliableNozzles();
    bool has_unknown = rack->HasUnknownNozzles();

    auto config = wxGetApp().preset_bundle->printers.get_edited_preset().config;

    if (dialog.ShowModal() == wxID_OK) {
        auto selected_option = dialog.GetSelectedOption();
        if (!selected_option)
            return std::nullopt;
        auto& preset_bundle = GUI::wxGetApp().preset_bundle;
        auto& project_config = preset_bundle->project_config;

        ConfigOptionEnumsGeneric* nozzle_volume_type_opt = project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");

        // write to preset bundle config
        for (int extruder_id = 0; extruder_id < 2; ++extruder_id) {
            NozzleVolumeType volume_type;
            int nozzle_count;
            bool clear_all = true;
            if (!selected_option->extruder_nozzle_stats.count(extruder_id)) {
                nozzle_count = 0;
                for(size_t idx = 0 ; idx < nvtHybrid; ++idx){
                    volume_type = static_cast<NozzleVolumeType>(idx);
                    setExtruderNozzleCount(preset_bundle, extruder_id, volume_type, nozzle_count, clear_all);
                    clear_all = false;
                }
            }
            else {
                for (auto& stat : selected_option->extruder_nozzle_stats[extruder_id]) {
                    volume_type = stat.first;
                    nozzle_count = stat.second;
                    setExtruderNozzleCount(preset_bundle, extruder_id, volume_type, nozzle_count, clear_all);
                    clear_all = false;
                }
            }
        }
        preset_bundle->extruder_nozzle_stat.set_nozzle_data_flag(ExtruderNozzleStat::ndfMachine);
        return selected_option;
    }

    return std::nullopt;
}

void setExtruderNozzleCount(PresetBundle* preset_bundle, int extruder_id, NozzleVolumeType volume_type, int nozzle_count, bool clear_all)
{
    if (!preset_bundle)
        return;
    if (volume_type == NozzleVolumeType::nvtHybrid)
        return;

    preset_bundle->extruder_nozzle_stat.set_extruder_nozzle_count(extruder_id, volume_type, nozzle_count, clear_all);
}

void updateNozzleCountDisplay(PresetBundle* preset_bundle, int extruder_id, NozzleVolumeType volume_type)
{
    int nozzle_count = preset_bundle->extruder_nozzle_stat.get_extruder_nozzle_count(extruder_id, volume_type);

    auto nozzle_count_opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    bool support_multi_nozzle = std::any_of(nozzle_count_opt->values.begin(), nozzle_count_opt->values.end(), [](int val) {return val > 1; });

    int display_count = support_multi_nozzle ? nozzle_count : -1;
    wxGetApp().plater()->sidebar().set_extruder_nozzle_count(extruder_id, display_count);

}

}