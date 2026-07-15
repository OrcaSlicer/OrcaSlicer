#include "FilamentMapPanel.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/MultiNozzleSync.hpp" // manuallySetNozzleCount producer for extruder_nozzle_stats
#include <algorithm>
#include <wx/dcbuffer.h>
#include <wx/utils.h>
#include "wx/graphics.h"

namespace Slic3r { namespace GUI {

static const wxColour BgNormalColor  = wxColour("#FFFFFF");
static const wxColour BgSelectColor  = wxColour("#E5F0EE"); // ORCA
static const wxColour BgDisableColor = wxColour("#CECECE");

static const wxColour BorderNormalColor   = wxColour("#CECECE");
static const wxColour BorderSelectedColor = wxColour("#009688");
static const wxColour BorderDisableColor  = wxColour("#EEEEEE");

static const wxColour TextNormalBlackColor = wxColour("#262E30");
static const wxColour TextNormalGreyColor = wxColour("#6B6B6B");
static const wxColour TextDisableColor = wxColour("#CECECE");
static const wxColour TextErrorColor = wxColour("#E14747");

wxDEFINE_EVENT(wxEVT_INVALID_MANUAL_MAP, wxCommandEvent);

// Orca: whether the edited printer has an extruder that can physically carry several nozzles
// (only such extruders track a per-volume-type nozzle inventory worth validating against).
static bool printer_has_multi_nozzle_extruder()
{
    auto *max_nozzle_counts_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    // Skip nil entries: a nullable-int nil is INT_MAX (> 1) and would otherwise falsely pass the gate.
    return max_nozzle_counts_opt &&
           std::any_of(max_nozzle_counts_opt->values.begin(), max_nozzle_counts_opt->values.end(),
                       [](int v) { return v > 1 && v != ConfigOptionIntsNullable::nil_value(); });
}

void FilamentMapManualPanel::OnTimer(wxTimerEvent &)
{
    bool             valid          = true;
    int              invalid_eid    = -1;
    NozzleVolumeType invalid_nozzle = NozzleVolumeType::nvtStandard;
    auto             preset_bundle  = wxGetApp().preset_bundle;
    auto             proj_config    = preset_bundle->project_config;
    auto             nozzle_volume_values = proj_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;
    std::vector<int> filament_map        = GetFilamentMaps();
    std::vector<int> filament_volume_map = GetFilamentVolumeMaps();
    // Orca: only multi-nozzle extruders carry a meaningful nozzle inventory; validating a plain
    // dual-extruder printer against it would flag every grouping whenever the stats are stale.
    if (printer_has_multi_nozzle_extruder()) {
        for (size_t eid = 0; eid < nozzle_volume_values.size(); ++eid) {
            NozzleVolumeType extruder_volume_type = NozzleVolumeType(nozzle_volume_values[eid]);
            bool             extruder_used        = std::find_if(m_filament_list.begin(), m_filament_list.end(),
                                                                 [&filament_map, eid](int fid) {
                                                        return fid - 1 < (int) filament_map.size() && (filament_map[fid - 1] - 1) == (int) eid;
                                                    }) != m_filament_list.end();

            if (!extruder_used) {
                continue;
            }

            if (extruder_volume_type == nvtHybrid) {
                int standard_count = getExtruderNozzleCount(preset_bundle, eid, NozzleVolumeType::nvtStandard);
                int highflow_count = getExtruderNozzleCount(preset_bundle, eid, NozzleVolumeType::nvtHighFlow);

                auto has_material_of_type = [this, eid, &filament_map, &filament_volume_map](NozzleVolumeType volume_type) {
                    return std::find_if(m_filament_list.begin(), m_filament_list.end(),
                                        [eid, &filament_map, &filament_volume_map, volume_type](int fid) {
                                            return fid - 1 < (int) filament_map.size() && (filament_map[fid - 1] - 1) == (int) eid &&
                                                   fid - 1 < (int) filament_volume_map.size() &&
                                                   filament_volume_map[fid - 1] == static_cast<int>(volume_type);
                                        }) != m_filament_list.end();
                };

                bool has_standard = has_material_of_type(NozzleVolumeType::nvtStandard);
                bool has_highflow = has_material_of_type(NozzleVolumeType::nvtHighFlow);

                if ((has_standard && standard_count == 0) ||
                    (has_highflow && highflow_count == 0)) {
                    valid          = false;
                    invalid_eid    = eid;
                    invalid_nozzle = (has_standard && standard_count == 0) ? NozzleVolumeType::nvtStandard : NozzleVolumeType::nvtHighFlow;
                    break;
                }
            } else {
                int count = getExtruderNozzleCount(preset_bundle, eid, extruder_volume_type);
                if (count == 0) {
                    valid          = false;
                    invalid_eid    = eid;
                    invalid_nozzle = extruder_volume_type;
                    break;
                }
            }
        }
    }

    bool update_ui  = m_invalid_id != invalid_eid;
    bool send_event = update_ui || m_force_validation;

    m_invalid_id = invalid_eid;

    if (update_ui) {
        if (valid) {
            m_errors->Hide();
            m_suggestion_panel->Hide();
        } else {
            m_errors->SetLabel(wxString::Format(_L("Error: %s extruder has no available %s nozzle, current group result is invalid."),
                                                invalid_eid == 0 ? _L("Left") : _L("Right"),
                                                invalid_nozzle == NozzleVolumeType::nvtStandard ? _L("Standard") : _L("High Flow")));
            // Re-wrap: wrapping only applies to the label text present when Wrap is called,
            // and the label was empty at construction time.
            m_errors->Wrap(FromDIP(520));
            m_errors->Show();
            m_suggestion_panel->Show();
        }
        m_left_panel->Freeze();
        m_right_panel->Freeze();
        m_tips->Freeze();
        m_description->Freeze();
        Layout();
        Fit();
        this->GetParent()->Layout();
        this->GetParent()->Fit();
        m_left_panel->Thaw();
        m_right_panel->Thaw();
        m_tips->Thaw();
        m_description->Thaw();
    }

    if (send_event) {
        wxCommandEvent event(wxEVT_INVALID_MANUAL_MAP);
        event.SetInt(valid);
        ProcessEvent(event);
        m_force_validation = false;
    }
}

void FilamentMapManualPanel::OnSuggestionClicked(wxCommandEvent &event)
{
    wxWindow *current = this;
    while (current && !wxDynamicCast(current, wxDialog)) {
        current = current->GetParent();
    }

    if (current) {
        wxDialog *dlg = wxDynamicCast(current, wxDialog);
        if (dlg) {
            int invalid_eid = m_invalid_id;
            dlg->EndModal(wxID_CANCEL);

            if (invalid_eid >= 0) {
                manuallySetNozzleCount(invalid_eid);
            }
            wxGetApp().plater()->update();
        }
    }
}

std::vector<int> FilamentMapManualPanel::GetFilamentMaps() const
{
    std::vector<int> new_filament_map = m_filament_map;
    std::vector<int> left_filaments   = this->GetLeftFilaments();
    std::vector<int> right_filaments  = this->GetRightFilaments();

    for (int i = 0; i < (int) new_filament_map.size(); ++i) {
        if (std::find(left_filaments.begin(), left_filaments.end(), i + 1) != left_filaments.end()) {
            new_filament_map[i] = 1;
        } else if (std::find(right_filaments.begin(), right_filaments.end(), i + 1) != right_filaments.end()) {
            new_filament_map[i] = 2;
        }
    }
    return new_filament_map;
}

std::vector<int> FilamentMapManualPanel::GetFilamentVolumeMaps() const
{
    std::vector<int> volume_map(m_filament_map.size(), 0);

    std::vector<int> left_filaments                = this->GetLeftFilaments();
    std::vector<int> right_high_flow_filaments     = this->GetRightHighFlowFilaments();
    std::vector<int> right_standard_filaments      = this->GetRightStandardFilaments();
    std::vector<int> right_tpu_high_flow_filaments = this->GetRightTPUHighFlowFilaments();

    auto preset_bundle        = wxGetApp().preset_bundle;
    auto proj_config          = preset_bundle->project_config;
    auto nozzle_volume_values = proj_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;

    for (int i = 0; i < (int) volume_map.size(); ++i) {
        int filament_id = i + 1;

        if (std::find(left_filaments.begin(), left_filaments.end(), filament_id) != left_filaments.end()) {
            if (nozzle_volume_values.size() > 0) {
                // Orca: never emit the Hybrid marker as a per-filament value; on a Hybrid
                // extruder each filament prints with a concrete flow, defaulting to Standard.
                volume_map[i] = nozzle_volume_values[0] == static_cast<int>(NozzleVolumeType::nvtHybrid) ?
                                    static_cast<int>(NozzleVolumeType::nvtStandard) :
                                    nozzle_volume_values[0];
            }
        }
        else if (std::find(right_high_flow_filaments.begin(), right_high_flow_filaments.end(), filament_id) != right_high_flow_filaments.end()) {
            volume_map[i] = static_cast<int>(NozzleVolumeType::nvtHighFlow);
        }
        else if (std::find(right_standard_filaments.begin(), right_standard_filaments.end(), filament_id) != right_standard_filaments.end()) {
            volume_map[i] = static_cast<int>(NozzleVolumeType::nvtStandard);
        }
        else if (std::find(right_tpu_high_flow_filaments.begin(), right_tpu_high_flow_filaments.end(), filament_id) != right_tpu_high_flow_filaments.end()) {
            volume_map[i] = static_cast<int>(NozzleVolumeType::nvtTPUHighFlow);
        }
    }

    return volume_map;
}

void FilamentMapManualPanel::SyncPanelHeights()
{
    if (!m_left_panel || !m_right_panel) return;

    auto curr_left  = m_left_panel->GetMinSize();
    auto curr_right = m_right_panel->GetMinSize();

    m_left_panel->SetMinSize(wxSize(FromDIP(260), FromDIP(110)));
    m_right_panel->SetMinSize(wxSize(FromDIP(260), FromDIP(110)));

    m_left_panel->Layout();
    m_left_panel->Fit();
    m_right_panel->Layout();
    m_right_panel->Fit();

    wxSize left_best_size  = m_left_panel->GetBestSize();
    wxSize right_best_size = m_right_panel->GetBestSize();

    int  max_height     = std::max(left_best_size.GetHeight(), right_best_size.GetHeight());
    bool height_changed = curr_left.GetHeight() != max_height || curr_right.GetHeight() != max_height;
    if (!height_changed) {
        if (curr_left.GetHeight() > 0)
            m_left_panel->SetMinSize(curr_left);
        if (curr_right.GetHeight() > 0)
            m_right_panel->SetMinSize(curr_right);
        if (GetParent()) {
            GetParent()->Layout();
            GetParent()->Fit();
        }
        return;
    }

    m_left_panel->SetMinSize(wxSize(FromDIP(260), max_height));
    m_right_panel->SetMinSize(wxSize(FromDIP(260), max_height));

    Layout();
    Fit();

    if (GetParent()) {
        GetParent()->Layout();
        GetParent()->Fit();
    }
}

void FilamentMapManualPanel::OnDragDropCompleted(wxCommandEvent &event)
{
    SyncPanelHeights();
    event.Skip();
}

FilamentMapManualPanel::FilamentMapManualPanel(wxWindow                       *parent,
                                               const std::vector<std::string> &color,
                                               const std::vector<std::string> &type,
                                               const std::vector<int>         &filament_list,
                                               const std::vector<int>         &filament_map,
                                               const std::vector<int>         &filament_volume_map)
    : wxPanel(parent)
    , m_filament_map(filament_map)
    , m_filament_volume_map(filament_volume_map)
    , m_filament_list(filament_list)
    , m_filament_color(color)
    , m_filament_type(type)
{
    SetName(wxT("FilamentMapManualPanel"));
    SetBackgroundColour(BgNormalColor);

    auto top_sizer = new wxBoxSizer(wxVERTICAL);

    m_description = new Label(this, _L("We will slice according to this grouping method:"));
    top_sizer->Add(m_description, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));
    m_description->Wrap(FromDIP(520));
    top_sizer->AddSpacer(FromDIP(8));

