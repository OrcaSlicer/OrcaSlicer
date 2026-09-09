#include "SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/AI/Orca/OrcaModelPreparationPanel.hpp"

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wx/button.h>
#include <wx/collpane.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <sstream>
#include <type_traits>

namespace Slic3r::GUI {
namespace {

wxString summary_text(const std::string& key)
{
    if (key == "capturing_workspace")
        return _L("正在读取当前打印板与配置…");
    if (key == "inspecting_printability")
        return _L("正在执行可打印性检查…");
    if (key == "printability_action_required")
        return _L("发现需要处理的可打印性问题");
    if (key == "preflight_complete_with_warnings")
        return _L("检查完成，但仍有提示需要留意");
    if (key == "preflight_complete")
        return _L("检查完成，可以进入候选优化");
    if (key == "planning_candidates")
        return _L("正在生成确定性候选方案…");
    if (key == "trial_slicing_baseline")
        return _L("正在试切当前基线方案…");
    if (key == "trial_slicing_candidates")
        return _L("正在顺序试切候选方案…");
    if (key == "candidates_ready")
        return _L("试切完成，请比较并选择方案");
    if (key == "applying_candidate")
        return _L("正在事务式应用所选方案…");
    if (key == "official_slicing")
        return _L("方案已应用，正在执行正式切片…");
    if (key == "official_slice_complete")
        return _L("正式切片完成，已进入预览");
    if (key == "official_slice_failed")
        return _L("正式切片失败，可一键撤销本次应用");
    if (key == "canceling")
        return _L("正在取消…");
    if (key == "canceled")
        return _L("检查已取消");
    if (key == "workspace_changed")
        return _L("工程已变化，需要重新检查");
    if (key == "applied_revision_unavailable")
        return _L("无法核实结果对应的工程版本，请重新检查。撤销请使用 Orca 历史。");
    if (key == "apply_undo_unavailable")
        return _L("本次应用已无法单独撤销。请查看 Orca 撤销历史，或重新检查当前工程。");
    if (key == "apply_undo_failed")
        return _L("撤销未完成，请重试或查看 Orca 撤销历史。");
    if (key == "close_active_model_tool")
        return _L("请先结束当前模型编辑工具，再重新检查并应用方案。");
    if (key == "preflight_failed")
        return _L("检查失败，请重试");
    if (key == "interrupted_workflow_recovered")
        return _L("上次智能切片被中断，已安全清理临时候选；请重新检查");
    return _L("从当前打印板开始智能切片");
}

wxString status_text(SmartSlicingStageStatus status)
{
    switch (status) {
    case SmartSlicingStageStatus::Active: return _L("进行中");
    case SmartSlicingStageStatus::Complete: return _L("完成");
    case SmartSlicingStageStatus::NeedsAttention: return _L("需处理");
    case SmartSlicingStageStatus::Disabled: return _L("尚未开始");
    case SmartSlicingStageStatus::Waiting: return _L("等待");
    }
    return _L("等待");
}

wxString issue_name(const std::string& code)
{
    if (code == "empty_plate")
        return _L("当前打印板为空");
    if (code == "open_mesh")
        return _L("网格存在开放边");
    if (code == "outside_build_volume")
        return _L("对象超出打印空间");
    if (code == "missing_printer")
        return _L("未选择打印机");
    if (code == "missing_process")
        return _L("未选择工艺预设");
    if (code == "missing_material")
        return _L("未选择材料");
    if (code == "incompatible_physical_slots")
        return _L("物理槽位材料温区不兼容");
    if (code == "invalid_material_temperature_range")
        return _L("材料推荐温区无效");
    if (code == "color_mapping_degraded")
        return _L("颜色到物理槽位映射不完整");
    if (code == "multicolor_evidence_unavailable")
        return _L("多色兼容证据不可用");
    if (code == "native_validation_unavailable")
        return _L("当前原生配置校验尚不可用");
    if (code == "configuration_validation_error")
        return _L("配置校验错误");
    if (code == "configuration_validation_warning")
        return _L("配置校验警告");
    return _L("可打印性问题");
}

wxString format_duration(const std::optional<double>& seconds)
{
    if (!seconds)
        return _L("不可用");
    const long long total_minutes = static_cast<long long>(std::llround(*seconds / 60.0));
    return wxString::Format("%lldh %02lldm", total_minutes / 60, total_minutes % 60);
}

wxString format_volume(const std::optional<double>& volume_mm3)
{
    return volume_mm3 ? wxString::Format("%.2f cm³", *volume_mm3 / 1000.0) : _L("不可用");
}

wxString format_delta(const std::optional<double>& value, double scale, const wxString& unit)
{
    return value ? wxString::Format("%+.2f %s", *value / scale, unit.c_str()) : _L("—");
}

wxString candidate_reason(const SmartSlicingCandidateView& candidate)
{
    if (candidate.failed)
        return _L("试切失败，可单独重试；基线仍然可用。");
    if (candidate.id == "baseline")
        return _L("当前正式工作区的只读基线。");
    wxString reason = candidate.recommended ? _L("推荐方案。") : _L("可选方案。");
    for (const std::string& evidence : candidate.evidence_codes) {
        if (evidence == "fewer_slice_warnings")
            reason += _L(" 切片警告更少。");
        else if (evidence == "lower_estimated_time" || evidence == "shorter_print_time")
            reason += _L(" 预计时间更短。");
        else if (evidence == "lower_filament_volume" || evidence == "less_material")
            reason += _L(" 材料用量更低。");
        else if (evidence == "lower_support_volume" || evidence == "less_support_material")
            reason += _L(" 支撑用量更低。");
        else if (evidence == "less_total_material_including_multicolor_waste")
            reason += _L(" 包含冲刷和擦料塔在内的总材料更少。");
        else if (evidence == "fewer_tool_changes")
            reason += _L(" 换料次数更少。");
        else if (evidence == "lower_flush_volume")
            reason += _L(" 冲刷废料更少。");
        else if (evidence == "lower_wipe_tower_volume")
            reason += _L(" 擦料塔用料更少。");
    }
    return reason;
}

wxString candidate_changes(const SmartSlicingCandidateView& candidate)
{
    using namespace AI::SmartSlicing;
    auto value_text = [](const ConfigValue& value) {
        return std::visit([](const auto& item) -> wxString {
            if constexpr (std::is_same_v<std::decay_t<decltype(item)>, bool>)
                return item ? _L("开启") : _L("关闭");
            else {
                std::ostringstream text;
                text << item;
                return from_u8(text.str());
            }
        }, value);
    };
    wxString text;
    if (candidate.explanation == "small_or_slender_footprint_brim_candidate")
        text = _L("增加裙边宽度，改善小底面或细高模型的附着。\n");
    else if (candidate.placement_change_count > 0)
        text = _L("调整模型摆放；请结合试切时间与支撑用量选择。\n");
    for (const auto& entry : candidate.parameter_changes) {
        const wxString name = entry.key == "brim_width" ? _L("裙边宽度（mm）") : from_u8(entry.key);
        const wxString scope = entry.scope == ConfigScope::Plate ? _L("当前打印板") :
            entry.scope == ConfigScope::Object ? _L("对象") :
            entry.scope == ConfigScope::Material ? _L("材料") : _L("工程");
        text += scope + _L(" · ") + name + ": " + value_text(entry.expected_value) +
                " → " + value_text(entry.new_value) + "\n";
    }
    if (candidate.placement_change_count > 0)
        text += wxString::Format(_L("摆放方案涉及 %llu 个模型实例\n"),
            static_cast<unsigned long long>(candidate.placement_change_count));
    return text;
}

wxString issue_action(const std::string& code)
{
    if (code == "empty_plate") return _L("请先添加模型，再重新检查。");
    if (code == "missing_printer" || code == "missing_process" || code == "missing_material")
        return _L("请在左侧选择打印机、工艺和材料，再重新检查。");
    if (code == "outside_build_volume") return _L("请移动或缩小模型，使其位于打印板范围内。");
    if (code == "open_mesh") return _L("请使用模型修复工具处理开放边，再重新检查。");
    return _L("请查看准备页的模型和配置提示，处理后重新检查；诊断详情可展开查看。");
}

} // namespace

SmartSlicingPanel::SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                                     PlanCandidatesFn plan_candidates, CancelTrialFn cancel_trial,
                                     std::function<void()> add_model, Plater* plater)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL)
    , m_coordinator(coordinator)
    , m_plan_candidates(std::move(plan_candidates))
    , m_cancel_trial(std::move(cancel_trial))
    , m_revision_timer(this)
{
    SetScrollRate(0, FromDIP(12));
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    auto* root        = new wxBoxSizer(wxVERTICAL);
    auto* title       = new wxStaticText(this, wxID_ANY, _L("智能切片"));
    wxFont title_font = title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 2);
    title->SetFont(title_font);
    root->Add(title, 0, wxEXPAND | wxALL, FromDIP(16));

    if (plater != nullptr)
        root->Add(new OrcaModelPreparationPanel(this, *plater,
            [this] { return m_worker_running.load(std::memory_order_acquire); },
            [this] { m_coordinator.refresh_revision(); }),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_summary = new wxStaticText(this, wxID_ANY, _L("从当前打印板开始智能切片"));
    m_summary->Wrap(FromDIP(330));
    root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    const std::array<wxString, 4> stage_names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    for (size_t index = 0; index < stage_names.size(); ++index) {
        m_stage_labels[index] = new wxStaticText(this, wxID_ANY, stage_names[index] + _L("  等待"));
        root->Add(m_stage_labels[index], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, FromDIP(16));
    m_issues = new wxStaticText(this, wxID_ANY, _L("尚未运行检查"));
    m_issues->Wrap(FromDIP(330));
    root->Add(m_issues, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_add_model = new wxButton(this, wxID_ANY, _L("添加模型…"));
    root->Add(m_add_model, 0, wxEXPAND | wxALL, FromDIP(16));
    m_add_model->Bind(wxEVT_BUTTON, [this, add_model](wxCommandEvent&) {
        if (m_can_recheck) m_coordinator.cancel();
        if (add_model) add_model();
    });
    m_diagnostics = new wxCollapsiblePane(this, wxID_ANY, _L("诊断详情"), wxDefaultPosition,
        wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    m_diagnostic_text = new wxStaticText(m_diagnostics->GetPane(), wxID_ANY, "");
    auto* diagnostic_sizer = new wxBoxSizer(wxVERTICAL);
    diagnostic_sizer->Add(m_diagnostic_text, 0, wxEXPAND | wxALL, FromDIP(8));
    m_diagnostics->GetPane()->SetSizer(diagnostic_sizer);
    root->Add(m_diagnostics, 0, wxEXPAND | wxALL, FromDIP(16));
    m_diagnostics->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) { Layout(); FitInside(); });

    m_p0_notice = new wxStaticText(this, wxID_ANY, _L("预检与候选试切均在隔离副本中执行。"));
    m_p0_notice->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_p0_notice->Wrap(FromDIP(330));
    root->Add(m_p0_notice, 0, wxEXPAND | wxALL, FromDIP(16));

    m_candidate_section = new wxPanel(this);
    auto* candidate_root = new wxBoxSizer(wxVERTICAL);
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        controls.panel = new wxPanel(m_candidate_section);
        auto* box = new wxStaticBoxSizer(wxVERTICAL, controls.panel, _L("候选方案"));
        controls.selector = new wxRadioButton(controls.panel, wxID_ANY, _L("选择此方案"), wxDefaultPosition,
                                              wxDefaultSize, index == 0 ? wxRB_GROUP : 0);
        controls.metrics = new wxStaticText(controls.panel, wxID_ANY, "");
        controls.reason  = new wxStaticText(controls.panel, wxID_ANY, "");
        controls.reason->Wrap(FromDIP(300));
        controls.retry = new wxButton(controls.panel, wxID_ANY, _L("重试此方案"));
        box->Add(controls.selector, 0, wxEXPAND | wxALL, FromDIP(8));
        box->Add(controls.metrics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.reason, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.retry, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        controls.panel->SetSizer(box);
        candidate_root->Add(controls.panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        controls.selector->Bind(wxEVT_RADIOBUTTON, [this, index](wxCommandEvent&) {
            if (!m_candidate_ids[index].empty())
                m_coordinator.select_candidate(m_candidate_ids[index]);
        });
        controls.retry->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (!m_candidate_ids[index].empty())
                run_in_background([this, candidate_id = m_candidate_ids[index]] {
                    m_coordinator.retry_candidate(candidate_id, true);
                });
        });
    }
    auto* candidate_actions = new wxBoxSizer(wxVERTICAL);
    m_keep_baseline = new wxButton(m_candidate_section, wxID_ANY, _L("保留当前方案"));
    m_undo_apply    = new wxButton(m_candidate_section, wxID_ANY, _L("撤销本次应用"));
    m_apply         = new wxButton(m_candidate_section, wxID_ANY, _L("应用并切片"));
    candidate_actions->Add(m_keep_baseline, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    candidate_actions->Add(m_undo_apply, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    candidate_actions->Add(m_apply, 0, wxEXPAND);
    candidate_root->Add(candidate_actions, 0, wxEXPAND);
    m_candidate_section->SetSizer(candidate_root);
    root->Add(m_candidate_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_candidate_section->Hide();
    m_keep_baseline->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.select_candidate("baseline"); });
    m_undo_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.undo_applied_candidate(); });
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.apply_selected_candidate(); });

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_cancel      = new wxButton(this, wxID_CANCEL, _L("取消"));
    m_start       = new wxButton(this, wxID_ANY, _L("开始检查"));
    actions->Add(m_cancel, 0, wxRIGHT, FromDIP(8));
    actions->Add(m_start, 1, wxEXPAND);
    root->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));
    SetSizer(root);

    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_can_plan_candidates) {
            std::vector<AI::SmartSlicing::SliceCandidate> candidates;
            try {
                if (m_plan_candidates)
                    candidates = m_plan_candidates();
            } catch (...) {
                candidates.clear();
            }
            run_in_background([this, candidates = std::move(candidates)]() mutable {
                m_coordinator.plan_and_slice_candidates(std::move(candidates),
                                                        AI::SmartSlicing::CandidateGoal::Stability, true);
            });
        } else {
            if (m_can_recheck) m_coordinator.cancel();
            m_coordinator.start();
        }
    });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_cancel_trial)
            m_cancel_trial();
        if (!m_worker_running.load(std::memory_order_acquire))
            m_coordinator.cancel();
    });
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (!event.IsShown() && m_worker_running.load(std::memory_order_acquire) && m_cancel_trial)
            m_cancel_trial();
        event.Skip();
    });
    Bind(wxEVT_TIMER, &SmartSlicingPanel::on_revision_timer, this, m_revision_timer.GetId());
    refresh_ai_appearance(this);
}