    auto drag_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_left_panel  = new DragDropPanel(this, _L("Left Nozzle"), false);
    m_right_panel = new SeparatedDragDropPanel(this, _L("Right Nozzle"), false);
    m_switch_btn  = new ScalableButton(this, wxID_ANY, "switch_filament_maps");

    UpdateNozzleVolumeType();

    for (size_t idx = 0; idx < m_filament_map.size(); ++idx) {
        auto iter = std::find(m_filament_list.begin(), m_filament_list.end(), idx + 1);
        if (iter == m_filament_list.end()) continue;
        wxColor color = Hex2Color(m_filament_color[idx]);
        std::string type = m_filament_type[idx];
        if (m_filament_map[idx] == 1) {
            m_left_panel->AddColorBlock(color, type, idx + 1);
        } else {
            assert(m_filament_map[idx] == 2);
            bool is_high_flow = (idx < m_filament_volume_map.size()) && (m_filament_volume_map[idx] == static_cast<int>(NozzleVolumeType::nvtHighFlow));
            m_right_panel->AddColorBlock(color, type, idx + 1, is_high_flow);
        }
    }
    m_left_panel->SetMinSize({FromDIP(260), FromDIP(110)});
    m_right_panel->SetMinSize({FromDIP(260), FromDIP(110)});

    drag_sizer->Add(m_left_panel, 1, wxEXPAND);
    drag_sizer->Add(m_switch_btn, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(8));
    drag_sizer->Add(m_right_panel, 1, wxEXPAND);

    top_sizer->Add(drag_sizer, 0, wxEXPAND);

    m_tips = new Label(this, _L("Tip: You can drag the filaments to reassign them to different nozzles."));
    m_tips->SetFont(Label::Body_14);
    m_tips->SetForegroundColour(TextNormalGreyColor);
    m_tips->Wrap(FromDIP(520));
    top_sizer->AddSpacer(FromDIP(20));
    top_sizer->Add(m_tips, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));

    m_errors = new Label(this, "");
    m_errors->SetFont(Label::Body_13);
    m_errors->SetForegroundColour(TextErrorColor);
    m_errors->Wrap(FromDIP(520));
    top_sizer->AddSpacer(FromDIP(10));
    top_sizer->Add(m_errors, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));

    m_errors->Hide();

    m_suggestion_panel = new wxPanel(this, wxID_ANY);
    m_suggestion_panel->SetBackgroundColour(*wxWHITE);
    auto suggestion_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto suggestion_text  = new Label(m_suggestion_panel, _L("Please adjust your grouping or click "));
    suggestion_text->SetFont(Label::Body_13);
    suggestion_text->SetForegroundColour(TextErrorColor);
    suggestion_text->SetBackgroundColour(*wxWHITE);
    auto suggestion_btn   = new ScalableButton(m_suggestion_panel, wxID_ANY, "edit", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true, 14);
    suggestion_btn->SetBackgroundColour(*wxWHITE);
    auto suggestion_text2 = new Label(m_suggestion_panel, _L(" to set nozzle count"));
    suggestion_text2->SetFont(Label::Body_13);
    suggestion_text2->SetForegroundColour(TextErrorColor);
    suggestion_text2->SetBackgroundColour(*wxWHITE);
    suggestion_sizer->Add(suggestion_text, 0, wxALIGN_CENTER_VERTICAL);
    suggestion_sizer->Add(suggestion_btn, 0, wxALIGN_CENTER_VERTICAL);
    suggestion_sizer->Add(suggestion_text2, 0, wxALIGN_CENTER_VERTICAL);
    m_suggestion_panel->SetSizer(suggestion_sizer);
    top_sizer->Add(m_suggestion_panel, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));
    m_suggestion_panel->Hide();
    suggestion_btn->Bind(wxEVT_BUTTON, &FilamentMapManualPanel::OnSuggestionClicked, this);

    // Multi-nozzle: give the user a reachable way to declare, per extruder, how many
    // physical nozzles of each volume type a multi-nozzle extruder carries. This is the
    // fallback "manual" producer of the extruder_nozzle_stats config (the full device nozzle-rack
    // auto-sync is deferred). Gated on the edited printer preset having an extruder with
    // extruder_max_nozzle_count > 1, so the trigger is not even created for any single-nozzle or
    // dual-extruder ({1,1}, H2D) printer - zero UI change for every existing profile.
    if (printer_has_multi_nozzle_extruder()) {
        auto *max_nozzle_counts_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
        auto *set_count_link = new Label(this, _L("Set the physical nozzle count..."));
        set_count_link->SetFont(Label::Body_14);
        set_count_link->SetForegroundColour(BorderSelectedColor);
        set_count_link->SetCursor(wxCursor(wxCURSOR_HAND));
        top_sizer->AddSpacer(FromDIP(8));
        top_sizer->Add(set_count_link, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));
        const std::vector<int> max_counts = max_nozzle_counts_opt->values;
        set_count_link->Bind(wxEVT_LEFT_DOWN, [max_counts](wxMouseEvent &evt) {
            for (int extruder_id = 0; extruder_id < (int) max_counts.size(); ++extruder_id) {
                if (max_counts[extruder_id] > 1 && max_counts[extruder_id] != ConfigOptionIntsNullable::nil_value())
                    GUI::manuallySetNozzleCount(extruder_id);
            }
            evt.Skip();
        });
    }

    m_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &FilamentMapManualPanel::OnTimer, this);
    Bind(wxEVT_DRAG_DROP_COMPLETED, &FilamentMapManualPanel::OnDragDropCompleted, this);

    m_switch_btn->Bind(wxEVT_BUTTON, &FilamentMapManualPanel::OnSwitchFilament, this);

    SetSizer(top_sizer);
    SetMinSize(wxSize(FromDIP(580), -1));
    Layout();
    Fit();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapManualPanel::UpdateNozzleVolumeType()
{
    auto check_separation = []() {
        auto preset_bundle        = wxGetApp().preset_bundle;
        auto nozzle_volume_values = preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;
        if (nozzle_volume_values.size() <= 1)
            return false;

        return nozzle_volume_values[1] == static_cast<int>(NozzleVolumeType::nvtHybrid);
    };
    bool should_separate = check_separation();
    m_right_panel->SetUseSeparation(should_separate);

    UpdateNozzleCountDisplay();

    Layout();
    Fit();
}