SmartSlicingPanel::~SmartSlicingPanel()
{
    m_revision_timer.Stop();
    if (m_worker_running.load(std::memory_order_acquire) && m_cancel_trial)
        m_cancel_trial();
    if (m_worker.joinable())
        m_worker.join();
}

bool SmartSlicingPanel::run_in_background(std::function<void()> work)
{
    if (!work || m_worker_running.exchange(true, std::memory_order_acq_rel))
        return false;
    if (m_worker.joinable())
        m_worker.join();
    m_worker = std::thread([this, work = std::move(work)] {
        try {
            work();
        } catch (...) {
            m_coordinator.cancel();
        }
        m_worker_running.store(false, std::memory_order_release);
    });
    return true;
}

void SmartSlicingPanel::render(const SmartSlicingViewModel& view_model)
{
    static const std::array<wxString, 4> names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    m_summary->SetLabel(summary_text(view_model.summary_key));
    m_summary->Wrap(FromDIP(330));
    for (size_t index = 0; index < m_stage_labels.size(); ++index)
        m_stage_labels[index]->SetLabel(names[index] + _L("  ") + status_text(view_model.stages[index].status));
    if (!view_model.has_report || view_model.issues.empty()) {
        m_issues->SetLabel(view_model.has_report ? _L("本次检查未发现问题") : _L("尚无有效检查结果"));
    } else {
        wxString issues = wxString::Format(_L("发现 %llu 项需要留意的问题"), static_cast<unsigned long long>(view_model.issue_count));
        const size_t visible_issue_count = std::min<size_t>(view_model.issues.size(), 5);
        for (size_t index = 0; index < visible_issue_count; ++index) {
            const auto& [code, evidence] = view_model.issues[index];
            issues += _L("\n• ") + issue_name(code) + "\n" + issue_action(code);
        }
        if (visible_issue_count < view_model.issues.size())
            issues += wxString::Format(_L("\n…另有 %llu 项"),
                                       static_cast<unsigned long long>(view_model.issues.size() - visible_issue_count));
        m_issues->SetLabel(issues);
        m_issues->Wrap(FromDIP(330));
    }
    wxString diagnostics;
    for (const auto& [code, evidence] : view_model.issues)
        diagnostics += from_u8(code) + ": " + from_u8(evidence) + "\n";
    for (const auto& candidate : view_model.candidates) {
        if (!candidate.explanation.empty())
            diagnostics += from_u8(candidate.id) + ": " + from_u8(candidate.explanation) + "\n";
        if (!candidate.diagnostic_code.empty())
            diagnostics += from_u8(candidate.id) + ": " + from_u8(candidate.diagnostic_code) + "\n";
    }
    m_diagnostic_text->SetLabel(diagnostics);
    m_diagnostic_text->Wrap(FromDIP(300));
    m_diagnostics->Show(view_model.has_report && !diagnostics.empty());
    m_add_model->Show(view_model.can_add_model);
    m_can_recheck = view_model.can_recheck;
    m_can_plan_candidates = view_model.can_plan_candidates;
    m_start->Enable(view_model.can_start || view_model.can_plan_candidates || view_model.can_recheck);
    m_start->SetLabel(view_model.can_plan_candidates ? _L("生成并试切方案") :
                      view_model.is_stale || view_model.can_recheck ? _L("重新检查") : _L("开始检查"));
    m_cancel->Enable(view_model.can_cancel);

    const bool show_candidates = !view_model.candidates.empty();
    m_candidate_section->Show(show_candidates);
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        const bool visible = index < view_model.candidates.size();
        controls.panel->Show(visible);
        m_candidate_ids[index].clear();
        if (!visible)
            continue;
        const SmartSlicingCandidateView& candidate = view_model.candidates[index];
        m_candidate_ids[index] = candidate.id;
        controls.selector->SetLabel(candidate.id == "baseline" ? _L("当前方案（基线）") :
                                    candidate.recommended ? _L("推荐候选") : _L("候选方案"));
        controls.selector->SetValue(candidate.selected);
        controls.selector->Enable(candidate.can_select);
        wxString metrics = _L("时间：") + format_duration(candidate.estimated_time_seconds) +
                           _L("  材料：") + format_volume(candidate.filament_volume_mm3) +
                           _L("\n支撑：") + format_volume(candidate.support_volume_mm3) +
                           _L("  换料：") +
                           (candidate.tool_changes ?
                                wxString::Format("%llu", static_cast<unsigned long long>(*candidate.tool_changes)) :
                                _L("不可用")) +
                           _L("\n冲刷：") + format_volume(candidate.flush_volume_mm3) +
                           _L("  擦料塔：") + format_volume(candidate.wipe_tower_volume_mm3);
        if (candidate.layer_tool_sequence_count > 0)
            metrics += wxString::Format(_L("\n层级工具序列：%llu 组"),
                                        static_cast<unsigned long long>(candidate.layer_tool_sequence_count));
        if (candidate.color_mapping_degraded == true)
            metrics += _L("\n颜色映射：已退化");
        else if (candidate.color_mapping_degraded == false)
            metrics += _L("\n颜色映射：保持一致");
        if (candidate.id != "baseline") {
            const wxString tool_delta = candidate.tool_change_delta ?
                wxString::Format("%+lld", *candidate.tool_change_delta) : _L("—");
            metrics += _L("\n相对基线：时间 ") + format_delta(candidate.time_delta_seconds, 60.0, _L("分钟")) +
                       _L("，材料 ") + format_delta(candidate.filament_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，支撑 ") + format_delta(candidate.support_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，换料 ") + tool_delta +
                       _L("，冲刷 ") + format_delta(candidate.flush_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，擦料塔 ") + format_delta(candidate.wipe_tower_delta_mm3, 1000.0, _L("cm³"));
        }
        controls.metrics->SetLabel(metrics);
        controls.metrics->Wrap(FromDIP(300));
        controls.reason->SetLabel(candidate_changes(candidate) + candidate_reason(candidate));
        controls.reason->Wrap(FromDIP(300));
        controls.retry->Show(candidate.can_retry);
    }
    m_keep_baseline->Enable(std::any_of(view_model.candidates.begin(), view_model.candidates.end(),
                                       [](const SmartSlicingCandidateView& candidate) {
                                           return candidate.id == "baseline" && !candidate.selected && candidate.can_select;
                                       }));
    m_apply->Enable(view_model.can_apply);
    m_undo_apply->Show(view_model.can_undo_apply);
    m_undo_apply->Enable(view_model.can_undo_apply);
    m_p0_notice->SetLabel(view_model.can_undo_apply ? _L("可撤销本次应用；若之后编辑了工程，请使用 Orca 撤销历史。") :
                         show_candidates ? _L("选择“应用并切片”后，将修改当前打印板并开始正式切片。") :
                                            _L("预检与候选试切均在隔离副本中执行。"));
    m_p0_notice->Wrap(FromDIP(330));
    if ((view_model.can_cancel || view_model.needs_polling) && !m_revision_timer.IsRunning())
        m_revision_timer.Start(1000);
    else if (!view_model.can_cancel && !view_model.needs_polling && m_revision_timer.IsRunning())
        m_revision_timer.Stop();
    Layout();
    FitInside();
}

void SmartSlicingPanel::on_revision_timer(wxTimerEvent&)
{
    if (!m_worker_running.load(std::memory_order_acquire))
        m_coordinator.refresh_revision();
}

} // namespace Slic3r::GUI