void FilamentMapManualPanel::UpdateNozzleCountDisplay()
{
    auto preset_bundle = wxGetApp().preset_bundle;

    // Orca: nozzle counts are only tracked (and only meaningful) for multi-nozzle extruders;
    // plain dual-extruder printers keep their unadorned zone titles.
    if (!printer_has_multi_nozzle_extruder()) {
        m_left_panel->UpdateLabel(_L("Left Nozzle"));
        m_right_panel->UpdateLabel(_L("Right Nozzle"));
        return;
    }

    // Format the count suffix separately so a translation containing '%' cannot
    // corrupt the wxString::Format output.
    int      left_count = getExtruderNozzleCountTotal(preset_bundle, 0);
    wxString left_title = _L("Left Nozzle") + wxString::Format("(%d)", left_count);
    m_left_panel->UpdateLabel(left_title);

    if (m_right_panel->IsUseSeparation()) {
        int      standard_count = getExtruderNozzleCount(preset_bundle, 1, NozzleVolumeType::nvtStandard);
        int      highflow_count = getExtruderNozzleCount(preset_bundle, 1, NozzleVolumeType::nvtHighFlow);
        wxString right_title    = _L("Right Nozzle") + wxString::Format("(Std: %d, HF: %d)", standard_count, highflow_count);
        m_right_panel->UpdateLabel(right_title);
    } else {
        int      right_count = getExtruderNozzleCountTotal(preset_bundle, 1);
        wxString right_title = _L("Right Nozzle") + wxString::Format("(%d)", right_count);
        m_right_panel->UpdateLabel(right_title);
    }
}

FilamentMapManualPanel::~FilamentMapManualPanel()
{
    m_timer->Stop();
    delete m_timer;
}

void FilamentMapManualPanel::OnSwitchFilament(wxCommandEvent &)
{
    auto left_blocks  = m_left_panel->get_filament_blocks();
    auto right_blocks = m_right_panel->get_filament_blocks();

    for (auto &block : left_blocks) {
        m_right_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false, false);
        m_left_panel->RemoveColorBlock(block, false);
    }

    for (auto &block : right_blocks) {
        m_left_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
        m_right_panel->RemoveColorBlock(block, false);
    }
    this->GetParent()->Layout();
    this->GetParent()->Fit();

    if (m_right_panel->IsUseSeparation()) {
        m_left_panel->Layout();
        m_left_panel->Fit();
        m_right_panel->Layout();
        m_right_panel->Fit();
        SyncPanelHeights();
    }
}

bool FilamentMapManualPanel::Show(bool show)
{
    m_force_validation = show;
    if (show) {
        m_timer->Start(500);
        SyncPanelHeights();
    } else {
        m_timer->Stop();
    }

    return wxPanel::Show(show);
}

GUI::FilamentMapBtnPanel::FilamentMapBtnPanel(wxWindow *parent, const wxString &label, const wxString &detail, const std::string &icon) : wxPanel(parent)
{
    SetBackgroundColour(*wxWHITE);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_hover = false;

    const int horizontal_margin = FromDIP(12);

    auto sizer = new wxBoxSizer(wxVERTICAL);

    icon_enabled = create_scaled_bitmap(icon, nullptr, 20);
    icon_disabled = create_scaled_bitmap(icon + "_disabled", nullptr, 20);

    m_btn    = new wxBitmapButton(this, wxID_ANY, icon_enabled, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
    m_btn->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_label = new wxStaticText(this, wxID_ANY, label);
    m_label->SetFont(Label::Head_14);
    m_label->SetForegroundColour(TextNormalBlackColor);

    auto label_sizer = new wxBoxSizer(wxHORIZONTAL);
    label_sizer->AddStretchSpacer();
    label_sizer->Add(m_btn, 0, wxEXPAND | wxLEFT, FromDIP(1));
    label_sizer->Add(m_label, 0, wxEXPAND| wxALL, FromDIP(3));
    label_sizer->AddStretchSpacer();

    m_disable_tip = new Label(this, _L("(Sync with printer)"));

    sizer->AddSpacer(FromDIP(32));
    sizer->Add(label_sizer, 0, wxEXPAND);
    sizer->Add(m_disable_tip, 0, wxALIGN_CENTER);
    sizer->AddSpacer(FromDIP(3));

    auto detail_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_detail          = new Label(this, detail);
    m_detail->SetFont(Label::Body_12);
    m_detail->SetForegroundColour(TextNormalGreyColor);
    m_detail->Wrap(FromDIP(180));

    detail_sizer->AddStretchSpacer();
    detail_sizer->Add(m_detail, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, horizontal_margin);
    detail_sizer->AddStretchSpacer();

    sizer->Add(detail_sizer, 0, wxEXPAND);
    sizer->AddSpacer(FromDIP(10));

    SetSizer(sizer);
    Layout();
    Fit();

    GUI::wxGetApp().UpdateDarkUIWin(this);

    auto forward_click_to_parent = [this](wxMouseEvent &event) {
        wxCommandEvent click_event(wxEVT_LEFT_DOWN, GetId());
        click_event.SetEventObject(this);
        this->ProcessEvent(click_event);
    };

    m_btn->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);
    m_label->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);
    m_detail->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);

    Bind(wxEVT_PAINT, &FilamentMapBtnPanel::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &FilamentMapBtnPanel::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &FilamentMapBtnPanel::OnLeaveWindow, this);
}

void FilamentMapBtnPanel::OnPaint(wxPaintEvent &event)
{
    wxAutoBufferedPaintDC dc(this);
    wxGraphicsContext    *gc = wxGraphicsContext::Create(dc);

    if (gc) {
        dc.Clear();
        wxRect rect = GetClientRect();
        gc->SetBrush(wxTransparentColour);
        gc->DrawRoundedRectangle(0, 0, rect.width, rect.height, 0);
        wxColour bg_color = m_selected ? BgSelectColor : BgNormalColor;

        wxColour border_color = m_hover || m_selected ? BorderSelectedColor : BorderNormalColor;

        bg_color     = StateColor::darkModeColorFor(bg_color);
        border_color = StateColor::darkModeColorFor(border_color);
        gc->SetBrush(wxBrush(bg_color));
        gc->SetPen(wxPen(border_color, 1));
        gc->DrawRoundedRectangle(1, 1, rect.width - 2, rect.height - 2, 8);
        delete gc;
    }
}

void FilamentMapBtnPanel::UpdateStatus()
{
    if (m_selected) {
        m_btn->SetBackgroundColour(BgSelectColor);
        m_label->SetBackgroundColour(BgSelectColor);
        m_detail->SetBackgroundColour(BgSelectColor);
        m_disable_tip->SetBackgroundColour(BgSelectColor);
    }
    else {
        m_btn->SetBackgroundColour(BgNormalColor);
        m_label->SetBackgroundColour(BgNormalColor);
        m_detail->SetBackgroundColour(BgNormalColor);
        m_disable_tip->SetBackgroundColour(BgNormalColor);
    }
    if (!m_enabled) {
        m_disable_tip->SetLabel(_L("(Sync with printer)"));
        m_disable_tip->SetForegroundColour(TextDisableColor);
        m_btn->SetBitmap(icon_disabled);
        m_btn->SetForegroundColour(BgDisableColor);
        m_label->SetForegroundColour(TextDisableColor);
        m_detail->SetForegroundColour(TextDisableColor);
    }
    else {
        m_disable_tip->SetLabel("");
        m_disable_tip->SetForegroundColour(TextNormalBlackColor);
        m_btn->SetBitmap(icon_enabled);
        m_btn->SetForegroundColour(BgNormalColor);
        m_label->SetForegroundColour(TextNormalBlackColor);
        m_detail->SetForegroundColour(TextNormalGreyColor);
    }
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapBtnPanel::OnEnterWindow(wxMouseEvent &event)
{
    if (!m_hover && m_enabled) {
        m_hover = true;
        UpdateStatus();
        Refresh();
        event.Skip();
    }
}

void FilamentMapBtnPanel::OnLeaveWindow(wxMouseEvent &event)
{
    if (m_hover) {
        wxPoint pos = this->ScreenToClient(wxGetMousePosition());
        if (this->GetClientRect().Contains(pos)) return;
        m_hover = false;
        UpdateStatus();
        Refresh();
        event.Skip();
    }
}

bool FilamentMapBtnPanel::Enable(bool enable)
{
    m_enabled = enable;
    UpdateStatus();
    Refresh();
    return true;
}

void FilamentMapBtnPanel::Select(bool selected)
{
    m_selected = selected;
    UpdateStatus();
    Refresh();
}

void GUI::FilamentMapBtnPanel::Hide()
{
    m_btn->Hide();
    m_label->Hide();
    m_detail->Hide();
    wxPanel::Hide();
}
void GUI::FilamentMapBtnPanel::Show()
{
    m_btn->Show();
    m_label->Show();
    m_detail->Show();
    wxPanel::Show();
}

FilamentMapAutoPanel::FilamentMapAutoPanel(wxWindow *parent, FilamentMapMode mode, bool machine_synced) : wxPanel(parent)
{
    const wxString AutoForFlushDetail = _L("Generates filament grouping for the left and right nozzles based on the most filament-saving principles to minimize waste.");

    const wxString AutoForMatchDetail = _L("Generates filament grouping for the left and right nozzles based on the printer's actual filament status, reducing the need for manual filament adjustment.");

    auto                  sizer              = new wxBoxSizer(wxHORIZONTAL);
    m_flush_panel                            = new FilamentMapBtnPanel(this, _L("Filament-Saving Mode"), AutoForFlushDetail, "flush_mode_panel_icon");
    m_match_panel                            = new FilamentMapBtnPanel(this, _L("Convenience Mode"), AutoForMatchDetail, "match_mode_panel_icon");

    if (!machine_synced) m_match_panel->Enable(false);

    sizer->AddStretchSpacer();
    sizer->Add(m_flush_panel, 1, wxEXPAND);
    sizer->AddSpacer(FromDIP(12));
    sizer->Add(m_match_panel, 1, wxEXPAND);
    sizer->AddStretchSpacer();

    m_flush_panel->Bind(wxEVT_LEFT_DOWN, [this](auto& event) {
        if (m_flush_panel->IsEnabled()) {
            this->OnModeSwitch(FilamentMapMode::fmmAutoForFlush);
        }
    });

    m_match_panel->Bind(wxEVT_LEFT_DOWN, [this](auto &event) {
        if (m_match_panel->IsEnabled()) {
            this->OnModeSwitch(FilamentMapMode::fmmAutoForMatch);
        }
    });

    m_mode = mode;
    UpdateStatus();

    SetSizerAndFit(sizer);
    Layout();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}
void FilamentMapAutoPanel::Hide()
{
    m_flush_panel->Hide();
    m_match_panel->Hide();
    wxPanel::Hide();
}

void FilamentMapAutoPanel::Show()
{
    m_flush_panel->Show();
    m_match_panel->Show();
    wxPanel::Show();
}

void FilamentMapAutoPanel::UpdateStatus()
{
    if (m_mode == fmmAutoForFlush) {
        m_flush_panel->Select(true);
        m_match_panel->Select(false);
    } else {
        m_flush_panel->Select(false);
        m_match_panel->Select(true);
    }
}

void FilamentMapAutoPanel::OnModeSwitch(FilamentMapMode mode)
{
    m_mode = mode;
    UpdateStatus();
}

FilamentMapDefaultPanel::FilamentMapDefaultPanel(wxWindow *parent) : wxPanel(parent)
{
    auto sizer = new wxBoxSizer(wxHORIZONTAL);

    m_label = new Label(this, _L("The filament grouping method for current plate is determined by the dropdown option at the slicing plate button."));
    m_label->SetFont(Label::Body_14);
    m_label->SetBackgroundColour(*wxWHITE);
    m_label->Wrap(FromDIP(500));

    sizer->AddStretchSpacer();
    sizer->Add(m_label, 1, wxEXPAND);
    sizer->AddStretchSpacer();

    SetSizerAndFit(sizer);
    Layout();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapDefaultPanel::Hide()
{
    m_label->Hide();
    wxPanel::Hide();
}

void FilamentMapDefaultPanel::Show()
{
    m_label->Show();
    wxPanel::Show();
}

}} // namespace Slic3r::GUI
